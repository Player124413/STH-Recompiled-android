#pragma once
// What the Android host's parts share: the window, the app's lifecycle, the port's own settings and the touch
// controls. The game itself is described by app::kGame (native/game_info.cpp).
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ImFont;
union SDL_Event;
struct SDL_Window;

namespace input {
struct Pad;
}

namespace host {

SDL_Window* Window();
// the app is in front with a surface to draw to; the game waits while it is not
bool Foreground();
// counts the native windows Android gave the app: the renderer makes its surface again when this changes
uint32_t SurfaceEpoch();
// the game thread, before a present: returns once the app is in front again
void WaitForeground();
// the window's size in pixels
void WindowPixels(int& w, int& h);
// the touch controls disabled, the in-game menu closed: the game gets its input
void SetGameActive(bool active);
bool GameActive();
// the app closes and the launcher comes back (the menu's EXIT TO LAUNCHER)
void ExitToLauncher();
void GraphicsReport();  // what the frames so far asked of the GPU, into the log
void DumpFrame();       // the frame's render targets as PNG files under dumps/, for a picture that comes out wrong
std::string GraphicsInfo();  // the same facts as the report, as text for the menu's ABOUT page
void CheckForUpdates();  // the newest release on GitHub, with "update now" or "later"
void ShowAbout();  // the port's credits, English and Russian
void CopyLog();   // the log's tail onto the clipboard
void ShareLog();  // the log's tail to another app
// a short vibration, from the touch controls (the VIBRATION setting)
void HapticTap(int ms);

}  // namespace host

// controllers through SDL (native/input.cpp): the plug events come from the main thread's loop, and a controller
// Android only passes on as key codes and axes plays port 0
namespace input {
void Init();
void OnPadEvent(const SDL_Event& e);
void LooseKey(int keyCode, bool down);
void LooseAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx, float hy);
void LooseRelease();
}  // namespace input

// the audio output (native/audio.cpp): paused while the app is in the background
namespace port {
void PauseAudio(bool paused);
}  // namespace port

// the port's settings, saved in settings.toml beside the saves (native/options.cpp)
namespace port {

// graphics
extern bool g_hd_scene;        // the 512x512 3D scene target gets 640/512 more scale, so stages are sharp
extern bool g_dynamic_res;     // the render resolution follows the frame rate
extern uint32_t g_min_percent; // dynamic resolution's floor, percent of the screen's height
extern uint32_t g_texture_filter;  // 0 linear, 1 point
// 2 automatic (render passes on ARM Mali), 1 dynamic rendering, 0 render passes; read when the device is made
extern uint32_t g_dynamic_rendering;
// 2 automatic (fences on ARM Mali), 1 timeline semaphores, 0 fences; read when the device is made
extern uint32_t g_timeline_semaphores;
extern bool g_safe_graphics;

// touch controls
extern bool g_touch_controls;
extern bool g_vibration;
extern uint32_t g_camera_speed;  // percent, how far a drag on the screen turns the camera

void Load(const void* table);  // settings.toml, for what app::kGame.options does not hold
void Save(void* table);
void Init();  // after the settings load, before the game boots
const char* VersionName();  // the port's version, from CMake

}  // namespace port

// The render resolution following the frame rate (native/dynamic_resolution.cpp): percent of the screen's height.
namespace dynres {
uint32_t Percent();  // the render resolution in use; app::render_percent while it is off
void Frame();        // render thread, each present
void Reset();
}  // namespace dynres

// The touch controls, their layout editor and the button look (native/touch.cpp).
namespace touch {

enum class Preset : uint32_t { Modern, Compact, Split, Count };  // the three starting layouts
extern const char* const kPresetNames[];

// main thread
void Init();
bool OnEvent(const SDL_Event& e);  // true when the controls took the event
void Pump();
// the touch controls' buttons and sticks onto port 0's pad, before the game reads it
void Apply(int port, input::Pad& pad);
// inside the ImGui frame: the controls over the picture
void Draw();
// in play with a finger on the screen recently
bool Shown();
bool Editing();
void BeginEdit();
void EndEdit();
void Save();  // touch.toml
void Load();

Preset CurrentPreset();
void SetPreset(Preset preset);
void NextPreset(int step);
bool Custom();
const char* PresetName();
uint32_t Opacity();  // percent
void SetOpacity(uint32_t percent);
uint32_t Scale();  // percent of each control's own size
void SetScale(uint32_t percent);
bool FloatingStick();
void SetFloatingStick(bool floating);
void ResetLayout();
const char* FilterName();  // NEAREST or LINEAR, the renderer's sampler

}  // namespace touch

// The in-game port menu (native/menu.cpp), drawn with ImGui over the frame.
namespace shadow {
extern std::atomic<bool> g_menu_open;  // the game gets no input while it is open
void MenuUi(double fps);
bool MenuPad(int port, input::Pad& pad);  // true when the menu owns this frame's pad
void MenuKey(int key);            // SDL key code, for Android's back button and the keyboard
void OpenMenu();
void CloseMenu();
void ToggleMenu();
bool MenuOpen();
}  // namespace shadow

// ImGui's input from SDL's events (native/ui.cpp), main thread, under app::UiLock
namespace ui {
void OnEvent(const SDL_Event& e);
void NewFrame(float width, float height);
void SetStyle();  // the port's dark red look, at ImGui's context creation
void LoadFonts();  // Roboto from the system, the UI font and a big one for the touch controls' letters
ImFont* BigFont();
ImFont* UiFont();
}  // namespace ui

// The game's own Android-side bits that the shared PC files do not carry.
namespace shadow {
void InitPcText();   // src/common/pc_text.cpp: PC wording for the save messages
void ApplyWidescreen();  // src/game/hooks.cpp
extern bool g_widescreen;
}

namespace port {
void ResetDefaults();  // the settings as a first run has them
}
