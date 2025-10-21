# RPCEmu – Debugger & Inspector Edition

## Overview
This repository hosts a feature-rich fork of **RPCEmu**, the open-source emulator for Acorn Risc PC and A7000 machines. Alongside the standard emulator core, this edition layers in a modern Qt 5 front-end, a live machine inspector, integrated debugger controls. The project remains distributed under the GNU GPL v2.

## Fork highlights
- **Machine Inspector window** – Inspect CPU registers, pipeline state, MMU flags, performance counters, and key peripheral snapshots (VIDC, SuperIO, IDE, podules) with optional auto-refresh.
- **Integrated debugger** – Pause/resume execution, single-step (×1/×5), and manage breakpoints and watchpoints directly in the GUI, with clear status readouts for the last halt reason.
- **Dynarec-aware instrumentation** – The ARM dynamic recompiler honours debugger pause requests, breakpoints, and watchpoints via shared hooks (`debugger_requires_instruction_hook`) so that interpreter and dynarec stay in sync.
- **Snapshot plumbing** – Thread-safe `MachineSnapshot` and peripheral snapshot structs marshal detailed state off the emulator thread for UI consumption without stalls.


## Project layout
| Path | Purpose |
| --- | --- |
| `src/` | Core emulator engine (ARM interpreter, dynarec, hardware devices, debugger plumbing). |
| `src/qt5/` | Qt 5 GUI, machine inspector, debugger controls, configuration & networking dialogs. |
| `slirp/` | Bundled SLiRP networking library for NAT mode. |
| `hostfs/`, `poduleroms/`, `riscos-progs/` | Support utilities, ROM stubs, and example binaries for HostFS/podule integration. |
| `roms/` | Place your licensed RISC OS ROM images here (see [official instructions](http://www.marutan.net/rpcemu/manual/romimage.html)). |

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
- In-depth machine inspector and debugger tooling not present upstream.
- Dynarec pause logic patched so debugger operations are consistent across cores.
- Additional utilities and dialogs for networking configuration.
 
## Troubleshooting
| Symptom | Remedy |
| --- | --- |
| Emulator launches without a window | Ensure Qt 5 runtime libs are discoverable; this fork delays snapshot requests to avoid deadlock, so missing Qt plugins are the usual culprit. |
| Debugger stuck on “Pausing…” | Verify you built this fork (includes dynarec hooks) and that `debugger_requires_instruction_hook` changes are present. |
| No network connectivity | Confirm SLiRP support was compiled in (`CONFIG_SLIRP`) and NAT rules are configured. |

## Contributing
Issues and pull requests are welcome, especially around debugger/inspector features.

## License & credits
- Licensed under the **GNU General Public License v2**. See `COPYING` for details.
- Original emulator by the RPCEmu contributors.
- Debugger, inspector, and stability enhancements developed within this fork to support modern RISC OS development workflows.
 