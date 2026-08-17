# RPCEmu – Extended Edition

**A modern ('ish), cross-platform RISC OS machine on your desktop — and a serious platform for RISC OS development.**

Extended Edition is an extended fork of **[RPCEmu](http://www.marutan.net/rpcemu/)**, the
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

- **Cross-platform** — runs on **Linux** (amd64 + arm64), **Windows** (amd64 + a native arm64 build, interpreter, from CI artifacts), and **macOS** (universal — Intel + Apple Silicon). The x86-64 dynamic recompiler gives full-speed emulation on Linux, Windows and Intel Macs. A native **arm64** recompiler is also implemented and validated under emulation ([docs/arm64-dynarec.md](docs/arm64-dynarec.md)); it is not yet enabled in prebuilt releases, pending testing on real arm64 hardware. Builds from a single CMake codebase. See [Supported systems](#supported-systems).
- **Kinetic StrongARM (512MB)** — emulates the Acorn Risc PC **Kinetic** StrongARM processor card and its full **512MB** of RAM: the 256MB the motherboard IOMD can address, plus two 128MB on-card SDRAM banks. Boots RISC OS 5 straight to the desktop.
- **Get RISC OS in one step** — RPCEmu ships no ROM, so a new installation has nothing to run. Creating a machine offers to fetch a ROM and the ready-made HardDisc4 hard disc from RISC OS Open and set them up on it. Stable 5.30 or the 5.31 nightly, with the licensing terms shown and agreed to first; also available headlessly as `--fetch-riscos`. An existing machine's hard disc is never overwritten. Needs a wxWidgets with `wxWebRequest`, which Debian 12 and Raspberry Pi OS have not got: see [COMPILE.md](COMPILE.md#wxwidgets-and-wxwebrequest). See [Getting RISC OS](#getting-risc-os).
- **Multi-machine configuration** — create, edit, clone, and delete machine profiles from a startup selector; each machine has isolated CMOS, HostFS, and hard disc storage.
- **Quick machine switching** — switch between machines via *File → Recent Machines* without restarting.
- **Package manager** — install software packaged for RISC OS straight onto a machine's disc, from the same repositories a real machine uses: over 350 applications, games, fonts and libraries from RISC OS Open, the RISC OS Community, and the Archimedes Software Preservation Project's preserved commercial games. *Tools → Package Manager*, or headlessly with `--pkg-list` and `--pkg-install`. **The repository list is yours**: add, edit, disable or remove sources under *Sources…*, or edit the plain-text `pkgsources` file directly. One-click section filters (*Games*, *Graphics*, *Desktop*…, with *All* to clear) sit above the list and narrow alongside the search box. Downloads are checked against the index's MD5, and what each package installed is recorded on that machine's disc in the RISC OS Packaging Project's own format, so it removes cleanly and other RISC OS package tools can see it. Needs a wxWidgets with `wxWebRequest`, which Debian 12 and Raspberry Pi OS have not got: see [COMPILE.md](COMPILE.md#wxwidgets-and-wxwebrequest). See [docs/packages.md](docs/packages.md).
- **Save/load state, suspend & resume** — snapshot a machine's complete running state (CPU, RAM, VRAM, devices, and networking) to disk and restore it exactly. Use *File → Save State* / *Load State* for named snapshots, or *File → Suspend* to save and exit and pick up right where you left off via the machine's **Resume** button in the selector. Contributed by Nick Brown.
- **Shared clipboard** — copy text on the host and paste it in RISC OS, or the other way about. Off by default (*Settings → Share Clipboard with RISC OS*), since it puts your host clipboard within the guest's reach; the guest half loads itself and needs nothing installed. Text and images (PNG or JPEG). RiscOS Cloverleaf's design and interface, credited below. See [docs/clipboard.md](docs/clipboard.md).
- **Dual HostFS drives** — per-machine **HostFS** plus a common **Shared** drive (`shared/`) visible to all machines. **The HostFS folder is configurable per machine** (*Settings → Machine → System*), so several machines can share one folder, which is handy when testing the same software across different configurations. Leave it empty for the machine's own folder, as before. See [docs/hostfs.md](docs/hostfs.md).
- **Access/ShareFS networking** — NAT-mode relay for Acorn Access and ShareFS file sharing between emulated and real machines.
- **Expansion cards (podules)** — assign emulated podules per machine (*Settings → Machine → Podules*): ROM, MIDI (AKA16/AKA12/MIDI Max, host MIDI via ALSA), and the Computer Concepts Lark sampler. Plugin ABI for adding more. See [docs/podules.md](docs/podules.md).
- **Full FPA10 emulation** — floating-point coprocessor with cycle-accurate timing; works with interpreter and dynarec.
- **MMU access permissions — ADFFS works** — RPCEmu did not enforce page access permissions when an address translation was already cached, so a User-mode write to a Supervisor-only page succeeded instead of faulting. That is why [ADFFS](https://www.jaspp.org.uk/) told people to avoid RPCEmu from 2013 onwards: its JIT uses page protection to detect self-modifying code, and without working faults very few games ran. Fixed, with a 19-check regression test, so ADFFS and the preserved games that need it run here. See [docs/mmu-permissions.md](docs/mmu-permissions.md).
- **Graphics card — display modes VRAM cannot reach** — an optional emulated expansion card with 15MB of its own display memory, so **2560 x 1440 in full colour** is available on a machine whose 2MB of VRAM otherwise stops at 800 x 600. An ordinary card in an ordinary EASI slot with its own GraphicsV driver in its ROM; off by default, and RISC OS keeps using VIDC20 until you run `*GfxCardOn`. See [docs/gfxcard.md](docs/gfxcard.md).
- **USB — real devices from the host, in RISC OS** — an emulated **OHCI** host controller on its own expansion card with four ports, carrying RISC OS Open's own USB stack in its ROM, so nothing needs installing in the guest. Plug a device on the host into a port from *Settings → USB…* and RISC OS enumerates it and names it as the real hardware, reading its descriptors, strings and serial number over the emulated bus. Keyboards and mice work immediately, since HID is compiled into USBDriver. Streaming devices work: isochronous transfers are implemented, so a camera's packets reach the guest a frame at a time, although nothing in RISC OS will display a webcam for you. **USB drives work too, and mount**: the card's ROM carries the SCSI modules and our own **MultiFS**, which reads what is actually on the stick, so a drive appears on the icon bar under its volume name and opens with the Filer. **FAT12/16/32 and exFAT are read and written**; files can be saved, renamed and deleted, directories made and removed, all keeping their long names in both directions, so a file written in RISC OS is the same file when the stick goes back into a PC. **NTFS is read only** - reading it is a large job and writing it safely is a far larger one, so MultiFS refuses rather than risk a volume chkdsk cannot repair. RISC OS file types survive on FAT, kept by the convention noted in the credits. Verified on Linux, and untested on Windows and macOS. See [USB devices](#usb-devices) and [docs/usb.md](docs/usb.md).
- **Pixel Perfect scaling** — optional integer scaling for sharp pixels (*Settings → Pixel Perfect*).
- **Built-in VNC server** — remote desktop access from any VNC client. Port and password belong to the machine, so several machines can each have their own and run at the same time; `rpcemu.cfg` supplies them before any machine is chosen and as the default for a machine that does not say. **A VNC client is not limited to typing at the guest:** `Ctrl+Alt+Shift+M` brings up a control menu over the running machine, to reset it or shut the emulator down. It is drawn into the VNC display only, so a local user never sees it, and it works whether the session is headless or an ordinary desktop one you have reconnected to. See [docs/vnc.md](docs/vnc.md).
- **Command-line control** — launch straight into a named machine (`--machine <name>`), and resume its saved state (`--resume`) or load a specific one (`--state <file>`), in either the GUI or headless. Options, messages and exit statuses are the same on all three platforms. Contributed by David Ramsden. See [Command-line reference](#command-line-reference).
- **Headless mode** — run a machine with no GUI window, accessed entirely over VNC (`--headless --machine <name>`). Genuinely display-less: no GUI toolkit is initialised at all, so it runs on a headless server (on Linux, with no X11/Wayland session). **Without `--machine` it offers the machine list over VNC**, so a remote emulator no longer has to be told which machine to run on the command line. See [Headless mode](#headless-mode) and [docs/vnc.md](docs/vnc.md).
- **Pyromaniac Networking — share a virtual network with other emulators** — join a JSON tun/tap server and every emulator connected to it is on one network, RISC OS Pyromaniac included. The server can run on another computer, so a Windows or macOS machine can join a network hosted on Linux without a TAP of its own. See [docs/pyromaniac-networking.md](docs/pyromaniac-networking.md).
- **HostCmd — drive the RISC OS command line from the host** — run guest commands from the host over a local socket and stream their output back, with the return code. Edit on the host (via HostFS), compile on the guest (`rpcemu-run -- cc -c hello`), or open an interactive RISC OS shell (`rpcemu-shell`). Ideal for IDE/LLM-driven development. See [docs/hostcmd.md](docs/hostcmd.md).
- **DebugCmd — debug the emulated ARM from the host** — the processor-level counterpart of HostCmd, over its own socket. Conditional breakpoints (`bp add main if r0 == 0`) rather than only "stop here", step over a call or out of a function, walk the call stack, and load a symbol file so addresses come back as names instead of numbers. The disassembler covers the FPA10, so floating-point code reads as `ADFD F0, F1, F2` rather than as a generic coprocessor operation. Driven from `rpcemu-debug`, an IDE, a script, or the MCP server. See [docs/debugcmd.md](docs/debugcmd.md).
- **MCP server — drive RISC OS from Claude / an agent** — a [Model Context Protocol](https://modelcontextprotocol.io) server exposing tools to run guest commands, read/write/list files (via HostFS), capture and click the screen, and inspect/control the emulated ARM CPU (registers, memory, disassembly, conditional breakpoints, watchpoints, stepping over and out, backtraces, symbols). Point Claude Code / Desktop at it for agent-driven RISC OS development. Setup and tool reference in [tools/mcp/README.md](tools/mcp/README.md).
- **Parallel port** — log raw output to a file, a virtual printer that captures jobs to `.prn` files with optional in-process PDF conversion via Ghostscript, or print on a real printer the host already has.
- **Serial port** — log to file, a TCP "modem" that dials real telnet BBSes (`ATDT host:port`) with a telnet client layer and 8-bit-clean X/Y/ZMODEM transfers, or a real serial port on the host (USB adapter, built-in port, or a pseudo terminal), with the speed and framing following whatever the guest programs. See [docs/peripherals.md](docs/peripherals.md).
- **Machine Inspector** — live CPU, disassembly, memory, peripheral, and debugger views with auto-refresh.
- **Integrated debugger** — pause/resume, single-step, breakpoints, and watchpoints; dynarec-aware via shared hooks.
- **Toolbar and status bar** — quick access to common actions; activity indicators for floppy, IDE, HostFS, and network.
- **Recent disc images** — quick access to recently used floppy and CD-ROM images.

---

## Architecture

The codebase splits into two layers:

| Layer    | Path       | Language | Role                                                             |
| -------- | ---------- | -------- | ---------------------------------------------------------------- |
| **Core** | `src/`     | C11      | Guest ARM CPU (interpreter or dynarec), devices, SLiRP, debugger |
| **GUI**  | `src/gui/` | C++17    | wxWidgets front-end, threading bridge, dialogs, VNC server       |

The GUI runs emulation on a **worker thread** (`EmulatorHost`). UI events are posted
as commands; video updates and debugger notifications come back through a `GuiBridge`
interface. Inspector snapshots are marshalled off the emulator thread as plain
`MachineSnapshot` structs.

Build with **CMake** — see [COMPILE.md](COMPILE.md) for full details.

---

## Project layout

| Path                             | Purpose                                                                                                                                                                                                                                                                                                                                                         |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/`                           | Emulator core (CPU, VIDC, IOMD, IDE, FDC, FPA, HostFS, SLiRP, …)                                                                                                                                                                                                                                                                                                |
| `src/gui/`                       | wxWidgets front-end, machine inspector, configuration dialogs                                                                                                                                                                                                                                                                                                   |
| `configs/`                       | Machine configuration files (`.cfg`, INI format)                                                                                                                                                                                                                                                                                                                |
| `machines/<name>/`               | Per-machine runtime data: `cmos.ram`, `hostfs/`, `hd4.hdf`, `hd5.hdf`                                                                                                                                                                                                                                                                                           |
| `shared/`                        | Common folder exposed as `HostFS::Shared.$` (created at startup if missing)                                                                                                                                                                                                                                                                                     |
| `roms/`                          | RISC OS ROM images — see [the project repository](https://github.com/andrewtimmins/rpcemu-extended)                                                                                                                                                                                                                                                             |
| `resources/`                     | Blank floppy/disc templates for *Disc → Floppy → Create Blank*                                                                                                                                                                                                                                                                                                  |
| `poduleroms/`                    | Compiled extension ROM images (HostFS, ScrollWheel — the built-in Support podule)                                                                                                                                                                                                                                                                               |
| `podules/`                       | Expansion-card (podule) ROMs — shipped system components, selectable per machine                                                                                                                                                                                                                                                                                |
| `gfxroms/`                       | The graphics card's display driver, carried in that card's own ROM                                                                                                                                                                                                                                                                                              |
| `netroms/`                       | The network card's DCI4 driver (EtherRPCEm), carried in that card's own ROM                                                                                                                                                                                                                                                                                     |
| `usbroms/`                       | RISC OS Open's USB stack (USBDriver, OHCIDriver) and the modules a USB drive needs (RTSupport, SCSISwitch, SCSISoftUSB, SCSIFS), carried in the USB card's own ROM — not ours and not GPL, see `usbroms/LICENCES.txt`                                                                                                                                           |
| `riscos-progs/`                  | RISC OS module source (HostFS, HostFSFiler, ScrollWheel, EtherRPCEm, RPCEmuSupport, RPCEmuGfx, SyncClock, RPCEmuUSBSupport, RPCEmuPCIEmulator)                                                                                                                                                                                                                  |
| `riscos-patches/`                | Our changes to RISC OS components that are not ours, as patches against a named upstream revision (currently OHCIDriver)                                                                                                                                                                                                                                        |
| `packaging/`                     | Desktop entry and other packaging files                                                                                                                                                                                                                                                                                                                         |
| `tests/`                         | Unit tests, the boot and command-line smoke tests, and the scripts CI and the pre-push hook both call                                                                                                                                                                                                                                                           |
| `.githooks/`                     | The pre-push hook that builds and tests before anything leaves your machine                                                                                                                                                                                                                                                                                     |
| `docs/release-notes/`            | One file per release, published as that release's notes                                                                                                                                                                                                                                                                                                         |
| `build.sh`                       | Unified build and release script                                                                                                                                                                                                                                                                                                                                |
| [`MANUAL.md`](MANUAL.md)         | The manual: what everything does, written for people new to RISC OS as well as to this emulator                                                                                                                                                                                                                                                                 |
| [`QUICKSTART.md`](QUICKSTART.md) | RISC OS running in about five minutes, with nothing to find first                                                                                                                                                                                                                                                                                               |
| `docs/dynarec.md`                | ARM dynamic recompiler (build, behaviour, limitations)                                                                                                                                                                                                                                                                                                          |
| `docs/arm64-dynarec.md`          | AArch64 (arm64) dynarec backend                                                                                                                                                                                                                                                                                                                                 |
| `docs/peripherals.md`            | Serial and parallel ports: file logging, TCP modem, a real host serial port, the virtual printer, and printing on a host printer                                                                                                                                                                                                                                |
| `docs/packages.md`               | Package manager: installing RISC OS software, where it comes from, and the per-machine database                                                                                                                                                                                                                                                                 |
| `docs/podules.md`                | Expansion cards (podules): bundled devices, configuration, plugin ABI                                                                                                                                                                                                                                                                                           |
| `docs/gfxcard.md`                | Graphics card: display modes beyond what VRAM allows, and its GraphicsV driver                                                                                                                                                                                                                                                                                  |
| `docs/kinetic.md`                | Kinetic StrongARM: how the card is detected, its 512MB memory map, and the three paths a new memory region needs                                                                                                                                                                                                                                                |
| `docs/usb.md`                    | USB: the emulated OHCI host controller, passing real devices through to the guest, streaming from a camera, USB drives, and why it is OHCI                                                                                                                                                                                                                      |
| `docs/mmu-permissions.md`        | MMU access permissions: the defect that made ADFFS unusable, how it was fixed, and the regression test that holds it                                                                                                                                                                                                                                            |
| `docs/openbus.md`                | OPEN Bus: the Risc PC second processor interface, what a card must bring itself, and the stub card used to test it                                                                                                                                                                                                                                              |
| `docs/hostfs.md`                 | HostFS: where a machine's drive lives, pointing several machines at one folder, and the sharing hazard                                                                                                                                                                                                                                                          |
| `docs/paths.md`                  | Where RPCEmu keeps machines, ROMs and settings: the first-run question, the precedence rules, and who is never asked                                                                                                                                                                                                                                            |
| `docs/keyboard.md`               | Keyboard: how a host key reaches RISC OS, why it is mapped by physical position rather than by character, non-UK layouts, and diagnosing it with `RPCEMU_KEYBOARD_DEBUG`                                                                                                                                                                                        |
| `docs/clipboard.md`              | Shared clipboard: copying text and images between the host and RISC OS                                                                                                                                                                                                                                                                                          |
| `docs/vnc.md`                    | VNC: using a machine remotely, choosing one over VNC in headless mode, and the control menu for a running machine                                                                                                                                                                                                                                               |
| `docs/multi-machine.md`          | Running several machines at once: how it works today, the virtual switch planned, and why the core is one machine per process                                                                                                                                                                                                                                   |
| `docs/pyromaniac-networking.md`  | Sharing a virtual network with RISC OS Pyromaniac and other emulators, through a JSON tun/tap server                                                                                                                                                                                                                                                            |
| `docs/hostcmd.md`                | HostCmd: drive the RISC OS command line from the host (`rpcemu-run`/`rpcemu-shell`)                                                                                                                                                                                                                                                                             |
| `docs/prminxml/`                 | **The host interfaces manual**, in PRM-in-XML: HostFS, HostCmd, the clipboard and the network SWI — every operation, its registers, and how each reports failure. Written for someone implementing the host half rather than for someone using the emulator. Rendered to HTML by CI and published with each release; `make` in that directory builds it locally |
| `tools/mcp/README.md`            | MCP server: drive a RISC OS machine from Claude / an agent (commands, files, screen, debugger). Setup + tool reference.                                                                                                                                                                                                                                         |
| `docs/debugcmd.md`               | DebugCmd: control the emulated CPU over a socket (`rpcemu-debug`) — registers, memory, disassembly, conditional breakpoints, stepping over and out, backtraces, symbols                                                                                                                                                                                         |
| `docs/disassembly.md`            | The ARM disassembler: what it covers, the FPA and RISC OS SWI names, and why it is not a library                                                                                                                                                                                                                                                                |
| `docs/debugger-tracing.md`       | Debugger: exception trapping, SWI tracing, logging watchpoints                                                                                                                                                                                                                                                                                                  |
| `docs/testing.md`                | Testing: running the suite, the pre-push hook, writing a test, the sanitiser build, and the known gaps                                                                                                                                                                                                                                                          |
| `docs/windows-build.md`          | Building for Windows (MinGW-w64)                                                                                                                                                                                                                                                                                                                                |
| `docs/macos-build.md`            | Building for macOS (universal binary)                                                                                                                                                                                                                                                                                                                           |
| `setup-build-env.sh`             | Install build dependencies (Debian/Ubuntu)                                                                                                                                                                                                                                                                                                                      |

### Where your data lives

When **installed** (e.g. from the `.deb`), the binary and read-only support files
(ROMs, podule ROMs, templates) live under `/usr/share/rpcemu`, while your own
machines, configs, ROMs, HostFS and logs are kept in a visible **`~/RPCEmu/`** folder,
seeded from the shared templates on first run. An existing `~/.local/share/rpcemu` from
an earlier version is migrated automatically. The **portable** `.tar.gz` instead keeps
everything self-contained in its own folder.

**On first run RPCEmu asks where that folder should be**, with the location above
already filled in, so pressing Return puts it exactly where earlier versions did.
Change it later from the machine selector's *Options ▾ → Data Folder…*, which points RPCEmu at a different
folder and moves nothing, or per-run with `--datadir`.

You are only asked when there is genuinely nothing to go on. An existing
installation, a portable or in-tree layout, `RPCEMU_DATADIR`, `--datadir` and every
run without a GUI all carry on silently as before. The full precedence, and which
situations do and do not ask, is in [docs/paths.md](docs/paths.md).

These environment variables override the defaults, and both outrank the folder
chosen on first run so that scripts and CI stay predictable:

| Variable                 | Meaning                                                                                                                                                                                                          |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `RPCEMU_DATADIR`         | Writable data directory (machines, configs, logs). Otherwise the folder chosen on first run, then the executable's directory or the current directory if it contains `configs/`, else the install prefix.        |
| `RPCEMU_RESOURCE_DIR`    | Read-only support files (ROM/config/podule templates).                                                                                                                                                           |
| `RPCEMU_NO_GUI_MESSAGES` | **Windows only.** Set to `1` to send `--help`, `--list-machines` and startup errors to stdout/stderr instead of a message box. See [Windows: messages appear in a dialog](#windows-messages-appear-in-a-dialog). |

---

## Getting started

### Supported systems

Each GitHub release ships prebuilt packages for four targets:

| Package                                      | Platform                        | CPU core                                                                                           |
| -------------------------------------------- | ------------------------------- | -------------------------------------------------------------------------------------------------- |
| `rpcemu_*_amd64.deb` / `_linux_amd64.tar.gz` | Linux x86-64                    | Recompiler (full speed)                                                                            |
| `rpcemu_*_arm64.deb` / `_linux_arm64.tar.gz` | Linux arm64 (e.g. Raspberry Pi) | Interpreter (native arm64 recompiler implemented, not yet enabled in releases)                     |
| `rpcemu_*_windows_amd64.zip`                 | Windows x64 (10/11)             | Recompiler (full speed)                                                                            |
| *(CI artifact, not a release asset yet)*     | Windows on ARM (ARM64)          | Interpreter — native build; the amd64 release also runs there under emulation, with the recompiler |
| `rpcemu_*_macos_universal.dmg`               | macOS (Intel + Apple Silicon)   | Universal app bundle — recompiler on Intel, interpreter on Apple Silicon                           |

**Linux** packages are built on **Ubuntu 24.04 LTS**; being dynamically linked, they run
on distributions whose system libraries are that version or newer:

| Distribution                                                                       | Runs the prebuilt release?                             |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------ |
| Ubuntu 24.04 LTS (Noble) and newer (24.10, 25.04, …)                               | ✅ Yes — primary target                                 |
| Linux Mint 22 / 22.x, Pop!_OS 24.04, Zorin 18, elementary 8, KDE neon (24.04 base) | ✅ Yes                                                  |
| Debian 13 (Trixie) and newer                                                       | ✅ Yes                                                  |
| arm64 / Raspberry Pi (Ubuntu 24.04+ base)                                          | ✅ Yes — `arm64` package (interpreter; slower than x86) |
| Ubuntu 22.04 LTS, Debian 12 (Bookworm) and older                                   | ❌ No — system libraries too old                        |

Linux minimum requirements: **glibc ≥ 2.34**, **libstdc++ from GCC 13.2+**
(`GLIBCXX_3.4.32`), and **wxWidgets 3.2** — standard on Ubuntu 24.04-era distributions.
On an older/different distribution (or for arm64), **build from source** instead:
`./setup-build-env.sh` then `./build.sh`. See [COMPILE.md](COMPILE.md).

**Building on Debian 12 (Bookworm), including Raspberry Pi OS:** its
`libwxgtk3.2-dev` is built without `wxWebRequest`, which RPCEmu uses for the
package manager and for downloading RISC OS. The build succeeds and the emulator
is unaffected, but **those two features are absent** and you supply a ROM image
yourself. Debian 13 (Trixie) and Ubuntu 24.04 are unaffected. See
[wxWidgets and wxWebRequest](COMPILE.md#wxwidgets-and-wxwebrequest) for what is
missing and how to get it back.

**Windows**: extract the `windows_amd64.zip` anywhere and run `rpcemu-recompiler.exe`.
The MinGW/SDL2/libvncserver runtime DLLs are bundled in the zip, so there is nothing
else to install. Windows 10/11 (x64). Built with MinGW-w64 via MSYS2 — see
[Build for Windows](#build-for-windows) to build it yourself.

**macOS**: open the `macos_universal.dmg` and drag **RPCEmu** into **Applications**. It is
a universal app (Intel and Apple Silicon); on Apple Silicon the Intel slice runs at full
speed under Rosetta 2.

Nothing needs to be installed alongside it. As on Linux, machines, configs, ROMs, HostFS
and logs are written to a visible **`~/RPCEmu/`** folder, never inside the app bundle, so
the app stays read-only in Applications.

#### First launch is blocked — how to open it

RPCEmu is not notarised by Apple, so the first launch is refused with *"RPCEmu cannot be
opened because the developer cannot be verified"* or *"Apple could not verify RPCEmu is
free of malware"*. This only has to be dealt with once — afterwards it opens by
double-clicking like anything else.

**macOS 15 Sequoia and macOS 26 Tahoe.** Control-clicking no longer offers a way past
this. Try to open the app and dismiss the message, then go to **System Settings >
Privacy & Security**, scroll to **Security**, and click **Open Anyway** beside the note
about RPCEmu. Confirm with **Open**, authenticating if asked. The button only appears for
about an hour after the blocked launch, so if it is not there, try opening the app again
first.

**macOS 14 Sonoma and earlier.** **Control-click (or right-click) the app, choose Open,
then Open again** in the dialog. If Open is not offered, use **System Settings** (or
**System Preferences**) **> Privacy & Security**, or **Security & Privacy > General** on
older releases, and click **Open Anyway**.

**If neither works**, macOS has quarantined the download more firmly than the dialogs can
clear. Remove the flag from Terminal, which works on every version:

```bash
xattr -d com.apple.quarantine /Applications/RPCEmu.app
```

Then open the app normally. There is no need to change the "Allow applications from"
setting to do any of this.

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
  `mingw-w64-x86_64-` toolchain, cmake, wxwidgets3.2-msw, SDL2, libvncserver, libusb),
  just run `./build-windows.sh --zip`.
- **Cross-compile, from Linux** — run `./setup-cross-build-env.sh` once (builds
  wxWidgets/SDL2/libvncserver/libusb for the mingw target into the sysroot), then
  `./build-windows.sh --zip`.

It defaults to the recompiler (`rpcemu-recompiler.exe`); pass `--interpreter` for the
interpreter build. Runtime DLLs are bundled into the staged folder automatically. This
is exactly what the `windows-amd64` CI job runs.

libusb is required, so that USB passthrough is not silently dropped from a release; see
[Building with USB support](#building-with-usb-support).

### Build for macOS

`build-macos.sh` produces a **universal** `RPCEmu.app` — the Intel (x86-64) slice includes
the dynamic recompiler, the Apple Silicon (arm64) slice is the interpreter — fused with
`lipo`, then ad-hoc signed and wrapped in a drag-to-Applications `.dmg`. Dependencies come
from Homebrew (`cmake ninja pkg-config wxwidgets sdl2 libvncserver libusb`), and both
slices must have the same versions of them or `lipo` will not fuse the result. (A native arm64 recompiler now exists, including the `MAP_JIT` support the
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
without an Apple Developer ID it is not notarised, so the first launch has to be allowed
past Gatekeeper (see [First launch is blocked](#first-launch-is-blocked-how-to-open-it)).
On a single machine each slice is built for its own architecture (the other builds
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

The disc HardDisc4 ships set up for an AKF60 monitor at 800 x 600 in 256 colours,
which is not what a machine on a modern display wants. So when the disc is unpacked
onto a machine, RPCEmu also sets its desktop screen mode: the largest standard mode
that fits inside the host's display and the machine's display memory, in 16 million
colours. Change it afterwards as you would on real hardware, in *Configure → Screen*.
Nothing is changed on a disc that is already installed, and a headless
`--fetch-riscos` leaves the setting alone, having no display to size it against.

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

| Option                           | Effect                                                                                                                                                                                                                                                                                                                                                              |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--machine <name>`               | Run this machine, skipping the selector. Also accepts `--machine=<name>`.                                                                                                                                                                                                                                                                                           |
| `--resume`                       | Resume the machine's own snapshot, consuming it to `.bak`. Requires `--machine`.                                                                                                                                                                                                                                                                                    |
| `--state <file>`                 | Load an explicit state file, leaving it in place. Requires `--machine`.                                                                                                                                                                                                                                                                                             |
| `--vnc-port <n>`                 | VNC port for this instance, overriding the settings file. Needed when running more than one emulator on a host, since the setting is per installation.                                                                                                                                                                                                              |
| `--no-vnc`                       | Do not start the VNC server, whatever the settings say. Ignored with `--headless`, which has no other way in.                                                                                                                                                                                                                                                       |
| `--hostcmd-socket <spec>`        | HostCmd socket for this instance: a path, or a bare port for TCP on localhost.                                                                                                                                                                                                                                                                                      |
| `--debug-socket <spec>`          | DebugCmd socket for this instance, same forms.                                                                                                                                                                                                                                                                                                                      |
| `--pyromaniac-net <host[:port]>` | Join a JSON tun/tap server, so this machine shares a virtual network with the other emulators on it, RISC OS Pyromaniac included. The port defaults to 33445. `off` leaves a machine whose settings have it on out of that network for this run. Applies to `--headless` as well as the window. See [docs/pyromaniac-networking.md](docs/pyromaniac-networking.md). |
| `--no-relay`                     | Do not relay Access broadcasts. Only one emulator per host can, and the others decline automatically; this says so deliberately.                                                                                                                                                                                                                                    |
| `--headless`                     | Run with no GUI window, over VNC. Without `--machine`, the machine list is offered over VNC and you choose one from a client. VNC is started for the run whether or not the settings enable it, since it is the only way in; the setting itself is left alone.                                                                                                      |
| `--list-machines`                | List the available machine configs and exit.                                                                                                                                                                                                                                                                                                                        |
| `--openbus-stub`                 | Fit the OPEN Bus test card to the second processor slot, so the plumbing a real card needs can be exercised on a running machine. A development aid rather than a model of real hardware, and nothing is fitted without it. See [docs/openbus.md](docs/openbus.md).                                                                                                 |
| `--datadir <dir>`                | Where machines, ROMs and settings live, for this run only. Also accepts `--datadir=<dir>`. Outranks `RPCEMU_DATADIR` and the folder chosen on first run, and is deliberately not remembered, so a scripted run cannot become the default for later interactive ones.                                                                                                |
| `--fetch-riscos[=which]`         | Download RISC OS from RISC OS Open, unpack it, create a machine and exit. `which` is `stable` (default) or `nightly`.                                                                                                                                                                                                                                               |
| `--no-disc`                      | With `--fetch-riscos`, fetch the ROM only.                                                                                                                                                                                                                                                                                                                          |
| `--accept-licence`               | Required by `--fetch-riscos`: agrees to the licensing terms of what is downloaded, which are printed first.                                                                                                                                                                                                                                                         |
| `--pkg-sources`                  | List the package repositories and the file they are configured in, and exit. Touches no network.                                                                                                                                                                                                                                                                    |
| `--pkg-list[=text]`              | List the available RISC OS packages, optionally only those matching `text`, and exit.                                                                                                                                                                                                                                                                               |
| `--pkg-info=<name>`              | Show everything the catalogue holds about one package, and exit.                                                                                                                                                                                                                                                                                                    |
| `--pkg-install=<name>`           | Install a package. Needs `--pkg-machine`.                                                                                                                                                                                                                                                                                                                           |
| `--pkg-remove=<name>`            | Remove a package. Needs `--pkg-machine`.                                                                                                                                                                                                                                                                                                                            |
| `--pkg-machine=<name>`           | Which machine's disc `--pkg-install` and `--pkg-remove` act on.                                                                                                                                                                                                                                                                                                     |
| `-h`, `--help`                   | Show usage and exit.                                                                                                                                                                                                                                                                                                                                                |

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
  with or without the `.cfg` suffix). On its own, without `--headless`, it starts the
  GUI on that machine, skipping the selector (see above).
- **Without `--machine`, the machine list is offered over VNC.** Connect a client to
  the VNC port and choose with the arrow keys, or type the number beside a machine,
  then press Enter. Escape gives up and exits. The port and password come from the
  emulator's own settings (`rpcemu.cfg` in the data directory), since no machine has
  been chosen yet to ask. The machine you pick then applies its own, so a machine
  with a port of its own is served on it.
- `--resume` and `--state <file>` work here too, so a headless machine can be brought
  back up from a snapshot — useful when a service manager restarts it.
- `--list-machines` prints the available machine names and exits.
- `--help` (or `-h`) prints usage and exits. All three of these run without a display.
- VNC is the only way into a headless machine, so `--headless` **implies it**: the
  server is started for the session even if `vnc_enabled=0`. The setting itself is
  your choice and is left alone, so running headless once does not enable VNC for
  the GUI afterwards. The port and password come from the machine's own
  configuration, falling back to `rpcemu.cfg` in the data directory for anything it
  does not mention, with the port defaulting to 5900. There is no password unless you set one, so do set one if the
  port is reachable from anywhere untrusted; headless says so at startup.
- **A VNC client can control the machine, not just type at it.** `Ctrl`+`Alt`+`Shift`+`M`
  brings up a menu over the running machine to reset it or shut the emulator down,
  which matters most here, where there is no window and no menu bar. See
  [docs/vnc.md](docs/vnc.md).
- **Running more than one emulator at once needs a different `vnc_port` for each.**
  Ports are not allocated automatically and the default is 5900, so a second
  instance left at the default cannot bind and exits with an error rather than
  starting unreachable. Since the port is now per-installation rather than per
  machine, two instances sharing a data directory need one of them overridden. The
  port and password can also be changed while a machine is running from
  **Settings > VNC Server** in the GUI.
- Press **Ctrl-C** (or send `SIGTERM`) to shut down cleanly — CMOS, disc images, and
  configuration are saved on exit, just as when closing the GUI window.
- Send `SIGUSR1` to reset the machine without stopping it (see
  [Resetting from outside](#resetting-from-outside-linux-and-macos)).

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

| Setting          | Options                                                                                                                                                   |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Model**        | RiscPC ARM610/710/810/StrongARM, Kinetic StrongARM (512MB), A7000, A7000+ (experimental), Phoebe (experimental)                                           |
| **RAM**          | 4, 8, 16, 32, 64, 128, 256 MB, or 512 MB (Kinetic)                                                                                                        |
| **VRAM**         | None or 2 MB                                                                                                                                              |
| **ROM**          | Subdirectory under `roms/` containing ROM components                                                                                                      |
| **Refresh rate** | 20–100 Hz                                                                                                                                                 |
| **Network**      | Off, NAT, Ethernet Bridging, IP Tunnelling                                                                                                                |
| **Hard discs**   | HardDisc 4 and 5 — create 256 MB, 512 MB, 1 GB, or 2 GB images                                                                                            |
| **VNC server**   | On/off, port (default 5900) and password — see [Settings > VNC Server](#headless-mode). Give each machine its own port if you run more than one at a time |

Configuration keys are stored under a `[General]` group (wxFileConfig INI format).
NAT port-forward rules are stored in a separate `[nat_port_forward_rules]` group.

---

## HostFS and Shared drives

Two filing system icons appear on the RISC OS icon bar:

| Icon       | RISC OS path       | Host path                                          | Scope                                                      |
| ---------- | ------------------ | -------------------------------------------------- | ---------------------------------------------------------- |
| **HostFS** | `HostFS::HostFS.$` | `machines/<name>/hostfs/` by default, configurable | Per-machine, or shared if you point machines at one folder |
| **Shared** | `HostFS::Shared.$` | `shared/`                                          | All machines, always                                       |

Use HostFS for machine-specific files and Shared for utilities or files you want
available across configurations.

### Pointing HostFS somewhere else

*Settings → Machine → System → **HostFS folder***, or the `hostfs_path` key in the
machine's `.cfg`. Three forms, the same convention `hd4_path` and `rom_dir` use:

| Setting              | Means                                                                                                                                 |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| *empty*              | `machines/<name>/hostfs`, as it has always been. **Nothing is stored**, so there is nothing to go stale if you move your data folder. |
| `discs/work`         | Relative, under the machine's own folder, so it moves with the machine.                                                               |
| `/srv/riscos/shared` | Absolute, used as given. This is how several machines share one folder.                                                               |

Sharing one folder between machines is supported and is what the setting is for,
but **do not run two machines on one folder at the same time**: HostFS holds open
file handles and RISC OS caches directory contents, so two guests writing the same
tree together can lose work. The machine editor tells you which other machine
already uses a folder. See [docs/hostfs.md](docs/hostfs.md).

---

## Machine Inspector and debugger

Open **Debug → Machine Inspector…** (or use the toolbar button).

| Tab             | Contents                                                                                                         |
| --------------- | ---------------------------------------------------------------------------------------------------------------- |
| **CPU**         | Registers R0–R15, CPSR, mode, MMU state, dynarec/interpreter, performance                                        |
| **Disassembly** | ARM disassembly at a chosen address, optional follow-PC                                                          |
| **Memory**      | Hex dump of emulated memory at a chosen address                                                                  |
| **Debugger**    | Run/Pause/Step, breakpoint and watchpoint lists, last halt reason                                                |
| **Trace**       | Exception traps, SWI tracing, and logging watchpoints — see [docs/debugger-tracing.md](docs/debugger-tracing.md) |
| **Peripherals** | VIDC, IOMD IRQ/timers, floppy, IDE, podule slot summary                                                          |

Auto-refresh runs every 500 ms by default. Breakpoints and watchpoints work while
the dynarec is active — `arm_dynarec.c` checks `debugger_requires_instruction_hook()`
before executing translated blocks.

---

## Serial and parallel ports

Configure via **Settings → Serial…** and **Settings → Parallel…**. The Risc PC has a
single hardware serial port (the 16550 UART at `0x3F8`), so only one **Serial** port
is exposed.

| Port               | Modes                                                              |
| ------------------ | ------------------------------------------------------------------ |
| **Serial (0x3F8)** | Disabled, log to file, TCP modem (telnet), a real port on the host |
| **Parallel (LPT)** | Disabled, log to file, virtual printer, print on this computer     |

- **Log to file** captures the raw byte stream the guest sends — handy for debugging
  or capturing print/serial output.

- **TCP modem** answers the Hayes AT command set and `ATDT host:port` opens a real TCP
  connection. It speaks telnet and negotiates binary mode, so telnet BBSes work and
  X/Y/ZMODEM transfers stay 8-bit clean. `+++` (guard-timed) returns to command mode;
  `ATH` hangs up.

- **Virtual printer** writes `.prn` files to a chosen folder
  (default: `machines/<name>/printjobs/`); with Ghostscript support, enable **Also
  create PDF files** for automatic conversion.

- **A real serial port on the host** hands the guest an actual port: a USB adapter, a
  built-in port, or a pseudo terminal. The dialogue lists the ports the machine has
  rather than names that might not exist, and the field is editable for anything
  unusual. The speed and framing follow whatever the guest programs, so
  `*Configure Baud` and friends behave as they would on real hardware, and DTR and RTS
  are mirrored onto the port. What is *not* attempted is bit-level timing: the host's
  own UART does the signalling. A device that cannot report its modem lines, which
  includes every pseudo terminal, is treated as asserting CTS, DSR and DCD, because
  RISC OS waits for CTS before transmitting and would otherwise hang.

- **Print on this computer** sends a finished job to a printer the host already knows
  about. Give it the name of a print queue and the job is spooled as raw data through
  the host print system, or give it a device path such as `/dev/usb/lp0` and it is
  written there directly. The bytes are passed through untouched, since what the guest
  produces is whatever its RISC OS printer driver emits.

**On the limits of the parallel port:** this carries the print stream, not the pins.
Devices that need real bidirectional IEEE-1284 signalling, such as dongles, Zip drives
and scanners, are not supported and are not planned: raw pin access needs hardware
almost no modern machine has, there is no portable way to reach it, and the handshake
turnarounds are shorter than an emulator that is not locked to the wall clock can meet.

Full details, including AT commands and how RISC OS drives each port, are in
[docs/peripherals.md](docs/peripherals.md).

---

## USB devices

The machine has a USB expansion card with an emulated **OHCI** host controller and four
ports, and RISC OS Open's own USB stack rides in that card's ROM. Nothing needs
installing in the guest and there is no switch to throw: the card is always fitted and
the modules start at boot.

A device on the host's own USB bus can be handed to the guest through
[libusb](https://libusb.info/). RISC OS enumerates it and names it as the real thing:

```
*USBDevices
No. Bus Dev Class Description
  1   1   1  9/ 0 Built-in OHCI root hub
  2   1   2 EF/ 2 Azurewave USB2.0 HD IR UVC WebCam
```

Pick a device per port in **Settings → USB…**. The choice is per machine and is
remembered as the device's identifiers rather than its position, so moving it to a
different socket does not lose it. Handing a device over is not a neutral act: the
host's own driver is detached from it for as long as the guest has it, and a device the
host is currently using takes a confirmation first.

### What to expect

Devices enumerate, and their descriptors, manufacturer and product strings and serial
numbers all come from the real hardware. Keyboards and mice work without anything being
installed, since HID is compiled into USBDriver.

**Streaming devices work too.** Isochronous transfers are implemented, so a webcam or an
audio device does more than describe itself: its packets reach the guest a frame at a
time, and a few lines of BASIC reading the endpoint through DeviceFS get the real data.
Nothing in RISC OS will display a webcam for you, so this is a foundation rather than a
feature, and [docs/usb.md](docs/usb.md) shows how to read one.

**USB drives work as well, and mount.** The card's ROM carries RISC OS Open's SCSI
modules, so a drive appears in `*SCSIDevices` with its real capacity and its sectors
read and write correctly. RISC OS itself will only mount a FileCore disc, and almost
every stick in the world is FAT or exFAT, so the card's ROM also carries **MultiFS** -
ours - to read them, and **MultiFSFiler** to put the disc on the icon bar under its volume
name. Plug a stick in, click the icon, and a Filer window opens on it. Files can be
read off a stick and saved, renamed and deleted on one, and directories made and
removed, with long file names in both directions, so a file keeps the name the host
gave it and a file written in RISC OS keeps the name it was given when the stick goes
back into a PC. That covers **FAT12/16/32 and exFAT**. **NTFS is read only**: its
files and directories are listed and read, and every attempt to change one is refused.
RISC OS file types are kept on FAT, following the convention recorded in the credits
below so that two machines agree about a stick; [docs/usb.md](docs/usb.md) says exactly where the edges are.

Hubs still cannot be passed through.

This is **verified on Linux**. Windows and macOS builds only gained libusb in this
release, so passthrough there is expected to work but has not yet been confirmed on
real hardware. Reports welcome.

### Letting RPCEmu reach the device

The emulated card needs nothing. Reaching a *real* device means getting past the host's
own claim on it, and each platform does that differently.

**Linux.** The device nodes under `/dev/bus/usb` belong to root, so add a udev rule
naming the device you want:

```
# /etc/udev/rules.d/70-rpcemu-usb.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="046d", ATTRS{idProduct}=="c077", TAG+="uaccess"
```

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`, and unplug and
replug the device. `TAG+="uaccess"` grants access to whoever is logged in at the
machine rather than to every account on it. Without the rule the dialogue still lists
the device, marked "no permission", rather than failing when you try to use it.

**Windows.** Windows binds its own class driver to a device (`usbstor`, HID, `usbccgp`
for composite devices) and libusb cannot open it through that. The device needs the
**WinUSB** driver bound instead, which is what [Zadig](https://zadig.akeo.ie/) is for:
run it, *Options → List All Devices*, pick the device, choose **WinUSB**, and replace
the driver. Two things worth knowing before you do: Windows itself stops using the
device while WinUSB is bound, and reverting means *Device Manager → the device →
Uninstall device*, ticking "delete the driver software", then replugging. Keyboards,
mice and hubs are held by Windows and are not candidates.

**macOS.** `brew install libusb` covers it, and most devices need no driver work
because libusb reaches them through IOKit directly. A device already claimed by one of
Apple's own class drivers, which includes HID, mass storage and audio, may refuse the
interface claim; there is no supported way to detach an Apple driver, so those are not
available.

### Building with USB support

The emulated controller and the card are always built. Passthrough needs **libusb-1.0**
at build time, and a release build now **fails** rather than quietly producing a binary
that cannot reach a device.

| Platform                  | How                                                                                |
| ------------------------- | ---------------------------------------------------------------------------------- |
| Linux                     | `./setup-build-env.sh` (installs `libusb-1.0-0-dev`)                               |
| Windows, native MSYS2     | `pacman -S mingw-w64-x86_64-libusb` (or `mingw-w64-clang-aarch64-libusb` on ARM64) |
| Windows, cross from Linux | `./setup-cross-build-env.sh` (builds libusb for the MinGW target)                  |
| macOS                     | `brew install libusb`                                                              |

Set `RPCEMU_REQUIRE_LIBUSB=OFF` in the environment to build without it deliberately.
`BUILDINFO.txt` in a Linux release records whether the binary has it, and the USB
dialogue says so plainly if it does not.

Full details, including the descriptor cache, how transfers avoid blocking the emulator
thread, and why the controller is OHCI rather than the historically correct ISP1161, are
in [docs/usb.md](docs/usb.md).

---

## Keyboard and host controls

RPCEmu does **not** bind any host keyboard shortcuts, so every key — including the
function keys (**F12** for the RISC OS command line, etc.) and Ctrl combinations —
passes straight through to RISC OS. All emulator actions (screenshot, reset, floppy
load/eject, full-screen, mute, machine settings, and the debugger Run/Pause/Step
controls) are available from the menus and the toolbar instead.

| Key           | Action                                          |
| ------------- | ----------------------------------------------- |
| **Alt+Enter** | Release the captured mouse, or exit full-screen |

The toolbar provides one-click access to reset, screenshot, floppy load, CD-ROM
ISO load, mute, full-screen, machine settings, and debugger controls.

### Resetting from outside (Linux and macOS)

Sending **`SIGUSR1`** resets the running machine, exactly as **Reset** on the File
menu does, so a guest can be restarted from a script or another terminal without
touching the window:

```bash
kill -USR1 $(pgrep rpcemu-recompiler)
```

This works whether the machine is running in the GUI or headless. If no machine is
running — the machine selector is still open — the signal is noted in `rpclog.txt`
and otherwise ignored. Windows has no equivalent.

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
- Dual HostFS drives (per-machine + shared), with the per-machine folder configurable so several machines can share one (see [docs/hostfs.md](docs/hostfs.md))
- The data folder is asked for on first run rather than created silently in your home directory, and can be moved afterwards (see [docs/paths.md](docs/paths.md))
- Access/ShareFS broadcast relay for NAT networking
- Full FPA10 emulation with cycle timing
- MMU access permissions enforced on cached translations, which upstream does not do — the defect that made ADFFS and most of the games needing it unusable (see [docs/mmu-permissions.md](docs/mmu-permissions.md))
- Pixel Perfect integer scaling
- Built-in VNC server, with a control menu a VNC client can call up over the running machine (`Ctrl`+`Alt`+`Shift`+`M`) to reset it or shut the emulator down
- Headless mode for display-less servers (run a machine over VNC with no GUI), including choosing which machine to run from a list shown over VNC
- Pyromaniac Networking: share one virtual network with other emulators, including RISC OS Pyromaniac, through a JSON tun/tap server that can live on another computer
- HostCmd: drive the guest RISC OS command line from the host (`rpcemu-run`/`rpcemu-shell`) for edit-on-host/compile-on-guest workflows
- DebugCmd: debug the emulated ARM from the host (`rpcemu-debug`) — conditional breakpoints, step over/out, backtraces, symbols, FPA disassembly
- MCP server for agent-driven RISC OS development: run commands, edit/build, screenshot, and inspect/control the emulated CPU (see `tools/mcp/`)
- Virtual printer with optional Ghostscript PDF conversion
- Serial log-to-file and a real telnet TCP modem (dial BBSes, 8-bit-clean transfers)
- Machine Inspector with disassembly and memory browser
- Dynarec debugger hooks for consistent breakpoint/watchpoint behaviour
- Debugger exception trapping, SWI tracing, and logging watchpoints (see [docs/debugger-tracing.md](docs/debugger-tracing.md))
- Native arm64 (AArch64) recompiler backend, in addition to upstream's x86 dynarec — implemented and validated under emulation, not yet enabled in prebuilt releases (see [docs/arm64-dynarec.md](docs/arm64-dynarec.md))
- Robustness & memory-safety hardening: bounds-checked HFE/ADF disc-image and HostFS input handling, FPA faults raised as undefined instructions rather than aborting the emulator, a wild branch in the guest reported as a Prefetch Abort instead of killing the emulator, and a fixed use-after-free on GUI shutdown
- CMake build system, cross-platform: Linux (amd64 and arm64), Windows (amd64, MinGW-w64), and macOS (universal — Intel + Apple Silicon)

---

## Troubleshooting

| Symptom                                             | Remedy                                                                                                     |
| --------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `error while loading shared libraries: …` (tarball) | Run `./setup-runtime-env.sh` to install the runtime libraries (wxWidgets, SDL2, libvncserver, Ghostscript) |
| Window does not appear / configs not found          | Run from the project root or a staged release directory                                                    |
| No audio                                            | Ensure PulseAudio or PipeWire is running (SDL2)                                                            |
| No network                                          | Select NAT in machine settings; SLiRP/NAT is always compiled in (Linux and Windows)                        |
| ROM not found                                       | Place ROM files in `roms/<subdir>/` and select the folder in machine settings                              |
| Machine data not persisting                         | Check that `machines/<name>/` exists and is writable                                                       |
| VNC option missing                                  | Rebuild with `libvncserver-dev` installed                                                                  |
| PDF conversion unavailable                          | Install `libgs-dev` and rebuild; runtime needs Ghostscript resource files                                  |
| Diagnostic log                                      | See `rpclog.txt` in the data directory                                                                     |

---

## Contributing

Issues and pull requests are welcome, especially around debugger, inspector, and
networking features.

The build scripts run the unit tests, so an ordinary `./build.sh` is also a test
run. Before you push, turn on the hook that builds and tests first:

```bash
git config core.hooksPath .githooks
```

Anything new wants a test in `tests/`, and one that has been shown to fail when
the code it covers is broken — a test that has only ever passed has not been
tested itself. CI runs the same suite on Linux, Windows and macOS, plus a real
boot on each and a build under AddressSanitizer and UndefinedBehaviorSanitizer.
[docs/testing.md](docs/testing.md) covers all of it, including what is *not*
covered.

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
- The **toolbar icons** are **[Lucide](https://github.com/lucide-icons/lucide)**
  (Copyright Lucide Icons and Contributors, ISC), recoloured to the green of the
  Acorn logo. Lucide is a fork of **Feather** (Copyright Cole Bemis, MIT), which
  covers some of the set. The SVGs are in `src/gui/icons/` and are built into the
  binary rather than installed.
- **SyncClock** is **DEEJ Technology PLC's** module (Copyright 2002, GNU GPL v2),
  carried in the expansion ROM as `poduleroms/syncclock,ffa`. It re-reads the
  emulated real-time clock every ten seconds and sets RISC OS's soft copy from it,
  which is what puts the clock right after a machine has been suspended or a state
  is resumed later. Theirs is the design and the code: it is supplied here
  translated from their BBC BASIC assembler source into the GNU as syntax the other
  guest modules use, so it builds with them. Version 0.11 assembled byte-identical
  to the module built from their original; 0.12 is that with one change of ours, a
  time zone correction without which RISC OS 5 is left an hour slow whenever
  daylight saving is in force. See `riscos-progs/SyncClock/`.
- **USB** uses **RISC OS Open Limited's** own USB stack, not one of ours: the modules
  in `usbroms/` are their **USBDriver** and **OHCIDriver**, carried in the USB card's
  ROM and run by the emulated CPU. They are not GPL and `COPYING` does not cover
  them: they are a mixture of Apache 2.0 for the ROOL and Castle code and BSD for
  the NetBSD USB core it is built on, some of that the original four-clause BSD.
  Nothing is linked into RPCEmu, which reads them at run time as data in the same
  way it reads a ROM image. `usbroms/LICENCES.txt` carries the notices in full.
  **OHCIDriver is modified.** The stock driver finds controllers by asking the
  machine's HAL, and `HAL_IOMD` has no USB support at all, so ours also searches the
  expansion cards. That change is ours and not ROOL's, and it is kept as a patch
  against their OHCIDriver 0.56 in `riscos-patches/ohcidriver/` so it can be read
  and rebuilt rather than only taken on trust. The emulated controller, the card and
  the host passthrough are ours; the stack that drives them is theirs, and USB on
  this machine exists because they published it. See `docs/usb.md`.
- The **package manager** implements the **RISC OS Packaging Project's** package and
  database format, as defined in its policy manual: the format is **Graham Shaw's**
  design and the manual is maintained by **Alan Buckley**. **RISC OS Open Limited**,
  **riscoscommunity.org** and the **[Archimedes Software Preservation Project](https://www.jaspp.org.uk/)**
  host the indexes and the packages; JASPP's is **Jonathan Abbott's** and its
  contributors' work of preserving the commercial software of the period and packaging it
  to run on a machine like this one. None of their code is used here; this is an
  implementation of a published specification, and it keeps to that specification so a
  machine it installs onto stays usable by the project's own tools, PackMan and RiscPkg.
  See `docs/packages.md`.
- Extended Edition enhancements by Andy Timmins and contributors.
- Machine save/load state (suspend & resume) contributed by **Nick Brown**, whose
  outstanding item on that work, putting the clock right on resume, is why
  SyncClock is here.
- Command-line options (`--machine` for the GUI as well as headless, `--resume`
  and `--state`), with consistent messages and exit statuses across Linux,
  Windows and macOS; the macOS application bundle fixes that made the shipped
  `.app` runnable on a Mac other than the one that built it, along with the
  bundling and verification work behind them; and the cross-platform
  command-line and boot smoke tests (`tests/cli_smoke.sh`, `tests/boot_smoke.py`).
  Contributed by **David Ramsden**.
