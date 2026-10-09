#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(SHIP_FURY_BUILD_RUNTIME)
#define SHIP_FURY_EXPORT __declspec(dllexport)
#else
#define SHIP_FURY_EXPORT __declspec(dllimport)
#endif
#else
#define SHIP_FURY_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Private build seam. Input spans are borrowed only during a call. No C++/ImGui ABI crosses it. */
typedef struct ShipFury ShipFury;
typedef struct {
    uint32_t kind; /* 1: Wayland, 2: X11, 3: Win32 */
    void* display;
    void* surface;
    uint64_t window;
} ShipFuryWindow;
typedef struct {
    int32_t x, y, width, height;
} ShipFuryRect;
typedef struct {
    uint32_t components; /* 1..4 float components; 0 means packed RGBA8_UNORM. */
    uint32_t offset;
} ShipFuryAttribute;
typedef struct {
    uint32_t program, framebuffer;
    uint32_t textures[6];
    uint32_t sampler[2]; /* bit 0 linear, bits 1..2 S, bits 3..4 T; 0 repeat, 1 mirror, 2 clamp */
    uint32_t width[2], height[2], linear[2];
    ShipFuryRect viewport, scissor;
    uint32_t depth_test, depth_write, decal, alpha;
    uint32_t noise_frame;
    float noise_scale, prim_depth, decal_slope;
    uint32_t gui;
    float gui_transform[4];
} ShipFuryDraw;
typedef struct {
    uint64_t submissions, recycling_waits, readback_waits;
} ShipFuryStats;

SHIP_FURY_EXPORT ShipFury* ship_fury_create(ShipFuryWindow window, uint32_t validation);
SHIP_FURY_EXPORT const char* ship_fury_error(ShipFury* context);
SHIP_FURY_EXPORT void ship_fury_destroy(ShipFury* context);
SHIP_FURY_EXPORT int ship_fury_present(ShipFury* context, uint32_t width, uint32_t height, uint32_t vsync);
SHIP_FURY_EXPORT int ship_fury_flush(ShipFury* context); /* Submit and wait (explicit synchronization). */
SHIP_FURY_EXPORT int ship_fury_submit(ShipFury* context); /* Submit without waiting; retirement is receipt-based. */
SHIP_FURY_EXPORT int ship_fury_stats(ShipFury* context, ShipFuryStats* stats);
/* Surface IDs are unique within a context; zero belongs to the main window. */
SHIP_FURY_EXPORT int ship_fury_register_surface(ShipFury* context, uint32_t surface, ShipFuryWindow window);
SHIP_FURY_EXPORT int ship_fury_present_surface(ShipFury* context, uint32_t surface, uint32_t framebuffer, uint32_t width, uint32_t height, uint32_t vsync);
SHIP_FURY_EXPORT int ship_fury_surface_state(ShipFury* context, uint32_t surface, uint32_t width, uint32_t height, uint32_t minimized);
SHIP_FURY_EXPORT int ship_fury_unregister_surface(ShipFury* context, uint32_t surface);
SHIP_FURY_EXPORT int ship_fury_delete_framebuffer(ShipFury* context, uint32_t framebuffer);
SHIP_FURY_EXPORT uint32_t ship_fury_program(ShipFury* context, const char* source, size_t length, const ShipFuryAttribute* attributes, uint32_t count, uint32_t stride);
SHIP_FURY_EXPORT int ship_fury_clear_programs(ShipFury* context);
SHIP_FURY_EXPORT uint32_t ship_fury_texture(ShipFury* context);
SHIP_FURY_EXPORT int ship_fury_upload(ShipFury* context, uint32_t texture, const uint8_t* rgba, uint32_t width, uint32_t height);
SHIP_FURY_EXPORT int ship_fury_delete_texture(ShipFury* context, uint32_t texture);
/* Framebuffer IDs UINT32_MAX-2 through UINT32_MAX are private scratch storage. */
SHIP_FURY_EXPORT int ship_fury_framebuffer(ShipFury* context, uint32_t id, uint32_t width, uint32_t height, uint32_t samples, uint32_t depth);
SHIP_FURY_EXPORT uint32_t ship_fury_framebuffer_texture(ShipFury* context, uint32_t id);
SHIP_FURY_EXPORT int ship_fury_clear(ShipFury* context, uint32_t framebuffer, uint32_t color, uint32_t depth, const ShipFuryRect* depth_region);
SHIP_FURY_EXPORT int ship_fury_draw(ShipFury* context, const ShipFuryDraw* state, const void* vertices, size_t bytes, uint32_t count, const void* indices, uint32_t index_bytes, int32_t vertex_offset);
SHIP_FURY_EXPORT int ship_fury_blit(ShipFury* context, uint32_t destination, uint32_t source, ShipFuryRect dst, ShipFuryRect src);
SHIP_FURY_EXPORT int ship_fury_read_color(ShipFury* context, uint32_t framebuffer, uint32_t width, uint32_t height, uint16_t* rgba5551);
SHIP_FURY_EXPORT int ship_fury_read_depth(ShipFury* context, uint32_t framebuffer, const int32_t* xy, uint32_t count, uint16_t* depth);
#ifdef __cplusplus
}
#endif
