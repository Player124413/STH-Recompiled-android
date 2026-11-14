// What the runtime asks of the game on Android: no window messages (there is no Win32), the touch controls and the
// port's menu in place of the keyboard and the PC settings screen. Strings must be set; hooks may stay null.
#include "app/app.h"
#include "common/pc_text.h"
#include "common/render_ext.h"
#include "core/input.h"
#include "host.h"

namespace shadow {
extern bool g_skip_cutscenes;  // src/game/hooks.cpp
extern bool g_cutscene_60;     // src/game/hooks.cpp
extern bool g_widescreen;      // src/game/hooks.cpp
extern bool g_show_fps;        // native/menu.cpp
void ApplyWidescreen();        // src/game/hooks.cpp
}  // namespace shadow

const app::GameInfo app::kGame = {
    .title = L"Shadow the Hedgehog",
    .short_title = L"Shadow",
    .name = L"ShadowAndroid",
    .log = "shadow",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put the game's disc image (.iso) in the Shadow folder, or import it from the launcher.",
    .images = {{"default.xbe", "Shadow"}},
    .options = {{"skip_cutscenes", &shadow::g_skip_cutscenes, nullptr},
                {"cutscene_60fps", &shadow::g_cutscene_60, nullptr},
                {"hd_scene", &port::g_hd_scene, nullptr},
                {"show_fps", &shadow::g_show_fps, nullptr},
                {"widescreen", &shadow::g_widescreen, nullptr},
                {"touch_controls", &port::g_touch_controls, nullptr},
                {"dynamic_resolution", &port::g_dynamic_res, nullptr},
                {"vibration", &port::g_vibration, nullptr},
                {"texture_filter", nullptr, &port::g_texture_filter},
                {"dynamic_rendering", nullptr, &port::g_dynamic_rendering},
                {"timeline_semaphores", nullptr, &port::g_timeline_semaphores},
        {"safe_graphics", &port::g_safe_graphics, nullptr},
                {"min_render_percent", nullptr, &port::g_min_percent},
                {"camera_speed", nullptr, &port::g_camera_speed}},
    .load = port::Load,
    .save = port::Save,
    .init = [] { shadow::InitPcText(); shadow::ApplyWidescreen(); port::Init(); },
    .loaded = nullptr,
    .window = nullptr,  // main.cpp starts the touch controls once ImGui exists
    .message = nullptr,
    .ui = [](double fps) { shadow::ApplyWidescreen(); shadow::MenuUi(fps); },
    .pad = [](int port, input::Pad& pad) {
        touch::Apply(port, pad);  // the touch controls are port 0's, under a real controller's
        if (shadow::MenuPad(port, pad)) {
            // the menu has the input this frame and has set the pad itself; the plug below still goes on
        } else if (touch::Editing()) {  // the layout editor has it too
            bool c = pad.connected;
            pad = input::Pad{};
            pad.connected = c;
        }
        // A controller is plugged into port 1 on the console and never gets pulled out. The touch controls already say so
        // while they are up, but the port's own menu takes the input, and a phone whose pad dropped or whose touch
        // controls are off said "nothing plugged in" -- and the game answers that with its "Please reconnect the
        // controller" message, pauses, and leaves the frame it was in the middle of drawing on the screen for as long as
        // the message is up. That is what a paused frame with stale pieces in it looks like.
        if (port >= 0) pad.connected = true;
    },
    .rumble = nullptr,
};
