# Display settings

RPCEmu's display settings answer two questions, and they are easy to confuse
because both of them make the picture bigger.

1. **How big is the RISC OS desktop?** How many pixels RISC OS has to draw on.
   Changing this changes the screen mode inside the guest, and RISC OS
   rearranges every window on its desktop when it happens.
2. **How is that desktop drawn in the window?** Whether those pixels are shown
   one for one, magnified, or stretched. Changing this changes nothing inside
   the guest at all.

*Settings* has one submenu for each, and the machine editor's Options page has
the same two groups with the same names, so a machine can be set up before it
starts. Full screen is a third, separate thing: it is where the window goes, and
both answers above still apply inside it.

## RISC OS Screen Size

| Choice | What happens |
| --- | --- |
| **Best for This Display** | When the machine starts, the largest standard mode that fits this display and this machine's display memory. The desktop then stays that size. This is the default. |
| **Match the Window** | Resize the window and RISC OS changes screen mode to suit, so the desktop grows and shrinks with it. |
| **Fixed Size** | Always one mode, whatever the display or the window is doing. |

### How the size is actually chosen

RISC OS will not use a screen mode its monitor definition does not offer, so
RPCEmu synthesises one. At ROM load it finds the monitor EDID block inside the
RISC OS 5 ROM image and rewrites its timings, making the preferred (native) mode
the largest standard mode that fits two limits:

- the bound this setting implies (below), and
- the display memory the machine has, budgeted at 32 bits per pixel because that
  is the deepest the desktop may choose.

The bound differs per choice, and this is the part worth knowing:

| Choice | Bound |
| --- | --- |
| Best for This Display | The display's **work area** - what a window may occupy, so the menu bar, dock or taskbar are excluded |
| Match the Window | The **whole display**, since the window may be dragged out to any of it and full screen can use all of it |
| Fixed Size | The size asked for, so that mode is certainly offered |

The work area matters for the default. At actual size the window is the same
size as the desktop, so a mode taller than the work area gives a window whose
title bar starts above the top of the screen, where it cannot be grabbed to move
or resize the window back. Bounding by the work area means the default never
asks for a size that cannot be shown.

RISC OS 3 and 4 have no EDID block in their ROMs and are left alone; their mode
lists come from the monitor definition files they ship with.

### Match the Window, in practice

The guest is told the window's size and picks the largest standard mode that
fits, so the desktop follows the window one rung of the mode ladder at a time -
1024 x 768, 1280 x 1024, 1680 x 1050 and so on, not every pixel in between.

Two things stop this being annoying:

- **It is debounced.** A resize drag fires continuously; only the size the drag
  settles on is sent, because each mode change reflows every window on the RISC
  OS desktop.
- **It is quantised before it is sent.** Dragging across pixels that land on the
  same standard mode tells the guest nothing, so it does not keep changing to
  the mode it is already in.

Paired with **Actual Size** the window then closes the gap: once the guest has
adopted a mode, the window is resized to it, so the result is a sharp desktop at
1:1 with no border. It feels like the window snapping to real screen modes, which
is the combination most people want from this option. The snap is bounded by the
display's work area and nothing else, so the desktop is free to grow to whatever
the display allows.

The window is not *locked* to the desktop in this mode. It cannot be: the window
decides the mode and the mode would then decide the window, which is a loop. What
stops that becoming one is that the two directions are told apart. RPCEmu
remembers the desktop size it last saw settle, so a change the guest made for its
own reasons is followed with the window, and a window that has moved while the
guest stood still is answered by asking the guest for a new mode.

### The guest side

None of this works without the RPCEmu Support module, which polls the emulator
for a mode to adopt and issues the `*WimpMode`. It ships as a podule ROM with the
emulator, so there is nothing to install; a guest without it (RISC OS 3 and 4,
which do not load it) keeps the mode it booted with whatever these settings say.

The module used to record the first mode it was ever offered as a baseline and do
nothing with it, which was correct while the emulator reported the host display
continuously - the first reading was simply what the display already was, and
reflowing the desktop over it at boot would have been an unasked-for surprise. It
is the wrong shape for a setting that publishes nothing until somebody asks: the
first real request became the baseline and vanished, so the first drag of a window
edge did nothing and only the second worked. Removed in module version 0.03.

### Fixed Size

The list offered is the standard modes the machine's display memory can hold, so
it changes with the VRAM fitted and with the graphics card. A 2MB machine stops
at 800 x 600; the graphics card carries 15MB of its own and reaches 2560 x 1440.
Only modes that will work are listed - a mode whose framebuffer does not fit
earns "not suitable for displaying the desktop" from RISC OS, which tells the
user nothing about why.

In the machine editor the list is rebuilt as soon as the VRAM or the graphics
card selection changes, so the effect of fitting the card is visible without
saving and reopening.

## Show In Window

| Choice | What happens |
| --- | --- |
| **Actual Size** | One RISC OS pixel per screen pixel. The window is sized to the desktop and cannot be resized, unless the screen size is following the window. The default. |
| **Whole Multiples Only** | The window resizes freely, but the desktop is only ever drawn at 2x, 3x and so on, so pixels stay square and sharp. A border appears when the window is between two multiples. |
| **Scale to Fit** | The window resizes freely and the desktop is stretched to fill it, keeping its shape. Any size, at the cost of some softness. |

## Full screen

*Settings → Full Screen*, or Alt+Enter, which also leaves again. A machine can
start there: *Start this machine full screen* on the machine editor's Display
page.

Full screen is not one of the two questions above. The screen size and drawing
choices both still apply inside it, and the interesting combinations are:

- **Match the Window + Actual Size**: the desktop is the whole display at 1:1,
  with no scaling anywhere in the path. The sharpest result available.
- **Best for This Display + Scale to Fit**: the desktop keeps the mode it booted
  with and is stretched to fill the display.

## What this replaced

Until this was reworked there were four switches - *Pixel Perfect*, *Fit to
Window*, *Follow Host Display Size*, and an unnamed default that was "none of
the above" - plus the EDID synthesis, which had no interface at all.

Two of those four scaled the same number of guest pixels, one changed how many
there were, and nothing said which was which. *Pixel Perfect* and *Fit to
Window* silently cleared each other, so a row of independent-looking checkboxes
behaved as though clicks were being lost. *Follow Host Display Size* only acted
when a monitor was attached or its resolution altered, never on the window, so
switching it on appeared to do nothing whatever. And the setting that decided
which modes RISC OS offered at all was invisible: it came from whichever display
the application happened to be on when the machine booted.

Configurations written before the change are converted on first read, once:

| Old key | Becomes |
| --- | --- |
| `fit_to_window=1` | `display_scaling` = Scale to Fit |
| `integer_scaling=1` | `display_scaling` = Whole Multiples Only |
| neither | `display_scaling` = Actual Size |
| `follow_host_display=1` | `screen_size` = Match the Window |
| `follow_host_display=0` | `screen_size` = Best for This Display |

`follow_host_display` becoming Match the Window is not exactly what it did, and
is the honest successor even so: what it was for was a RISC OS desktop that
tracks the front end, and the reason it was reported as not working is that it
hardly ever fired.

The new keys are `display_scaling`, `screen_size`, `screen_size_x` and
`screen_size_y`. The old keys are read but no longer written, so an older RPCEmu
and this one cannot end up each reading its own set and neither seeing the
other's edits.

## Where this lives in the code

| File | What it does |
| --- | --- |
| `src/gui/display_options.h` | Every label and explanation, defined once so the menu and the machine editor cannot drift apart |
| `src/display_mode.c` | The standard mode list, and "largest that fits these bounds and this much memory" |
| `src/rpcemu.c` | `rpcemu_edid_bound()`, `rpcemu_request_guest_size()`, `rpcemu_guest_display_target()` |
| `src/rom_patch.c` | Rewrites the monitor EDID in the loaded ROM |
| `src/gui/emulator_panel.cpp` | The three drawing rules, and whether the window is locked to the desktop |
| `src/gui/main_frame.cpp` | The menus, and publishing the window size to the guest |
| `src/hostcmd.c` | `HC_OP_DISPLAY`, which the guest support module polls for a mode to adopt |
| `riscos-progs/RPCEmuSupport/rpcemusupport.s` | The guest side: `poll_display` and `set_display_mode` |
| `src/gui/headless_main.cpp` | Applies a fixed screen size headless, where there is no window |
