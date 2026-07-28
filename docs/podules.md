# Podules (expansion cards)

RPCEmu (Spork Edition) emulates the Risc PC expansion-card ("podule") bus and ships a
small library of emulated podules. You choose which podules a machine has from its
configuration; RISC OS then sees them as ordinary expansion cards.

The subsystem uses a plugin-style ABI adapted from Arculator (`src/podule_api.h`), so
device implementations are self-contained and new ones are easy to add.

---

## Choosing podules for a machine

Open **Settings → Machine…** (or **Edit** a machine in the startup selector) and choose
the **Podules** tab. It shows the eight Risc PC expansion-card slots:

- **Slot 0** is the built-in **RPCEmu Support** ROM and is locked. Everything in
  `poduleroms/` is scanned into that one card, which is how the guest-side modules
  arrive without anything being installed on the hard disc: HostFS and its filer,
  scroll-wheel support, the shared clipboard, the RPCEmuSupport module (the host
  command channel) and SyncClock. See [Where the ROMs live](#where-the-roms-live).
- **Slot 1** is the **network card** when networking is enabled (locked while on).
- **Slots 2–7** (and slot 1 when networking is off) are yours to assign.

Pick a podule per slot from the drop-down. A podule can only occupy **one** slot — once
chosen it disappears from the other slots' lists. Some podules have extra options
(e.g. which host MIDI device to use); those slots get a **Configure** (`…`) button.

Changes are saved into the machine's `.cfg` (an `[Podules]` section, plus a
`[PoduleConfig/…]` section for per-device options) and take effect after the emulator is
**reset**.

---

## Bundled podules

| Podule | RISC OS name | Notes |
| --- | --- | --- |
| **Acorn AKA05** | *ROM podule* | Paged ROM/RAM expansion. |
| **Wild Vision MIDI Max** | *Wild Vision MIDI MAX* | MIDI interface (16550 UART). Host MIDI via ALSA. |
| **Acorn AKA16** | *Midi podule* | MIDI interface (SCC2691 UART). |
| **Acorn AKA12** | *Midi and User Port* | MIDI + user port (SCC2691 + 6522 VIA). |
| **Computer Concepts Lark A16** | *Lark A16* | 16-bit sampler + MIDI (AD1848 codec). Host audio via SDL2, capture via ALSA. |

MIDI devices route to/from real host MIDI ports through **ALSA** — select the in/out
device with the slot's **Configure** button. If RPCEmu was built without ALSA, MIDI is
inert (the cards still appear, but no notes flow).

---

## Where the ROMs live

Podule ROMs are **shipped system components**, read from the shared resource directory —
they are not per-user data:

- Installed (`.deb`): `/usr/share/rpcemu/podules/<name>/`
- Portable (`.tar.gz`): `podules/<name>/` beside the binary

You don't copy these into your `~/RPCEmu` folder; you just select which podules to use.

Three directories are involved, and they are not interchangeable:

| Directory | What it holds |
| --- | --- |
| `podules/<name>/` | ROMs for the selectable podules described above, one directory each |
| `poduleroms/` | RISC OS modules scanned into the built-in slot-0 card. Every regular file is picked up (`.txt` and dotfiles aside), so adding a module is a matter of dropping it in |
| `gfxroms/` | The graphics card's own display driver, which travels in that card's ROM rather than slot 0. See [gfxcard.md](gfxcard.md) |

---

## Adding your own podule (plugin ABI)

Podules are written against `src/podule_api.h` — the same `podule_probe()` /
`podule_header_t` contract Arculator uses. There are two ways to add one:

1. **Built-in**: drop a `<name>.c` in `src/podules/`, register its `<name>_probe` in the
   `internal_podules[]` table in `src/podules.c`, and add it to `src/CMakeLists.txt`.
2. **External plugin**: build a shared library exposing `podule_probe` and place it at
   `<resourcedir>/podules/<name>/<name>.so`. It is discovered and loaded at start-up
   (`dlopen`), with no rebuild of RPCEmu.

Shared device-support code (UART, MIDI/sound back-ends, VIA, ADC) lives in
`src/podules/common/`.

The podule subsystem — the `src/podule_api.h` ABI and the implementations under
`src/podules/` — is derived from **[Arculator](https://b-em.bbcmicro.com/arculator/)**,
Sarah Walker's Acorn Archimedes emulator, which is likewise GPL v2. Copyright of
that code remains with Sarah Walker and the Arculator contributors.

---

## What is and isn't supported

The Risc PC bus decodes podule accesses in **IOC** and **EASI** space, so RPCEmu drives
podules through those. It does **not** decode Archimedes-style **MEMC** podule space, so
expansion cards that move their data through MEMC (for example several Archimedes SCSI
and analogue/ADC cards) cannot fully function here even though their control registers
and ROM may enumerate.
