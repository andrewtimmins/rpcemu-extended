# Accelerators

Some of what RISC OS draws, it draws a pixel at a time on the emulated ARM. Where
the work arrives as a SWI, and where this computer can produce the identical
result, it is done here instead and the guest's own code skipped.

One operation is accelerated today: plotting a sprite onto the screen.

| | |
| --- | --- |
| Setting | *Settings → Machine… → System*, "Let the host do drawing it can do identically" |
| Config key | `accelerators_enabled`, per machine, **on by default** |
| What it did | *Debug → Machine Inspector → Accelerators* |
| Source | `src/accelerators.c`, hooked from `opSWI()` in `src/arm_common.c` |

The hook is the same one mousehack, HostFS and the clipboard use, so it serves the
interpreter and the recompiler alike, and anything not handled returns to the
guest untouched.

## What is taken, and what is not

A plot is done here only when every one of these holds:

- reason **&34**, plot scaled, at **1:1** (which is what a desktop actually does)
- the source is a **32bpp sprite**, `&xBGR`, with no palette of its own
- **no colour translation table** (R7 = 0)
- **no transparency**: no mask plane, no alpha channel
- plain **overwrite** (the plot action, R5 bits 0-2, is zero)
- the screen is **32bpp**
- VDU output is going to the framebuffer being scanned out

Everything else is left to RISC OS, including every case above turned round. That
is deliberate: a plot drawn nearly right is worse than one drawn by the guest,
because it looks like a broken emulator and the cause is invisible.

Refusals are counted by reason, and the Accelerators tab lists them. That is how
the next case to support gets chosen rather than guessed at.

## What it is worth

Measured on a 1920 x 1080 desktop at 32bpp, over a minute of ordinary use
(dragging windows, Filer windows, scrolling), repeated across several sessions:

| | |
| --- | --- |
| Sprite plots taken | 13% to 16% of all plots |
| **Their share of the pixels actually drawn** | **94%** |
| Plots refused for a colour translation table | about 80% |

The two figures look contradictory and are not. The plots needing translation
tables are small icons, drawn whole; the ones taken are backdrops, images and
thumbnails, which are large. Counting plots says this is a small feature and
counting pixels says it is most of the work, and pixels are what the emulated ARM
pays for.

Note that both figures are **after clipping**. A window's redraw plots the whole
of a backdrop and clips it to the rectangle being repaired, so an unclipped count
overstates what the guest grinds through and what any accelerator can save.

## Knowing where output is going

A plot can be redirected into a sprite instead of the screen, and accelerating
that would write the display and leave the sprite untouched. The only way to tell
is the kernel's VDU driver workspace, which begins at **&1000**:

| Field | Offset | | Field | Offset |
| --- | --- | --- | --- | --- |
| `GWLCol`, `GWBRow`, `GWRCol`, `GWTRow` | +&50 … +&5C | | `XEigFactor`, `YEigFactor` | +&98, +&9C |
| `XWindLimit`, `YWindLimit`, `LineLength` | +&80, +&84, +&88 | | `Log2BPP` | +&A4 |
| `ScreenStart` | +&C0 | | `OrgX`, `OrgY` | +&F0, +&F4 |

Those are the kernel's own, from `Kernel/hdr/KernelWS`, which also derives the
&1000 base and asserts it is 64-byte aligned.

**Several fields have a second copy named `Display<field>`.** The plain field
describes wherever output currently goes; the `Display` copy describes the
display. They are equal exactly when output is *not* redirected, which is the one
circumstance that has to be detected, so anything that finds its offsets by
searching for known values can match the wrong set and be blind to the case it
exists for.

Every field is therefore cross-checked against the emulator's own state before a
plot is taken: pixels across, rows, bytes per row and depth must all agree with
the mode being scanned out, the graphics window must lie inside the screen, and
`ScreenStart` must translate into that framebuffer. Any disagreement means the
plot is not ours.

Guest memory is read through `mem_debug_read()` and `mem_debug_host_ptr()`, which
translate without faulting and without running the debugger's watchpoint hooks,
so a sprite pointer the guest has got wrong cannot abort the machine and
watchpoints stay a record of what the *guest* touched.

## A host write must mark the pages dirty

The display converts only the 4KB pages written since the last frame, and it
learns that from the guest's writes passing through the memory system. A write
from the host bypasses all of it, so every accelerated plot marks the rows it
touched, in the graphics card's record or VIDC20's `dirtybuffer` as appropriate.
Without that the plot is invisible until something else happens to dirty the same
pages, which looks like an emulator that randomly fails to draw.

## Coordinates

RISC OS counts y upwards from the bottom of the screen, gives positions in OS
units relative to a movable graphics origin, and treats both edges of a graphics
window as inclusive. A framebuffer counts rows downwards from the top. Sprite
pixel data is stored top row first.

Clipping never moves where the sprite starts: a plot clipped on its left takes its
pixels from part-way into each row, and one clipped at the top starts part-way
down the sprite. `accel_plot_rect()` and `accel_blit_row_plan()` are separated
from the copying for exactly this reason, and `tests/test_accel_plot.c` checks
them against results worked out by hand, including an end-to-end clipped copy
compared with an image written out pixel by pixel.

## Limits

- 16bpp sources are refused, though they are the next obvious case: roughly a
  thousand sprites a session, needing a 565-to-32bpp conversion per pixel.
- Transparency is refused. Fewer than twenty plots a session carried any, so the
  exact blend arithmetic is not worth the risk yet.
- Colour translation tables are refused, and probably always will be: they are
  most of the plots and few of the pixels.
- A screen in main memory (a machine with no VRAM) is counted but not written.
