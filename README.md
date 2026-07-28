# RPCEmu – Spork Edition

**A modern, cross-platform RISC OS machine on your desktop — and a serious platform for RISC OS development.**

Spork Edition is an extended fork of **[RPCEmu](http://www.marutan.net/rpcemu/)**, the
open-source emulator for Acorn's Risc PC and A7000, brought right up to date. It runs on
**Linux, Windows and macOS** with a clean wxWidgets interface and a full-speed dynamic
recompiler. Run **several machines** from a startup selector, **suspend and resume** a
running system to disk, and emulate the **Kinetic StrongARM card with the full 512 MB of
RAM** — booting RISC OS 5 straight to the desktop. Build software on it, too: edit on the
host and compile on the guest over a socket with **HostCmd**, or drive a whole machine
from an AI agent through the built-in **MCP server**. Round it out with an integrated
**debugger and machine inspector**, a **built-in VNC server** and genuine **headless
mode**, Access/ShareFS file sharing, high-resolution auto-detected displays, complete
**FPA10** floating-point emulation, and modern CMake build tooling.

Licensed under the **GNU GPL v2** — see `COPYING`.

---

## Highlights

- **Cross-platform** — runs on **Linux** (amd64 + arm64), **Windows** (amd64), and **macOS** (universal — Intel + Apple Silicon). The x86-64 dynamic recompiler gives full-speed emulation on Linux, Windows and Intel Macs. A native **arm64** recompiler is also implemented and validated under emulation ([docs/arm64-dynarec.md](docs/arm64-dynarec.md)); it is not yet enabled in prebuilt releases, pending testing on real arm64 hardware. Builds from a single CMake codebase. See [Supported systems](#supported-systems).
- **Kinetic StrongARM (512MB)** — emulates the Acorn Risc PC **Kinetic** StrongARM processor card and its full **512MB** of RAM: the 256MB the motherboard IOMD can address, plus two 128MB on-card SDRAM banks. Boots RISC OS 5 straight to the desktop.
- **Get RISC OS in one step** — RPCEmu ships no ROM, so a new installation has nothing to run. Creating a machine offers to fetch a ROM and the ready-made HardDisc4 hard disc from RISC OS Open and set them up on it. Stable 5.30 or the 5.31 nightly, with the licensing terms shown and agreed to first; also available headlessly as `--fetch-riscos`. An existing machine's hard disc is never overwritten. See [Getting RISC OS](#getting-risc-os).
- **Multi-machine configuration** — create, edit, clone, and delete machine profiles from a startup selector; each machine has isolated CMOS, HostFS, and hard disc storage.
- **Quick machine switching** — switch between machines via *File → Recent Machines* without restarting.
- **Save/load state, suspend & resume** — snapshot a machine's complete running state (CPU, RAM, VRAM, devices, and networking) to disk and restore it exactly. Use *File → Save State* / *Load State* for named snapshots, or *File → Suspend* to save and exit and pick up right where you left off via the machine's **Resume** button in the selector. Contributed by Nick Brown.
- **Shared clipboard** — copy text on the host and paste it in RISC OS, or the other way about. Off by default (*Settings → Share Clipboard with RISC OS*), since it puts your host clipboard within the guest's reach; the guest half loads itself and needs nothing installed. Text and images (PNG or JPEG). RiscOS Cloverleaf's design and interface, credited below. See [docs/clipboard.md](docs/clipboard.md).
- **Dual HostFS drives** — per-machine **HostFS** plus a common **Shared** drive (`shared/`) visible to all machines.
- **Access/ShareFS networking** — NAT-mode relay for Acorn Access and ShareFS file sharing between emulated and real machines.
- **Expansion cards (podules)** — assign emulated podules per machine (*Settings → Machine → Podules*): ROM, MIDI (AKA16/AKA12/MIDI Max, host MIDI via ALSA), and the Computer Concepts Lark sampler. Plugin ABI for adding more. See [docs/podules.md](docs/podules.md).
- **Full FPA10 emulation** — floating-point coprocessor with cycle-accurate timing; works with interpreter and dynarec.
- **Graphics card — display modes VRAM cannot reach** — an optional emulated expansion card with 15MB of its own display memory, so **2560 x 1440 in full colour** is available on a machine whose 2MB of VRAM otherwise stops at 800 x 600. An ordinary card in an ordinary EASI slot with its own GraphicsV driver in its ROM; off by default, and RISC OS keeps using VIDC20 until you run `*GfxCardOn`. See [docs/gfxcard.md](docs/gfxcard.md).
- **Pixel Perfect scaling** — optional integer scaling for sharp pixels (*Settings → Pixel Perfect*).
- **Built-in VNC server** — remote desktop access from any VNC client.
- **Command-line control** — launch straight into a named machine (`--machine <name>`), and resume its saved state (`--resume`) or load a specific one (`--state <file>`), in either the GUI or headless. Options, messages and exit statuses are the same on all three platforms. Contributed by David Ramsden. See [Command-line reference](#command-line-reference).
- **Headless mode** — run a machine with no GUI window, accessed entirely over VNC (`--headless --machine <name>`). Genuinely display-less: no GUI toolkit is initialised at all, so it runs on a headless server (on Linux, with no X11/Wayland session). See [Headless mode](#headless-mode).
- **HostCmd — drive the RISC OS command line from the host** — run guest commands from the host over a local socket and stream their output back, with the return code. Edit on the host (via HostFS), compile on the guest (`rpcemu-run -- cc -c hello`), or open an interactive RISC OS shell (`rpcemu-shell`). Ideal for IDE/LLM-driven development. See [docs/hostcmd.md](docs/hostcmd.md).
- **MCP server — drive RISC OS from Claude / an agent** — a [Model Context Protocol](https://modelcontextprotocol.io) server exposing tools to run guest commands, read/write/list files (via HostFS), capture and click the screen, and inspect/control the emulated ARM CPU (registers, memory, disassembly, breakpoints, watchpoints, single-step). Point Claude Code / Desktop at it for agent-driven RISC OS development. Setup and tool reference in [tools/mcp/README.md](tools/mcp/README.md).
- **Parallel port** — log raw output to a file, or a virtual printer that captures jobs to `.prn` files with optional in-process PDF conversion via Ghostscript.
- **Serial port** — log to file, or a TCP "modem" that dials real telnet BBSes (`ATDT host:port`) with a telnet client layer and 8-bit-clean X/Y/ZMODEM transfers. See [docs/peripherals.md](docs/peripherals.md).
- **Machine Inspector** — live CPU, disassembly, memory, peripheral, and debugger views with auto-refresh.
- **Integrated debugger** — pause/resume, single-step, breakpoints, and watchpoints; dynarec-aware via shared hooks.
- **Toolbar and status bar** — quick access to common actions; activity indicators for floppy, IDE, HostFS, and network.
- **Recent disc images** — quick access to recently used floppy and CD-ROM images.

---

## Architecture

The codebase splits into two layers:

| Layer | Path | Language | Role |
| --- | --- | --- | --- |
| **Core** | `src/` | C11 | Guest ARM CPU (interpreter or dynarec), devices, SLiRP, debugger |
| **GUI** | `src/gui/` | C++17 | wxWidgets front-end, threading bridge, dialogs, VNC server |

The GUI runs emulation on a **worker thread** (`EmulatorHost`). UI events are posted
as commands; video updates and debugger notifications come back through a `GuiBridge`
interface. Inspector snapshots are marshalled off the emulator thread as plain
`MachineSnapshot` structs.

Build with **CMake** — see [COMPILE.md](COMPILE.md) for full details.

---

## Project layout

| Path | Purpose |
| --- | --- |
| `src/` | Emulator core (CPU, VIDC, IOMD, IDE, FDC, FPA, HostFS, SLiRP, …) |
| `src/gui/` | wxWidgets front-end, machine inspector, configuration dialogs |
| `configs/` | Machine configuration files (`.cfg`, INI format) |
| `machines/<name>/` | Per-machine runtime data: `cmos.ram`, `hostfs/`, `hd4.hdf`, `hd5.hdf` |
| `shared/` | Common folder exposed as `HostFS::Shared.$` (created at startup if missing) |
| `roms/` | RISC OS ROM images — see [the project repository](https://github.com/andrewtimmins/rpcemu-extended) |
| `resources/` | Blank floppy/disc templates for *Disc → Floppy → Create Blank* |
| `poduleroms/` | Compiled extension ROM images (HostFS, ScrollWheel — the built-in Support podule) |
| `podules/` | Expansion-card (podule) ROMs — shipped system components, selectable per machine |
| `gfxroms/` | The graphics card's display driver, carried in that card's own ROM |
| `riscos-progs/` | RISC OS module source (HostFS, HostFSFiler, ScrollWheel, EtherRPCEm, RPCEmuSupport, RPCEmuGfx) |
| `packaging/` | Desktop entry and other packaging files |
| `build.sh` | Unified build and release script |
| `docs/dynarec.md` | ARM dynamic recompiler (build, behaviour, limitations) |
| `docs/arm64-dynarec.md` | AArch64 (arm64) dynarec backend |
| `docs/peripherals.md` | Serial and parallel ports (file logging, TCP modem, printer) |
| `docs/podules.md` | Expansion cards (podules): bundled devices, configuration, plugin ABI |
| `docs/gfxcard.md` | Graphics card: display modes beyond what VRAM allows, and its GraphicsV driver |
| `docs/clipboard.md` | Shared clipboard: copying text between the host and RISC OS |
| `docs/hostcmd.md` | HostCmd: drive the RISC OS command line from the host (`rpcemu-run`/`rpcemu-shell`) |
| `tools/mcp/README.md` | MCP server: drive a RISC OS machine from Claude / an agent (commands, files, screen, debugger). Setup + tool reference. |
| `docs/debugcmd.md` | DebugCmd: control the emulated CPU over a socket (registers, memory, disassembly, breakpoints, single-step) |
| `docs/debugger-tracing.md` | Debugger: exception trapping, SWI tracing, logging watchpoints |
| `docs/windows-build.md` | Building for Windows (MinGW-w64) |
| `docs/macos-build.md` | Building for macOS (universal binary) |
| `setup-build-env.sh` | Install build dependencies (Debian/Ubuntu) |

### Where your data lives

When **installed** (e.g. from the `.deb`), the binary and read-only support files
(ROMs, podule ROMs, templates) live under `/usr/share/rpcemu`, while your own
machines, configs, ROMs, HostFS and logs are kept in a visible **`~/RPCEmu/`** folder,
seeded from the shared templates on first run. An existing `~/.local/share/rpcemu` from
an earlier version is migrated automatically. The **portable** `.tar.gz` instead keeps
everything self-contained in its own folder.

These environment variables override the defaults:

| Variable | Meaning |
| --- | --- |
| `RPCEMU_DATADIR` | Writable data directory (machines, configs, logs). Otherwise the executable's directory or the current directory if it contains `configs/`, else the install prefix. |
| `RPCEMU_RESOURCE_DIR` | Read-only support files (ROM/config/podule templates). |
| `RPCEMU_NO_GUI_MESSAGES` | **Windows only.** Set to `1` to send `--help`, `--list-machines` and startup errors to stdout/stderr instead of a message box. See [Windows: messages appear in a dialog](#windows-messages-appear-in-a-dialog). |

---

## Getting started

### Supported systems

Each GitHub release ships prebuilt packages for four targets:

| Package | Platform | CPU core |
| --- | --- | --- |
| `rpcemu_*_amd64.deb` / `_linux_amd64.tar.gz` | Linux x86-64 | Recompiler (full speed) |
| `rpcemu_*_arm64.deb` / `_linux_arm64.tar.gz` | Linux arm64 (e.g. Raspberry Pi) | Interpreter (native arm64 recompiler implemented, not yet enabled in releases) |
| `rpcemu_*_windows_amd64.zip` | Windows x64 (10/11) | Recompiler (full speed) |
| `rpcemu_*_macos_universal.dmg` | macOS (Intel + Apple Silicon) | Universal app bundle — recompiler on Intel, interpreter on Apple Silicon |

**Linux** packages are built on **Ubuntu 24.04 LTS**; being dynamically linked, they run
on distributions whose system libraries are that version or newer:

| Distribution | Runs the prebuilt release? |
| --- | --- |
| Ubuntu 24.04 LTS (Noble) and newer (24.10, 25.04, …) | ✅ Yes — primary target |
| Linux Mint 22 / 22.x, Pop!_OS 24.04, Zorin 18, elementary 8, KDE neon (24.04 base) | ✅ Yes |
| Debian 13 (Trixie) and newer | ✅ Yes |
| arm64 / Raspberry Pi (Ubuntu 24.04+ base) | ✅ Yes — `arm64` package (interpreter; slower than x86) |
| Ubuntu 22.04 LTS, Debian 12 (Bookworm) and older | ❌ No — system libraries too old |

Linux minimum requirements: **glibc ≥ 2.34**, **libstdc++ from GCC 13.2+**
(`GLIBCXX_3.4.32`), and **wxWidgets 3.2** — standard on Ubuntu 24.04-era distributions.
On an older/different distribution (or for arm64), **build from source** instead:
`./setup-build-env.sh` then `./build.sh`. See [COMPILE.md](COMPILE.md).

**Windows**: extract the `windows_amd64.zip` anywhere and run `rpcemu-recompiler.exe`.
The MinGW/SDL2/libvncserver runtime DLLs are bundled in the zip, so there is nothing
else to install. Windows 10/11 (x64). Built with MinGW-w64 via MSYS2 — see
[Build for Windows](#build-for-windows) to build it yourself.

**macOS**: open the `macos_universal.dmg` and drag **RPCEmu** into **Applications**. It is
a universal app (Intel and Apple Silicon); on Apple Silicon the Intel slice runs at full
speed under Rosetta 2.

The app is ad-hoc signed but not notarised by Apple, so the first launch is blocked with
"RPCEmu cannot be opened because the developer cannot be verified". To open it the first
time only: **right-click (or Control-click) the app, choose Open, then Open again** in the
dialog. macOS remembers the choice and launches it normally thereafter. (On Sequoia and
later, if Open is not offered, go to **System Settings > Privacy & Security** and click
**Open Anyway**.) Nothing needs to be installed alongside it. As on Linux, machines,
configs, ROMs, HostFS and logs are written to a visible **`~/RPCEmu/`** folder, never
inside the app bundle, so the app stays read-only in Applications.

### Install the `.deb`

Install with **apt** — not `dpkg -i`, which reports missing dependencies but won't fetch
them. The runtime libraries (wxWidgets, SDL2, libvncserver, …) live in Ubuntu's
**`universe`** component, so make sure it's enabled first:

```bash
sudo add-apt-repository universe     # if not already enabled
sudo apt update
sudo apt install ./rpcemu_*_amd64.deb   # or _arm64.deb on a Pi
```

`apt` reads the package's declared dependencies and pulls them in. If `apt` complains the
packages are *"not installable"*, it's almost always because `universe` isn't enabled or
the package lists are stale — the two commands above fix that.

The portable `.tar.gz` instead bundles everything in one folder; run
`./setup-runtime-env.sh` once to install its runtime libraries. See [Run](#run) below.

### Build

```bash
./setup-build-env.sh    # install dependencies (Debian/Ubuntu)
./build.sh --zip        # build and package to releases/linux/amd64/
./build.sh --deb --zip  # + .deb package
```

See [COMPILE.md](COMPILE.md) for manual CMake, GhostPDL, and podule ROM rebuilds.

### Build for Windows

`build-windows.sh` builds the Windows package to `releases/windows/amd64/` and is
dual-mode:

- **Native, on Windows** — from an **MSYS2 MINGW64** shell (install the
  `mingw-w64-x86_64-` toolchain, cmake, wxwidgets3.2-msw, SDL2, libvncserver), just
  run `./build-windows.sh --zip`.
- **Cross-compile, from Linux** — run `./setup-cross-build-env.sh` once (builds
  wxWidgets/SDL2/libvncserver for the mingw target into the sysroot), then
  `./build-windows.sh --zip`.

It defaults to the recompiler (`rpcemu-recompiler.exe`); pass `--interpreter` for the
interpreter build. Runtime DLLs are bundled into the staged folder automatically. This
is exactly what the `windows-amd64` CI job runs.

### Build for macOS

`build-macos.sh` produces a **universal** `RPCEmu.app` — the Intel (x86-64) slice includes
the dynamic recompiler, the Apple Silicon (arm64) slice is the interpreter — fused with
`lipo`, then ad-hoc signed and wrapped in a drag-to-Applications `.dmg`. Dependencies come
from Homebrew. (A native arm64 recompiler now exists, including the `MAP_JIT` support the
hardened runtime needs, but it is not yet used for the Apple Silicon slice pending testing
on real arm64 hardware — see [docs/arm64-dynarec.md](docs/arm64-dynarec.md).) Build each
slice, then fuse and package:

```bash
./build-macos.sh --arch x86_64   # Intel slice (recompiler)
./build-macos.sh --arch arm64    # Apple Silicon slice (interpreter)
./build-macos.sh --fuse --zip    # lipo -> RPCEmu.app + releases/macos/*.dmg (+ .tar.gz)
```

The app bundle keeps its read-only payload in `Contents/Resources` and seeds writable data
into `~/RPCEmu` on first run. The `.icns` icon is built from `resources/rpcemu.png` with
`iconutil`, and the app is ad-hoc signed (Apple Silicon will not run an unsigned binary);
without an Apple Developer ID it is not notarised, so first launch needs a right-click >
Open. On a single machine each slice is built for its own architecture (the other builds
under Rosetta); the `macos-x86_64`, `macos-arm64`, and `macos-universal` CI jobs do exactly
this and fuse the result.

### Run

```bash
./releases/linux/amd64/rpcemu-recompiler
```

Run from the project root (or a staged release directory) so data files are found.

If you downloaded the portable **`.tar.gz`** release and see an error like
`error while loading shared libraries: libwx_gtk3u_core-3.2.so.0`, install the
runtime libraries once:

```bash
./setup-runtime-env.sh
```

(The **`.deb`** package pulls these in automatically via apt, so this step is only
needed for the portable tarball.)

### First launch

1. The **Machine Selector** dialog lists available configurations.
2. Use **New**, **Edit**, **Clone**, or **Delete** to manage machines.
3. Select a machine and click **Start**.
4. Place licensed RISC OS ROM files in `roms/<subdir>/` and select the ROM folder in
   the machine editor.

RPCEmu ships no ROM, so on a new installation step 4 has to happen before a machine
can start. Creating a machine offers to do it for you, as below.

### Getting RISC OS

RISC OS Open publish both a ROM and a ready-to-use hard disc, and RPCEmu can fetch
them and set them up on a machine. **New...** in the machine selector asks, and
downloads on OK before the machine editor opens. Afterwards the same download is
available from **Get RISC OS...** beside the ROM chooser in the machine editor.

There are two choices to make:

- **Version.** RISC OS **5.30**, the current stable release, or **5.31**, the nightly
  development build. Each ROM is named for its version, and a nightly also for the day
  it was built, so several can sit side by side in `roms/` and you can go back to an
  earlier one by picking it in the machine editor.
- **Whether to include the hard disc.** HardDisc4 carries applications, utilities,
  `!System` and a configured `!Boot`. Without it a machine starts at the supervisor
  prompt with nothing on its HostFS.

About 15 MB is downloaded. Files come from `riscosopen.org`, and every request
identifies itself as RPCEmu so that RISC OS Open can see what the traffic is.

Before anything is fetched, RPCEmu acknowledges whose work this is and asks you to
agree to the licence. RISC OS is copyright RISC OS Developments Ltd and is developed and
maintained by RISC OS Open Ltd, under the Apache License, Version 2.0, which the
dialogue reproduces in full. Some applications, logos and other material in the
downloads come from third parties under their own terms, so the dialogue also links to
[RISC OS Open's licensing page](https://www.riscosopen.org/content/documents/licences)
and to [donations](https://www.riscosopen.org/content/donations), which is how the work
is funded. ROOL's own copy of the licence is written to the root of the machine's hard
disc alongside the files it covers.

Nothing is written into place until the download and unpacking have both finished, so
cancelling, or losing the network part way, leaves the installation exactly as it was.

**An existing machine's hard disc is never overwritten.** In the machine editor the
hard-disc option is only available while that machine's disc is empty, and says so
when it is not; the ROM can always be fetched. To move an existing machine to a newer
ROM, fetch it there and select it. To get a fresh copy of the disc, make a new
machine, which costs nothing and cannot disturb the old one.

The same thing is available without the interface, which is useful for a scripted or
container install:

```bash
./rpcemu-recompiler --fetch-riscos --accept-licence           # stable, with the disc
./rpcemu-recompiler --fetch-riscos=nightly --accept-licence   # the development build
./rpcemu-recompiler --fetch-riscos --no-disc --accept-licence # just the ROM
```

`--accept-licence` is the same agreement the dialogue asks for. The acknowledgement is
printed either way, with the address of the licence rather than its 202 lines; without
the option nothing is downloaded and the exit status is **1**.

It installs and exits, printing what it created, and needs no display.

### Skipping the machine selector

To launch the GUI straight into a known machine, name it with `--machine`:

```bash
./rpcemu-recompiler --machine <name>
```

The selector is bypassed and the machine boots immediately. On its own, `--machine`
performs a plain boot and leaves any saved state untouched. The selector's **Resume**
and **Load State** actions have command-line equivalents:

```bash
./rpcemu-recompiler --machine <name> --resume          # resume its saved state
./rpcemu-recompiler --machine <name> --state <file>    # load a specific state file
```

- `--resume` loads the machine's own snapshot (`machines/<name>/suspend.state`). As
  in the GUI, the snapshot is **consumed** on success — renamed to `.bak`, so it is
  recoverable but not resumed again on the next launch.
- `--state <file>` loads an explicit state file and **leaves it in place**.
- The two are mutually exclusive, and both require `--machine`.
- Both also work with `--headless`.

An unknown machine name is reported and exits with status 2 without opening a window;
so does `--resume` with no saved state, or a `--state` file that does not exist. A
state file that fails to load is a warning, not an error — the machine performs a
normal boot instead, matching the GUI's behaviour.

The name must match the config file's case on case-sensitive filesystems (most Linux
setups); `--list-machines` prints the names as spelled.

### Command-line reference

The same options work on Linux, macOS and Windows, so a command line or a script is
portable between them.

| Option | Effect |
| --- | --- |
| `--machine <name>` | Run this machine, skipping the selector. Also accepts `--machine=<name>`. |
| `--resume` | Resume the machine's own snapshot, consuming it to `.bak`. Requires `--machine`. |
| `--state <file>` | Load an explicit state file, leaving it in place. Requires `--machine`. |
| `--headless` | Run with no GUI window, over VNC. Requires `--machine`. |
| `--list-machines` | List the available machine configs and exit. |
| `--fetch-riscos[=which]` | Download RISC OS from RISC OS Open, unpack it, create a machine and exit. `which` is `stable` (default) or `nightly`. |
| `--no-disc` | With `--fetch-riscos`, fetch the ROM only. |
| `--accept-licence` | Required by `--fetch-riscos`: agrees to the licensing terms of what is downloaded, which are printed first. |
| `-h`, `--help` | Show usage and exit. |

Exit status is **0** on success and **2** for a usage error — an unknown option, a
stray argument, an unknown machine, a missing state file, or `--resume`/`--state`
without `--machine`. `--resume` and `--state` cannot be combined.

Only long options are accepted. RPCEmu takes no positional arguments, so an
unrecognised option or a stray argument is reported rather than ignored. On Windows,
DOS-style switches such as `/H` are rejected too — use `--help`.

### Windows: messages appear in a dialog

On Windows the emulator is built as a GUI application so that double-clicking it
never opens a console window. Such a program has no terminal to write to, so the
messages above — `--help`, `--list-machines`, and any startup error — are shown in a
**message box** instead. The options, the exit codes and the behaviour are identical
to Linux and macOS; only the presentation differs.

For scripting, set `RPCEMU_NO_GUI_MESSAGES=1` to send that text to stdout/stderr
instead of a dialog:

```bat
set RPCEMU_NO_GUI_MESSAGES=1
rpcemu-recompiler.exe --list-machines > machines.txt
```

This matters for automation: a message box waits for someone to click OK, which would
otherwise block a script or a scheduled task indefinitely. Redirect the output (to a
file or a pipe) when using it — a GUI application launched without redirection has
nowhere to write, so the text is simply lost. The variable has no effect on Linux or
macOS, where output always goes to the terminal.

Headless mode (`--headless`) always writes to the console on every platform, since it
is driven from a terminal or a service manager where a dialog would be useless.

### Headless mode

A machine can be run without the GUI window and accessed entirely over the
built-in VNC server — useful for servers or always-on machines:

```bash
./rpcemu-recompiler --headless --machine <name>
```

- `--machine <name>` selects a machine by its config name (the file in `configs/`,
  with or without the `.cfg` suffix). It is required in headless mode, since there
  is no interactive selector. On its own — without `--headless` — it starts the GUI
  on that machine, skipping the selector (see above).
- `--resume` and `--state <file>` work here too, so a headless machine can be brought
  back up from a snapshot — useful when a service manager restarts it.
- `--list-machines` prints the available machine names and exits.
- `--help` (or `-h`) prints usage and exits. All three of these run without a display.
- The chosen machine **must have the VNC server enabled** (`vnc_enabled=1`) in its
  configuration; headless mode refuses to start otherwise, as there would be no way
  to reach the machine. The VNC port/password come from that same config.
- Press **Ctrl-C** (or send `SIGTERM`) to shut down cleanly — CMOS, disc images, and
  configuration are saved on exit, just as when closing the GUI window.

Headless mode is genuinely display-less: it is handled before any GUI toolkit is
initialised, so it needs **no display or desktop session** on any platform — on Linux
no X11/Wayland session, on Windows no interactive desktop, on macOS no window server.
It therefore runs happily on a bare server or under a service manager.

Data is located via `$RPCEMU_DATADIR`, else the executable or current directory if it
contains a `configs/` folder, else the install prefix.

---

## Machine configuration

Each machine is defined by a `.cfg` file in `configs/` and a data directory under
`machines/<name>/`.

| Setting | Options |
| --- | --- |
| **Model** | RiscPC ARM610/710/810/StrongARM, Kinetic StrongARM (512MB), A7000, A7000+ (experimental), Phoebe (experimental) |
| **RAM** | 4, 8, 16, 32, 64, 128, 256 MB, or 512 MB (Kinetic) |
| **VRAM** | None or 2 MB |
| **ROM** | Subdirectory under `roms/` containing ROM components |
| **Refresh rate** | 20–100 Hz |
| **Network** | Off, NAT, Ethernet Bridging, IP Tunnelling |
| **Hard discs** | HardDisc 4 and 5 — create 256 MB, 512 MB, 1 GB, or 2 GB images |

Configuration keys are stored under a `[General]` group (wxFileConfig INI format).
NAT port-forward rules are stored in a separate `[nat_port_forward_rules]` group.

---

## HostFS and Shared drives

Two filing system icons appear on the RISC OS icon bar:

| Icon | RISC OS path | Host path | Scope |
| --- | --- | --- | --- |
| **HostFS** | `HostFS::HostFS.$` | `machines/<name>/hostfs/` | Per-machine |
| **Shared** | `HostFS::Shared.$` | `shared/` | All machines |

Use HostFS for machine-specific files and Shared for utilities or files you want
available across configurations.

---

## Machine Inspector and debugger

Open **Debug → Machine Inspector…** (or use the toolbar button).

| Tab | Contents |
| --- | --- |
| **CPU** | Registers R0–R15, CPSR, mode, MMU state, dynarec/interpreter, performance |
| **Disassembly** | ARM disassembly at a chosen address, optional follow-PC |
| **Memory** | Hex dump of emulated memory at a chosen address |
| **Debugger** | Run/Pause/Step, breakpoint and watchpoint lists, last halt reason |
| **Trace** | Exception traps, SWI tracing, and logging watchpoints — see [docs/debugger-tracing.md](docs/debugger-tracing.md) |
| **Peripherals** | VIDC, IOMD IRQ/timers, floppy, IDE, podule slot summary |

Auto-refresh runs every 500 ms by default. Breakpoints and watchpoints work while
the dynarec is active — `arm_dynarec.c` checks `debugger_requires_instruction_hook()`
before executing translated blocks.

---

## Serial and parallel ports

Configure via **Settings → Serial…** and **Settings → Parallel…**. The Risc PC has a
single hardware serial port (the 16550 UART at `0x3F8`), so only one **Serial** port
is exposed.

| Port | Modes |
| --- | --- |
| **Serial (0x3F8)** | Disabled, log to file, TCP modem (telnet) |
| **Parallel (LPT)** | Disabled, log to file, virtual printer |

- **Log to file** captures the raw byte stream the guest sends — handy for debugging
  or capturing print/serial output.
- **TCP modem** answers the Hayes AT command set and `ATDT host:port` opens a real TCP
  connection. It speaks telnet and negotiates binary mode, so telnet BBSes work and
  X/Y/ZMODEM transfers stay 8-bit clean. `+++` (guard-timed) returns to command mode;
  `ATH` hangs up.
- **Virtual printer** writes `.prn` files to a chosen folder
  (default: `machines/<name>/printjobs/`); with Ghostscript support, enable **Also
  create PDF files** for automatic conversion.

Physical host passthrough (`/dev/tty*`) is not yet implemented. Full details,
including AT commands and how RISC OS drives each port, are in
[docs/peripherals.md](docs/peripherals.md).

---

## Keyboard and host controls

RPCEmu does **not** bind any host keyboard shortcuts, so every key — including the
function keys (**F12** for the RISC OS command line, etc.) and Ctrl combinations —
passes straight through to RISC OS. All emulator actions (screenshot, reset, floppy
load/eject, full-screen, mute, machine settings, and the debugger Run/Pause/Step
controls) are available from the menus and the toolbar instead.

| Key | Action |
| --- | --- |
| **Ctrl+End** | Release the captured mouse, or exit full-screen |

The toolbar provides one-click access to screenshot, floppy load, CD-ROM ISO load,
reset, mute, full-screen, machine settings, and debugger controls.

---

## FPA (Floating Point Accelerator) emulation

Complete FPA10 coprocessor emulation in `src/fpa.c`:

- **Dyadic:** ADF, MUF, SUF, RSF, DVF, RDF, POW, RPW, RMF, FML, FDV, FRD, POL
- **Monadic:** MVF, MNF, ABS, RND, SQT, LOG, LGN, EXP, SIN, COS, TAN, ASN, ACS, ATN, URD, NRM
- **Conversion:** FIX, FLT (all IEEE rounding modes)
- **Comparison:** CMF, CMFE, CNF, CNFE with NaN handling
- **Transfer:** LDF, STF, LFM, SFM

Cycle costs are modelled (e.g. 10 cycles for load/store, 150 for SIN/COS/TAN).
Works with both interpreter and dynarec. See [docs/dynarec.md](docs/dynarec.md) for
how the JIT is built and when it falls back to interpretation.

---

## Differences from upstream RPCEmu

- Kinetic StrongARM processor-card emulation with 512MB RAM (two on-card SDRAM banks), booting RISC OS 5 to the desktop
- wxWidgets front-end with machine selector, toolbar, and integrated debugger
- Multi-machine configuration with isolated per-machine storage
- Quick machine switching and recent-machines menu
- Dual HostFS drives (per-machine + shared)
- Access/ShareFS broadcast relay for NAT networking
- Full FPA10 emulation with cycle timing
- Pixel Perfect integer scaling
- Built-in VNC server
- Headless mode for display-less servers (run a machine over VNC with no GUI)
- HostCmd: drive the guest RISC OS command line from the host (`rpcemu-run`/`rpcemu-shell`) for edit-on-host/compile-on-guest workflows
- MCP server for agent-driven RISC OS development: run commands, edit/build, screenshot, and inspect/control the emulated CPU (see `tools/mcp/`)
- Virtual printer with optional Ghostscript PDF conversion
- Serial log-to-file and a real telnet TCP modem (dial BBSes, 8-bit-clean transfers)
- Machine Inspector with disassembly and memory browser
- Dynarec debugger hooks for consistent breakpoint/watchpoint behaviour
- Debugger exception trapping, SWI tracing, and logging watchpoints (see [docs/debugger-tracing.md](docs/debugger-tracing.md))
- Native arm64 (AArch64) recompiler backend, in addition to upstream's x86 dynarec — implemented and validated under emulation, not yet enabled in prebuilt releases (see [docs/arm64-dynarec.md](docs/arm64-dynarec.md))
- Robustness & memory-safety hardening: bounds-checked HFE/ADF disc-image and HostFS input handling, FPA faults raised as undefined instructions rather than aborting the emulator, and a fixed use-after-free on GUI shutdown
- CMake build system, cross-platform: Linux (amd64 and arm64), Windows (amd64, MinGW-w64), and macOS (universal — Intel + Apple Silicon)

---

## Troubleshooting

| Symptom | Remedy |
| --- | --- |
| `error while loading shared libraries: …` (tarball) | Run `./setup-runtime-env.sh` to install the runtime libraries (wxWidgets, SDL2, libvncserver, Ghostscript) |
| Window does not appear / configs not found | Run from the project root or a staged release directory |
| No audio | Ensure PulseAudio or PipeWire is running (SDL2) |
| No network | Select NAT in machine settings; SLiRP/NAT is always compiled in (Linux and Windows) |
| ROM not found | Place ROM files in `roms/<subdir>/` and select the folder in machine settings |
| Machine data not persisting | Check that `machines/<name>/` exists and is writable |
| VNC option missing | Rebuild with `libvncserver-dev` installed |
| PDF conversion unavailable | Install `libgs-dev` and rebuild; runtime needs Ghostscript resource files |
| Diagnostic log | See `rpclog.txt` in the data directory |

---

## Contributing

Issues and pull requests are welcome, especially around debugger, inspector, and
networking features.

---

## License and credits

- Licensed under the **GNU General Public License v2**. See `COPYING`.
- Based on **[RPCEmu](http://www.marutan.net/rpcemu/)** — the open-source Acorn
  Risc PC and A7000 emulator by Sarah Walker, Peter Howkins, Matthew Howkins
  and the RPCEmu contributors, hosted at <http://www.marutan.net/rpcemu/>. RPCEmu is distributed
  under the GNU GPL v2; this fork inherits and complies with that license.
- The **podule (expansion card) subsystem** — the podule API/ABI and the bundled
  podule implementations under `src/podules/` — is derived from
  **[Arculator](https://b-em.bbcmicro.com/arculator/)**, Sarah Walker's Acorn
  Archimedes emulator, also distributed under the GNU GPL v2. Copyright of that
  code remains with **Sarah Walker** and the Arculator contributors.
- The bundled **SLiRP** user-mode networking stack under `src/slirp/` originates
  with **Danny Gasparovski** and carries 4.4BSD-derived code from **The Regents
  of the University of California**. Fixes to its IP fragment reassembly have
  been backported from **[libslirp](https://gitlab.freedesktop.org/slirp/libslirp)**,
  which maintains the descendant of that code: commit `c5927943` by **Samuel
  Thibault** (CVE-2019-15890) and commit `9bd6c591` by **Marc-André Lureau**
  (CVE-2020-1983). Copyright for those changes remains with their authors and the
  libslirp contributors; see the provenance note at the top of
  `src/slirp/ip_input.c`.
- The **graphics card** follows the precedent set by
  **[ViewFinder](https://www.zeridajh.org/hardware/viewfinder/)**, **John
  Kortink's** graphics expansion card for the Acorn Risc PC, which showed that a
  card-hosted framestore driven by its own display driver could take the machine
  well beyond VIDC20's limits. No ViewFinder code, firmware or programming
  interface is used: the register interface here is our own and the driver is
  written from the GraphicsV documentation in the RISC OS sources. The
  acknowledgement is to the idea, gratefully.
- The **shared clipboard** is **RiscOS Cloverleaf's** design, from the
  RpcemuHelper module in their RPCEmu fork at
  <https://github.com/riscoscloverleaf/rpcemu>. Their SWI interface and reason
  codes are kept exactly, so their guest module and ours are interchangeable, and
  `src/hostclipboard.c` is derived from theirs and carries their copyright (GNU
  GPL v2). The guest module is theirs too (2-clause BSD), with wheel scrolling
  removed, renamed, and starting its own task; our changes are noted at the top of
  each file we touched. It is built inside the emulator with the RISC OS DDE. Two of
  their ideas do the heavy lifting: the guest hands the host RISC OS's own UCS
  conversion table, so text is converted through the alphabet the machine is
  configured for, and the host announces a change with a pollword the guest's task
  waits on, so neither side polls. The Latin-1 UCS table the module carries is
  **NetSurf's** (Copyright 2005 **John M Bell**, GNU GPL v2), by way of
  Cloverleaf's `ucstables.c`. See `docs/clipboard.md`.
- **SyncClock** is **DEEJ Technology PLC's** module (Copyright 2002, GNU GPL v2),
  carried in the expansion ROM as `poduleroms/syncclock,ffa`. It re-reads the
  emulated real-time clock every ten seconds and sets RISC OS's soft copy from it,
  which is what puts the clock right after a machine has been suspended or a state
  is resumed later. Theirs is the design and the code: it is supplied here
  translated from their BBC BASIC assembler source into the GNU as syntax the other
  guest modules use, so it builds with them, and the assembled code is
  byte-identical to the module built from their original. See
  `riscos-progs/SyncClock/`.
- Spork Edition enhancements by Andy Timmins and contributors.
- Machine save/load state (suspend & resume) contributed by **Nick Brown**, whose
  outstanding item on that work, putting the clock right on resume, is why
  SyncClock is here.
- Command-line options (`--machine` for the GUI as well as headless, `--resume`
  and `--state`), with consistent messages and exit statuses across Linux,
  Windows and macOS; the macOS application bundle fixes that made the shipped
  `.app` runnable on a Mac other than the one that built it, along with the
  bundling and verification work behind them; and the cross-platform
  command-line and VNC smoke tests (`tests/cli_smoke.sh`, `tests/vnc_smoke.py`).
  Contributed by **David Ramsden**.
