// The in-game port menu: a touch-friendly screen drawn with ImGui in the port's dark red look. It opens from the
// MENU button of the touch controls, Android's back gesture and a controller's BACK + START, and saves every change
// to settings.toml at once. The game's own pause comes up behind it: opening the menu taps START for the game.
#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "app/app.h"
#include "common/render_ext.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "kernel/kernel.h"

namespace shadow {

extern bool g_skip_cutscenes;  // src/game/hooks.cpp
extern bool g_cutscene_60;     // src/game/hooks.cpp
extern bool g_widescreen;      // src/game/hooks.cpp
void ApplyWidescreen();        // src/game/hooks.cpp

std::atomic<bool> g_menu_open{false};
bool g_show_fps = false;

namespace {

enum Tab : int { kDisplay, kGraphics, kControls, kGame, kAbout, kTabs };
const char* const kTabNames[] = {"DISPLAY", "GRAPHICS", "CONTROLS", "GAME", "ABOUT"};

int g_tab = kDisplay;
double g_fps = 0;
uint64_t g_start_press = 0;  // ms: START is the game's own pause for a moment while our menu opens
bool g_start_seen = false;

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr uint32_t kFrameRates[] = {30, 60, 90, 120, 144, 0};
const char* FrameRateName(uint32_t cap) {
    switch (cap) {
        case 0: return "UNLIMITED";
        case 30: return "30 FPS";
        case 60: return "60 FPS";
        case 90: return "90 FPS";
        case 120: return "120 FPS";
        case 144: return "144 FPS";
        default: return "CUSTOM";
    }
}

void StepFrameRate(int step) {
    uint32_t cap = app::Get().fps_cap;
    constexpr size_t n = std::size(kFrameRates);
    size_t i = 0;
    while (i < n && kFrameRates[i] != cap) i++;
    if (i >= n) i = 1;
    i = (i + n + size_t(step)) % n;
    app::Get().fps_cap = kFrameRates[i];
}

const uint32_t kAniso[] = {0, 2, 4, 8, 16};
void StepAniso(int step) {
    uint32_t a = app::Get().aniso;
    constexpr size_t n = std::size(kAniso);
    size_t i = 0;
    while (i < n && kAniso[i] != a) i++;
    if (i >= n) i = 0;
    i = (i + n + size_t(step)) % n;
    app::Get().aniso = kAniso[i];
}

std::string AnisoName() {
    uint32_t a = app::Get().aniso;
    return a ? std::to_string(a) + "x" : "OFF";
}

// ---- rows, made for a finger: a full-width band, the label left, the value right; the whole band is the button

float RowHeight() { return ImGui::GetFontSize() * 2.15f; }

// a tap, not a drag: a finger that has moved has been scrolling the page, not choosing the row
bool Tapped() {
    if (!ImGui::IsItemHovered() || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return false;
    return ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 12.0f * 12.0f;
}

void RowBackground(const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? IM_COL32(64, 24, 26, 255) : hovered ? IM_COL32(44, 26, 28, 255) : IM_COL32(24, 22, 25, 255);
    dl->AddRectFilled(a, b, bg, 10.0f);
    dl->AddRect(a, b, IM_COL32(126, 40, 38, hovered ? 200 : 110), 10.0f, 0, 1.5f);
}

// a row with a left and a right side; true when tapped
bool Row(const char* label, const char* value, ImU32 value_color, bool value_left = false) {
    float fs = ImGui::GetFontSize();
    float h = RowHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool tapped = Tapped();
    bool hovered = ImGui::IsItemHovered();
    RowBackground(a, ImVec2(a.x + w, a.y + h), hovered, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(a.x + fs * 0.9f, a.y + (h - fs) * 0.5f), IM_COL32(236, 232, 230, 255), label);
    ImVec2 t = ImGui::CalcTextSize(value);
    float vx = value_left ? a.x + w - t.x - fs * 3.0f : a.x + w - t.x - fs * 0.9f;
    dl->AddText(ImVec2(vx, a.y + (h - fs) * 0.5f), value_color, value);
    if (value_left) {
        dl->AddText(ImVec2(a.x + w - fs * 0.9f - t.x * 0.25f, a.y + (h - fs) * 0.5f), IM_COL32(150, 80, 78, 255), ">");
        dl->AddText(ImVec2(a.x + w - t.x - fs * 4.3f, a.y + (h - fs) * 0.5f), IM_COL32(150, 80, 78, 255), "<");
    }
    ImGui::Dummy(ImVec2(0, 3));
    return tapped;
}

bool Row(const char* label, const std::string& value, ImU32 color, bool value_left = false) {
    return Row(label, value.c_str(), color, value_left);
}

// an ON / OFF row: true when it was tapped
bool ToggleRow(const char* label, bool on) {
    return Row(label, on ? "ON" : "OFF", on ? IM_COL32(110, 210, 110, 255) : IM_COL32(150, 150, 155, 255));
}

// a percentage row with - and + ends, tapped at either end: -1, +1, or 0
int RangeRow(const char* label, uint32_t value, const char* suffix, uint32_t lo, uint32_t hi) {
    float fs = ImGui::GetFontSize();
    float h = RowHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    int step = 0;
    if (Tapped()) {
        float mx = ImGui::GetIO().MousePos.x;
        step = mx < a.x + w * 0.25f ? -1 : mx > a.x + w * 0.75f ? 1 : 0;
        if (!step) step = 1;
    }
    RowBackground(a, ImVec2(a.x + w, a.y + h), hovered, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(a.x + fs * 0.9f, a.y + (h - fs) * 0.5f), IM_COL32(236, 232, 230, 255), label);
    char text[32];
    std::snprintf(text, sizeof(text), "%u%s", value, suffix);
    ImVec2 t = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(a.x + w - t.x - fs * 1.3f, a.y + (h - fs) * 0.5f), IM_COL32(240, 120, 110, 255), text);
    // the ends, so the player sees where to tap
    dl->AddText(ImVec2(a.x + w - t.x - fs * 3.2f, a.y + (h - fs) * 0.5f), IM_COL32(160, 90, 88, 255), "-");
    dl->AddText(ImVec2(a.x + w - fs * 0.4f, a.y + (h - fs) * 0.5f), IM_COL32(160, 90, 88, 255), "+");
    ImGui::Dummy(ImVec2(0, 3));
    if (value <= lo && step < 0) return 0;
    if (value >= hi && step > 0) return 0;
    return step;
}

bool ButtonRow(const char* label, ImU32 tint) {
    float fs = ImGui::GetFontSize();
    float h = RowHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool tapped = Tapped();
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = hovered ? IM_COL32(60, 24, 26, 255) : IM_COL32(24, 22, 25, 255);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), bg, 10.0f);
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), tint, 10.0f, 0, 1.5f);
    ImVec2 t = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(a.x + (w - t.x) * 0.5f, a.y + (h - fs) * 0.5f), tint, label);
    ImGui::Dummy(ImVec2(0, 3));
    return tapped;
}

void Section(const char* text) {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.58f, 0.58f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
}

void Save() { app::Save(); }

// ---- the pages

void DisplayPage() {
    Section("PICTURE");
    uint32_t p = app::Get().render_percent;
    int step = RangeRow("RENDER RESOLUTION", p, "%", 30, 100);
    if (step) {
        app::Get().render_percent = uint32_t(std::clamp<int>(int(p) + step * 5, 30, 100));
        Save();
    }
    if (ToggleRow("DYNAMIC RESOLUTION", port::g_dynamic_res)) {
        port::g_dynamic_res = !port::g_dynamic_res;
        dynres::Reset();
        Save();
    }
    Section("FRAME RATE");
    if (Row("TARGET FRAME RATE", FrameRateName(app::Get().fps_cap), IM_COL32(240, 120, 110, 255), true)) {
        StepFrameRate(1);
        Save();
    }
    if (ToggleRow("V-SYNC", app::Get().vsync)) {
        app::Get().vsync = !app::Get().vsync;
        Save();
    }
    if (ToggleRow("FPS COUNTER", g_show_fps)) {
        g_show_fps = !g_show_fps;
        Save();
    }
    Section("IMAGE");
    if (Row("TEXTURE FILTER", touch::FilterName(), IM_COL32(240, 120, 110, 255), true)) {
        port::g_texture_filter = port::g_texture_filter ? 0 : 1;
        Save();
    }
    if (Row("ANISOTROPIC FILTER", AnisoName(), IM_COL32(240, 120, 110, 255), true)) {
        StepAniso(1);
        Save();
    }
    uint32_t lo = port::g_min_percent;
    step = RangeRow("DYNAMIC FLOOR", lo, "%", 30, 100);
    if (step) {
        port::g_min_percent = uint32_t(std::clamp<int>(int(lo) + step * 5, 30, 100));
        dynres::Reset();
        Save();
    }
}

const char* DynamicRenderingName() {
    switch (port::g_dynamic_rendering) {
        case 1: return "ON";
        case 2: return "AUTOMATIC";
        default: return "OFF (RENDER PASSES)";
    }
}

const char* SyncName() {
    switch (port::g_timeline_semaphores) {
        case 1: return "TIMELINE";
        case 0: return "FENCES";
        default: return "AUTOMATIC (FENCES)";
    }
}

void GraphicsPage() {
    Section("THE PORT");
    if (ToggleRow("HD 3D WORLD", port::g_hd_scene)) {
        port::g_hd_scene = !port::g_hd_scene;
        shadow::g_hd_scene = port::g_hd_scene;
        Save();
    }
    if (Row("DYNAMIC RENDERING", DynamicRenderingName(), IM_COL32(240, 120, 110, 255), true)) {
        port::g_dynamic_rendering = (port::g_dynamic_rendering + 1) % 3;  // off -> on -> automatic -> off
        Save();
        LOGI("port: dynamic rendering %s, takes effect on the next start", DynamicRenderingName());
    }
    if (Row("GPU SYNC", SyncName(), IM_COL32(240, 120, 110, 255), true)) {
        port::g_timeline_semaphores = (port::g_timeline_semaphores + 1) % 3;  // automatic -> timeline -> fences
        Save();
        LOGI("port: gpu sync %s, takes effect on the next start", SyncName());
    }
    if (ToggleRow("WIDESCREEN 16:9", g_widescreen)) {
        g_widescreen = !g_widescreen;
        ApplyWidescreen();
        Save();
    }
    if (ButtonRow("GRAPHICS REPORT", IM_COL32(200, 170, 90, 255))) host::GraphicsReport();
    Section("CUTSCENES");
    if (ToggleRow("SKIP CUTSCENES", g_skip_cutscenes)) {
        g_skip_cutscenes = !g_skip_cutscenes;
        Save();
    }
    if (ToggleRow("CUTSCENES AT 60 FPS", g_cutscene_60)) {
        g_cutscene_60 = !g_cutscene_60;
        Save();
    }
}

void ControlsPage() {
    Section("TOUCH CONTROLS");
    if (ToggleRow("SHOW CONTROLS", port::g_touch_controls)) {
        port::g_touch_controls = !port::g_touch_controls;
        Save();
    }
    if (Row("LAYOUT", touch::PresetName(), IM_COL32(240, 120, 110, 255), true)) {
        touch::NextPreset(1);
    }
    if (ButtonRow("EDIT LAYOUT", IM_COL32(240, 120, 110, 255))) {
        CloseMenu();
        touch::BeginEdit();
    }
    uint32_t size = touch::Scale();
    int step = RangeRow("BUTTON SIZE", size, "%", 60, 160);
    if (step) touch::SetScale(uint32_t(std::clamp<int>(int(size) + step * 5, 60, 160)));
    uint32_t alpha = touch::Opacity();
    step = RangeRow("OPACITY", alpha, "%", 10, 100);
    if (step) touch::SetOpacity(uint32_t(std::clamp<int>(int(alpha) + step * 5, 10, 100)));
    if (Row("MOVE STICK", touch::FloatingStick() ? "FLOATING" : "FIXED", IM_COL32(240, 120, 110, 255), true))
        touch::SetFloatingStick(!touch::FloatingStick());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.68f, 0.68f, 1.0f));
    ImGui::TextWrapped("The camera is the screen: drag anywhere that is not a button. CAMERA SPEED is how far a drag "
                       "turns it.");
    ImGui::PopStyleColor();
    uint32_t camera = port::g_camera_speed;
    step = RangeRow("CAMERA SPEED", camera, "%", 50, 400);
    if (step) {
        port::g_camera_speed = uint32_t(std::clamp<int>(int(camera) + step * 25, 50, 400));
        Save();
    }
    if (ToggleRow("VIBRATION", port::g_vibration)) {
        port::g_vibration = !port::g_vibration;
        if (port::g_vibration) host::HapticTap(30);
        Save();
    }
    Section("CONTROLLER");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.68f, 0.68f, 1.0f));
    ImGui::TextWrapped("A gamepad plugged into the phone plays as on the console and can be used here too. "
                       "Xbox, PlayStation and Switch pads all work.");
    ImGui::PopStyleColor();
}

void GamePage() {
    Section("SESSION");
    if (ButtonRow("RESUME", IM_COL32(120, 220, 120, 255))) CloseMenu();
    if (ButtonRow("EXIT TO LAUNCHER", IM_COL32(240, 120, 110, 255))) {
        Save();
        logx::Flush();
        host::ExitToLauncher();
        kernel::Shutdown(0);
    }
    Section("RESET");
    if (ButtonRow("RESTORE DEFAULT SETTINGS", IM_COL32(200, 170, 90, 255))) {
        port::ResetDefaults();
        dynres::Reset();
    }
}

void AboutPage() {
    Section("SHADOW THE HEDGEHOG - ANDROID");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.80f, 0.79f, 1.0f));
    ImGui::TextWrapped("An unofficial native port of the original Xbox game, built by static recompilation: the game's own "
                       "x86 code runs as native ARM64, drawn with Vulkan. This app ships no game files and no game code; it "
                       "is built from your own disc image.");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped("Version %s. Renderer and touch controls after the XboxXBE-Recomp SDK (MIT) and Rangga Casper's "
                       "Black Android port (MIT).",
                       port::VersionName());
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped("Shadow the Hedgehog is a trademark of SEGA. This project is not affiliated with SEGA.");
    ImGui::PopStyleColor();
    if (ButtonRow("CHECK FOR UPDATES", IM_COL32(200, 170, 90, 255))) host::CheckForUpdates();
    if (ButtonRow("ABOUT THIS PORT", IM_COL32(200, 170, 90, 255))) host::ShowAbout();
    Section("LOG");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.68f, 0.68f, 1.0f));
    ImGui::TextWrapped("If something goes wrong, this run's log is one tap from the clipboard or from any app you can "
                       "send it with - no need to dig it out of Android/data.");
    ImGui::PopStyleColor();
    if (ButtonRow("COPY LOG", IM_COL32(120, 200, 240, 255))) host::CopyLog();
    if (ButtonRow("SEND LOG", IM_COL32(120, 200, 240, 255))) host::ShareLog();
    Section("HOW TO PLAY");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.80f, 0.79f, 1.0f));
    ImGui::TextWrapped("Left stick walks, the right stick turns the camera, A jumps, B is the action button, X fires, "
                       "Y switches weapons, BLACK and WHITE are the shoulder buttons, LT aims and RT fires the heavy weapon. "
                       "START pauses, BACK opens the map. Drag any control in EDIT LAYOUT.");
    ImGui::PopStyleColor();
}

// ---- the frame

void DrawChrome(ImVec2 size) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(6, 5, 7, 232));
    // a red glow from the top, the console menu's
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.35f), IM_COL32(90, 12, 12, 160),
                                IM_COL32(90, 12, 12, 160), IM_COL32(6, 5, 7, 0), IM_COL32(6, 5, 7, 0));
    float fs = ImGui::GetFontSize();
    ImVec2 title(size.x * 0.05f, size.y * 0.055f);
    dl->AddText(ImGui::GetFont(), fs * 2.2f, title, IM_COL32(232, 228, 226, 255), "SHADOW THE HEDGEHOG");
    dl->AddText(ImVec2(size.x * 0.05f, size.y * 0.115f), IM_COL32(200, 70, 62, 255), "PORT SETTINGS");
    char fps[64];
    std::snprintf(fps, sizeof(fps), "%s  -  %.0f FPS", port::VersionName(), g_fps);
    ImVec2 t = ImGui::CalcTextSize(fps);
    dl->AddText(ImVec2(size.x * 0.95f - t.x, size.y * 0.06f), IM_COL32(170, 165, 165, 255), fps);
    dl->AddLine(ImVec2(size.x * 0.05f, size.y * 0.165f), ImVec2(size.x * 0.95f, size.y * 0.165f),
                IM_COL32(180, 40, 36, 190), 2.0f);
}

void DrawTabs(ImVec2 size) {
    ImGui::SetCursorPos(ImVec2(size.x * 0.05f, size.y * 0.19f));
    float fs = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.9f, 0));
    for (int i = 0; i < kTabs; i++) {
        bool on = g_tab == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.62f, 0.12f, 0.11f, 1) : ImVec4(0.12f, 0.11f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 0.95f, 0.94f, 1) : ImVec4(0.68f, 0.64f, 0.64f, 1));
        if (ImGui::Button(kTabNames[i], ImVec2(fs * 7.4f, fs * 2.2f))) g_tab = i;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    ImGui::PopStyleVar();
}

}  // namespace

void OpenMenu() {
    if (g_menu_open) return;
    g_menu_open = true;
    g_start_press = NowMs() + 220;  // the game's own pause takes the START that opens our menu
    g_start_seen = false;
    LOGI("menu: open");
}

void CloseMenu() {
    if (!g_menu_open) return;
    g_menu_open = false;
    app::Save();
    LOGI("menu: closed");
}

void ToggleMenu() {
    if (g_menu_open) CloseMenu();
    else OpenMenu();
}

bool MenuOpen() { return g_menu_open; }

void MenuKey(int key) {
    if (!g_menu_open) return;
    if (key == SDLK_ESCAPE || key == SDLK_F1) CloseMenu();
}

bool MenuPad(int port, input::Pad& pad) {
    if (port != 0) return false;
    bool b = (pad.buttons & (1u << 5)) != 0, s = (pad.buttons & (1u << 4)) != 0;
    if (!g_menu_open) {
        // BACK + START, as the PC build
        static bool back = false, start = false;
        if (b && s && !(back && start)) OpenMenu();
        back = b;
        start = s;
        return false;
    }
    // open: the game gets no input, but the START that opens the menu is still passed on, so the game pauses behind it
    bool connected = pad.connected;
    pad = input::Pad{};
    pad.connected = connected;
    if (!g_start_seen && NowMs() < g_start_press) {
        pad.buttons = 1u << 4;
        g_start_seen = true;
    }
    return true;
}

void MenuUi(double fps) {
    g_fps = fps;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (g_show_fps && !g_menu_open) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f FPS", fps);
        ImFont* font = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(size.x * 0.5f - t.x * 0.5f - 8, size.y * 0.012f - 4),
                          ImVec2(size.x * 0.5f + t.x * 0.5f + 8, size.y * 0.012f + t.y + 4), IM_COL32(0, 0, 0, 110), 6);
        dl->AddText(ImVec2(size.x * 0.5f - t.x * 0.5f, size.y * 0.012f), IM_COL32(220, 220, 225, 210), text);
    }
    if (!g_menu_open) return;

    DrawChrome(size);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##portmenu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(3);
    DrawTabs(size);
    ImGui::SetCursorPos(ImVec2(size.x * 0.05f, size.y * 0.28f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ImGui::BeginChild("##page", ImVec2(size.x * 0.90f, size.y * 0.60f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(std::clamp(size.y / 1100.0f, 0.55f, 1.1f));
    switch (g_tab) {
        case kDisplay: DisplayPage(); break;
        case kGraphics: GraphicsPage(); break;
        case kControls: ControlsPage(); break;
        case kGame: GamePage(); break;
        default: AboutPage(); break;
    }
    // a phone scrolls a page by dragging it: ImGui itself only scrolls on the mouse wheel
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float dy = ImGui::GetIO().MouseDelta.y;
        if (std::fabs(dy) > 0.5f) ImGui::SetScrollY(ImGui::GetScrollY() - dy);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // the footer: what BACK does, and the close button
    ImGui::SetCursorPos(ImVec2(size.x * 0.05f, size.y * 0.91f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.60f, 0.60f, 1.0f));
    ImGui::TextUnformatted("BACK closes   -   changes are saved as you make them");
    ImGui::PopStyleColor();
    float fs = ImGui::GetFontSize();
    ImGui::SetCursorPos(ImVec2(size.x * 0.80f, size.y * 0.895f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.10f, 0.09f, 1));
    if (ImGui::Button("RESUME", ImVec2(fs * 9.0f, fs * 2.4f))) CloseMenu();
    ImGui::PopStyleColor();
    ImGui::End();
}

}  // namespace shadow
