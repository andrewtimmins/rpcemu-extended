# Building RPCEmu (Extended Edition) for Windows

Windows builds are produced with **MinGW-w64** (not MSVC) and ship the full-speed
recompiler (`rpcemu-recompiler.exe`). The wxWidgets GUI is cross-platform, so the
Windows-specific code is confined to the C core (POSIX→Win32 shims) and the build
system.

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
  relay uses `GetAdaptersInfo`. Bridged TUN/TAP networking is **not** available on
  Windows (see below).
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

- **Bridged (TUN/TAP) networking.** This needs a signed TAP kernel driver and
  overlapped I/O; NAT covers the vast majority of uses. Use NAT mode.
- **MSVC.** MinGW-w64 only — the codebase relies on GCC features (`__attribute__`,
  the codegen macros, `-std=gnu11`).
- **Real-drive CD-ROM.** ISO-file images only.
