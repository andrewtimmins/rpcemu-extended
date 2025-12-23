# RPCEmu – Spork Edition

## Overview
This repository hosts a feature-rich fork of **RPCEmu**, the open-source emulator for Acorn Risc PC and A7000 machines. Alongside the standard emulator core, this edition layers in a modern Qt 5 front-end, a live machine inspector, integrated debugger controls, and a multi-machine configuration system. The project remains distributed under the GNU GPL v2.

## Fork highlights
- **Multi-machine configuration** – Create, edit, clone and manage multiple machine configurations from a startup selector dialog. Each machine has isolated CMOS, HostFS and hard disc storage.
- **Quick machine switching** – Switch between machines on-the-fly via File → Recent Machines without restarting the emulator.
- **Shared HostFS drive** – A second "Shared" drive icon on the RISC OS icon bar provides access to a common `shared/` folder visible to all machine instances, enabling easy file sharing between configurations.
- **Access/ShareFS networking** – Full support for Acorn Access and ShareFS file sharing via NAT networking.
- **FPA emulation** – Full Floating Point Accelerator (FPA10) coprocessor emulation with all operations implemented and cycle-accurate timing. Enables floating-point intensive RISC OS applications to run correctly.
- **Pixel Perfect scaling** – Optional integer scaling mode for sharp, crisp pixels without bilinear blur. Toggle via Settings → Pixel Perfect.
- **Built-in VNC server** – Remote desktop access to RISC OS from any VNC client. Enable via Settings → VNC Server. Connect from tablets, phones, or other computers on your network.
- **Sound mute toggle** – Quickly mute/unmute audio via Settings → Mute Sound or press F10.
- **Machine Inspector window** – Inspect CPU registers, pipeline state, MMU flags, performance counters, and key peripheral snapshots (VIDC, SuperIO, IDE, podules) with optional auto-refresh.
- **Integrated debugger** – Pause/resume execution, single-step (×1/×5), and manage breakpoints and watchpoints directly in the GUI, with clear status readouts for the last halt reason.
- **Keyboard shortcuts** – Comprehensive keyboard shortcuts for common operations (see Keyboard Shortcuts section).
- **Recent disc images** – Quick access to recently used floppy and CD-ROM images via the Disc menu.
- **Dynarec-aware instrumentation** – The ARM dynamic recompiler honours debugger pause requests, breakpoints, and watchpoints via shared hooks (`debugger_requires_instruction_hook`) so that interpreter and dynarec stays in sync.
- **Snapshot plumbing** – Thread-safe `MachineSnapshot` and peripheral snapshot structs marshal detailed state off the emulator thread for UI consumption without stalls.

## Machine configuration system
The emulator now supports multiple isolated machine configurations:

| Directory | Purpose |
| --- | --- |
| `configs/` | Machine configuration files (`.cfg`). Each file defines model, RAM, VRAM, ROM, refresh rate, and networking settings. |
| `machines/<name>/` | Per-machine runtime data: `cmos.ram`, `hostfs/`, `hd4.hdf`, `hd5.hdf`. Fully isolated between configurations. |
| `shared/` | Common shared folder accessible from all machine instances via the Shared drive icon. |
| `roms/` | Shared ROM images. Select which ROM to use per-machine in the Edit dialog. |

### Configuration options
Each machine configuration supports:
- **Model** – RiscPC ARM610/710/810/StrongARM, A7000, A7000+, Phoebe
- **RAM** – 4 MB to 256 MB
- **VRAM** – None or 2 MB
- **ROM** – Select from available ROM files in the `roms/` directory
- **Refresh rate** – 20 Hz to 100 Hz (slider control)
- **Networking** – Off, NAT, Ethernet Bridging, IP Tunnelling (Linux)
- **Hard discs** – Create/manage HardDisc 4 and HardDisc 5 images (256 MB to 2 GB)


## Project layout
| Path | Purpose |
| --- | --- |
| `src/` | Core emulator engine (ARM interpreter, dynarec, hardware devices, debugger plumbing). |
| `src/qt5/` | Qt 5 GUI, machine inspector, debugger controls, configuration selector & networking dialogs. |
| `slirp/` | Bundled SLiRP networking library for NAT mode. |
| `riscos-progs/` | RISC OS module source code (HostFS, HostFSFiler, ScrollWheel, EtherRPCEm). |
| `poduleroms/` | Compiled podule ROM images loaded by the emulator. |
| `hostfs/` | Default HostFS content (legacy location, now per-machine in `machines/<name>/hostfs/`). |
| `shared/` | Common shared folder accessible from all RISC OS instances. |
| `roms/` | Place your licensed RISC OS ROM images here (see [official instructions](http://www.marutan.net/rpcemu/manual/romimage.html)). |
| `configs/` | Machine configuration files. |
| `machines/` | Per-machine data directories (auto-created). |

## HostFS and Shared drives
The emulator provides two filing system icons on the RISC OS icon bar:

| Icon | RISC OS Path | Host Path | Purpose |
| --- | --- | --- | --- |
| **HostFS** | `HostFS::HostFS.$` | `machines/<name>/hostfs/` | Per-machine storage, isolated between configurations. |
| **Shared** | `HostFS::Shared.$` | `shared/` | Common storage visible to all machine instances. |

This allows you to:
- Keep machine-specific files (applications, settings) isolated in each machine's HostFS
- Share common files, utilities, or applications between all machines via the Shared drive
- Transfer files between different machine configurations without network setup

## Using the machine selector
1. On startup, the **Machine Selector** dialog appears listing all available configurations.
2. **New** – Create a new machine configuration with default settings.
3. **Edit** – Modify machine settings (model, RAM, ROM, networking, hard discs).
4. **Clone** – Duplicate an existing configuration.
5. **Delete** – Remove a configuration and optionally its associated data.
6. **Start** – Launch the emulator with the selected configuration.

## Using the debugger & machine inspector
1. Open **Debug → Machine Inspector…** to reveal the inspector window.
2. Tabs provide:
   - **CPU** – Registers R0–R15, CPSR/mode, performance counters, interpreter vs dynarec state.
   - **Pipeline** – Upcoming instruction addresses and opcodes for easy tracing.
   - **Peripherals** – VIDC palette & doublescan mode, SuperIO registers, IDE channel state, podule IRQ mapping.
   - **Debugger** – Run/Pause, Step, Step ×5 buttons, breakpoint & watchpoint lists, last hit summaries.
3. Auto-refresh every 500 ms keeps the view current; toggle it or hit **Refresh now** for manual polling.
4. Breakpoints and watchpoints entered in the inspector apply even while the dynarec is active, thanks to the shared debugger hooks in `ArmDynarec.c`.

## Differences versus upstream RPCEmu
- Qt front-end reworked for stability with modern Qt 5 deployments.
- Multi-machine configuration system with isolated per-machine storage.
- Quick machine switching via Recent Machines menu.
- ROM selection per configuration.
- **Dual HostFS drives** – Per-machine HostFS plus a common Shared drive for cross-machine file sharing.
- Access/ShareFS networking support for file sharing between emulated machines.
- **Full FPA emulation** – Complete FPA10 floating-point coprocessor with all operations and cycle timing (see below).
- **Pixel Perfect scaling** – Integer scaling option for sharp pixels without blur.
- **Built-in VNC server** – Remote desktop access using libvncserver (Linux). Enable via Settings → VNC Server.
- **Sound mute toggle** – Quickly mute/unmute emulator audio.
- **Recent disc images** – Quick access menus for recently used floppy and CD-ROM images.
- **Floppy eject actions** – Eject floppy discs via menu or keyboard shortcuts.
- In-depth machine inspector and debugger tooling not present upstream.
- Comprehensive keyboard shortcuts for all major functions.
- Dynarec pause logic patched so debugger operations are consistent across cores.
- Custom ARM cross-assembler toolchain support for building RISC OS modules.

## Keyboard shortcuts
| Shortcut | Action |
| --- | --- |
| **Ctrl+S** | Save screenshot |
| **Ctrl+R** | Reset machine |
| **Ctrl+Q** | Quit emulator |
| **Ctrl+1** | Load floppy disc into drive :0 |
| **Ctrl+2** | Load floppy disc into drive :1 |
| **Ctrl+Shift+1** | Eject floppy from drive :0 |
| **Ctrl+Shift+2** | Eject floppy from drive :1 |
| **Ctrl+,** | Open Settings/Configure dialog |
| **F10** | Toggle sound mute |
| **F11** | Toggle full-screen mode |
| **F5** | Run (resume emulation) |
| **F6** | Pause emulation |
| **F7** | Single step |
| **F8** | Step ×5 |
| **Ctrl+I** | Open Machine Inspector |
| **F1** | Open online manual |

## FPA (Floating Point Accelerator) emulation
This fork includes complete FPA10 coprocessor emulation, enabling RISC OS applications that use hardware floating-point to run correctly. The implementation includes:

### Supported operations
| Category | Operations |
| --- | --- |
| **Dyadic** | ADF, MUF, SUF, RSF, DVF, RDF, POW, RPW, RMF, FML, FDV, FRD, POL |
| **Monadic** | MVF, MNF, ABS, RND, SQT, LOG, LGN, EXP, SIN, COS, TAN, ASN, ACS, ATN, URD, NRM |
| **Conversion** | FIX (float→int), FLT (int→float) with all IEEE rounding modes |
| **Comparison** | CMF, CMFE, CNF, CNFE with proper NaN exception handling |
| **Transfer** | LDF, STF, LFM, SFM (load/store single and multiple) |

### Features
- **Cycle-accurate timing** – Each operation has appropriate cycle costs (e.g., 10 cycles for load/store, 150 cycles for transcendental functions like SIN/COS/TAN).
- **IEEE rounding modes** – FIX, RND, and URD support all four IEEE 754 rounding modes (nearest, +∞, −∞, zero).
- **NaN handling** – Comparison operations correctly set the V flag for unordered (NaN) comparisons.
- **Dynarec integration** – FPA works with both the interpreter and dynamic recompiler.

### Compatibility notes
- Ensure 16-bit sound is enabled in RISC OS configuration for correct audio playback when using FPA-intensive applications.
- Some applications (e.g., !AMPlayer) may exhibit timing-sensitive behaviour; if issues occur, test with the interpreter mode.
 
## Troubleshooting
| Symptom | Remedy |
| --- | --- |
| Emulator launches without a window | Ensure Qt 5 runtime libs are discoverable; this fork delays snapshot requests to avoid deadlock, so missing Qt plugins are the usual culprit. |
| No network connectivity | Confirm SLiRP support was compiled in (`CONFIG_SLIRP`) and NAT rules are configured. |
| ROM not found | Ensure ROM files are placed in the `roms/` directory and selected in the machine configuration. |
| Machine data not persisting | Check that the `machines/<name>/` directory exists and is writable. |

## Contributing
Issues and pull requests are welcome, especially around debugger/inspector features.

## License & credits
- Licensed under the **GNU General Public License v2**. See `COPYING` for details.
- Original emulator by the RPCEmu contributors.
 