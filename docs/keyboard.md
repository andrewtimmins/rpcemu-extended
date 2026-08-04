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
on macOS it asserts every one of them against Apple's own `kVK_` constants. A
mistyped number is a **macOS build failure** rather than a key that quietly does
the wrong thing on someone else's Mac.

The include is the umbrella header, **`<Carbon/Carbon.h>`**. The constants live in
`Carbon/HIToolbox/Events.h`, but HIToolbox is a subframework and that path does
not resolve on its own. Asking for it directly is what the first version did, and
because the whole block was wrapped in `__has_include` and said nothing either
way, **every assertion was silently skipped on every macOS build** while the table
looked as though it had been checked. A check that quietly does not run is worse
than no check, because it gets mistaken for one.

So the outcome is now announced with `#pragma message` and appears in the macOS
build log, either `macOS keycodes ARE being checked` or a warning that they are
`UNVERIFIED`. It stays conditional rather than a hard error, since a missing SDK
header should never be why a port fails to build.

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
- **[#70](https://github.com/andrewtimmins/rpcemu-extended/issues/70), shifted
  characters on macOS.** wxWidgets has only one `WXK_SHIFT` and no code for the
  right-hand key, so **both Shift keys became the same scancode**. Combined with
  `NativeKeyPress()` ignoring a press for a scancode it already holds, changing
  hands mid-typing silently released Shift in the guest: the second Shift's press
  was swallowed as a duplicate, and the first Shift's release then cancelled a
  modifier that was still physically held. Every letter after that arrived
  unshifted, which is the reported `QWERTYUIOPASD` followed by `fghjklzxcvbnm`.
  The reporter's observation that toggling Caps Lock fixed it needs no more
  explanation than the obvious one: with caps on, letters are uppercase without
  Shift, so the broken path is bypassed. Left and right Shift are now distinct,
  as are left and right Ctrl.

  Worth recording how this was nearly missed. The letter table folded lowercase
  onto uppercase before lookup, so it was provably case-independent, and that was
  taken as showing the mapping could not cause a case error. True as far as it
  went, and it stopped one step short: the *modifier* collapsing is a different
  mechanism in the same function, and it produces exactly the reported symptom.
  An elaborate Caps Lock desynchronisation theory got invented to fill the gap
  that better reasoning would have closed.

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

### Which keys are held

`tests/test_held_keys.c`, 42 checks, covering `src/gui/held_keys.c`.

A scancode is not a key. Several physical keys can map to one RISC OS key, and on
macOS that is **deliberate and permanent**, since both Command and Control are
sent as Ctrl so that Cmd+C copies. Track what is held as a set of scancodes and
letting go of one of them releases a key the user is still holding, which is the
mechanism behind #70.

So held keys are keyed by the **physical** key, and the guest is told when the
first key mapping to a scancode goes down and again when the last one comes up.
The cases cover the Shift hand-over, two physical keys sharing one scancode in
both release orders, a repeated press of one key counting once (which a plain
reference count would get wrong, leaving the key stuck down), a release taking the
scancode recorded at press time rather than from the release event, releasing
everything on focus loss with each scancode released exactly once, and running out
of capacity without press and release losing symmetry.

Also checked by breaking it: reverting `held_keys.c` to the old
scancode-set behaviour fails 6 checks.

## Known gaps

- **AltGr on Windows also generates a phantom left Ctrl.** Windows sends a
  synthetic left-Ctrl press before right Alt, indistinguishable from a real one
  except by timestamp, so the guest sees Ctrl+AltGr. Whether this stops German
  AltGr characters working in RISC OS is unconfirmed; the debug log above will
  show the extra Ctrl if it does.
- Print Screen has no X11 keycode entry and is not passed through.
- The GUI keyboard path is only tested at the mapping layer. Nothing exercises
  wxWidgets event delivery itself.
