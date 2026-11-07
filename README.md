<img width="3464" height="3464" alt="1000870619" src="https://github.com/user-attachments/assets/5af36cd2-f501-4c1f-aaf4-757e1c2c1759" />

# 🍌 Banana Doom

A native Android port of the original **DOOM** (id Software's `linuxdoom-1.10` source) with on-screen touch controls, gamepad and keyboard support, an OpenGL ES 2.0 video backend, a low-latency software sound mixer, and a built-in **OPL/FM music synthesizer** that plays the music straight from the WAD — no soundfonts, no extra files.

The original game logic, renderer and data formats are untouched in spirit: the 1990s C engine runs as-is inside a `NativeActivity`, and a small (~2,100 lines of C) platform layer replaces everything that used to talk to DOS/X11/OSS.

```
Android app  ──►  NativeActivity (Java, ~190 lines)  ──►  libdoom.so
                                                          ├─ doom/      original engine (C, 56 .c / 60 .h)
                                                          └─ platform/  Android glue: EGL/GLES2, AAudio,
                                                                        touch/gamepad input, FM music, JNI keyboard
```

> **You need a WAD file.** The game data (`doom1.wad`, `doom.wad`, `doom2.wad`, `freedoom1.wad`, …) is **not** included in this repository. See [Getting a WAD](#getting-a-wad).

---

## Table of contents

1. [Features](#features)
2. [Getting started](#getting-started)
3. [Controls](#controls)
4. [Project layout](#project-layout)
5. [How it works](#how-it-works)
   - [Architecture and threads](#architecture-and-threads)
   - [Build system](#build-system)
   - [Application entry and lifecycle](#application-entry-and-lifecycle)
   - [IWAD discovery and file locations](#iwad-discovery-and-file-locations)
   - [Video: EGL + OpenGL ES 2.0](#video-egl--opengl-es-20)
   - [Input](#input)
   - [Sound effects: the software mixer](#sound-effects-the-software-mixer)
   - [Music: MUS sequencer + FM synthesizer](#music-mus-sequencer--fm-synthesizer)
   - [Soft keyboard bridge (JNI)](#soft-keyboard-bridge-jni)
   - [Fatal error screen](#fatal-error-screen)
   - [Changes made to the original engine](#changes-made-to-the-original-engine)
6. [Tuning constants](#tuning-constants)
7. [Known caveats and housekeeping](#known-caveats-and-housekeeping)
8. [Troubleshooting](#troubleshooting)
9. [Credits and license](#credits-and-license)

---

## Features

- **Original DOOM engine** — the full software renderer, game logic, AI, menus, automap, intermission screens, save/load.
- **Supports multiple IWADs** — Doom 2, Plutonia, TNT, Ultimate Doom, Doom (registered), Doom shareware, FreeDoom 1 & 2. The first one found is used.
- **Touch controls** — floating analog stick, drag-to-turn, drag-while-firing aiming, context-aware buttons (FIRE / USE / weapon prev-next / MENU / MAP).
- **Gamepad and hardware keyboard** support (Bluetooth/USB controllers, WASD, arrows, Ctrl, Space, …).
- **OpenGL ES 2.0 output** — 320×200 indexed frame converted to RGBA, uploaded to a texture, drawn 4:3 letterboxed with nearest-neighbour filtering and vsync.
- **Low-latency audio** through **AAudio** (shared, low-latency mode) with a 16-channel mixer, pitch shifting, stereo separation, and automatic stream re-opening after a device change.
- **Music without a soundfont** — an OPL2-style two-operator FM synthesizer fed by the `GENMIDI` instrument bank that is already inside the WAD, i.e. the same patch bank the original DOS game used for AdLib/SoundBlaster.
- **System soft keyboard for save names** via a hidden `EditText` and JNI, with Cyrillic → Latin transliteration so Russian/Ukrainian input becomes readable in DOOM's menu font.
- **Auto-pause** when the app goes to the background (opens the DOOM menu, which freezes the game, and pauses audio).
- **Immersive fullscreen**, display-cutout aware, screen kept on, forced landscape (sensor landscape).
- **Crash-proof startup** — if no WAD is found (or a fatal engine error occurs) a readable in-game error screen is drawn instead of a silent crash.
- **16 KB page-size compatible** native libraries (required for newer Android versions/devices).
- Builds for **arm64-v8a** and **armeabi-v7a**.

---

## Getting started

### Requirements

| Component        | Version used by this project                  |
|------------------|-----------------------------------------------|
| Android Gradle Plugin | 8.13.0                                   |
| Gradle           | 9.0.0 (wrapper included)                      |
| JDK              | 17                                            |
| Android SDK      | `compileSdk 36`, `targetSdk 34`, **`minSdk 26`** |
| Android NDK      | `29.0.14033849`                               |
| CMake            | `3.31.6` (Android SDK Manager package)        |
| Device           | OpenGL ES 2.0, Android 8.0 (API 26)+          |

`minSdk 26` is required because the audio backend uses **AAudio**.

### Getting a WAD

You must provide an IWAD. Options:

- **Freedoom** (free, open content): <https://freedoom.github.io/> — use `freedoom1.wad` or `freedoom2.wad`.
- **Doom shareware** (`doom1.wad`) — freely distributable shareware episode.
- **Your own copy** of `doom.wad`, `doom2.wad`, `plutonia.wad`, `tnt.wad`, `doomu.wad`.

Then do **one** of the following:

1. **Bundle it into the APK** — copy the file into `app/src/main/assets/` *before* building. (`.wad` files are stored uncompressed in the APK via `noCompress += "wad"`.) On first launch it is extracted to the app's private storage.
2. **Copy it to the device** after installing the app:
   ```
   /sdcard/Android/data/com.banana.doom/files/doom1.wad
   ```
   For example: `adb push doom1.wad /sdcard/Android/data/com.banana.doom/files/`

> ⚠️ Do not commit commercial WAD files to a public repository

### Build

```bash
git clone https://github.com/CanavarIT/Banana-Doom.git
cd Banana-Doom

# put a WAD into app/src/main/assets/

./gradlew assembleDebug          # Windows: gradlew.bat assembleDebug
```

The APK is produced at `app/build/outputs/apk/debug/app-debug.apk`. Install it with:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

For a release build use `./gradlew assembleRelease`. Note that the release build type is currently signed with the **debug key** (see `app/build.gradle.kts`) — replace it with your own keystore before publishing.

You can also open the project directly in **Android Studio** or **AndroidIDE** (the project was created with AndroidIDE, hence the `.acside/` folder).

To run on an **emulator**, add `"x86_64"` to `abiFilters` in `app/build.gradle.kts` (there is a comment marking the spot).

---

## Controls

### Touch screen

| Area / control                | Action in game                                         | Action in menus / intermission     |
|-------------------------------|--------------------------------------------------------|------------------------------------|
| **Left 45 % of the screen** (touch anywhere) | Floating analog stick: forward/back + strafe | Up / Down / Left / Right arrows (with key repeat) |
| **Right side** (drag horizontally) | Turn left/right                                   | —                                  |
| **FIRE** button               | Fire (`Ctrl`). Keep the finger down and **drag** to aim while shooting | **OK** (`Enter`) |
| **USE** button                | Open doors / switches (`Space`)                        | **YES** — only visible on yes/no prompts |
| **`<` / `>`** buttons         | Previous / next owned weapon                           | hidden                             |
| **MAP** button                | Toggle automap (`Tab`)                                 | hidden                             |
| **MENU** button               | Open menu (`Esc`)                                      | **BACK** (`Esc`)                   |

Stick behaviour: half deflection = **walk**, full deflection = **run** (classic DOOM speeds).

### Gamepad

| Input                 | Action                                  |
|-----------------------|-----------------------------------------|
| Left stick            | Move / strafe (menus: navigate)         |
| Right stick (X axis)  | Turn                                    |
| **A** / **R2**        | Fire                                    |
| **X**                 | Use                                     |
| **Y**                 | Automap                                 |
| **B** / **Start** / **Select** / Back | Escape / menu           |
| **L1** / **R1**       | Previous / next weapon                  |
| D-pad                 | Arrow keys (left/right turn, up/down move) |
| D-pad centre / Enter  | Enter                                   |

### Hardware keyboard

`W` / `S` move, `A` / `D` strafe, arrows move/turn, `Ctrl` fire, `Space` use, `Shift` run, `Tab` automap, `Esc` menu, `1`–`9`/`0` select weapons, `F1`–`F10` DOOM function keys, `Y` / `N` answer prompts, `-` / `=` change screen size.

Volume keys and other unmapped keys are passed through to the system.

---

## Project layout

```
BananaDoom/
├── build.gradle.kts, settings.gradle.kts, gradle/libs.versions.toml, gradle.properties
├── gradlew, gradlew.bat, gradle/wrapper/
└── app/
    ├── build.gradle.kts                  # Android + NDK/CMake configuration
    ├── proguard-rules.pro
    └── src/main/
        ├── AndroidManifest.xml           # NativeActivity-based launcher activity
        ├── assets/                       # drop your WAD here (PUT_WAD_HERE.txt)
        ├── java/com/banana/doom/
        │   └── MainActivity.java         # fullscreen + hidden EditText for soft keyboard
        ├── res/                          # icons, theme, app name
        └── cpp/
            ├── CMakeLists.txt            # builds libdoom.so
            ├── doom/                     # the original DOOM engine (linuxdoom-1.10 based)
            └── platform/                 # the Android port itself
                ├── android_main.c        # entry point, lifecycle, system layer, IWAD lookup, input dispatch
                ├── i_video.c             # EGL + OpenGL ES 2.0 output
                ├── touch.c               # on-screen controls: logic + drawing + 5x7 bitmap font
                ├── i_sound.c             # SFX mixer + AAudio stream
                ├── music.c               # MUS sequencer + FM synthesizer
                ├── textinput.c           # JNI bridge for the system soft keyboard
                └── plat.h                # shared declarations between platform modules
```

---

## How it works

### Architecture and threads

Everything game-related runs in a single shared library, `libdoom.so`, loaded by `MainActivity` (a thin subclass of `android.app.NativeActivity`) through the `android.app.lib_name = doom` manifest meta-data.

Three threads are involved:

| Thread | What runs there |
|--------|-----------------|
| **Game thread** (created by `native_app_glue`) | `android_main()` → `D_DoomMain()` → the DOOM game loop, rendering, **and all input handling**. `native_app_glue` delivers touch/key events through an `ALooper` that DOOM drains in `I_StartTic()` on this same thread, so the input state in `touch.c` needs no locking. |
| **Audio thread** (AAudio callback) | `audio_cb()` in `i_sound.c`: mixes sound effects and calls `mus_render()` to synthesize music. |
| **UI thread** (Java) | Views, the hidden `EditText`, and the soft keyboard. Talks to the game thread through a mutex-protected buffer (`textinput.c`). |

Cross-thread rules are deliberately minimal and lock-free where it matters:

- SFX channels are handed to the audio thread with `__atomic` acquire/release on a per-channel `active` flag.
- Music state is guarded by a mutex that the game thread takes normally but the **audio thread only `trylock`s** — if it is busy, the audio thread outputs silence for that block instead of ever blocking.
- Keyboard text arrives on the UI thread, is parked behind a mutex, and is consumed by `kbd_poll()` once per tic on the game thread.

### Build system

**`app/build.gradle.kts`**

- `ndkVersion = "29.0.14033849"`, CMake `3.31.6`, `abiFilters = arm64-v8a, armeabi-v7a`.
- CMake arguments: `-DANDROID_STL=none` (the engine is pure C, no C++ runtime is linked) and `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`.
- `androidResources { noCompress += "wad" }` so a bundled WAD stays uncompressed in the APK.
- `packaging.jniLibs.useLegacyPackaging = false` together with `android:extractNativeLibs="false"` in the manifest: the `.so` files are mapped directly from the APK instead of being extracted on install.
- Release: R8 minification and resource shrinking enabled, debug symbols stripped (`debugSymbolLevel = NONE`).

**`app/src/main/cpp/CMakeLists.txt`**

- Builds one shared library `doom` from `doom/*.c` + `platform/*.c` (both found by `file(GLOB … CONFIGURE_DEPENDS)`, so new files are picked up automatically) **plus** the NDK's `android_native_app_glue.c`, compiled straight into the library.
- Links against `android log EGL GLESv2 aaudio m`.
- Compiler flags and why:

  | Flag | Reason |
  |------|--------|
  | `-O2`, `-flto=thin`, `-ffunction-sections -fdata-sections`, `-Wl,--gc-sections` | Fast software renderer, small binary |
  | `-fvisibility=hidden` | Only JNI/glue entry points are exported |
  | `-fno-strict-aliasing -fwrapv` | The engine is 1990s C that relies on type punning and wrapping integer arithmetic |
  | **`-fsigned-char`** | DOOM assumes plain `char` is **signed** (x86). On ARM/ARM64 Android it is unsigned, which breaks negative `forwardmove`/`sidemove` (inverted and jerky movement). Forcing the original behaviour fixes it everywhere |
  | `-Wl,-z,max-page-size=16384` | 16 KB page-size compatibility |

### Application entry and lifecycle

`MainActivity` (Java) sets up the window, then `native_app_glue` starts the game thread at `android_main()` (`platform/android_main.c`):

1. Save the `android_app` pointer and register the command (`on_cmd`) and input (`on_input`) callbacks.
2. `chdir()` into the app's **internal data directory** — DOOM's relative-path file I/O (`default.cfg`, `doomsavN.dsg`) therefore lands in private app storage without any engine changes.
3. `touch_init()`, `kbd_init()` (registers the JNI keyboard bridge).
4. Pump the looper until the EGL surface exists (`gl_ready()`).
5. Call the original `D_DoomMain()` — from here on it is the normal DOOM startup: `IdentifyVersion` → `M_LoadDefaults` → `Z_Init` → `W_InitMultipleFiles` → `R_Init` → `P_Init` → `I_Init` (sound + music) → … → `D_DoomLoop`.

Lifecycle commands handled in `on_cmd`:

| Command | Handling |
|---------|----------|
| `APP_CMD_INIT_WINDOW` | `gl_attach(window)`: create EGL display/surface/context and GL resources |
| `APP_CMD_TERM_WINDOW` | `gl_detach()`: tear everything down |
| `APP_CMD_PAUSE`   | Pause the audio stream, cancel all touches, and — if a level is running and no menu is open — call `M_StartControlPanel()`, which pauses the game |
| `APP_CMD_RESUME`  | Resume audio |
| `destroyRequested` | Save `default.cfg`, release GL, exit |

`I_StartTic()` (called by the engine before each tic) is where the platform layer gets its time slice. It calls `pump()` in a loop and **blocks** on the looper while there is no window, the app is paused, or GL is not ready — so the game is truly frozen (no CPU spin) while in the background.

Timing: `I_GetTime()` is `CLOCK_MONOTONIC` converted to DOOM tics (`TICRATE`, 35 Hz). `TryRunTics()` and the screen-wipe loop use `I_Sleep(1)` instead of busy-waiting, which keeps CPU usage and heat down.

### IWAD discovery and file locations

`IdentifyVersion()` in `d_main.c` walks a table of known IWADs in this priority order:

`doom2.wad` → `plutonia.wad` → `tnt.wad` → `doomu.wad` → `doom.wad` → `doom1.wad` → `freedoom1.wad` → `freedoom2.wad`

For each name, `I_FindIWAD()` (in `android_main.c`) looks, in order, for the lowercase and the UPPERCASE file name in:

1. `externalDataPath` — `/sdcard/Android/data/com.banana.doom/files/` (easy to reach with `adb` or a file manager)
2. `internalDataPath` — the private app directory
3. **APK assets** — if the file is bundled, it is streamed out of the APK with `AAssetManager` into the internal directory (written to a `.tmp` file and atomically `rename()`d, so an interrupted copy never leaves a corrupt WAD). Next launch finds it in step 2.

The detected file sets `gamemode` / `gamemission` (shareware, registered, retail, commercial; Doom 2 / Plutonia / TNT), which in turn selects the correct episode/map structure, lump names and text.

Files written by the game (all in the internal data dir, which is the working directory):

| File | Content |
|------|---------|
| `default.cfg` | 7 settings: mouse sensitivity (used as touch-look sensitivity), SFX volume, music volume, show messages, screen blocks, detail level, gamma |
| `doomsav0.dsg … doomsav5.dsg` | Savegames |

### Video: EGL + OpenGL ES 2.0

Implemented in `platform/i_video.c`. The engine still renders into its classic **320×200, 8-bit indexed** framebuffer (`screens[0]`); the platform layer turns that into pixels on screen:

1. **`I_SetPalette()`** — called by the engine whenever the palette changes (damage flashes, item pickups, gamma…). The 256 palette entries are run through DOOM's `gammatable[usegamma]` and stored as packed RGBA in `pal32[]`.
2. **`I_FinishUpdate()`**, once per frame:
   - converts all 64,000 pixels `screens[0]` → `rgba[]` through `pal32[]`;
   - uploads them into a 320×200 RGBA texture with `glTexSubImage2D`;
   - draws a textured quad (`GL_TRIANGLE_STRIP`) **letterboxed to 4:3**, centred (DOOM's pixels are 1:1.2, so 320×200 is displayed at 4:3, as on the original hardware);
   - draws the touch-control overlay;
   - `eglSwapBuffers` (swap interval 1 → vsync).
3. The texture uses `GL_NEAREST` min/mag filtering and `GL_CLAMP_TO_EDGE` for crisp pixels.

Two tiny shader programs are used:

- **Texture program** — passthrough position/UV, `texture2D(...).rgb`.
- **Colour program** — takes vertices in **pixel coordinates** plus an RGBA colour, and converts them to clip space in the vertex shader via a `uRes` uniform. All overlay graphics (stick, buttons, text) are built as plain coloured triangle lists and drawn with alpha blending in one `glDrawArrays` call.

EGL specifics: an RGB888 `EGL_OPENGL_ES2_BIT` window config is chosen, `ANativeWindow_setBuffersGeometry` is set to the config's native visual ID, and the surface size is re-queried every frame (`check_size()`) so rotation/resizing re-lays out the touch controls automatically. Because `gl_detach()`/`gl_attach()` run on every window loss/creation, all GL objects (programs, texture) are re-created in `gl_setup()` each time the surface comes back.

### Input

All input enters through `on_input()` in `android_main.c` and is translated into **native DOOM events** (`D_PostEvent`) or into a few extra "touch/analog" variables read by `G_BuildTiccmd()`.

#### Touch controls (`platform/touch.c`)

*Layout* — everything is expressed in units of `min(width, height)`, so the layout scales to any screen. Buttons are anchored to the right edge/bottom (MENU and MAP to the top), clamped to stay on screen, and hit-tested with a slightly enlarged radius (`HIT_PAD`) so small buttons remain easy to press.

*Floating stick* — touching anywhere in the left 45 % of the screen (and not on a button) starts a stick centred on the touch point. Its output goes through:

1. normalisation to −1…1 (the stick radius is `0.16 × min(W,H)`);
2. a **soft radial dead zone** (0.15) that remaps continuously, preserving direction;
3. **exponential smoothing** (0.35) to remove touch-digitizer jitter that otherwise makes strafing stutter, plus a snap-to-zero below 0.04;
4. a **walk/run curve**: half deflection maps to DOOM's *walk* speed, full deflection to *run* speed, using the engine's original tables — `forwardmove = {25, 50}`, `sidemove = {24, 40}`.

The result is written to `touch_fwd` / `touch_side`, which `G_BuildTiccmd()` adds to the keyboard movement and then **clears** (so a missed tick can never keep thrusting the player). The side-move clamp was fixed to 40 (the engine used the forward limit of 50, which made analog strafing ~25 % faster than keyboard running).

*Turning* — dragging on the right side (or dragging while holding FIRE) accumulates horizontal pixel deltas into `look_units`. The conversion is `0.20° per pixel × (65536/360) angle units × (720 / screen_min_dimension)`, i.e. resolution-independent. Every tick the accumulator is converted into an `ev_mouse` event: the engine turns mouse X into `angleturn -= mousex * 8`, so the value is divided by 8 and the **fractional remainder is carried over** to the next tick for smooth slow turns. The existing *Mouse Sensitivity* slider in DOOM's options menu therefore also adjusts touch-look speed.

*Buttons* — each button has separate in-game and in-menu behaviour (label, keys, visibility):

| Button | In game | In menus / non-level screens |
|--------|---------|------------------------------|
| FIRE   | `Ctrl` | `Enter` + `Ctrl` — labelled **OK** |
| USE    | `Space` | `y` — labelled **YES**, only shown while a yes/no prompt is waiting (`messageNeedsInput`) |
| `<` `>`| cycle weapon | hidden |
| MENU   | `Esc` | `Esc` — labelled **BACK** |
| MAP    | `Tab` | hidden |

Each button tracks the pointer ID that pressed it (true multi-touch: move with one thumb, fire with the other) and releases exactly the keys it pressed. In menus the stick becomes an **arrow-key generator**: a deflection past 0.5 posts a key press/release, with 350 ms before the first repeat and 130 ms between repeats.

*Drawing* — rings, discs and text are generated each frame as triangles (`ring()`, `disc()`, `text()`), and the labels use a built-in **5×7 bitmap font** (`font5x7[64][5]`, ASCII 0x20–0x5F) so the overlay needs no textures or font files. The same font renders the fatal-error screen.

#### Weapon cycling

`T_CycleWeapon()` in `g_game.c` walks the order *fist/chainsaw → pistol → shotgun → super shotgun → chaingun → rocket launcher → plasma gun → BFG* in the requested direction and picks the next weapon that is **owned and has ammo**. DOOM's tic command only has 3 bits for the weapon number, and the original engine remaps "shotgun → super shotgun" and "fist → chainsaw" when you press a number key. For on-screen cycling that is wrong (you could never select the plain shotgun when you own the super one), so a new ticcmd button flag **`BT_EXACT`** (`d_event.h`) was added: together with `BT_CHANGE` it means *"select exactly this weapon, no remapping"*, with weapon field value `7` standing for the super shotgun. `P_PlayerThink()` in `p_user.c` honours it.

#### Gamepad and keyboard

Joystick-class motion events feed `touch_pad(lx, ly, rx)` (left stick X/Y and the right stick's horizontal axis `AXIS_Z`), which is merged with the on-screen stick before the dead zone/smoothing/curve stage — so touch and gamepad share one movement pipeline. The right stick turns at `480 × axis` angle units per tick. Key events pass through `map_key()`, which converts Android key codes to DOOM key codes (see [Controls](#controls)); L1/R1 set `touch_wpn` for weapon cycling; unmapped keys (volume etc.) return "not handled" so Android keeps its default behaviour.

### Sound effects: the software mixer

Implemented in `platform/i_sound.c`, replacing the original sound server.

- **Output:** one **AAudio** output stream — `PCM_I16`, stereo, `LOW_LATENCY`, `SHARED` — driven by a data callback. The actual device sample rate is queried and used for all pitch maths. If the stream reports `AAUDIO_ERROR_DISCONNECTED` (headphones unplugged, route change) a flag is set and `I_UpdateSound()` re-opens the stream on the next tic.
- **Sample decoding:** DOOM sounds are `DS*` lumps in the "DMX" format (header `3, 0`, sample rate, length, 16 padding bytes at each end, unsigned 8-bit PCM). Each is decoded **lazily on first use** to signed 16-bit and cached, with one spare trailing sample for interpolation. Missing sounds fall back to `DSPISTOL`.
- **Mixer:** 16 channels. Each channel keeps a **16.16 fixed-point play position and step**; the step is `2^((pitch−128)/64) × sample_rate / output_rate`, so DOOM's pitch values (128 = unchanged) and any WAD sample rate work at any device rate. Playback uses **linear interpolation** and is mixed into 32-bit accumulators, then clipped to 16-bit.
- **Stereo separation and volume:** `set_vols()` uses the original DOOM distance/separation formula to derive left/right gains (0–127) per channel; `I_UpdateSoundParams()` updates them as the source moves.
- **Voice management:** a free channel is used if available, otherwise the **oldest** one is stolen. Chainsaw sounds, the stone/platform-movement sound and the pistol are restricted to one instance at a time (as in the original). Handles are `(counter << 5) | slot`, so stale handles for stopped sounds are detected safely.
- **Thread safety:** the game thread writes a channel's parameters, then publishes it with an atomic release-store to `active`; the audio thread reads it with acquire-loads.

The music renderer's output (`mus_render()`) is mixed into the same callback, 256 frames at a time.

### Music: MUS sequencer + FM synthesizer

Implemented in `platform/music.c` (~600 lines). Instead of relying on MIDI hardware or a soundfont, the port **emulates OPL2-style FM synthesis in software**:

1. **Instruments** — the `GENMIDI` lump (header `#OPL_II#`, 175 patches × 36 bytes: 128 melodic + 47 percussion) is parsed at start-up. Each patch holds one or two *voices*, each with a modulator and a carrier operator (waveform, multiplier, attack/decay/sustain/release, key-scale level, total level, tremolo/vibrato/sustain flags, feedback and connection type), a note offset, and flags for fixed-pitch and double-voice instruments. If the lump is missing or malformed, music is disabled but the game still runs.
2. **Sequencer** — `I_RegisterSong()` copies a **MUS** lump (the format used by DOOM) and `seq_tick()` interprets it at **140 Hz**, the MUS tick rate. It handles note on/off (with running velocity), pitch bend (±2 semitones), the controllers DOOM uses (program change, channel volume, expression), "all notes off" / "reset all controllers", **looping**, and score end. Channel 15 is percussion (notes 35–81 → patches 128+).
3. **Synthesizer** — 24 polyphonic voices. Each voice has two operators with:
   - four OPL waveforms (sine, half-sine, absolute sine, quarter-sine pulses) from 4096-entry lookup tables;
   - envelope generators with attack/decay/sustain/release derived from the OPL rate tables, level in dB (0.75 dB steps) with key-scale attenuation;
   - **FM** (modulator → carrier phase modulation, self-feedback on the modulator) or **additive** (parallel) connection;
   - global **tremolo (3.7 Hz, ≈1 dB)** and **vibrato (6.1 Hz, ≈7 cents)** LFOs applied to operators that enable them.
   Voice stealing prefers the quietest released voice, otherwise the oldest.
4. **Rendering** — all of this runs **inside the audio callback**: `mus_render()` advances the sequencer by sample count (`samples_per_tick = rate / 140`), renders the active voices to a mono float buffer, applies master/music volume, clips, and returns it to the SFX mixer, which adds it to both channels. Pause/resume and volume changes from the DOOM menu map to `I_PauseSong`, `I_ResumeSong`, and `I_SetMusicVolume`.

### Soft keyboard bridge (JNI)

A `NativeActivity` has no text input, but DOOM's *Save Game* menu needs the player to type a name. The bridge consists of three parts:

**1. `MainActivity.java`**

- Adds a **hidden 1×1 transparent `EditText`** to the window; it is only focusable while the game is editing text, and uses `TYPE_TEXT_FLAG_CAP_CHARACTERS | NO_SUGGESTIONS`.
- `showTextInput(initial, maxLen)` and `hideTextInput()` are **called from C**; they hop onto the UI thread, focus the field, and show/hide the IME (with a retry loop of up to 5 attempts because some keyboards are slow to attach).
- A `TextWatcher` forwards every change to the native method `nativeTextChanged(String)` after running it through `toDoomText()`:
  - upper-cases the text (DOOM's menu font is upper-case only);
  - **transliterates Cyrillic** (А–Я, Ё, І, Ї, Є, Ґ → `A…YA`, `YO`, `I`, `YI`, `YE`, `G`);
  - drops any character outside the printable ASCII range DOOM's font can draw.
- The soft-input mode is `ADJUST_NOTHING | STATE_ALWAYS_HIDDEN`, so opening the keyboard never resizes the window and makes the game surface jump (`NativeActivity` otherwise asks for `adjustResize`).
- Immersive mode (`hideSystemUi()`) is re-applied on every focus change and when the keyboard closes, using `WindowInsetsController` on API 30+ and the legacy system-UI flags below that. Display cutouts use `SHORT_EDGES` mode; the screen is kept awake.

**2. `platform/textinput.c`**

- `kbd_init()` obtains the `JavaVM` and the activity object, **registers `nativeTextChanged` with `RegisterNatives`** (no JNI-name-mangled symbol is needed), and caches the method IDs for `showTextInput` / `hideTextInput`.
- `I_TextInputStart()` / `I_TextInputStop()` (called by the engine's menu code) attach the calling thread to the JVM if needed and invoke the Java methods.
- `native_text_changed()` runs on the UI thread, copies the string into a 128-byte buffer under a mutex and sets a dirty flag.
- `kbd_poll()` — called once per tic from `I_StartTic()` on the game thread — takes the text and calls **`M_SaveStringSet()`**.

**3. `m_menu.c`** (engine)

- `M_SaveStringSet()` replaces the save-slot name with the typed text, keeping only characters the DOOM font can draw and stopping when the string no longer fits the slot border.
- The save-name flow calls `I_TextInputStart()` when editing begins and `I_TextInputStop()` on confirm/cancel; an empty name re-opens the keyboard instead of saving.
- "Done" on the soft keyboard only closes it; the on-screen game button **OK** (`Enter`) confirms the save.

### Fatal error screen

`I_Error()` is overridden: it logs to logcat (tag `DOOM`), then enters a loop that keeps pumping events and draws the message with the same 5×7 overlay font on a dark-red background — wrapped to the screen width, scaled to the screen height. For a missing WAD it also prints exactly where to put the file. **Tap to exit.** A second `I_Error` while already in the error screen simply exits.

### Changes made to the original engine

The `doom/` directory is the `linuxdoom-1.10` source with the following kinds of changes (the engine's behaviour is otherwise unchanged):

**Removed / replaced (not needed on Android)**
- The network layer, sound server, X11 video backend, DOS/assembly code and the French string tables were removed. `d_net.c` is now a small single-player stub that keeps `NetUpdate` / `TryRunTics` semantics (build ticcmds for elapsed time, run due tics) but **sleeps instead of spinning**.
- `i_system`/`i_video`/`i_sound` functions are implemented by `platform/*.c`. The engine zone is a single 16 MB `malloc` block (`I_ZoneBase`).
- Configuration: the original key-binding config file parsing was replaced by built-in default bindings plus seven persisted settings (`M_LoadDefaults` / `M_SaveDefaults`).
- `IdentifyVersion()` uses the IWAD table and `I_FindIWAD()` described above, including FreeDoom.

**Hooks for the platform layer**
- `G_BuildTiccmd()` consumes `touch_fwd`, `touch_side`, `touch_wpn`; side-move clamped to 40.
- `T_CycleWeapon()`, `BT_EXACT` (see [Weapon cycling](#weapon-cycling)).
- `messageNeedsInput` exported from `m_menu.c` so the touch UI knows when to show YES.
- `I_TextInputStart/Stop` + `M_SaveStringSet` for the soft keyboard.

**64-bit / ARM portability and robustness fixes**
- `boolean` handling made compatible with Android's `<stdbool.h>`.
- **Savegames:** `PADSAVEP()` now pads to 8 bytes *relative to the start of the save buffer* (the buffer is `malloc`ed when saving but allocated from the zone when loading, so padding by absolute address produced different layouts), and pointer↔index conversions use `intptr_t`.
- **`spechit` overflow:** the original fixed array of 8 special lines overflowed on some maps (e.g. Doom 2 MAP02) and corrupted adjacent memory; it is now a growable array (`P_AddSpecHit`).
- **Intercept overflow:** `MAXINTERCEPTS` raised from 128 to 1024 and bounds checks added in `PIT_AddLineIntercepts` / `PIT_AddThingIntercepts`.
- **Line specials:** `MAXLINEANIMS` raised from 64 to 256 and the line-special list is bounds-checked.
- Signed `char` forced by the compiler (see [Build system](#build-system)).

---

## Tuning constants

Most of the "feel" is controlled by a handful of constants at the top of `platform/touch.c`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `LOOK_DEG_PER_PX` | `0.20` | Turning speed per pixel of horizontal drag (at 720 px reference height) |
| `LOOK_MAX_UNITS` | `30000` | Cap on accumulated turn per tick |
| `PAD_TURN_UNITS` | `480` | Gamepad right-stick turn rate |
| `STICK_DZ` | `0.15` | Radial dead zone of the analog stick |
| `STICK_SMOOTH` | `0.35` | Stick smoothing (0 = raw, 1 = frozen) |
| `HIT_PAD` | `1.15` | Hit-test radius multiplier for buttons |
| `BTN_SCALE` | `1.0` | Size of FIRE / USE / weapon buttons (`1.2` = 20 % larger) |
| `stick_r` (in `layout()`) | `0.16 × min(W,H)` | Radius of the on-screen stick |
| Button table `btn[]` | — | Each button's position (`ux`, `uy`) and radius (`ur`) in units of `min(W,H)` |

Other places worth knowing:

| Where | What |
|-------|------|
| `android_main.c` → `ZONE_MB` | Engine memory zone size (16 MB) |
| `i_sound.c` → `NCHAN` | Number of SFX channels (16) |
| `music.c` → `NVOICES`, `MUS_MASTER`, `MOD_CYC` | Music polyphony (24), loudness, FM modulation depth |
| `build.gradle.kts` → `abiFilters` | Target ABIs |

---

## Known caveats and housekeeping

- **R8 / release builds:** `showTextInput()` and `hideTextInput()` in `MainActivity` are only ever called from native code. With minification enabled, R8 cannot see those calls, so it may rename or remove them, which would silently disable the save-name keyboard in release builds. If you hit this, add a keep rule to `app/proguard-rules.pro`:
  ```
  -keepclassmembers class com.banana.doom.MainActivity {
      public void showTextInput(java.lang.String, int);
      public void hideTextInput();
  }
  ```
- **Unused template leftovers:** `cpp/native-lib.c` (a "Hello from C" JNI stub), `res/layout/activity_main.xml`, `viewBinding`, and the AndroidX/Material/ConstraintLayout dependencies are not used by the game. `native-lib.c` is not part of the CMake target (only `doom/*.c` and `platform/*.c` are compiled). They can be deleted to shrink the APK.
- **CPU palette conversion:** each frame converts 64,000 pixels on the CPU before upload. This is cheap on modern devices, but a palette-lookup fragment shader would remove it if you want to optimise further.
- **No multiplayer / demos from the network:** the network layer was removed; this is a single-player port.
- **Do not commit build artefacts.** `app/.cxx/`, `app/build/`, `.gradle/`, `.acside/`, `local.properties`, and the `app/src/main/*.zip` source backups should be ignored. A suitable `.gitignore`:
  ```
  .gradle/
  build/
  app/build/
  app/.cxx/
  .acside/
  local.properties
  *.apk
  *.aab
  app/src/main/*.zip
  # game data
  *.wad
  *.WAD
  ```

---

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| Red "DOOM ERROR – No IWAD found" screen | No WAD in assets or on the device. Copy `doom1.wad`/`freedoom1.wad`/… to `/sdcard/Android/data/com.banana.doom/files/` (the screen prints the exact path) or bundle it in `assets/` and rebuild. |
| Black screen after launch | Check `adb logcat -s DOOM` — look for `GL ready WxH`. The device must support OpenGL ES 2.0. |
| Movement is inverted or jerky | The code must be built with `-fsigned-char` (already set in `CMakeLists.txt`). |
| No music, effects work | The WAD has no valid `GENMIDI` lump (log: `music: no GENMIDI lump`). Use a complete IWAD such as Freedoom or a retail WAD. |
| No sound at all | AAudio stream failed to open; check logcat. Requires API 26+. |
| Keyboard doesn't open when saving | Tap the save slot, then wait a moment (the field retries for ~0.5 s). In release builds see the R8 note above. |
| Build fails: NDK/CMake not found | Install NDK `29.0.14033849` and CMake `3.31.6` via the SDK Manager, or adjust `ndkVersion` / `cmake.version` in `app/build.gradle.kts`. |
| Native lib fails to load on Android 15+ devices with 16 KB pages | Already handled (`max-page-size=16384`, `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`); make sure you rebuilt after pulling. |

Useful commands:

```bash
adb logcat -s DOOM                 # game log (GL size, music/GENMIDI status, errors)
adb push doom1.wad /sdcard/Android/data/com.banana.doom/files/
adb shell run-as com.banana.doom ls files     # debug builds: look at saves and default.cfg
```

---

## Credits and license

- **DOOM** © id Software. The engine in `app/src/main/cpp/doom/` is derived from the original `linuxdoom-1.10` source release (source file headers retain id Software's copyright notices and refer to the *DOOM Source Code License*).
- **Android port** — platform layer, touch UI, audio mixer, FM music synthesizer, JNI keyboard bridge, and engine fixes: this repository's author(s).
- **Freedoom** — a free-content IWAD project, recommended for a fully legal setup: <https://freedoom.github.io/>.

> **Licensing note:** this repository contains no game data. Please review the licence terms that apply to the id Software source code you are redistributing, and add a matching `LICENSE` file to the repository before publishing. DOOM, the DOOM logo and related names are trademarks of their respective owners; this is an unofficial fan port.
