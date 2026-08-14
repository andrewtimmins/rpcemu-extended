# Android

RISC OS runs on Android. There is an APK, one machine runs at a time, and it boots
to its desktop and takes touch, mouse and keyboard input. Sound, suspend on
leaving the foreground, and any settings beyond choosing a ROM are not written yet.

Everything below has been exercised on an x86_64 AVD. No arm64 tablet has run this
yet, so treat the arm64 build as compiled rather than tested.

## The application

`src/gui-android/` is a front end of its own and deliberately not a port of the
wxWidgets one: wxWidgets has no Android build, and the desktop shape - modal
dialogues, a five-tab machine editor, several machines at once - is wrong for a
tablet. It is SDL2 for the window, input and sound, and Kotlin for the two screens.

| part | file |
|---|---|
| the machine: display, input, driving the core | `rpcemu_android.c` |
| launcher, ROM list | `android/.../MachineListActivity.kt` |
| the data directory and what has to be in it | `android/.../MachineStore.kt` |
| the SDL activity the machine runs in | `android/.../RPCEmuActivity.kt` |

Files live in `Android/data/uk.co.rpcemu.android/files`, which needs no permission
and is still reachable over MTP or `adb push`, so a ROM can be put in `roms/`
without a file picker. The podule ROMs and a CMOS template ship as assets and are
copied out on first run; HostFS is one of those podules, so without them RISC OS
never finds `!Boot` and stops at a Supervisor prompt.

## Input

`config.mousehackon` is set, so the guest's pointer goes where the finger is
rather than drifting relative to it.

RISC OS needs three buttons and Android offers one. `config.mousetwobutton` is set,
which puts Menu on the right button and Adjust on the middle, and:

- one finger is Select, two are Menu, three are Adjust;
- a single press held still for half a second becomes Menu, because an AVD (and a
  plain tablet mouse) gives one pointer and RISC OS is unusable without Menu. Any
  real movement cancels it, so dragging is unaffected;
- a real USB or Bluetooth mouse gets its buttons as they are.

Keys are handed to the core **one at a time and spaced out**, which is not a
nicety. RISC OS reads scan codes on an interrupt but only turns them into key
events on its 100Hz tick, so a press and its release that arrive together leave the
key state unchanged when the tick comes round and the keystroke is lost completely.
`SDL_PollEvent()` is drained dry before any instruction runs, so a tap whose down
and up were both queued during the previous batch arrives with nothing in between.
The spacing is measured both in timer interrupts raised and in turns of the main
loop; the second is what matters on a slow device, where every timer tick due can
accrue inside a single batch of emulated CPU. See `key_pump()` for the measurements
behind the two constants.

## What works today

`librpcemu_core.a` builds for Android with the NDK, in three configurations, all
of them clean:

| ABI | engine | files that differ |
|---|---|---|
| `arm64-v8a` | interpreter | `arm.c` |
| `arm64-v8a` | recompiler | `arm_dynarec.c`, `codegen_arm64.c` |
| `x86_64` | recompiler | `arm_dynarec.c`, `codegen_amd64.c` |

`arm64-v8a` is what a tablet runs. `x86_64` is what an Android emulator AVD runs,
which makes it the easier target to test on a desktop, and it has the advantage of
exercising the amd64 backend that everything else already uses.

The AVD runs the amd64 recompiler and RISC OS boots on it. The AArch64 recompiler
compiles here but is not shipped on any platform and still has an open fault (a
blank screen on a Pi 5), so the first thing to try on a real tablet should be the
interpreter.

## Building it

```sh
# One-off: the NDK. r27c is what this was developed against.
mkdir -p ~/Android/ndk && cd ~/Android/ndk
curl -LO https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
unzip -q android-ndk-r27c-linux.zip && rm android-ndk-r27c-linux.zip

# The core, for a device
NDK=~/Android/ndk/android-ndk-r27c
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DRPCEMU_BUILD_TESTS=OFF \
  -DRPCEMU_DYNAREC=OFF \
  -DRPCEMU_ENABLE_VNC=OFF \
  -DRPCEMU_ENABLE_GHOSTPDL=OFF
cmake --build build-android -j"$(nproc)"
```

No JDK, SDK or Gradle is needed for that; they only become necessary for an APK.
CI needs no download at all, because the GitHub runners already carry an NDK at
`$ANDROID_NDK_LATEST_HOME`.

`android-24` is not arbitrary. `broadcast_relay.c` calls `getifaddrs()`, which
bionic gained at API 24, so that is the floor. API 24 is Android 7, which is old
enough not to be a real constraint on tablets.

## What the port had to change, and why

- **`CMakeLists.txt`** names Android in the supported-platform guard. Android
  reports `CMAKE_SYSTEM_NAME` as `Android`, not `Linux`, so it was rejected
  outright.
- **`src/CMakeLists.txt`** skips `gui/` and `tools/` for Android. `gui/` requires
  SDL2, GTK 3 and libvncserver through pkg-config, and wxWidgets on top; none of
  that exists here. The Android front end is `src/gui-android/` instead.
- **`src/CMakeLists.txt`** gives Android the same null network layer and ISO-file
  CD-ROM backend as Windows and macOS. `network-tun.c` goes through
  `/dev/net/tun`, which needs root and which an app cannot open, and there is no
  CD-ROM to `ioctl`. NAT networking still works, because that is SLiRP in user
  space.
- **`podules/common/sound_out_null.c`** is new. `sound_out_sdl2.c` includes
  `<SDL.h>` unconditionally and was listed unconditionally, so SDL2 was really a
  compile-time requirement of the core even though CMake treated the link as
  optional - it only went unnoticed because every machine this had been built on
  had the headers. The backend is now chosen the way the MIDI and sound-input
  backends already were.

### The trap worth knowing about

`pkg_check_modules()` calls pkg-config directly, so it ignores
`CMAKE_FIND_ROOT_PATH` and answers with the **host's** packages. An arm64 Android
configure happily reported

```
-- Podule sound-out backend: SDL2
-- Podule MIDI + sound-in backend: ALSA
-- USB passthrough: libusb 1.0.27
```

all three from `/usr/lib/x86_64`, and would have compiled the core against x86
headers before failing somewhere much less obvious. The three pkg-config blocks
are therefore skipped for Android, and the CI job checks the resulting archive's
architecture rather than trusting the build's exit status - which is what would
notice that guard being lost.

## Building the APK

```sh
export JAVA_HOME=~/Android/jdk-17 ANDROID_HOME=~/Android/sdk
cd src/gui-android/android
gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Gradle drives the emulator's own top-level `CMakeLists.txt` and builds only the
`main` target, because building the desktop targets here would need wxWidgets and
GTK. SDL2 is fetched by URL and hash rather than found: pkg-config cannot be used
(see the trap above) and there is no Android SDL2 to find. The download is kept
outside the Gradle build tree so a clean does not throw it away.

The native library must be called `main`. SDL's `SDLActivity` loads `SDL2` and then
`main` by those names, and renaming it fails with nothing more informative than a
missing library.

## What is left

1. **Sound.** The sound thread runs and the buffers are consumed, but nothing is
   handed to an audio device.
2. **Suspending.** Going to the background is logged and otherwise ignored. The
   machine keeps running, and a proper answer saves state - which `savestate.c`
   already does.
3. **Settings.** Model, memory, VRAM and the rest are fixed in
   `machine_start()`. There is no editor, so the only choice offered is the ROM.
4. **A file picker.** Putting a ROM in place currently means a file manager, MTP or
   `adb push`.
5. **An arm64 device.** Nothing here has run on real hardware, and the AArch64
   recompiler is not shipped on any platform and still has an open fault (a blank
   screen on a Pi 5).
6. **Packaging.** A decision about F-Droid versus Play for a GPL application.

## What CI does

The `android` job builds all three configurations, checks each archive really is
for the target rather than the host, and confirms each configuration compiled the
engine it claims. It is **not** in `release`'s dependencies, because there is no
artefact to ship; it is in `notify`'s, because a job whose failures nobody is told
about is worse than no job.
