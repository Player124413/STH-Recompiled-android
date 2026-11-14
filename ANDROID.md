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
* **Touch controls**: the console's own buttons and nothing else — A/B/X/Y in the Xbox diamond (Y above, X left,
  B right, A below), LT/RT, START and the port's MENU button, plus a move stick that is floating or fixed. **The
  camera is the screen**: drag anywhere that is not a button. The turn follows how fast the finger moves rather than
  how far, so a drag turns the same amount at any frame rate; a few pixels of travel are ignored first, so tapping a
  button never jolts the view, and the deflection is eased in and out instead of snapping. CAMERA SPEED (200% out of
  the box, adjustable 50–400%) is the whole of it. Buttons can be resized, faded, moved with the layout editor, and
  three presets (Modern, Compact, Split) give a starting point; the layout is `touch.toml` beside `settings.toml`, and
  what is drawn is exactly what is saved.
* **Controllers**: every pad SDL3 knows (Xbox, DualSense, Switch Pro, generic HID) plays as it does on the console,
  with rumble. Controllers Android hands the app as a "virtual device" SDL ignores still play: the activity forwards
  their keys and axes to port 0.
* **The port menu**: `DISPLAY` / `GRAPHICS` / `CONTROLS` / `GAME` / `ABOUT`, in the port's dark red look, opened with the
  on-screen MENU button, Android's back gesture, or BACK + START on a pad. Changes are saved to `settings.toml` as they
  are made; the game's own pause comes up behind it (the menu taps START for the game).
* **Quit to launcher** from the menu.
* **Check for updates**: asks GitHub for the port's newest release and puts the choice up — *Update now* downloads the
  APK the release carries and hands it to Android's installer (the same signing key, so it installs over this copy and
  keeps the settings and the imported image), *Later* closes it. Nothing is checked at launch. A release without an
  APK attached opens its page instead, and a repository with no releases says so. Reachable from the menu's ABOUT page
  and from the about dialog.
* **Who made it**: an "About this port" dialog — the port's author, the Telegram channel and the Black Android port it
  was modelled on, in English and in Russian, with buttons that open both links. It comes up once at the first launch
  and stays a tap away on the menu's ABOUT page.
* **Dynamic rendering**: the backend draws through `vkCmdBeginRendering` where the driver has it and through render
  passes where it does not. **OFF is the default**: render passes draw the scene's effects right, where dynamic
  rendering showed them as hard-edged wedges of the picture (a Mali-G57 with Vulkan 1.3; an Adreno 710 with Vulkan 1.1
  never had the choice and was always right). GRAPHICS → DYNAMIC RENDERING switches it: OFF (RENDER PASSES), ON, and
  AUTOMATIC, which picks render passes on ARM Mali and dynamic rendering elsewhere; the choice is taken at the next
  start.
* **Vulkan level**: the port asks for Vulkan 1.1 and nothing more, unless DYNAMIC RENDERING = ON or GPU SYNC = TIMELINE
  asks for it: the phone whose picture is right is a 1.1 device, and every 1.2/1.3 path has drawn a wrong frame on a
  phone that reports 1.3.
* **GPU sync**: timeline semaphores where the driver has them, fences where it does not, and on ARM Mali fences by
  default — the phone whose picture is right runs render passes *and* fences, so the port matches it on both counts.
  GRAPHICS → GPU SYNC switches AUTOMATIC, TIMELINE, FENCES and STRICT, taken at the next start. STRICT stops the CPU at
  every submit: a frame still wrong under it is wrong in what the port asked the GPU to draw, not in when the driver got
  round to it, which is how a sync hole is told apart from bad geometry data. On that Mali driver the
  timeline path was the bug behind a scene with no geometry in it: the "the GPU is past this" value came back wrong, the
  CPU refilled the vertex ring while the GPU was still reading it, and the frame came out as fog with one smeared corner.
  Fences fixed it, which is why AUTOMATIC means fences on every device now: a fence per submit is cheap next to a frame
  with no scene in it. TIMELINE is still there to compare, at the next start.
* **Shader caches are dropped when the shaders change**: the SPIR-V cache uses one file per module named after the
  build's shader hash *and* the SPIR-V version it was translated to, so a phone that has been updated across shader
  changes never runs an old module, and the driver's own pipeline cache (`shadercache/pipelines.bin`) is deleted when
  the version stamp beside it no longer matches — a driver cache that outlives the shaders it was built from is a good
  way to keep a fixed bug on screen.
* **Send a frame**: GRAPHICS → SEND A FRAME TO THE DEV writes the frame's render targets into `dumps/` beside the settings, the
  largest first and then hands them to the share sheet, so a picture that comes out wrong can be sent from the phone in
  one tap and looked at pass by pass instead of guessed at from a screenshot. The depth targets go with them as grey,
  near bright, because a vertex that went astray shows its shape in the depth before anywhere else.
* **DXT is always decoded by the port**, never by the GPU: no phone this port has run on can decode BC, so the driver's
  own path has never been seen to draw a correct picture, while the CPU decoder was checked texel by texel against a
  reference one. A phone that offers BC (an RDNA2 based one does) would otherwise be the only device on untested code,
  and a texture unpacked wrongly is what smears and checkerboards look like.
* **The controller is always plugged in**: port 1 never says "nothing connected", not while the port's own menu has the
  input, not after a pad drops, not with the touch controls off. The game answers an empty port with its "Please
  reconnect the controller" message, pauses, and leaves the frame it was drawing on the screen - which is what a picture
  with stale pieces of earlier frames in it actually is.
* **Vertex fetch**: with safe graphics on, every vertex is converted to float4 on the CPU and the driver fetches no host
  formats at all. Safe graphics off restores the faster path, where the guest's own buffers are read by the GPU - which is
  also the path a driver can get wrong, and a model drawn stretched over the screen is what that looks like.
* **Safe graphics**: on by default, and one toggle on the GRAPHICS page for a phone whose picture comes out wrong. Applied as it is
  pressed: the frame is drawn at the console's own size (no render resolution, no HD scene), the glow composite and the
  extra upscale taps are skipped, dynamic resolution stands still and DXT is unpacked here instead of in the driver. It
  is the first thing to try, and every one of those five also has a switch of its own to bisect it.
* **Widescreen**: on means the picture fills the phone's screen with the game's camera widened for it; off means the
  console's own 4:3, whole, with black bars beside it - not the same stretch again without the wider camera.
* **This phone**: ABOUT prints the same facts the report logs - GPU, Vulkan version, driver, sync path, dynamic
  rendering, depth format, draw scale, swapchain, texture memory, draw counters - so one screenshot of that page compares
  two phones without a log file.
* **Graphics report**: GRAPHICS REPORT on the menu's GRAPHICS page writes what the frames so far asked of the GPU into
  the log — draws, point and point-sprite draws, reads of a render target while drawing into it, wide taps, targets
  made, texture uploads, ring flushes, pipelines — with the device's own capabilities beside it (BC/ASTC/ETC2, point
  size range, depth format, dynamic rendering, render scale). It also lands at the first frames and once the title is
  running, so a picture that is wrong on one driver and not another can be told apart without a debugger.
* **The log in one tap**: `COPY LOG` on the menu's ABOUT page puts the last 3000 lines on the clipboard, `SEND LOG` hands
  them to any app; the launcher's "where the game goes" dialog has `COPY LOG` too, for when the game never starts.
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
3. Install `android/app/build/outputs/apk/release/app-release.apk` and copy or import the game.

### Updating without uninstalling

Every build — CI's and a PC's — is signed with the same key, `android/keystore/shadow.keystore`, and the workflow
gives each build a higher version code (the run number), so a new APK installs straight over the last one and keeps the
game files, saves and settings. The key is in the repository on purpose: it is a sideload identity, not a store one,
and keeping it the same is what makes updates work. To sign with a key of your own instead, set `SHADOW_KEYSTORE` (a
keystore path) and `SHADOW_KEYSTORE_PASSWORD`, `SHADOW_KEY_ALIAS`, `SHADOW_KEY_PASSWORD`; every build after that switch
needs one last uninstall, and then updates work again.

Builds made before this (signed with a throwaway debug key) cannot be updated over — one last uninstall of those.

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

## What needs the game recompiled

The port's Android host is in the APK, but the game's own fixes — cutscenes at 60 FPS and skippable on the first
playthrough, true widescreen, the frame pacing, the CPU lock — are hooks the recompiler plants in the recompiled
code, and they come from `config/game.toml`. Recompiling the game (step 1 above) applies them; building only the APK
does not. A hook that does not fit an instruction is reported by the recompiler by name and address, and the build
stops, so a wrong one never ships silently.

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

## A phone whose picture comes out deformed, and what other ports found

One report stands out from every other: on a **Unisoc T6xx with a Mali-G57** (Vulkan 1.3.315, driver 0D801000) a
character's model is drawn stretched into a long sheet, while every other kind of draw is right. The same APK on an
Adreno 710 draws it correctly, the game files are byte-identical, and the deformation is already in the render target
that was dumped from the phone -- so the port asked the GPU for the right thing and the GPU drew something else.

Checked against the other Android ports built on the same SDK, so the work is not repeated:

* **Black Android (Black-Continue)**: its MALI FIX row carries four things. The helper specialisation that keeps a
  texture or sampler out of a function parameter (`Specialize` in `vk_shader.cpp`) is the real fix there and is already
  in this port. Fences instead of timeline semaphores on Mali is also already here. "Glow passes dropped" is what SAFE
  GRAPHICS already does. **"Full precision", their relaxed-precision strip, does not apply**: glslang emits no
  `RelaxedPrecision` decoration for this port's shaders -- checked by compiling the port's own generated pixel and
  vertex shaders with glslang 16.0.0 and the port's own flags, which gave zero such decorations in both. Their
  optimizer flag cannot do anything either: glslang's optimizer is SPIRV-Tools, which is not linked in either port, so
  modules come out byte-identical with it on and off (1786 and 2791 words, same call count).
* **MarathonRecomp-Android**: the same class of report on the same silicon -- a Moto G04s (Unisoc T606, Mali-G57 MP1)
  with "graphical glitching / stretched vertices", never resolved.
* **mw2-android**: a Moto G35 (Mali-G57) whose "geometry is stretched into spikes".
* **UnleashedRecomp-Android** (the port this one's Mali findings would come from) states its floor as Valhall G610 and
  newer and says "older Mali generations cannot work"; the Black port's floor is Mali-G7x. A G57 is below both.
* **A different driver is not an option on a stock phone**: PanVK is the Mesa driver for Mali, and it needs the upstream
  panfrost or panthor kernel interface, which no retail Android phone ships; Turnip, the usual way out, is Adreno only.

That leaves the vendor's shader compiler, and nothing in the port can be pointed at it from here. What a port *can* do
is stop feeding it the construct it miscompiles -- which needs to be identified on a device that has it, not guessed at.

## The OpenGL ES renderer (in progress)

A second backend is being added for the phones whose Vulkan driver draws the picture wrong. Its reason is narrow and
worth writing down: on a Mali-G57 the same game files, the same render targets and the same SPIR-V come out deformed,
while GL ES is a different driver front end on the same silicon, and the shaders reach it through SPIRV-Cross rather
than the driver's Vulkan compiler.

What is in place:

* `native/glsl.cpp` -- the port's HLSL through glslang, out of SPIR-V through SPIRV-Cross as **GLSL ES 3.00**, with the
  layout bindings GL ES 3.00 has no syntax for stripped and the uniform blocks bound by name afterwards, and the
  separate texture/sampler bindings turned into one combined sampler per stage. Checked against the port's own generated
  shaders (vertex, fixed function pixel, a pixel shader sampling a shadow map, a screen quad): all four are valid ES
  3.00 as glslang itself parses them.
* `native/draw_common.h` -- the constants, keys and CPU-side helpers a draw needs, copied **verbatim** from the Vulkan
  backend. The two renderers must compute the same numbers, so the second one does not get its own version of them.
* SPIRV-Cross is fetched by `android/CMakeLists.txt` the same way glslang is, and builds for arm64.

What is still to come: the backend itself (`native/gles.cpp`: EGL window, targets, textures, the draw path, present and
read-back), the `RENDERER` row in the port menu, and choosing the window's graphics API before it is created. Vulkan
stays the default; GL ES will be a switch, and an option the port can pick by itself on a GPU it knows draws badly.

## Credits and licences

The renderer, the SDK and the port's structure come from the **XboxXBE-Recomp / Recompiled** project (MIT), and the
Vulkan backend, touch controls and Android host were adapted from **Rangga Casper's Black Android port** (MIT, © 2026
Rangga Casper). Both notices are kept in `LICENSE` files beside their code. *Shadow the Hedgehog* is a trademark of
SEGA; this project ships no SEGA code and no SEGA assets, and is not affiliated with SEGA.
