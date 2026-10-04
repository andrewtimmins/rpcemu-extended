# Building RPCEmu Extended for Windows

Windows builds are produced with **MinGW-w64** (not MSVC). Both the x86-64 build
and the native **ARM64** build ship the full-speed recompiler
(`rpcemu-recompiler.exe`) as the one to reach for, with the interpreter alongside
it - see [Windows on ARM](#windows-on-arm). The wxWidgets GUI is cross-platform,
so the Windows-specific code is confined to the C core (POSIX→Win32 shims) and
the build system.

`build-windows.sh` decides which of the two it is building from `$MSYSTEM`:
`MINGW64` gives amd64, `CLANGARM64` gives arm64, and neither (on Linux) gives the
amd64 cross build.

## Building

### Native on MSYS2 / MinGW-w64 (recommended)

This is what the CI `windows-amd64` job runs. From the **MINGW64** shell, install
the toolchain and dependencies (packages are prefixed `mingw-w64-x86_64-`):

```
toolchain cmake pkgconf wxwidgets3.2-msw SDL2 libvncserver
```

SLiRP is bundled (`src/slirp/`), so no package is needed for NAT networking.
Then:

```sh
./build-windows.sh          # builds + stages releases/windows/amd64/
./build-windows.sh --zip    # also writes releases/windows/rpcemu_<ver>_windows_amd64.zip
```

`build-windows.sh` builds the recompiler by default; pass `--interpreter` for the
(slower) interpreter build.

### Windows on ARM

Native ARM64, from the **CLANGARM64** shell (packages are prefixed
`mingw-w64-clang-aarch64-`, and that environment is clang rather than GCC):

```
toolchain cmake ninja pkgconf wxwidgets3.2-msw SDL2 libvncserver libusb
```

```sh
./build-windows.sh          # builds + stages releases/windows/arm64/
```

It is native: clang there reports `aarch64-w64-windows-gnu` and `file` calls the
result `PE32+ ... ARM64`. MSYS2's own runtime is x86-64 and runs under Windows'
Prism emulation, so the *build* is emulated and slow while its *output* is not.

**It ships the recompiler now**, same as every other platform - see
[arm64-dynarec.md](arm64-dynarec.md) for the history: a narrow, specific worry
(cache maintenance there wants `FlushInstructionCache` rather than the EL0
`dc cvau` / `ic ivau` sequence the backend falls back on) held it back after the
AArch64 dynarec was already shipping on macOS and Linux, and CI now settles that
worry empirically rather than by inspection - the `windows-arm64` job boots RISC
OS on the recompiler, on a real `windows-11-arm` runner, and requires it to draw
the desktop. That is why the shipped release trusts it.

A plain, flag-less `./build-windows.sh` here still stages the interpreter by
itself - a quick single build someone runs by hand. Pass `--dynarec` for the
recompiler alone, or `--both` as CI does, which is what actually ships: both
binaries, with `BUILDINFO.txt` and the archive written by the recompiler pass so
it is the one named as primary.

Built by the CI `windows-arm64` job, which also boots a real machine with a real
ROM and checks over VNC that RISC OS drew something - the same test the other
platforms get, but the only one still proving a brand new compiler and
architecture combination rather than a known-good one. The zip it produces is
attached to the GitHub Release like every other platform's; what CI cannot show
is somebody using it day to day - sound, USB, a window on a real ARM laptop.

### Cross-compiling from Linux

For fast compile-iteration on a Linux box. The resulting `.exe` cannot be run on
Linux (there is no wine step in CI), so real verification is the native/CI build
above.

```sh
bash setup-cross-build-env.sh   # builds the MinGW deps (wxWidgets/SDL2/libvncserver/…)
                                # into /usr/x86_64-w64-mingw32 (run once)
./build-windows.sh              # auto-detects cross mode and stages releases/windows/amd64/
```

The build links wxWidgets statically but needs the MinGW/SDL2/libvncserver
runtime DLLs to *run*; `build-windows.sh` bundles that DLL closure beside the
`.exe`s in the staged release.

## Platform-specific behaviour

- **Networking.** NAT/SLiRP for the guest, plus TCP-loopback for the HostCmd and
  DebugCmd control sockets (Windows lacks the AF_UNIX semantics used on Linux, so
  those default to ports 15590/15591). Winsock is initialised with
  `WSAStartup`/`WSACleanup`, and adapter enumeration for the ShareFS broadcast
  relay uses `GetAdaptersInfo`. Networking is NAT, the same as on every other
  platform.
- **Recompiler.** The dynarec was ported to the Windows x64 ABI: JIT→C helper
  calls are shuffled to the RCX/RDX/R8 argument registers with 32 bytes of shadow
  space, and the block prologue/epilogue preserve the callee-saved RSI/RDI. This
  is localised to `codegen_amd64.c` behind `_WIN32`; the Linux/macOS dynarec is
  unaffected.
- **Filesystem and system calls.** MinGW provides `<dirent.h>`, so the HostFS
  directory code is unchanged; the remaining shims (`_mkdir`,
  `GetDiskFreeSpaceEx`, `GetTickCount64`, `Sleep`) live in `src/rpcemu-win.h` and
  `src/socket-compat.h`.
- **Loadable podules** use `LoadLibrary`/`GetProcAddress` with a `.dll`
  extension; the built-in podules are always available.
- **CD-ROM** uses the portable ISO-file backend (the Linux ioctl backend is not
  built).
- **GUI.** Toolbar icons render from an embedded SVG pack (with 24×24 raster
  fallbacks), and the app/window/taskbar icons come from `resources/rpcemu.ico`
  and `.png`.

## Not supported on Windows

- **MSVC.** MinGW-w64 only — the codebase relies on GCC features (`__attribute__`,
  the codegen macros, `-std=gnu11`).
- **Real-drive CD-ROM.** ISO-file images only.
