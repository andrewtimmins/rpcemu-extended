# How to Compile RPCEmu

RPCEmu (Spork Edition) builds with **CMake**. This guide covers the **Linux** build
via the `build.sh` script at the project root; for the other platforms see
[docs/windows-build.md](docs/windows-build.md) and
[docs/macos-build.md](docs/macos-build.md).

The emulator core (`src/`) is C11. The wxWidgets front-end (`src/gui/`) is C++17.

---

## Quick start

```bash
./setup-build-env.sh    # once: install apt dependencies
./build.sh --zip        # release build + staged tarball
```

Run the staged binary:

```bash
./releases/linux/amd64/rpcemu-recompiler
```

Or run directly from the build tree (requires runtime data in the project root):

```bash
./build/bin/rpcemu-recompiler
```

End users who download the portable `.tar.gz` (rather than building) can install the
runtime shared libraries with `./setup-runtime-env.sh`. The `.deb` package declares
these dependencies (auto-derived via `dpkg-shlibdeps`), so apt installs them for you.

Debian packages install the binary to `/usr/bin`, bundled assets under
`/usr/share/rpcemu/`, and per-user data under `~/.local/share/rpcemu/` (or
`$XDG_DATA_HOME/rpcemu/`). On first run, default configs and machine templates
are copied into the user data directory. Override paths with `RPCEMU_DATADIR` or
`RPCEMU_RESOURCE_DIR` if needed.

**Important:** run RPCEmu from the project root (or from a staged release directory)
so it can find `configs/`, `machines/`, `roms/`, `resources/`, `poduleroms/`, and
`shared/` when using portable tarballs. Installed `.deb` packages resolve these
automatically.

---

## Build script reference

```bash
./build.sh                         # Linux release build (dynarec)
./build.sh --interpreter           # interpreter build (no dynarec)
./build.sh --debug                 # debug build (-debug suffix on binary name)
./build.sh --arch arm64            # Linux arm64 (native on Pi)
./build.sh --cross-arm64           # cross-compile Linux arm64 from x86_64
./build.sh --deb                   # Linux + .deb for selected arch
./build.sh --zip                   # Linux .tar.gz in releases/linux/
./build.sh --no-podules            # skip the guest ROMs under riscos-progs/
./build.sh --podules               # insist on the guest ROMs (fail if the ARM tools are absent)
./build.sh --clean                 # remove build trees and releases/
./build.sh --help                  # full option list
```

### Output locations

| Build | Binary | Staged release |
| --- | --- | --- |
| Linux dynarec (default) | `build/bin/rpcemu-recompiler` | `releases/linux/<arch>/rpcemu-recompiler` |
| Linux interpreter | `build/bin/rpcemu-interpreter` | `releases/linux/<arch>/rpcemu-interpreter` |
| Linux debug | `build/bin/rpcemu-recompiler-debug` | `releases/linux/<arch>/rpcemu-recompiler-debug` |
| Linux `.deb` | — | `releases/linux/<arch>/rpcemu_<version>_<arch>.deb` |

Supported Linux architectures: **amd64** and **arm64** (`--arch` or native host).

Release archive: `releases/linux/rpcemu_<version>_linux_<arch>.tar.gz`

The Linux release staging step copies `configs/`, `poduleroms/`, `gfxroms/`,
`netroms/`, `resources/`, `roms/`, `podules/` and `default/`, and pre-creates empty
`machines/` and `shared/` directories.

No machine is shipped. One the emulator invented would have no ROM and an empty disc,
so it could not start; **New…** creates one properly, fetching RISC OS from RISC OS
Open and seeding it from `default/`, which is why that directory ships.

---

## Dependencies

Installed automatically by `./setup-build-env.sh` on Debian/Ubuntu:

| Package | Purpose |
| --- | --- |
| `build-essential`, `cmake`, `pkg-config` | Build toolchain |
| `libwxgtk3.2-dev` | wxWidgets GUI (GTK 3 backend). **Must have wxWebRequest** for the package manager and the RISC OS download: 3.1.5 or later, built with a backend (libcurl on Linux). Not every distribution's is — see the note below. Configure refuses a wxWidgets without it rather than failing most of the way through the build. |
| `libsdl2-dev` | Audio output |
| `libvncserver-dev` | Built-in VNC server |
| `libusb-1.0-0-dev` | USB passthrough: handing a real host device to the guest |
| `libgs-dev`, `ghostscript` | In-process PostScript → PDF conversion (optional but recommended) |

Optional extras:

```bash
./setup-build-env.sh --cross-arm64  # aarch64-linux-gnu (cross from x86 PC)
./setup-build-env.sh --podules      # ARM binutils for podule ROM rebuild
```

---

## Manual CMake

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRPCEMU_DYNAREC=ON \
  -DRPCEMU_ENABLE_VNC=ON \
  -DRPCEMU_ENABLE_GHOSTPDL=ON

cmake --build build -j"$(nproc)"
./build/bin/rpcemu-recompiler
```

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `RPCEMU_DYNAREC` | ON | Link the dynamic recompiler (x86-64, i386 or arm64 hosts) |
| `RPCEMU_ENABLE_VNC` | ON | VNC server support (requires libvncserver) |
| `RPCEMU_ENABLE_GHOSTPDL` | ON | Link against Ghostscript/GhostPDL for in-process PDF conversion |
| `RPCEMU_ENABLE_WARNINGS` | ON | Extra compiler warnings (`-Wall -Wextra -Werror=switch`) |
| `RPCEMU_REQUIRE_LIBUSB` | OFF | Fail the build if libusb-1.0 is missing. The release scripts turn this ON so a platform cannot quietly ship without USB passthrough |

Interpreter build:

```bash
cmake -S . -B build -DRPCEMU_DYNAREC=OFF
cmake --build build -j"$(nproc)"
```

Debug build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

On **arm64** hosts (e.g. Raspberry Pi, Apple Silicon) the dynarec now builds a
native AArch64 backend ([docs/arm64-dynarec.md](docs/arm64-dynarec.md)), selected
automatically. It is new — validated under `qemu-aarch64` but not yet exercised on
real arm64 hardware — so if you hit trouble, fall back to the interpreter with
`--interpreter` (or `-DRPCEMU_DYNAREC=OFF`) and please report it.

Further detail: [docs/dynarec.md](docs/dynarec.md).

---

## GhostPDL / Ghostscript (in-process PDF conversion)

When `libgs-dev` is present at configure time, RPCEmu links against `libgs` and can
convert captured PostScript `.prn` files to PDF without external tools. Enable **Also
create PDF files** in *Settings → Parallel Port*.

For PCL/XPS print jobs, install full GhostPDL and point the build at it:

```bash
export GHOSTPDL_PREFIX=/opt/ghostpdl
./build.sh
```

---

## The guest ROMs (podules and the display driver)

`riscos-progs/` holds the RISC OS side of the emulator: the HostFS filer, the
RPCEmuSupport module, and the graphics card's display driver and its desktop
utility. **`build.sh` rebuilds these as part of every build**, so a change to any
of them is picked up without a flag, and the release directory always carries what
the sources say.

They need the ARM cross-assembler:

```bash
./setup-build-env.sh --podules     # once: arm-linux-gnueabi-as and related binutils
```

Without it the guest ROMs are **skipped with a note** and the committed ones are
used, so a machine that only builds the emulator (CI included) is unaffected.
`--podules` insists on building them, treating a missing toolchain as an error
rather than skipping it; `--no-podules` leaves them alone.

---

## Continuous integration and releases

GitHub Actions builds on every push/PR to `main`. Push a version tag (e.g. `v1.0.0`)
to publish a GitHub Release with the Linux tarball. Update `VERSION` before tagging.

---

## Troubleshooting builds

| Problem | Remedy |
| --- | --- |
| `cmake not found` | Run `./setup-build-env.sh` |
| wxWidgets not found | Install `libwxgtk3.2-dev` |
| `This wxWidgets was built without wxWebRequest support` | Your wxWidgets has `wxUSE_WEBREQUEST` set to 0, so `wx/webrequest.h` declares nothing. See **wxWidgets and wxWebRequest** below. Check what you have with `grep 'define wxUSE_WEBREQUEST ' $(wx-config --cflags \| tr ' ' '\n' \| grep '^-I' \| sed 's/^-I//')/wx/setup.h` |
| `'wxWebRequest' has not been declared` part-way through a build | The same problem on a tree from before that check existed. Update, reconfigure, and the error above will explain it. |
| VNC build fails | Install `libvncserver-dev`, or `-DRPCEMU_ENABLE_VNC=OFF` |
| Ghostscript not detected | Install `libgs-dev`, or `-DRPCEMU_ENABLE_GHOSTPDL=OFF` |
| `USB passthrough was required ... libusb-1.0 was not found` | Install `libusb-1.0-0-dev`, or `-DRPCEMU_REQUIRE_LIBUSB=OFF` to build without it |
| Dynarec misbehaves on arm64 (new backend) | Fall back with `./build.sh --interpreter` and report it |

### wxWidgets and wxWebRequest

The package manager and the RISC OS download are built on `wxWebRequest`, which
needs a backend inside wxWidgets: libcurl on Linux, WinHTTP on Windows,
NSURLSession on macOS. **wxWidgets turns `wxUSE_WEBREQUEST` off silently when its
own configure cannot find one**, and it is the distribution's build that decides
this, not anything you install afterwards. A new enough wxWidgets is necessary
but not sufficient.

Checked against the packages themselves:

| Distribution | `libwxgtk3.2-dev` | wxWebRequest |
| --- | --- | --- |
| Debian 12 bookworm, and Raspberry Pi OS built on it | 3.2.2 | **No** |
| Debian 13 trixie | 3.2.8 | Yes |
| Ubuntu 24.04 | 3.2.4 | Yes |

On bookworm, `apt install libcurl4-openssl-dev` changes nothing, because the
wxWidgets library is already built. The options are a newer distribution, or
building wxWidgets from source with libcurl development files present:

```sh
sudo apt install libcurl4-openssl-dev libgtk-3-dev
# from a wxWidgets 3.2.x source tree
./configure --with-gtk=3 --enable-webrequest && make -j4 && sudo make install
sudo ldconfig
```

Then point CMake at it if it is not first on the path:
`cmake -B build -DwxWidgets_CONFIG_EXECUTABLE=/usr/local/bin/wx-config`.

---

## Runtime troubleshooting

| Problem | Remedy |
| --- | --- |
| Emulator cannot find configs/ROMs | Run from the project root or a staged release directory |
| No audio | Ensure PulseAudio or PipeWire is running (SDL2) |
| No network | Confirm NAT is selected in machine settings; SLiRP is always enabled on Linux |
| No VNC menu item | Rebuild with `libvncserver-dev` installed |
| USB dialogue lists no devices | Built without libusb (check `BUILDINFO.txt`), or the host has not granted access to the device: see [USB devices](README.md#usb-devices) |
| Log file | Check `rpclog.txt` in the data directory |
