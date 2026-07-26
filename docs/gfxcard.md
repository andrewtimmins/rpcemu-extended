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
and the machine gains an expansion card in the next free EASI slot. RISC OS keeps
using VIDC20 until you ask it to switch, so nothing about an existing machine
changes until you do.

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

Modes are still offered by the monitor definition in force, so the mode you want
has to be one your monitor type advertises. The card serves the same monitor
definition the machine booted with, so switching to it does not change what is on
offer. *Settings → Follow Host Display Size*
with `MonitorType EDID` is the easy route: the emulator synthesises a monitor
definition from the host's real display, up to 2560 x 1440.

If a mode is too large for the card, RISC OS says "Mode not available" rather
than trying it, because the driver vets each mode against the card's memory
before the kernel commits to it.

`*GfxCardOff` returns to VIDC20, and so does killing the driver (`*RMKill
RPCEmuGfx`), which hands the display back before it goes.

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
| WritePaletteEntry / Entries / Read (10-12) | The 256-entry palette |
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

Two GraphicsV features are deliberately not claimed, so RISC OS does them in
software as it would on any card without them: the hardware pointer, and
rendering (rectangle copy and fill).

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

The driver is assembled by hand and the result committed, because the ARM
toolchain is not part of an ordinary build:

```
./setup-build-env.sh --podules      # once, for the arm-linux-gnueabi tools
./build.sh --podules
```

`tests/test_gfxrom.c` reads the committed image the way the module system does
(header offsets, title, help string, command table), so a stale or truncated
image is caught by `ctest` rather than by a guest that will not boot.
