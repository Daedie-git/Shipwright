// Synthetic C-bridge GPU regression, not proof of adapter-level or real-game parity.
// Logical images/rectangles have a top-left origin on BOTH backends. GL viewport,
// scissor and blit endpoints are converted to bottom-left coordinates; GL readback
// rows are reversed. Clip-space Y stays up; GL clip Z maps Fury's [0,1] to [-1,1].
#define GL_GLEXT_PROTOTYPES 1
#include "Bridge.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int W = 32, H = 24;
struct Vertex {
    float x, y, z, w, r, g, b, a;
};
using Color = std::array<float, 4>;
constexpr Color red { 1, 0, 0, 1 }, green { 0, 1, 0, 1 }, blue { 0, 0, 1, 1 };
constexpr Color yellow { 1, 1, 0, 1 }, magenta { 1, 0, 1, 1 };
struct Target {
    GLuint fbo = 0, color = 0, depth = 0;
    int w, h, samples;
};

void require(bool ok, const std::string& message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void glCheck(const char* operation)
{
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s: OpenGL error 0x%x", operation, error);
        throw std::runtime_error(message);
    }
}
GLuint shader(GLenum stage, const char* source)
{
    GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(std::string("GL shader: ") + log);
    }
    return s;
}

struct Harness {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    ShipFury* fury = nullptr;
    GLuint program = 0, vao = 0, vbo = 0;
    uint32_t furyProgram = 0;
    std::map<uint32_t, Target> targets;
    unsigned toleratedDepth = 0;

    ~Harness()
    {
        if (fury)
            ship_fury_destroy(fury);
        if (gl) {
            for (auto& [id, t] : targets) {
                glDeleteFramebuffers(1, &t.fbo);
                glDeleteRenderbuffers(1, &t.color);
                glDeleteRenderbuffers(1, &t.depth);
            }
            glDeleteProgram(program);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
            SDL_GL_DeleteContext(gl);
        }
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }
    void check(int ok, const char* operation)
    {
        require(ok != 0, std::string(operation) + ": " + (fury ? ship_fury_error(fury) : "null Fury context"));
    }
    void init()
    {
        require(SDL_Init(SDL_INIT_VIDEO) == 0, std::string("SDL video: ") + SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        window = SDL_CreateWindow("ShipFury GL parity", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(window != nullptr, std::string("SDL hidden GL window: ") + SDL_GetError());
        gl = SDL_GL_CreateContext(window);
        require(gl != nullptr, std::string("SDL GL context: ") + SDL_GetError());
        std::printf("OpenGL: %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
        // Fury stays offscreen: no Vulkan swapchain/native-window dependency.
        fury = ship_fury_create({ 2, nullptr, nullptr, 0 }, 1);
        require(fury != nullptr, "Fury creation returned null");
        require(!*ship_fury_error(fury), ship_fury_error(fury));
        const char* vs = R"(#version 330 core
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
out vec4 c;
void main(){gl_Position=vec4(position.xy,position.z*2.0-position.w,position.w);c=color;}
)";
        const char* ps = R"(#version 330 core
in vec4 c;out vec4 outputColor;
void main(){outputColor=c;}
)";
        GLuint v = shader(GL_VERTEX_SHADER, vs), p = shader(GL_FRAGMENT_SHADER, ps);
        program = glCreateProgram();
        glAttachShader(program, v);
        glAttachShader(program, p);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(p);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[4096] = {};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            throw std::runtime_error(std::string("GL link: ") + log);
        }
        const char* slang = R"(
struct V {float4 position:SV_Position;float4 color:COLOR0;};
V VSMain([[vk::location(0)]] float4 position:POSITION,[[vk::location(1)]] float4 color:COLOR0)
{V v;v.position=position;v.color=color;return v;}
float4 PSMain(V v):SV_Target{return v.color;}
)";
        ShipFuryAttribute attributes[] = { { 4, 0 }, { 4, 16 } };
        furyProgram = ship_fury_program(fury, slang, std::strlen(slang), attributes, 2, sizeof(Vertex));
        check(furyProgram != 0, "Fury program");
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(16));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glCheck("initialization");
    }
    void target(uint32_t id, int w = W, int h = H, int samples = 1)
    {
        require(!targets.contains(id), "duplicate target ID");
        Target t { 0, 0, 0, w, h, samples };
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glGenRenderbuffers(1, &t.color);
        glBindRenderbuffer(GL_RENDERBUFFER, t.color);
        if (samples > 1)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color);
        glGenRenderbuffers(1, &t.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
        if (samples > 1)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        targets.emplace(id, t);
        require(status == GL_FRAMEBUFFER_COMPLETE, "incomplete GL framebuffer");
        check(ship_fury_framebuffer(fury, id, w, h, samples, 1), "Fury framebuffer");
        glCheck("framebuffer allocation");
    }
    void clear(uint32_t id, bool color = true, bool depth = true, const ShipFuryRect* region = nullptr)
    {
        auto& t = targets.at(id);
        check(ship_fury_clear(fury, id, color, depth, region), "Fury clear");
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 1); // Desktop renderer clears OPAQUE black, including RGBA5551 alpha.
        glClearDepth(1);
        glDisable(GL_SCISSOR_TEST);
        if (region) {
            require(!color, "regional clear is depth-only");
            int x = std::max(0, region->x), y = std::max(0, region->y);
            int r = std::min(t.w, region->x + region->width), b = std::min(t.h, region->y + region->height);
            if (r <= x || b <= y)
                return;
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, t.h - b, r - x, b - y);
        }
        glClear((color ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
        glDisable(GL_SCISSOR_TEST);
        glCheck("clear");
    }
    void draw(uint32_t id, ShipFuryRect rect, Color color, float leftZ = 0.25f, float rightZ = 0.25f, bool depth = false, bool depthTest = false)
    {
        auto& t = targets.at(id);
        auto vertex = [&](int x, int y, float z) {
            return Vertex { 2.0f * x / t.w - 1, 1 - 2.0f * y / t.h, z, 1, color[0], color[1], color[2], color[3] };
        };
        int r = rect.x + rect.width, b = rect.y + rect.height;
        Vertex vertices[] = { vertex(rect.x, rect.y, leftZ), vertex(r, rect.y, rightZ), vertex(rect.x, b, leftZ), vertex(rect.x, b, leftZ), vertex(r, rect.y, rightZ), vertex(r, b, rightZ) };
        ShipFuryDraw s = {};
        s.program = furyProgram;
        s.framebuffer = id;
        s.viewport = s.scissor = { 0, 0, t.w, t.h };
        s.depth_write = depth;
        s.depth_test = depthTest;
        check(ship_fury_draw(fury, &s, vertices, sizeof(vertices), 6, nullptr, 0, 0), "Fury draw");
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glViewport(0, 0, t.w, t.h);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(depthTest ? GL_LESS : GL_ALWAYS);
        glDepthMask(depth);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glCheck("draw");
    }
    void asymmetric(uint32_t id)
    {
        clear(id);
        draw(id, { 0, 0, 16, 9 }, red);
        draw(id, { 16, 0, 16, 9 }, green);
        draw(id, { 0, 9, 11, 15 }, blue);
        draw(id, { 11, 9, 21, 15 }, yellow);
        draw(id, { 3, 2, 5, 4 }, { 1, 0, 1, 0 }); // Explicit zero alpha, blending disabled.
        draw(id, { 22, 16, 7, 5 }, magenta);
    }
    // GL disallows scaled MSAA blits. Resolve full extent first, then transform
    // the single-sample result. This is the desktop renderer's two-stage oracle.
    GLuint resolved(uint32_t id, GLbitfield mask)
    {
        auto& t = targets.at(id);
        if (t.samples == 1)
            return t.fbo;
        uint32_t scratch = 1000 + id;
        if (!targets.contains(scratch))
            target(scratch, t.w, t.h);
        auto& tmp = targets.at(scratch);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp.fbo);
        glBlitFramebuffer(0, 0, t.w, t.h, 0, 0, t.w, t.h, mask, GL_NEAREST);
        glCheck("full-extent MSAA resolve");
        return tmp.fbo;
    }
    void blit(uint32_t dst, uint32_t src, ShipFuryRect dr, ShipFuryRect sr)
    {
        auto& d = targets.at(dst);
        auto& s = targets.at(src);
        require(dst != src && d.samples == 1, "oracle requires distinct targets and single-sample destination");
        check(ship_fury_blit(fury, dst, src, dr, sr), "Fury blit");
        GLuint source = resolved(src, GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
        glBlitFramebuffer(sr.x, s.h - sr.y, sr.x + sr.width, s.h - sr.y - sr.height, dr.x, d.h - dr.y, dr.x + dr.width, d.h - dr.y - dr.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glCheck("blit");
    }
    std::vector<uint16_t> glColors(uint32_t id)
    {
        auto& t = targets.at(id);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolved(id, GL_COLOR_BUFFER_BIT));
        std::vector<uint8_t> raw(t.w * t.h * 4);
        glReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        glCheck("color readback");
        std::vector<uint16_t> result(t.w * t.h);
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) {
                auto* p = raw.data() + ((t.h - 1 - y) * t.w + x) * 4;
                result[y * t.w + x] = ((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7);
            }
        return result;
    }
    void colors(uint32_t id, const char* scenario)
    {
        auto& t = targets.at(id);
        auto expected = glColors(id);
        std::vector<uint16_t> actual(t.w * t.h);
        check(ship_fury_read_color(fury, id, t.w, t.h, actual.data()), "Fury color readback");
        for (size_t i = 0; i < actual.size(); ++i)
            if (actual[i] != expected[i]) {
                char message[256];
                std::snprintf(
                    message, sizeof(message), "%s: RGBA5551 (%zu,%zu) GL=0x%04x Fury=0x%04x (alpha GL=%u Fury=%u)", scenario, i % t.w, i / t.w, expected[i], actual[i], expected[i] & 1, actual[i] & 1);
                throw std::runtime_error(message);
            }
    }
    std::vector<uint16_t> glDepth(uint32_t id)
    {
        auto& t = targets.at(id);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolved(id, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
        std::vector<uint32_t> raw(t.w * t.h);
        glReadPixels(0, 0, t.w, t.h, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, raw.data());
        glCheck("D24 depth readback");
        std::vector<uint16_t> result(t.w * t.h);
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x)
                result[y * t.w + x] = (raw[(t.h - 1 - y) * t.w + x] >> 18) << 2;
        return result;
    }
    std::vector<uint16_t> furyDepth(uint32_t id)
    {
        auto& t = targets.at(id);
        std::vector<int32_t> xy;
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) {
                xy.push_back(x);
                xy.push_back(y);
            }
        std::vector<uint16_t> out(t.w * t.h);
        check(ship_fury_read_depth(fury, id, xy.data(), out.size(), out.data()), "Fury depth readback");
        return out;
    }
    void depths(uint32_t id, const char* scenario, const std::function<float(int, int)>& analytic = {})
    {
        auto& t = targets.at(id);
        auto expected = glDepth(id), actual = furyDepth(id);
        for (size_t i = 0; i < actual.size(); ++i)
            if (actual[i] != expected[i]) {
                // D24 rounds before dropping 10 bits; D32 truncates z*16384.
                // Only one 14-bit bucket is permitted, AND only within 2e-7 of an
                // analytic bucket boundary. This is not a blanket depth epsilon.
                float z = analytic ? analytic(i % t.w, i / t.w) : -1;
                double bucket = double(z) * 16384.0;
                bool boundary = analytic && std::abs(bucket - std::round(bucket)) <= 2e-7 * 16384.0;
                if (boundary && std::abs(int(actual[i]) - int(expected[i])) == 4) {
                    ++toleratedDepth;
                    continue;
                }
                char message[256];
                std::snprintf(message, sizeof(message), "%s: depth (%zu,%zu) GL D24=%u Fury D32=%u analytic=%.9g", scenario, i % t.w, i / t.w, expected[i], actual[i], z);
                throw std::runtime_error(message);
            }
    }
    void msaaDiagnostic(uint32_t id)
    {
        auto& t = targets.at(id);
        auto g = glDepth(id), f = furyDepth(id);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        std::puts("MSAA depth ramp diagnostic (resolve sample selection is implementation-dependent; not a parity assertion):");
        for (int sample = 0; sample < t.samples; ++sample) {
            float pos[2] = {};
            glGetMultisamplefv(GL_SAMPLE_POSITION, sample, pos);
            std::printf("  GL sample %d: offset=(%.6f,%.6f), logical Y offset=%.6f\n", sample, pos[0], pos[1], 1 - pos[1]);
        }
        for (int x : { 2, 9, 21, 29 }) {
            int y = 12;
            std::printf("  (%d,%d) GL=%u Fury=%u; GL-position candidate buckets:", x, y, g[y * t.w + x], f[y * t.w + x]);
            for (int sample = 0; sample < t.samples; ++sample) {
                float pos[2] = {};
                glGetMultisamplefv(GL_SAMPLE_POSITION, sample, pos);
                double z = 0.1 + 0.8 * (x + pos[0]) / t.w;
                std::printf(" %u", unsigned(std::min(16383.0, std::floor(z * 16384))) * 4);
            }
            std::puts("");
        }
        glCheck("MSAA sample diagnostics");
    }
};
} // namespace

int main(int argc, char** argv)
{
    bool diagnostic = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--diagnose-msaa") == 0)
            diagnostic = true;
        else {
            std::fprintf(stderr, "Usage: %s [--diagnose-msaa]\n", argv[0]);
            return 1;
        }
    }
    const char* skip = std::getenv("SHIP_FURY_PARITY_SKIP_NO_DISPLAY");
    auto present = [](const char* key) {
        const char* s = std::getenv(key);
        return s && *s;
    };
    if (skip && std::strcmp(skip, "1") == 0 && !present("DISPLAY") && !present("WAYLAND_DISPLAY")) {
        std::puts("SKIP: no DISPLAY or WAYLAND_DISPLAY (explicit opt-in)");
        return 77;
    }
    Harness h;
    try {
        h.init();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Parity initialization FAILED: %s\n", e.what());
        return 1;
    }
    int failures = 0;
    auto run = [&](const char* name, const std::function<void()>& fn) {
        try {
            fn();
            std::printf("PASS: %s\n", name);
        } catch (const std::exception& e) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s: %s\n", name, e.what());
        }
    };
    run("opaque clear and explicit alpha bits", [&] {
        h.target(1);
        h.clear(1);
        h.colors(1, "opaque black clear");
        h.draw(1, { 2, 3, 7, 5 }, { 1, 0, 0, 0 });
        h.colors(1, "transparent red patch");
    });
    run("asymmetric raster and single-sample copies", [&] {
        h.target(2);
        h.target(3);
        h.asymmetric(2);
        h.colors(2, "asymmetric raster");
        struct Case {
            const char* name;
            ShipFuryRect dst, src;
        };
        const Case cases[] = {
            { "full copy", { 0, 0, W, H }, { 0, 0, W, H } },
            { "scaled destination preservation", { 4, 3, 16, 12 }, { 0, 0, W, H } },
            { "cropped source", { 3, 4, 20, 12 }, { 4, 3, 20, 12 } },
            { "source XY flip", { 0, 0, W, H }, { W, H, -W, -H } },
            { "destination XY flip", { W, H, -W, -H }, { 0, 0, W, H } },
            { "destination clipping", { -4, -3, W, H }, { 0, 0, W, H } },
            { "source clipping", { 0, 0, W, H }, { -4, -3, W, H } },
        };
        for (auto& c : cases) {
            h.clear(3);
            h.draw(3, { 0, 0, W, H }, magenta);
            h.blit(3, 2, c.dst, c.src);
            h.colors(3, c.name);
        }
    });
    run("asymmetric MSAA resolve, scale, crop, flip and preservation", [&] {
        h.target(4, W, H, 4);
        h.target(5);
        h.asymmetric(4);
        h.colors(4, "MSAA full readback");
        struct Case {
            const char* name;
            ShipFuryRect dst, src;
        };
        const Case cases[] = {
            { "MSAA full copy", { 0, 0, W, H }, { 0, 0, W, H } },
            { "MSAA scaled inset", { 4, 3, 16, 12 }, { 0, 0, W, H } },
            { "MSAA cropped scale", { 2, 4, 24, 16 }, { 4, 3, 12, 8 } },
            { "MSAA source flip", { 0, 0, W, H }, { W, H, -W, -H } },
            { "MSAA destination flip", { 28, 21, -24, -18 }, { 0, 0, W, H } },
            { "MSAA destination clipping", { -4, -3, W, H }, { 0, 0, W, H } },
        };
        for (auto& c : cases) {
            h.clear(5);
            h.draw(5, { 0, 0, W, H }, magenta);
            h.blit(5, 4, c.dst, c.src);
            h.colors(5, c.name);
        }
    });
    run("asymmetric depth patches, depth test and clipped regional clear", [&] {
        h.target(6);
        h.clear(6);
        h.depths(6, "depth clear");
        h.draw(6, { 0, 0, W, H }, blue, 0.73123f, 0.73123f, true);
        h.draw(6, { 2, 3, 9, 7 }, red, 0.12345f, 0.12345f, true, true);
        h.draw(6, { 19, 12, 11, 9 }, green, 0.87654f, 0.87654f, true, true); // Must fail LESS.
        h.draw(6, { 16, 2, 13, 6 }, yellow, 0.4321f, 0.4321f, true, true);
        h.depths(6, "nonuniform depth patches");
        h.colors(6, "depth test colors");
        auto colorBefore = h.glColors(6);
        ShipFuryRect region { -3, 1, 9, 8 };
        h.clear(6, false, true, &region);
        h.depths(6, "clipped top-left regional depth clear");
        h.colors(6, "regional clear preserves color");
        require(h.glColors(6) == colorBefore, "GL regional clear changed color");
        ShipFuryRect empty { 10, 10, 0, 0 };
        h.clear(6, false, true, &empty);
        h.depths(6, "empty regional clear");
    });
    run("depth ramp and explicit quantization boundaries", [&] {
        h.target(7);
        h.clear(7);
        h.draw(7, { 0, 0, W, H }, red, 0.1f, 0.9f, true);
        h.depths(7, "varying depth ramp", [](int x, int) {
            return 0.1f + 0.8f * (x + 0.5f) / W;
        });
        const float values[] = { 0.0f, 0.03125f, 0.12345f, 0.24999997f, 0.25f, 0.25000003f, 0.49999997f, 0.5f, 0.50000006f, 0.73123f, 0.9999f, 1.0f };
        for (int y = 0; y < 12; ++y)
            h.draw(7, { 0, y * 2, W, 2 }, green, values[y], values[y], true);
        h.depths(7, "boundary depth stripes", [&](int, int y) {
            return values[y / 2];
        });
    });
    run("MSAA nonuniform flat depth and regional clear", [&] {
        h.target(8, W, H, 4);
        h.clear(8);
        h.draw(8, { 0, 0, W, H }, blue, 0.73123f, 0.73123f, true);
        h.draw(8, { 1, 2, 10, 7 }, red, 0.12345f, 0.12345f, true);
        h.draw(8, { 17, 11, 13, 10 }, green, 0.4321f, 0.4321f, true);
        h.depths(8, "MSAA flat asymmetric depths");
        ShipFuryRect region { 20, 14, 20, 20 };
        h.clear(8, false, true, &region);
        h.depths(8, "MSAA clipped regional clear");
    });
    if (diagnostic)
        run("MSAA sample-selection diagnostic", [&] {
            h.target(9, W, H, 4);
            h.clear(9);
            h.draw(9, { 0, 0, W, H }, red, 0.1f, 0.9f, true);
            h.msaaDiagnostic(9);
        });
    std::printf("Synthetic bridge/GL parity: %d failed scenarios; %u boundary-adjacent depth pixels differ by one bucket.\n", failures, h.toleratedDepth);
    return failures ? 1 : 0;
}
