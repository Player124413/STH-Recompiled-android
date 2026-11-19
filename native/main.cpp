// The Android host: SDL's window, the Vulkan surface's lifecycle, the app's life in front and behind, and the event
// loop. The game is described by app::kGame (native/game_info.cpp) and boots on threads of its own.
#include <jni.h>
#include <android/log.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
struct ANativeWindow;  // android/native_window.h, forward: this file passes the pointer on and nothing more
#include "gpu/android_platform.h"  // the GL ES backend's window
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_foreground{true};
std::atomic<bool> g_game_active{true};
std::atomic<uint32_t> g_surface_epoch{0};
std::atomic<bool> g_quit{false};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// the log's lines to logcat as well: stderr is read on a thread of its own
void LogToLogcat() {
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]] {
        char buf[2048];
        std::string line;
        for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                __android_log_write(ANDROID_LOG_INFO, "SHADOW", line.substr(0, nl).c_str());
        }
    }).detach();
}

void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (on && !g_foreground) g_surface_epoch++;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "in front" : "in the background");
}

// a JNI call to the activity; false when it has no such method (a build mismatch) or the VM is gone
bool CallActivity(const char* name, const char* sig, int arg = 0, bool has_arg = false) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    if (!cls) return false;
    jmethodID m = env->GetMethodID(cls, name, sig);
    bool ok = m != nullptr;
    if (ok) {
        if (has_arg) env->CallVoidMethod(activity, m, jint(arg));
        else env->CallVoidMethod(activity, m);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return ok;
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() { return g_foreground.load(); }
uint32_t SurfaceEpoch() { return g_surface_epoch.load(); }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load() || g_quit.load(); });
}

void WindowPixels(int& w, int& h) {
    w = h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

void SetGameActive(bool active) { g_game_active = active; }
bool GameActive() { return g_game_active.load() && g_foreground.load(); }

void ExitToLauncher() {
    LOGI("app: back to the launcher");
    CallActivity("finishGame", "()V");
}

// the log, out of the app: the activity reads its tail and puts it on the clipboard, or hands it to another app
void CopyLog() {
    LOGI("app: copying the log");
    if (!CallActivity("copyLog", "()V")) LOGW("app: this build's activity has no copyLog");
}

void CheckForUpdates() {
    if (!CallActivity("checkForUpdates", "()V")) LOGW("app: this build's activity has no checkForUpdates");
}

void ShowAbout() {
    if (!CallActivity("showAbout", "()V")) LOGW("app: this build's activity has no showAbout");
}

void ShareFrameDump() {
    LOGI("app: sharing the frame dump");
    if (!CallActivity("shareDumps", "()V")) LOGW("app: this build's activity has no shareDumps");
}

void ShareLog() {
    LOGI("app: sharing the log");
    if (!CallActivity("shareLog", "()V")) LOGW("app: this build's activity has no shareLog");
}

void HapticTap(int ms) {
    if (!port::g_vibration) return;
    static std::atomic<uint64_t> last{0};
    uint64_t now = uint64_t(SDL_GetTicks());
    if (now - last.load() < 35) return;  // a burst of presses is one buzz
    last = now;
    CallActivity("tapHaptic", "(I)V", ms, true);
}

}  // namespace host

// Android hands controllers which it stopped giving SDL over to us (native/input.cpp): a phone's game service may
// present one as a virtual device SDL ignores.
extern "C" JNIEXPORT void JNICALL
Java_com_sthrecompiled_shadow_NativeBridge_looseKey(JNIEnv*, jclass, jint code, jboolean down) {
    input::LooseKey(code, down == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sthrecompiled_shadow_NativeBridge_looseAxes(JNIEnv*, jclass, jfloat lx, jfloat ly, jfloat rx, jfloat ry,
                                                     jfloat lt, jfloat rt, jfloat hx, jfloat hy) {
    input::LooseAxes(lx, ly, rx, ry, lt, rt, hx, hy);
}

extern "C" JNIEXPORT void JNICALL Java_com_sthrecompiled_shadow_NativeBridge_looseRelease(JNIEnv*, jclass) {
    input::LooseRelease();
}

namespace {

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            g_quit = true;
            g_foreground_cv.notify_all();
            logx::Flush();
            kernel::Shutdown(0);
            return;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            SetForeground(false);
            return;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            SetForeground(true);
            return;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: Android is low on memory");
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            return;
        default:
            break;
    }
    if (touch::OnEvent(e)) return;
    // the port menu takes the keyboard first (Back, Enter, the arrows)
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_AC_BACK) {
            if (touch::Editing()) touch::EndEdit();
            else if (shadow::MenuOpen()) shadow::CloseMenu();
            else shadow::OpenMenu();
            return;
        }
        if (shadow::MenuOpen() || touch::Editing()) shadow::MenuKey(int(e.key.key));
    }
    std::lock_guard lock(app::UiLock());
    ui::OnEvent(e);
}

void OnWindowEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            g_surface_epoch++;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            SetForeground(false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            SetForeground(true);
            break;
        default:
            break;
    }
}

}  // namespace

int main(int, char**) {
    LogToLogcat();
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");  // the game keeps running behind our menus
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        __android_log_print(ANDROID_LOG_ERROR, "SHADOW", "SDL_Init: %s", SDL_GetError());
        return 1;
    }

    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    // the window: fullscreen, Vulkan, high pixel density (the swapchain is in real pixels)
    std::string title = Utf8(app::kGame.title);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    // The window is a plain native window either way: Vulkan makes its own surface from it, and so does the GL ES
    // backend (sdk/runtime/gpu/gles.cpp), which owns its EGL context -- SDL is never asked for an OpenGL one.
    if (port::g_renderer == 0) SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1280);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 720);
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    // what the menu asked for, handed to the platform before anything is created
    gpu::SetRendererChoice(port::g_renderer ? gpu::kRendererGles : gpu::kRendererVulkan);
    if (g_window && port::g_renderer != 0) {
        // the GL ES backend presents to this window itself, so it is handed the native one before the title draws
        auto* native = static_cast<ANativeWindow*>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
        if (native) gpu::SetAndroidWindow(native);
        LOGI("graphics: native window %s for the GL ES renderer", native ? "handed over" : "missing");
    }
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        return 1;
    }

    // the game waits for its files: the launcher may still be importing the disc image, or the player has to allow
    // access to the folder first. It starts the moment they are there, without the app being opened again
    app::GameFiles game;
    for (int waited = 0; !app::FindGame(game); waited++) {
        if (waited % 10 == 0) LOGI("game: still waiting for the game files (%d s)", waited);
        logx::Flush();
        SDL_Delay(1000);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // the GL ES backend asks for this before every frame: the menu and touch controls are the port's, not the SDK's
    gpu::SetUiDraw(&gpu::UiGlDraw);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_IsTouchScreen;
    io.BackendPlatformName = "android_sdl3";
    ui::SetStyle();
    ui::LoadFonts();

    input::Init();
    touch::Init();

    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shadow the Hedgehog",
                                 "The game failed to start. See the log in the logs folder.", g_window);
        logx::Flush();
        return 1;
    }

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                OnWindowEvent(e);
                Dispatch(e);
            } while (SDL_PollEvent(&e));
        }
        touch::Pump();
    }
}
