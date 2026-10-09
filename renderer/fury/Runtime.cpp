#include "Bridge.h"

#include "device/DeviceInit.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/device/Device.hpp"
#include "renderer/device/GraphicsQueue.hpp"
#include "renderer/presentation/SurfaceBackend.hpp"
#include "renderer/resources/UploadRing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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
constexpr nri::AccessLayoutStage sampled = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER };
constexpr nri::AccessLayoutStage colorTarget = { nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT };
constexpr nri::AccessLayoutStage depthTarget
    = { nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ | nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE, nri::Layout::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::DEPTH_STENCIL_ATTACHMENT };
constexpr nri::AccessLayoutStage copySrc = { nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY };
constexpr nri::AccessLayoutStage copyDst = { nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY };

// Runtime owners outlive submitted work. The first adapter deliberately waits at flush/present.
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
struct Framebuffer {
    uint32_t color = 0;
    std::unique_ptr<Texture> depth;
    uint32_t width = 0, height = 0, samples = 1;
};
const char* blitSource = R"(
[[vk::binding(0,0)]] Texture2D<float4> image : register(t0);
[[vk::binding(6,0)]] SamplerState imageSampler : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V VSMain([[vk::location(0)]] float2 position : POSITION, [[vk::location(1)]] float2 uv : TEXCOORD0) {
    V v; v.position = float4(position,0,1); v.uv=uv; return v;
}
float4 PSMain(V v) : SV_Target { return image.Sample(imageSampler,v.uv); }
)";
const char* depthSource = R"(
[[vk::binding(0,0)]] Texture2DMS<float> depthImage : register(t0);
struct V {float4 position:SV_Position;};
V VSMain([[vk::location(0)]] float2 position:POSITION){V v;v.position=float4(position,0,1);return v;}
float4 PSMain(V v):SV_Target {
    uint z=min(uint(saturate(depthImage.Load(int2(v.position.xy),0))*16384.0),16383u);
    return float4((z&255)/255.0,(z>>8)/255.0,0,1);
}
)";
}

struct ShipFury {
    std::string error;
    Device::NativeHandles native = {};
    std::unique_ptr<Renderer::Device> device;
    std::unique_ptr<Renderer::GraphicsQueue> queue;
    std::unique_ptr<Renderer::Presentation::SurfaceBackend> surface;
    nri::Window window = {};
    Renderer::ShaderCompiler compiler;
    std::optional<Renderer::CommandList> commands;
    std::optional<Renderer::UploadRing> upload;
    nri::DescriptorPool* pool = nullptr;
    nri::PipelineLayout* layout = nullptr;
    bool rendering = false;
    uint32_t renderFb = UINT32_MAX, sets = 0, nextTexture = 1, nextProgram = 1, blitProgram = 0, depthProgram = 0;
    uint64_t surfaceSequence = 0;
    std::map<uint32_t, std::unique_ptr<Texture>> textures;
    std::map<uint32_t, Framebuffer> framebuffers;
    std::map<uint32_t, Program> programs;
    using PipelineKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<PipelineKey, nri::Pipeline*> pipelines;
    std::map<uint32_t, Renderer::SamplerView> samplers;
    std::vector<Renderer::BufferView> constantViews;

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
        device->waitIdle();
        if (surface)
            surface->shutdown();
        surface.reset();
        framebuffers.clear();
        textures.clear();
        for (auto& [_, p] : pipelines)
            device->core.DestroyPipeline(p);
        for (auto& [_, s] : samplers)
            device->destroyDescriptor(s);
        for (auto& v : constantViews)
            device->destroyDescriptor(v);
        if (pool)
            device->core.DestroyDescriptorPool(pool);
        if (layout)
            device->core.DestroyPipelineLayout(layout);
        upload.reset();
        queue.reset();
        Renderer::shutdown(*device);
        device.reset();
        Device::shutdownDevice(native);
    }

    void init(ShipFuryWindow w, bool validation)
    {
        Device::DeviceInitDesc desc;
        desc.computeQueueCount = 0;
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
        if (w.kind == 1)
            window.wayland = { w.display, w.surface };
        else if (w.kind == 2)
            window.x11 = { w.display, w.window };
        else if (w.kind == 3)
            window.windows = { w.surface };
        else
            throw std::runtime_error("Unsupported SDL native window system");
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
        check(device->core.CreateDescriptorPool(device->device, pd, pool), "CreateDescriptorPool");
        auto u = device->createUploadRing(64ull * 1024 * 1024, nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX | nri::BufferUsageBits::CONSTANT, 1);
        if (!u)
            throw std::runtime_error("Could not allocate Fury upload storage");
        upload.emplace(std::move(*u));
        upload->reset();
        const uint8_t white[4] = { 255, 255, 255, 255 };
        uploadTexture(0, white, 1, 1);
    }

    void begin()
    {
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
    void reclaim()
    {
        for (auto& v : constantViews)
            device->destroyDescriptor(v);
        constantViews.clear();
        device->core.ResetDescriptorPool(*pool);
        sets = 0;
        upload->reset();
        queue->nextFrame();
    }
    void flush()
    {
        if (!commands)
            return;
        endRendering();
        upload->flush();
        commands->end();
        auto receipt = queue->submit(*commands);
        if (!receipt)
            throw std::runtime_error("Fury graphics submission failed");
        queue->wait(receipt->value());
        commands.reset();
        reclaim();
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
        auto s = upload->allocate(
            bytes,
            [&](Span<std::byte> out) {
                std::memcpy(out.data(), data, bytes);
            },
            alignment);
        if (!s) {
            flush();
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
        textures[id] = std::move(t);
    }
    void framebuffer(uint32_t id, uint32_t w, uint32_t h, uint32_t samples, bool depth)
    {
        auto& f = framebuffers[id];
        if (f.width == w && f.height == h && f.samples == samples && bool(f.depth) == depth)
            return;
        flush();
        if (!f.color)
            f.color = nextTexture++;
        textures[f.color] = makeTexture(w, h, nri::Format::RGBA8_UNORM, samples, false);
        f.depth = depth ? makeTexture(w, h, nri::Format::D32_SFLOAT, samples, true) : nullptr;
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
    uint32_t program(const char* source, size_t length, const ShipFuryAttribute* attributes, uint32_t count, uint32_t stride)
    {
        Program p;
        p.stride = stride;
        Renderer::ShaderCompiler::CompileOptions opts;
        opts.entryPoint = "VSMain";
        auto vs = compiler.compileShader({ source, length }, Renderer::ShaderType::eVertex, opts, "ship-fury");
        if (!vs)
            throw std::runtime_error(vs.error().c_str());
        opts.entryPoint = "PSMain";
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
        PipelineKey key = { s.program, static_cast<uint32_t>(color), static_cast<uint32_t>(depth), samples, s.depth_test | (s.depth_write << 1), s.decal, s.alpha };
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
            desc.rasterization.depthBias = { .constant = -2, .slope = -2 };
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
        auto s = stage(data, bytes, device->deviceDesc().memoryAlignment.constantBufferOffset);
        auto v = device->createBufferView(Renderer::makeBufferViewDesc(s.buffer, nri::BufferView::CONSTANT_BUFFER, nri::Format::UNKNOWN, s.offset, s.size));
        if (!v)
            throw std::runtime_error("Fury constant buffer view failed");
        try {
            constantViews.push_back(*v);
        } catch (...) {
            device->destroyDescriptor(*v);
            throw;
        }
        return v->descriptor;
    }
    void draw(const ShipFuryDraw& s, const void* vertices, size_t bytes, uint32_t count, const void* indices, uint32_t indexBytes, int32_t vertexOffset, Texture* firstTexture = nullptr)
    {
        if (!count || s.viewport.width <= 0 || s.viewport.height <= 0 || s.scissor.width <= 0 || s.scissor.height <= 0)
            return;
        if (sets >= 4000 || !upload->canAllocate(bytes + uint64_t(count) * indexBytes + 4096, 256))
            flush();
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
        descriptors[8] = constant(s.gui ? static_cast<const void*>(s.gui_transform) : &frame, 16);
        uint32_t dims[8] = { s.width[0], s.height[0], s.linear[0], 0, s.width[1], s.height[1], s.linear[1], 0 };
        descriptors[9] = constant(dims, sizeof(dims));
        float prim[4] = { s.prim_depth, 0, 0, 0 };
        descriptors[10] = constant(prim, sizeof(prim));
        auto v = stage(vertices, bytes);
        std::optional<Renderer::UploadSlice> ix;
        if (indices)
            ix = stage(indices, uint64_t(count) * indexBytes);
        nri::DescriptorSet* set = nullptr;
        check(device->core.AllocateDescriptorSets(*pool, *layout, 0, &set, 1, 0), "AllocateDescriptorSets");
        ++sets;
        std::array<nri::UpdateDescriptorRangeDesc, 11> updates = {};
        for (uint32_t i = 0; i < 11; ++i)
            updates[i] = { set, i, 0, &descriptors[i], 1 };
        device->core.UpdateDescriptorRanges(updates.data(), updates.size());
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
            if (f.width != df.width || f.height != df.height || df.samples != 1)
                throw std::runtime_error("MSAA resolve requires equal-sized single-sample destination");
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
            blitProgram = program(blitSource, std::strlen(blitSource), attributes, 2, 16);
        }
        struct V {
            float x, y, u, v;
        };
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
        s.viewport = dr;
        s.scissor = { 0, 0, int32_t(df.width), int32_t(df.height) };
        draw(s, vertices, sizeof(vertices), 6, nullptr, 0, 0);
    }
    void present(uint32_t w, uint32_t h, bool vsync)
    {
        using namespace Renderer::Presentation;
        flush();
        SurfaceFrameBatch batch(++surfaceSequence);
        SurfaceDesiredState desired = {};
        desired.key = { { 1 }, 1 };
        desired.window.value = window;
        desired.extent = { w, h };
        desired.minimized = !w || !h;
        desired.presentMode = vsync ? SurfacePresentMode::Fifo : SurfacePresentMode::Immediate;
        if (auto existing = surface->snapshot({ 1 }); existing && existing->format != nri::Format::UNKNOWN)
            desired.requiredFormat = existing->format;
        if (!batch.add(desired))
            throw std::runtime_error("Invalid Fury presentation state");
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
        auto snapshot = surface->snapshot({ 1 });
        if (snapshot && snapshot->status == SurfaceStatus::AwaitingFormat) {
            // Discovery publishes a format; acknowledge that exact contract before acquisition.
            desired.requiredFormat = snapshot->format;
            SurfaceFrameBatch confirmed(++surfaceSequence);
            if (!confirmed.add(desired))
                throw std::runtime_error("Invalid discovered Fury surface format");
            auto ready = surface->reconcile(std::move(confirmed));
            if (ready.accepted && ready.failure && ready.failure->nativeResult == nri::Result::OUT_OF_DATE)
                return;
            if (!ready.accepted || ready.failure)
                throw std::runtime_error("Fury surface format confirmation failed");
        }
        SurfaceKey key = { { 1 }, 1 };
        auto acquired = surface->acquire({ &key, 1 });
        if (!acquired) {
            if (acquired.error().nativeResult == nri::Result::OUT_OF_DATE)
                return;
            throw std::runtime_error("Fury surface acquisition failed: kind " + std::to_string(int(acquired.error().kind)) + ", native " + std::to_string(int(acquired.error().nativeResult)));
        }
        auto& t = acquired->targets()[0];
        // Render framebuffer zero to the actual surface with the same blit shader.
        if (!blitProgram) {
            ShipFuryAttribute a[] = { { 2, 0 }, { 2, 8 } };
            blitProgram = program(blitSource, std::strlen(blitSource), a, 2, 16);
        }
        ShipFuryDraw s = {};
        s.program = blitProgram;
        s.textures[0] = framebuffers.at(0).color;
        s.sampler[0] = 1 | (2 << 1) | (2 << 3);
        auto& image = *textures.at(s.textures[0]);
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
        nri::DescriptorSet* set = nullptr;
        check(device->core.AllocateDescriptorSets(*pool, *layout, 0, &set, 1, 0), "Presentation descriptor set");
        ++sets;
        std::array<nri::UpdateDescriptorRangeDesc, 11> updates = {};
        for (uint32_t i = 0; i < 11; ++i)
            updates[i] = { set, i, 0, &descriptors[i], 1 };
        device->core.UpdateDescriptorRanges(updates.data(), updates.size());
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
        auto result = acquired->present({ &submission, 1 });
        queue->wait(receipt->value());
        commands.reset();
        reclaim();
        if (!result && result.error().nativeResult != nri::Result::OUT_OF_DATE)
            throw std::runtime_error("Fury present failed: kind " + std::to_string(int(result.error().kind)) + ", native " + std::to_string(int(result.error().nativeResult)));
    }
    std::vector<uint8_t> readDepth(uint32_t id)
    {
        auto& f = framebuffers.at(id);
        if (!f.depth)
            throw std::runtime_error("Framebuffer has no depth");
        if (f.samples == 1)
            return read(*f.depth, f.width, f.height);
        framebuffer(UINT32_MAX, f.width, f.height, 1, false);
        if (!depthProgram) {
            ShipFuryAttribute a = { 2, 0 };
            depthProgram = program(depthSource, std::strlen(depthSource), &a, 1, 8);
        }
        float vertices[] = { -1, 1, 3, 1, -1, -3 };
        ShipFuryDraw s = {};
        s.program = depthProgram;
        s.framebuffer = UINT32_MAX;
        s.viewport = s.scissor = { 0, 0, int32_t(f.width), int32_t(f.height) };
        draw(s, vertices, sizeof(vertices), 3, nullptr, 0, 0, f.depth.get());
        return read(*textures.at(framebuffers.at(UINT32_MAX).color), f.width, f.height);
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
            flush();
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
int guarded(ShipFury* c, Fn fn) noexcept
{
    if (!c)
        return 0;
    try {
        if (!c->error.empty())
            return 0;
        fn();
        return 1;
    } catch (const std::exception& e) {
        c->error = e.what();
        return 0;
    } catch (...) {
        c->error = "Unknown Fury runtime failure";
        return 0;
    }
}
}
extern "C" {
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
        c->flush();
    });
}
int ship_fury_present(ShipFury* c, uint32_t w, uint32_t h, uint32_t vsync)
{
    return guarded(c, [&] {
        c->present(w, h, vsync != 0);
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
        for (auto& [_, p] : c->pipelines)
            c->device->core.DestroyPipeline(p);
        c->pipelines.clear();
        c->programs.clear();
        c->blitProgram = 0;
        c->depthProgram = 0;
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
        if (id)
            c->textures.erase(id);
    });
}
int ship_fury_framebuffer(ShipFury* c, uint32_t id, uint32_t w, uint32_t h, uint32_t samples, uint32_t depth)
{
    return guarded(c, [&] {
        c->framebuffer(id, w, h, samples, depth != 0);
    });
}
uint32_t ship_fury_framebuffer_texture(ShipFury* c, uint32_t id)
{
    uint32_t texture = 0;
    guarded(c, [&] {
        texture = c->framebuffers.at(id).color;
    });
    return texture;
}
int ship_fury_clear(ShipFury* c, uint32_t fb, uint32_t color, uint32_t depth, const ShipFuryRect* region)
{
    return guarded(c, [&] {
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
        c->draw(*s, v, bytes, count, i, indexBytes, offset);
    });
}
int ship_fury_blit(ShipFury* c, uint32_t dst, uint32_t src, ShipFuryRect dr, ShipFuryRect sr)
{
    return guarded(c, [&] {
        c->blit(dst, src, dr, sr);
    });
}
int ship_fury_read_color(ShipFury* c, uint32_t fb, uint32_t w, uint32_t h, uint16_t* out)
{
    return guarded(c, [&] {
        auto& f = c->framebuffers.at(fb);
        auto bytes = c->readColor(fb);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t i = (size_t(y) * f.height / h * f.width + size_t(x) * f.width / w) * 4;
                out[size_t(y) * w + x] = ((bytes[i] >> 3) << 11) | ((bytes[i + 1] >> 3) << 6) | ((bytes[i + 2] >> 3) << 1) | (bytes[i + 3] != 0);
            }
    });
}
int ship_fury_read_depth(ShipFury* c, uint32_t fb, const int32_t* xy, uint32_t count, uint16_t* out)
{
    return guarded(c, [&] {
        auto& f = c->framebuffers.at(fb);
        auto bytes = c->readDepth(fb);
        for (uint32_t i = 0; i < count; ++i) {
            int x = std::clamp(xy[i * 2], 0, int(f.width) - 1), y = std::clamp(xy[i * 2 + 1], 0, int(f.height) - 1);
            size_t pixel = (size_t(y) * f.width + x) * 4;
            if (f.samples > 1)
                out[i] = uint16_t((uint32_t(bytes[pixel]) | (uint32_t(bytes[pixel + 1]) << 8)) << 2);
            else {
                float z;
                std::memcpy(&z, bytes.data() + pixel, 4);
                out[i] = uint16_t(std::min(uint32_t(std::clamp(z, 0.0f, 1.0f) * 16384.0f), 16383u) << 2);
            }
        }
    });
}
}
