# Fury Vulkan follow-up qualification

## Scope

This records the five follow-up areas: general MSAA copies, custom HLSL shaders, detached ImGui windows, OpenGL/asset qualification, and asynchronous submission/performance. Implementation is opt-in; OpenGL remains the default and fallback. Fury itself stays pinned and unchanged.

The exercised machine uses Linux/Hyprland, NVIDIA GeForce RTX 5080 and driver 610.57.04. Results do not establish other-driver or Windows/D3D12 parity. Game tests use isolated `SHIP_HOME` configurations and saves. Their local captures live under ignored `captures/fury-port/`; ROMs, packs and captures are not committed.

## Automated GPU checks

```sh
cmake --build build-cmake --target soh -j6
ctest --test-dir build-cmake/fury-renderer-clang --output-on-failure
python3 libultraship/tests/fury_shader_override_tests.py build-cmake
python3 libultraship/tests/fury_shader_override_gpu_tests.py build-cmake
python3 libultraship/tests/fury_viewport_host_tests.py
python3 libultraship/tests/keyboard_controller_headless_tests.py --build-dir build-cmake
```

- `ship_fury_runtime`: actual Vulkan raster/color/depth reads, asymmetric orientation, source/destination clipping, transformed/aliased MSAA copies, partial clears, upload replacement, deletion, resize, and pending-resource retention. Depth gathering crosses the 2048-request boundary with 2049 shuffled/duplicate/out-of-range requests on single-sample and MSAA targets; empty requests submit nothing.
- Constant and descriptor reuse is observed through A→B→A color changes plus GUI/frame constant interpretation within one recording, across eight submissions. Allocation-driven rollover is tested with more than one upload segment's data and pixel assertions for draws on both sides of the rollover.
- `ship_fury_gpu_lag`: an independent compute queue gates the graphics queue. Three submissions return while the GPU remains blocked; upload bytes, descriptors and retired texture/framebuffer/pipeline owners survive. Reusing the fourth slot waits for actual completion. A watchdog releases the gate on failure.
- Error injection exercises a submission failure arising during teardown itself, sticky-error framebuffer cleanup, and a partially initialized context with a registered desired surface but no backend. These headless cases do not prove retirement of a realized native swapchain after a driver failure.
- `ship_fury_gl_parity`: a hidden SDL desktop OpenGL context and headless Vulkan runtime compare opaque/explicit-alpha color, asymmetric images, full/scaled/cropped/flipped/clipped copies, MSAA transformations, destination preservation, depth ramps, depth testing and partial clears. Packed colors match exactly on this machine. Boundary-adjacent depth values may differ by one 14-bit bucket; only analytically identified quantization-boundary cases receive that tolerance. MSAA depth uses sample zero; partially covered pixels are not qualified against GL sample locations.
- Shader source tests exercise selected override/include paths, GLSL-only diagnostics, strict vertex/resource ABI checks, formatting variations, conflicting annotations/registers, unsupported resources/types and include cycles/expansion limits. Real Slang/Vulkan tests compile 16 default/custom variants and check every pixel of their 8×8 draws. Archive lookup/adapter ownership are doubled; Prism generation, annotation, Slang, bridge draw and readback are production code. These variants use a constant combiner color and identical white texture slots; they do not qualify distinct mask/blend slot computations.
- Viewport tests are structural host-wiring checks, not SDL/window execution. Compiled keyboard controller tests exercise production mapping dispatch and pad outputs with application-service substitutes: all held mappings release on focus loss while ordinary key events retain first-handled behavior. Twelve assertions fail before the dispatch fix and pass afterward; SDL focus routing is not exercised.

`SOH_ENABLE_FURY=OFF` and `ON` both rebuild successfully after the shared host changes. A separate `BUILD_TESTING=OFF` runtime build succeeds and exports no `ship_fury_test_*` hooks; the test-enabled configuration is restored afterward. No additional game is launched for these build checks.

## Copy and shader contracts

Only complete, equal-sized MSAA-to-single-sample copies use native resolve. Other copies resolve first, then apply the original affine rectangles and nearest-neighbor policy. Out-of-bounds source regions clip the destination instead of replicating edge pixels. Private scratch framebuffer IDs cannot be supplied through the public bridge. Shader-transformed rectangle components are limited to magnitude 16384 to keep exact integer pixel mapping within range.

Custom shaders must provide HLSL compatible with the interpreter's template layout. Pixel computations and ordinary expression macros may change, but canonical vertex parameters, descriptors and constant layouts remain fixed. Macros cannot redefine ABI identifiers; HLSL `#include` is rejected in favor of Prism `@include`, which expands before validation. Nineteen constant-layout mutations and eleven preprocessing bypasses are rejected by both shader harnesses, including split comment delimiters. Line splicing precedes comment recognition, with original-offset mapping for annotations; valid split ABI tokens also compile and render. GLSL-only shaders are not automatically translated and never silently replaced by the default. Missing/invalid overrides name the selected archive path.

Include expansion is limited to 128 includes and 1 MiB cumulative archive input. This prevents include cycles and excessive loaded source, not arbitrary Prism loop computation or generated-output growth. Shader mods are trusted input, as with the existing shader backend; this is not a shader sandbox.

## Native windows

Earlier Wayland tests establish title/save/gameplay, menus, pause captures, fullscreen and clean exit. SDL2 Wayland does not expose ImGui native viewports; docking continues to work.

X11 testing creates two real detached Console/Stats windows with separate surfaces and framebuffers. A native resize originally reproduced an immediate NRI invalid-extent abort. The adapter now pumps SDL events and refreshes every live drawable/minimized state immediately before each presentation, without clearing other windows' pending render flags. Two fixed stress runs complete 160 independently requested detached resizes; rendered contents survive. Native close requests and menu recreation produce healthy secondaries, and closing the main window exits cleanly.

Recreated windows use Shipwright's menu Console/Stats instances rather than the exact legacy GUI instances first detached. A later title-identification discrepancy prevents counting another stress run. Native minimize requests are not observable as iconification in this compositor, so live minimize/restore remains unqualified. Focus-loss release behavior has headless dispatch coverage, not a complete live focus-switch qualification.

Refreshing SDL state is not atomic with swapchain creation. A native resize during the narrower query-to-create interval may still trigger Fury/NRI's fatal extent check. Recoverable out-of-date acquire/present results skip presentation and retry next frame; this does not claim general surface-loss recovery.

## Installed assets

Stock, HD-only, Young Link subset and combined configurations on both OpenGL and Fury reach save A in Link's house and capture gameplay plus pause inventory. Logs confirm loading `OoT_Reloaded_v11.0.0_HD.o2r` and the Young Link 05 model, 06 equipment and 07 textures archives. Configurations request alternate assets and enable those mods.

This is scene-level launch/load smoke coverage only. Replacement activation is not conclusively proven. Files named equipment captures still show inventory because shoulder navigation failed; equipment previews are not qualified. Movement captures do not cover the full animation set. Compositor tiling changes dimensions between runs, so these are not pixel-matched screenshots or performance comparisons. No logged validation/error messages appear, but layer activation is not independently confirmed for these pack runs. The final combined-Fury retry is interrupted when visible testing is stopped; some captures predate that retry's log.

## Performance

CPU sampling identifies full-frame depth readback/memcpy as a substantial cost, even for a few requested pixels. Depth is now gathered on the GPU into a compact row, with batches of at most 2048 requests. Only requested packed values cross to the CPU. Blocking depth queries still synchronize because the interpreter needs their current-frame result.

Submission no longer waits after every present/flush. Three slots retain upload segments, immutable constant views and descriptor sets until their queue receipts complete; replaced resources retire against completed submission values. Upload storage is 64 MiB per slot, 192 MiB total. Constant contents and complete descriptor identities are reused within a slot; caches reset only after that slot is safe to recycle.

Controlled isolated gameplay measurements use native SDL/X11 windows, stock Link's-house gameplay, 1.5× internal resolution and 4× MSAA, with VSync and validation disabled:

| Native window | Requested cap | Five observed FPS samples | Median |
| --- | --- | --- | --- |
| 1280×720 | 240 | 240, 240, 240, 240, 240 | 240 |
| 1280×720 | 120 | 120, 118.7, 120, 120, 120 | 120 |
| 2560×1440 | 240 | 238.8, 240, 240, 240, 240 | 240 |
| 2560×1440 | 120 | 120, 120, 120, 120, 119.9 | 120 |

The runs exit normally. FPS is sampled from the game's Stats display, not external GPU timestamps. These are post-change cap checks, not a matched before/after speedup ratio, an uncapped ceiling, or qualification of every scene. VSync-on testing is limited by the 144 Hz display. Keep `SHIP_FURY_VALIDATION` unset for normal play; merely setting it to `0` still enables validation. The configured VSync variable is `gSettings.VsyncEnabled`, not `gVsyncEnabled`.

The initial visible tests overlap and launch too many instances. After the user's objection, all extra test games are closed; subsequent automatic verification stays headless. Any further visible qualification must run at most one game at a time.
