#include "Bridge.h"
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
#include "RuntimeTestHooks.h"
#include "renderer/device/ComputeQueue.hpp"
#include <atomic>
#endif

#include "device/DeviceInit.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/device/Device.hpp"
#include "renderer/device/GraphicsQueue.hpp"
#include "renderer/presentation/SurfaceBackend.hpp"
#include "renderer/resources/UploadRing.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
void check(nri::Result result, const char* operation)
{
    if (result != nri::Result::SUCCESS)
        throw std::runtime_error(std::string(operation) + ": NRI result " + std::to_string(static_cast<int>(result)));
}
uint64_t alignUp(uint64_t n, uint64_t alignment)
{
    return (n + alignment - 1) / alignment * alignment;
}
constexpr uint32_t firstScratchFramebuffer = UINT32_MAX - 2;
void publicFramebuffer(uint32_t id)
{
    if (id >= firstScratchFramebuffer)
        throw std::runtime_error("Framebuffer ID is reserved for Fury scratch storage");
}
constexpr nri::AccessLayoutStage sampled = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER };
constexpr nri::AccessLayoutStage colorTarget = { nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT };
constexpr nri::AccessLayoutStage depthTarget
    = { nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ | nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE, nri::Layout::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::DEPTH_STENCIL_ATTACHMENT };
constexpr nri::AccessLayoutStage copySrc = { nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY };
constexpr nri::AccessLayoutStage copyDst = { nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY };

// Resource owners survive their exact queue submission, including partially submitted frames.
struct Texture {
    Renderer::Device& device;
    Renderer::TextureResource resource = {};
    Renderer::TextureView view = {}, attachment = {};
    nri::AccessLayoutStage state = {};
    explicit Texture(Renderer::Device& d)
        : device(d)
    {
    }
    ~Texture()
    {
        device.destroyDescriptor(view);
        device.destroyDescriptor(attachment);
        device.destroy(resource);
    }
};
struct Program {
    Renderer::ShaderCompiler::CompileResult vertex, fragment;
    std::vector<nri::VertexAttributeDesc> attributes;
    uint32_t stride = 0;
};
struct CopyRects {
    ShipFuryRect source, destination;
};
struct Framebuffer {
    uint32_t color = 0;
    std::unique_ptr<Texture> depth;
    uint32_t width = 0, height = 0, samples = 1;
};
const char* blitSource = R"(
[[vk::binding(0,0)]] Texture2D<float4> image : register(t0);
[[vk::binding(6,0)]] SamplerState imageSampler : register(s0);
[[vk::binding(8,0)]] cbuffer CopyRectangles {int4 sourceRect;int4 destinationRect;}
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V VSMain([[vk::location(0)]] float2 position : POSITION, [[vk::location(1)]] float2 uv : TEXCOORD0) {
    V v; v.position = float4(position,0,1); v.uv=uv; return v;
}
float4 PSMain(V v) : SV_Target { return image.Sample(imageSampler,v.uv); }
int copyCoordinate(int pixel, int sourceStart, int sourceSize, int targetStart, int targetSize, bool y) {
    // Exact half-pixel rational mapping avoids interpolated UV rounding at nearest ties.
    int n=(2*pixel+1-2*targetStart)*sourceSize+2*sourceStart*targetSize;
    int d=2*targetSize;
    if(d<0){n=-n;d=-d;}
    int result=n/d;
    if(n<0 && n%d!=0) --result;
    bool decreasing=(sourceSize<0)!=(targetSize<0);
    if(n%d==0 && (y ? !decreasing : decreasing)) --result;
    return result;
}
float4 PSCopy(V v) : SV_Target {
    int2 pixel=int2(v.position.xy);
    int x=copyCoordinate(pixel.x,sourceRect.x,sourceRect.z,destinationRect.x,destinationRect.z,false);
    int y=copyCoordinate(pixel.y,sourceRect.y,sourceRect.w,destinationRect.y,destinationRect.w,true);
    uint w,h; image.GetDimensions(w,h);
    return image.Load(int3(clamp(int2(x,y),int2(0,0),int2(w,h)-1),0));
}
)";
const char* depthSource = R"(
struct V {float4 position:SV_Position;nointerpolation float2 pixel:TEXCOORD0;};
V VSMain([[vk::location(0)]] float2 position:POSITION, [[vk::location(1)]] float2 pixel:TEXCOORD0){
    V v;v.position=float4(position,0,1);v.pixel=pixel;return v;
}
float4 PSMain(V v):SV_Target {
    uint z=min(uint(saturate(sampleDepth(int2(v.pixel)))*16384.0),16383u);
    return float4((z&255)/255.0,(z>>8)/255.0,0,1);
}
)";
}

struct ShipFury {
    std::string error;
    Device::NativeHandles native = {};
    std::unique_ptr<Renderer::Device> device;
    std::unique_ptr<Renderer::GraphicsQueue> queue;
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
    // This queue owns the gate timeline fence; it is never used by ordinary contexts.
    std::unique_ptr<Renderer::ComputeQueue> testQueue;
    uint64_t testGateValue = 0;
    std::atomic<bool> testGateReleased { true };
    std::atomic<bool> testSlotWaiting { false };

    int releaseTestGate() noexcept
    {
        if (testGateReleased.exchange(true))
            return 1;
        // Only the release thread (or teardown after it is joined) touches testQueue.
        try {
            if (testQueue->signal())
                return 1;
        } catch (...) {
        }
        testGateReleased.store(false);
        return 0;
    }
#endif
    std::unique_ptr<Renderer::Presentation::SurfaceBackend> surface;
    std::map<uint32_t, Renderer::Presentation::SurfaceDesiredState> surfaces;
    std::set<uint32_t> usedSurfaces;
    Renderer::ShaderCompiler compiler;
    std::optional<Renderer::CommandList> commands;
    std::optional<Renderer::UploadRing> upload;
    nri::DescriptorPool* pool = nullptr;
    nri::PipelineLayout* layout = nullptr;
    bool rendering = false;
    uint32_t renderFb = UINT32_MAX, sets = 0, nextTexture = 1, nextProgram = 1, blitProgram = 0, presentProgram = 0;
    std::array<uint32_t, 2> depthPrograms = {};
    uint64_t surfaceSequence = 0;
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
    bool testFailNextSubmit = false;
#endif
    struct Flight {
        nri::DescriptorPool* pool = nullptr;
        std::vector<Renderer::BufferView> constants;
        std::map<std::pair<size_t, std::array<uint8_t, 32>>, nri::Descriptor*> constantCache;
        std::map<std::array<uintptr_t, 11>, nri::DescriptorSet*> descriptorCache;
        uint64_t ticket = 0;
    };
    std::array<Flight, Renderer::frames_in_flight> flights;
    uint32_t slot = 0;
    bool slotReady = false;
    struct RetiredTexture {
        uint64_t ticket;
        std::unique_ptr<Texture> texture;
    };
    std::vector<RetiredTexture> retiredTextures;
    std::vector<std::pair<uint64_t, nri::Pipeline*>> retiredPipelines;
    ShipFuryStats stats = {};
    std::map<uint32_t, std::unique_ptr<Texture>> textures;
    std::map<uint32_t, Framebuffer> framebuffers;
    std::map<uint32_t, Program> programs;
    using PipelineKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<PipelineKey, nri::Pipeline*> pipelines;
    std::map<uint32_t, Renderer::SamplerView> samplers;

    ~ShipFury()
    {
        if (!device) {
            if (native)
                Device::shutdownDevice(native);
            return;
        }
        // An abandoned recording is not submitted during teardown; idle covers all successful submits.
        if (rendering && commands)
            commands->endRendering();
        commands.reset();
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        // Failure paths must unblock graphics before waiting for either queue.
        releaseTestGate();
#endif
        device->waitIdle();
        if (surface)
            surface->shutdown();
        surface.reset();
        framebuffers.clear();
        textures.clear();
        retiredTextures.clear();
        for (auto& [_, p] : retiredPipelines)
            device->core.DestroyPipeline(p);
        for (auto& [_, p] : pipelines)
            device->core.DestroyPipeline(p);
        for (auto& [_, s] : samplers)
            device->destroyDescriptor(s);
        for (auto& flight : flights) {
            for (auto& v : flight.constants)
                device->destroyDescriptor(v);
            if (flight.pool)
                device->core.DestroyDescriptorPool(flight.pool);
        }
        if (layout)
            device->core.DestroyPipelineLayout(layout);
        upload.reset();
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        // The gate fence must outlive every graphics wait and compute signal.
        testQueue.reset();
#endif
        queue.reset();
        Renderer::shutdown(*device);
        device.reset();
        Device::shutdownDevice(native);
    }

    void init(ShipFuryWindow w, bool validation
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        ,
        bool testQueues = false
#endif
    )
    {
        Device::DeviceInitDesc desc;
        desc.computeQueueCount = 0;
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        if (testQueues)
            desc.computeQueueCount = 1;
#endif
        desc.transferQueueCount = 0;
        desc.videoDecodeQueueCount = 0;
        desc.enableValidation = validation;
        desc.enableGraphicsAPIValidation = validation;
        auto n = Device::initDevice(desc);
        if (!n)
            throw std::runtime_error("Fury Vulkan device initialization failed");
        native = std::move(*n);
        auto d = Renderer::init(native.deviceResult);
        if (!d)
            throw std::runtime_error(std::string(d.error().c_str()));
        device = std::make_unique<Renderer::Device>(std::move(*d));
        queue = std::make_unique<Renderer::GraphicsQueue>(device->createGraphicsQueue());
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        if (testQueues) {
            testQueue = std::make_unique<Renderer::ComputeQueue>(device->createComputeQueue());
            if (device->core.GetQueueNativeObject(&testQueue->queueHandle()) == device->core.GetQueueNativeObject(&queue->queueHandle()))
                throw std::runtime_error("GPU lag test requires an independent native compute queue");
        }
#endif
        registerSurface(0, w);
        surface = std::make_unique<Renderer::Presentation::SurfaceBackend>(*device, *queue);
        if (!compiler.valid())
            throw std::runtime_error(compiler.error().c_str());
        std::array<nri::DescriptorRangeDesc, 11> ranges = {};
        for (uint32_t i = 0; i < 11; ++i)
            ranges[i] = { i, 1,
                i < 6       ? nri::DescriptorType::TEXTURE
                    : i < 8 ? nri::DescriptorType::SAMPLER
                            : nri::DescriptorType::CONSTANT_BUFFER,
                nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER, {} };
        nri::DescriptorSetDesc set = { 0, ranges.data(), static_cast<uint32_t>(ranges.size()), {} };
        nri::PipelineLayoutDesc ld = {};
        ld.descriptorSets = &set;
        ld.descriptorSetNum = 1;
        ld.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        check(device->core.CreatePipelineLayout(device->device, ld, layout), "CreatePipelineLayout");
        nri::DescriptorPoolDesc pd = {};
        pd.descriptorSetMaxNum = 4096;
        pd.textureMaxNum = 4096 * 6;
        pd.samplerMaxNum = 4096 * 2;
        pd.constantBufferMaxNum = 4096 * 3;
        for (auto& flight : flights)
            check(device->core.CreateDescriptorPool(device->device, pd, flight.pool), "CreateDescriptorPool");
        pool = flights[slot].pool;
        // The factory size is per segment; it multiplies by the slot count itself.
        auto u = device->createUploadRing(64ull * 1024 * 1024, nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX | nri::BufferUsageBits::CONSTANT, Renderer::frames_in_flight);
        if (!u)
            throw std::runtime_error("Could not allocate Fury upload storage");
        upload.emplace(std::move(*u));
        const uint8_t white[4] = { 255, 255, 255, 255 };
        uploadTexture(0, white, 1, 1);
    }

    void begin()
    {
        prepareSlot();
        if (commands)
            return;
        auto c = queue->begin();
        if (!c)
            throw std::runtime_error("Fury command recording unavailable");
        commands.emplace(std::move(*c));
        commands->begin(pool);
    }
    void endRendering()
    {
        if (rendering) {
            commands->endRendering();
            rendering = false;
            renderFb = UINT32_MAX;
        }
    }
    void collect()
    {
        const auto completed = queue->completedValue();
        std::erase_if(retiredTextures, [&](const auto& retired) {
            return retired.ticket <= completed;
        });
        std::erase_if(retiredPipelines, [&](const auto& retired) {
            if (retired.first > completed)
                return false;
            device->core.DestroyPipeline(retired.second);
            return true;
        });
    }
    void prepareSlot()
    {
        if (slotReady)
            return;
        auto& flight = flights[slot];
        if (queue->completedValue() < flight.ticket)
            ++stats.recycling_waits;
        // Includes presentation-completion signals recorded by SurfaceBackend on the same slot.
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        if (testQueue)
            testSlotWaiting.store(queue->completedValue() < flight.ticket);
#endif
        queue->waitForFrameSlot({ slot });
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        testSlotWaiting.store(false);
#endif
        for (auto& v : flight.constants)
            device->destroyDescriptor(v);
        flight.constants.clear();
        pool = flight.pool;
        device->core.ResetDescriptorPool(*pool);
        flight.constantCache.clear();
        flight.descriptorCache.clear();
        sets = 0;
        upload->setFrameSlot(slot);
        upload->reset();
        collect();
        slotReady = true;
    }
    void submitted(uint64_t ticket)
    {
        ++stats.submissions;
        flights[slot].ticket = ticket;
        commands.reset();
        queue->nextFrame();
        slot = (slot + 1) % flights.size();
        slotReady = false;
    }
    void flush(bool wait = false)
    {
        if (commands) {
            endRendering();
            upload->flush();
            commands->end();
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
            if (std::exchange(testFailNextSubmit, false))
                throw std::runtime_error("Injected Fury graphics submission failure");
            const nri::FenceSubmitDesc gate = { testQueue ? &testQueue->fence() : nullptr, testGateValue, nri::StageBits::ALL };
            auto receipt = testGateValue ? queue->submit(*commands, { &gate, 1 }) : queue->submit(*commands);
#else
            auto receipt = queue->submit(*commands);
#endif
            if (!receipt)
                throw std::runtime_error("Fury graphics submission failed");
            submitted(receipt->value());
        }
        if (wait) {
            queue->wait(queue->lastSubmittedValue());
            collect();
        }
    }
    void retire(std::unique_ptr<Texture>& texture)
    {
        if (!texture)
            return;
        // Reserve ownership before transferring it; failed host allocation leaves the old owner intact.
        retiredTextures.push_back({ queue->lastSubmittedValue(), nullptr });
        retiredTextures.back().texture = std::move(texture);
        collect();
    }
    void deleteFramebuffer(uint32_t id)
    {
        auto i = framebuffers.find(id);
        if (i == framebuffers.end())
            return;
        flush();
        auto& color = textures.at(i->second.color);
        retire(color);
        textures.erase(i->second.color);
        retire(i->second.depth);
        framebuffers.erase(i);
    }
    void registerSurface(uint32_t id, ShipFuryWindow w)
    {
        if (usedSurfaces.contains(id))
            throw std::runtime_error("Fury surface IDs cannot be reused");
        Renderer::Presentation::SurfaceDesiredState desired = {};
        desired.key = { { uint64_t(id) + 1 }, 1 };
        desired.minimized = true;
        if (w.kind == 1)
            desired.window.value.wayland = { w.display, w.surface };
        else if (w.kind == 2)
            desired.window.value.x11 = { w.display, w.window };
        else if (w.kind == 3)
            desired.window.value.windows = { w.surface };
        else
            throw std::runtime_error("Unsupported SDL native window system");
        surfaces.emplace(id, desired);
        usedSurfaces.insert(id);
    }
    void discardRecording()
    {
        if (rendering && commands)
            commands->endRendering();
        rendering = false;
        renderFb = UINT32_MAX;
        commands.reset();
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
        if (testQueue)
            releaseTestGate();
#endif
        // Failure cleanup is allowed to block: native windows must not outlive their surfaces.
        if (device)
            device->waitIdle();
        if (device && queue)
            collect();
    }
    void surfaceState(uint32_t id, uint32_t w, uint32_t h, bool minimized)
    {
        auto& desired = surfaces.at(id);
        desired.extent = { w, h };
        desired.minimized = minimized || !w || !h;
    }
    void unregisterSurface(uint32_t id)
    {
        auto i = surfaces.find(id);
        if (i == surfaces.end())
            return;
        flush();
        if (!surface) {
            surfaces.erase(i);
            return; // Initialization may have registered a desired surface before backend creation.
        }
        auto retired = surface->retire(i->second.key);
        if (retired == Renderer::Presentation::SurfaceRetireResult::Busy || retired == Renderer::Presentation::SurfaceRetireResult::Stale)
            throw std::runtime_error("Fury surface retirement failed");
        surfaces.erase(i);
    }
    void transition(Texture& t, nri::AccessLayoutStage state)
    {
        if (t.state.access == sampled.access && state.access == sampled.access && t.state.layout == state.layout && t.state.stages == state.stages)
            return;
        endRendering();
        begin();
        nri::TextureBarrierDesc b = {};
        b.texture = t.resource.handle;
        b.before = t.state;
        b.after = state;
        b.mipNum = 1;
        b.layerNum = 1;
        b.planes = nri::PlaneBits::ALL;
        commands->barrier({ .textures = &b, .textureNum = 1 });
        t.state = state;
    }
    Renderer::UploadSlice stage(const void* data, size_t bytes, uint64_t alignment = 16)
    {
        prepareSlot();
        auto s = upload->allocate(
            bytes,
            [&](Span<std::byte> out) {
                std::memcpy(out.data(), data, bytes);
            },
            alignment);
        if (!s) {
            flush();
            prepareSlot();
            s = upload->allocate(
                bytes,
                [&](Span<std::byte> out) {
                    std::memcpy(out.data(), data, bytes);
                },
                alignment);
        }
        if (!s)
            throw std::runtime_error("Draw/upload exceeds Fury staging capacity");
        begin();
        return *s;
    }
    std::unique_ptr<Texture> makeTexture(uint32_t w, uint32_t h, nri::Format format, uint32_t samples, bool depth)
    {
        if (!w || !h || w > 16384 || h > 16384)
            throw std::runtime_error("Invalid Fury texture extent");
        auto t = std::make_unique<Texture>(*device);
        nri::TextureDesc desc = {};
        desc.type = nri::TextureType::TEXTURE_2D;
        desc.usage = nri::TextureUsageBits::SHADER_RESOURCE | (depth ? nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT : nri::TextureUsageBits::COLOR_ATTACHMENT);
        desc.format = format;
        desc.width = w;
        desc.height = h;
        desc.depth = 1;
        desc.mipNum = 1;
        desc.layerNum = 1;
        desc.sampleNum = samples;
        auto r = device->create(desc, nri::MemoryLocation::DEVICE);
        if (!r)
            throw std::runtime_error("Fury texture allocation failed");
        t->resource = *r;
        auto view = device->createTextureView(Renderer::makeTexture2DViewDesc(r->handle, nri::TextureView::TEXTURE, format));
        if (!view)
            throw std::runtime_error("Fury texture view creation failed");
        t->view = *view;
        auto a = device->createTextureView(Renderer::makeTexture2DViewDesc(r->handle, depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT, format));
        if (!a)
            throw std::runtime_error("Fury attachment view creation failed");
        t->attachment = *a;
        return t;
    }
    void uploadTexture(uint32_t id, const uint8_t* data, uint32_t w, uint32_t h)
    {
        if (!data || !w || !h)
            throw std::runtime_error("Invalid Fury texture upload");
        if (textures.contains(id))
            flush();
        auto t = makeTexture(w, h, nri::Format::RGBA8_UNORM, 1, false);
        const auto& caps = device->deviceDesc();
        uint32_t pitch = alignUp(w * 4, caps.memoryAlignment.uploadBufferTextureRow);
        uint32_t slice = alignUp(uint64_t(pitch) * h, caps.memoryAlignment.uploadBufferTextureSlice);
        std::vector<uint8_t> packed(slice);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(packed.data() + y * pitch, data + y * w * 4, w * 4);
        auto s = stage(packed.data(), packed.size(), caps.memoryAlignment.uploadBufferTextureSlice);
        transition(*t, copyDst);
        commands->uploadBufferToTexture(
            s.buffer, { s.offset, pitch, slice }, t->resource.handle, { .width = static_cast<nri::Dim_t>(w), .height = static_cast<nri::Dim_t>(h), .depth = 1, .planes = nri::PlaneBits::ALL });
        transition(*t, sampled);
        auto& owner = textures[id];
        retire(owner);
        owner = std::move(t);
    }
    void framebuffer(uint32_t id, uint32_t w, uint32_t h, uint32_t samples, bool depth)
    {
        auto& f = framebuffers[id];
        if (f.width == w && f.height == h && f.samples == samples && bool(f.depth) == depth)
            return;
        flush();
        if (!f.color)
            f.color = nextTexture++;
        auto color = makeTexture(w, h, nri::Format::RGBA8_UNORM, samples, false);
        auto z = depth ? makeTexture(w, h, nri::Format::D32_SFLOAT, samples, true) : nullptr;
        retire(textures[f.color]);
        retire(f.depth);
        textures[f.color] = std::move(color);
        f.depth = std::move(z);
        f.width = w;
        f.height = h;
        f.samples = samples;
    }
    void target(uint32_t id, bool clearColor = false, bool clearDepth = false)
    {
        if (rendering && renderFb == id && !clearColor && !clearDepth)
            return;
        auto& f = framebuffers.at(id);
        auto& t = *textures.at(f.color);
        transition(t, colorTarget);
        if (f.depth)
            transition(*f.depth, depthTarget);
        nri::AttachmentDesc a = {};
        a.descriptor = t.attachment.descriptor;
        a.clearValue.color.f = { 0, 0, 0, 1 };
        a.loadOp = clearColor ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
        a.storeOp = nri::StoreOp::STORE;
        nri::RenderingDesc r = { .colors = &a, .colorNum = 1 };
        if (f.depth) {
            r.depth.descriptor = f.depth->attachment.descriptor;
            r.depth.loadOp = clearDepth ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
            r.depth.storeOp = nri::StoreOp::STORE;
            r.depth.clearValue.depthStencil.depth = 1;
        }
        commands->beginRendering(r);
        rendering = true;
        renderFb = id;
    }
    uint32_t program(const char* source, size_t length, const ShipFuryAttribute* attributes, uint32_t count, uint32_t stride, const char* fragmentEntry = "PSMain")
    {
        Program p;
        p.stride = stride;
        Renderer::ShaderCompiler::CompileOptions opts;
        opts.entryPoint = "VSMain";
        auto vs = compiler.compileShader({ source, length }, Renderer::ShaderType::eVertex, opts, "ship-fury");
        if (!vs)
            throw std::runtime_error(vs.error().c_str());
        opts.entryPoint = fragmentEntry;
        auto ps = compiler.compileShader({ source, length }, Renderer::ShaderType::eFragment, opts, "ship-fury");
        if (!ps)
            throw std::runtime_error(ps.error().c_str());
        p.vertex = std::move(*vs);
        p.fragment = std::move(*ps);
        const nri::Format formats[] = { nri::Format::RGBA8_UNORM, nri::Format::R32_SFLOAT, nri::Format::RG32_SFLOAT, nri::Format::RGB32_SFLOAT, nri::Format::RGBA32_SFLOAT };
        for (uint32_t i = 0; i < count; ++i) {
            if (attributes[i].components > 4)
                throw std::runtime_error("Invalid vertex attribute");
            p.attributes.push_back({ .vk = { i }, .offset = attributes[i].offset, .format = formats[attributes[i].components], .streamIndex = 0 });
        }
        uint32_t id = nextProgram++;
        programs.emplace(id, std::move(p));
        return id;
    }
    nri::Pipeline& pipeline(const ShipFuryDraw& s, nri::Format color, nri::Format depth, uint32_t samples)
    {
        PipelineKey key
            = { s.program, static_cast<uint32_t>(color), static_cast<uint32_t>(depth), samples, s.depth_test | (s.depth_write << 1), s.decal, s.alpha, std::bit_cast<uint32_t>(s.decal_slope) };
        auto i = pipelines.find(key);
        if (i != pipelines.end())
            return *i->second;
        auto& p = programs.at(s.program);
        nri::VertexStreamDesc stream = { .bindingSlot = 0, .stride = static_cast<uint16_t>(p.stride) };
        nri::VertexInputDesc vi = { p.attributes.data(), static_cast<uint8_t>(p.attributes.size()), &stream, 1 };
        nri::ShaderDesc shaders[] = { { nri::StageBits::VERTEX_SHADER, p.vertex.shaderBinary.data(), p.vertex.shaderBinary.size(), p.vertex.binaryEntryPoint.c_str() },
            { nri::StageBits::FRAGMENT_SHADER, p.fragment.shaderBinary.data(), p.fragment.shaderBinary.size(), p.fragment.binaryEntryPoint.c_str() } };
        nri::ColorAttachmentDesc a = {};
        a.format = color;
        a.colorWriteMask = nri::ColorWriteBits::RGBA;
        a.blendEnabled = s.alpha;
        a.colorBlend = { nri::BlendFactor::SRC_ALPHA, nri::BlendFactor::ONE_MINUS_SRC_ALPHA, nri::BlendOp::ADD };
        a.alphaBlend = { s.gui ? nri::BlendFactor::ONE : nri::BlendFactor::SRC_ALPHA, nri::BlendFactor::ONE_MINUS_SRC_ALPHA, nri::BlendOp::ADD };
        nri::MultisampleDesc ms = { .sampleMask = UINT32_MAX, .sampleNum = static_cast<nri::Sample_t>(samples) };
        nri::GraphicsPipelineDesc desc = {};
        desc.pipelineLayout = layout;
        desc.vertexInput = &vi;
        desc.inputAssembly.topology = nri::Topology::TRIANGLE_LIST;
        desc.rasterization.fillMode = nri::FillMode::SOLID;
        desc.rasterization.cullMode = nri::CullMode::NONE;
        if (s.decal)
            desc.rasterization.depthBias = { .constant = -2, .slope = s.decal_slope };
        desc.multisample = &ms;
        desc.outputMerger.colors = &a;
        desc.outputMerger.colorNum = 1;
        desc.outputMerger.depthStencilFormat = depth;
        desc.outputMerger.depth.compareOp = depth == nri::Format::UNKNOWN ? nri::CompareOp::NONE
            : s.depth_test                                                ? (s.decal ? nri::CompareOp::LESS_EQUAL : nri::CompareOp::LESS)
            : s.depth_write                                               ? nri::CompareOp::ALWAYS
                                                                          : nri::CompareOp::NONE;
        desc.outputMerger.depth.write = s.depth_write && depth != nri::Format::UNKNOWN;
        desc.shaders = shaders;
        desc.shaderNum = 2;
        nri::Pipeline* result = nullptr;
        check(device->core.CreateGraphicsPipeline(device->device, desc, result), "CreateGraphicsPipeline");
        try {
            pipelines.emplace(key, result);
        } catch (...) {
            device->core.DestroyPipeline(result);
            throw;
        }
        return *result;
    }
    nri::Descriptor* sampler(uint32_t bits)
    {
        auto i = samplers.find(bits);
        if (i != samplers.end())
            return i->second.descriptor;
        const nri::AddressMode modes[] = { nri::AddressMode::REPEAT, nri::AddressMode::MIRRORED_REPEAT, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::MIRROR_CLAMP_TO_EDGE };
        nri::Filter filter = (bits & 1) ? nri::Filter::LINEAR : nri::Filter::NEAREST;
        auto s = device->createSampler(Renderer::makeSamplerDesc({ filter, filter, nri::Filter::NEAREST }, { modes[(bits >> 1) & 3], modes[(bits >> 3) & 3], nri::AddressMode::CLAMP_TO_EDGE }));
        if (!s)
            throw std::runtime_error("Fury sampler allocation failed");
        try {
            samplers.emplace(bits, *s);
        } catch (...) {
            device->destroyDescriptor(*s);
            throw;
        }
        return s->descriptor;
    }
    nri::Descriptor* constant(const void* data, size_t bytes)
    {
        if (!data || bytes > 32)
            throw std::runtime_error("Invalid Fury constant buffer data");
        prepareSlot();
        std::pair<size_t, std::array<uint8_t, 32>> key = { bytes, {} };
        std::memcpy(key.second.data(), data, bytes);
        if (auto i = flights[slot].constantCache.find(key); i != flights[slot].constantCache.end())
            return i->second;
        auto s = stage(data, bytes, device->deviceDesc().memoryAlignment.constantBufferOffset);
        auto v = device->createBufferView(Renderer::makeBufferViewDesc(s.buffer, nri::BufferView::CONSTANT_BUFFER, nri::Format::UNKNOWN, s.offset, s.size));
        if (!v)
            throw std::runtime_error("Fury constant buffer view failed");
        try {
            flights[slot].constants.push_back(*v);
        } catch (...) {
            device->destroyDescriptor(*v);
            throw;
        }
        flights[slot].constantCache.emplace(key, v->descriptor);
        return v->descriptor;
    }
    nri::DescriptorSet* descriptorSet(const std::array<nri::Descriptor*, 11>& descriptors)
    {
        std::array<uintptr_t, 11> key;
        std::transform(descriptors.begin(), descriptors.end(), key.begin(), [](auto* d) {
            return reinterpret_cast<uintptr_t>(d);
        });
        auto& cache = flights[slot].descriptorCache;
        if (auto i = cache.find(key); i != cache.end())
            return i->second;
        nri::DescriptorSet* set = nullptr;
        check(device->core.AllocateDescriptorSets(*pool, *layout, 0, &set, 1, 0), "AllocateDescriptorSets");
        ++sets;
        std::array<nri::UpdateDescriptorRangeDesc, 11> updates = {};
        for (uint32_t i = 0; i < 11; ++i)
            updates[i] = { set, i, 0, &descriptors[i], 1 };
        device->core.UpdateDescriptorRanges(updates.data(), updates.size());
        cache.emplace(key, set);
        return set;
    }
    void draw(const ShipFuryDraw& s, const void* vertices, size_t bytes, uint32_t count, const void* indices, uint32_t indexBytes, int32_t vertexOffset, Texture* firstTexture = nullptr,
        const CopyRects* copy = nullptr)
    {
        if (!count || s.viewport.width <= 0 || s.viewport.height <= 0 || s.scissor.width <= 0 || s.scissor.height <= 0)
            return;
        if (!vertices || !bytes || (indices && indexBytes != 2 && indexBytes != 4))
            throw std::runtime_error("Invalid Fury draw spans");
        prepareSlot();
        const auto cbAlignment = std::max<uint64_t>(16, device->deviceDesc().memoryAlignment.constantBufferOffset);
        const uint64_t required = 3 * alignUp(32, cbAlignment) + alignUp(bytes, 16) + alignUp(indices ? uint64_t(count) * indexBytes : 0, 16) + cbAlignment;
        // One draw's constants, vertices and indices must share one receipt-owned upload slot.
        // Never allow stage() to submit a prefix and recycle it before the actual draw uses it.
        if (sets >= 4000 || !upload->canAllocate(required, cbAlignment)) {
            flush();
            prepareSlot();
            if (!upload->canAllocate(required, cbAlignment))
                throw std::runtime_error("Draw exceeds Fury staging slot capacity");
        }
        std::array<nri::Descriptor*, 11> descriptors = {};
        for (uint32_t i = 0; i < 6; ++i) {
            auto t = textures.find(s.textures[i]);
            if (t == textures.end())
                t = textures.find(0);
            Texture& image = i == 0 && firstTexture ? *firstTexture : *t->second;
            transition(image, sampled);
            descriptors[i] = image.view.descriptor;
        }
        descriptors[6] = sampler(s.sampler[0]);
        descriptors[7] = sampler(s.sampler[1]);
        struct Frame {
            uint32_t frame;
            float scale;
            float pad[2];
        } frame = { s.noise_frame, s.noise_scale, {} };
        descriptors[8] = copy ? constant(copy, sizeof(*copy)) : constant(s.gui ? static_cast<const void*>(s.gui_transform) : &frame, 16);
        uint32_t dims[8] = { s.width[0], s.height[0], s.linear[0], 0, s.width[1], s.height[1], s.linear[1], 0 };
        descriptors[9] = constant(dims, sizeof(dims));
        float prim[4] = { s.prim_depth, 0, 0, 0 };
        descriptors[10] = constant(prim, sizeof(prim));
        auto v = stage(vertices, bytes);
        std::optional<Renderer::UploadSlice> ix;
        if (indices)
            ix = stage(indices, uint64_t(count) * indexBytes);
        auto* set = descriptorSet(descriptors);
        target(s.framebuffer);
        auto& f = framebuffers.at(s.framebuffer);
        commands->bindGraphicsPipeline(pipeline(s, nri::Format::RGBA8_UNORM, f.depth ? nri::Format::D32_SFLOAT : nri::Format::UNKNOWN, f.samples), layout);
        commands->bindDescriptorSet(0, set);
        commands->setVertexBuffer(0, { v.buffer, v.offset, programs.at(s.program).stride });
        commands->setViewport(s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height);
        commands->setScissor(std::max(s.scissor.x, 0), std::max(s.scissor.y, 0), std::max(s.scissor.width, 0), std::max(s.scissor.height, 0));
        if (ix) {
            commands->setIndexBuffer(*ix->buffer, ix->offset, indexBytes);
            commands->drawIndexed(count, 1, 0, vertexOffset, 0);
        } else
            commands->draw(count, 1, 0, 0);
    }
    void blit(uint32_t dst, uint32_t src, ShipFuryRect dr, ShipFuryRect sr)
    {
        if (!dr.width || !dr.height || !sr.width || !sr.height)
            return;
        if (dst == src) {
            auto& original = framebuffers.at(src);
            framebuffer(UINT32_MAX - 1, original.width, original.height, 1, false);
            blit(UINT32_MAX - 1, src, { 0, 0, int32_t(original.width), int32_t(original.height) }, { 0, 0, int32_t(original.width), int32_t(original.height) });
            blit(dst, UINT32_MAX - 1, dr, sr);
            return;
        }
        auto& f = framebuffers.at(src);
        auto& df = framebuffers.at(dst);
        if (f.samples > 1) {
            const bool fullResolve = df.samples == 1 && f.width == df.width && f.height == df.height && dr.x == 0 && dr.y == 0 && dr.width == int32_t(f.width) && dr.height == int32_t(f.height)
                && sr.x == 0 && sr.y == 0 && sr.width == int32_t(f.width) && sr.height == int32_t(f.height);
            if (!fullResolve) {
                // Resolve before transforming. Native MSAA resolves cannot crop, scale or flip.
                constexpr uint32_t resolved = UINT32_MAX - 2;
                framebuffer(resolved, f.width, f.height, 1, false);
                blit(resolved, src, { 0, 0, int32_t(f.width), int32_t(f.height) }, { 0, 0, int32_t(f.width), int32_t(f.height) });
                blit(dst, resolved, dr, sr);
                return;
            }
            auto& a = *textures.at(f.color);
            auto& b = *textures.at(df.color);
            transition(a, { nri::AccessBits::RESOLVE_SOURCE, nri::Layout::RESOLVE_SOURCE, nri::StageBits::RESOLVE });
            transition(b, { nri::AccessBits::RESOLVE_DESTINATION, nri::Layout::RESOLVE_DESTINATION, nri::StageBits::RESOLVE });
            device->core.CmdResolveTexture(commands->commandBuffer(), *b.resource.handle, nullptr, *a.resource.handle, nullptr, nri::ResolveOp::AVERAGE);
            commands->markWork();
            return;
        }
        if (!blitProgram) {
            ShipFuryAttribute attributes[] = { { 2, 0 }, { 2, 8 } };
            blitProgram = program(blitSource, std::strlen(blitSource), attributes, 2, 16, "PSCopy");
        }
        const CopyRects copy = { sr, dr };
        auto supported = [](ShipFuryRect r) {
            return std::abs(int64_t(r.x)) <= 16384 && std::abs(int64_t(r.y)) <= 16384 && std::abs(int64_t(r.width)) <= 16384 && std::abs(int64_t(r.height)) <= 16384;
        };
        if (!supported(sr) || !supported(dr))
            throw std::runtime_error("Framebuffer copy rectangle exceeds supported coordinate range");
        struct V {
            float x, y, u, v;
        };
        // Clip both endpoints through their affine mapping, instead of sampling clamped edge
        // pixels outside the source. Keep the original viewport/UV mapping for subpixel scaling.
        auto clippedAxis = [](int32_t sourceStart, int32_t sourceSize, uint32_t sourceExtent, int32_t targetStart, int32_t targetSize, uint32_t targetExtent) {
            double t0 = double(-int64_t(sourceStart)) / sourceSize;
            double t1 = double(int64_t(sourceExtent) - sourceStart) / sourceSize;
            if (t0 > t1)
                std::swap(t0, t1);
            t0 = std::max(t0, 0.0);
            t1 = std::min(t1, 1.0);
            if (t0 >= t1)
                return std::pair<int32_t, int32_t> { 0, 0 };
            double lo = targetStart + targetSize * t0, hi = targetStart + targetSize * t1;
            if (lo > hi)
                std::swap(lo, hi);
            int32_t begin = int32_t(std::clamp(std::ceil(lo - 0.5), 0.0, double(targetExtent)));
            int32_t end = int32_t(std::clamp(std::ceil(hi - 0.5), 0.0, double(targetExtent)));
            return std::pair { begin, std::max(end, begin) };
        };
        auto [left, right] = clippedAxis(sr.x, sr.width, f.width, dr.x, dr.width, df.width);
        auto [top, bottom] = clippedAxis(sr.y, sr.height, f.height, dr.y, dr.height, df.height);
        if (right <= left || bottom <= top)
            return;
        float u0 = float(sr.x) / f.width, v0 = float(sr.y) / f.height, u1 = float(sr.x + sr.width) / f.width, v1 = float(sr.y + sr.height) / f.height;
        if (dr.width < 0) {
            dr.x += dr.width;
            dr.width = -dr.width;
            std::swap(u0, u1);
        }
        if (dr.height < 0) {
            dr.y += dr.height;
            dr.height = -dr.height;
            std::swap(v0, v1);
        }
        V vertices[] = { { -1, 1, u0, v0 }, { 1, 1, u1, v0 }, { 1, -1, u1, v1 }, { -1, 1, u0, v0 }, { 1, -1, u1, v1 }, { -1, -1, u0, v1 } };
        ShipFuryDraw s = {};
        s.program = blitProgram;
        s.framebuffer = dst;
        s.textures[0] = f.color;
        s.sampler[0] = 1 | (2 << 1) | (2 << 3);
        s.sampler[0] = (2 << 1) | (2 << 3); // Match OpenGL's nearest-neighbor framebuffer copies.
        s.viewport = dr;
        s.scissor = { left, top, right - left, bottom - top };
        draw(s, vertices, sizeof(vertices), 6, nullptr, 0, 0, nullptr, &copy);
    }
    void present(uint32_t id, uint32_t framebuffer, uint32_t w, uint32_t h, bool vsync)
    {
        using namespace Renderer::Presentation;
        flush();
        auto& desired = surfaces.at(id);
        desired.extent = { w, h };
        desired.minimized = !w || !h;
        desired.presentMode = vsync ? SurfacePresentMode::Fifo : SurfacePresentMode::Immediate;
        if (auto existing = surface->snapshot(desired.key.id); existing && existing->format != nri::Format::UNKNOWN)
            desired.requiredFormat = existing->format;
        auto batchForSurfaces = [&] {
            SurfaceFrameBatch batch(++surfaceSequence);
            for (auto& [_, state] : surfaces)
                if (!batch.add(state))
                    throw std::runtime_error("Invalid Fury presentation state");
            return batch;
        };
        auto batch = batchForSurfaces();
        auto r = surface->reconcile(std::move(batch));
        // Native extent changes can race SDL's drawable snapshot. The backend retires failed
        // swapchains; skip this presentation and reconcile the latest extent on the next frame.
        if (r.accepted && r.failure && r.failure->nativeResult == nri::Result::OUT_OF_DATE)
            return;
        if (!r.accepted || r.failure)
            throw std::runtime_error(
                "Fury surface reconciliation failed" + (r.failure ? std::string(": kind ") + std::to_string(int(r.failure->kind)) + ", native " + std::to_string(int(r.failure->nativeResult)) : ""));
        if (!w || !h)
            return;
        auto snapshot = surface->snapshot(desired.key.id);
        if (snapshot && snapshot->status == SurfaceStatus::AwaitingFormat) {
            // Discovery publishes a format; acknowledge that exact contract before acquisition.
            desired.requiredFormat = snapshot->format;
            auto confirmed = batchForSurfaces();
            auto ready = surface->reconcile(std::move(confirmed));
            if (ready.accepted && ready.failure && ready.failure->nativeResult == nri::Result::OUT_OF_DATE)
                return;
            if (!ready.accepted || ready.failure)
                throw std::runtime_error("Fury surface format confirmation failed");
        }
        SurfaceKey key = desired.key;
        auto acquired = surface->acquire({ &key, 1 });
        if (!acquired) {
            if (acquired.error().nativeResult == nri::Result::OUT_OF_DATE)
                return;
            throw std::runtime_error("Fury surface acquisition failed: kind " + std::to_string(int(acquired.error().kind)) + ", native " + std::to_string(int(acquired.error().nativeResult)));
        }
        auto& t = acquired->targets()[0];
        // Each native window presents its own logical framebuffer; texture tokens stay shared.
        if (!presentProgram) {
            ShipFuryAttribute a[] = { { 2, 0 }, { 2, 8 } };
            presentProgram = program(blitSource, std::strlen(blitSource), a, 2, 16);
        }
        ShipFuryDraw s = {};
        s.program = presentProgram;
        s.textures[0] = framebuffers.at(framebuffer).color;
        s.sampler[0] = 1 | (2 << 1) | (2 << 3);
        auto& image = *textures.at(s.textures[0]);
        begin();
        transition(image, sampled);
        nri::TextureBarrierDesc b = {};
        b.texture = t.texture;
        b.before = t.initialState;
        b.after = colorTarget;
        b.mipNum = 1;
        b.layerNum = 1;
        b.planes = nri::PlaneBits::ALL;
        commands->barrier({ .textures = &b, .textureNum = 1 });
        std::array<nri::Descriptor*, 11> descriptors = {};
        for (uint32_t i = 0; i < 6; ++i)
            descriptors[i] = (i == 0 ? image.view : textures.at(0)->view).descriptor;
        descriptors[6] = sampler(s.sampler[0]);
        descriptors[7] = sampler(0);
        uint32_t zeros[8] = {};
        descriptors[8] = constant(zeros, 16);
        descriptors[9] = constant(zeros, 32);
        descriptors[10] = constant(zeros, 16);
        auto* set = descriptorSet(descriptors);
        float vertices[] = { -1, 1, 0, 0, 1, 1, 1, 0, 1, -1, 1, 1, -1, 1, 0, 0, 1, -1, 1, 1, -1, -1, 0, 1 };
        auto v = stage(vertices, sizeof(vertices));
        nri::AttachmentDesc a = {};
        a.descriptor = t.colorView.descriptor;
        a.loadOp = nri::LoadOp::CLEAR;
        a.storeOp = nri::StoreOp::STORE;
        commands->beginRendering({ .colors = &a, .colorNum = 1 });
        commands->bindGraphicsPipeline(pipeline(s, t.textureDesc.format, nri::Format::UNKNOWN, 1), layout);
        commands->bindDescriptorSet(0, set);
        commands->setVertexBuffer(0, { v.buffer, v.offset, 16 });
        commands->setViewport(0, 0, w, h);
        commands->setScissor(0, 0, w, h);
        commands->draw(6, 1, 0, 0);
        commands->endRendering();
        b.before = colorTarget;
        b.after = { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };
        commands->barrier({ .textures = &b, .textureNum = 1 });
        upload->flush();
        commands->end();
        nri::FenceSubmitDesc wait = {}, signal = {};
        if (t.acquireFence)
            wait = *t.acquireFence;
        if (t.releaseFence)
            signal = *t.releaseFence;
        auto receipt = queue->submit(*commands, t.acquireFence ? Span<const nri::FenceSubmitDesc> { &wait, 1 } : Span<const nri::FenceSubmitDesc> {},
            t.releaseFence ? Span<const nri::FenceSubmitDesc> { &signal, 1 } : Span<const nri::FenceSubmitDesc> {});
        if (!receipt)
            throw std::runtime_error("Fury presentation submission failed");
        SurfacePresentSubmission submission = { t.key, t.backendGeneration, receipt->value(), signal.fence };
        // Resolve the acquisition immediately after successful submission, before recycling any
        // slot. SurfaceBackend adds its own presentation-completion ticket to this queue slot.
        auto result = acquired->present({ &submission, 1 });
        submitted(queue->lastSubmittedValue());
        if (!result && result.error().nativeResult != nri::Result::OUT_OF_DATE)
            throw std::runtime_error("Fury present failed: kind " + std::to_string(int(result.error().kind)) + ", native " + std::to_string(int(result.error().nativeResult)));
    }
    std::vector<uint8_t> readDepth(uint32_t id, const int32_t* xy, uint32_t count)
    {
        auto& f = framebuffers.at(id);
        if (!f.depth)
            throw std::runtime_error("Framebuffer has no depth");
        std::vector<uint8_t> result(size_t(count) * 4);
        if (!count)
            return result;
        const uint32_t variant = f.samples > 1;
        auto& depthProgram = depthPrograms[variant];
        if (!depthProgram) {
            std::string source = variant ? "[[vk::binding(0,0)]] Texture2DMS<float> depthImage; float sampleDepth(int2 p){return depthImage.Load(p,0);}\n"
                                         : "[[vk::binding(0,0)]] Texture2D<float> depthImage; float sampleDepth(int2 p){return depthImage.Load(int3(p,0));}\n";
            source += depthSource;
            ShipFuryAttribute a[] = { { 2, 0 }, { 2, 8 } };
            depthProgram = program(source.data(), source.size(), a, 2, 16);
        }
        // Read only the requested pixels. A full-frame read for each glow/sun depth query costs
        // tens of megabytes at high internal resolutions and defeats asynchronous rendering.
        struct Vertex {
            float x, y, px, py;
        };
        constexpr uint32_t batchCapacity = 2048;
        for (uint32_t first = 0; first < count; first += batchCapacity) {
            const uint32_t n = std::min(count - first, batchCapacity);
            framebuffer(UINT32_MAX, n, 1, 1, false);
            std::vector<Vertex> vertices;
            vertices.reserve(size_t(n) * 6);
            for (uint32_t i = 0; i < n; ++i) {
                const float x0 = -1.0f + 2.0f * i / n, x1 = -1.0f + 2.0f * (i + 1) / n;
                const float px = std::clamp(xy[(first + i) * 2], 0, int(f.width) - 1);
                const float py = std::clamp(xy[(first + i) * 2 + 1], 0, int(f.height) - 1);
                vertices.insert(vertices.end(), { { x0, 1, px, py }, { x1, 1, px, py }, { x1, -1, px, py }, { x0, 1, px, py }, { x1, -1, px, py }, { x0, -1, px, py } });
            }
            ShipFuryDraw s = {};
            s.program = depthProgram;
            s.framebuffer = UINT32_MAX;
            s.viewport = s.scissor = { 0, 0, int32_t(n), 1 };
            draw(s, vertices.data(), vertices.size() * sizeof(Vertex), n * 6, nullptr, 0, 0, f.depth.get());
            auto bytes = read(*textures.at(framebuffers.at(UINT32_MAX).color), n, 1);
            std::memcpy(result.data() + size_t(first) * 4, bytes.data(), bytes.size());
        }
        return result;
    }
    std::vector<uint8_t> readColor(uint32_t id)
    {
        auto& f = framebuffers.at(id);
        if (f.samples == 1)
            return read(*textures.at(f.color), f.width, f.height);
        framebuffer(UINT32_MAX - 2, f.width, f.height, 1, false);
        blit(UINT32_MAX - 2, id, { 0, 0, int32_t(f.width), int32_t(f.height) }, { 0, 0, int32_t(f.width), int32_t(f.height) });
        return read(*textures.at(framebuffers.at(UINT32_MAX - 2).color), f.width, f.height);
    }
    std::vector<uint8_t> read(Texture& t, uint32_t w, uint32_t h)
    {
        if (t.resource.desc.sampleNum > 1)
            throw std::runtime_error("Multisample readback requires a resolve first");
        if (w > t.resource.desc.width || h > t.resource.desc.height)
            throw std::runtime_error("Readback exceeds framebuffer");
        uint32_t pitch = alignUp(w * 4, device->deviceDesc().memoryAlignment.uploadBufferTextureRow);
        uint32_t size = alignUp(uint64_t(pitch) * h, device->deviceDesc().memoryAlignment.uploadBufferTextureSlice);
        auto result = device->createBuffer({ size, nri::BufferUsageBits::NONE }, nri::MemoryLocation::HOST_READBACK);
        if (!result)
            throw std::runtime_error("Fury readback allocation failed");
        auto buffer = *result;
        try {
            auto old = t.state;
            transition(t, copySrc);
            commands->readbackTextureToBuffer(t.resource.handle,
                { .width = static_cast<nri::Dim_t>(w),
                    .height = static_cast<nri::Dim_t>(h),
                    .depth = 1,
                    .planes = t.resource.desc.format == nri::Format::D32_SFLOAT ? nri::PlaneBits::DEPTH : nri::PlaneBits::ALL },
                buffer.handle, { 0, pitch, size });
            transition(t, old);
            ++stats.readback_waits;
            flush(true);
            auto* data = device->mapBuffer(buffer);
            if (!data)
                throw std::runtime_error("Fury readback map failed");
            std::vector<uint8_t> bytes(uint64_t(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(bytes.data() + y * w * 4, data + y * pitch, w * 4);
            device->destroyBuffer(buffer);
            return bytes;
        } catch (...) {
            device->waitIdle();
            device->destroyBuffer(buffer);
            throw;
        }
    }
};

namespace {
template <class Fn>
int guarded(ShipFury* c, Fn fn, bool cleanup = false) noexcept
{
    if (!c)
        return 0;
    try {
        if (!c->error.empty()) {
            if (!cleanup)
                return 0;
            c->discardRecording();
        }
        fn();
        return 1;
    } catch (const std::exception& e) {
        if (c->error.empty())
            c->error = e.what();
    } catch (...) {
        if (c->error.empty())
            c->error = "Unknown Fury runtime failure";
    }
    if (cleanup) {
        // A failure in the cleanup's first submission must not leave a surface attached
        // to the SDL window that ImGui destroys immediately after this callback returns.
        try {
            c->discardRecording();
            fn();
        } catch (...) {
            // Cleanup cannot recover normal rendering after a sticky error. Retire all
            // native surfaces before allowing any platform window to be destroyed.
            if (c->surface)
                c->surface->shutdown();
            c->surfaces.clear();
        }
    }
    return 0; // Preserve and report the original failure even if fallback cleanup succeeded.
}
}
extern "C" {
#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
ShipFury* ship_fury_test_incomplete_context(void)
{
    try {
        auto c = std::make_unique<ShipFury>();
        c->registerSurface(0, { 2, nullptr, nullptr, 0 });
        c->error = "Injected Fury initialization failure";
        return c.release();
    } catch (...) {
        return nullptr;
    }
}
int ship_fury_test_fail_next_submit(ShipFury* c)
{
    return guarded(c, [&] {
        c->testFailNextSubmit = true;
    });
}
int ship_fury_test_surface_registered(ShipFury* c, uint32_t id)
{
    return c && c->surfaces.contains(id);
}
ShipFury* ship_fury_test_create(ShipFuryWindow window, uint32_t validation)
{
    try {
        auto c = std::make_unique<ShipFury>();
        guarded(c.get(), [&] {
            c->init(window, validation != 0, true);
        });
        return c.release();
    } catch (...) {
        return nullptr;
    }
}
int ship_fury_test_gate_begin(ShipFury* c)
{
    return guarded(c, [&] {
        if (!c->testQueue || !c->testGateReleased.load())
            throw std::runtime_error("GPU test gate unavailable or already active");
        c->flush(true);
        c->testQueue->wait(c->testQueue->lastSubmittedValue());
        c->testGateValue = c->testQueue->lastSubmittedValue() + 1;
        c->testGateReleased.store(false);
    });
}
int ship_fury_test_gate_release(ShipFury* c)
{
    return c ? c->releaseTestGate() : 0;
}
int ship_fury_test_slot_waiting(ShipFury* c)
{
    return c && c->testSlotWaiting.load();
}
int ship_fury_test_stats(ShipFury* c, ShipFuryTestStats* out)
{
    return guarded(c, [&] {
        const auto completed = c->queue->completedValue();
        *out = { c->queue->lastSubmittedValue(), completed, c->retiredTextures.size(), c->retiredPipelines.size(), 0, static_cast<uint32_t>(c->flights.size()) };
        for (const auto& flight : c->flights)
            out->pending_slots += flight.ticket > completed;
    });
}
#endif
ShipFury* ship_fury_create(ShipFuryWindow window, uint32_t validation)
{
    try {
        auto c = std::make_unique<ShipFury>();
        guarded(c.get(), [&] {
            c->init(window, validation != 0);
        });
        return c.release();
    } catch (...) {
        return nullptr;
    }
}
const char* ship_fury_error(ShipFury* c)
{
    return c ? c->error.c_str() : "Fury context allocation failed";
}
void ship_fury_destroy(ShipFury* c)
{
    delete c;
}
int ship_fury_flush(ShipFury* c)
{
    return guarded(c, [&] {
        c->flush(true);
    });
}
int ship_fury_submit(ShipFury* c)
{
    return guarded(c, [&] {
        c->flush();
    });
}
int ship_fury_present(ShipFury* c, uint32_t w, uint32_t h, uint32_t vsync)
{
    return guarded(c, [&] {
        c->present(0, 0, w, h, vsync != 0);
    });
}
int ship_fury_register_surface(ShipFury* c, uint32_t id, ShipFuryWindow window)
{
    return guarded(c, [&] {
        c->registerSurface(id, window);
    });
}
int ship_fury_present_surface(ShipFury* c, uint32_t id, uint32_t framebuffer, uint32_t w, uint32_t h, uint32_t vsync)
{
    return guarded(c, [&] {
        publicFramebuffer(framebuffer);
        c->present(id, framebuffer, w, h, vsync != 0);
    });
}
int ship_fury_surface_state(ShipFury* c, uint32_t id, uint32_t w, uint32_t h, uint32_t minimized)
{
    return guarded(c, [&] {
        c->surfaceState(id, w, h, minimized != 0);
    });
}
int ship_fury_unregister_surface(ShipFury* c, uint32_t id)
{
    return guarded(
        c,
        [&] {
            c->unregisterSurface(id);
        },
        true);
}
int ship_fury_delete_framebuffer(ShipFury* c, uint32_t id)
{
    return guarded(
        c,
        [&] {
            publicFramebuffer(id);
            c->deleteFramebuffer(id);
        },
        true);
}
int ship_fury_stats(ShipFury* c, ShipFuryStats* out)
{
    return guarded(c, [&] {
        *out = c->stats;
    });
}
uint32_t ship_fury_program(ShipFury* c, const char* source, size_t length, const ShipFuryAttribute* a, uint32_t count, uint32_t stride)
{
    uint32_t id = 0;
    guarded(c, [&] {
        id = c->program(source, length, a, count, stride);
    });
    return id;
}
int ship_fury_clear_programs(ShipFury* c)
{
    return guarded(c, [&] {
        c->flush();
        c->retiredPipelines.reserve(c->retiredPipelines.size() + c->pipelines.size());
        for (auto& [_, p] : c->pipelines)
            c->retiredPipelines.emplace_back(c->queue->lastSubmittedValue(), p);
        c->pipelines.clear();
        c->collect();
        c->programs.clear();
        c->blitProgram = c->presentProgram = 0;
        c->depthPrograms = {};
    });
}
uint32_t ship_fury_texture(ShipFury* c)
{
    uint32_t id = 0;
    guarded(c, [&] {
        if (c->nextTexture == UINT32_MAX)
            throw std::runtime_error("Texture ID exhaustion");
        id = c->nextTexture++;
    });
    return id;
}
int ship_fury_upload(ShipFury* c, uint32_t id, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    return guarded(c, [&] {
        c->uploadTexture(id, rgba, w, h);
    });
}
int ship_fury_delete_texture(ShipFury* c, uint32_t id)
{
    return guarded(c, [&] {
        c->flush();
        auto i = c->textures.find(id);
        if (id && i != c->textures.end()) {
            c->retire(i->second);
            c->textures.erase(i);
        }
    });
}
int ship_fury_framebuffer(ShipFury* c, uint32_t id, uint32_t w, uint32_t h, uint32_t samples, uint32_t depth)
{
    return guarded(c, [&] {
        publicFramebuffer(id);
        c->framebuffer(id, w, h, samples, depth != 0);
    });
}
uint32_t ship_fury_framebuffer_texture(ShipFury* c, uint32_t id)
{
    uint32_t texture = 0;
    guarded(c, [&] {
        publicFramebuffer(id);
        texture = c->framebuffers.at(id).color;
    });
    return texture;
}
int ship_fury_clear(ShipFury* c, uint32_t fb, uint32_t color, uint32_t depth, const ShipFuryRect* region)
{
    return guarded(c, [&] {
        publicFramebuffer(fb);
        if (!region) {
            c->target(fb, color != 0, depth != 0);
            return;
        }
        auto& f = c->framebuffers.at(fb);
        int x = std::max(region->x, 0), y = std::max(region->y, 0);
        int right = std::min(region->x + region->width, int(f.width)), bottom = std::min(region->y + region->height, int(f.height));
        if (right <= x || bottom <= y || !f.depth)
            return;
        c->target(fb);
        nri::ClearAttachmentDesc clear = {};
        clear.value.depthStencil.depth = 1;
        clear.planes = nri::PlaneBits::DEPTH;
        nri::Rect rect = { static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(right - x), static_cast<uint16_t>(bottom - y) };
        if (!c->device->deviceDesc().features.rectDepthStencilClears)
            throw std::runtime_error("Device does not support regional depth clears");
        c->device->core.CmdClearAttachments(c->commands->commandBuffer(), &clear, 1, &rect, 1);
    });
}
int ship_fury_draw(ShipFury* c, const ShipFuryDraw* s, const void* v, size_t bytes, uint32_t count, const void* i, uint32_t indexBytes, int32_t offset)
{
    return guarded(c, [&] {
        publicFramebuffer(s->framebuffer);
        c->draw(*s, v, bytes, count, i, indexBytes, offset);
    });
}
int ship_fury_blit(ShipFury* c, uint32_t dst, uint32_t src, ShipFuryRect dr, ShipFuryRect sr)
{
    return guarded(c, [&] {
        publicFramebuffer(dst);
        publicFramebuffer(src);
        c->blit(dst, src, dr, sr);
    });
}
int ship_fury_read_color(ShipFury* c, uint32_t fb, uint32_t w, uint32_t h, uint16_t* out)
{
    return guarded(c, [&] {
        publicFramebuffer(fb);
        auto& f = c->framebuffers.at(fb);
        if (w > f.width || h > f.height)
            throw std::runtime_error("Readback exceeds framebuffer");
        auto bytes = c->readColor(fb);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t i = (size_t(y) * f.width + x) * 4;
                out[size_t(y) * w + x] = ((bytes[i] >> 3) << 11) | ((bytes[i + 1] >> 3) << 6) | ((bytes[i + 2] >> 3) << 1) | (bytes[i + 3] != 0);
            }
    });
}
int ship_fury_read_depth(ShipFury* c, uint32_t fb, const int32_t* xy, uint32_t count, uint16_t* out)
{
    return guarded(c, [&] {
        publicFramebuffer(fb);
        auto bytes = c->readDepth(fb, xy, count);
        for (uint32_t i = 0; i < count; ++i) {
            size_t pixel = size_t(i) * 4;
            out[i] = uint16_t((uint32_t(bytes[pixel]) | (uint32_t(bytes[pixel + 1]) << 8)) << 2);
        }
    });
}
}
