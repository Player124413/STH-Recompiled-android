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
constexpr int64_t kPortVersion = 1;

void Load(const void* table) {
    const auto* t = static_cast<const toml::table*>(table);  // the scalars come through app::kGame.options
    int64_t version = t ? (*t)["port_version"].value_or(0) : 0;
    if (version < 1) g_camera_speed = 200;  // the camera was too slow to steer with a thumb at the old 100%
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
