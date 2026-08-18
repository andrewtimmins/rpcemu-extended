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
 * test_accel_plot - where a host-side sprite plot lands.
 *
 * The accelerators do a plot RISC OS would otherwise do a pixel at a time, and
 * the way that goes wrong is not the copying, it is the coordinates: RISC OS
 * counts y up from the bottom of the screen and gives positions in OS units
 * relative to a movable origin, while a framebuffer counts rows down from the
 * top; graphics window edges are inclusive at both ends.
 *
 * Every expectation here is worked out by hand in the comment beside it, because
 * a test that computes the answer the same way the code does would agree with
 * the code about being upside down.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accelerators.h"

static int failures;

#define CHECK(what, got, want)                                                 \
	do {                                                                   \
		const long g = (long) (got), w = (long) (want);                \
									       \
		if (g != w) {                                                  \
			printf("  FAIL %-14s got %ld, wanted %ld\n", what, g,  \
			    w);                                                \
			failures++;                                            \
		}                                                              \
	} while (0)

/* A 1920x1080 screen with a full-screen graphics window, origin at the bottom
   left, and two OS units to the pixel in both directions. */
#define W	1920
#define H	1080
#define FULL	0, 0, W - 1, H - 1
#define EIG	1, 1

int
main(void)
{
	AccelPlotRect r;
	int visible;

	printf("test_accel_plot\n");

	/*
	 * A 64x64 sprite at the origin. In OS units (0,0) is the bottom left, so
	 * the sprite occupies the bottom-left 64 pixels: columns 0-63, and rows
	 * 1080-0-64 = 1016 to 1079.
	 */
	visible = accel_plot_rect(W, H, FULL, 0, 0, EIG, 0, 0, 64, 64, &r);
	printf("bottom-left corner\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 0);
	CHECK("right", r.right, 63);
	CHECK("top", r.top, 1016);
	CHECK("bottom", r.bottom, 1079);
	CHECK("origin_top", r.origin_top, 1016);

	/*
	 * The same sprite at OS (256, 512): 128 pixels across, 256 pixels up. Its
	 * top row is 1080 - 256 - 64 = 760.
	 */
	visible = accel_plot_rect(W, H, FULL, 0, 0, EIG, 256, 512, 64, 64, &r);
	printf("placed away from the origin\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 128);
	CHECK("right", r.right, 191);
	CHECK("top", r.top, 760);
	CHECK("bottom", r.bottom, 823);

	/*
	 * Top of the screen. At OS y = 2032 the sprite sits 1016 pixels up, so its
	 * top row is 1080 - 1016 - 64 = 0: exactly flush, nothing clipped.
	 */
	visible = accel_plot_rect(W, H, FULL, 0, 0, EIG, 0, 2032, 64, 64, &r);
	printf("flush with the top\n");
	CHECK("visible", visible, 1);
	CHECK("top", r.top, 0);
	CHECK("bottom", r.bottom, 63);

	/*
	 * A Wimp redraw rectangle: the graphics window is columns 100-299 and rows
	 * 200-399 counted up from the bottom, which is framebuffer rows
	 * 1080-1-399 = 680 to 1080-1-200 = 879.
	 *
	 * The sprite is plotted at OS (0,0), so it wants columns 0-63 and rows
	 * 1016-1079. Columns are wholly left of the window and rows wholly below
	 * it, so nothing is drawn.
	 */
	visible = accel_plot_rect(W, H, 100, 200, 299, 399, 0, 0, EIG,
	    0, 0, 64, 64, &r);
	printf("outside the redraw rectangle\n");
	CHECK("visible", visible, 0);

	/*
	 * The same window, with the sprite plotted at OS (200, 600): pixels
	 * (100, 300), so columns 100-163 and rows 1080-300-64 = 716 to 779. Both
	 * inside, so nothing is clipped away.
	 */
	visible = accel_plot_rect(W, H, 100, 200, 299, 399, 0, 0, EIG,
	    200, 600, 64, 64, &r);
	printf("inside the redraw rectangle\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 100);
	CHECK("right", r.right, 163);
	CHECK("top", r.top, 716);
	CHECK("bottom", r.bottom, 779);

	/*
	 * Clipped on two sides by that window. At OS (500, 380) the sprite is at
	 * pixels (250, 190): columns 250-313 against a right edge of 299, and rows
	 * 1080-190-64 = 826 to 889 against a bottom edge of 879.
	 */
	visible = accel_plot_rect(W, H, 100, 200, 299, 399, 0, 0, EIG,
	    500, 380, 64, 64, &r);
	printf("clipped by the window\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 250);
	CHECK("right", r.right, 299);
	CHECK("top", r.top, 826);
	CHECK("bottom", r.bottom, 879);
	/* The unclipped position is what the source is indexed from, so it must
	   survive the clipping: the first column drawn is 250-250 = 0 into the
	   sprite, and the first row drawn is 826-826 = 0. */
	CHECK("origin_left", r.origin_left, 250);
	CHECK("origin_top", r.origin_top, 826);

	/*
	 * A moved graphics origin, which is how a task plots relative to its own
	 * window. Origin (100, 200) in OS units with the sprite at OS (100, 200)
	 * puts it at OS (200, 400) absolute: pixels (100, 200), rows
	 * 1080-200-64 = 816 to 879.
	 */
	visible = accel_plot_rect(W, H, FULL, 100, 200, EIG, 100, 200, 64, 64, &r);
	printf("with the origin moved\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 100);
	CHECK("top", r.top, 816);
	CHECK("bottom", r.bottom, 879);

	/*
	 * A mode with four OS units to the pixel across and two down, which is what
	 * a low-resolution mode looks like. OS (256, 512) is then pixel (64, 256).
	 */
	visible = accel_plot_rect(640, 480, 0, 0, 639, 479, 0, 0, 2, 1,
	    256, 512, 32, 32, &r);
	printf("with wider eig factors\n");
	CHECK("visible", visible, 1);
	CHECK("left", r.left, 64);
	CHECK("right", r.right, 95);
	/* 480 - 256 - 32 = 192 */
	CHECK("top", r.top, 192);
	CHECK("bottom", r.bottom, 223);

	/*
	 * A sprite hanging off the bottom of the screen: at OS y = -64 it is 32
	 * pixels below the bottom, so its top row is 480 - (-32) - 32 = 480, past
	 * the last row. The window clips it to nothing rather than letting it write
	 * past the end of the framebuffer.
	 */
	visible = accel_plot_rect(640, 480, 0, 0, 639, 479, 0, 0, 1, 1,
	    0, -64, 32, 32, &r);
	printf("hanging off the bottom\n");
	CHECK("visible", visible, 0);

	/*
	 * And one hanging off the top, where the top row goes negative: 480 - 464
	 * - 32 = -16. Sixteen rows are above the screen, and the sixteen that are
	 * on it must still be drawn, from row 0.
	 */
	visible = accel_plot_rect(640, 480, 0, 0, 639, 479, 0, 0, 1, 1,
	    0, 928, 32, 32, &r);
	printf("hanging off the top\n");
	CHECK("visible", visible, 1);
	CHECK("top", r.top, 0);
	CHECK("bottom", r.bottom, 15);
	/* The source is indexed from the unclipped position, so the first row
	   drawn is row 16 of the sprite: 0 - (-16). */
	CHECK("origin_top", r.origin_top, -16);

	/*
	 * The copy itself, end to end, against an image drawn by hand.
	 *
	 * A 6x4 screen, a 4x3 sprite whose pixels are numbered so a shear or a flip
	 * shows up as an out-of-order value, and a graphics window that cuts the
	 * plot on two sides. Every expected pixel below is worked out from the
	 * geometry, not from running the code.
	 */
	printf("copying a clipped sprite\n");
	{
		/* Sprite pixels, top row first: 0x100 + row * 16 + column. */
		uint32_t sprite[3 * 4];
		uint32_t screen[4 * 6];
		uint32_t expected[4 * 6];
		int row, col;

		for (row = 0; row < 3; row++) {
			for (col = 0; col < 4; col++) {
				sprite[row * 4 + col] =
				    0x100u + (uint32_t) row * 16u + (uint32_t) col;
			}
		}
		for (row = 0; row < 4 * 6; row++) {
			screen[row] = 0xeeeeeeeeu;
			expected[row] = 0xeeeeeeeeu;
		}

		/*
		 * Screen 6x4. Graphics window: columns 1-3, rows 0-2 counted up from
		 * the bottom, so framebuffer rows 4-1-2 = 1 to 4-1-0 = 3.
		 *
		 * Plot at OS (0,0) with eig 0,0 (one OS unit per pixel here, to keep
		 * the arithmetic visible): left 0, bottom 0, so the sprite's top row is
		 * 4 - 0 - 3 = 1, and it wants columns 0-3, rows 1-3.
		 *
		 * Clipped to the window: columns 1-3, rows 1-3. So the leftmost column
		 * of the sprite is cut off, and all three rows survive.
		 */
		visible = accel_plot_rect(6, 4, 1, 0, 3, 2, 0, 0, 0, 0,
		    0, 0, 4, 3, &r);
		CHECK("visible", visible, 1);
		CHECK("left", r.left, 1);
		CHECK("right", r.right, 3);
		CHECK("top", r.top, 1);
		CHECK("bottom", r.bottom, 3);
		CHECK("origin_left", r.origin_left, 0);
		CHECK("origin_top", r.origin_top, 1);

		for (row = r.top; row <= r.bottom; row++) {
			uint32_t off = 0, bytes = 0;

			accel_blit_row_plan(&r, row, 4 * 4, &off, &bytes);
			CHECK("bytes", bytes, 3 * 4);
			memcpy(&screen[row * 6 + r.left],
			       (const uint8_t *) sprite + off, bytes);
		}

		/*
		 * By hand: row 1 of the screen takes sprite row 0 columns 1,2,3 =
		 * 0x101, 0x102, 0x103, placed at screen columns 1,2,3. Row 2 takes
		 * sprite row 1: 0x111, 0x112, 0x113. Row 3 takes sprite row 2: 0x121,
		 * 0x122, 0x123. Row 0 and columns 0, 4 and 5 are untouched.
		 */
		expected[1 * 6 + 1] = 0x101; expected[1 * 6 + 2] = 0x102;
		expected[1 * 6 + 3] = 0x103;
		expected[2 * 6 + 1] = 0x111; expected[2 * 6 + 2] = 0x112;
		expected[2 * 6 + 3] = 0x113;
		expected[3 * 6 + 1] = 0x121; expected[3 * 6 + 2] = 0x122;
		expected[3 * 6 + 3] = 0x123;

		for (row = 0; row < 4; row++) {
			for (col = 0; col < 6; col++) {
				const int i = row * 6 + col;

				if (screen[i] != expected[i]) {
					printf("  FAIL pixel (%d,%d) got &%03x, wanted &%03x\n",
					       col, row, screen[i], expected[i]);
					failures++;
				}
			}
		}
	}

	/*
	 * How wide a sprite row really is.
	 *
	 * The header counts WORDS less one, so `width + 1` is the pixel count only
	 * at 32bpp. Each case below is the bits from lbit to rbit inclusive, plus
	 * whole words for the rest, divided by the depth - worked out here, not
	 * taken from the code.
	 */
	printf("row width in pixels\n");
	/* 4 pixels at 32bpp: 3 whole words + bits 0-31 = 96 + 32 = 128, / 32. */
	CHECK("32bpp 4px", accel_sprite_row_pixels(3, 0, 31, 32), 4);
	/* 5 pixels at 16bpp = 80 bits = 2.5 words, so 3 words with the last one
	   half used: 2 whole words + bits 0-15 = 64 + 16 = 80, / 16. */
	CHECK("16bpp 5px", accel_sprite_row_pixels(2, 0, 15, 16), 5);
	/* 6 pixels at 16bpp fills its last word: 64 + 32 = 96, / 16. */
	CHECK("16bpp 6px", accel_sprite_row_pixels(2, 0, 31, 16), 6);
	/* The trap this exists for: read as words, that same sprite is 3 wide. */
	CHECK("16bpp not 3", accel_sprite_row_pixels(2, 0, 31, 16) != 3, 1);
	/* 5 pixels at 8bpp: 1 whole word + bits 0-7 = 32 + 8 = 40, / 8. */
	CHECK("8bpp 5px", accel_sprite_row_pixels(1, 0, 7, 8), 5);
	/* 33 pixels at 1bpp: 1 whole word + bit 0 = 32 + 1. */
	CHECK("1bpp 33px", accel_sprite_row_pixels(1, 0, 0, 1), 33);
	/*
	 * Nonsense headers answer 0 rather than a plausible number. Each case
	 * here is chosen so that ONE guard is the thing rejecting it: a header
	 * that two guards both catch proves neither of them works.
	 */
	CHECK("no depth", accel_sprite_row_pixels(3, 0, 31, 0), 0);
	/* One whole word plus bits 0-15 is 48 bits, which is a pixel and a half
	   at 32bpp. Truncating would answer 1 and draw half a row. */
	CHECK("part pixel", accel_sprite_row_pixels(1, 0, 15, 32), 0);
	/* rbit below lbit: at 1bpp every bit count divides, so the part-pixel
	   guard cannot save this one and the wrapped subtraction would answer
	   with most of four billion. */
	CHECK("rbit < lbit", accel_sprite_row_pixels(0, 8, 0, 1), 0);

	if (failures != 0) {
		printf("FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("PASS\n");
	return EXIT_SUCCESS;
}
