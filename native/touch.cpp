// The touch controls: a move stick, the Xbox diamond of face buttons, the triggers, START and the port menu, drawn with
// ImGui over the game and fed into port 0's pad. The camera is not a stick: dragging anywhere on the screen that is
// not a control turns it, the way a phone is expected to work. Every control can be dragged where the thumb wants it
// (the layout editor), the layout is saved in touch.toml, and three presets give a starting point.
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"

namespace touch {

namespace {

using clock = std::chrono::steady_clock;

float NowSeconds() {
    return std::chrono::duration<float>(clock::now().time_since_epoch()).count();
}

// the buttons of the console controller, and nothing else
enum Part : int {
    kMove, kA, kB, kX, kY, kLt, kRt, kStart, kMenu, kParts, kNone = -1,
    kCamera = -2  // not a control: a finger dragging the picture turns the camera
};

enum class Kind { Stick, Circle, Pill };

struct Info {
    int part;
    const char* id;
    const char* label;
    Kind kind;
    float size;  // of the screen's height, before the player's size percent
    ImU32 color;
};

const ImU32 kGreen = IM_COL32(96, 190, 96, 255), kRed = IM_COL32(214, 74, 64, 255), kBlue = IM_COL32(74, 132, 224, 255),
           kYellow = IM_COL32(226, 186, 74, 255), kTrigger = IM_COL32(176, 176, 188, 255),
           kSystem = IM_COL32(206, 206, 216, 255);

// the order is the draw order: the move stick, the face buttons, the triggers, then MENU
const Info kInfo[kParts] = {
    {kMove, "move", "MOVE", Kind::Stick, 0.17f, kSystem},
    {kA, "a", "A", Kind::Circle, 0.060f, kGreen},
    {kB, "b", "B", Kind::Circle, 0.055f, kRed},
    {kX, "x", "X", Kind::Circle, 0.055f, kBlue},
    {kY, "y", "Y", Kind::Circle, 0.055f, kYellow},
    {kLt, "lt", "LT", Kind::Pill, 0.060f, kTrigger},
    {kRt, "rt", "RT", Kind::Pill, 0.060f, kTrigger},
    {kStart, "start", "START", Kind::Pill, 0.052f, kSystem},
    {kMenu, "menu", "MENU", Kind::Pill, 0.050f, kRed},
};

// The Xbox diamond: Y on top, X on the left, B on the right, A at the bottom. The horizontal offsets are smaller
// than the vertical one because the screen is wider than it is tall: on a phone held sideways this lands as a circle

const Info& InfoOf(int part) { return kInfo[part]; }

struct Place {
    float x = 0.5f, y = 0.5f;  // of the screen
    float scale = 1.0f;        // of the control's own size
};

using Layout = std::array<Place, kParts>;

// Three starting points. Fractions of the screen, y down.
const Layout kModern = {{
    {0.15f, 0.75f, 1.0f},    // move
    {0.90f, 0.765f, 1.0f},   // A, the bottom of the diamond
    {0.95f, 0.66f, 1.0f},    // B
    {0.85f, 0.66f, 1.0f},    // X
    {0.90f, 0.555f, 1.0f},   // Y
    {0.075f, 0.115f, 1.0f},  // LT
    {0.925f, 0.115f, 1.0f},  // RT
    {0.925f, 0.275f, 1.0f},  // START
    {0.075f, 0.045f, 1.0f},  // MENU
}};

// the same arrangement smaller and lower, for thumbs that want the screen's middle left alone
const Layout kCompact = {{
    {0.14f, 0.78f, 0.90f},   {0.90f, 0.775f, 0.95f}, {0.944f, 0.68f, 0.90f}, {0.856f, 0.68f, 0.90f},
    {0.90f, 0.585f, 0.90f},  {0.065f, 0.13f, 0.95f}, {0.935f, 0.13f, 0.95f}, {0.935f, 0.285f, 0.95f},
    {0.075f, 0.055f, 0.95f},
}};

// the diamond high on the right, the stick low on the left: the thumbs never cross
const Layout kSplit = {{
    {0.15f, 0.82f, 1.0f},    {0.88f, 0.50f, 1.0f},   {0.927f, 0.40f, 1.0f},  {0.833f, 0.40f, 1.0f},
    {0.88f, 0.30f, 1.0f},    {0.06f, 0.10f, 1.0f},   {0.94f, 0.10f, 1.0f},   {0.88f, 0.175f, 1.0f},
    {0.075f, 0.045f, 1.0f},
}};

const char* const kPresetIds[] = {"modern", "compact", "split"};
const Layout* const kPresets[] = {&kModern, &kCompact, &kSplit};

Layout g_layout = kModern;   // where the player put the controls
Layout g_shown = kModern;    // what is drawn: a floating stick follows the thumb
Preset g_preset = Preset::Modern;
uint32_t g_opacity = 55;
uint32_t g_scale = 100;
bool g_floating = true;
bool g_custom = false;
bool g_loaded = false;
bool g_editing = false;

struct Live {
    std::atomic<bool> pressed[kParts];
    std::atomic<float> move_x{0}, move_y{0};
    // camera travel since the last frame, in screen heights: a frame turns the camera by how far the finger went
    std::atomic<float> drag_x{0}, drag_y{0};
} g_live;

float g_last_touch = -100;

struct Finger {
    SDL_FingerID id = 0;
    int part = kNone;
    float x = 0, y = 0;    // normalized
    float ox = 0, oy = 0;  // where it came down: a floating stick's base
    bool active = false;
};
Finger g_fingers[10];

int FindFinger(SDL_FingerID id) {
    for (int i = 0; i < 10; i++)
        if (g_fingers[i].active && g_fingers[i].id == id) return i;
    return -1;
}

int FreeFinger() {
    for (int i = 0; i < 10; i++)
        if (!g_fingers[i].active) return i;
    return -1;
}

// ---- geometry in screen pixels

struct Pixels {
    float w = 1, h = 1;
};

Pixels Screen() {
    ImGuiIO& io = ImGui::GetIO();
    return {io.DisplaySize.x > 0 ? io.DisplaySize.x : 1, io.DisplaySize.y > 0 ? io.DisplaySize.y : 1};
}

float Unit(const Info& info, float scale) {
    return Screen().h * info.size * scale * (float(g_scale) / 100.0f);
}

ImVec2 Center(const Place& place) {
    Pixels p = Screen();
    return ImVec2(place.x * p.w, place.y * p.h);
}

ImVec2 PillHalf(const Info& info, float scale) {
    float u = Unit(info, scale);
    return ImVec2(u * 1.15f, u * 0.42f);
}

bool Hit(const Info& info, const ImVec2& at, const Place& place) {
    ImVec2 c = Center(place);
    float dx = at.x - c.x, dy = at.y - c.y;
    if (info.kind == Kind::Pill) {
        ImVec2 half = PillHalf(info, place.scale);
        return std::fabs(dx) <= half.x * 1.18f && std::fabs(dy) <= half.y * 1.40f;
    }
    float r = info.kind == Kind::Stick ? Unit(info, place.scale) * 1.15f : Unit(info, place.scale) * 0.78f;
    return dx * dx + dy * dy <= r * r;
}

// ---- drawing

ImU32 Fade(ImU32 color, float a) {
    return (color & 0x00FFFFFFu) | (ImU32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
}

void TextCentered(ImDrawList* dl, ImVec2 at, const char* text, ImU32 color, float size, bool big = false) {
    ImFont* font = big ? ui::BigFont() : ImGui::GetFont();
    size = std::min(size, 96.0f);
    ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    dl->AddText(font, size, ImVec2(at.x - t.x * 0.5f, at.y - t.y * 0.5f), color, text);
}

void DrawStick(ImDrawList* dl, const Info& info, const Place& place, float a, bool active) {
    ImVec2 c = Center(place);
    float r = Unit(info, place.scale);
    ImU32 ring = Fade(active ? info.color : IM_COL32(190, 190, 200, 255), a * (active ? 0.95f : 0.5f));
    dl->AddCircleFilled(c, r * 1.08f, Fade(IM_COL32(8, 8, 12, 255), a * 0.32f), 48);
    dl->AddCircle(c, r * 1.08f, ring, 48, 2.5f);
    dl->AddCircle(c, r * 0.55f, Fade(IM_COL32(200, 200, 210, 255), a * 0.16f), 40, 1.5f);
    ImVec2 knob(c.x + g_live.move_x.load() * r * 0.72f, c.y - g_live.move_y.load() * r * 0.72f);
    float kr = r * 0.44f;
    dl->AddCircleFilled(ImVec2(knob.x + 1.5f, knob.y + 2.5f), kr, Fade(IM_COL32(0, 0, 0, 255), a * 0.45f), 32);
    dl->AddCircleFilled(knob, kr, Fade(IM_COL32(34, 34, 40, 255), a * 0.92f), 32);
    dl->AddCircle(knob, kr, Fade(info.color, a * (active ? 0.95f : 0.5f)), 32, 2.5f);
    dl->AddCircleFilled(knob, kr * 0.26f, Fade(IM_COL32(220, 220, 230, 255), a * 0.5f), 16);
}

void DrawCircle(ImDrawList* dl, const Info& info, const Place& place, float a, bool pressed) {
    ImVec2 c = Center(place);
    float r = Unit(info, place.scale) * (pressed ? 1.05f : 1.0f);
    dl->AddCircleFilled(c, r * 1.22f, Fade(info.color, a * (pressed ? 0.30f : 0.10f)), 40);
    dl->AddCircleFilled(ImVec2(c.x + 1.5f, c.y + 3), r, Fade(IM_COL32(0, 0, 0, 255), a * 0.5f), 40);
    dl->AddCircleFilled(c, r, Fade(pressed ? IM_COL32(58, 58, 66, 255) : IM_COL32(20, 20, 24, 255), a * 0.9f), 40);
    dl->AddCircle(c, r, Fade(info.color, a * (pressed ? 1.0f : 0.85f)), 40, pressed ? 4.0f : 3.0f);
    TextCentered(dl, c, info.label, Fade(info.color, a * (pressed ? 1.0f : 0.9f)), r * 1.05f, true);
}

void DrawPill(ImDrawList* dl, const Info& info, const Place& place, float a, bool pressed) {
    ImVec2 c = Center(place);
    ImVec2 half = PillHalf(info, place.scale);
    if (pressed) {
        half.x *= 1.05f;
        half.y *= 1.05f;
    }
    ImVec2 lo(c.x - half.x, c.y - half.y), hi(c.x + half.x, c.y + half.y);
    float rounding = half.y * 0.85f;
    dl->AddRectFilled(ImVec2(lo.x, lo.y + 2.5f), ImVec2(hi.x, hi.y + 2.5f), Fade(IM_COL32(0, 0, 0, 255), a * 0.45f),
                      rounding);
    dl->AddRectFilled(lo, hi, Fade(pressed ? IM_COL32(56, 56, 64, 255) : IM_COL32(18, 18, 22, 255), a * 0.9f), rounding);
    dl->AddRect(lo, hi, Fade(info.color, a * (pressed ? 1.0f : 0.75f)), rounding, 0, pressed ? 3.5f : 2.5f);
    TextCentered(dl, c, info.label, Fade(info.color, a * (pressed ? 1.0f : 0.85f)), half.y * 0.95f, true);
}

// ---- the layout editor's bar

enum BarAction { kPresetAction, kScaleDown, kScaleUp, kAlphaDown, kAlphaUp, kStickAction, kResetAction, kDoneAction, kBarActions };

struct BarItem {
    float x0 = 0, x1 = 0;
    int action = -1;
    std::string text;
};

std::vector<BarItem> BarItems() {
    Pixels p = Screen();
    std::vector<BarItem> out;
    float x = p.w * 0.018f, gap = p.w * 0.010f;
    char buf[64];
    auto add = [&](const char* text, float width, int action) {
        BarItem item;
        item.x0 = x;
        item.x1 = x + width;
        item.action = action;
        item.text = text;
        out.push_back(item);
        x += width + gap;
    };
    add(kPresetNames[uint32_t(g_preset)], p.w * 0.115f, kPresetAction);
    add("-", p.w * 0.032f, kScaleDown);
    std::snprintf(buf, sizeof(buf), "SIZE %u", g_scale);
    add(buf, p.w * 0.085f, -1);
    add("+", p.w * 0.032f, kScaleUp);
    add("-", p.w * 0.032f, kAlphaDown);
    std::snprintf(buf, sizeof(buf), "ALPHA %u", g_opacity);
    add(buf, p.w * 0.100f, -1);
    add("+", p.w * 0.032f, kAlphaUp);
    add(g_floating ? "STICK FLOAT" : "STICK FIXED", p.w * 0.115f, kStickAction);
    add("RESET", p.w * 0.075f, kResetAction);
    BarItem done;
    done.x0 = p.w * 0.84f;
    done.x1 = p.w * 0.982f;
    done.action = kDoneAction;
    done.text = "DONE";
    out.push_back(done);
    return out;
}

void DrawEditor(ImDrawList* dl) {
    Pixels p = Screen();
    float bar = p.h * 0.155f;
    float fs = bar * 0.21f;
    float cy = bar * 0.40f;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(p.w, bar), IM_COL32(8, 8, 10, 236));
    dl->AddLine(ImVec2(0, bar), ImVec2(p.w, bar), IM_COL32(200, 44, 40, 210), 2.0f);
    for (const BarItem& item : BarItems()) {
        ImVec2 lo(item.x0, cy - fs * 0.95f), hi(item.x1, cy + fs * 0.95f);
        bool live = item.action >= 0;
        dl->AddRectFilled(lo, hi, live ? IM_COL32(32, 29, 33, 255) : IM_COL32(18, 17, 19, 255), 8);
        if (live) dl->AddRect(lo, hi, IM_COL32(200, 50, 45, 190), 8, 0, 2.0f);
        TextCentered(dl, ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f), item.text.c_str(),
                     IM_COL32(238, 234, 232, 255), fs);
    }
    TextCentered(dl, ImVec2(p.w * 0.5f, bar * 0.80f),
                 "Drag a control where your thumb wants it  -  DONE or BACK keeps the layout",
                 IM_COL32(176, 170, 170, 255), bar * 0.14f);
}

bool EditorTap(float nx, float ny) {
    Pixels p = Screen();
    float bar = p.h * 0.155f;
    float fs = bar * 0.21f, cy = bar * 0.40f;
    float my = ny * p.h, mx = nx * p.w;
    if (my > bar) return false;
    if (my < cy - fs * 0.95f || my > cy + fs * 0.95f) return false;
    for (const BarItem& item : BarItems()) {
        if (item.action < 0 || mx < item.x0 || mx > item.x1) continue;
        switch (item.action) {
            case kPresetAction: NextPreset(1); break;
            case kScaleDown: SetScale(std::max<uint32_t>(g_scale - 5, 60)); break;
            case kScaleUp: SetScale(std::min<uint32_t>(g_scale + 5, 160)); break;
            case kAlphaDown: SetOpacity(std::max<uint32_t>(g_opacity - 5, 10)); break;
            case kAlphaUp: SetOpacity(std::min<uint32_t>(g_opacity + 5, 100)); break;
            case kStickAction: SetFloatingStick(!g_floating); break;
            case kResetAction: ResetLayout(); break;
            case kDoneAction: EndEdit(); break;
            default: break;
        }
        return true;
    }
    return true;  // the bar takes every touch, so a control is never dragged from it
}

// A floating stick's base is where the thumb landed: the layout's place until then, so the menu and the editor still
// show a control in a place that makes sense.
void HomeShowing() { g_shown[kMove] = g_layout[kMove]; }

}  // namespace

const char* const kPresetNames[] = {"MODERN", "COMPACT", "SPLIT"};

std::filesystem::path File() { return app::ExeDir() / "touch.toml"; }

void Save() {
    toml::table t;
    t.insert("preset", kPresetIds[uint32_t(g_preset)]);
    t.insert("opacity", int64_t(g_opacity));
    t.insert("scale", int64_t(g_scale));
    t.insert("floating", g_floating);
    t.insert("custom", g_custom);
    toml::table controls;
    for (const Info& info : kInfo) {
        const Place& pl = g_layout[info.part];
        controls.insert(info.id, toml::array{pl.x, pl.y, pl.scale});
    }
    t.insert("controls", std::move(controls));
    std::ofstream f(File());
    if (f) f << t << "\n";
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    g_layout = *kPresets[uint32_t(g_preset)];
    try {
        toml::table t = toml::parse_file(File().string());
        std::string preset = t["preset"].value_or(std::string(kPresetIds[0]));
        for (uint32_t i = 0; i < uint32_t(Preset::Count); i++)
            if (preset == kPresetIds[i]) g_preset = Preset(i);
        g_layout = *kPresets[uint32_t(g_preset)];
        g_opacity = std::clamp<uint32_t>(t["opacity"].value_or(g_opacity), 10u, 100u);
        g_scale = std::clamp<uint32_t>(t["scale"].value_or(g_scale), 60u, 160u);
        g_floating = t["floating"].value_or(g_floating);
        g_custom = t["custom"].value_or(false);
        if (auto* controls = t["controls"].as_table()) {
            for (const Info& info : kInfo) {
                auto* arr = (*controls)[info.id].as_array();
                if (!arr || arr->size() != 3) continue;
                Place& pl = g_layout[info.part];
                pl.x = float((*arr)[0].value_or(double(pl.x)));
                pl.y = float((*arr)[1].value_or(double(pl.y)));
                pl.scale = float((*arr)[2].value_or(double(pl.scale)));
            }
            g_custom = true;
        }
        LOGI("touch: layout %s, alpha %u, size %u", kPresetNames[uint32_t(g_preset)], g_opacity, g_scale);
    } catch (...) {
        LOGI("touch: no touch.toml yet, the MODERN layout");
    }
    HomeShowing();
}

Preset CurrentPreset() { return g_preset; }

void SetPreset(Preset preset) {
    g_preset = preset;
    g_layout = *kPresets[uint32_t(preset)];
    g_custom = false;
    HomeShowing();
    Save();
}

void NextPreset(int step) {
    uint32_t n = uint32_t(Preset::Count);
    SetPreset(Preset((uint32_t(g_preset) + n + uint32_t(step)) % n));
}

bool Custom() { return g_custom; }
const char* PresetName() { return g_custom ? "CUSTOM" : kPresetNames[uint32_t(g_preset)]; }
uint32_t Opacity() { return g_opacity; }
void SetOpacity(uint32_t percent) {
    g_opacity = std::clamp<uint32_t>(percent, 10u, 100u);
    Save();
}
uint32_t Scale() { return g_scale; }
void SetScale(uint32_t percent) {
    g_scale = std::clamp<uint32_t>(percent, 60u, 160u);
    Save();
}
bool FloatingStick() { return g_floating; }
void SetFloatingStick(bool floating) {
    g_floating = floating;
    Save();
}
void ResetLayout() {
    g_layout = *kPresets[uint32_t(g_preset)];
    g_custom = false;
    HomeShowing();
    Save();
}
const char* FilterName() { return port::g_texture_filter ? "NEAREST" : "LINEAR"; }

bool Editing() { return g_editing; }

void BeginEdit() {
    g_editing = true;
    host::SetGameActive(false);
    for (Finger& f : g_fingers) f = {};
    HomeShowing();
    LOGI("touch: editing the layout");
}

void EndEdit() {
    g_editing = false;
    g_custom = true;
    host::SetGameActive(true);
    Save();
    for (Finger& f : g_fingers) f = {};
    LOGI("touch: layout saved");
}

void Init() {
    for (Finger& f : g_fingers) f = {};
    Load();
}

bool Shown() { return port::g_touch_controls && NowSeconds() - g_last_touch < 2.5f; }

// ---- input

namespace {

void Press(int part) {
    if (part < 0 || part >= kParts) return;
    if (!g_live.pressed[part].exchange(true)) host::HapticTap(16);
}

void Release(int part) {
    if (part < 0 || part >= kParts) return;
    g_live.pressed[part] = false;
}

int PartAt(float nx, float ny) {
    Pixels p = Screen();
    ImVec2 at(nx * p.w, ny * p.h);
    for (int i = kParts - 1; i >= 0; i--) {
        const Info& info = kInfo[i];
        if (info.kind == Kind::Stick) continue;
        if (Hit(info, at, g_layout[i])) return i;
    }
    if (g_floating) {
        // a floating stick comes up under the thumb, anywhere in its half below the shoulder buttons
        if (nx < 0.45f && ny > 0.32f) return kMove;
    } else if (Hit(kInfo[kMove], at, g_layout[kMove])) {
        return kMove;
    }
    // not a control: the finger drags the camera
    return kCamera;
}

void FollowStick(Finger& f) {
    const Info& info = InfoOf(kMove);
    Pixels p = Screen();
    float r = Unit(info, g_shown[kMove].scale) * 0.85f;
    float dx = (f.x - f.ox) * p.w, dy = (f.y - f.oy) * p.h;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > r) {
        dx *= r / len;
        dy *= r / len;
    }
    g_live.move_x = dx / r;
    g_live.move_y = -dy / r;
}

// The camera: the finger's travel since the last frame becomes the right stick's deflection for one frame, so the
// distance dragged is the angle turned, whatever the frame rate. A held-still finger turns nothing.
constexpr float kDragFullStick = 0.085f;  // of the screen's height in one frame for full deflection

void DragCamera(float from_x, float from_y, float to_x, float to_y) {
    Pixels p = Screen();
    float dx = (to_x - from_x) * p.w / p.h, dy = to_y - from_y;  // both in screen heights
    if (dx == 0 && dy == 0) return;
    g_live.drag_x.store(std::clamp(g_live.drag_x.load() + dx, -1.0f, 1.0f));
    g_live.drag_y.store(std::clamp(g_live.drag_y.load() + dy, -1.0f, 1.0f));
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    if (shadow::MenuOpen()) return false;  // ImGui has the screen
    if (!port::g_touch_controls && !g_editing) return false;
    Pixels p = Screen();
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        g_last_touch = NowSeconds();
        int slot = FreeFinger();
        if (slot < 0) return false;
        Finger& f = g_fingers[slot];
        f = {};
        f.id = e.tfinger.fingerID;
        f.x = std::clamp(e.tfinger.x, 0.0f, 1.0f);
        f.y = std::clamp(e.tfinger.y, 0.0f, 1.0f);
        f.ox = f.x;
        f.oy = f.y;
        f.active = true;
        if (g_editing) {
            if (EditorTap(f.x, f.y)) {
                f.active = false;
                return true;
            }
            int part = kNone;
            for (int i = kParts - 1; i >= 0; i--)
                if (Hit(kInfo[i], ImVec2(f.x * p.w, f.y * p.h), g_layout[i])) part = i;
            f.part = part;
            return true;
        }
        if (Hit(InfoOf(kMenu), ImVec2(f.x * p.w, f.y * p.h), g_layout[kMenu])) {
            f.part = kMenu;
            Press(kMenu);
            return true;
        }
        int part = PartAt(f.x, f.y);
        f.part = part;
        if (part == kMove) {
            if (g_floating) g_shown[kMove] = Place{f.x, f.y, g_layout[kMove].scale};
            f.ox = g_shown[kMove].x;
            f.oy = g_shown[kMove].y;
        } else if (part != kCamera && part != kNone) {
            Press(part);
        }
    } else if (e.type == SDL_EVENT_FINGER_MOTION) {
        int slot = FindFinger(e.tfinger.fingerID);
        if (slot < 0) return false;
        Finger& f = g_fingers[slot];
        float was_x = f.x, was_y = f.y;
        f.x = std::clamp(e.tfinger.x, 0.0f, 1.0f);
        f.y = std::clamp(e.tfinger.y, 0.0f, 1.0f);
        g_last_touch = NowSeconds();
        if (g_editing) {
            if (f.part == kNone) return true;
            Place& pl = g_layout[f.part];
            pl.x = std::clamp(f.x, 0.02f, 0.98f);
            pl.y = std::clamp(f.y, 0.19f, 0.98f);
            g_shown[f.part] = pl;
            g_custom = true;
            return true;
        }
        if (f.part == kMove) FollowStick(f);
        else if (f.part == kCamera) DragCamera(was_x, was_y, f.x, f.y);
    } else if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        int slot = FindFinger(e.tfinger.fingerID);
        if (slot < 0) return false;
        Finger& f = g_fingers[slot];
        if (g_editing) {
            f = {};
            return true;
        }
        if (f.part == kMove) {
            g_live.move_x = 0;
            g_live.move_y = 0;
            HomeShowing();
        } else if (f.part == kMenu) {
            Release(kMenu);
            shadow::OpenMenu();
        } else if (f.part != kNone) {
            Release(f.part);
        }
        f = {};
    }
    return true;
}

void Pump() {}

// ---- into the console controller

void Apply(int port, input::Pad& pad) {
    if (port != 0) return;  // the touch controls are the first controller's
    // taken every frame, so travel does not pile up into a jump while the menu or the editor is up
    float drag_x = g_live.drag_x.exchange(0), drag_y = g_live.drag_y.exchange(0);
    if (!port::g_touch_controls || shadow::MenuOpen() || g_editing) return;
    // the console keeps a controller plugged into port 1; without this the game pauses as if it was pulled out
    pad.connected = true;
    float mx = g_live.move_x.load(), my = g_live.move_y.load();
    if (mx * mx + my * my > 0.02f) {
        pad.lx = int16_t(std::clamp(mx, -1.0f, 1.0f) * 32767.0f);
        pad.ly = int16_t(std::clamp(my, -1.0f, 1.0f) * 32767.0f);
    }
    float sens = float(port::g_camera_speed) / 100.0f;
    float rx = std::clamp(drag_x / kDragFullStick * sens, -1.0f, 1.0f);
    float ry = std::clamp(-drag_y / kDragFullStick * sens, -1.0f, 1.0f);
    if (rx * rx + ry * ry > 0.0004f) {
        pad.rx = int16_t(rx * 32767.0f);
        pad.ry = int16_t(ry * 32767.0f);
    }
    auto press = [&](int part, int analog) {
        if (g_live.pressed[part].load()) pad.analog[analog] = 0xFF;
    };
    press(kA, 0);
    press(kB, 1);
    press(kX, 2);
    press(kY, 3);
    press(kLt, 6);
    press(kRt, 7);
    if (g_live.pressed[kStart].load()) pad.buttons |= 1u << 4;
}

// ---- once a frame, inside the ImGui frame

void Draw() {
    if (!ImGui::GetCurrentContext() || shadow::MenuOpen()) return;
    if (!port::g_touch_controls && !g_editing) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (g_editing) {
        for (const Info& info : kInfo) {
            Place pl = g_shown[info.part];
            if (info.kind == Kind::Stick) DrawStick(dl, info, pl, 0.95f, true);
            else if (info.kind == Kind::Pill) DrawPill(dl, info, pl, 0.95f, false);
            else DrawCircle(dl, info, pl, 0.95f, false);
        }
        DrawEditor(dl);
        return;
    }
    float idle = NowSeconds() - g_last_touch;
    float alpha = float(g_opacity) / 100.0f * (idle > 4.0f ? 0.55f : 1.0f);
    bool moving = std::fabs(g_live.move_x.load()) + std::fabs(g_live.move_y.load()) > 0.01f;
    DrawStick(dl, kInfo[kMove], g_shown[kMove], alpha, moving);
    for (int i = kA; i <= kStart; i++) {
        const Info& info = kInfo[i];
        bool pressed = g_live.pressed[i].load();
        if (info.kind == Kind::Pill) DrawPill(dl, info, g_shown[i], alpha, pressed);
        else DrawCircle(dl, info, g_shown[i], alpha, pressed);
    }
    DrawPill(dl, InfoOf(kMenu), g_shown[kMenu], 0.7f, g_live.pressed[kMenu].load());
}

}  // namespace touch
