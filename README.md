# RPCEmu – Access/ShareFS & Debugger Edition

## Overview
This repository hosts a feature-rich fork of **RPCEmu**, the open-source emulator for Acorn Risc PC and A7000 machines. This edition brings **Access+/ShareFS networking support**, a powerful live machine inspector, integrated debugger controls, and a comprehensive ARM disassembler with full SWI name lookup. 

The project remains distributed under the GNU GPL v2.

---

## 🌐 Access/ShareFS Networking (MAT support)

**RPCEmu can discover and connect to network shares using Access+ and ShareFS using NAT setups!**

**Access+** and **ShareFS** are RISC OS's native network file sharing protocols—the Acorn equivalent of Windows file sharing or NFS. ShareFS uses **Freeway**, a broadcast-based service discovery protocol that lets RISC OS machines automatically find and advertise shared discs on the local network. Previously, this was completely non-functional in emulation because Freeway broadcasts couldn't escape SLiRP's NAT boundary. This fork implements a dedicated **broadcast relay** that bridges Freeway traffic between the emulated network and your real LAN, finally bringing full Access+ connectivity to RPCEmu using NAT.

### What this means for you:
- 🖥️ **Browse network shares from the RISC OS Filer** – Open the Access+ or OmniClient filer and see real network shares appear, just like on physical hardware
- 🔄 **Seamless integration** – No complex setup required; shares are discovered automatically via the broadcast relay

### How it works:
The new `broadcast_relay.c` module captures ShareFS/Freeway discovery broadcasts from the emulated machine, relays them to the real network, and routes responses back. 

### Network configuration:
1. Enable **NAT** networking in **Settings → Networking**
2. Share folders using *Share command in RISC OS
3. Open the network filer – shares appear automatically!

---

## Fork highlights

### 🌐 Networking
- **Access+/ShareFS broadcast relay** – Support for RISC OS network file sharing over NAT.

### 🔍 Machine Inspector & Debugger
- **Machine Inspector window** – Six comprehensive tabs for deep system introspection:
  - **CPU** – Registers R0–R15, CPSR/mode breakdown, real-time MIPS performance counter
  - **Pipeline** – Instruction fetch/decode/execute pipeline visualisation
  - **Disassembly** – Live disassembly around PC with full SWI name resolution
  - **Memory** – Hex viewer with Prev/Next navigation, word size toggle (8/16/32-bit), quick jumps to ROM/RAM/SP/PC, byte pattern search, and copy-to-clipboard
  - **Peripherals** – VIDC, SuperIO, IDE, and podule state snapshots
  - **Debugger** – Run/Pause, Step ×1/×5, breakpoint & watchpoint management

- **Safe debug memory access** – Memory viewer reads physical memory without triggering MMU faults or crashing the guest OS

- **Integrated debugger** – Pause/resume execution, single-step, and manage breakpoints/watchpoints directly in the GUI

- **Dynarec-aware instrumentation** – The ARM dynamic recompiler honours debugger requests via shared hooks so interpreter and dynarec stay in sync

### 📖 ARM Disassembler
- **400+ SWI name entries** – Comprehensive lookup table covering:
  - Core OS SWIs (OS_WriteC through OS_LeaveOS)
  - Wimp, Font, Draw, Sprite, ColourTrans, Sound
  - FileCore, ADFS, RAMFS, CDFS, DOSFS, ResourceFS
  - MessageTrans, Territory, Hourglass, Debugger
  - Full Toolbox suite (Window, Menu, Iconbar, all gadgets)
  - DeviceFS, PDriver, ScreenModes, DMA, JPEG, Freeway, ShareFS
  - And many more from the official RISC OS PRM

### 🧵 Thread-safe architecture
- **Snapshot plumbing** – Thread-safe `MachineSnapshot` and peripheral snapshot structs marshal detailed state off the emulator thread for UI consumption without stalls

---

## Project layout
| Path | Purpose |
| --- | --- |
| `src/` | Core emulator engine (ARM interpreter, dynarec, hardware devices, networking, debugger plumbing) |
| `src/qt5/` | Qt 5 GUI, machine inspector, debugger controls, NAT configuration dialogs |
| `src/slirp/` | Bundled SLiRP networking library for NAT mode |
| `hostfs/`, `poduleroms/`, `riscos-progs/` | Support utilities, ROM stubs, and example binaries |
| `roms/` | Place your licensed RISC OS ROM images here ([instructions](http://www.marutan.net/rpcemu/manual/romimage.html)) |

---

## Using the Machine Inspector

1. Open **Debug → Machine Inspector…** to reveal the inspector window
2. **Tabs provide:**
   - **CPU** – Registers, flags, mode, live MIPS counter, interpreter/dynarec state
   - **Pipeline** – Instruction addresses and opcodes in the ARM pipeline
   - **Disassembly** – Disassembled instructions around PC with SWI names resolved
   - **Memory** – Navigate physical memory, change word sizes, search for byte patterns, jump to key addresses
   - **Peripherals** – VIDC palette & timing, SuperIO registers, IDE channel state, podule mapping
   - **Debugger** – Execution control, breakpoint/watchpoint lists, halt reason display
3. Auto-refresh (500 ms) keeps the view current; toggle it or click **Refresh** for manual polling
4. Breakpoints and watchpoints work even while the dynarec is active

---

## Differences versus upstream RPCEmu

| Feature | Upstream | This Fork |
| --- | --- | --- |
| Access+/ShareFS networking | ❌ Not functional | ✅ Full broadcast relay support |
| Machine Inspector | ❌ None | ✅ 6-tab deep inspection window |
| Memory viewer | ❌ None | ✅ With search, word sizes, quick jumps |
| SWI name lookup | ❌ Basic/none | ✅ 400+ entries from official PRM |
| Debugger integration | ❌ Limited | ✅ Full breakpoint/watchpoint/step support |
| Dynarec debug hooks | ❌ None | ✅ Consistent debugging across cores |

---

## Troubleshooting

| Symptom | Remedy |
| --- | --- |
| Emulator launches without a window | Ensure Qt 5 runtime libs are discoverable; check for missing Qt plugins |
| No network connectivity | Confirm SLiRP support compiled in (`CONFIG_SLIRP`) and NAT mode enabled |
| ShareFS shows no shares | Check broadcast relay is active; verify Access+ credentials in RISC OS |
| Memory viewer causes crash | Fixed in this fork – now uses safe physical memory reads |
| Debugger doesn't break in dynarec | Ensure `debugger_requires_instruction_hook` is enabled (automatic) |

---

## Building

```bash
cd src/qt5
qmake rpcemu.pro
make -j$(nproc)
```

The resulting `rpcemu-recompiler` binary will be placed in the parent directory.

---

## Contributing
Issues and pull requests are welcome, especially around networking, debugger, and inspector features.
