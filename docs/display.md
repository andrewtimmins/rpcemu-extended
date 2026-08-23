# Display settings

Two settings: how big the RISC OS desktop is, and how it is drawn in the window.

- ***Settings → RISC OS Screen Size*** — a list of resolutions. The window is
  this size, and the machine is asked to use it.
- ***Settings → Show In Window*** — **Actual Size** (one RISC OS pixel per screen
  pixel) or **Whole Multiples Only** (2x, 3x, still perfectly sharp).

Full screen is separate: it is where the window goes, and Alt+Enter leaves again.

The machine editor's Options page offers the same two, under the same names. The
labels come from `src/gui/display_options.h` so the two views cannot drift apart.

## The window is the size of the desktop

The window is always exactly the size of the RISC OS desktop. Smaller would clip
it - and the first thing lost off the bottom is the icon bar - while larger would
leave a black border. So when the guest's screen mode changes, the window changes
with it, and is re-centred with its title bar kept inside the display's work area
so it can always be grabbed.

What it does **not** do is follow every mode change as it happens. RISC OS changes
mode two or three times on the way to a desktop, and on a machine with the
graphics card fitted the display is handed over to the card part way through as
well. Resizing on each one made the window jump through three sizes and positions
before settling, which is a startup nobody would design on purpose. Each change
now restarts a half-second timer, and the window is sized once the changes stop -
so a normal boot is two sizes, the mode RISC OS starts in and the desktop it
arrives at, and both of them are worth seeing.

The screen-size setting decides what the guest is *asked* to be. What it actually
is, is what gets shown.

## Why there is no "follow the window" or "best for this display"

Both existed, and both were removed. The reason is a hard constraint in RISC OS:

**RISC OS will not adopt an arbitrary screen mode.** It accepts only modes the
monitor definition in force declares. Verified directly - with the emulator's
synthesised EDID advertising 1232x704 as the monitor's preferred timing,
`*WimpMode X1232 Y704 C16M F60` still answers *"This screen mode is unsuitable for
displaying the desktop"*. Alignment is not the issue either; 1234, 1288 and 1232
were all refused.

So a desktop that follows the window can only ever land on a coarse and
unpredictable set of sizes. Every window size in between left either a black
border or a stretched picture, and every mode change moved and resized the window
while the user was still dragging it. Chasing the window produced worse behaviour
than not chasing it.

A named resolution has none of those problems. The window is that size and stays
that size, the desktop fills it exactly, and nothing moves.

## Which sizes are actually available

The list offered is filtered twice: by what the machine's display memory can hold
(a 2MB machine stops at 800x600; the graphics card carries 15MB of its own and
reaches 2560x1440), and by what the guest has been found to accept.

That second filter is learned, because it cannot be known. The definition RISC OS
is using is normally a **monitor definition file** its own `!Boot` loads, not the
EDID this emulator synthesises, and nothing on the host can read what such a file
contains. Measured on one machine with the graphics card fitted, only six of the
thirteen modes that fit its display memory were accepted:

| Accepted | Refused |
| --- | --- |
| 1920x1200, 1600x1200, 1280x960, 1280x720, 1024x768, 800x600 | 2560x1440, 1920x1080, 1680x1050, 1440x900, 1280x1024, 1152x864, 640x480 |

Not a pattern anyone would guess - 1920x1200 accepted, 1920x1080 refused.

So a size picked by name is checked rather than trusted. If the desktop has not
become that size within two seconds, it is struck off the list and one plain
message says so. Nothing else needs doing: the window is the size of whatever the
desktop actually is, so a refusal simply leaves it where it was.

The size a machine is configured for is asked for at startup but deliberately
*not* checked. That request goes out before RISC OS has a desktop, so a check two
seconds later would find the guest still booting and read it as a refusal - which
is exactly what happened, striking a perfectly good size off the list and dragging
the window down to whatever transient boot mode was on screen at the time.

Refusals are remembered for the session only. The monitor definition belongs to
the guest and changes when it is reconfigured, so remembering them for ever would
outlive the reason for them.

### Getting the full list back

If sizes are being refused, RISC OS is using a monitor definition file. In
Configure, set the monitor type to Auto or EDID rather than a definition file, and
restart the machine. The emulator's own synthesised monitor definition declares
every size in the list.

`*GfxCardStatus` reports which definition is in force - "monitor type 7" means a
definition file, and the card's own note says the same thing.

Note that advertising a size as the monitor's native mode does **not** make RISC
OS boot into it: the desktop mode comes from CMOS and the monitor definition only
vets it. Tested, so that nobody is sent round a loop that does not arrive.

## RISC OS 3 and 4

Neither loads the RPCEmu Support module, which is what polls the emulator for a
screen size and issues the `*WimpMode`. On those, these settings size the window
and nothing else; the desktop keeps the mode it booted with and is drawn centred.

## What this replaced

| Old key | Becomes |
| --- | --- |
| `integer_scaling=1` | `display_scaling` = Whole Multiples Only |
| anything else | `display_scaling` = Actual Size |
| `follow_host_display`, and the later `screen_size` policies | no resolution stored, so one is chosen for this display at first start and then kept |

`screen_size_x` and `screen_size_y` hold the resolution. Zero means "choose one
for this display", which happens once and is then written down - a value, not a
policy that re-decides itself behind the user's back. The old keys are read but no
longer written, so an older RPCEmu and this one cannot each read their own set and
neither see the other's edits.

## Where this lives in the code

| File | What it does |
| --- | --- |
| `src/gui/display_options.h` | Every label, defined once so the menu and the machine editor cannot drift apart |
| `src/display_mode.c` | The list of standard modes, "largest that fits", and the learned refusals |
| `src/rpcemu.c` | `rpcemu_edid_bound()`, `rpcemu_default_screen_size()`, `rpcemu_request_guest_size()`, `rpcemu_guest_display_target()` |
| `src/rom_patch.c` | Rewrites the monitor EDID in the loaded ROM |
| `src/gui/emulator_panel.cpp` | Sized from the configuration, draws the guest centred at a whole-number scale |
| `src/gui/main_frame.cpp` | The menus, `CentreWindowOnScreen()`, and `RequestGuestMode()` with its verification |
| `src/hostcmd.c` | `HC_OP_DISPLAY`, which the guest support module polls for a mode to adopt |
| `riscos-progs/RPCEmuSupport/rpcemusupport.s` | The guest side: `poll_display` and `set_display_mode` |
