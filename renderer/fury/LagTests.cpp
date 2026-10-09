#include "RuntimeTestHooks.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
void require(bool ok, const char* message)
{
    if (!ok)
        throw std::runtime_error(message);
}
}

int main()
{
    using namespace std::chrono_literals;
    ShipFury* c = ship_fury_test_create({ 2, nullptr, nullptr, 0 }, 1);
    std::thread releaseThread;
    std::atomic<bool> cleanup { false }, releaseStarted { false }, watchdog { false };
    std::atomic<bool> fourthReturned { false }, observedBlocked { false }, releaseOK { false };
    auto check = [&](bool ok) {
        require(ok, ship_fury_error(c));
    };
    auto finish = [&] {
        cleanup.store(true);
        if (releaseThread.joinable())
            releaseThread.join();
        ship_fury_destroy(c);
    };
    try {
        check(c && !*ship_fury_error(c));
        // Compile, allocate, upload and warm the pipeline before the GPU gate.
        // Ordinary contexts still request zero compute queues.
        const char* shader = R"(
[[vk::binding(0,0)]] Texture2D<float4> image;
[[vk::binding(6,0)]] SamplerState imageSampler;
struct V {float4 position:SV_Position;};
V VSMain([[vk::location(0)]] float4 position:POSITION){V v;v.position=position;return v;}
float4 PSMain(V v):SV_Target{return image.Sample(imageSampler,float2(0.5,0.5));}
)";
        ShipFuryAttribute attribute = { 4, 0 };
        auto program = ship_fury_program(c, shader, std::strlen(shader), &attribute, 1, 16);
        check(program != 0);
        std::array<uint32_t, 3> textures = {};
        const uint8_t colors[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
        for (uint32_t i = 0; i < 3; ++i) {
            textures[i] = ship_fury_texture(c);
            check(textures[i] && ship_fury_upload(c, textures[i], colors[i], 1, 1));
        }
        for (uint32_t i = 0; i < 4; ++i)
            check(ship_fury_framebuffer(c, i, 16, 16, 1, 1));
        ShipFuryDraw draw = {};
        draw.program = program;
        draw.viewport = draw.scissor = { 0, 0, 16, 16 };
        draw.textures[0] = textures[0];
        const float vertices[] = { -1, 1, 0.5, 1, 3, 1, 0.5, 1, -1, -3, 0.5, 1 };
        check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
        check(ship_fury_test_gate_begin(c));
        ShipFuryTestStats before = {}, pending = {};
        ShipFuryStats cpuBefore = {}, cpuAfter = {};
        check(ship_fury_test_stats(c, &before));
        check(ship_fury_stats(c, &cpuBefore));
        require(before.slot_count == 3 && before.completed == before.submitted, "Lag test requires three idle flight slots");

        // A single thread owns compute-queue submission. It never records commands
        // or reads mutable runtime statistics. A defective blocking submit is
        // released after 15 seconds and then fails, rather than hanging this test.
        releaseThread = std::thread([&] {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cleanup.load() && !ship_fury_test_slot_waiting(c)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    watchdog.store(true);
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            if (!cleanup.load() && !watchdog.load()) {
                const auto holdUntil = std::chrono::steady_clock::now() + 500ms;
                while (!cleanup.load() && std::chrono::steady_clock::now() < holdUntil)
                    std::this_thread::sleep_for(1ms);
                observedBlocked.store(ship_fury_test_slot_waiting(c) && !fourthReturned.load());
            }
            releaseStarted.store(true);
            releaseOK.store(ship_fury_test_gate_release(c) != 0);
        });

        for (uint32_t frame = 0; frame < 3; ++frame) {
            draw.framebuffer = frame;
            draw.textures[0] = textures[frame];
            check(ship_fury_draw(c, &draw, vertices, sizeof(vertices), 3, nullptr, 0, 0));
            check(ship_fury_submit(c));
            require(!releaseStarted.load(), "A CPU draw/submit did not return before GPU gate release");
            if (frame == 0)
                check(ship_fury_delete_texture(c, textures[0]));
            if (frame == 1) {
                // The replacement upload joins the third flight slot, but the
                // second draw must retain the original green sampled owner.
                check(ship_fury_upload(c, textures[1], colors[2], 1, 1));
            }
        }
        check(ship_fury_delete_framebuffer(c, 2));
        check(ship_fury_clear_programs(c));
        check(ship_fury_test_stats(c, &pending));
        require(!releaseStarted.load(), "GPU gate released before lifetime assertions");
        require(pending.submitted == before.submitted + 3 && pending.completed == before.completed && pending.pending_slots == 3, "Native completion advanced while graphics was gated");
        require(pending.retired_textures >= before.retired_textures + 4, "Deleted/replaced sampled textures and pending framebuffer owners were not retained");
        require(pending.retired_pipelines > before.retired_pipelines, "Shader-cache clearing destroyed a pending pipeline");

        // No available fourth slot: the release thread observes an actual native
        // slot wait, holds it for 500 ms, and only then enqueues the GPU signal.
        check(ship_fury_clear(c, 3, 1, 1, nullptr));
        fourthReturned.store(true);
        releaseThread.join();
        require(!watchdog.load() && releaseOK.load() && observedBlocked.load(), "Fourth-slot reuse did not wait for the independently released GPU gate");
        check(ship_fury_submit(c));
        check(ship_fury_flush(c));
        check(ship_fury_stats(c, &cpuAfter));
        require(cpuAfter.recycling_waits > cpuBefore.recycling_waits && cpuAfter.readback_waits == cpuBefore.readback_waits, "Slot reuse/readback synchronization accounting mismatch");
        ShipFuryTestStats drained = {};
        check(ship_fury_test_stats(c, &drained));
        require(drained.completed == drained.submitted && drained.pending_slots == 0 && drained.retired_textures == 0 && drained.retired_pipelines == 0,
            "Completed submissions did not reclaim retired owners");
        std::array<uint16_t, 256> pixels = {};
        for (uint32_t fb = 0; fb < 2; ++fb) {
            check(ship_fury_read_color(c, fb, 16, 16, pixels.data()));
            for (auto pixel : pixels)
                require(pixel == (fb == 0 ? 0xf801 : 0x07c1), "Gated draw lost its sampled texture, descriptors, upload bytes or pipeline");
        }
        finish();
        std::puts("Fury deterministic GPU lag, nonblocking submit and asynchronous owner lifetime: PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fury GPU lag test: %s\n", e.what());
        finish();
        return 1;
    }
}
