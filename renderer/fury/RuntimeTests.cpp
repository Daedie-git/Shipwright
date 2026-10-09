#include "Bridge.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

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
            if (pixel != 0)
                throw std::runtime_error("Clear/readback color mismatch");
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
        check(ship_fury_blit(c, 1, 0, { 0, 0, 8, 8 }, { 0, 0, 16, 16 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xf801, 0x07c1, 0x003f, 0xffff);
        check(ship_fury_blit(c, 1, 1, { 8, 8, -8, -8 }, { 0, 0, 8, 8 }));
        check(ship_fury_read_color(c, 1, 8, 8, small.data()));
        expectCorners(small, 8, 0xffff, 0x003f, 0x07c1, 0xf801);
        ShipFuryRect partial = { 0, 0, 4, 4 };
        check(ship_fury_clear(c, 0, 0, 1, &partial));
        check(ship_fury_read_depth(c, 0, xy, 3, depth));
        if (depth[0] != 65532 || depth[1] != 32768 || depth[2] != 32768)
            throw std::runtime_error("Regional depth clear mismatch");
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
        check(ship_fury_present(c, 0, 0, 1)); // Minimized: never acquires the null native window.
        check(ship_fury_framebuffer(c, 0, 32, 24, 1, 1));
        check(ship_fury_clear(c, 0, 1, 1, nullptr));
        check(ship_fury_flush(c));
        ship_fury_destroy(c);
        std::puts("Fury runtime raster, orientation, readback, MSAA, lifetime, and resize: PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fury runtime test: %s\n", e.what());
        ship_fury_destroy(c);
        return 1;
    }
}
