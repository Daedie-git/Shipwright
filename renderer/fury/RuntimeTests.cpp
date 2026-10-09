#include "Bridge.h"
#include "RuntimeTestHooks.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

int main()
{
    // Headless: creation does not touch the native window until present().
    ShipFury* c = ship_fury_create({ 2, nullptr, nullptr, 0 }, 1);
    auto check = [&](bool ok) {
        if (!ok)
            throw std::runtime_error(ship_fury_error(c));
    };
    try {
        check(c && !*ship_fury_error(c)); // Includes the initial white-texture staging allocation.
        check(ship_fury_framebuffer(c, 0, 16, 16, 1, 1));
        check(ship_fury_clear(c, 0, 1, 1, nullptr));
        ShipFuryRect empty = { 0, 0, 0, 0 };
        check(ship_fury_clear(c, 0, 0, 1, &empty));
        std::array<uint16_t, 256> rgba = {};
        check(ship_fury_read_color(c, 0, 16, 16, rgba.data()));
        for (auto pixel : rgba)
            if (pixel != 1)
                throw std::runtime_error("Opaque clear/readback color mismatch");
        int32_t xy[] = { 0, 0, 8, 8, 15, 15 };
        uint16_t depth[3] = {};
        check(ship_fury_read_depth(c, 0, xy, 3, depth));
        for (auto z : depth)
            if (z != 65532)
                throw std::runtime_error("Clear/readback depth mismatch");
        const char* shader = R"(
struct V {float4 position:SV_Position;};
V VSMain([[vk::location(0)]] float4 position:POSITION){V v;v.position=position;return v;}
float4 PSMain(V v):SV_Target{return float4(1,0,0,1);}
)";
        ShipFuryAttribute attribute = { 4, 0 };
        auto program = ship_fury_program(c, shader, std::strlen(shader), &attribute, 1, 16);
        check(program != 0);
        ShipFuryDraw draw = {};
        draw.program = program;
        draw.viewport = draw.scissor = { 0, 0, 16, 16 };
        draw.depth_test = draw.depth_write = 1;
        float vertices[] = { -1, 1, 0.5, 1, 3, 1, 0.5, 1, -1, -3, 0.5, 1 };
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_read_color(c, 0, 16, 16, rgba.data()));
        for (auto pixel : rgba)
            if (pixel != 0xf801)
                throw std::runtime_error("Triangle/readback color mismatch");
        // Recording must resume correctly after a blocking read.
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_read_depth(c, 0, xy, 3, depth));
        for (auto z : depth)
            if (z != 32768)
                throw std::runtime_error("Triangle/readback depth mismatch");
        check(ship_fury_framebuffer(c, 1, 8, 8, 1, 0));
        check(ship_fury_blit(c, 1, 0, { 0, 0, 8, 8 }, { 0, 0, 16, 16 }));
        std::array<uint16_t, 64> small = {};
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        for (auto pixel : small)
            if (pixel != 0xf801)
                throw std::runtime_error("Scaled blit mismatch");
        check(ship_fury_blit(c, 1, 1, { 8, 8, -8, -8 }, { 0, 0, 8, 8 }));
        check(ship_fury_read_color(c, 1, 4, 4, small.data()));
        for (size_t i = 0; i < 16; ++i)
            if (small[i] != 0xf801)
                throw std::runtime_error("Aliased/flipped/scaled readback mismatch");
        check(ship_fury_framebuffer(c, 2, 16, 16, 4, 1));
        check(ship_fury_clear(c, 2, 1, 1, nullptr));
        draw.framebuffer = 2;
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_read_color(c, 2, 16, 16, rgba.data()));
        for (auto pixel : rgba)
            if (pixel != 0xf801)
                throw std::runtime_error("MSAA color readback mismatch");
        check(ship_fury_read_depth(c, 2, xy, 3, depth));
        for (auto z : depth)
            if (z != 32768)
                throw std::runtime_error("MSAA depth readback mismatch");
        // A deliberately asymmetric image qualifies texture, raster, copy and readback orientation.
        const char* textured = R"(
[[vk::binding(0,0)]] Texture2D<float4> image;
[[vk::binding(6,0)]] SamplerState imageSampler;
struct V {float4 position:SV_Position;float2 uv:TEXCOORD0;};
V VSMain([[vk::location(0)]] float4 position:POSITION){V v;v.position=position;v.uv=float2(position.x+1,1-position.y)*0.5;return v;}
float4 PSMain(V v):SV_Target{return image.Sample(imageSampler,v.uv);}
)";
        auto texture = ship_fury_texture(c);
        uint8_t corners[] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        check(texture != 0 && ship_fury_upload(c, texture, corners, 2, 2));
        draw.program = ship_fury_program(c, textured, std::strlen(textured), &attribute, 1, 16);
        check(draw.program != 0);
        draw.framebuffer = 0;
        draw.textures[0] = texture;
        draw.depth_test = 0;
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_read_color(c, 0, 16, 16, rgba.data()));
        auto expectCorners = [&](const auto& image, uint32_t width, uint16_t tl, uint16_t tr, uint16_t bl, uint16_t br) {
            if (image[0] != tl || image[width - 1] != tr || image[(width - 1) * width] != bl || image[width * width - 1] != br)
                throw std::runtime_error("Asymmetric image orientation mismatch");
        };
        expectCorners(rgba, 16, 0xf801, 0x07c1, 0x003f, 0xffff);
        draw.framebuffer = 2;
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_blit(c, 1, 2, { 0, 0, 8, 8 }, { 0, 0, 16, 16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xf801, 0x07c1, 0x003f, 0xffff);
        check(ship_fury_blit(c, 1, 2, { 0, 0, 8, 8 }, { 8, 8, 8, 8 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        for (auto pixel : small)
            if (pixel != 0xffff)
                throw std::runtime_error("Cropped MSAA blit mismatch");
        check(ship_fury_blit(c, 1, 2, { 0, 0, 8, 8 }, { 16, 16, -16, -16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xffff, 0x003f, 0x07c1, 0xf801);
        check(ship_fury_blit(c, 2, 2, { 16, 16, -16, -16 }, { 0, 0, 16, 16 }));
        check(ship_fury_read_color(c, 2, 16, 16, rgba.data()));
        expectCorners(rgba, 16, 0xffff, 0x003f, 0x07c1, 0xf801);
        draw.framebuffer = 0;
        check(ship_fury_blit(c, 1, 0, { 0, 0, 8, 8 }, { 0, 0, 16, 16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xf801, 0x07c1, 0x003f, 0xffff);
        check(ship_fury_blit(c, 1, 1, { 8, 8, -8, -8 }, { 0, 0, 8, 8 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xffff, 0x003f, 0x07c1, 0xf801);
        // A source region outside the attachment clips the destination, not edge-replicates.
        check(ship_fury_clear(c, 1, 1, 0, nullptr));
        check(ship_fury_blit(c, 1, 0, { 0, 0, 8, 8 }, { -8, 0, 16, 16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 1, 0xf801, 1, 0x003f);
        check(ship_fury_blit(c, 1, 0, { 0, 0, 8, 8 }, { 32, 32, 16, 16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 1, 0xf801, 1, 0x003f);
        check(ship_fury_read_color(c, 0, 4, 4, small.data()));
        for (size_t pixel = 0; pixel < 16; ++pixel)
            if (small[pixel] != 0xf801)
                throw std::runtime_error("Readback region was unexpectedly rescaled");
        ShipFuryRect partial = { 0, 0, 4, 4 };
        check(ship_fury_clear(c, 0, 0, 1, &partial));
        check(ship_fury_read_depth(c, 0, xy, 3, depth));
        if (depth[0] != 65532 || depth[1] != 32768 || depth[2] != 32768)
            throw std::runtime_error("Regional depth clear mismatch");
        // Cross the gather batch boundary with shuffled, duplicated and clamped requests.
        std::vector<int32_t> queries(2049 * 2);
        std::vector<uint16_t> gathered(2049);
        for (size_t i = 0; i < gathered.size(); ++i) {
            constexpr int32_t positions[][2] = { { -100, -100 }, { 100, 100 }, { 3, 3 }, { 4, 0 }, { 0, 4 } };
            queries[i * 2] = positions[(i * 7) % 5][0];
            queries[i * 2 + 1] = positions[(i * 7) % 5][1];
        }
        check(ship_fury_read_depth(c, 0, queries.data(), gathered.size(), gathered.data()));
        for (size_t i = 0; i < gathered.size(); ++i)
            if (gathered[i] != ((i * 7) % 5 == 0 || (i * 7) % 5 == 2 ? 65532 : 32768))
                throw std::runtime_error("Batched depth gather ordering/clamping mismatch");
        check(ship_fury_read_depth(c, 2, queries.data(), gathered.size(), gathered.data()));
        for (auto z : gathered)
            if (z != 32768)
                throw std::runtime_error("Batched MSAA depth gather mismatch");
        ShipFuryStats zeroBefore = {}, zeroAfter = {};
        check(ship_fury_stats(c, &zeroBefore));
        check(ship_fury_read_depth(c, 0, nullptr, 0, nullptr));
        check(ship_fury_stats(c, &zeroAfter));
        if (zeroBefore.readback_waits != zeroAfter.readback_waits || zeroBefore.submissions != zeroAfter.submissions)
            throw std::runtime_error("Empty depth gather submitted work");
        // Deletion and replacement must retire earlier sampled draws, including unsubmitted work.
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_delete_texture(c, texture));
        check(ship_fury_read_color(c, 0, 16, 16, rgba.data()));
        expectCorners(rgba, 16, 0xf801, 0x07c1, 0x003f, 0xffff);
        check(ship_fury_upload(c, texture, corners, 2, 2));
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        std::memset(corners, 255, sizeof(corners));
        check(ship_fury_upload(c, texture, corners, 2, 2));
        check(ship_fury_read_color(c, 0, 16, 16, rgba.data()));
        expectCorners(rgba, 16, 0xf801, 0x07c1, 0x003f, 0xffff);
        check(ship_fury_clear_programs(c));
        draw.program = ship_fury_program(c, textured, std::strlen(textured), &attribute, 1, 16);
        check(draw.program != 0);
        draw.depth_write = 0;
        ShipFuryStats before = {}, after = {};
        check(ship_fury_stats(c, &before));
        // Multiple pending submissions must preserve each draw's descriptors, upload bytes and
        // replaced texture owner. There are deliberately no CPU readbacks in this loop.
        for (uint32_t frame = 0; frame < 12; ++frame) {
            uint8_t pixel[] = { uint8_t(frame % 2 ? 0 : 255), 0, uint8_t(frame % 2 ? 255 : 0), 255 };
            check(ship_fury_upload(c, texture, pixel, 1, 1));
            draw.framebuffer = 10 + frame;
            check(ship_fury_framebuffer(c, draw.framebuffer, 16, 16, 1, 0));
            for (int repeat = 0; repeat < 100; ++repeat)
                check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
            check(ship_fury_submit(c));
        }
        check(ship_fury_stats(c, &after));
        if (after.readback_waits != before.readback_waits || after.submissions < before.submissions + 12)
            throw std::runtime_error("Nonblocking submission accounting mismatch");
        // Pipeline invalidation and framebuffer destruction also retire against submissions.
        check(ship_fury_clear_programs(c));
        for (uint32_t frame = 0; frame < 12; ++frame) {
            check(ship_fury_read_color(c, 10 + frame, 16, 16, rgba.data()));
            for (auto pixel : rgba)
                if (pixel != (frame % 2 ? 0x003f : 0xf801))
                    throw std::runtime_error("In-flight upload/descriptor/texture retirement mismatch");
            check(ship_fury_delete_framebuffer(c, 10 + frame));
        }
        const char* constantsShader = R"(
[[vk::binding(8,0)]] cbuffer Frame {uint noiseFrame;float noiseScale;float2 pad;};
[[vk::binding(9,0)]] cbuffer Dimensions {uint width;uint3 dimensionsPad;uint4 otherDimensions;};
[[vk::binding(10,0)]] cbuffer Primitive {float primitiveDepth;float3 primitivePad;};
struct V {float4 position:SV_Position;};
V VSMain([[vk::location(0)]] float4 position:POSITION){V v;v.position=position;return v;}
float4 PSMain(V v):SV_Target{return float4(noiseScale,float(width),primitiveDepth,1);}
)";
        const char* guiConstantsShader = R"(
[[vk::binding(8,0)]] cbuffer Gui {float4 transform;};
struct V {float4 position:SV_Position;};
V VSMain([[vk::location(0)]] float4 position:POSITION){V v;v.position=position;return v;}
float4 PSMain(V v):SV_Target{return float4(transform.xyz,1);}
)";
        const auto constantsProgram = ship_fury_program(c, constantsShader, std::strlen(constantsShader), &attribute, 1, 16);
        const auto guiConstantsProgram = ship_fury_program(c, guiConstantsShader, std::strlen(guiConstantsShader), &attribute, 1, 16);
        check(constantsProgram && guiConstantsProgram);
        for (uint32_t frame = 0; frame < 8; ++frame)
            check(ship_fury_framebuffer(c, 40 + frame, 15, 4, 1, 0));
        // A->B->A plus GUI->frame interpretation in the SAME recording, followed by slot rollover.
        for (uint32_t frame = 0; frame < 8; ++frame) {
            ShipFuryDraw state = {};
            state.framebuffer = 40 + frame;
            state.viewport = { 0, 0, 15, 4 };
            for (uint32_t band = 0; band < 5; ++band) {
                state.program = band == 3 ? guiConstantsProgram : constantsProgram;
                state.scissor = { int32_t(band * 3), 0, 3, 4 };
                state.gui = band == 3;
                state.noise_scale = band == 1 ? 0 : 1;
                state.width[0] = band == 1 && !(frame % 2);
                state.prim_depth = band == 1 && frame % 2;
                state.gui_transform[2] = 1;
                check(ship_fury_draw(c, &state, vertices, sizeof(vertices), 3, nullptr, 0, 0));
            }
            check(ship_fury_submit(c));
        }
        for (uint32_t frame = 0; frame < 8; ++frame) {
            check(ship_fury_read_color(c, 40 + frame, 15, 4, small.data()));
            for (size_t pixel = 0; pixel < 60; ++pixel) {
                const size_t band = (pixel % 15) / 3;
                const uint16_t expected = band == 3 || (band == 1 && frame % 2) ? 0x003f : band == 1 ? 0x07c1 : 0xf801;
                if (small[pixel] != expected)
                    throw std::runtime_error("Constant cache content/interpretation/slot discrimination mismatch");
            }
            check(ship_fury_delete_framebuffer(c, 40 + frame));
        }
        // Force allocation-driven rollover, not only explicit per-frame submissions.
        std::vector<uint8_t> oversizedVertices(2 * 1024 * 1024);
        std::memcpy(oversizedVertices.data(), vertices, sizeof(vertices));
        check(ship_fury_framebuffer(c, 60, 36, 1, 1, 0));
        ShipFuryStats rolloverBefore = {}, rolloverAfter = {};
        check(ship_fury_stats(c, &rolloverBefore));
        for (int band = 0; band < 36; ++band) {
            ShipFuryDraw state = {};
            state.program = constantsProgram;
            state.framebuffer = 60;
            state.viewport = { 0, 0, 36, 1 };
            state.scissor = { band, 0, 1, 1 };
            state.noise_scale = !(band % 2);
            state.width[0] = band % 2;
            check(ship_fury_draw(c, &state, oversizedVertices.data(), oversizedVertices.size(), 3, nullptr, 0, 0));
        }
        check(ship_fury_stats(c, &rolloverAfter));
        if (rolloverAfter.submissions <= rolloverBefore.submissions || rolloverAfter.readback_waits != rolloverBefore.readback_waits)
            throw std::runtime_error("Staging exhaustion did not submit asynchronously");
        check(ship_fury_read_color(c, 60, 36, 1, small.data()));
        for (size_t pixel = 0; pixel < 36; ++pixel)
            if (small[pixel] != (pixel % 2 ? 0x07c1 : 0xf801))
                throw std::runtime_error("Allocation-driven rollover corrupted earlier draws");
        check(ship_fury_delete_framebuffer(c, 60));
        check(ship_fury_register_surface(c, 7, { 2, nullptr, nullptr, 0 }));
        check(ship_fury_present_surface(c, 7, 0, 0, 0, 1));
        check(ship_fury_unregister_surface(c, 7));
        check(ship_fury_present(c, 0, 0, 1)); // Minimized: never acquires the null native window.
        check(ship_fury_framebuffer(c, 0, 32, 24, 1, 1));
        check(ship_fury_clear(c, 0, 1, 1, nullptr));
        check(ship_fury_flush(c));
        // The first teardown operation itself can fail, not just an earlier draw.
        check(ship_fury_register_surface(c, 9, { 2, nullptr, nullptr, 0 }));
        check(ship_fury_clear(c, 0, 1, 1, nullptr));
        check(ship_fury_test_fail_next_submit(c));
        if (ship_fury_unregister_surface(c, 9) || ship_fury_test_surface_registered(c, 9) || std::strstr(ship_fury_error(c), "Injected Fury graphics submission failure") == nullptr)
            throw std::runtime_error("First-operation teardown failure left a registered surface");
        // Already-sticky cleanup also releases framebuffer ownership safely.
        if (!ship_fury_delete_framebuffer(c, 0))
            throw std::runtime_error("Sticky-error framebuffer cleanup failed");
        ship_fury_destroy(c);
        c = ship_fury_test_incomplete_context();
        if (!c || !ship_fury_test_surface_registered(c, 0) || !ship_fury_unregister_surface(c, 0) || ship_fury_test_surface_registered(c, 0) || !ship_fury_delete_framebuffer(c, 0))
            throw std::runtime_error("Incomplete-context cleanup failed");
        ship_fury_destroy(c);
        std::puts("Fury runtime raster, orientation, readback, MSAA, lifetime, and resize: PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fury runtime test: %s\n", e.what());
        ship_fury_destroy(c);
        return 1;
    }
}
