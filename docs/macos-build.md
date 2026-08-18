# Building RPCEmu (Extended Edition) for macOS

macOS builds produce a universal **`RPCEmu.app`** bundle whose `rpcemu` binary
contains two slices:

| Slice    | Engine                       | Why                                                            |
|----------|------------------------------|----------------------------------------------------------------|
| `x86_64` | recompiler (dynarec)         | The JIT (`codegen_amd64.c`) emits x86-64 machine code.         |
| `arm64`  | interpreter                  | The native arm64 recompiler ([arm64-dynarec.md](arm64-dynarec.md)) is new and not yet enabled for macOS release builds. |

The two slices are built **separately**, fused with `lipo` into `RPCEmu.app`,
ad-hoc signed, and wrapped in a drag-to-Applications `.dmg`. On Apple Silicon the
x86_64 slice also runs (fast) under Rosetta 2, so an x86_64-only download is a
reasonable alternative to universal.

## Recommended: build on macOS (GitHub Actions)

The CI workflow (`.github/workflows/build.yml`) builds macOS natively on
GitHub's runners — the only place these builds get *executed and tested*:

- `macos-x86_64` (Intel runner): x86_64 + dynarec, runs the JIT unit test.
- `macos-arm64` (Apple Silicon runner): arm64 + interpreter, runs the test.
- `macos-universal`: `lipo`s the two slices into `releases/macos/RPCEmu.app`,
  ad-hoc signs it, and packages `releases/macos/*.dmg`.

Locally on a Mac you can reproduce this with:

```sh
brew install cmake ninja pkg-config wxwidgets sdl2 libvncserver
./build-macos.sh            # both slices + lipo -> RPCEmu.app + releases/macos/*.dmg
```

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
  notarised** — notarisation needs a paid Apple Developer ID, which the project
  does not have. Gatekeeper therefore refuses the first launch, and how the user
  gets past it depends on the release: Control-click > Open up to Sonoma, and
  System Settings > Privacy & Security > Open Anyway on Sequoia and Tahoe, where
  the Control-click route was removed. Both are documented in the README.
  Assembly, signing and the DMG all happen in `build-macos.sh`; wire in
  `notarytool` + `stapler` there and in the `macos-universal` CI job (behind
  repository secrets) if a Developer ID becomes available.
- The `.icns` icon is generated from `resources/rpcemu.png`, which is only
  256×256; the 512/1024 icon variants are upscaled. Supply a 1024×1024 master for
  crisp Retina rendering.
