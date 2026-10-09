# Fury renderer port

## Goal and scope

Implement an opt-in Fury backend for Shipwright that reaches playable gameplay first, then closes rendering parity gaps. Keep OpenGL available as the reference and fallback.

The seam is `Fast::GfxRenderingAPI` in `libultraship/include/fast/backends/gfx_rendering_api.h`. The existing interpreter already supplies clip-space triangles, decoded RGBA textures, combiner identities, and ordered framebuffer operations. Preserve that work; do not translate it into Fury scenes, meshes, or PBR materials.

First target: Linux Vulkan on this machine, including its Wayland session. Use the same adapter design for a later D3D12 qualification, but do not claim that compiling it proves Windows works.

This is an execution plan for a focused first-port session, not a multi-week engine integration. Stop at working checkpoints instead of building speculative infrastructure. A playable first pass and complete parity are separate acceptance gates.

## Current state

- Shipwright branch: `customizations`, pushed to `Daedie-git/Shipwright`.
- Fury is a submodule at `1f0d62824d9dc4c6f8ee35b99d4a16db535ab67c`.
- `SOH_ENABLE_FURY=ON` builds the isolated runtime and registers the selectable **Fury Vulkan** backend (saved backend ID 4). OpenGL remains the default.
- Shipwright/libultraship uses C++20 and spdlog v1; the standalone Fury dependency uses Clang, C++26, and spdlog v2. Keep that isolation.
- `soh` builds and runs with Fury; the original `SOH_ENABLE_FURY=OFF` configuration also builds and launches.
- `libultraship` is forked to `Daedie-git/libultraship` on its own `customizations` branch. The original `kenix3/libultraship` repository remains its upstream.
- This machine has game archives and optional texture/model packs in `build-cmake/soh`. Use a separate clean runtime configuration for the initial comparison rather than changing the existing mod setup.

## Implemented checkpoint

The implementation follows the architecture and staged plan below, with these verified results:

- Linux Vulkan on native Wayland: title sequence, file selection, isolated save creation and gameplay in Link's house.
- Existing ImGui fonts, icons and settings menus render without creating an OpenGL context; input and docking remain on the host side.
- Gameplay and pause background capture work at 1.5x internal resolution with 4x MSAA. Fullscreen and return to windowed mode run with Vulkan validation enabled.
- `ship_fury_runtime_tests` checks bootstrap, rasterization, asymmetric texture/copy/readback orientation, scaled and aliased flipped blits, partial depth clears, blocking reads followed by more drawing, MSAA color/depth reads, texture retirement/replacement, minimized presentation and framebuffer resizing.
- Explicit renderer shutdown precedes native-window destruction. `DeinitOTR()` destroys the host context before Vulkan validation-layer static teardown; normal exit succeeds under the debugger and in regular launches.
- A staged install launches using its adjacent Fury and Slang libraries. This machine's locally supplied SDL2_net and opusfile runtime libraries were also copied into the test stage; normal host dependency provisioning is still required.
- The OpenGL-only build launches and exits successfully after the shared lifetime changes.

Implementation lives in `renderer/fury/` and the libultraship Fury adapter. The existing DX11 HLSL generator is exposed through a portable header and a shared compile guard; it is not yet moved into a separate source file. Fury itself remains unchanged.

### Remaining qualification and limits

- The runtime deliberately waits at every flush/present and before retiring changed resources. It is a correctness-first implementation, not an optimized submission path.
- Custom Prism shader overrides are explicitly unsupported. Detached ImGui platform windows are disabled.
- MSAA depth reads select sample zero and preserve the interpreter's 14-bit depth quantization. Synthetic values pass, but depth-sensitive gameplay effects still need direct OpenGL comparison.
- Optional HD texture packs, model packs and custom shader packs are not qualified. Stock-scene coverage is limited; full visual parity is not claimed.
- Runtime filter/vsync switching, prolonged window resizing/minimize/restore, surface-loss recovery, frame dropping and GUI texture reload still need dedicated live stress tests. A zero-sized presentation is covered headlessly.
- Only Linux Vulkan/Wayland is runtime-qualified. X11 and Windows native handle plumbing exists but has not been tested; D3D12 and other platforms remain deferred.

## Architecture

```text
Shipwright game and existing ImGui context
    -> libultraship interpreter (unchanged command decoding)
    -> GfxRenderingAPIFury (C++20 adapter)
    -> private, plain-data bridge
    -> Shipwright-owned Fury runtime (C++26, isolated build)
    -> Fury renderer core -> NRI Vulkan
```

### Host adapter

Implement `GfxRenderingAPIFury` beside the existing OpenGL/DX11/Metal adapters. It satisfies the existing rendering interface, preserves shader/texture/framebuffer IDs, accumulates mutable draw state, and submits immutable draw descriptions plus copied vertex bytes.

Keep N64 combiner decoding and archive shader lookup on the libultraship side. Shader source generation can also stay there, so the isolated runtime needs neither Shipwright resource types nor its logging headers.

### Isolated runtime

Put game-specific GPU implementation in Shipwright-owned sources, proposed under `renderer/fury/`, compiled by `CMake/FuryRenderer/CMakeLists.txt`. Do not put N64 behavior into the Fury submodule.

Use a small C-compatible bridge header, proposed as `renderer/fury/Bridge.h`, with an opaque context, fixed-width handles, plain draw/resource descriptions, byte spans, and explicit status/error retrieval. Keep C++ containers, exceptions, allocation ownership, spdlog, and ImGui types out of this interface. Borrow input bytes only for the duration of a call; copy everything retained. Free runtime-owned memory in the runtime.

This is an internal build seam, not a general rendering framework. Group mutable draw state into one description instead of exporting every legacy state setter. The caller-facing seam remains `GfxRenderingAPI`.

Use existing Fury primitives:

- `Device::initDevice` and `Renderer::init` for device bootstrap.
- `Renderer::DeviceResourceFactory` for buffers, textures, views, and samplers.
- `Renderer::GraphicsQueue` / `CommandList` for recording, submission, and receipts.
- `Renderer::ShaderCompiler` for Slang compilation to SPIR-V, reflection, and emitted entry-point names.
- `Renderer::Pipeline::Registry` and layout/descriptor primitives where applicable for pipeline ownership and caching.
- `Renderer::Presentation::SurfaceBackend` for native surface acquisition, presentation, resize, and retirement.

Start with explicit ordered command recording on one graphics queue and one CPU owner. Do not install Fury Engine/ECS, its renderer lane, scene system, bindless scene materials, or FrameGraph for this first adapter. The existing interface is sequential and has blocking reads; direct renderer-core use avoids introducing a second scheduling model. Track resource layouts/access transitions privately and use exact submission receipts for recycling and destruction.

## Implementation order

### 0. Establish a reproducible reference and writable nested repo

- Fork `kenix3/libultraship` to our account, keep the original as upstream, and create its `customizations` branch from the pinned revision. Update Shipwright's submodule URL and later pin published commits. Do not push to somebody else's repo or leave unpublished submodule revisions in Shipwright.
- Launch the existing OpenGL build from a clean runtime setup with the existing game archives and no optional packs. Record title/file-select and one gameplay scene; note any pre-existing visual issues.
- Use fixed resolution, matching filtering, MSAA off, and fixed configuration for the first comparison. Leave the user's existing configuration/mod files untouched.

Exit: known-working baseline, repeatable scenes, and both repos ready for changes.

### 1. Boot Fury, present a clear, and resize

- Add an appended `FAST3D_SDL_FURY` backend value, its display name, availability registration, and guarded factory construction. Preserve existing saved backend IDs and defaults.
- Extend the existing SDL window module with an explicit Fury mode; reuse events, keyboard/controller handling, fullscreen, and pacing. Do not copy the whole window implementation.
- Remove the assumption that every non-Apple SDL window uses OpenGL. Create the appropriate Vulkan-capable SDL window and get native Wayland/X11 handles through SDL. Both NRI platform options are enabled in the current dependency build.
- Use Vulkan drawable dimensions rather than GL drawable queries for this mode. Handle minimized/zero-sized windows without acquiring or dividing by zero.
- Initialize the bridge/runtime, reconcile the surface, acquire, clear, submit with acquire/release synchronization, transition to present, and present.
- Keep SDL frame pacing, but bypass GL context creation, GL swap interval, and `SDL_GL_SwapWindow` for Fury. Present exactly once from the Fury path.
- Add bridge build byproducts, exported symbols, linkage, and installation to `CMake/Fury.cmake`. Ensure Slang and the renderer load from the installed layout, not only sibling-checkout absolute paths.

Exit: selectable Fury window shows a clear, survives resize/minimize/restore, exits cleanly, and OpenGL still works.

### 2. Render the existing ImGui UI

This is required early: Shipwright uses ImGui to display the game framebuffer and its menus, not just an optional debug overlay.

- Keep Shipwright's existing ImGui version/context and SDL2 platform backend. Initialize SDL for Vulkan and add Fury cases to event, gamepad, new-frame, render, and shutdown dispatch.
- Implement a small GPU draw-data renderer using the Fury runtime; do not pull in Fury's editor/ECS ImGui integration or a second ImGui library.
- Convert ImGui vertices, indices, and commands to plain bridge descriptions. Handle clip rectangles, display position/scale, vertex/index offsets, font upload, reset-state callbacks, and host user callbacks in command order.
- Return opaque texture tokens through the existing `ImTextureID` and framebuffer texture hooks. Resolve these to runtime-owned sampled views; never pass GL texture names or dangling native descriptors.
- Support resource upload outside an active game frame. Window initialization currently calls `Gui::Init()` before `mRapi->Init()`; explicitly arrange Fury bootstrap/deferred font upload so this order cannot access an uninitialized device.
- Disable detached ImGui platform viewports for Fury initially through `SupportsViewports()`. Keep docking and ordinary menus; add multi-window presentation only after the main-window port works.

Exit: fonts, menus, icons, input, and a sample image work without an OpenGL context.

### 3. Render N64 triangles and reach gameplay

- Implement shader lookup/load/cache metadata and the exact variable vertex layout implied by `CCFeatures`.
- Extract the portable HLSL combiner generator from `gfx_direct3d11.cpp` into a backend-neutral source/header without D3D includes or Windows-only guards. Keep DX11 behavior unchanged.
- Start from `shaders/directx/default.shader.hlsl`, compiled through Fury's Slang compiler. Validate a generated untextured and textured variant immediately; adjust only necessary Slang bindings, semantics, and layout. Do not assume DX11 HLSL compiles unchanged.
- Preserve both combiner cycles, fog, grayscale, alpha edge/threshold/noise/invisibility, primitive depth, and clamp attributes. Reserve all six texture slots: two base textures, two masks, and two replacement/blend textures.
- Implement texture create/select/upload/delete, wrap/mirror/clamp samplers, point/linear/three-point filtering, and texture-cache invalidation on filter changes. Capture textures and sampler policy per draw rather than mutating descriptors already used by recorded work.
- Upload triangle bytes into fence-recycled storage; never retain the interpreter's reusable vertex-buffer pointer.
- Cache pipelines by shader variant plus vertex layout, attachment formats, sample count, depth test/write/decal state, and alpha blend state. Key compiled variants by filter and sRGB policy as well as combiner IDs.
- Preserve draw order. Do not sort opaque or transparent draws during this port.
- Use `[0,1]` clip depth via `GetClipParameters`; qualify Vulkan Y handling with an asymmetric image. Convert the legacy bottom-origin viewport/scissor coordinates consistently. Do not change game transforms or infer framebuffer orientation from one global flip.
- Reproduce existing color behavior using simple UNORM targets and explicit sRGB policy. Do not apply Fury's HDR/PBR display transform or tonemapping.

Exit: title, file selection, in-game HUD, and a representative textured gameplay scene render with functioning menus.

### 4. Complete framebuffer operations and synchronous reads

Basic framebuffer creation/binding/clearing and sampling are needed during step 3; complete the remaining behavior here before claiming parity.

- Represent framebuffer 0 with owned color/depth resources rather than assuming a swapchain image is sampleable or has depth. Preserve the existing direct-to-0 and offscreen-game-target branches; composite framebuffer 0 to the acquired surface at the end.
- Implement attachment creation/resizing, target switching, full clears, scissored depth clears, framebuffer texture selection, color copies, scaled/flipped blits, and MSAA color resolve.
- End/restart rendering when an operation requires copy/transfer/barrier work. Preserve attachment load/store contents and track the actual current layout after every operation.
- A scaled/flipped `CopyFramebuffer` needs a raster blit, not only an equal-sized texture copy. Handle source/destination aliasing with a temporary when necessary.
- Preserve the meaning of `opengl_invertY` for each operation instead of treating the name as a request to copy GL internals. Test drawing, sampling, copies, and readbacks together to avoid double flips.
- Implement blocking color readback with row-pitch handling and exact RGBA5551 conversion, including existing alpha-bit behavior.
- Implement batched pixel-depth reads with the interpreter's coordinate adjustment and expected quantization. Close and submit pending commands, wait for the exact receipt, read, restore state, and resume recording when called mid-frame. Also handle requests after the frame was submitted. Never return a placeholder or a previous frame's depth.
- Add a defined MSAA depth extraction path (sampleable MSAA depth plus a conversion/gather shader if required). Qualify its selection/quantization against the reference; color resolve alone does not implement depth reads.
- Preserve custom Prism shader selection where compatible; report unsupported shader overrides explicitly rather than silently drawing with the default shader.

Exit: pause/background captures, framebuffer effects, depth-dependent gameplay effects, internal resolution changes, and MSAA work correctly.

### 5. Qualify lifetime and package the working port

- Exercise `RunGuiOnly`, frame dropping, runtime filtering changes, GUI texture reload, framebuffer resize, fullscreen, vsync changes, minimize/restore, and repeated startup/shutdown.
- Establish renderer shutdown before SDL destroys native windows. Today `Interpreter::Destroy()` destroys the window before `Fast3dWindow` deletes the renderer; Fury's surface ownership requires an explicit earlier shutdown hook/order.
- Recycle staging, descriptors, pipelines, and replaced textures only after their exact GPU receipts complete. Preserve resources across readback-induced partial submissions.
- Run Vulkan/NRI validation and fix messages before calling the Linux port ready. Failure paths must cancel unresolved acquisitions and retain submitted resources until safe cleanup.
- Verify a staged install can launch with its own renderer/Slang libraries, and verify `SOH_ENABLE_FURY=OFF` still builds the original path.
- Commit/push the libultraship customization first; commit the reachable submodule revision plus Shipwright adapter/runtime changes second. Leave Fury pinned unless a demonstrated generic renderer deficiency requires a separate Fury change.

Exit: repeatable playable Linux port, documented limitations, clean validation on exercised paths, and reproducible published dependencies.

## Verification

Add small scripted adapter tests through `GfxRenderingAPI`, not a new display-list replay framework. Run matching synthetic cases against OpenGL and Fury where possible:

- Asymmetric colored/textured triangles: winding, clip depth, viewport, scissor, framebuffer orientation.
- Combiner variants: both cycles, two textures, mask/replacement slots, fog, alpha, noise, primitive depth, and every filter mode.
- Overlapping triangles: depth test/write combinations, decal behavior, alpha blend, and draw ordering.
- Framebuffers: render-then-sample, copy/scale/flip, partial depth clear, MSAA resolve, and readback.
- Lifetime: upload/draw/delete, resize after submit, and mid-frame readback followed by further drawing.

Use exact assertions for handle/lifetime behavior, packed-color conversion, and depth quantization where defined. Compare image regions with justified tolerance for native raster/filter differences; freeze noise state when comparing shaders.

Then compare real title/file-select/gameplay/pause scenes at identical settings. Enable the existing HD texture pack, model packs, and custom shader packs separately only after stock rendering passes; this distinguishes adapter defects from pack compatibility problems.

## Deliberately deferred

- Fury scenes, ECS integration, PBR conversion, ray tracing, new lighting, and post-processing.
- FrameGraph adoption, asynchronous renderer threads, transfer-queue overlap, draw sorting, and disk pipeline caches.
- Detached ImGui windows and qualification on D3D12, macOS, mobile, or WebGPU.

The playable checkpoint is implemented. Continue with targeted parity and lifetime qualification rather than redesigning the interpreter or adopting Fury's scene system.
