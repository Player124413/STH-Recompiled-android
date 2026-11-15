// The Spider-Man 2 renderer's platform, on this port's own window: the GL ES 3.1 context the launcher already makes,
// and the frames going to the window's framebuffer. Their renderer asks for six things and this is all of them; the
// rest of glr::Host has defaults that leave the console's picture size and no program cache, which is what this port
// wants until the renderer earns them.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "gl43/gl_api.h"
#include "gl43/gl_backend.h"

namespace port {
extern std::string g_renderer_note;  // shown on the menu, so the reason needs no log file
}

namespace {

class SdlHost final : public glr::Host {
public:
    explicit SdlHost(SDL_Window* window) : window_(window) {}

    // made and its functions loaded the first time; after that only made current on whichever thread is drawing
    bool MakeCurrent() override {
        if (!window_) return false;
        if (!ctx_) {
            ctx_ = SDL_GL_CreateContext(window_);
            if (!ctx_) {
                port::g_renderer_note = std::string("no GL context: ") + SDL_GetError();
                LOGE("gl43: no GL context on this window (%s)", SDL_GetError());
                return false;
            }
            // their loader: every function the renderer names, through the driver's own lookup, which is what EGL
            // gives on Android
            auto get = [](const char* name) -> void* {
                return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
            };
            const char* missing = nullptr;
            // glPolygonMode and glClearDepth do not exist on any ES driver; the second is mapped to glClearDepthf below,
            // and the first is not called any more, so both are loaded as optional and named in the log if absent
            if (!glapi::Load(get, &missing)) {
                port::g_renderer_note = std::string("this driver has no ") + (missing ? missing : "entry point");
                LOGE("gl43: this driver has no %s, so the renderer cannot run", missing ? missing : "entry point");
                return false;
            }
            // through the loader, not the prototypes glcorearb.h leaves behind: on Android there is no libGL to link
            // them against, and a bare call is an undefined symbol at link time
            GLint major = 0, minor = 0;
            glapi::glGetIntegerv(GL_MAJOR_VERSION, &major);
            glapi::glGetIntegerv(GL_MINOR_VERSION, &minor);
            SDL_Log("gl43: OpenGL ES %d.%d, %s", int(major), int(minor),
                    reinterpret_cast<const char*>(glapi::glGetString(GL_RENDERER)));
        } else if (SDL_GL_GetCurrentContext() != ctx_) {
            if (!SDL_GL_MakeCurrent(window_, ctx_)) {
                port::g_renderer_note = std::string("context not current: ") + SDL_GetError();
                LOGE("gl43: the context cannot be made current (%s)", SDL_GetError());
                return false;
            }
        }
        return true;
    }

    void Release() override { SDL_GL_MakeCurrent(window_, nullptr); }

    void ScreenSize(uint32_t& w, uint32_t& h) override {
        int iw = 0, ih = 0;
        SDL_GetWindowSizeInPixels(window_, &iw, &ih);
        w = uint32_t(iw);
        h = uint32_t(ih);
    }

    void Swap() override { SDL_GL_SwapWindow(window_); }

    void SetSwapInterval(uint32_t blanks) override { SDL_GL_SetSwapInterval(int(blanks)); }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
};

SdlHost* g_host = nullptr;  // the renderer holds the pointer, so it outlives the backend

}  // namespace

namespace glr {

// nullptr when GL cannot be set up: the launcher then keeps the Vulkan renderer, and the log says which piece was
// missing
std::unique_ptr<gpu::Backend> CreateBackendOnWindow(SDL_Window* window) {
    if (!g_host) g_host = new SdlHost(window);
    if (!g_host->MakeCurrent()) return nullptr;
    auto backend = CreateBackend(g_host);
    if (!backend) {
        if (port::g_renderer_note.empty()) port::g_renderer_note = "the renderer refused this GL ES context";
        LOGE("gl43: the renderer refused this GL ES context, Vulkan draws instead");
    }
    return backend;
}

}  // namespace glr
