// The host side on Android: the window's size, the settings file, the folders, pacing, and the stubs for what a
// Windows desktop has and a phone does not (display modes, splash, handover, a second instance).
#include <SDL3/SDL.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"

namespace host {

SDL_Window* Window();
bool Foreground();

}  // namespace host

namespace app {

namespace {

namespace fs = std::filesystem;

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

fs::path File() { return ExeDir() / kGame.settings; }

// a phone's first run: 60 FPS, no V-Sync (Android's compositor already paces the frame), and the render resolution
// below the screen's so a mid-range GPU holds 60. Dynamic resolution moves it from there.
void PhoneDefaults(Settings& s) {
    s.display_mode = DisplayMode::Borderless;
    s.width = s.height = 0;
    s.render_percent = 60;
    s.fps_cap = 60;
    s.vsync = false;
    s.aniso = 0;
    s.custom_textures = false;
    s.dump_textures = false;
}

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

std::string Property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

// the screen in the orientation the game runs in: landscape
Mode ScreenMode() {
    int w = 0, h = 0;
    host::WindowPixels(w, h);
    if (w > 0 && h > 0) return {uint32_t(w), uint32_t(h)};
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return {1920, 1080};
    uint32_t dw = uint32_t(m->w * (m->pixel_density > 0 ? m->pixel_density : 1));
    uint32_t dh = uint32_t(m->h * (m->pixel_density > 0 ? m->pixel_density : 1));
    return {std::max(dw, dh), std::min(dw, dh)};
}

float RefreshRate() {
    SDL_DisplayID d = host::Window() ? SDL_GetDisplayForWindow(host::Window()) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
}

}  // namespace

Settings& Get() {
    static Settings s = [] {
        Settings d;
        PhoneDefaults(d);
        return d;
    }();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
        LOGR("Settings: %s", File().string().c_str());
    } catch (...) {
        LOGR("Settings: %s not there yet, defaults", File().string().c_str());
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = std::clamp<uint32_t>(t["render_percent"].value_or(s.render_percent), 20, 200);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = std::min<uint32_t>(t["anisotropic_filtering"].value_or(s.aniso), 16);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = uint32_t(t[o.key].value_or(*o.value));
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, port %s", kGame.title, IMAGE_XBE, date, port::VersionName());
    LOGR("Path: %s", ExeDir().string().c_str());
    LOGR("Device: %s %s, Android %s (API %d)", Property("ro.product.manufacturer").c_str(),
         Property("ro.product.model").c_str(), Property("ro.build.version.release").c_str(), SDL_GetAndroidSDKVersion());
    LOGR("SoC: %s, %d cores", Property("ro.soc.model").c_str(), SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Screen: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

void ApplyDisplay() {}  // the app is always fullscreen

Mode Desktop() { return ScreenMode(); }
Mode Output() { return Desktop(); }

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

// The console draws 640x480; on a phone the frame covers the screen. Rendering enough pixels for the wider axis keeps
// the 3D view and the HUD sharp, and the percentage scales the whole thing down for the GPU.
float RenderScale(uint32_t guest_height) {
    Mode out = Output();
    uint32_t percent = dynres::Percent();
    float h = float(out.height) * percent / 100.0f;
    float w = float(out.width) * percent / 100.0f;
    if (!guest_height) return 1.0f;
    float scale = h / float(guest_height);
    if (out.width * 2 >= out.height * 3) scale = std::max(scale, w / 640.0f);
    return std::clamp(scale, 0.1f, 8.0f);
}

std::filesystem::path ExeDir() {
    static const fs::path dir = [] {
        const char* p = nullptr;
        if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) p = SDL_GetAndroidExternalStoragePath();
        if (!p) p = SDL_GetAndroidInternalStoragePath();
        fs::path d = p ? p : ".";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

int Instance() { return 0; }
void* Window() { return host::Window(); }

// no splash, no relaunch into another XBE on Android
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app
