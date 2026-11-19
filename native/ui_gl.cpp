// The port's own UI -- the menu and the touch controls -- drawn on the GL ES backend.
//
// Vulkan draws it inside its own frame (vulkan.cpp's RenderUi); the GL ES backend belongs to the SDK and knows nothing
// about this port's UI, so it offers a hook instead (android_platform.h's SetUiDraw) and calls it on its thread with its
// EGL context current, just before the frame goes to the window. This is what that hook draws, with ImGui's GL ES 3
// renderer -- which is why the touch overlay exists at all on this path.
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include "app/app.h"
#include "core/log.h"
#include "gpu/android_platform.h"
#include "host.h"

namespace gpu {
namespace {
bool g_started = false;   // ImGui's GL ES renderer is made once, on the backend's thread and context
ImGuiContext* g_ctx = nullptr;  // the port's ImGui context: this thread has its own current-context slot
}

void UiGlBindContext() { g_ctx = ImGui::GetCurrentContext(); }

void UiGlDraw(uint32_t width, uint32_t height) {
    // the backend may draw on a thread of its own, and ImGui's current context is per thread: without this the menu and
    // the touch controls are simply not drawn there
    if (g_ctx && ImGui::GetCurrentContext() != g_ctx) ImGui::SetCurrentContext(g_ctx);
    if (!g_started) {
        // 3.20 es: the context the GL ES backend made is that one, and ImGui's shaders are written for it
        // 3.20 es is what the backend's context is; a driver that answers with 3.0 still takes 300 es, and saying which
        // one worked is what tells one failure from another
        const char* versions[] = {"#version 320 es", "#version 300 es"};
        for (const char* glsl : versions) {
            if (ImGui_ImplOpenGL3_Init(glsl)) {
                LOGI("ui: ImGui draws through the GL ES backend (%s)", glsl);
                g_started = true;
                break;
            }
            LOGE("ui: ImGui's GL ES renderer refused %s", glsl);
        }
        if (!g_started) {
            LOGE("ui: ImGui's GL ES renderer did not start; the menu and touch controls are not drawn on this backend");
            return;
        }
        LOGI("ui: ImGui draws through the GL ES backend");
    }
    ImGui_ImplOpenGL3_NewFrame();
    ui::NewFrame(float(width), float(height));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(GetFps());
    touch::Draw();
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    if (draw && (draw->CmdListsCount == 0 || draw->TotalVtxCount == 0)) {
        static int quiet = 0;
        if (quiet++ < 3) LOGE("ui: the frame has no draw data (menu and touch controls would be invisible)");
    }
    ImGui_ImplOpenGL3_RenderDrawData(draw);
}

}  // namespace gpu
