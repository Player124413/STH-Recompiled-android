// The port's own settings: the values the in-game menu and the touch controls keep in settings.toml beside the saves.
// The game's own switches (skip_cutscenes, cutscene_60fps, widescreen, hd_scene) are the same names the PC build
// uses, so a settings file moves between them.
#include "host.h"

#include <filesystem>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/render_ext.h"
#include "core/log.h"

namespace port {

bool g_hd_scene = true;         // the HD scene target (shadow::g_hd_scene, src/common/render_ext.cpp)
bool g_dynamic_res = true;      // the render resolution follows the frame rate
uint32_t g_min_percent = 40;    // dynamic resolution's floor, percent of the screen's height
uint32_t g_texture_filter = 0;  // 0 linear, 1 point
uint32_t g_dynamic_rendering = 0;  // 0 render passes (the default), 1 dynamic rendering, 2 automatic (Mali: passes)
uint32_t g_renderer = 0;
// the culling experiment: the two renderers disagree on which winding is a front face, and one frame each way on the
// phone settles it faster than another build
bool g_flip_front = false;
bool g_no_depth = false;  // the diagnostic: no depth test at all, see the menu
// why a GL renderer did not start, as the menu shows it: the answer without a log file
std::string g_renderer_note;      // 0 Vulkan, 1 OpenGL ES 3.0 -- Vulkan only for now, see CreateBackend()
bool g_safe_graphics = true;  // every workaround at once, for a phone whose picture comes out wrong
uint32_t g_safe_mode = 1;     // 0 off, 1 every workaround, 2..7 just one of them (see SafePart in vulkan.cpp)
uint32_t g_timeline_semaphores = 2;  // 2 automatic (fences on ARM Mali), 1 timeline semaphores, 0 fences
bool g_touch_controls = true;
bool g_vibration = true;
uint32_t g_camera_speed = 200;  // percent: how fast a drag turns the camera

}  // namespace port

namespace port {

#ifndef SHADOW_ANDROID_VERSION
#define SHADOW_ANDROID_VERSION "1.4"
#endif

const char* VersionName() { return SHADOW_ANDROID_VERSION; }

// Bumped when a default changes in a way an old file should not keep.
constexpr int64_t kPortVersion = 5;

void Load(const void* table) {
    const auto* t = static_cast<const toml::table*>(table);  // the scalars come through app::kGame.options
    int64_t version = t ? (*t)["port_version"].value_or(0) : 0;
    if (version < 1) g_camera_speed = 200;  // the camera was too slow to steer with a thumb at the old 100%
    // render passes draw the scene's effects right where dynamic rendering did not (see the menu's DYNAMIC RENDERING);
    // a file from the one build that wrote 2 for it gets the new default once
    if (version < 2) g_dynamic_rendering = 0;
    // a phone that has never drawn this game before starts on the workarounds: a picture that comes out right on a weak
    // driver is worth more than the few things safe graphics leaves out
    if (version < 3) g_safe_graphics = true;
    // 4: the workarounds are off unless asked for. Two phones that draw the whole picture (Adreno 710, Mali-G57) said
    // what the fallback costs: 480 lines on a 720 line screen, no glow, no HD scene -- a first look at the port that
    // reads as graphical bugs, on a driver that had none
    // 5: the workarounds are a mode, so a picture that needs one of them can say which, and a file that only held the
    // on/off keeps what it had
    if (version < 5) g_safe_mode = g_safe_graphics ? 1 : 0;
    g_safe_graphics = g_safe_mode != 0;
    // the renderer is the player's now: the window is created for it before any backend is built, so an unknown value
    // has to fall back here and not at the end
    if (g_renderer > 1) g_renderer = 0;
    shadow::g_hd_scene = g_hd_scene;
    LOGI("port: settings version %lld, camera speed %u%%", (long long)version, g_camera_speed);
}

void Save(void* table) {
    if (auto* t = static_cast<toml::table*>(table)) t->insert("port_version", kPortVersion);
}

void ResetDefaults() {
    app::Settings& s = app::Get();
    s.render_percent = 60;
    s.fps_cap = 60;
    s.vsync = false;
    s.aniso = 0;
    g_hd_scene = true;
    g_dynamic_res = true;
    g_min_percent = 40;
    g_texture_filter = 0;
    g_dynamic_rendering = 0;
    g_timeline_semaphores = 2;
    g_safe_graphics = true;
    g_safe_mode = 1;
    g_touch_controls = true;
    g_vibration = true;
    g_camera_speed = 200;
    shadow::g_hd_scene = true;
    touch::SetFloatingStick(true);
    touch::SetScale(100);
    touch::SetOpacity(55);
    touch::ResetLayout();
    app::Save();
    LOGI("port: settings back to their defaults");
}

void Init() {
    std::error_code ec;
    for (const char* dir : {"logs", "hdd", "shadercache", "textures"})
        std::filesystem::create_directories(app::ExeDir() / dir, ec);
    shadow::g_hd_scene = g_hd_scene;
    touch::Load();
    dynres::Reset();
    LOGI("port: %s build, render %u%%%s, fps %u, vsync %d, hd scene %d, touch %d", VersionName(),
         app::Get().render_percent, g_dynamic_res ? " (dynamic)" : "", app::Get().fps_cap,
         app::Get().vsync ? 1 : 0, g_hd_scene ? 1 : 0, g_touch_controls ? 1 : 0);
}

}  // namespace port
