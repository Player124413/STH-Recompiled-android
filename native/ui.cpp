// ImGui on Android: SDL's fingers, mouse and keys into ImGui's IO (main thread, under app::UiLock), the first
// controller for the menu's navigation, and the port's dark red look.
#include <SDL3/SDL.h>

#include <cfloat>
#include <chrono>

#include <imgui.h>

#include <algorithm>

#include "host.h"

namespace ui {

namespace {

ImGuiKey KeyOf(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

int MouseButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_RIGHT: return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default: return -1;
    }
}

SDL_FingerID g_finger = 0;  // the finger that is ImGui's mouse
bool g_fingered = false;

}  // namespace

void SetStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10;
    s.ChildRounding = 8;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 8;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(18, 16);
    s.FramePadding = ImVec2(14, 10);
    s.ItemSpacing = ImVec2(12, 10);
    s.ScrollbarSize = 10;
    s.GrabMinSize = 22;
    ImVec4* c = s.Colors;
    const ImVec4 red(0.86f, 0.16f, 0.13f, 1.0f), red_dim(0.55f, 0.10f, 0.09f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.92f, 0.91f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.53f, 0.53f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.05f, 0.06f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.085f, 0.10f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.075f, 0.09f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.28f, 0.10f, 0.10f, 0.80f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.13f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.32f, 0.12f, 0.12f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.13f, 0.12f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.05f, 0.06f, 1.0f);
    c[ImGuiCol_TitleBgActive] = red_dim;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.15f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonHovered] = red_dim;
    c[ImGuiCol_ButtonActive] = red;
    c[ImGuiCol_Header] = red_dim;
    c[ImGuiCol_HeaderHovered] = red;
    c[ImGuiCol_HeaderActive] = red;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.32f, 0.27f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.36f, 0.30f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.12f, 0.12f, 0.9f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = red_dim;
    c[ImGuiCol_NavHighlight] = red;
}

namespace {
ImFont* g_ui = nullptr;
ImFont* g_big = nullptr;
const char* const kFonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoCondensed-Regular.ttf",
                              "/system/fonts/DroidSans.ttf"};
}  // namespace

ImFont* BigFont() { return g_big ? g_big : ImGui::GetIO().Fonts->Fonts.empty() ? ImGui::GetFont() : ImGui::GetIO().Fonts->Fonts[0]; }
ImFont* UiFont() { return g_ui ? g_ui : ImGui::GetFont(); }

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    int w = 0, h = 0;
    host::WindowPixels(w, h);
    float ui = std::clamp(float(h > 0 ? h : 1080) * 0.028f, 20.0f, 34.0f);
    for (const char* path : kFonts) {
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(path, ui)) {
            g_ui = f;
            break;
        }
    }
    if (!g_ui) g_ui = io.Fonts->AddFontDefault();
    io.FontDefault = g_ui;
    for (const char* path : kFonts) {
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(path, 64.0f)) {
            g_big = f;
            break;
        }
    }
    if (!g_big) g_big = g_ui;
}

void OnEvent(const SDL_Event& e) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    int w = 1, h = 1;
    host::WindowPixels(w, h);
    switch (e.type) {
        case SDL_EVENT_FINGER_DOWN:
            if (g_fingered) break;
            g_finger = e.tfinger.fingerID;
            g_fingered = true;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            io.AddMouseButtonEvent(0, true);
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            g_fingered = false;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // a lifted finger hovers nothing
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.motion.x, e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
            if (int b = MouseButton(e.button.button); b >= 0) {
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMouseButtonEvent(b, e.button.down);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
            if (ImGuiKey k = KeyOf(e.key.scancode); k != ImGuiKey_None) io.AddKeyEvent(k, e.key.down);
            break;
        case SDL_EVENT_TEXT_INPUT:
            io.AddInputCharactersUTF8(e.text.text);
            break;
        default:
            break;
    }
}

void NewFrame(float width, float height) {
    ImGuiIO& io = ImGui::GetIO();
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    auto now = clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60;
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    // the first controller steps through the menu
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* pad = count ? SDL_GetGamepadFromID(ids[0]) : nullptr;
    SDL_free(ids);
    if (!pad) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    constexpr Map kMap[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},        {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},     {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},      {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},     {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},   {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    };
    for (const Map& m : kMap) io.AddKeyEvent(m.key, SDL_GetGamepadButton(pad, m.button));
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, SDL_GamepadAxis a) {
        float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
        io.AddKeyAnalogEvent(neg, v < -0.3f, v < -0.3f ? -v : 0);
        io.AddKeyAnalogEvent(pos, v > 0.3f, v > 0.3f ? v : 0);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
}

}  // namespace ui
