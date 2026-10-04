# Building RPCEmu Extended for macOS

macOS builds produce a universal **`RPCEmu.app`** bundle whose `rpcemu` binary
contains two slices, each with the recompiler for its own architecture:

| Slice    | Engine                | Backend            |
|----------|-----------------------|--------------------|
| `x86_64` | recompiler (dynarec)  | `codegen_amd64.c`  |
| `arm64`  | recompiler (dynarec)  | `codegen_arm64.c` ([arm64-dynarec.md](arm64-dynarec.md)) |

Each slice is built on a runner of its own architecture, then the two are fused
with `lipo` into `RPCEmu.app`, ad-hoc signed, and wrapped in a
drag-to-Applications `.dmg`.

## Minimum macOS

Both slices target **macOS 15**, and the bundle says so.

That figure is set by the libraries in the bundle, not by the emulator's own
code. MacPorts builds each port against the OS it runs on, so every dylib
bundled from a macOS 15 runner declares `LC_BUILD_VERSION minos 15.0`. Claiming
anything lower is a promise the app cannot keep: it launches, and dyld then
fails on the first dependency.

Each slice job prints what every staged file declares, so the claim can be
checked against a build rather than assumed.

## Dependencies

CI takes both slices' libraries from MacPorts, which publishes binary archives
for both architectures and installs to `/opt/local` on either. One prefix on
both sides is what lets the fuse step match the two slices' libraries by the
path they came from.

A local build can use **MacPorts or Homebrew**. The script uses whichever
`wx-config` and `pkg-config` are first on `PATH`, and stops before configuring,
naming anything missing.

MacPorts:

```sh
sudo port install wxWidgets-3.2 libsdl2 LibVNCServer libusb cmake ninja pkgconfig
sudo port select --set wxWidgets wxWidgets-3.2
```

The `port select` is not optional: wxWidgets installs as a framework under
`/opt/local/Library/Frameworks/`, and the select is what puts `wx-config` on
`PATH`.

Homebrew:

```sh
brew install wxwidgets sdl2 libvncserver libusb cmake ninja pkg-config
```

Homebrew installs libraries for one architecture only, so on Apple Silicon
build the arm64 slice with `./build-macos.sh --arch arm64`.

That `wxwidgets` formula is whatever Homebrew currently calls stable, not the
3.2 CI and the releases build against. Nothing here requires 3.2 specifically;
it is not a supported/unsupported line, just what is tested. To match CI
exactly (bisecting a platform-specific report, say), use the version-pinned
formula instead and link it, since it does not put `wx-config` on `PATH` by
itself:

```sh
brew install wxwidgets@3.2 sdl2 libvncserver libusb cmake ninja pkg-config
brew link --force wxwidgets@3.2
```

MacPorts has no such gap - `wxWidgets-3.2` is its newest wxWidgets port, so a
MacPorts build is on 3.2 either way.

Do **not** use the `+universal` variant. It has no binary archives, so it builds
from source, and the per-architecture slices give a better result anyway — each
gets its own recompiler backend.

## CI

The workflow (`.github/workflows/build.yml`) builds macOS natively on GitHub's
runners — the only place these builds are *executed and tested*:

- `macos-x86_64` on `macos-15-intel`: the x86_64 slice, running its JIT tests
  on the architecture they target.
- `macos-arm64` on `macos-15`: the arm64 slice, likewise.
- `macos-universal`: `lipo`s the two into `releases/macos/RPCEmu.app`, ad-hoc
  signs it and packages `releases/macos/*.dmg`. It installs nothing — `lipo`,
  `codesign` and `hdiutil` all come from the OS.

Each slice job asserts that every staged Mach-O file is its own architecture
before the two ever meet (`.github/scripts/macos/check-slice-arch.sh`), and reports
the minimum macOS each one declares.

`macos-15-intel` is the last x86_64 image GitHub offers, available until August
2027. The two slice jobs are independent, so when it goes, `macos-x86_64` and
`macos-universal` are deleted and `macos-arm64` is untouched.

Locally on a Mac, with the dependencies above installed (MacPorts or Homebrew):

```sh
./build-macos.sh --arch $(uname -m)   # this machine's slice + RPCEmu.app + .dmg
```

A bare `./build-macos.sh` builds both slices, which needs a Homebrew or MacPorts
of each architecture; see Dependencies above.

## Cross-compiling from Linux (osxcross)

For fast compile-iteration only: a Linux host **cannot run** the resulting
Mach-O binaries, so real verification is the native/CI build above.

It also **requires a macOS SDK you provide yourself** — it cannot be downloaded
(it comes from Xcode, behind an Apple ID). Produce a `MacOSX<NN>.sdk.tar.xz`
(see the osxcross README, "packaging the SDK"; needs SDK ≥ 11 for arm64) and:

```sh
mkdir -p macos-sdk && cp /path/to/MacOSX14.sdk.tar.xz macos-sdk/
./setup-macos-cross-build-env.sh     # builds osxcross + cross wxWidgets/SDL2
source ./macos-cross-env.sh
./build-macos.sh                     # cross-builds both slices + lipo
```

The cross build drops VNC and Ghostscript (extra cross-built libraries not
worth the effort for the unverified path); the native/CI build keeps them.

## Notable macOS-specific code

- `src/arm_dynarec.c` — `set_memory_executable()` uses POSIX `mprotect(RWX)` on
  the static JIT buffer for the Intel slice. The arm64 recompiler in
  `src/codegen_arm64.c` already allocates its code cache with `mmap(MAP_JIT)` and
  toggles `pthread_jit_write_protect_np()`, so the hardened runtime is satisfied
  when that slice is eventually shipped ([arm64-dynarec.md](arm64-dynarec.md)).
- `src/socket-compat.h` — macOS has no `MSG_NOSIGNAL`; SIGPIPE is suppressed
  per-socket via `SO_NOSIGPIPE` instead.
- `src/CMakeLists.txt` — macOS uses the portable ISO CD-ROM backend (like
  Windows); the Linux CD-ROM ioctl backend is not built. Networking is NAT on
  every platform and needs nothing from the host, so there is no platform
  difference there any more.

## Limitations

- The release is an **ad-hoc signed `RPCEmu.app`** in a `.dmg`, but it is **not
  notarised**. Gatekeeper therefore refuses the first launch; the user gets past
  it via System Settings > Privacy & Security > Open Anyway, documented in the
  README.
  Assembly, signing and the DMG all happen in `build-macos.sh`; wire in
  `notarytool` + `stapler` there and in the `macos-universal` CI job (behind
  repository secrets) if notarisation becomes available.
- The `.icns` icon is generated from `resources/rpcemu.png`, which is only
  256×256; the 512/1024 icon variants are upscaled. Supply a 1024×1024 master for
  crisp Retina rendering.
