// The port's own UI -- the menu and the touch controls -- drawn on the SDK's GL ES backend.
//
// Vulkan draws this UI inside its own frame; the GL ES backend belongs to the SDK and calls a hook instead
// (android_platform.h's SetUiDraw), on its thread with its EGL context current, just before the frame is swapped.
//
// The drawing here is ImGui's vertex data on a shader of this port's own, not imgui_impl_opengl3: that backend draws one
// enormous blur over the whole screen on this driver -- the font atlas smeared across the picture -- and no menu at all,
// so the vertex format, the projection and the scissor are spelled out here where they can be read.
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "imgui.h"

#include "app/app.h"
#include "core/log.h"
#include "gpu/android_platform.h"
#include "host.h"

namespace gpu {
namespace {

ImGuiContext* g_ctx = nullptr;  // the port's context: ImGui's current one is per thread, and the backend draws on its own
bool g_started = false;
GLuint g_program = 0, g_vao = 0, g_vbo = 0, g_ibo = 0, g_font = 0, g_white = 0, g_quad = 0;

const char* kVertex = R"(#version 300 es
precision highp float;
uniform vec2 u_scale;   // 2/w, -2/h: ImGui's pixels to clip space, with the origin at the top left
uniform vec2 u_shift;   // -1, 1
in vec2 a_pos;
in vec2 a_uv;
in vec4 a_col;
out vec2 v_uv;
out vec4 v_col;
void main() {
    v_uv = a_uv;
    v_col = a_col;
    gl_Position = vec4(a_pos * u_scale + u_shift, 0.0, 1.0);
}
)";

const char* kFragment = R"(#version 300 es
precision highp float;
uniform sampler2D u_tex;
in vec2 v_uv;
in vec4 v_col;
out vec4 o_color;
void main() { o_color = v_col * texture(u_tex, v_uv); }
)";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        LOGE("ui: the %s shader was refused: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool Start() {
    GLuint vs = Compile(GL_VERTEX_SHADER, kVertex), fs = Compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) return false;
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glBindAttribLocation(g_program, 0, "a_pos");
    glBindAttribLocation(g_program, 1, "a_uv");
    glBindAttribLocation(g_program, 2, "a_col");
    glLinkProgram(g_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(g_program, sizeof(log) - 1, nullptr, log);
        LOGE("ui: the UI program was refused: %s", log);
        return false;
    }
    // the font atlas, once: ImGui hands out RGBA8 and this is what the shader samples
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    glGenTextures(1, &g_font);
    glBindTexture(GL_TEXTURE_2D, g_font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(g_font));  // an id, not a pointer: 1.91 keeps it in an integer
    // a 1x1 white texel, and a square to prove the whole path on a phone: if the square is on screen and the menu is
    // not, the shader and the swap are fine and the fault is in ImGui's vertex data; if neither is, the hook never ran
    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ibo);
    LOGI("ui: the port's UI draws on GL ES, font atlas %dx%d", w, h);
    return true;
}

}  // namespace

void UiGlBindContext() { g_ctx = ImGui::GetCurrentContext(); }

void UiGlDraw(uint32_t width, uint32_t height) {
    if (g_ctx && ImGui::GetCurrentContext() != g_ctx) ImGui::SetCurrentContext(g_ctx);
    if (!g_started) {
        if (!Start()) return;
        g_started = true;
    }
    ui::NewFrame(float(width), float(height));  // the port's input and display size, then this frame's widgets
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(GetFps());
    touch::Draw();
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0 || width == 0 || height == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, GLsizei(width), GLsizei(height));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font);
    glUseProgram(g_program);
    glUniform2f(glGetUniformLocation(g_program, "u_scale"), 2.0f / float(width), -2.0f / float(height));
    glUniform2f(glGetUniformLocation(g_program, "u_shift"), -1.0f, 1.0f);
    glUniform1i(glGetUniformLocation(g_program, "u_tex"), 0);
    glBindVertexArray(g_vao);

    for (int n = 0; n < draw->CmdListsCount; n++) {
        const ImDrawList* list = draw->CmdLists[n];
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(list->VtxBuffer.Size * sizeof(ImDrawVert)), list->VtxBuffer.Data,
                     GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(list->IdxBuffer.Size * sizeof(ImDrawIdx)),
                     list->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            // this port reads VtxOffset by moving the attribute pointers, which ES 3.0 can do without base vertex
            const size_t vtx = size_t(cmd.VtxOffset) * sizeof(ImDrawVert);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<const void*>(vtx + 0));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<const void*>(vtx + 8));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert),
                                  reinterpret_cast<const void*>(vtx + 16));
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            // ImGui's clip rectangle is in its own pixels from the top left; GL counts from the bottom
            const float x0 = cmd.ClipRect.x, y0 = cmd.ClipRect.y, x1 = cmd.ClipRect.z, y1 = cmd.ClipRect.w;
            glScissor(GLint(x0), GLint(float(height) - y1), GLsizei(std::max(0.0f, x1 - x0)),
                      GLsizei(std::max(0.0f, y1 - y0)));
            glDrawElements(GL_TRIANGLES, GLsizei(cmd.ElemCount), GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(size_t(cmd.IdxOffset) * sizeof(ImDrawIdx)));
        }
    }
    // the square: 120x120 at the top left, solid red, drawn after ImGui so it cannot be hidden by it
    struct V { float x, y, u, v; unsigned int c; };
    const V quad[6] = {{0, 0, 0, 0, 0xFF0000FFu},     {120, 0, 1, 0, 0xFF0000FFu}, {0, 120, 0, 1, 0xFF0000FFu},
                       {120, 0, 1, 0, 0xFF0000FFu},   {120, 120, 1, 1, 0xFF0000FFu}, {0, 120, 0, 1, 0xFF0000FFu}};
    if (!g_quad) glGenBuffers(1, &g_quad);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glScissor(0, GLint(height) - 120, 120, 120);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<const void*>(8));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), reinterpret_cast<const void*>(16));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace gpu
