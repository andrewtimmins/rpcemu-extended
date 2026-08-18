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
- the source is a **32bpp `&xBGR` sprite** (type 6) or a **16bpp one** in either
  5:6:5 (type 10) or 1:5:5:5 (type 5), with no palette of its own
- **no colour translation table** (R7 = 0)
- **no transparency**: no mask plane, no alpha channel
- plain **overwrite** (the plot action, R5 bits 0-2, is zero)
- the sprite's layout can be turned into the **screen's** layout exactly (see
  below); a 16bpp screen is written as readily as a 32bpp one
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

## Layouts, and which pairs are allowed

Both the sprite and the screen have a pixel layout, and a plot is taken only
when this can get from one to the other exactly.

| | to 32bpp `&xBGR` | to 16bpp 5:6:5 | to 16bpp 1:5:5:5 | to 8bpp |
| --- | --- | --- | --- | --- |
| from 32bpp `&xBGR` | copy | yes | yes | no |
| from 16bpp 5:6:5 | yes | copy | yes | no |
| from 16bpp 1:5:5:5 | yes | yes | copy | no |
| from 16bpp 4:4:4:4 | yes | yes | yes | no |
| from 8bpp | no | no | no | copy |

The same layout at both ends is a plain copy, exact by construction, and on a
16bpp desktop it is the common case: a machine at 1280x720 has no room for
32bpp in a Risc PC's video memory, so both the screen and the sprites it keeps
tend to be 16bpp.

**4:4:4:4 is a source only.** As a screen it would have a fourth channel to
fill and nothing here knows what RISC OS would put in it.

**8bpp may only be copied to itself.** A pixel is an index into a palette and
means nothing on its own, so it cannot become a colour, nor a colour become an
index without searching the palette for one. Copied index for index onto a
screen of the same depth, with no translation table asked for, it needs no
palette at all and RISC OS does exactly the same. That is worth having rather
than dismissing: on a machine whose desktop is 8bpp - which 1920x1080 must be
on 2MB of VRAM - it is nearly all of the drawing. 4bpp and below are left out
because a pixel is not a whole byte, so a clipped row would begin part-way into
one.

### One rule serves every pair

RISC OS **widens** a channel by replicating its top bits into the bits it
gains, and **narrows** by keeping the top bits and discarding the rest - a
plain `UBFX` in the generated code, with no rounding. So every conversion here
goes through eight bits a channel, and that is exact rather than approximate:

- widening `a` to `b`: replicating to 8 puts the channel at the top with copies
  of itself below, so the top `b` bits are the channel followed by its own top
  `b - a` bits, which is exactly replicate `a` to `b`;
- narrowing `a` to `b`: the top `b` bits of the replicated value are the top `b`
  bits of the channel, which is exactly the truncation.

One path, no table of special cases to get wrong. The same-layout case does not
come through it: that is a straight copy, which keeps the spare top bit that
RISC OS also leaves alone.

### Why reducing a depth is safe

Narrowing would be unreproducible if RISC OS dithered, because a dithered result
depends on where the pixel is. It does not, unless asked: `SprOp` shifts R5 right
by four and reads its flags out of the result, so **bit 6 of R5 turns dithering
on** and SpriteExtend initialises `dither_truecolour` to zero otherwise. A plot
that sets it is refused on the action, along with translucency (R5 bits 8-15),
colour mapping (bit 7) and a wide translation table (bit 5).

Those upper bits matter beyond dithering: a plot asking for translucency and
drawn opaque here would be plainly wrong and would look like a fault in the
application.

### Which 16bpp the screen is

`ModeFlag_64k`, bit 7 of the mode's flags, is the only thing separating 5:6:5
from 1:5:5:5, and it is **the same bit as `ModeFlag_FullPalette`**, which means
something else at every other depth. Under 4096 colours at that depth is
4:4:4:4, whose fourth channel is alpha, and that is not written here. The mode
variables come from the VDU workspace: `NColour` at +&8C and `ModeFlags` at
+&94, beside the fields already listed above.

## Where the arithmetic comes from

The tempting `v << 3` for a five-bit channel is wrong at the top rather than in
the middle: it can never produce white, because &1F would become &F8, so a
photograph drawn that way is imperceptibly dark everywhere and plainly wrong
where it should be pure. The replication rule above is what SpriteExtend emits:
`get_expansion_mask()` in
`Video/Render/SprExtend/c/asmcore` builds a mask of the bits each channel gains,
and the generated code is

```
AND  r_temp1, r_pixel, #expansion_mask   ; the channels' top bits
ORR  r_pixel, r_pixel, r_temp1, LSR #n   ; replicated into the new low bits
```

with a hand-written second step for 5:6:5's green, whose six bits cannot share
the five-bit shift the other two channels use. Channel positions come from the
table in `Sources/PutScaled`: red is in the low bits of both formats.

**The top byte comes out zero.** A screen mode carries no alpha channel, so
SpriteExtend builds the output word from the source's three channels and nothing
else; neither 5:6:5's spare bit nor 1:5:5:5's T bit reaches it. Writing &FF there
instead would look identical and differ from RISC OS in memory.

Note that a 5:6:5 sprite cannot carry alpha whatever its mode word says, and the
kernel clears that flag itself for this depth, so the flag is ignored here too.

## A sprite's width is in words, not pixels

The header's width field counts **words less one**, so `width + 1` is the pixel
count at 32bpp and at no other depth. The general form is the kernel's own, from
`readspritevars` in `SprOp`: the bits from the left-hand wastage to the
right-hand one inclusive, over the bits per pixel.

Two things follow. A 16bpp sprite of odd width pads its rows to a whole word, so
its stride and its width times its depth differ by two bytes on every row - a
plot that used the product would lean further left the further down it went. And
the **counts** in the tab were previously taken with a word to a pixel for every
sprite, so anything shallower than 32bpp was measured too narrow; the share of
the drawn pixels a host-side plot can take was therefore reported against a
denominator that was too small.

## Limits

- Transparency is refused. Fewer than twenty plots a session carried any, so the
  exact blend arithmetic is not worth the risk yet.
- Colour translation tables are refused, and probably always will be: they are
  most of the plots and few of the pixels.
- A screen in main memory (a machine with no VRAM) is counted but not written.
- An `&xRGB` screen is refused; every mode seen so far is `&xBGR`.
- 24bpp, CMYK, JPEG and YCbCr sources have no fixed width in pixels here, so
  they are counted but their sizes are not.
- Colour translation tables and old-format sprites are refused, and the counts
  say to leave them that way: on an 8bpp desktop they were 154 and 27 plots
  respectively, and between them under 5% of the pixels drawn.

The tab now breaks the type refusals down and gives the pixels behind each, so
the next format worth reading is chosen the same way this one was.
