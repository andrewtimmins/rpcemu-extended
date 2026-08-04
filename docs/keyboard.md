# Keyboard

How a key press on the host becomes a key press in RISC OS, why it is done by
physical position rather than by character, and how to diagnose it on a platform
you cannot sit in front of.

## The chain

```
host key event
  -> X11 keycode          ("native scancode" throughout the source)
  -> PS/2 Set 2 make code (keyboard_x.c)
  -> IOMD keyboard port   (keyboard.c)
  -> RISC OS, which applies its own keyboard layout
```

The middle currency is an **X11 hardware keycode**. That is a historical choice
rather than an X11 dependency: the value identifies a key by **where it is on
the keyboard**, and `keyboard_map_key()` in `src/gui/keyboard_x.c` is the single
table that turns a position into the make codes a PS/2 keyboard would send.

Two things produce that currency:

| Producer | Source file | Notes |
| --- | --- | --- |
| The GUI | `src/gui/input_helpers.cpp` | A wxWidgets key event |
| VNC | `src/gui/vnc_server.cpp` | An RFB keysym |

## Positions, not characters

**RISC OS applies its own keyboard layout.** So RPCEmu must tell it *which key
was pressed*, not *which character the host decided that key types*. If the host
translates first, the layout is applied twice and the result is wrong in ways
that depend on both layouts.

Where each platform hides the position:

| Platform | Where wxWidgets puts it | What it is |
| --- | --- | --- |
| Linux / GTK | `GetRawKeyFlags()` | An X11 keycode already, so nothing to translate |
| Windows | `GetRawKeyFlags()` | The `WM_KEY*` `lParam`: scancode in bits 16-23, extended flag in bit 24 |
| macOS | `GetRawKeyCode()` | `[NSEvent keyCode]`, an ADB keycode |

`src/gui/keymap_platform.c` holds the Windows and macOS tables. Neither is
conditionally compiled, which is what lets `tests/test_keymap.c` check all three
platforms' mappings from whichever one the test happens to run on.

For Windows the mapping is arithmetic, not a table: **X11 keycode = Set 1
scancode + 8**, for everything from Escape (`0x01` to `0x09`) to F12 (`0x58` to
`0x60`). Linux derives its evdev keycodes from the same AT numbering with the
same offset. Only the `E0`-prefixed keys need a table, because they duplicate
scancodes from the main block. For macOS a full table is unavoidable, since ADB
numbered keys by the 1984 keyboard's matrix.

### The macOS keycode numbers are checked by the compiler

`keymap_platform.c` must hold literal numbers so it compiles and can be tested
everywhere, which leaves the risk that a literal is simply wrong. So when built
on macOS it asserts every one of them against Apple's own `kVK_` constants from
`<Carbon/HIToolbox/Events.h>`. A mistyped number is a **macOS build failure**
rather than a key that quietly does the wrong thing on someone else's Mac. The
include is optional (`__has_include`), so it can never be the reason a build
fails to configure.

## What this fixed

Until August 2026 the Windows and macOS paths mapped the **character** that
`GetKeyCode()` reported onto a table written for a **UK** layout. Three reported
faults came from that one decision.

- **[#88](https://github.com/andrewtimmins/rpcemu-extended/issues/88), German
  keyboards.** Umlauts and `ß < > |` were completely dead, because no entry in a
  UK table types those characters. `z` and `y` swapped "the wrong way around"
  when the RISC OS layout was changed, because Windows had already applied German
  and RISC OS then applied it again. Digits worked, because the digit row is in
  the same place on both layouts. A UK host layout with a German RISC OS layout
  worked perfectly, which is the system working by accident.
- **[#91](https://github.com/andrewtimmins/rpcemu-extended/issues/91), the third
  mouse button on macOS.** wxWidgets reports **Command** as `WXK_CONTROL` and the
  physical Control key as `WXK_RAW_CONTROL`, so at the character level Command and
  Control are one key and neither could be spared for the menu button. By
  position they are four distinct keys.
- **[#70](https://github.com/andrewtimmins/rpcemu-extended/issues/70), partly.**
  Left and right Shift, and left and right Ctrl, were reported to the guest as
  the left-hand key in both cases. See the caveat below for the part of #70 this
  does not explain.

Also fixed along the way: **AltGr reached nothing at all, on every platform.**
Windows and macOS reported it as left Alt, and X11 reported keycode `0x6c` which
`keyboard_x.c` had no entry for. A German keyboard cannot manage without it.

## macOS: the two keys a RiscPC has not got

Handled as policy in `input_helpers.cpp`, deliberately not in the mapping table,
so the table stays a one-to-one statement about positions and can be checked for
collisions:

| Mac key | Sent to RISC OS as | Why |
| --- | --- | --- |
| Left Command | Ctrl | RISC OS uses Ctrl where macOS uses Command, so Cmd+C copies |
| Right Command | Menu | A Mac has no menu key, and RPCEmu offers that as the third mouse button |
| Left / right Option | Left / right Alt | |
| Left / right Control | Left / right Ctrl | |

## Diagnosing a keyboard fault

Set `RPCEMU_KEYBOARD_DEBUG` in the environment and every key event is written to
`rpclog.txt`:

```
Keyboard: down raw=0x0000001e flags=0x001e0001 wxkey=65 position=0x26 scancode=0x26 ps2=yes
```

- `raw` and `flags` are what the host reported, untouched.
- `wxkey` is the character wxWidgets made of it, which is what the old code used.
- `position` is the physical key as an X11 keycode. **Zero means the physical
  tables did not place the key** and the character fallback was used, which is
  the shape of #88 and #91.
- `scancode` is what RPCEmu will send, after the macOS policy above.
- `ps2` says whether `keyboard_map_key()` could deliver it. `DROPPED` means the
  guest received nothing.

This exists because Windows and macOS keyboard behaviour cannot be reproduced on
a Linux development machine. A reporter can be asked for a log rather than for
adjectives. Attach it with *Help → Save Support Files*.

## Testing

`tests/test_keymap.c`, 99 checks, on every platform. It asserts the properties
that were actually violated rather than restating the tables:

- Every mapped position resolves to a key `keyboard_map_key()` can deliver, swept
  exhaustively over both tables. No dead ends.
- No two positions collide on one RISC OS key.
- Both tables agree with each other about where a key is.
- A position means the same thing whatever the host layout claims it types
  (the #88 cases, including AltGr and the two international keys).
- Left and right modifiers stay distinct, and Command stays distinguishable from
  Control (the #91 and #70 cases).
- The `lParam` unpacking ignores the repeat count, the transition bits and the
  Alt context bit, and honours bit 24.

The test was checked by breaking the code on purpose: reintroducing the #88 Y/Z
swap, deleting the AltGr entry, and collapsing right Command onto Control each
make it fail.

## Known gaps

- **AltGr on Windows also generates a phantom left Ctrl.** Windows sends a
  synthetic left-Ctrl press before right Alt, indistinguishable from a real one
  except by timestamp, so the guest sees Ctrl+AltGr. Whether this stops German
  AltGr characters working in RISC OS is unconfirmed; the debug log above will
  show the extra Ctrl if it does.
- **The rest of [#70](https://github.com/andrewtimmins/rpcemu-extended/issues/70)
  is not explained by any of this.** The report is that with Shift held, the
  first thirteen letters came out uppercase and the rest lowercase, and that
  toggling Caps Lock fixed it. Upper and lower case letters map to the *same*
  position, so the mapping cannot produce a case error: something is
  desynchronising the guest's Shift or Caps Lock state. The leading theory is
  that macOS reports Caps Lock as a toggle through `flagsChanged` rather than as
  a press and release, and `NativeKeyPress()` drops a press for a key already
  held, so the guest's caps state can end up inverted relative to the host. That
  is a theory, not a diagnosis, and it needs a Mac.
- Print Screen has no X11 keycode entry and is not passed through.
- The GUI keyboard path is only tested at the mapping layer. Nothing exercises
  wxWidgets event delivery itself.
