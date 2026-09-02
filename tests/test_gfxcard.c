/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * test_gfxcard.c - the graphics card's registers and frame validation
 *
 * The card takes its geometry from registers the guest writes, so the checks in
 * gfxcard_frame() are the boundary between a driver bug and the emulator reading
 * outside its own framestore. Those are what this exercises, along with the
 * register file itself.
 *
 * The card is driven through its own expansion card handlers, the same entry
 * points the guest reaches, rather than by poking its internals. The unit is
 * included directly and the backplane stubbed: linking podules.c would drag in
 * every bundled expansion card and the plugin loader with them.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "edid.h"
#include "gfxcard.h"
#include "podules.h"
#include "savestate.h"

/* ---- the backplane, and the little of the emulator the card touches ---- */

static podule test_podule;
static int irq_raised;

podule *
addpodule(void (*writel)(podule *, PoduleIoType, uint32_t, uint32_t),
          void (*writew)(podule *, PoduleIoType, uint32_t, uint16_t),
          void (*writeb)(podule *, PoduleIoType, uint32_t, uint8_t),
          uint32_t (*readl)(podule *, PoduleIoType, uint32_t),
          uint16_t (*readw)(podule *, PoduleIoType, uint32_t),
          uint8_t (*readb)(podule *, PoduleIoType, uint32_t),
          int (*timercallback)(podule *),
          void (*reset)(podule *))
{
	test_podule.writel = writel;
	test_podule.writew = writew;
	test_podule.writeb = writeb;
	test_podule.readl = readl;
	test_podule.readw = readw;
	test_podule.readb = readb;
	test_podule.timercallback = timercallback;
	test_podule.reset = reset;
	return &test_podule;
}

int  podule_slot_number(const podule *p) { (void) p; return 3; }
void podule_irq_raise(podule *p)  { (void) p; irq_raised = 1; test_podule.irq = 1; }
void podule_irq_lower(podule *p)  { (void) p; test_podule.irq = 0; }

Config config;

void rpclog(const char *format, ...) { (void) format; }
void fatal(const char *format, ...) { (void) format; printf("fatal() called\n"); exit(2); }
const char *rpcemu_get_resourcedir(void) { return "/nonexistent/"; }
/* The card's driver comes out of the data directory now, extracted there from
   the copy embedded in the binary - see support_files.h. Both of these point at
   nothing, so the card comes up without a driver, which is what these tests
   want; support_root_for() is stubbed rather than linked because the real one
   brings the whole embedded payload with it. */
const char *rpcemu_get_datadir(void) { return "/nonexistent/"; }
const char *support_root_for(const char *subdir) { (void) subdir; return "/nonexistent/"; }

/* The serialisation helpers, so the card's suspend and resume can be exercised
   without linking savestate.c - which wants the whole machine, every other
   device's chunk with it. These are deliberately not the real encoder: they only
   have to be each other's inverse, which is all a round trip needs. What is
   under test here is the card's own field order and its clamping, and both
   halves of the card go through one pair of helpers here exactly as they go
   through one pair in the emulator. The byte format itself is savestate.c's, and
   every other chunk exercises it. */
int savestate_error;

void savestate_write_u32(FILE *f, uint32_t v) { fwrite(&v, sizeof v, 1, f); }
void savestate_write_i32(FILE *f, int32_t v)  { fwrite(&v, sizeof v, 1, f); }
void savestate_write_rle(FILE *f, const void *data, size_t len) { fwrite(data, 1, len, f); }

uint32_t
savestate_read_u32(FILE *f)
{
	uint32_t v = 0;

	if (fread(&v, sizeof v, 1, f) != 1) {
		savestate_error = 1;
	}
	return v;
}

int32_t
savestate_read_i32(FILE *f)
{
	int32_t v = 0;

	if (fread(&v, sizeof v, 1, f) != 1) {
		savestate_error = 1;
	}
	return v;
}

void
savestate_read_rle(FILE *f, void *data, size_t len)
{
	if (fread(data, 1, len, f) != len) {
		savestate_error = 1;
	}
}

/*
 * Which snapshot version the card's loader thinks it is reading.
 *
 * Settable, because the card's chunk changed shape at version 9 - it gained the
 * 16bpp pixel format - and both sides of that need exercising: a current
 * snapshot round trips the field, and an older one must NOT read it, or every
 * value after it in the chunk is off by a word. Defaults to current, so a test
 * that does not care gets the modern layout.
 */
static uint32_t stub_snapshot_version = SNAPSHOT_VERSION_GFX_PIXFMT;

uint32_t savestate_version_being_loaded(void) { return stub_snapshot_version; }

#include "gfxcard.c"

static int failures;

static void
check(int cond, const char *what)
{
	printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

/* Registers are one word apart in the card's EASI space. */
#define REG(n)		GFXCARD_REG_ADDR(n)

static void
wr(int reg, uint32_t val)
{
	gfxcard_writel(&test_podule, PODULE_IO_TYPE_EASI, REG(reg), val);
}

static uint32_t
rd(int reg)
{
	return gfxcard_readl(&test_podule, PODULE_IO_TYPE_EASI, REG(reg));
}

static void
set_mode(unsigned w, unsigned h, unsigned bpp, unsigned stride, uint32_t start)
{
	wr(GFXCARD_REG_WIDTH, w);
	wr(GFXCARD_REG_HEIGHT, h);
	wr(GFXCARD_REG_BPP, bpp);
	wr(GFXCARD_REG_STRIDE, stride);
	wr(GFXCARD_REG_START, start);
}

int
main(void)
{
	GfxCardFrame frame;

	/* The card is optional, and only appears when configured. */
	config.gfxcard_enabled = 0;
	gfxcard_init();
	check(!gfxcard_active() && gfxcard_fb == NULL,
	      "absent until the configuration asks for it");

	config.gfxcard_enabled = 1;
	gfxcard_init();
	check(gfxcard_fb != NULL, "framestore allocated once enabled");
	check(gfxcard_fb_phys >= 0x08000000u && gfxcard_fb_phys < 0x10000000u,
	      "framestore sits in expansion card address space");
	check((gfxcard_fb_phys & 0xffffffu) == GFXCARD_FB_OFFSET,
	      "framestore starts above the card ROM");

	printf("\nidentity and capabilities are readable\n");
	check(rd(GFXCARD_REG_ID) == GFXCARD_ID, "identity register reads \"RPGx\"");
	check(rd(GFXCARD_REG_VERSION) == GFXCARD_VERSION, "interface version");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_32BPP) != 0, "32bpp offered");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_16BPP) != 0, "16bpp offered");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_8BPP) != 0, "8bpp offered");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_16BPP555) != 0,
	      "and 16bpp in 555 as well as 565");
	check(rd(GFXCARD_REG_MAX_WIDTH) == GFXCARD_MAX_WIDTH &&
	      rd(GFXCARD_REG_MAX_HEIGHT) == GFXCARD_MAX_HEIGHT,
	      "largest accepted mode is readable");
	check(rd(GFXCARD_REG_FB_PHYS) == gfxcard_fb_phys &&
	      rd(GFXCARD_REG_FB_SIZE) == GFXCARD_FB_SIZE,
	      "framestore address and size readable");

	/*
	 * The 16bpp pixel format.
	 *
	 * RISC OS has two 16bpp modes and they are not interchangeable: 565 is what
	 * it calls 64 thousand colours and 555 is 32 thousand. The card scanned out
	 * 565 and nothing else, which left a 32 thousand colour desktop unavailable
	 * on the card while being the only 16bpp mode VIDC20 offers - issue #220,
	 * where one HostFS image shared between RISC OS 4 and 5 has no depth in
	 * common between the two machines.
	 *
	 * Getting this register wrong does not raise an error: it shows as a
	 * picture whose greens and blues are wrong, which is why it is worth
	 * pinning rather than eyeballing.
	 */
	printf("\nthe 16bpp pixel format\n");
	check(rd(GFXCARD_REG_PIXFMT) == GFXCARD_PIXFMT_565,
	      "565 after a reset, which is what the card did before it could be asked");
	wr(GFXCARD_REG_PIXFMT, GFXCARD_PIXFMT_555);
	check(rd(GFXCARD_REG_PIXFMT) == GFXCARD_PIXFMT_555, "555 can be selected");
	wr(GFXCARD_REG_PIXFMT, 0x5a5au);
	check(rd(GFXCARD_REG_PIXFMT) == GFXCARD_PIXFMT_565,
	      "a value the card does not know reads back as 565, not as itself");
	wr(GFXCARD_REG_PIXFMT, GFXCARD_PIXFMT_555);
	set_mode(640, 480, 16, 640 * 2, 0);
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);
	check(gfxcard_frame(&frame) && frame.bpp == 16 &&
	      frame.pixfmt == GFXCARD_PIXFMT_555,
	      "and the frame carries it to whatever converts the pixels");
	wr(GFXCARD_REG_PIXFMT, GFXCARD_PIXFMT_565);
	check(gfxcard_frame(&frame) && frame.pixfmt == GFXCARD_PIXFMT_565,
	      "and follows it when it changes");
	wr(GFXCARD_REG_CTRL, 0);

	printf("\nnothing is displayed until the guest enables the card\n");
	set_mode(1920, 1080, 32, 1920 * 4, 0);
	check(!gfxcard_active(), "geometry alone does not start it");
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);
	check(gfxcard_active(), "enabled once the control bit is set");
	check(gfxcard_frame(&frame) && frame.width == 1920 && frame.height == 1080 &&
	      frame.bpp == 32 && frame.stride == 1920 * 4,
	      "frame reports the geometry it was given");

	printf("\na mode that would read outside the framestore is refused\n");
	/* 2560x1440 at 32bpp is 14.06MB and fits; the card is sized for it. */
	set_mode(2560, 1440, 32, 2560 * 4, 0);
	check(gfxcard_frame(&frame), "2560x1440 at 32bpp fits");

	set_mode(2560, 1440, 32, 2560 * 4, GFXCARD_FB_SIZE - 4096);
	check(!gfxcard_frame(&frame), "same mode refused near the end of the store");

	set_mode(4096, 2160, 32, 4096 * 4, 0);
	check(!gfxcard_frame(&frame), "a mode larger than the card allows is refused");

	set_mode(1920, 1080, 32, 0x100000u, 0);
	check(!gfxcard_frame(&frame), "an absurd stride is refused");

	set_mode(1920, 1080, 32, 1920 * 4, GFXCARD_FB_SIZE);
	check(!gfxcard_frame(&frame), "a start beyond the store is refused");

	set_mode(1920, 1080, 24, 1920 * 3, 0);
	check(!gfxcard_frame(&frame), "a depth the card does not claim is refused");

	/* 16bpp costs half of 32bpp, which is the point of offering it: a mode
	   that will not fit at 32 may well fit at 16. */
	set_mode(2560, 1440, 16, 2560 * 2, 0);
	check(gfxcard_frame(&frame) && frame.bpp == 16 &&
	      frame.stride == 2560 * 2,
	      "2560x1440 at 16bpp fits, and is reported as 16bpp");

	set_mode(1920, 0, 32, 1920 * 4, 0);
	check(!gfxcard_frame(&frame), "no lines is not a display");
	set_mode(0, 1080, 32, 0, 0);
	check(!gfxcard_frame(&frame), "no pixels either");

	set_mode(1920, 1080, 32, 1920 * 2, 0);
	check(!gfxcard_frame(&frame), "a stride narrower than a row is refused");

	/* A stride chosen so that stride * (height - 1) is exactly 2^32, which at
	   32 bits looks like a mode needing almost no memory at all. */
	set_mode(64, 257, 32, 0x01000000u, 0);
	check(!gfxcard_frame(&frame), "a stride that overflows the arithmetic is refused");

	printf("\nhardware scroll moves the window, within bounds\n");
	set_mode(640, 480, 8, 640, 0);
	check(gfxcard_frame(&frame) && frame.fb == gfxcard_fb,
	      "start of zero displays from the base");
	set_mode(640, 480, 8, 640, 64000);
	check(gfxcard_frame(&frame) && frame.fb == gfxcard_fb + 64000,
	      "a display start offsets the window");

	printf("\nblanking is reported rather than changing the geometry\n");
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE | GFXCARD_CTRL_BLANK);
	check(gfxcard_frame(&frame) && frame.blanked, "blank reported to the video code");
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);
	check(gfxcard_frame(&frame) && !frame.blanked, "and cleared again");

	printf("\nthe palette index steps on after each entry\n");
	wr(GFXCARD_REG_PAL_INDEX, 7);
	wr(GFXCARD_REG_PAL_ENTRY, 0x56781234u);
	wr(GFXCARD_REG_PAL_ENTRY, 0x11223344u);
	check(gfxcard_palette()[7] == 0x56781234u &&
	      gfxcard_palette()[8] == 0x11223344u,
	      "a run of colours needs the index written once");
	check(rd(GFXCARD_REG_PAL_INDEX) == 9, "and the index has moved on");
	wr(GFXCARD_REG_PAL_INDEX, 255);
	wr(GFXCARD_REG_PAL_ENTRY, 0xdeadbeefu);
	check(gfxcard_palette()[255] == 0xdeadbeefu && rd(GFXCARD_REG_PAL_INDEX) == 0,
	      "the index wraps rather than running off the end");

	printf("\nvsync is latched, and cleared by writing the bit back\n");
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);
	wr(GFXCARD_REG_STATUS, GFXCARD_STATUS_VSYNC);
	check((rd(GFXCARD_REG_STATUS) & GFXCARD_STATUS_VSYNC) == 0, "starts clear");
	gfxcard_vsync();
	check((rd(GFXCARD_REG_STATUS) & GFXCARD_STATUS_VSYNC) != 0, "latched on a frame");
	wr(GFXCARD_REG_STATUS, GFXCARD_STATUS_VSYNC);
	check((rd(GFXCARD_REG_STATUS) & GFXCARD_STATUS_VSYNC) == 0,
	      "cleared by writing the bit back");

	printf("\nthe monitor's EDID is served over the card\n");
	check(rd(GFXCARD_REG_EDID_SIZE) == 0,
	      "nothing to serve until the emulator publishes a block");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_EDID) == 0,
	      "and the card does not claim it can");
	{
		/* A block built the way the emulator builds the one it installs in
		   the ROM, so the card and the ROM answer for the same monitor. */
		uint8_t base[EDID_BLOCK_SIZE], block[EDID_BLOCK_SIZE];
		unsigned i;

		memset(base, 0, sizeof(base));
		memcpy(base, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
		base[18] = 1;			/* EDID version 1 */
		edid_build_from_base(block, base, 2560, 1440, 60);
		check(edid_block_is_valid(block), "the block built for the test is sound");
		edid_publish(block);

		check(rd(GFXCARD_REG_EDID_SIZE) == EDID_BLOCK_SIZE,
		      "a published block is offered whole");
		check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_EDID) != 0,
		      "and the card now claims it");

		wr(GFXCARD_REG_EDID_INDEX, 0);
		for (i = 0; i < EDID_BLOCK_SIZE; i++) {
			if (rd(GFXCARD_REG_EDID_DATA) != block[i]) {
				break;
			}
		}
		check(i == EDID_BLOCK_SIZE, "every byte reads back, in order");
		check(rd(GFXCARD_REG_EDID_INDEX) == EDID_BLOCK_SIZE,
		      "the index stops at the end rather than wrapping");
		check(rd(GFXCARD_REG_EDID_DATA) == 0,
		      "reading past the end gives zero, not the start again");

		wr(GFXCARD_REG_EDID_INDEX, 8);
		check(rd(GFXCARD_REG_EDID_DATA) == block[8],
		      "the index can be set to read part of the block");
		wr(GFXCARD_REG_EDID_INDEX, 0x1000);
		check(rd(GFXCARD_REG_EDID_INDEX) == EDID_BLOCK_SIZE,
		      "an index beyond the block is clamped");
	}

	printf("\nthe pointer is the card's to draw\n");
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_HW_POINTER) != 0,
	      "the card claims it draws the pointer");
	set_mode(640, 480, 8, 640, 0);
	wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);
	check(gfxcard_frame(&frame) && !frame.ptr_visible,
	      "nothing drawn until there is a shape to draw");

	wr(GFXCARD_REG_PTR_WIDTH, 8);		/* 32 pixels, as RISC OS pads to */
	wr(GFXCARD_REG_PTR_HEIGHT, 12);
	wr(GFXCARD_REG_PTR_PHYS, 0x18000000u);
	wr(GFXCARD_REG_PTR_X, 100);
	wr(GFXCARD_REG_PTR_Y, 50);
	check(gfxcard_frame(&frame) && !frame.ptr_visible,
	      "nor until it is switched on");
	wr(GFXCARD_REG_PTR_CTRL, GFXCARD_PTR_CTRL_SHOW);
	check(gfxcard_frame(&frame) && frame.ptr_visible &&
	      frame.ptr_x == 100 && frame.ptr_y == 50 &&
	      frame.ptr_width == 8 && frame.ptr_height == 12 &&
	      frame.ptr_phys == 0x18000000u,
	      "shape, place and address reported with the frame");

	wr(GFXCARD_REG_PTR_WIDTH, 0x1000);
	check(rd(GFXCARD_REG_PTR_WIDTH) == GFXCARD_PTR_MAX_WIDTH,
	      "an absurd shape width is clamped, not trusted");
	wr(GFXCARD_REG_PTR_HEIGHT, 0x1000);
	check(rd(GFXCARD_REG_PTR_HEIGHT) == GFXCARD_PTR_MAX_HEIGHT,
	      "and so is the height");

	printf("\nthe pointer has its own three colours\n");
	check(gfxcard_frame(&frame) && frame.ptr_palette[0] == 0,
	      "colour 0 is transparent");
	wr(GFXCARD_REG_PTR_PAL_IDX, 1);
	wr(GFXCARD_REG_PTR_PAL, 0x11223300u);
	wr(GFXCARD_REG_PTR_PAL, 0x44556600u);
	wr(GFXCARD_REG_PTR_PAL, 0x77889900u);
	check(gfxcard_frame(&frame) && frame.ptr_palette[1] == 0x11223300u &&
	      frame.ptr_palette[2] == 0x44556600u && frame.ptr_palette[3] == 0x77889900u,
	      "three colours written as a run from index 1");
	wr(GFXCARD_REG_PTR_PAL_IDX, 0);
	wr(GFXCARD_REG_PTR_PAL, 0xdeadbeefu);
	check(gfxcard_frame(&frame) && frame.ptr_palette[0] == 0,
	      "and colour 0 stays transparent however it is written");

	wr(GFXCARD_REG_PTR_CTRL, 0);
	check(gfxcard_frame(&frame) && !frame.ptr_visible, "switched off again");

	printf("\nframes displayed are counted, for diagnostics\n");
	{
		const uint32_t before = rd(GFXCARD_REG_FRAMES);

		gfxcard_vsync();
		gfxcard_vsync();
		check(rd(GFXCARD_REG_FRAMES) == before + 2, "two frames counted");
	}

	printf("\nthe framestore is reachable through the card, byte and word\n");
	gfxcard_writeb(&test_podule, PODULE_IO_TYPE_EASI, GFXCARD_FB_OFFSET + 5, 0xa5);
	check(gfxcard_fb[5] == 0xa5, "a byte write lands in the framestore");
	check(gfxcard_readb(&test_podule, PODULE_IO_TYPE_EASI, GFXCARD_FB_OFFSET + 5) == 0xa5,
	      "and reads back");
	gfxcard_writew(&test_podule, PODULE_IO_TYPE_EASI, GFXCARD_FB_OFFSET + 8, 0x1357);
	check(gfxcard_readw(&test_podule, PODULE_IO_TYPE_EASI, GFXCARD_FB_OFFSET + 8) == 0x1357,
	      "a half-word round-trips");

	printf("\nthe card ROM identifies the card at offset zero\n");
	/* Read a byte per word, as the other cards here are read. */
	check(gfxcard_readb(&test_podule, PODULE_IO_TYPE_EASI, 0) == 0,
	      "identity byte 0: Acorn conformant, extended identity");
	check(gfxcard_readb(&test_podule, PODULE_IO_TYPE_EASI, 4) == 3,
	      "identity byte 1: chunk directories present");

	printf("\nrectangles are copied and filled in the framestore\n");
	{
		/* A small 8bpp mode, so a pixel is a byte and the arithmetic below is
		   readable. The card is told rows down from the top; turning RISC OS's
		   bottom-up coordinates over is the driver's job, not the card's. */
		const unsigned w = 64, h = 16, stride = 64;
		unsigned x, y;
		int ok;

		set_mode(w, h, 8, stride, 0);
		wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);

		/* Every pixel its own value, so a copy that slips by a row or a column
		   cannot go unnoticed. */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				gfxcard_fb[y * stride + x] = (uint8_t) (y * 16u + (x & 15u));
			}
		}

		/* Copy an 8x4 block four columns to the right and two rows down. */
		wr(GFXCARD_REG_RENDER_SRC_X, 0);
		wr(GFXCARD_REG_RENDER_SRC_Y, 0);
		wr(GFXCARD_REG_RENDER_X, 4);
		wr(GFXCARD_REG_RENDER_Y, 2);
		wr(GFXCARD_REG_RENDER_W, 8);
		wr(GFXCARD_REG_RENDER_H, 4);
		wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_COPY);

		ok = 1;
		for (y = 0; y < 4; y++) {
			for (x = 0; x < 8; x++) {
				if (gfxcard_fb[(y + 2) * stride + (x + 4)] !=
				    (uint8_t) (y * 16u + (x & 15u))) {
					ok = 0;
				}
			}
		}
		check(ok, "a rectangle copies to where it was told");
		check(gfxcard_fb[0] == 0 && gfxcard_fb[stride * 8] == (uint8_t) (8 * 16),
		      "and nothing outside it is disturbed");

		/* Overlapping, which is what scrolling a window is: the rows have to be
		   walked away from the overlap or the copy eats its own source. */
		for (y = 0; y < h; y++) {
			memset(gfxcard_fb + y * stride, (int) y, w);
		}
		wr(GFXCARD_REG_RENDER_SRC_X, 0);
		wr(GFXCARD_REG_RENDER_SRC_Y, 0);
		wr(GFXCARD_REG_RENDER_X, 0);
		wr(GFXCARD_REG_RENDER_Y, 1);	/* down one row, rectangles overlap */
		wr(GFXCARD_REG_RENDER_W, w);
		wr(GFXCARD_REG_RENDER_H, h - 1);
		wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_COPY);

		ok = 1;
		for (y = 1; y < h; y++) {
			if (gfxcard_fb[y * stride] != (uint8_t) (y - 1)) {
				ok = 0;
			}
		}
		check(ok, "an overlapping copy does not eat its own source");

		/* Fill: (destination OR ora) EOR eor, which with ora all ones and eor
		   the complement of a colour is how RISC OS clears the screen. */
		{
			const uint8_t colour = 0x5a;
			unsigned i;

			memset(gfxcard_fb, 0x11, stride * h);
			wr(GFXCARD_REG_RENDER_PAT_IDX, 0);
			for (i = 0; i < GFXCARD_PATTERN_WORDS; i += 2) {
				wr(GFXCARD_REG_RENDER_PAT, 0xffffffffu);	/* ora */
				wr(GFXCARD_REG_RENDER_PAT, ~(0x5a5a5a5au));	/* eor */
			}
			wr(GFXCARD_REG_RENDER_X, 8);
			wr(GFXCARD_REG_RENDER_Y, 3);
			wr(GFXCARD_REG_RENDER_W, 16);
			wr(GFXCARD_REG_RENDER_H, 5);
			wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_FILL);

			ok = 1;
			for (y = 3; y < 8; y++) {
				for (x = 8; x < 24; x++) {
					if (gfxcard_fb[y * stride + x] != colour) {
						ok = 0;
					}
				}
			}
			check(ok, "a fill paints the colour the pattern describes");
			check(gfxcard_fb[3 * stride + 7] == 0x11 &&
			      gfxcard_fb[3 * stride + 24] == 0x11 &&
			      gfxcard_fb[2 * stride + 8] == 0x11 &&
			      gfxcard_fb[8 * stride + 8] == 0x11,
			      "and stops exactly at its edges");
		}

		printf("\na rectangle that would leave the display is refused\n");
		memset(gfxcard_fb, 0x22, stride * h);
		wr(GFXCARD_REG_RENDER_X, w - 4);
		wr(GFXCARD_REG_RENDER_Y, 0);
		wr(GFXCARD_REG_RENDER_W, 8);		/* runs off the right */
		wr(GFXCARD_REG_RENDER_H, 1);
		wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_FILL);
		check(gfxcard_fb[w - 4] == 0x22, "one wider than the display does nothing");

		wr(GFXCARD_REG_RENDER_X, 0);
		wr(GFXCARD_REG_RENDER_Y, h - 1);
		wr(GFXCARD_REG_RENDER_W, 4);
		wr(GFXCARD_REG_RENDER_H, 4);		/* runs off the bottom */
		wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_FILL);
		check(gfxcard_fb[(h - 1) * stride] == 0x22,
		      "and one taller than the display does nothing");

		wr(GFXCARD_REG_RENDER_X, 0);
		wr(GFXCARD_REG_RENDER_Y, 0);
		wr(GFXCARD_REG_RENDER_W, 4);
		wr(GFXCARD_REG_RENDER_H, 4);
		wr(GFXCARD_REG_RENDER_SRC_X, w - 2);	/* source runs off the right */
		wr(GFXCARD_REG_RENDER_SRC_Y, 0);
		wr(GFXCARD_REG_RENDER_OP, GFXCARD_RENDER_COPY);
		check(gfxcard_fb[0] == 0x22, "nor does a copy from outside it");

		wr(GFXCARD_REG_CTRL, 0);
	}

	printf("\nthe card's state survives suspend and resume\n");
	{
		/* A card that is only half restored is worse than one that is not
		   restored at all: the guest carries on driving a display whose
		   registers no longer describe it. So this sets a distinctive state
		   through the register interface, saves it, scrubs the card, checks the
		   scrub really took (or the restore below would prove nothing), and
		   then restores. */
		static const uint32_t marker_off[] = { 0, 12345, GFXCARD_FB_SIZE - 1 };
		FILE *f = tmpfile();
		uint32_t frames_at_save;
		unsigned i;
		int ok;

		set_mode(1280, 1024, 8, 1280, 4096);
		wr(GFXCARD_REG_PAL_INDEX, 0);
		for (i = 0; i < 256; i++) {
			wr(GFXCARD_REG_PAL_ENTRY, 0x01000000u * i + 0x0a0b0c00u);
		}
		wr(GFXCARD_REG_PTR_WIDTH, 8);
		wr(GFXCARD_REG_PTR_HEIGHT, 12);
		wr(GFXCARD_REG_PTR_PHYS, 0x18004000u);
		wr(GFXCARD_REG_PTR_X, 321);
		wr(GFXCARD_REG_PTR_Y, 654);
		wr(GFXCARD_REG_PTR_CTRL, GFXCARD_PTR_CTRL_SHOW);
		wr(GFXCARD_REG_PTR_PAL_IDX, 1);
		wr(GFXCARD_REG_PTR_PAL, 0xcafe0100u);
		wr(GFXCARD_REG_PTR_PAL, 0xcafe0200u);
		wr(GFXCARD_REG_PTR_PAL, 0xcafe0300u);
		wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE | GFXCARD_CTRL_VSYNC_IRQ);
		for (i = 0; i < 3; i++) {
			gfxcard_fb[marker_off[i]] = (uint8_t) (0x40 + i);
		}
		gfxcard_vsync();	/* counts a frame, sets vsync, raises the interrupt */
		frames_at_save = rd(GFXCARD_REG_FRAMES);

		check(f != NULL, "a scratch file for the snapshot");
		gfxcard_savestate(f);
		check(!savestate_error && ftell(f) > 0, "the card writes a chunk");

		/* Scrub it: a reset clears the registers, and the framestore and the
		   palette are wiped by hand because a reset deliberately leaves both
		   alone (a real card's memory survives one). */
		gfxcard_podule_reset(&test_podule);
		podule_irq_lower(&test_podule);
		memset(gfxcard_fb, 0, GFXCARD_FB_SIZE);
		memset(gfx.palette, 0, sizeof gfx.palette);
		check(!gfxcard_active() && gfxcard_fb[12345] == 0 &&
		      gfx.palette[7] == 0 && test_podule.irq == 0,
		      "scrubbed, so the restore below has something to prove");

		rewind(f);
		gfxcard_loadstate(f);
		check(!savestate_error, "and reads it back without error");

		check(gfxcard_active(), "the card is displaying again");
		ok = gfxcard_frame(&frame) &&
		     frame.width == 1280 && frame.height == 1024 &&
		     frame.bpp == 8 && frame.stride == 1280 &&
		     frame.fb == gfxcard_fb + 4096;
		check(ok, "mode, depth, stride and display start restored");

		ok = 1;
		for (i = 0; i < 256; i++) {
			if (gfxcard_palette()[i] != 0x01000000u * i + 0x0a0b0c00u) {
				ok = 0;
			}
		}
		check(ok, "all 256 palette entries restored");

		ok = gfxcard_frame(&frame) && frame.ptr_visible &&
		     frame.ptr_x == 321 && frame.ptr_y == 654 &&
		     frame.ptr_width == 8 && frame.ptr_height == 12 &&
		     frame.ptr_phys == 0x18004000u &&
		     frame.ptr_palette[0] == 0 &&
		     frame.ptr_palette[1] == 0xcafe0100u &&
		     frame.ptr_palette[2] == 0xcafe0200u &&
		     frame.ptr_palette[3] == 0xcafe0300u;
		check(ok, "the pointer, its place and its colours restored");

		ok = 1;
		for (i = 0; i < 3; i++) {
			if (gfxcard_fb[marker_off[i]] != (uint8_t) (0x40 + i)) {
				ok = 0;
			}
		}
		check(ok, "the framestore restored, first byte to last");

		check(rd(GFXCARD_REG_FRAMES) == frames_at_save && frames_at_save != 0,
		      "the frame count came with it");
		check(test_podule.irq == 1,
		      "the interrupt line put back, since nothing else carries it");

		/* The interrupt is the card's, not a register: acknowledging the vsync
		   has to drop it again, exactly as it would have before the suspend. */
		wr(GFXCARD_REG_STATUS, GFXCARD_STATUS_VSYNC);
		check(test_podule.irq == 0, "and behaves normally afterwards");

		printf("\na snapshot is no more trustworthy than the guest that wrote it\n");
		{
			/*
			 * Doctor the saved chunk in place. The layout is a word each, in
			 * the order gfxcard_savestate() writes them:
			 *
			 *   0 present   1 slot     2 easi_phys  3 ctrl    4 status
			 *   5 width     6 height   7 bpp        8 pixfmt  9 stride
			 *  10 start    11 pal_idx 12 frames    13 edid_index
			 *  14 ptr_ctrl 15 ptr_x   16 ptr_y     17 ptr_width
			 *
			 * ★ These indices MOVE when the chunk gains a field. pixfmt was
			 * added at word 8 for snapshot version 9 and pushed everything
			 * after it along by one, which is how this test found the change:
			 * it doctored what it thought was the pointer width, hit stride
			 * instead, and the clamp it was checking never fired.
			 */
			const long word_edid_index = 13;
			const long word_ptr_width  = 17;
			const uint32_t absurd = 0xffffffffu;

			rewind(f);
			fseek(f, word_edid_index * 4, SEEK_SET);
			fwrite(&absurd, sizeof absurd, 1, f);
			fseek(f, word_ptr_width * 4, SEEK_SET);
			fwrite(&absurd, sizeof absurd, 1, f);	/* width */
			fwrite(&absurd, sizeof absurd, 1, f);	/* height */

			rewind(f);
			gfxcard_loadstate(f);
			check(!savestate_error, "a doctored snapshot still loads");
			check(gfx.ptr_width == GFXCARD_PTR_MAX_WIDTH &&
			      gfx.ptr_height == GFXCARD_PTR_MAX_HEIGHT,
			      "an absurd pointer shape is clamped on the way in");
			check(gfx.edid_index <= EDID_BLOCK_SIZE,
			      "and so is an EDID index past the end of the block");
		}

		fclose(f);
	}

	/*
	 * A snapshot older than the pixel format must not have the field read out
	 * of it.
	 *
	 * The card's chunk grew a word at snapshot version 9. A loader that reads
	 * it unconditionally takes the NEXT field's bytes for it and every value
	 * after that is off by a word - which does not fail, it silently restores
	 * a machine with the wrong pointer position, palette and display start. So
	 * the guard is worth a test, and testing it needs an old-shaped chunk:
	 * this writes a current one and then splices the pixel-format word out,
	 * which is exactly what version 8 would have written.
	 */
	printf("\nan older snapshot has no pixel format in it\n");
	{
		FILE *f = tmpfile();
		FILE *old = tmpfile();
		long total;
		long cut;
		long i;

		config.gfxcard_enabled = 1;
		gfxcard_init();
		set_mode(800, 600, 16, 800 * 2, 2048);
		wr(GFXCARD_REG_PIXFMT, GFXCARD_PIXFMT_555);
		wr(GFXCARD_REG_PTR_X, 111);
		wr(GFXCARD_REG_PTR_Y, 222);
		wr(GFXCARD_REG_CTRL, GFXCARD_CTRL_ENABLE);

		gfxcard_savestate(f);
		total = ftell(f);
		check(!savestate_error && total > 0, "a current chunk to work from");

		/*
		 * Where the word sits: present, slot, easi, ctrl, status, width,
		 * height, bpp, THEN pixfmt. Counted in the order gfxcard_savestate()
		 * writes them, so if that order changes this test stops finding it and
		 * the check below fails rather than passing by accident.
		 */
		cut = 9 * 4;

		rewind(f);
		for (i = 0; i < total; i++) {
			const int c = fgetc(f);

			if (i < cut - 4 || i >= cut) {
				fputc(c, old);
			}
		}
		check(ftell(old) == total - 4, "and an older one, a word shorter");

		/* Scrub, then load the old-shaped chunk as version 8 would be. */
		gfxcard_podule_reset(&test_podule);
		stub_snapshot_version = SNAPSHOT_VERSION_GFX_PIXFMT - 1;
		rewind(old);
		gfxcard_loadstate(old);
		stub_snapshot_version = SNAPSHOT_VERSION_GFX_PIXFMT;

		check(!savestate_error, "it loads without error");
		check(rd(GFXCARD_REG_PIXFMT) == GFXCARD_PIXFMT_565,
		      "the format is 565, which is all that version could scan out");
		check(rd(GFXCARD_REG_PTR_X) == 111 && rd(GFXCARD_REG_PTR_Y) == 222,
		      "and every field after it is still in the right place");
		check(gfxcard_frame(&frame) && frame.width == 800 &&
		      frame.height == 600 && frame.bpp == 16,
		      "including the mode");

		fclose(old);
		fclose(f);
	}

	printf("\na machine with no card writes a chunk that says so\n");
	{
		FILE *f = tmpfile();
		long len;

		config.gfxcard_enabled = 0;
		gfxcard_init();
		check(gfxcard_fb == NULL, "no card fitted");

		gfxcard_savestate(f);
		len = ftell(f);
		check(!savestate_error && len == 4,
		      "the chunk is one word: there was no card");

		rewind(f);
		gfxcard_loadstate(f);
		check(!savestate_error && gfxcard_fb == NULL,
		      "and loading it leaves the machine without one");

		fclose(f);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
