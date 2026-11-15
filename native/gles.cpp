// The GL ES 3.0 renderer: the port's second backend, for phones whose Vulkan driver draws the picture wrong. A Mali-G57
// in the reports stretches a character's model toward the camera, with the same game files, the same render targets and
// the same SPIR-V; GL ES is a different driver front end on the same silicon, and the shaders reach it through
// SPIRV-Cross instead of the driver's Vulkan compiler.
//
// It draws what the Vulkan backend draws, from the same gpu::State, the same HLSL and the same constants (draw_common.h
// holds those verbatim). Where GL ES 3.00 cannot express something the picture differs, and each such place says so.
#include <SDL3/SDL.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include "app/app.h"
#include "core/log.h"
#include "draw_common.h"
#include "glsl.h"
#include "gles.h"
#include "gpu/backend.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "vk_shader.h"

namespace {

using namespace gpu;
using namespace port::draw;

// ---- what a draw's textures are asked for, as GL sampler state

GLenum Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 0x300: return alpha ? GL_SRC_ALPHA : GL_SRC_COLOR;
        case 0x301: return alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
        case 0x302: return GL_SRC_ALPHA;
        case 0x303: return GL_ONE_MINUS_SRC_ALPHA;
        case 0x304: return GL_DST_ALPHA;
        case 0x305: return GL_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? GL_DST_ALPHA : GL_DST_COLOR;
        case 0x307: return alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? GL_ONE : GL_SRC_ALPHA_SATURATE;
        case 0x8001: return GL_CONSTANT_COLOR;
        case 0x8002: return GL_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return GL_CONSTANT_ALPHA;
        case 0x8004: return GL_ONE_MINUS_CONSTANT_ALPHA;
        default: return GL_ONE;
    }
}

GLenum BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return GL_FUNC_SUBTRACT;
        case 0x800B: return GL_FUNC_REVERSE_SUBTRACT;
        case 0x8007: return GL_MIN;
        case 0x8008: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

GLenum StencilOp(uint32_t v) {
    switch (v) {
        case 0: return GL_ZERO;
        case 0x1E01: return GL_REPLACE;
        case 0x1E02: return GL_INCR;
        case 0x1E03: return GL_DECR;
        case 0x150A: return GL_INVERT;
        case 0x8507: return GL_INCR_WRAP;
        case 0x8508: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

GLenum Wrap(uint32_t v) {
    switch (v) {
        case 2: return GL_MIRRORED_REPEAT;
        case 3: case 5: return GL_CLAMP_TO_EDGE;
        default: return GL_REPEAT;  // 4, clamp to border, has no GL ES 3.00 form: its edges come out the frame's
    }
}

// the console's depth comparison as GL ES names the same eight: the render states hold D3DCMP_NEVER 0x200 to
// D3DCMP_ALWAYS 0x207, in the order GL keeps them, and this used to read them as 1..8, which turned every compare
// into "always" -- depth, stencil and the shadow sampler's compare all pass whatever is in the buffer
GLenum Compare(uint32_t v) {
    static const GLenum kCompare[8] = {GL_NEVER,  GL_LESS,    GL_EQUAL,  GL_LEQUAL,
                                       GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    if (v >= 0x200 && v <= 0x207) return kCompare[v - 0x200];
    if (v >= 1 && v <= 8) return kCompare[v - 1];  // the same eight without the 0x200, as some callers pass them
    return GL_ALWAYS;
}

GLenum MinFilter(uint32_t min, uint32_t mip, bool mipmapped, bool linearable) {
    bool lin = min >= kTexfLinear && linearable;
    if (!mipmapped) return lin ? GL_LINEAR : GL_NEAREST;
    if (mip == kTexfLinear) return lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    return lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
}

struct GImage {
    GLuint tex = 0;
    uint32_t w = 0, h = 0;
};

struct Target {
    GImage img;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    GLuint fbo = 0;
    bool drawn = false;   // the GPU wrote it since the last read-back
    uint64_t sig = 0;     // guest memory as last matched
    uint64_t epoch = 0;   // the lock counter it was last uploaded at
    std::vector<uint8_t> read;
};

struct Tex {
    GImage img;
    TextureDesc d;
    uint32_t levels = 1;
    bool decode = false;  // DXT unpacked here, as every phone needs
    uint64_t sig = 0;
    uint64_t epoch = 0;
};

struct Program {
    GLuint id = 0;
    std::vector<glsl::Sampler> samplers;
};

// the program a full screen pass runs: the frame's front target to the window, with the console's gamma ramp on top
// the build this file is, on the screen: a screenshot then says which one the phone is running
constexpr const char* kGlBuild = "gamma3";

const char* kPresentVertex = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
)";

// the same full screen triangle, with the frame's depth buffer as its picture: a frame whose depth holds 1.0
// everywhere had no geometry rasterised into it at all, and that is the difference between a vertex path and a
// pixel path that has to be told apart from the screen alone
const char* kDepthProbePixel = R"(#version 300 es
precision highp float;
uniform sampler2D src;
in vec2 v_uv;
out vec4 o;
void main() {
    float d = texture(src, v_uv).r;
    o = vec4(d, d, d, 1.0);
}
)";

const char* kPresentPixel = R"(#version 300 es
precision highp float;
uniform sampler2D src;
uniform sampler2D lut;
uniform int gamma;
in vec2 v_uv;
out vec4 o;
void main() {
    vec4 c = texture(src, v_uv);
    if (gamma != 0) {
        c.r = texture(lut, vec2(c.r, 0.5)).r;
        c.g = texture(lut, vec2(c.g, 0.5)).g;
        c.b = texture(lut, vec2(c.b, 0.5)).b;
    }
    o = vec4(c.rgb, 1.0);
}
)";

class GlesBackend final : public Backend {
  public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void FrameStats(Target& t);
    bool MakeDepthProbe();
    void Sync(uint32_t data_va, uint32_t size) override;
    void SyncLate(uint32_t data_va) override { Sync(data_va, 0); }
    void Flush() override {
        if (Ready()) glFlush();
    }
    void SetGamma(const uint8_t* ramp) override;

    void Report(const char* what);
    void CheckGl(const char* where);  // the first driver error, with the step that caused it
    const std::string& Name() const { return renderer_; }

  private:
    bool Ready();  // the context, made on the first call and brought to the thread that draws
    bool Start(SDL_Window* window);
    bool SelfTest();  // one triangle of this file's own, to tell "GL draws" from "the port's draws come out empty"
    // resources
    Target* FindTarget(uint32_t data, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(Target& t);
    bool UploadTarget(Target& t, const Surface& s);
    Tex* EnsureTexture(const TextureDesc& d, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint32_t* colours);
    void ReleaseTexture(Tex& tx);
    // shaders
    uint64_t Module(const std::string& hlsl, glsl::Stage stage, const char* entry);
    Program* EnsureProgram(uint64_t vs, uint64_t ps);
    // state
    GLuint Bind(const Target* rt, const Target* ds);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    bool Readback(Target& t, std::vector<uint8_t>& px);
    void SetSamplerState(const State& s, int stage, const DrawTextures& stages, GLuint tex);
    void RememberDrawn(Target* rt, Target* ds);
    void Latch(Target& t);  // a target the CPU wrote since the GPU last drew it
    GLuint Feedback(GLuint tex, uint32_t w, uint32_t h);  // a copy of a target a draw reads while it writes it

    uint32_t guest_w_ = 0, guest_h_ = 0;
    float scale_ = 1.0f;
    uint32_t window_w_ = 0, window_h_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // source hash -> module hash
    std::unordered_map<uint64_t, GLuint> modules_;
    std::unordered_map<uint64_t, Program> programs_;
    GLuint vao_ = 0, vbo_ = 0, ibo_ = 0;
    size_t vbo_size_ = 0, ibo_size_ = 0;
    GLuint ubo_[2] = {};
    GLuint present_ = 0, lut_ = 0;
    bool gamma_ = false;
    int ramp_mean_[3] = {};
    int ramp_high_ = 0;
    std::vector<uint8_t> ramp_;
    std::unordered_map<uint64_t, std::vector<glsl::Sampler>> sampler_units_;  // per shader module, until it is linked
    std::unordered_map<uint64_t, GLuint> fbos_;  // a colour and a depth target drawn into together
    Assembled asm_;
    GLint gamma_loc_ = -1;
    bool warned_cube_ = false;
    bool error_seen_ = false;
    GLuint dummy_tex_ = 0;  // stands in for a stage that cannot be read at all
    GLuint present_vao_ = 0;
    GLuint feedback_tex_ = 0, feedback_fbo_ = 0;  // the copy feedback stages read
    uint32_t feedback_w_ = 0, feedback_h_ = 0;
    GLuint scratch_ = 0;   // guest sized copy of a target being read back
    GLuint scratch_fbo_ = 0;
    uint32_t scratch_w_ = 0, scratch_h_ = 0;
    uint32_t draw_fbo_ = 0, draw_rt_tex_ = 0, draw_ds_tex_ = 0;
    SDL_GLContext ctx_ = nullptr;  // made on the thread the draws run on
    bool start_failed_ = false;
    int selftest_ = 0;  // 0 not run, 1 draws a pixel, 2 draws nothing
    std::string renderer_ = "?";   // the driver's name, for the graphics report
    std::string problem_;          // the first thing that went wrong with a shader or the driver, shown on the screen
    uint32_t draws_ = 0, issued_ = 0;
    int32_t last_vp_[4] = {};
    uint32_t drawn_into_ = 0;  // the address of the target the last draw went into
    int32_t drawn_size_[2] = {};
    bool frame_black_ = false;
    GLuint depth_probe_ = 0;        // the full screen pass that shows a depth buffer
    GLuint drawn_ds_tex_ = 0, drawn_ds_fbo_ = 0;  // the depth target the frame's draws went into, and its buffer
    uint32_t drawn_ds_ = 0;
    unsigned depth_lit_ = 0, depth_read_ = 0;  // how many of the depth probe's samples hold geometry
    unsigned scene_draws_ = 0;                // draws that had a depth target
    // a black screen names the piece of state that blacks it out, if the pieces are tried one at a time on the phone:
    // each of these is forced for twenty frames, and the frame that comes out is measured
    int force_ = 0, phase_ = -1;
    unsigned phase_lit_[7] = {};
    static constexpr const char* kPhaseName[7] = {"as drawn",   "no cull",    "front flipped", "no depth test",
                                                  "no blend",   "all channels", "all of it"};
    double asm_ms_ = 0, draw_ms_ = 0, present_ms_ = 0;  // this frame's CPU time, and where it went
    // where the present pass puts the console's picture, and what the drawable underneath actually is: a picture that
    // does not fill the phone is one of these two numbers disagreeing
    int32_t present_vp_[4] = {};
    int32_t drawable_[2] = {};
    bool present_wide_ = false;
    unsigned frame_lit_ = 0;       // how much of the presented target is not black, in percent
    int frame_mean_[3] = {};       // and the average of its colour
    GLuint stats_fbo_ = 0, stats_tex_ = 0;  // a small copy of the target, for that average   // the target this frame presents was read back and had no pixel in it
    bool window_black_ = false;  // and so was the window after the present pass drew it
    double fps_ = 0;
    int fps_n_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t presents_ = 0;
    uint32_t drawn_targets_ = 0, uploaded_textures_ = 0, programs_built_ = 0;
};

bool GlesBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    // the frame is drawn at the console's own size, not at a multiple of it: this renderer's targets then hold exactly
    // the pixels the game's shaders ask for, and the present pass stretches the frame over the phone's screen by itself
    scale_ = 1.0f;
    // the console's vertices are read by the CPU and handed over as float4: one vertex layout, no driver format rules
    g_raw_vertices = false;
    // the port hands its commands to a render thread of its own, and a GL context belongs to the thread that made it:
    // the context is made on whichever thread asks for it first, and every call from then on brings it along
    vkshader::Init(app::ExeDir() / "shadercache");
    // the port hands its commands to a render thread of its own, and an EGL context is current on one thread at a time:
    // the context is made on the thread that first draws, and every call after it brings the context along
    LOGI("gl: %ux%u guest, the context is made where the draws are", width, height);
    return true;
}

bool GlesBackend::Ready() {
    SDL_Window* window = host::Window();
    if (!window) return false;
    if (start_failed_) return false;  // the driver said no once: the log has why, and every frame need not ask again
    if (ctx_) {
        if (SDL_GL_GetCurrentContext() != ctx_ && !SDL_GL_MakeCurrent(window, ctx_)) {
            LOG_FIRST(4, "gl: the context does not follow to this thread: %s", SDL_GetError());
            return false;
        }
        return true;
    }
    if (Start(window)) return true;
    start_failed_ = true;
    return false;
}

bool GlesBackend::Start(SDL_Window* window) {
    ctx_ = SDL_GL_CreateContext(window);
    if (!ctx_) {
        LOGE("gl: no GL context: %s", SDL_GetError());
        return false;
    }
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    renderer_ = renderer ? renderer : "?";
    LOGI("gl: %s, %s", version ? version : "?", renderer_.c_str());
    int major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3) {
        LOGE("gl: GL ES 3.0 is needed, this driver has %d.%d", major, minor);
        return false;
    }
    int win_w = 0, win_h = 0;
    host::WindowPixels(win_w, win_h);
    window_w_ = uint32_t(std::max(win_w, 1));
    window_h_ = uint32_t(std::max(win_h, 1));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glDisable(GL_DITHER);
    glDisable(GL_CULL_FACE);
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenVertexArrays(1, &present_vao_);  // no attribute arrays: the present pass reads nothing but its own vertices
    glGenTextures(1, &dummy_tex_);
    glBindTexture(GL_TEXTURE_2D, dummy_tex_);
    {
        const uint8_t px[4] = {0, 0, 0, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    glGenBuffers(2, ubo_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_[0]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(State::vsc), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_[1]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(DrawConstants), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_[0]);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo_[1]);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    // the frame goes to the window through one full screen pass, gamma ramp included
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kPresentVertex, nullptr);
    glCompileShader(vs);
    GLuint ps = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(ps, 1, &kPresentPixel, nullptr);
    glCompileShader(ps);
    present_ = glCreateProgram();
    glAttachShader(present_, vs);
    glAttachShader(present_, ps);
    glLinkProgram(present_);
    glDeleteShader(vs);
    glDeleteShader(ps);
    GLint linked = 0;
    glGetProgramiv(present_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        glGetProgramInfoLog(present_, sizeof(log), nullptr, log);
        LOGE("gl: the present pass does not link: %s", log);
        return false;
    }
    glUseProgram(present_);
    glUniform1i(glGetUniformLocation(present_, "src"), 0);
    glUniform1i(glGetUniformLocation(present_, "lut"), 1);
    gamma_loc_ = glGetUniformLocation(present_, "gamma");
    glUseProgram(0);
    glGenTextures(1, &lut_);
    glBindTexture(GL_TEXTURE_2D, lut_);
    std::vector<uint8_t> identity(256 * 4);
    for (int i = 0; i < 256; i++) identity[size_t(i) * 4 + 0] = identity[size_t(i) * 4 + 1] = identity[size_t(i) * 4 + 2] =
        uint8_t(i), identity[size_t(i) * 4 + 3] = 255;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, identity.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ramp_.assign(256 * 3, 0);
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        LOGE("gl: the overlay does not start");
        return false;
    }
    MakeDepthProbe();
    selftest_ = SelfTest() ? 1 : 2;
    if (selftest_ == 2 && problem_.empty()) problem_ = "this driver does not draw the renderer's own triangle";
    LOGI("gl: %ux%u window, ready, the renderer's own test %s", window_w_, window_h_,
         selftest_ == 1 ? "draws a pixel" : "draws nothing");
    return true;
}

// One triangle with a shader this file writes itself, into a 4x4 texture: no port shader, no console state, no guest
// memory. A pixel coming back out says GL itself draws here, so an empty frame is the port's draws and not the
// framebuffer, the buffers or the swap.
bool GlesBackend::SelfTest() {
    const char* vs_src = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    const char* fs_src = R"(#version 300 es
precision highp float;
out vec4 o;
void main() { o = vec4(1.0, 0.0, 1.0, 1.0); }
)";
    auto compile = [](GLenum type, const char* text) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &text, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            LOGE("gl: the self test's shader does not compile: %s", log);
            glDeleteShader(sh);
            return GLuint(0);
        }
        return sh;
    };
    GLuint vsh = compile(GL_VERTEX_SHADER, vs_src), fsh = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vsh || !fsh) return false;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsh);
    glAttachShader(prog, fsh);
    glLinkProgram(prog);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOGE("gl: the self test's program does not link: %s", log);
        glDeleteProgram(prog);
        return false;
    }
    GLuint tex = 0, fbo = 0, vao = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool drew = false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glViewport(0, 0, 4, 4);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(1, 1, 1, 1);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        uint8_t px[4] = {0, 0, 0, 0};
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        drew = px[0] == 255 && px[2] == 255;
        if (!drew) LOGE("gl: the self test drew %u,%u,%u,%u", px[0], px[1], px[2], px[3]);
    } else {
        LOGE("gl: the self test's framebuffer is incomplete (%04X)", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    }
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    glBindVertexArray(vao_);
    return drew;
}

// ---- targets and textures

Target* GlesBackend::FindTarget(uint32_t data, bool depth) {
    auto it = targets_.find(uint64_t(data) << 1 | uint64_t(depth));
    return it == targets_.end() ? nullptr : &it->second;
}

void GlesBackend::ReleaseTarget(Target& t) {
    for (auto it = fbos_.begin(); it != fbos_.end();) {
        uint32_t rt = uint32_t(it->first >> 32), ds = uint32_t(it->first);
        if (rt == t.img.tex || ds == t.img.tex) {
            glDeleteFramebuffers(1, &it->second);
            it = fbos_.erase(it);
        } else {
            ++it;
        }
    }
    if (t.img.tex) glDeleteTextures(1, &t.img.tex);
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    t = Target{};
}

Target* GlesBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    uint64_t key = uint64_t(data) << 1 | uint64_t(depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it->second);
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    glGenTextures(1, &t.img.tex);
    glBindTexture(GL_TEXTURE_2D, t.img.tex);
    if (depth) {
        // one depth-stencil texture, so a draw can read the shadow map it fills in an earlier pass
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, GLsizei(t.w), GLsizei(t.h), 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.w), GLsizei(t.h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    t.img.w = t.w;
    t.img.h = t.h;
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    if (depth)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t.img.tex, 0);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.img.tex, 0);
    // a new target starts cleared, as the Vulkan backend's does: a depth texture the driver leaves at zero is a depth
    // buffer where every fragment fails the depth test, and a picture of nothing is what comes out of that
    glDisable(GL_SCISSOR_TEST);
    if (depth) {
        glDepthMask(GL_TRUE);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    } else {
        glColorMask(1, 1, 1, 1);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t.data = data;
    t.bytes = GuestPitch(gw) * gh;  // a console pixel is four bytes, depth and colour alike
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    drawn_targets_++;
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// the CPU wrote the target's guest memory since the GPU last drew it: upload what it wrote
bool GlesBackend::UploadTarget(Target& t, const Surface& s) {
    (void)s;
    if (t.depth) return false;
    if (!t.data || (t.w == t.gw && t.h == t.gh)) {
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
        if (!p) return false;
        uint32_t pitch = GuestPitch(t.gw);
        std::vector<uint8_t> tight(size_t(t.gw) * t.gh * 4);
        for (uint32_t y = 0; y < t.gh; y++) {
            const uint8_t* row = p + size_t(y) * pitch;
            uint8_t* out = &tight[size_t(y) * t.gw * 4];
            for (uint32_t x = 0; x < t.gw; x++, row += 4, out += 4) {
                out[0] = row[2];  // guest BGRA to the RGBA the texture was made with
                out[1] = row[1];
                out[2] = row[0];
                out[3] = row[3];
            }
        }
        glBindTexture(GL_TEXTURE_2D, t.img.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(t.gw), GLsizei(t.gh), GL_RGBA, GL_UNSIGNED_BYTE, tight.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        t.sig = MemSig(t.data, t.bytes);
        t.epoch = DirtyEpoch(t.data);
        t.drawn = false;
        return true;
    }
    return false;  // a scaled target: the guest wrote pixels the GPU has already drawn over, leave the picture alone
}

Tex* GlesBackend::EnsureTexture(const TextureDesc& d, uint64_t key) {
    auto it = textures_.find(key);
    if (it != textures_.end()) {
        Tex& tx = it->second;
        bool same = tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height && tx.d.levels == d.levels &&
                    tx.d.cube == d.cube;
        if (same && tx.epoch == DirtyEpoch(d.data)) return &tx;
        if (!same) ReleaseTexture(tx);
        else return &tx;  // the pixels changed under it: the caller uploads
    }
    Tex tx;
    tx.d = d;
    const FormatInfo& fi = Format(d.format);
    tx.decode = fi.compressed;
    // glTexStorage2D rejects a chain longer than the sizes allow, so the count asked for is capped at what fits
    uint32_t fit = 1;
    for (uint32_t m = std::max(d.width, d.height); m > 1; m >>= 1) fit++;
    tx.levels = std::clamp(d.levels, 1u, fit);
    glGenTextures(1, &tx.img.tex);
    glBindTexture(GL_TEXTURE_2D, tx.img.tex);
    GLsizei w = GLsizei(d.cube ? d.width : d.width), h = GLsizei(d.height);
    glTexStorage2D(GL_TEXTURE_2D, GLsizei(tx.levels), GL_RGBA8, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    tx.img.w = d.width;
    tx.img.h = d.height;
    tx.epoch = ~0ull;  // never matches a write counter, so the caller uploads the pixels this time round
    uploaded_textures_++;
    Tex& ref = textures_[key];
    ref = std::move(tx);
    return &ref;
}

void GlesBackend::ReleaseTexture(Tex& tx) {
    if (tx.img.tex) glDeleteTextures(1, &tx.img.tex);
    tx = Tex{};
}

// the console's texture to RGBA texels: palettes and DXT are unpacked on the CPU, as the Vulkan backend does for every
// phone it runs on
void GlesBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint32_t* colours) {
    const FormatInfo& fi = Format(d.format);
    uint32_t face = 0;
    glBindTexture(GL_TEXTURE_2D, tx.img.tex);
    for (uint32_t l = 0; l < tx.levels; l++) {
        const uint8_t* base = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        if (!base) break;
        std::vector<uint8_t> px, decoded;
        uint32_t pitch = 0;
        DecodeLevel(d, l, face, base, colours, px, pitch);
        uint32_t w = std::max(d.width >> l, 1u), h = std::max(d.height >> l, 1u);
        const uint8_t* src = px.data();
        if (tx.decode) {
            DecodeDxt(fi.host, px, pitch, w, h, 1, decoded);
            src = decoded.data();
            pitch = w * 4;
        }
        // BGRA to RGBA, which is what GL ES takes without asking for an extension
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* in = src + size_t(y) * pitch + size_t(x) * 4;
                uint8_t* out = &rgba[(size_t(y) * w + x) * 4];
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = in[3];
            }
        glTexSubImage2D(GL_TEXTURE_2D, GLint(l), 0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    tx.sig = MemSig(d.data, std::max(1u, TotalSize(d)));
    tx.epoch = DirtyEpoch(d.data);
}

// ---- shaders

uint64_t GlesBackend::Module(const std::string& hlsl, glsl::Stage stage, const char* entry) {
    // the shader cache on disk is keyed by this hash and the Vulkan renderer's entries live in the same folder: the GL
    // renderer's are tagged, so the two can never read each other's modules
    constexpr uint64_t kGlVertexSalt = 0x9A6C2F31D4B75E01ull, kGlPixelSalt = 0x4C1E8B27A9D3F605ull;
    uint64_t seed = Hash(entry, std::strlen(entry), stage == glsl::Stage::Vertex ? kGlVertexSalt : kGlPixelSalt);
    uint64_t hash = Hash(hlsl.data(), hlsl.size(), seed);
    if (modules_.count(hash)) return hash;
    std::string code;
    std::vector<glsl::Sampler> samplers;
    if (!glsl::Compile(hlsl, entry, stage, hash, code, samplers)) return 0;
    GLenum type = stage == glsl::Stage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    GLuint sh = glCreateShader(type);
    const char* text = code.c_str();
    glShaderSource(sh, 1, &text, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        LOGE("gl: %s %s does not compile: %s", stage == glsl::Stage::Vertex ? "vertex" : "pixel", entry, log);
        if (problem_.empty()) {
            problem_ = std::string(stage == glsl::Stage::Vertex ? "vertex" : "pixel") + " shader rejected: " + log;
            if (problem_.size() > 220) problem_.resize(220);
        }
        LOGE("%s", code.c_str());
        glDeleteShader(sh);
        return 0;
    }
    modules_[hash] = sh;
    // the sampler units the program expects, kept beside the program's shader until it is linked
    sampler_units_[hash] = samplers;
    return hash;
}

Program* GlesBackend::EnsureProgram(uint64_t vs, uint64_t ps) {
    uint64_t pair[2] = {vs, ps};
    uint64_t key = Hash(pair, sizeof(pair));
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second.id ? &it->second : nullptr;
    Program p;
    p.id = glCreateProgram();
    glAttachShader(p.id, modules_[vs]);
    glAttachShader(p.id, modules_[ps]);
    glLinkProgram(p.id);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p.id, sizeof(log), nullptr, log);
        LOGE("gl: link failed: %s", log);
        if (problem_.empty()) {
            problem_ = std::string("the two shaders do not link: ") + log;
            if (problem_.size() > 220) problem_.resize(220);
        }
        glDeleteProgram(p.id);
        p.id = 0;
    } else {
        // the constant blocks are bound by name: GL ES 3.00 has no layout(binding) for them
        glUseProgram(p.id);  // a sampler's unit goes through the program, and that needs it bound
        GLuint vc = glGetUniformBlockIndex(p.id, "VC"), dc = glGetUniformBlockIndex(p.id, "DC");
        programs_built_++;
        if (vc != GL_INVALID_INDEX) glUniformBlockBinding(p.id, vc, 0);
        if (dc != GL_INVALID_INDEX) glUniformBlockBinding(p.id, dc, 1);
        if (vc == GL_INVALID_INDEX || dc == GL_INVALID_INDEX)
            LOG_FIRST(2, "gl: a program names its constant blocks differently (VC %s, DC %s)",
                      vc == GL_INVALID_INDEX ? "missing" : "found", dc == GL_INVALID_INDEX ? "missing" : "found");
        for (const auto& list : {sampler_units_[vs], sampler_units_[ps]})
            for (const glsl::Sampler& s : list) {
                GLint at = glGetUniformLocation(p.id, s.name.c_str());
                if (at >= 0) glUniform1i(at, GLint(s.unit));
                bool known = false;
                for (const glsl::Sampler& have : p.samplers) known = known || have.name == s.name;
                if (!known) p.samplers.push_back(s);
            }
        glUseProgram(0);
        programs_built_++;
    }
    Program& ref = programs_[key];
    ref = std::move(p);
    return ref.id ? &ref : nullptr;
}


// ---- drawing state

GLuint GlesBackend::Bind(const Target* rt, const Target* ds) {
    uint32_t rt_tex = rt ? rt->img.tex : 0, ds_tex = ds ? ds->img.tex : 0;
    uint64_t key = uint64_t(rt_tex) << 32 | uint64_t(ds_tex);
    auto it = fbos_.find(key);
    if (it != fbos_.end()) {
        glBindFramebuffer(GL_FRAMEBUFFER, it->second);
        return it->second;
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (rt) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_tex, 0);
    if (ds) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds_tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) LOGE("gl: framebuffer incomplete (%04X)", status);
    fbos_[key] = fbo;
    return fbo;
}

void GlesBackend::RememberDrawn(Target* rt, Target* ds) {
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
}

// the stage's texture, its sampler state applied to the texture object (GL ES keeps filtering and wrapping there) and
// the stage's place in the draw's constant block
bool GlesBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    // the targets this draw writes into: a stage that names one of them cannot be read while the draw runs
    uint32_t rt_tex = 0, ds_tex = 0;
    if (Target* rt = FindTarget(mem::VirtualFromPhysical(s.rt.data), false)) rt_tex = rt->img.tex;
    if (Target* ds = FindTarget(mem::VirtualFromPhysical(s.zs.data), true)) ds_tex = ds->img.tex;
    for (int i = 0; i < 4; i++) {
        stages.tex[i] = 0;
        stages.info[i] = TexInfo{};
        stages.levels[i] = 1;
        uint32_t va = s.textures[size_t(i)];
        if (!va) continue;
        TextureDesc d;
        if (!Describe(va, d)) continue;
        const FormatInfo& fi = Format(d.format);
        uint32_t data = mem::VirtualFromPhysical(d.data);
        GLuint tex = 0;
        uint32_t levels = 1;
        Target* as_target = FindTarget(data, fi.depth != 0);
        if (as_target) {
            // the pixels a stage shows are the GPU's when the game drew into them, and the guest's when it wrote them
            Latch(*as_target);
            tex = as_target->img.tex;
            if (!as_target->depth) {
                stages.rt[i] = true;
                stages.gw[i] = as_target->gw;
                stages.k[i] = float(as_target->w) / float(as_target->gw);
            }
        } else {
            StagePalette palette;
            ReadStagePalette(s, i, d, palette);
            uint64_t key = Hash(&d.data, sizeof(d.data));
            uint64_t more[4] = {d.format, d.width, d.height, d.levels * 2 + (d.cube ? 1 : 0)};
            key = Hash(more, sizeof(more), key);
            Tex* tx = EnsureTexture(d, key);
            if (!tx) continue;
            if (tx->epoch != DirtyEpoch(d.data)) UploadTexture(*tx, d, palette.va ? palette.colours : nullptr);
            tex = tx->img.tex;
            levels = tx->levels;
            if (d.cube || d.depth > 1) {
                // GL ES has cube maps and volumes, but this backend has only been written for 2D so far: the stage is
                // read from its first level as a flat texture, which is enough for the port to run and is logged once
                if (!warned_cube_) {
                    warned_cube_ = true;
                    LOGI("gl: %08X is %s, read flat by this renderer", d.data, d.cube ? "a cube map" : "a volume texture");
                }
            }
        }
        if (fi.depth && !as_target) {
            tex = dummy_tex_;
            levels = 1;
        }
        if (fi.depth && as_target) stages.info[i].shadow = ShadowDepthKind(d.format);
        // a render target is read where the console's own pixel offset puts it, exactly as the Vulkan backend marks it
        if (as_target) stages.info[i].flags |= kTexTarget;
        stages.info[i].kind = kTex2D;
        stages.levels[i] = levels;
        // a colour target sampled while the draw writes into it: GL leaves that undefined, so it is copied out first.
        // The depth buffer is different: a copy of it is not a depth texture any more, so the stage reads nothing, and
        // that is also what the Vulkan backend does with it
        if (tex && tex == ds_tex) {
            tex = dummy_tex_;
            levels = 1;
            stages.levels[i] = 1;
            stages.info[i].shadow = 0;
        } else if (tex && tex == rt_tex && as_target) {
            tex = Feedback(tex, as_target->w, as_target->h);
        }
        if (!tex) tex = dummy_tex_;
        glActiveTexture(GLenum(GL_TEXTURE0 + i));
        glBindTexture(GL_TEXTURE_2D, tex);
        stages.tex[i] = tex;
        // sampler state: GL ES 3.00 has one object per unit, so the texture carries these
        const auto& t = s.tss[size_t(i)];
        bool shadow = stages.info[i].shadow != 0;
        bool linearable = fi.host != Host::R32F && fi.host != Host::R16Unorm;
        if (shadow) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, Compare(s.rs[kRsShadowFunc]));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GLenum(MinFilter(t[kTssMinFilter], t[kTssMipFilter], levels > 1, linearable)));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            t[kTssMagFilter] >= kTexfLinear && linearable ? GL_LINEAR : GL_NEAREST);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLenum(Wrap(t[kTssAddressU])));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLenum(Wrap(t[kTssAddressV])));
    }
    return true;
}

// the game wrote into the target's memory from the CPU since the GPU last drew it: the GPU's copy is the stale one
void GlesBackend::Latch(Target& t) {
    if (t.depth || !t.data) return;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig) return;
    t.epoch = DirtyEpoch(t.data);
    t.sig = sig;
    Surface su;
    su.data = t.data;
    UploadTarget(t, su);
}

// a stage that names the target the draw writes into: GL leaves that read undefined, the Vulkan backend keeps a copy,
// and so does this one, taken from the target before the draw
GLuint GlesBackend::Feedback(GLuint tex, uint32_t w, uint32_t h) {
    if (!w || !h) return tex;
    if (feedback_tex_ && (feedback_w_ != w || feedback_h_ != h)) {
        glDeleteTextures(1, &feedback_tex_);
        glDeleteFramebuffers(1, &feedback_fbo_);
        feedback_tex_ = 0;
        feedback_fbo_ = 0;
    }
    if (!feedback_tex_) {
        glGenTextures(1, &feedback_tex_);
        glBindTexture(GL_TEXTURE_2D, feedback_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &feedback_fbo_);
        feedback_w_ = w;
        feedback_h_ = h;
    }
    Target* src = nullptr;
    for (auto& [key, t] : targets_)
        if (t.img.tex == tex) src = &t;
    if (!src) return tex;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, feedback_fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, feedback_tex_, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
    glBlitFramebuffer(0, 0, GLint(w), GLint(h), 0, 0, GLint(w), GLint(h), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return feedback_tex_;
}

// ---- what the game asks the renderer to do

void GlesBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                        uint32_t stencil) {
    if (!Ready()) return;
    Target* rt = s.rt.header ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = s.zs.header ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;
    Bind(rt, ds);
    Target& t = rt ? *rt : *ds;
    float c[4];
    Color(color, c);
    glClearColor(c[0], c[1], c[2], c[3]);
    glClearDepthf(z);
    glClearStencil(GLint(stencil));
    GLbitfield bits = 0;
    if (flags & kClearTarget) bits |= GL_COLOR_BUFFER_BIT;
    if (flags & kClearZ) bits |= GL_DEPTH_BUFFER_BIT;
    if (flags & kClearStencil) bits |= GL_STENCIL_BUFFER_BIT;
    // the console's rectangles are in console pixels, the target may be bigger
    float sc = float(t.w) / float(t.gw ? t.gw : t.w);
    uint32_t n = count && rects ? count : 1;
    uint32_t saved[4] = {32, 32, 32, 32};
    if (count && rects) {
        glGetIntegerv(GL_SCISSOR_BOX, reinterpret_cast<GLint*>(saved));
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, GLsizei(t.w), GLsizei(t.h));
        glClear(bits);
        for (uint32_t i = 0; i < n; i++) {
            GuestRect r = ReadGuestRect(uint32_t(reinterpret_cast<uintptr_t>(rects)), i);
            int32_t x0 = int32_t(std::lround(r.left * sc)), y0 = int32_t(std::lround(r.top * sc));
            int32_t x1 = int32_t(std::lround(r.right * sc)), y1 = int32_t(std::lround(r.bottom * sc));
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor(x0, GLint(t.h) - y1, x1 - x0, y1 - y0);
            glClear(bits);
        }
        glScissor(GLint(saved[0]), GLint(saved[1]), GLsizei(saved[2]), GLsizei(saved[3]));
        glDisable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
        glClear(bits);
    }
    RememberDrawn(rt, ds);
    CheckGl("the clear");
}

void GlesBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!Ready()) return;
    draws_++;
    auto draw_t0 = std::chrono::steady_clock::now();
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    auto draw_t1 = std::chrono::steady_clock::now();
    asm_ms_ += std::chrono::duration<double, std::milli>(draw_t1 - draw_t0).count();
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt) Latch(*rt);  // the game may have put the frame together in memory since the last draw
    Target& screen = rt ? *rt : *ds;
    GuestExtent size;
    SurfaceSize(rt ? s.rt : s.zs, size.w, size.h);

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    std::string vs = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (sh.quad) vs = QuadHlsl(std::move(vs));
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(vs);
        if (!points.empty()) {
            vs = std::move(points);
            entry = "main_points";
        }
    }
    sh.vh = Module(vs, glsl::Stage::Vertex, entry);
    DrawTextures stages;
    if (!BindStageTextures(s, stages)) return;
    CheckGl("the stage textures");
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    sh.ph = Module(PixelHlsl(pk), glsl::Stage::Pixel, "main");
    if (!sh.vh || !sh.ph) return;
    Program* prog = EnsureProgram(sh.vh, sh.ph);
    if (!prog) return;
    CheckGl("the shader program");

    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    if (a.topology == Topology::Points) c.surf[3] = float(screen.w) / float(screen.gw ? screen.gw : screen.w);
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    // a render target read by the console's own vertex path sits half a host texel off, as in the Vulkan backend
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    float target_shift = offset ? -0.5f * (float(screen.w) / float(screen.gw ? screen.gw : screen.w)) : 0.0f;
    FillTextureConstants(s, sh, target_shift, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    c.zparams[3] = 0;
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_[0]);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(s.vsc), s.vsc.data());
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_[1]);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(c), &c);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // the console's vertices arrive as float4 registers packed one after another, which is one attribute each
    uint32_t stride = 0, regs = 0;
    uint32_t locations[16] = {}, offsets[16] = {};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        locations[regs] = r;
        offsets[regs] = stride;
        stride += 16;
        regs++;
    }
    if (!regs || a.vertex_count == 0) return;
    size_t vbytes = size_t(a.vertex_count) * stride;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (vbytes > vbo_size_) {
        vbo_size_ = std::max(vbytes, size_t(1) << 16);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vbo_size_), nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vbytes), a.vertices.data());
    for (uint32_t i = 0; i < regs; i++) {
        glEnableVertexAttribArray(locations[i]);
        glVertexAttribPointer(locations[i], 4, GL_FLOAT, GL_FALSE, GLsizei(stride),
                              reinterpret_cast<const void*>(uintptr_t(offsets[i])));
    }
    // a draw that starts at the title's own first vertex has that vertex taken off its indices: GL ES has no base
    // vertex offset, and a negative one in the attributes would be no offset at all
    std::vector<uint32_t> rebased;
    const void* index_data = a.index_src;
    bool index16 = a.index16;
    if (a.index_count && a.base_vertex) {
        rebased.resize(a.index_count);
        int32_t base = a.base_vertex;
        if (a.index16) {
            const uint16_t* src = static_cast<const uint16_t*>(a.index_src);
            for (uint32_t i = 0; i < a.index_count; i++) rebased[i] = uint32_t(int32_t(src[i]) + base);
        } else {
            const uint32_t* src = static_cast<const uint32_t*>(a.index_src);
            for (uint32_t i = 0; i < a.index_count; i++) rebased[i] = uint32_t(int32_t(src[i]) + base);
        }
        index_data = rebased.data();
        index16 = false;
    }
    uint32_t drawn = a.index_count ? a.index_count : a.draw_count;
    GLenum mode = a.topology == Topology::Points ? GL_POINTS
                  : a.topology == Topology::Lines ? (a.strip ? GL_LINE_STRIP : GL_LINES)
                                                  : (a.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES);

    Bind(rt, ds);
    RememberDrawn(rt, ds);
    glUseProgram(prog->id);
    // the console counts the viewport's pixels from the top of its target and GL counts them from the bottom of the
    // window: y is turned over for both the viewport and the scissor, the x edges are the same
    float sc = float(screen.w) / float(size.w);
    glViewport(0, 0, GLsizei(screen.w), GLsizei(screen.h));
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp(x0, 0, int32_t(screen.w));
    y0 = std::clamp(y0, 0, int32_t(screen.h));
    x1 = std::clamp(x1, 0, int32_t(screen.w));
    y1 = std::clamp(y1, 0, int32_t(screen.h));
    if (x1 <= x0 || y1 <= y0) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);

    // blending: the same factors on the alpha channel as on the colour, which is what the Vulkan backend binds
    bool blend = SimpleState(s, kMBlendEnable) != 0;
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GLenum(Blend(SimpleState(s, kMSrcBlend), false)), GLenum(Blend(SimpleState(s, kMDestBlend), false)));
        glBlendEquation(GLenum(BlendOp(SimpleState(s, kMBlendOp))));
    } else {
        glDisable(GL_BLEND);
    }
    // which channels the draw may write: the console's four bits, in the order Vulkan's colour components take them
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    uint32_t wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    glColorMask(wmask & 1, ( wmask >> 1) & 1, (wmask >> 2) & 1, (wmask >> 3) & 1);
    // the console's depth bias on the polygons it asks it for: GL's units are a depth buffer step, the console's are
    // 24 bit fractions of the range, and this renderer's depth range is the console's own
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        float units = Bits(SimpleState(s, kMDepthOffset)) * float(kDepth24Max);
        float slope = Bits(SimpleState(s, kMSlopeScale));
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, units);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    // depth and stencil
    if (s.rs[kRsZEnable]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(Compare(SimpleState(s, kMZFunc)));
        glDepthMask(SimpleState(s, kMZWriteEnable) ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (s.rs[kRsStencilEnable]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(Compare(SimpleState(s, kMStencilFunc)), GLint(SimpleState(s, kMStencilRef) & 0xFF),
                      GLuint(SimpleState(s, kMStencilMask) & 0xFF));
        glStencilMask(GLuint(SimpleState(s, kMStencilWriteMask) & 0xFF));
        glStencilOp(GLenum(StencilOp(s.rs[kRsStencilFail])), GLenum(StencilOp(SimpleState(s, kMStencilZFail))),
                    GLenum(StencilOp(SimpleState(s, kMStencilPass))));
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    // culling: the console's winding as Vulkan's KeyRasterizer reads it
    uint32_t cull = s.rs[kRsCullMode];
    GLenum front = cull == 0x900 ? GL_CCW : GL_CW;
    if (cull && a.topology == Topology::Triangles) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(front);
    } else {
        glDisable(GL_CULL_FACE);
    }
    // the panel's experiments: one piece of this draw's state forced for twenty frames at a time, so that a frame of
    // nothing says which piece it is on the phone instead of waiting for another build
    switch (force_) {
        case 1: glDisable(GL_CULL_FACE); break;
        case 2:
            if (a.topology == Topology::Triangles) {
                glEnable(GL_CULL_FACE);
                glFrontFace(front == GL_CCW ? GL_CW : GL_CCW);
            }
            break;
        case 3: glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); break;
        case 4: glDisable(GL_BLEND); break;
        case 5: glColorMask(1, 1, 1, 1); break;
        case 6:
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_BLEND);
            glColorMask(1, 1, 1, 1);
            break;
        default: break;
    }
    issued_++;
    last_vp_[0] = x0;
    last_vp_[1] = y0;
    last_vp_[2] = x1 - x0;
    last_vp_[3] = y1 - y0;
    if (ds) {
        drawn_ds_tex_ = ds->img.tex;
        drawn_ds_fbo_ = ds->fbo;
        drawn_ds_ = ds->data;
        scene_draws_++;
    }
    drawn_into_ = screen.data;
    drawn_size_[0] = int32_t(screen.w);
    drawn_size_[1] = int32_t(screen.h);
    if (a.index_count) {
        size_t ibytes = size_t(a.index_count) * (index16 ? 2 : 4);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        if (ibytes > ibo_size_) {
            ibo_size_ = std::max(ibytes, size_t(1) << 16);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(ibo_size_), nullptr, GL_DYNAMIC_DRAW);
        }
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, GLsizeiptr(ibytes), index_data);
        glDrawElements(mode, GLsizei(drawn), index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(mode, 0, GLsizei(drawn));
    }
    CheckGl("the draw call");
    draw_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_t1).count();
    (void)prim;
    (void)count;
}

void GlesBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    if (!Ready()) return;
    Target* from = FindTarget(mem::VirtualFromPhysical(src.data), false);
    if (!from || !from->drawn) {
        // pixels the CPU made: the copy happens in memory, as the Vulkan backend does
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) LOG_FIRST(8, "gl: CopyRects skipped");
        return;
    }
    Target* to = EnsureTarget(dst, false);
    if (!to || to == from) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to->fbo);
    uint32_t n = count && src_rects ? count : 1;
    float sc = float(to->w) / float(to->gw ? to->gw : to->w);
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = 0, t = 0, r = int32_t(std::min(from->w, to->w)), b = int32_t(std::min(from->h, to->h));
        int32_t dx = 0, dy = 0;
        if (count && src_rects) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = int32_t(ScaleUnsigned(g.left, sc));
            t = int32_t(ScaleUnsigned(g.top, sc));
            r = int32_t(std::min(ScaleUnsigned(g.right, sc), from->w));
            b = int32_t(std::min(ScaleUnsigned(g.bottom, sc), from->h));
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px = 0, py = 0;
                ReadGuestPoint(dst_points, i, px, py);
                dx = int32_t(ScaleUnsigned(px, sc));
                dy = int32_t(ScaleUnsigned(py, sc));
            }
        }
        if (r <= l || b <= t || dx >= int32_t(to->w) || dy >= int32_t(to->h)) continue;
        r = std::min(r, l + int32_t(to->w) - dx);
        b = std::min(b, t + int32_t(to->h) - dy);
        int32_t fh = int32_t(from->h), th = int32_t(to->h), hgt = b - t;
        glBlitFramebuffer(l, fh - b, r, fh - t, dx, th - dy - hgt, dx + (r - l), th - dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    to->drawn = true;
    CheckGl("the copy");
}

// ---- present and read-back

// the full screen pass that turns a depth buffer into a picture, for FrameStats
bool GlesBackend::MakeDepthProbe() {
    auto compile = [](GLenum type, const char* text) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &text, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            LOGE("gl: the depth probe's shader does not compile: %s", log);
            glDeleteShader(sh);
            return GLuint(0);
        }
        return sh;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kPresentVertex), ps = compile(GL_FRAGMENT_SHADER, kDepthProbePixel);
    if (!vs || !ps) return false;
    depth_probe_ = glCreateProgram();
    glAttachShader(depth_probe_, vs);
    glAttachShader(depth_probe_, ps);
    glLinkProgram(depth_probe_);
    glDeleteShader(vs);
    glDeleteShader(ps);
    GLint linked = 0;
    glGetProgramiv(depth_probe_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(depth_probe_, sizeof(log), nullptr, log);
        LOGE("gl: the depth probe does not link: %s", log);
        glDeleteProgram(depth_probe_);
        depth_probe_ = 0;
        return false;
    }
    glUseProgram(depth_probe_);
    glUniform1i(glGetUniformLocation(depth_probe_, "src"), 0);
    glUseProgram(0);
    return true;
}

// How much of the frame is not black, and how bright it is on average: an eight of the target through the blitter, so
// a black screen can be told from a dark picture without reading a megabyte off the phone every frame
void GlesBackend::FrameStats(Target& t) {
    constexpr uint32_t kW = 80, kH = 60;
    if (!stats_fbo_) {
        glGenTextures(1, &stats_tex_);
        glBindTexture(GL_TEXTURE_2D, stats_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(kW), GLsizei(kH), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &stats_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, stats_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stats_tex_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, stats_fbo_);
    glBlitFramebuffer(0, 0, GLint(t.w), GLint(t.h), 0, 0, GLint(kW), GLint(kH), GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, stats_fbo_);
    static uint8_t px[kW * kH * 4];
    glReadPixels(0, 0, GLint(kW), GLint(kH), GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    unsigned lit = 0;
    uint64_t sum[3] = {0, 0, 0};
    for (uint32_t i = 0; i < kW * kH; i++) {
        const uint8_t* q = px + size_t(i) * 4;
        if (q[0] > 8 || q[1] > 8 || q[2] > 8) lit++;
        for (int c = 0; c < 3; c++) sum[c] += q[c];
    }
    frame_lit_ = lit * 100 / (kW * kH);
    for (int c = 0; c < 3; c++) frame_mean_[c] = int(sum[c] / (kW * kH));
    frame_black_ = lit == 0;
    // and the depth buffer of the same frame, drawn as a picture: geometry that never rasterised leaves no mark there
    depth_lit_ = depth_read_ = 0;
    if (depth_probe_ && drawn_ds_tex_) {
        static uint8_t dpx[kW * kH * 4];
        glBindFramebuffer(GL_FRAMEBUFFER, stats_fbo_);
        glViewport(0, 0, GLsizei(kW), GLsizei(kH));
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(1, 1, 1, 1);
        glUseProgram(depth_probe_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, drawn_ds_tex_);
        glBindVertexArray(present_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, GLint(kW), GLint(kH), GL_RGBA, GL_UNSIGNED_BYTE, dpx);
        unsigned deep = 0;
        for (uint32_t i = 0; i < kW * kH; i++)
            if (dpx[size_t(i) * 4] < 250) deep++;  // 1.0 is 255: anything under it had something in front of it
        depth_lit_ = deep;
        depth_read_ = kW * kH;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void GlesBackend::Present(const Surface& front) {
    if (!Ready()) return;
    auto present_t0 = std::chrono::steady_clock::now();
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = double(fps_n_) / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
    Target* t = EnsureTarget(front, false);
    // the experiments run on their own, one piece of state every twenty frames: the frame each one produces is
    // measured when it ends, and the panel holds the results side by side
    int phase = presents_ < 7 * 20 ? int((presents_ / 20) % 7) : 0;
    if (phase != phase_) {
        if (phase_ >= 0 && t) {
            FrameStats(*t);
            phase_lit_[phase_] = frame_lit_;
        }
        phase_ = phase;
        force_ = phase;
    }
    if (t) Latch(*t);  // the frame may have been put together by the CPU, or copied there by the game
    int win_w = 0, win_h = 0;
    host::WindowPixels(win_w, win_h);
    if (win_w > 0 && win_h > 0) {
        window_w_ = uint32_t(win_w);
        window_h_ = uint32_t(win_h);
    }
    {   // the drawable's own size, as the last frame's overlay left the viewport: the phone's screen in pixels
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        drawable_[0] = vp[2];
        drawable_[1] = vp[3];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // this frame's vertices and indices get storage of their own: a driver handed the same buffer every frame has to
    // wait for the last one to finish reading it, and orphaning it here is what keeps the frame from stalling on that
    if (vbo_size_) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vbo_size_), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    if (ibo_size_) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(ibo_size_), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    // the console's picture on the phone's screen: edge to edge when the game is widened for the wider screen, and its
    // own 4:3 with black bars beside it when it is not, the same rule the Vulkan renderer's present pass keeps
    float w = float(std::max(window_w_, 1u)), h = float(std::max(window_h_, 1u));
    float vx = 0, vy = 0, vw = w, vh = h;
    if (!shadow::g_widescreen && guest_w_ && guest_h_) {
        float aspect = float(guest_w_) / float(guest_h_);
        vw = h * aspect;
        vh = h;
        if (vw > w) {
            vw = w;
            vh = w / aspect;
        }
        vx = (w - vw) * 0.5f;
        vy = (h - vh) * 0.5f;
    }
    glViewport(GLint(vx), GLint(vy), GLsizei(std::max(vw, 1.0f)), GLsizei(std::max(vh, 1.0f)));
    present_vp_[0] = int32_t(vx);
    present_vp_[1] = int32_t(vy);
    present_vp_[2] = int32_t(std::max(vw, 1.0f));
    present_vp_[3] = int32_t(std::max(vh, 1.0f));
    present_wide_ = shadow::g_widescreen;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_FALSE);
    glColorMask(1, 1, 1, 1);  // the overlay draws with whatever the frame's last draw left, unless it is put back
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (t) {
        if (gamma_ && !ramp_.empty()) {
            std::vector<uint8_t> px(256 * 4);
            for (int i = 0; i < 256; i++) {
                px[size_t(i) * 4 + 0] = ramp_[size_t(i)];
                px[size_t(i) * 4 + 1] = ramp_[256 + size_t(i)];
                px[size_t(i) * 4 + 2] = ramp_[512 + size_t(i)];
                px[size_t(i) * 4 + 3] = 255;
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, lut_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        glUseProgram(present_);
        if (gamma_loc_ >= 0) glUniform1i(gamma_loc_, gamma_ ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t->img.tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lut_);
        glBindVertexArray(present_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    CheckGl("the present pass");
    if (t && presents_ % 120 == 0) {
        // is the picture we are showing actually empty? how much of the same target is not black answers it, and a
        // black screen with nothing else to go on is exactly the question this renderer has been asked
        FrameStats(*t);
        // and the window itself: this reads black on drivers that do not answer for the window surface, so it says
        // "the present pass drew nothing" only when the target holds a picture
        bool window_any = false;
        for (int gy = 1; gy < 4 && !window_any; gy++)
            for (int gx = 1; gx < 4 && !window_any; gx++) {
                uint8_t px[4] = {0, 0, 0, 0};
                glReadPixels(GLint(uint32_t(gx) * window_w_ / 4), GLint(uint32_t(gy) * window_h_ / 4), 1, 1, GL_RGBA,
                             GL_UNSIGNED_BYTE, px);
                window_any = px[0] | px[1] | px[2];
            }
        window_black_ = !window_any;
        if (frame_black_ || window_black_)
            LOG_FIRST(3, "gl: black frame (%s%s) -- %u draws, %u issued, target %08X %ux%u, last drawn %08X %dx%d",
                      frame_black_ ? "target empty" : "target has pixels", window_black_ ? ", window empty" : "",
                      draws_, issued_, t->data, t->w, t->h, drawn_into_, drawn_size_[0], drawn_size_[1]);
    }
    {
        std::lock_guard lock(app::UiLock());
        ImGui_ImplOpenGL3_NewFrame();
        ui::NewFrame(float(window_w_), float(window_h_));
        ImGui::NewFrame();
        if (app::kGame.ui) app::kGame.ui(fps_);
        touch::Draw();
        // a picture that is not there has one of two causes, and both of them are written down on the screen instead of
        // in a log the player would have to find: a shader the driver rejected, or a frame nothing was drawn into
        std::string banner = problem_;
        if (banner.empty() && draws_ == 0 && presents_ > 60) banner = "no draw has reached the screen yet";
        if (banner.empty() && frame_black_ && depth_read_ && depth_lit_ == 0 && presents_ > 120)
            banner = "nothing the game draws lands in its frame: no colour and no depth";
        if (banner.empty() && frame_black_ && depth_lit_ > 0 && presents_ > 120)
            banner = "the frame has geometry but no colour: the pixel path writes black";
        if (banner.empty() && frame_black_ && presents_ > 120) banner = "the frame the game draws holds no pixel";
        // a driver that does not answer for the window surface reports it empty whatever is on the screen, so this is
        // only worth saying when the target has a picture to show and the screen still has none
        if (banner.empty() && !frame_black_ && window_black_ && presents_ > 120)
            banner = "the frame has a picture, the window has none: the present pass draws nothing";
        if (!banner.empty()) {
            ImGui::SetNextWindowPos(ImVec2(12, 96), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.75f);
            ImGui::Begin("gl-diagnostic", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextUnformatted("GL ES RENDERER");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", banner.c_str());
            ImGui::PopStyleColor();
            ImGui::Text("%llu frames, %u draws, %u issued, %u programs, %u textures", (unsigned long long)presents_,
                        draws_, issued_, programs_built_, unsigned(textures_.size()));
            if (issued_) ImGui::Text("scissor %d,%d %dx%d, drawn into %08X %dx%d", last_vp_[0], last_vp_[1],
                                     last_vp_[2], last_vp_[3], drawn_into_, drawn_size_[0], drawn_size_[1]);
            if (t)
                ImGui::Text("showing %08X %ux%u%s", t->data, t->w, t->h, t->data == drawn_into_ ? " (drawn)" : "");
            ImGui::Text("window %ux%u, frame %u%% lit, mean %d,%d,%d", window_w_, window_h_, frame_lit_, frame_mean_[0],
                        frame_mean_[1], frame_mean_[2]);
            // a gamma ramp the game set is applied on the way to the screen, so a ramp of zeros is a black screen with
            // a full frame behind it: the ends of it are worth having on the screen too
            ImGui::Text("tried: %s %u%%, %s %u%%, %s %u%%, %s %u%%, %s %u%%, %s %u%%, %s %u%%", kPhaseName[0],
                        phase_lit_[0], kPhaseName[1], phase_lit_[1], kPhaseName[2], phase_lit_[2], kPhaseName[3],
                        phase_lit_[3], kPhaseName[4], phase_lit_[4], kPhaseName[5], phase_lit_[5], kPhaseName[6],
                        phase_lit_[6]);
            if (depth_read_)
                ImGui::Text("depth %u/%u samples hold geometry, %u draws into target %08X", depth_lit_, depth_read_,
                            scene_draws_, drawn_ds_);
            ImGui::Text("gamma %s, ramp means %d %d %d, high %d", gamma_ ? "on" : "off", ramp_mean_[0], ramp_mean_[1],
                        ramp_mean_[2], ramp_high_);
            ImGui::Text("build %s, cpu draw %.1f ms (vertices %.1f), present %.1f ms", kGlBuild, draw_ms_, asm_ms_,
                        present_ms_);
            ImGui::Text("present %dx%d at %d,%d, widescreen %s, drawable %dx%d", present_vp_[2], present_vp_[3],
                        present_vp_[0], present_vp_[1], present_wide_ ? "on" : "off", drawable_[0], drawable_[1]);
            ImGui::Text("self test: %s", selftest_ == 1 ? "drawing works" : selftest_ == 2 ? "drawing fails" : "not run");
            // the frame itself, drawn by the overlay: what the game put in the target is then in the screenshot
            // whether or not the present pass gets it to the screen
            if (false) {
                ImGui::Text("the frame the game draws:");
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
                ImGui::Image(ImTextureRef(ImTextureID(t->img.tex)), ImVec2(256.0f, 192.0f));
#else
                ImGui::Image(ImTextureID(t->img.tex), ImVec2(256.0f, 192.0f));
#endif
            }
            ImGui::End();
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    SDL_GL_SwapWindow(host::Window());
    presents_++;
    // the frame's own numbers for the next panel, and the counters start again: the draws of the frame after this one
    // are the ones they will add up
    present_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_t0).count();
    draw_ms_ = asm_ms_ = 0;
}

bool GlesBackend::Readback(Target& t, std::vector<uint8_t>& px) {
    uint32_t w = t.gw, h = t.gh;
    GLuint fbo = t.fbo;
    if (t.w != w || t.h != h) {
        // a target drawn at the render scale: the game wants it at the console's size
        if (!scratch_fbo_) {
            glGenTextures(1, &scratch_);
            glGenFramebuffers(1, &scratch_fbo_);
        }
        if (scratch_w_ < w || scratch_h_ < h) {
            scratch_w_ = std::max(scratch_w_, w);
            scratch_h_ = std::max(scratch_h_, h);
            glBindTexture(GL_TEXTURE_2D, scratch_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(scratch_w_), GLsizei(scratch_h_), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scratch_fbo_);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratch_, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glBlitFramebuffer(0, 0, GLint(t.w), GLint(t.h), 0, 0, GLint(w), GLint(h), GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fbo = scratch_fbo_;
    }
    px.assign(size_t(w) * h * 4, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

void GlesBackend::Sync(uint32_t data, uint32_t) {
    if (!Ready()) return;
    uint32_t va = mem::VirtualFromPhysical(data);
    Target* t = FindTarget(va, false);
    if (!t || !t->drawn) return;
    glFinish();
    std::vector<uint8_t> px;
    if (!Readback(*t, px)) return;
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(t->data));
    if (!guest) return;
    uint32_t pitch = GuestPitch(t->gw);
    for (uint32_t y = 0; y < t->gh; y++) {
        const uint8_t* in = &px[size_t(y) * t->gw * 4];
        uint8_t* out = guest + size_t(y) * pitch;
        for (uint32_t x = 0; x < t->gw; x++) {
            out[size_t(x) * 4 + 0] = in[size_t(x) * 4 + 2];
            out[size_t(x) * 4 + 1] = in[size_t(x) * 4 + 1];
            out[size_t(x) * 4 + 2] = in[size_t(x) * 4 + 0];
            out[size_t(x) * 4 + 3] = in[size_t(x) * 4 + 3];
        }
    }
    CheckGl("the read-back");
    t->read = std::move(px);
    t->drawn = false;
    t->sig = MemSig(t->data, t->bytes);
    t->epoch = DirtyEpoch(t->data);
}

void GlesBackend::SetGamma(const uint8_t* ramp) {
    ramp_.assign(ramp, ramp + 256 * 3);
    bool identity = true;
    for (int i = 0; i < 256 && identity; i++) identity = ramp[i] == i && ramp[256 + i] == i && ramp[512 + i] == i;
    // a ramp that maps a channel to black everywhere is a black screen with a full frame behind it, and the console
    // reads the ramp it wants; a title hands over one of zeros while it fades, and that is not a picture to show
    bool usable = true;
    ramp_high_ = 0;
    for (int c = 0; c < 3; c++) {
        long sum = 0;
        int high = 0;
        for (int i = 0; i < 256; i++) {
            sum += ramp[c * 256 + i];
            high = std::max(high, int(ramp[c * 256 + i]));
        }
        ramp_mean_[c] = int(sum / 256);
        ramp_high_ = std::max(ramp_high_, high);
        if (sum / 256 < 8) usable = false;  // this channel comes out black
    }
    gamma_ = !identity && usable;
    LOG_FIRST(2, "gl: gamma ramp %s (mean %d %d %d, high %d)", gamma_ ? "applied" : identity ? "identity" : "black, ignored",
              ramp_mean_[0], ramp_mean_[1], ramp_mean_[2], ramp_high_);
}

// a black screen with one error code in the log says nothing; the step it came from says everything
void GlesBackend::CheckGl(const char* where) {
    if (error_seen_) return;
    // glGetError makes a mobile driver wait for the frame in flight, and this renderer asks after every stage of every
    // draw: the checks earn their place while it starts up, and then they are what the frame rate would go to
    if (presents_ > 200) return;
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    error_seen_ = true;
    char text[160];
    std::snprintf(text, sizeof(text), "GL error 0x%04X after %s", e, where);
    LOGE("gl: %s", text);
    if (problem_.empty()) problem_ = text;
}

void GlesBackend::Report(const char* what) {
    LOGI("gl: %s -- %llu presents, %u targets, %u texture uploads, %u programs built, %zd programs, %zd textures", what,
         (unsigned long long)presents_, drawn_targets_, uploaded_textures_, programs_built_, programs_.size(),
         textures_.size());
}

GlesBackend* g_gles = nullptr;

}  // namespace

std::unique_ptr<gpu::Backend> CreateGlesBackend() {
    auto backend = std::make_unique<GlesBackend>();
    g_gles = backend.get();
    return backend;
}

namespace gpu {
void GlesGraphicsReport() {
    if (g_gles) g_gles->Report("the GL ES renderer");
}
std::string GlesGraphicsInfo() { return g_gles ? "OpenGL ES 3.0, " + g_gles->Name() : std::string("no renderer"); }
}  // namespace gpu
