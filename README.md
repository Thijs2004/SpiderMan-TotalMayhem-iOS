# Spider-Man: Total Mayhem for iOS

A native iPhone port of **Ultimate Spider-Man: Total Mayhem HD** (Gameloft, 2010), built on top of
[Spider-Man Total Mayhem Revived](https://github.com/RedMemories/SpiderManTotalMayhem) by **RedMemories**, the
Android revival that recompiled the original 32-bit game engine to native 64-bit code.

The game left the App Store years ago and its original 32-bit iOS version no longer runs on any iPhone
(iOS dropped 32-bit apps in iOS 11). This project takes the Android revival and runs it on iOS, **natively and
without a JIT**: no emulator, no debugger tricks, just a normal signed app.

> This repository contains only the port's own code. It contains **no game files**: no APK, no libraries, no
> game data, no Gameloft or Marvel artwork. You need your own copy of the revival's APK; the setup script
> unpacks it next to this folder. Spider-Man is a trademark of Marvel; the game is by Gameloft; the revival is
> by RedMemories. This is a non-commercial personal project, not affiliated with any of them.

## Status

| | |
|---|---|
| Boots, title screen, menus, difficulty select | ✅ |
| Intro video (Marvel / Gameloft logo) | ✅ tap to skip |
| Cutscenes and gameplay | ✅ |
| Touch controls (joystick and buttons at the screen edges) | ✅ |
| Music and sound effects | ✅ own mixer, Ogg Vorbis |
| Game controllers (Xbox, PlayStation, MFi) | ✅ same layout as the revival |
| Tilt (accelerometer) | ✅ |
| Settings: frame rate 30/60/120, resolution 100/75/50 %, screen mode, FPS counter | ✅ |
| 120 Hz on ProMotion iPhones | Metal presentation built; to be confirmed on device |
| Pause when the app goes to the background | ✅ |

Verified so far: the full flow up to gameplay in the iOS Simulator, and boot, rendering, touch and the menus
on an iPhone 16 Pro (iOS 26). Sound, controller, tilt and 120 Hz still need a full test on the device.

## Using the app

### First launch
The app opens straight into the game: the Marvel / Gameloft intro video plays first (tap anywhere to skip),
then the title screen (*Touch the screen*). The first launch takes a few seconds longer while the game data
is prepared. The game always runs in landscape, full screen, with the status bar and home indicator hidden.

### Settings, FPS counter, frame rate, resolution
**Long-press the Spider-Man icon on the home screen → *Settings*.** (The same way the revival does it on
Android.) If the game isn't running, the settings screen opens first and *Play* starts the game with them. If
it is running, the game pauses, and *Done* takes you back.

| Setting | Options | Notes |
|---|---|---|
| **Frame rate** | 30 · 60 · 120 | 120 uses ProMotion on iPhone Pro models. Low Power Mode limits every app to 60. |
| **Resolution** | 100 % · 75 % · 50 % | Rendering resolution; lower is faster and slightly softer. |
| **Screen** | Wide · Original · Stretched | Wide fills the screen (the 3D view widens, menus and HUD keep their proportions); Original keeps the 2010 aspect with bars; Stretched stretches. |
| **FPS counter** | on / off | A small green counter in the top-left corner. Applies immediately. |
| **Controller** | on / off | Applies immediately. |
| **Tilt** | on / off | Accelerometer input like the original. Applies immediately. |

Frame rate, resolution and screen are set when the game engine starts. Changing them while playing asks to
close the game; open it again and they're active. Your progress is kept.

Music and sound-effect volume are in the **game's own Options menu** (main menu → Options), as in the
original.

### Touch controls
The game's original touch controls: a joystick on the left, action buttons on the right (jump, punch, web,
spider-sense, super). In widescreen they sit at the real edges of the screen. Menus are touch-driven, as in
the original.

### Game controller
Any Xbox, PlayStation or MFi controller works (Bluetooth or USB-C). The layout follows the revival:

| Button | Action |
|---|---|
| Left stick / D-pad | Move (the d-pad wins when pressed) |
| A / Cross | Jump |
| X / Square | Punch |
| Y / Triangle | Web |
| B / Circle | Spider-sense; **back** in menus |
| R1 / R2 | Super |
| L1 / L2 | Action: rescue, switches |
| Menu / Options | Pause (the Android back key) |

### Tilt
With *Tilt* on, the accelerometer values go to the game exactly as on Android, in the same units and
direction. Turn it off in Settings if you don't want it.

### Pausing
Leaving the app (home gesture, notification, call) pauses the game and its sound; coming back resumes it.

### Saves and files
Saves, settings and the game data live in the app's **Documents** folder, which is visible in the Files app
(*On My iPhone → Spider-Man → SpiderManTotalMayhemRevived*). Deleting the app deletes your saves; copy
`levelRanks.dat`, `settings.dat` and `androidTrophy.dat` from there to keep them.

### Troubleshooting
- **Stuck at 60 fps with 120 selected:** turn off Low Power Mode; check the FPS counter.
- **No sound:** raise the media volume (the game follows the media volume and plays even with the silent
  switch on); check the in-game Options volume.
- **Something wrong:** the log is `Documents/smtm.log` (also visible in the Files app).

## How it works

The Android revival consists of three native libraries:

| Library | What it is |
|---|---|
| `libspidermanarm32.so` | The original 2010 Gameloft engine (32-bit ARMv5TE, Irrlicht 1.4.2) |
| `libspidermanaot.so` | That engine recompiled ahead of time to arm64: 19,089 of its 19,096 functions |
| `libspiderman.so` | RedMemories' arm64 runtime: loads the 32-bit engine into a 4 GB guest space, provides its libc / OpenGL ES / JNI imports, widescreen and frame pacing, and runs the few remaining 32-bit paths on the dynarmic JIT |

On Android these are loaded by the system linker and driven by Java code. On iOS none of that exists, and
iOS also forbids generating executable code at run time (no JIT). This port makes the two arm64 libraries run
as-is, replaces Android and Java underneath them, and replaces the JIT with an interpreter.

```
 iOS app (app/)                    UIKit, Metal, AVAudio, GameController, CoreMotion
   │  calls the game's JNI natives (nativeGameInit, nativeUpdateAndRender, nativeOnTouch, ...)
   ▼
 libspiderman.so (arm64, baked into the app)   ◄── bionic layer (runtime/bionic.c), fake JVM (runtime/jni.c)
   │  guest calls, SVCs
   ├──► libspidermanaot.so (arm64, baked in)     native recompiled game code
   └──► interpreter (runtime/arm32.c)            the few 32-bit paths that aren't recompiled
          ▲ replaces dynarmic's Jit::Run
```

### 1. Android libraries as signed iOS code (`tools/elf2macho.py`)

iOS only executes code that is signed and mapped from the app bundle, so the ELF libraries can't be loaded
at run time. At build time each one is turned into a Mach-O dylib with the **same memory layout**: an `r-x`
segment with the ELF's code and read-only data, followed directly by a `rw-` segment with its data and bss, so
every PC-relative reference keeps working. The loader (`runtime/elfload.c`) then applies the ELF relocations
(RELR, RELA, JUMP_SLOT, TLSDESC) into the writable copy, resolves imports and runs the init arrays. Code pages
are never written.

While baking, a few instructions are patched:

- **Thread pointer.** Android code reads its thread pointer with `mrs xN, tpidr_el0` (2,348 times in the
  runtime: stack-protector canaries and thread-local variables). On Apple CPUs that register holds the CPU
  number and user writes are dropped. All of them become `mrs xN, tpidrro_el0`, Darwin's stable per-thread
  TSD base. The canary slot (`[tp, #0x28]`) is a constant 0 there, and the runtime's thread-local block
  (196 bytes) lives in consecutive pthread TSD slots, so a TLS access stays a single load (the TLSDESC resolver
  just returns a constant offset).
- **`CTR_EL0` and `GCSPR_EL0`** reads (cache type for JIT flushing, guarded control stack) trap on Apple
  CPUs and become constants.
- **Hooks.** `--hook` replaces a function's first instructions with a jump through a slot the loader fills
  in. `Dynarmic::A32::Jit::Run` is hooked this way to the interpreter.

### 2. Android libc on Darwin (`runtime/bionic.c`, `runtime/asm.S`)

About 450 imports. Most map directly to Darwin; the ones where the ABI differs are converted:

- pthread mutexes, condition variables, rwlocks, once, keys and thread attributes (bionic's objects are
  smaller than Darwin's; each holds a pointer to a lazily created Darwin object)
- unnamed semaphores (Darwin has none)
- `stat`, `statfs`, `dirent`, `utsname` layouts
- `open` / `fcntl` / `mmap` flag values, clock IDs, `sysconf` names, errno quirks
- stdio streams (`__sF`, `stdin` / `stdout` / `stderr`)
- **variadic calls**: AAPCS64 passes variadic arguments in registers, Darwin on the stack. Assembly
  trampolines spill the registers into an AAPCS `va_list`, and `printf` / `scanf`-style functions convert it
  to Darwin's layout by parsing the format string (including 128-bit `long double`)
- `dlopen` / `dlsym` / `dladdr` / `dl_iterate_phdr` over the baked images (C++ exceptions need the latter)
- Android paths (`/sdcard/...`, `/data/data/...`) mapped into the app container

### 3. The Java side (`runtime/jni.c`, `app/java.m`, `app/sound.m`)

A complete JNI 1.6 function table with classes, methods, strings and arrays. The runtime registers its natives
through `RegisterNatives`, and the app calls them in the same order as the Android activity, renderer and
surface view. The Java classes the game calls back into are written natively:

- **`GLMediaPlayer`** (Gameloft's sound player): a mixer on `AVAudioSourceNode` that plays the game's 493
  Ogg Vorbis files with [stb_vorbis](https://github.com/nothings/stb). Same emitter model as the Java original:
  create / play / pause / resume / stop / auto-destroy, music vs. effects volume groups. Music streams; effects
  are decoded once and cached.
- `Revived` (data paths), device and language queries, video state.

Every call to a Java method that isn't implemented is logged with its arguments, which is how the missing
pieces were found.

### 4. No JIT: an ARMv5TE interpreter (`runtime/arm32.c`)

The 2010 engine is ARMv5TE: ARM + 16-bit Thumb, software floating point. The interpreter covers that, plus
the handful of ARMv6/v7 and VFP instructions the runtime's own guest helpers use (`__aeabi` float/double/divide
routines: `vmov`, `vadd` / `vsub` / `vmul` / `vdiv`, `vcmp`, `vmrs`, `vcvt`, `movw` / `movt`, `mls`, `udiv`,
`uxtb`, `ldrex` / `strex`, ...).

It replaces dynarmic's `Jit::Run` and works on dynarmic's own register block, CPSR and halt flags. Every
`SVC` goes to the runtime's `Cpu::CallSVC`, which handles imports (libc, GL, JNI) and entries into recompiled
code. On every jump or return the interpreter checks the recompiled engine's entry table
(`aot::direct_entry`) and goes **back to native code** as soon as it can. That cut interpreted work 47-fold:
in gameplay roughly 0.7 million instructions per second still run interpreted, a few percent of one core.
Everything else is native.

### 5. Graphics (`app/host.m`, `app/present.m`)

The game renders with OpenGL ES 1.1 (fixed function) through Apple's OpenGL ES. Because OpenGL ES content
presented with `CAEAGLLayer` stays at 60 Hz on ProMotion screens, frames go through Metal: the game renders
into one framebuffer whose colour image rotates over three IOSurface-backed buffers shared between OpenGL and
Metal, a GL fence marks when a frame is done, and a fullscreen Metal pass shows it in a `CAMetalLayer` that
can run at 120 Hz. (The 2010 engine remembers the framebuffer bound at start-up, so the framebuffer object stays
the same and only its attachment changes.) If OpenGL can't draw into the shared images, as in the Simulator's
software renderer, a read-back fallback is used automatically.

### 6. App

- Scene-based UIKit app, landscape, full screen, pauses the game and its sound in the background.
- **Settings**: long-press the app icon → *Settings* (like the revival on Android). Frame rate 30 / 60 / 120,
  resolution 100 / 75 / 50 %, screen Wide / Original / Stretched, FPS counter, controller, tilt. Frame rate,
  resolution and screen are applied when the game starts; changing them during play offers to close the game.
- **Controller** (`app/input.m`), following the revival's `Pad` class: A jump, X punch, Y web, B spider-sense
  (back in menus), R1/R2 super, L1/L2 action/rescue, left stick or d-pad to move, Menu to pause.
- **Tilt**: accelerometer values in Android's units and sign, sent to `nativeAccelerator`.
- The Gameloft logo video plays at start (`AVPlayerLayer`); tap to skip.

## Building

Requirements: macOS with Xcode (iOS 17+ SDK), Python 3 with Pillow, a JDK (for jadx, used once to read
the sound name table), the GitHub CLI, and the revival's APK (v1.3).

```sh
./setup.sh /path/to/SpiderManTotalMayhem.apk   # once: unpacks libraries, game data and the sound table next to this folder
./build.sh sim                                 # iOS Simulator build, installed into the "iPhone 18 Pro" simulator
INSTALL=1 ./build.sh ios                       # device build: ../dist/SpiderMan.ipa, installed on the iPhone
```

For device builds, put your signing setup in `local.env` next to `build.sh` (it is not committed):
`SIGN_ID`, `PROFILE`, `TEAM_ID`, `BUNDLE_ID`, and `DEVICE` for `INSTALL=1` (see the top of `build.sh`). The
app has no special entitlements, so a free or paid Apple developer account with a matching profile works. The device build bundles the 370 MB of game data; on first launch it is cloned (APFS clone,
no extra space) into the app's Documents folder, where the game also keeps its saves.

Running in the Simulator, with the game data read straight from the Mac:

```sh
SIMCTL_CHILD_SMTM_DATA=$PWD/../data/SpiderManTotalMayhemRevived \
  xcrun simctl launch --console "iPhone 18 Pro" <your bundle ID>
```

Test switches (environment variables, Simulator only): `SMTM_TAPS="frame:x,y;frame:x,y"` sends scripted taps
(x, y as fractions of the screen), `SMTM_SHOW_SETTINGS=1` opens Settings at launch, `SMTM_PROPS` sets the
runtime's `debug.smhp.*` properties.

## Logs

Everything goes to `Documents/smtm.log` in the app container (and the console):

- the runtime's own log (Italian, from RedMemories) and the game's Gameloft log
- `java: Class.method(sig) (args) not implemented`: Java calls without an implementation yet
- `arm32: N instructions interpreted in the last 5 s`: how much runs on the interpreter
- `frames: N/s, game X ms, present Y ms`: real frame rate and per-frame cost
- `present: Metal, ...`: which presentation path is used

Pull it from the device with `xcrun devicectl device copy from --domain-type appDataContainer
--domain-identifier <your bundle ID> --source Documents/smtm.log`.

## Layout

```
tools/elf2macho.py     ELF -> Mach-O baking, instruction patches, symbol export, hooks
runtime/elfload.c      ELF loader: relocations, imports, TLS, init arrays, dl_iterate_phdr
runtime/bionic.c       Android libc on Darwin
runtime/asm.S          TLSDESC resolver, missing-import traps, AAPCS variadic trampolines
runtime/jni.c          JavaVM / JNIEnv
runtime/gl.m           GLES 1/2 and EGL imports
runtime/arm32.c        ARMv5TE / Thumb interpreter replacing the JIT
app/host.m             app, scene, game view, boot sequence, touch, video, FPS counter
app/present.m          OpenGL ES -> Metal presentation
app/java.m             Java classes the game calls
app/sound.m            GLMediaPlayer: mixer and Ogg decoding
app/input.m            controller and tilt
app/settings.m         settings screen and values
res/Info.plist         app metadata
third_party/stb_vorbis.c  Ogg Vorbis decoder (public domain, Sean Barrett)
setup.sh, build.sh     preparation and build
```

## Known limitations

- 120 Hz is implemented but still being verified on device.
- Online features of the 2010 game (Gameloft LIVE, "More Games", tracking) talk to services that no longer
  exist; their Java calls are logged and ignored.
- The settings screen covers display and controls; sound volume lives in the game's own options menu.

## Credits

- **Gameloft**: the original game. **Marvel**: Spider-Man.
- **RedMemories**: the Android revival, the ahead-of-time recompiler and the runtime this port runs on.
- **dynarmic** (in the revival's runtime), **stb_vorbis** by Sean Barrett, **jadx** (used during setup).
- iOS port by Thijs Mussig.
