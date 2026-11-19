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
bool g_started = false;  // ImGui's GL ES renderer is made once, on the backend's thread and context
}

void UiGlDraw(uint32_t width, uint32_t height) {
    if (!g_started) {
        // 3.20 es: the context the GL ES backend made is that one, and ImGui's shaders are written for it
        if (!ImGui_ImplOpenGL3_Init("#version 320 es")) {
            LOGE("ui: ImGui's GL ES renderer did not start; the menu and touch controls are not drawn on this backend");
            return;
        }
        g_started = true;
        LOGI("ui: ImGui draws through the GL ES backend");
    }
    ImGui_ImplOpenGL3_NewFrame();
    ui::NewFrame(float(width), float(height));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(GetFps());
    touch::Draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace gpu
