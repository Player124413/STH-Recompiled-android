# Shadow the Hedgehog on Android

An unofficial Android port of the original Xbox game, on the same static recompilation the PC build uses: the game's
own x86 code is recompiled to ARM64 and runs as native code, drawn with Vulkan 1.1 through SDL3. No game code and no
game files are shipped — the app is built from *your* disc image, and the phone reads it at runtime.

This folder (`android/`) is the whole Android project: the Gradle files, the activity, the game importer, and the
CMake build that links the SDK's portable runtime, the recompiled game code and the port's own host code in `native/`.

## What the phone gets

Everything the PC build has, in a phone's shape:

* **60 FPS with frame pacing**, dynamic resolution (scales the render resolution to hold the target), V-Sync as an
  option, an on-screen FPS counter.
* **The full picture**: 16:9 Hor+ widescreen with the widescreen 2D layer squeezed back to its own shape, the HD 3D
  world (512×512 render targets), anisotropic filtering, the HD texture/menu fonts from `src/common/pc_text.cpp`.
* **Cutscene skip** and **cutscenes at 60 FPS**, as the PC build's hooks.
* **Touch controls**: two sticks, A/B/X/Y, BLACK/WHITE, LT/RT, START/BACK and a MENU button — floating or fixed
  sticks, adjustable size and opacity, three presets (Modern, Compact, Split) and a layout editor you drag controls
  around in. The layout is `touch.toml` beside `settings.toml`.
* **Controllers**: every pad SDL3 knows (Xbox, DualSense, Switch Pro, generic HID) plays as it does on the console,
  with rumble. Controllers Android hands the app as a "virtual device" SDL ignores still play: the activity forwards
  their keys and axes to port 0.
* **The port menu**: `DISPLAY` / `GRAPHICS` / `CONTROLS` / `GAME` / `ABOUT`, in the port's dark red look, opened with the
  on-screen MENU button, Android's back gesture, or BACK + START on a pad. Changes are saved to `settings.toml` as they
  are made; the game's own pause comes up behind it (the menu taps START for the game).
* **Quit to launcher** from the menu.
* Saves, settings, caches and logs beside the app's own files, so nothing needs a permission to write.

## Where the game goes

The app reads the game from, in order:

1. its own folder on the phone: `Android/data/com.sthrecompiled.shadow/files/assets/` (used by the importer),
2. `/storage/emulated/0/Shadow/`,
3. `Shadow/` at the top of any SD card or USB stick.

Each of those may hold either the game's own files (`default.xbe` and the rest, as the PC build's `assets/` has them)
or a **single disc image** (`.iso`). The app checks the image's SHA-1: it must be the USA release the recompiled code
was made from.

Reading `/storage/emulated/0/Shadow` needs *all files access*, a special access Android grants on a settings page of its
own. The activity asks for it at launch and explains where the game goes; the game itself waits for the files and
starts as soon as it finds them, so importing the disc image from the app (IMPORT A DISC IMAGE) works without
restarting the app.

## Building

Requirements: a PC with the Android toolchain (JDK 17–21, Android SDK 34, NDK `28.2.13676358`, CMake `3.31.6`,
Gradle 8.7 through the wrapper), plus a phone or tablet with arm64 and Vulkan 1.1.

1. **Recompile the game once on a PC**, as the PC build does (`build.bat` on Windows): it writes `config/game.toml`,
   `assets/default.xbe` and the recompiled code in `generated/game`. This step needs the Windows toolchain the SDK's
   recompiler uses; the Android build takes its output as it is.
2. **Build the APK**:

   ```
   cd android
   ../gradlew assembleDebug        # or assembleRelease
   ```

   Gradle clones SDL3 (`android/third_party/SDL`) and CMake fetches glslang, toml++ and ImGui. To build from a
   recompiled tree somewhere else, pass `-PxbeGenerated=<dir>` (and `-PxbeImage=<default.xbe>`).
3. Install `android/app/build/outputs/apk/debug/app-debug.apk` and copy or import the game.

### Building it on GitHub's runners

`.github/workflows/android.yml` builds the APK without a PC set up for it. Every push that touches `android/`,
`native/`, `src/` or `sdk/` runs it, and it can also be run by hand (*Actions → Android APK → Run workflow*).

* **Without the game** it builds a *shell* APK: `tools/android/make_stub_game.py` writes a stand-in for
  `generated/game`, so the toolchain, the SDK's runtime, the renderer, ImGui and the Java side all get compiled and
  linked. The APK installs and starts, and stops at "the game is not here" — it proves the port builds, not that it
  plays.
* **With the game** give the run `iso_url`, a direct link to your own disc image (or set the repository variable
  `SHADOW_ISO_URL`). The runner downloads it, reads `default.xbe` out of it with `tools/host/xiso.py`, builds the SDK's
  recompiler on Linux (`tools/android/xbe_recomp`, no Windows needed), recompiles the game, builds, and keeps the APK
  as an artifact for seven days. The image itself is deleted with the runner and never cached or committed.

The APK comes back under the run's *Artifacts*. A failed build leaves a check run named `ci-report` holding the
compiler's errors, which is the quickest place to read why.

## The project's files

| File | What it is |
| --- | --- |
| `CMakeLists.txt` | libmain.so: the SDK's portable runtime (minus its Windows parts), the recompiled code, `src/game/hooks.cpp`, `src/common/render_ext.cpp`, `src/common/pc_text.cpp`, every `native/*.cpp`, glslang for the shaders, and ImGui's Vulkan backend |
| `app/build.gradle` | the APK: SDK 34, NDK 28.2, arm64 only, minSdk 29, SDL3 cloned and built as `libSDL3.so` |
| `app/src/main/AndroidManifest.xml` | Vulkan 1.1 required, gamepad and vibration optional, all files access for the game folder |
| `app/src/main/java/.../MainActivity.java` | SDL's activity: storage access and where the game goes, the loose-controller forwarding, quit to launcher, haptics |
| `.../GameImporter.java` | copies a picked disc image into the app's own folder, checking for the Xbox volume descriptor first |
| `.../NativeBridge.java` | the native calls the activity makes for the forwarded controller |

The port's own side (`native/`, one folder up) is shared with nothing else: `main.cpp` is the Android entry point and
event loop, `app.cpp` the settings/folders/pacing host, `vulkan.cpp` the renderer, `touch.cpp` the controls, `menu.cpp`
the port menu, `audio.cpp` the SDL3 audio device, `input.cpp` the pads and `dynamic_resolution.cpp` the resolution
scaling.

## Known differences from the PC build

* Texture replacement and texture dumping are off: a phone has neither the files nor the memory for it, and the
  settings are not offered in the menu.
* Point sprites are drawn one pixel wide (they are in the PC build's Vulkan path too, which this renderer comes from).
* No system link yet: the SDK's networking is portable, but nothing has been tested over a phone's Wi-Fi.
* No splash, no relaunch into another XBE (an Android app has no second executable).

## Credits and licences

The renderer, the SDK and the port's structure come from the **XboxXBE-Recomp / Recompiled** project (MIT), and the
Vulkan backend, touch controls and Android host were adapted from **Rangga Casper's Black Android port** (MIT, © 2026
Rangga Casper). Both notices are kept in `LICENSE` files beside their code. *Shadow the Hedgehog* is a trademark of
SEGA; this project ships no SEGA code and no SEGA assets, and is not affiliated with SEGA.
