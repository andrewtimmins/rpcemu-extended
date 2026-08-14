# Android

The emulator core cross-compiles for Android and is built by CI. There is **no
application yet**: nothing draws, nothing takes input, and there is no APK. What
exists is the half that turned out to be portable, and a build that keeps it that
way while the other half is written.

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

Compiling is not running. The AArch64 recompiler compiles here but is not shipped
on any platform and still has an open fault (a blank screen on a Pi 5), so the
first thing to run on a device should be the interpreter.

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
  that exists here. Android's front end will live in `src/gui-android/`.
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

## What is left, roughly in order

1. **A front end in `src/gui-android/`.** SDL2 has first-class Android support and
   an official project template, and SDL2 is already understood by the core, so
   that is the obvious route. It needs to build SDL2 for Android as a subproject
   rather than find it through pkg-config.
2. **A surface to draw on.** Everything that consumes the framebuffer today is on
   the wx side (`src/gui/emulator_host.cpp`, `headless_main.cpp`,
   `vnc_dialog.cpp`), so there is no GUI-free display consumer to inherit and the
   Android front end has to supply one.
3. **Input.** RISC OS is a three-button-mouse desktop with menus on the middle
   button. This needs a real design answer rather than a mouse emulation hack.
4. **Storage.** Scoped storage on Android 11+ against a data directory the user is
   expected to poke at - machines, roms, hostfs as ordinary folders. HostFS in
   particular assumes a normal filesystem.
5. **Packaging.** JDK, SDK and Gradle, and a decision about F-Droid versus Play
   for a GPL application.

## What CI does

The `android` job builds all three configurations, checks each archive really is
for the target rather than the host, and confirms each configuration compiled the
engine it claims. It is **not** in `release`'s dependencies, because there is no
artefact to ship; it is in `notify`'s, because a job whose failures nobody is told
about is worse than no job.
