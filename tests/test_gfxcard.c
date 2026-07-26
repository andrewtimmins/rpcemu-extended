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
	check((rd(GFXCARD_REG_CAPS) & GFXCARD_CAP_16BPP) == 0,
	      "16bpp not offered, since it is not implemented");
	check(rd(GFXCARD_REG_MAX_WIDTH) == GFXCARD_MAX_WIDTH &&
	      rd(GFXCARD_REG_MAX_HEIGHT) == GFXCARD_MAX_HEIGHT,
	      "largest accepted mode is readable");
	check(rd(GFXCARD_REG_FB_PHYS) == gfxcard_fb_phys &&
	      rd(GFXCARD_REG_FB_SIZE) == GFXCARD_FB_SIZE,
	      "framestore address and size readable");

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

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
