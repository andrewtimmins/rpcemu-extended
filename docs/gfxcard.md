# Graphics card

An optional emulated graphics expansion card whose display memory is its own,
rather than the motherboard's VRAM. That is the whole point of it: the modes a
Risc PC can show are limited by how much VRAM is fitted, and this card is not.

| Display memory | Largest mode at 32bpp |
| --- | --- |
| 2MB of VRAM (Kinetic, and the usual Risc PC) | 800 x 600 |
| 8MB of VRAM | 1920 x 1080 |
| The card's own 15MB | 2560 x 1440 |

Verified on a Kinetic with 2MB of VRAM: 1920 x 1080 in 16M colours (8.3MB of
screen) is refused by VIDC20 and displayed by the card.

The card is off by default. Turn it on in *Settings → Machine → Graphics card*,
and the machine gains an expansion card in the next free EASI slot. Nothing about
an existing machine changes until you do.

Tick **"...and make it the display at boot"** as well and there is nothing else to
do: RISC OS comes up on the card, at whatever mode the desktop is configured for,
with no `*GfxCardOn` and no mode change once the desktop is already up. The driver
also selects the EDID monitor type for that session if the configured one would not
offer the card's modes - a selection, not a change to your configuration, so a boot
without the card behaves exactly as before.

Leave it unticked and RISC OS keeps using VIDC20 until you run `*GfxCardOn`.

## Using it

The card carries its own display driver in its ROM, so RISC OS starts the driver
at boot with nothing to install. Three commands:

```
*GfxCardStatus      show the card, the driver, and what is being displayed
*GfxCardOn          make the card the display
*GfxCardOff         hand the display back to VIDC20
```

`*GfxCardOn` keeps the mode you are in, so a desktop at 1920 x 1080 stays there,
now scanning out of the card. Afterwards, any mode the monitor offers:

```
*WimpMode X1920 Y1080 C256
```

### The monitor type decides what you can select

Modes are offered by the **monitor definition in force**, not by the card. The
card can hold 2560 x 1440; whether RISC OS will let you select it depends on the
configured monitor type:

| Monitor type | What is offered |
| --- | --- |
| `EDID` | The card's own EDID: every mode it can show. This is the one to use. |
| A loaded definition (`*LoadModeFile`) | Whatever that file defines |
| `VGA`, `SVGA`, `Auto`, ... | Whatever that monitor type defines - `VGA` is 640x480 and nothing else |

So on a machine configured for VGA or Auto the card appears not to work, when in
fact nothing larger is on offer to select. One command and a reset fixes it:

```
*Configure MonitorType EDID
```

`*GfxCardStatus` reports the monitor type and says when it is the thing standing
in the way, and `*GfxCardOn` says so at the moment of switching. *Settings → Follow Host Display Size*
with `MonitorType EDID` is the easy route: the emulator synthesises a monitor
definition from the host's real display, up to 2560 x 1440.

If a mode is too large for the card, RISC OS says "Mode not available" rather
than trying it, because the driver vets each mode against the card's memory
before the kernel commits to it.

`*GfxCardOff` returns to VIDC20, and so does killing the driver (`*RMKill
RPCEmuGfx`), which hands the display back before it goes.

## The utility in the Apps folder

The card carries a small application in its ROM, registered with ResourceFS at
initialisation, so it appears as `Resources:$.Apps.!GfxCard` - the Apps folder on
the icon bar. Nothing is installed on a disc, and it is there exactly when the
card is.

Double-clicking it puts the card's icon on the icon bar, with this menu:

```
GfxCard
-------------------------------
Info                          >
Display                       >
-------------------------------
Use Card
Use VIDC20
-------------------------------
Quit
```

*Display* opens a window with what the card is doing, read fresh each time it is
shown:

```
Display Information
Resolution   1280 x 1024, 32bpp
Framestore   5120K of 15360K
Frames       3036
```

Frames is the card actually scanning out, so it is the quickest way to tell
whether the card is driving the screen or merely fitted. *Info* is the usual
program information window.

If RISC OS refuses one of the two commands - which it will if the monitor
definition in force does not offer the mode - the application says so rather
than appearing to do nothing.

### Adding icons, templates and message files

Anything the application needs travels in the same ROM. Drop a file into
`riscos-progs/RPCEmuGfx/` with a RISC OS **filetype suffix** and it appears in the
application directory next time the module is built - no change to the assembler
or the Makefile:

| File | Becomes | Notes |
| --- | --- | --- |
| `!Sprites,ff9` | `Resources:$.Apps.!GfxCard.!Sprites` | Sprite named `!gfxcard`, used for the application and the icon bar |
| `Templates,fec` | `...!GfxCard.Templates` | Wimp templates: `displayinfo` and `proginfo` |
| `Messages,fff` | `...!GfxCard.Messages` | `Name:`, `Purpose:`, `Author:` and `Version:` for the information window |
| `!Boot,feb` | `...!GfxCard.!Boot` | Obey, run by the Filer when it sees the application |
| `!Run,feb` | `...!GfxCard.!Run` | Replaces the generated default |

The names an application always carries - `!Boot`, `!Run`, `!Sprites`,
`Templates`, `Messages`, `!Help` - are taken **with or without** the suffix,
because the type follows from the name. Anything else needs its filetype suffix.

`mkresfs.py` builds the ResourceFS chain from whatever is there, and the module
carries it. Because the files are inside `gfxroms/RPCEmuGfx,ffa`, every release
gets them automatically - all three build scripts stage that directory and the
CMake install rules cover it, so there is nothing to add anywhere else.

### What the application expects of those files

The two windows and their words are the resource files' business, not the
program's: it fills icons by number and looks tokens up by name.

| Where | What goes in it |
| --- | --- |
| `displayinfo` icons 1, 2, 3 | Resolution, framestore, frames - indirected text, any buffer length |
| `proginfo` icons 1, 2, 3, 4 | Name, purpose, author, version |
| `Messages` | One `Token:value` per line: `Name`, `Purpose`, `Author`, `Version` |
| `!Sprites` | A sprite called `!gfxcard` |
| `!Run` | Must `*IconSprites` the sprite file and set `GfxCard$Path`, which is how the application finds Templates and Messages |

Two things about the icon bar icon are worth knowing if it is ever changed: the
sprite has to be named in the icon's **validation string** (`S!gfxcard`), because
the sprite-only form of the icon data draws nothing on the icon bar; and the
application asks Wimp_Initialise for `Message_MenuWarning` by name rather than
passing 0 for "every message", which this Wimp does not honour. Neither failure
reports an error - the icon or the windows simply never appear.

The application is `riscos-progs/RPCEmuGfx/app.s`, an absolute file assembled
alongside the driver and carried in the same ROM. It is written in assembler like
the rest of the guest code here; there is no RISC OS C toolchain in this build
environment, and BASIC would have to be tokenised at every launch.

### Reading the card's state from a program

`*GfxCardVars` puts the card's state into three system variables, ready for
anything that wants to display it:

```
*GfxCardVars
*Show GfxCard*
GfxCard$Frames : 1526
GfxCard$Mode   : 1920 x 1080, 32bpp
GfxCard$Store  : 8100K of 15360K
```

The values are bare, with no words around them, so whatever displays them can
label them itself - which is what the application's window does.

That indirection is necessary rather than decorative. The card's registers are in
expansion card space, which RISC OS maps for **privileged access only**: a desktop
application cannot read them however much it knows about the card. The driver can,
so it formats what there is to say and leaves it somewhere anything can pick it up.

`RPCEmuGfx$Base` holds the logical address of the registers for code that *is*
privileged (a module, or the debugger). The register map is in `src/gfxcard.h`;
register *n* is at `RPCEmuGfx$Base + n * 4`.

## What the card is

An ordinary expansion card in an ordinary EASI slot, with 16MB of address space
laid out as:

| EASI offset | Contents |
| --- | --- |
| `0x000000` | Expansion card ROM (identity bytes, description, the driver module) |
| `0x080000` | Registers, 32-bit, one word each |
| `0x100000` | Framestore, 15MB |

There is no fabricated address window, no patched ROM and no dependence on the
VRAM setting. The guest reaches the card the way it reaches any expansion card,
and everything it needs to know it reads from the card's own registers: what
depths the card can scan out, where its framestore is and how large it is, and
the largest mode it will accept.

The framestore starts on a megabyte boundary because that is how the RISC OS
kernel maps a card-hosted framestore, rounding the address down and the size up
to megabytes. The registers are in EASI space rather than the card's IOC space
because EASI is a 32-bit bus: a register is one word, read and written as one
word.

## What the driver is

`riscos-progs/RPCEmuGfx/` — an ordinary GraphicsV driver, assembled into
`gfxroms/RPCEmuGfx,ffa` and carried in the card's ROM. It registers with
`OS_ScreenMode 64`, claims GraphicsV, and answers the calls the kernel makes to
drive a display:

| GraphicsV call | What the driver does |
| --- | --- |
| DisplayFeatures (8) | Reports a separate framestore, hardware scroll, and 8bpp/32bpp |
| FramestoreAddress (9) | Where the card's memory is, for the kernel to map |
| PixelFormats (17) | 256-colour and 16M-colour |
| VetMode (7) | Accepts a mode only if it fits the card |
| SetMode (2) | Programs the geometry and starts scanning out |
| SetDMAAddress (6) | Moves the displayed window, which is how RISC OS scrolls |
| SetBlank (4) | Blanks the output |
| UpdatePointer (5) | Position and shape of the pointer, which the card draws |
| WritePaletteEntry / Entries / Read (10-12) | The screen palette, and the pointer's three colours |
| IICOp (14) | Serves the monitor's EDID, so the mode list survives the switch |

The vsync interrupt the card raises is passed on as `GraphicsV_VSync`, which is
what makes the pointer move smoothly and palette changes land on a frame
boundary. If the driver cannot claim the card's interrupt it tells the kernel so,
and the kernel makes its own vsyncs from the centisecond ticker instead:
`*GfxCardStatus` says which is happening.

### Why the driver serves the EDID

RISC OS re-reads the monitor's EDID **from the display driver** whenever the
display driver changes: ScreenModes calls `readedid()` from its
Service_DisplayChanged handler, which is a DDC read over `GraphicsV_IICOp`. A
driver that cannot answer leaves the machine with a fallback monitor definition
and only the handful of modes that go with it - so switching to the card would
*lose* modes rather than gain them.

The card therefore serves the same EDID block the emulator presents to the ROM
(`src/edid.c`, published by `rom_patch.c`), through three registers: the size
available, an index, and a data register that steps the index on. Switching to
the card leaves the machine believing in exactly the monitor it booted with.

### Why one ROM word is patched

RISC OS decides which modes exist by measuring each one against the video memory
it believes the machine has, and that figure is the fitted VRAM: a driver with a
framestore of its own is invisible to it. With 2MB of VRAM the card would be held
to 2MB of screen however much it carries.

The figure comes from the kernel's `GetBandwidthAndSize`, which reads the VRAM
size out of low memory. The sorting-out its own source asks for ("Sort out
GetBandwidthAndSize") is sitting in the workspace next door: `TotalScreenSize`,
which the kernel maintains as the screen memory of whichever display driver is
current - the screen dynamic area under VIDC20, and whatever
`GraphicsV_FramestoreAddress` reported under a card. The kernel's own
`OS_CheckModeValid` already uses it for exactly this comparison.

So `rom_patch.c` redirects that load, in the two places the macro appears. It is
two words per site, in the slots already there, and it changes nothing for VIDC20
(where `TotalScreenSize` is the VRAM). It is applied only when the card is fitted,
and only when every part of the pattern is found: the offsets are read out of the
ROM rather than assumed, so an image this does not recognise is left alone and the
card simply keeps the VRAM-sized ceiling.

### Why the card draws the pointer

The pointer is the card's, not RISC OS's. That is not just an optimisation: this
emulator's mouse-following works by drawing the *hardware* pointer wherever the
host's mouse is, while telling the guest the same position through `OS_Mouse`. A
driver that leaves the pointer to RISC OS gets a software pointer plotted at the
position the kernel maintains from mouse movement - which mouse-following
suppresses - so the pointer sits still while the host's mouse moves.

So the driver claims `GraphicsV_UpdatePointer` and hands the card the shape's
physical address, its size and where to put it; the card's scan-out composites it,
at the host's pointer position when the front-end is placing it. The shape is
2bpp as GraphicsV defines, four pixels to a byte, colour 0 transparent and 1-3
from the pointer palette, and it stays where RISC OS built it - the card fetches
it, as real hardware would.

One detail of that format is easy to get wrong, and did go wrong here: the width
RISC OS reports is the width the shape was *defined* with, unpadded, but rows in
the buffer are always eight bytes apart. The kernel zero-fills each row out to a
multiple of eight when the shape is defined, and refuses any width above eight.
Stepping from row to row by the reported width therefore only looks right for a
shape that happens to be the full eight bytes wide, as the Wimp's arrow is. The
Hourglass declares six, so its rows each came out two bytes early and the shape
sheared into stripes. Use the padded stride to walk the rows, and the reported
width only to decide how much of each row to draw.

One GraphicsV feature is deliberately not claimed, so RISC OS does it in
software as it would on any card without it: rendering (rectangle copy and fill).

GraphicsV and `OS_ScreenMode 64` are RISC OS 5 features. On an older RISC OS the
driver declines to initialise and says so; the card is simply unused.

## Prior art

The approach follows the precedent set by
**[ViewFinder](https://www.zeridajh.org/hardware/viewfinder/)**, John Kortink's
graphics expansion card for the Acorn Risc PC, which showed that a card-hosted
framestore driven by its own display driver could take the machine well beyond
VIDC20's limits. No ViewFinder code, firmware or programming interface is used
here: the register interface is our own, and the driver is written from the
GraphicsV documentation in the RISC OS sources.

## Rebuilding the driver

The result is committed, because the ARM toolchain is not part of an ordinary
build. With the toolchain installed, `build.sh` rebuilds the driver and its
utility on every build; without it they are skipped and the committed image is
used:

```
./setup-build-env.sh --podules      # once, for the arm-linux-gnueabi tools
./build.sh                          # rebuilds the driver as part of the build
```

`tests/test_gfxrom.c` reads the committed image the way the module system does
(header offsets, title, help string, command table), so a stale or truncated
image is caught by `ctest` rather than by a guest that will not boot.
