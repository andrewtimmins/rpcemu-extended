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
			uint32_t off = 0, pixels = 0;

			accel_blit_row_plan(&r, row, 4 * 4, 4, &off, &pixels);
			CHECK("pixels", pixels, 3);
			memcpy(&screen[row * 6 + r.left],
			       (const uint8_t *) sprite + off, pixels * 4);
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

	/*
	 * Widening a 16bpp pixel to the screen's 32bpp.
	 *
	 * RISC OS replicates each channel's top bits into the bits it gains, so
	 * both endpoints stay exact. Every value below is worked out by hand from
	 * that rule; a test that called the same helper the code calls would agree
	 * with it about being wrong.
	 *
	 * Layout is red in the LOW bits: 565 is r@0-4 g@5-10 b@11-15, and 1:5:5:5
	 * is r@0-4 g@5-9 b@10-14 with bit 15 the T bit. The result is &xBGR, so
	 * red is the low byte and the top byte is zero.
	 */
	printf("16bpp 5:6:5 to 32bpp\n");
	CHECK("565 black", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x0000), 0x00000000);
	/* All ones: 31 -> (31<<3)|(31>>2) = 248|7 = 255, and 63 -> 252|3 = 255. */
	CHECK("565 white", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0xffff), 0x00ffffff);
	/* Red alone: r = 31 -> 0xff in the low byte. */
	CHECK("565 red", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x001f), 0x000000ff);
	/* Green alone: bits 5-10 all set = 63 -> 0xff in the second byte. */
	CHECK("565 green", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x07e0), 0x0000ff00);
	/* Blue alone: bits 11-15 all set = 31 -> 0xff in the third byte. */
	CHECK("565 blue", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0xf800), 0x00ff0000);
	/* One step of red: 1 -> (1<<3)|(1>>2) = 8. Not 0, which a bare shift and a
	   divide would both also give; the point is what happens at the top. */
	CHECK("565 red 1", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x0001), 0x00000008);
	/* One step of green: 1 -> (1<<2)|(1>>4) = 4. */
	CHECK("565 green 1", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x0020), 0x00000400);
	/* Mid grey, r=16 g=32 b=16 = 16 | 32<<5 | 16<<11 = &8410.
	   16 -> (16<<3)|(16>>2) = 128|4 = 132 = &84
	   32 -> (32<<2)|(32>>4) = 128|2 = 130 = &82 */
	CHECK("565 mid", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP, 0x8410), 0x00848284);

	printf("16bpp 1:5:5:5 to 32bpp\n");
	CHECK("1555 black", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x0000), 0x00000000);
	CHECK("1555 white", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x7fff), 0x00ffffff);
	CHECK("1555 red", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x001f), 0x000000ff);
	/* Green is bits 5-9 here, not 5-10. */
	CHECK("1555 green", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x03e0), 0x0000ff00);
	CHECK("1555 blue", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x7c00), 0x00ff0000);
	/* The T bit is not alpha and not colour: the screen has no channel for it,
	   so it changes nothing, and white with it set is still white. */
	CHECK("1555 T alone", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x8000), 0x00000000);
	CHECK("1555 T white", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0xffff), 0x00ffffff);
	/* r=g=b=16 = 16 | 16<<5 | 16<<10 = &4210, each -> &84. */
	CHECK("1555 mid", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_32BPP, 0x4210), 0x00848484);

	/* A 32bpp source is passed straight through, top byte and all: RISC OS
	   copies the word when the formats match, so we must not tidy it. */
	printf("32bpp passes through\n");
	CHECK("32bpp kept", accel_convert_pixel(ACCEL_FMT_32BPP, ACCEL_FMT_32BPP, 0xdeadbeef),
	    0xdeadbeef);
	/*
	 * Which pairs may be done here at all.
	 *
	 * The same layout at both ends is always allowed and is a plain copy - on
	 * a 16bpp desktop that is the common case and the reason any of this fires
	 * there. Widening 16bpp to 32bpp is allowed. Narrowing is NOT: RISC OS may
	 * dither when it reduces a depth, so an undithered version would differ on
	 * exactly the pictures anyone would notice. Nor is one 16bpp layout to the
	 * other, which throws a bit of green away.
	 */
	/*
	 * What layout a screen mode is in.
	 *
	 * The awkward one is 16bpp, where ModeFlag_64k (bit 7) is the only thing
	 * separating 5:6:5 from 1:5:5:5 - and that same bit means FullPalette at
	 * every other depth, so it cannot simply be tested on its own.
	 */
	printf("screen mode layouts\n");
	CHECK("32bpp", accel_mode_format(5, 0xffffffff, 0), ACCEL_FMT_32BPP);
	/* 16bpp with the 64k flag clear and 65536 colours is 1:5:5:5. */
	CHECK("16bpp 1555", accel_mode_format(4, 65535, 0), ACCEL_FMT_1555);
	/* The same depth with bit 7 set is 5:6:5, and nothing else distinguishes
	   them. */
	CHECK("16bpp 565", accel_mode_format(4, 65535, 1u << 7), ACCEL_FMT_565);
	/* Under 4096 colours at that depth is 4:4:4:4, whose fourth channel is
	   alpha, so it is not written here. */
	CHECK("16bpp 4444", accel_mode_format(4, 4095, 0), ACCEL_FMT_NONE);
	/* Bit 7 at 32bpp is FullPalette and must not be read as 5:6:5. */
	CHECK("32bpp b7 set", accel_mode_format(5, 0xffffffff, 1u << 7),
	    ACCEL_FMT_32BPP);
	/* An &xRGB screen is refused at either depth: our pixels are &xBGR. */
	CHECK("32bpp RGB", accel_mode_format(5, 0xffffffff, 1u << 14),
	    ACCEL_FMT_NONE);
	CHECK("16bpp RGB", accel_mode_format(4, 65535, (1u << 14) | (1u << 7)),
	    ACCEL_FMT_NONE);
	/* Palette depths are not written here. */
	CHECK("24bpp", accel_mode_format(6, 0xffffff, 0), ACCEL_FMT_NONE);

	printf("which conversions are allowed\n");
	CHECK("565->565", accel_can_convert(ACCEL_FMT_565, ACCEL_FMT_565), 1);
	CHECK("1555->1555", accel_can_convert(ACCEL_FMT_1555, ACCEL_FMT_1555), 1);
	CHECK("32->32", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_32BPP), 1);
	CHECK("565->32", accel_can_convert(ACCEL_FMT_565, ACCEL_FMT_32BPP), 1);
	CHECK("1555->32", accel_can_convert(ACCEL_FMT_1555, ACCEL_FMT_32BPP), 1);
	CHECK("4444->32", accel_can_convert(ACCEL_FMT_4444, ACCEL_FMT_32BPP), 1);
	CHECK("4444->1555", accel_can_convert(ACCEL_FMT_4444, ACCEL_FMT_1555), 1);
	/* Narrowing is allowed because dithering is off unless R5 bit 6 asks for
	   it, and a plot that asks is refused on the action instead. */
	CHECK("32->565", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_565), 1);
	CHECK("32->1555", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_1555), 1);
	CHECK("565->1555", accel_can_convert(ACCEL_FMT_565, ACCEL_FMT_1555), 1);
	CHECK("1555->565", accel_can_convert(ACCEL_FMT_1555, ACCEL_FMT_565), 1);
	/* 4:4:4:4 is a source only: as a screen it has a fourth channel to fill. */
	CHECK("32->4444", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_4444), 0);
	CHECK("none->32", accel_can_convert(ACCEL_FMT_NONE, ACCEL_FMT_32BPP), 0);
	CHECK("32->none", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_NONE), 0);

	/*
	 * A palette depth may be copied to itself and converted to nothing.
	 *
	 * An 8bpp pixel is an index; with no translation table asked for, RISC OS
	 * puts the same index on an 8bpp screen and needs no palette to do it, so
	 * neither do we. But an index cannot be turned INTO a colour without the
	 * palette, nor a colour into an index without searching one, so every
	 * other pair involving it is refused.
	 */
	CHECK("8->8", accel_can_convert(ACCEL_FMT_8BPP, ACCEL_FMT_8BPP), 1);
	CHECK("8->32", accel_can_convert(ACCEL_FMT_8BPP, ACCEL_FMT_32BPP), 0);
	CHECK("8->1555", accel_can_convert(ACCEL_FMT_8BPP, ACCEL_FMT_1555), 0);
	CHECK("32->8", accel_can_convert(ACCEL_FMT_32BPP, ACCEL_FMT_8BPP), 0);
	CHECK("1555->8", accel_can_convert(ACCEL_FMT_1555, ACCEL_FMT_8BPP), 0);
	CHECK("8bpp index kept", accel_convert_pixel(ACCEL_FMT_8BPP,
	    ACCEL_FMT_8BPP, 0xa7), 0xa7);
	CHECK("bytes 8bpp", accel_format_bytes(ACCEL_FMT_8BPP), 1);
	/* An 8bpp screen is writable, from an 8bpp sprite and nothing else. */
	CHECK("8bpp screen", accel_mode_format(3, 255, 0), ACCEL_FMT_8BPP);
	/* 4bpp and below are not: a clipped row would start part-way into a byte. */
	CHECK("4bpp screen", accel_mode_format(2, 15, 0), ACCEL_FMT_NONE);
	CHECK("1bpp screen", accel_mode_format(0, 1, 0), ACCEL_FMT_NONE);

	/*
	 * 4:4:4:4 to the screen. Channels are r@0-3 g@4-7 b@8-11, alpha above
	 * them and dropped: no screen mode here has a channel to put it in.
	 * Widening 4 bits to 8 is (v<<4)|v, so &F becomes &FF and &1 becomes &11.
	 */
	printf("16bpp 4:4:4:4 to 32bpp\n");
	CHECK("4444 black", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x0000), 0x00000000);
	/* Alpha all ones, colour all zero: the alpha must not reach the pixel. */
	CHECK("4444 alpha only", accel_convert_pixel(ACCEL_FMT_4444,
	    ACCEL_FMT_32BPP, 0xf000), 0x00000000);
	CHECK("4444 white", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x0fff), 0x00ffffff);
	CHECK("4444 red", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x000f), 0x000000ff);
	CHECK("4444 green", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x00f0), 0x0000ff00);
	CHECK("4444 blue", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x0f00), 0x00ff0000);
	/* One step of red: 1 -> (1<<4)|1 = &11. */
	CHECK("4444 red 1", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x0001), 0x00000011);
	/* r=g=b=8 -> (8<<4)|8 = &88 each. */
	CHECK("4444 mid", accel_convert_pixel(ACCEL_FMT_4444, ACCEL_FMT_32BPP,
	    0x0888), 0x00888888);

	/*
	 * 4:4:4:4 to 1:5:5:5, which is the pair that matters on a 16bpp desktop.
	 * Widening 4 bits to 5 is (v<<1)|(v>>3): &F -> &1F, &8 -> &10|&1 = &11.
	 */
	printf("16bpp 4:4:4:4 to 1:5:5:5\n");
	CHECK("4444->1555 white", accel_convert_pixel(ACCEL_FMT_4444,
	    ACCEL_FMT_1555, 0xffff), 0x7fff);
	CHECK("4444->1555 red", accel_convert_pixel(ACCEL_FMT_4444,
	    ACCEL_FMT_1555, 0x000f), 0x001f);
	CHECK("4444->1555 blue", accel_convert_pixel(ACCEL_FMT_4444,
	    ACCEL_FMT_1555, 0x0f00), 0x7c00);
	/* r=8 -> &11 = 17, at bits 0-4. */
	CHECK("4444->1555 r8", accel_convert_pixel(ACCEL_FMT_4444,
	    ACCEL_FMT_1555, 0x0008), 17);

	/*
	 * Narrowing keeps the TOP bits and throws the rest away - a plain UBFX in
	 * RISC OS's generated code, no rounding. So &FF -> &1F and &FF -> &3F,
	 * and a value just under a step boundary rounds DOWN, not to nearest.
	 */
	printf("narrowing to 16bpp\n");
	CHECK("32->1555 white", accel_convert_pixel(ACCEL_FMT_32BPP,
	    ACCEL_FMT_1555, 0x00ffffff), 0x7fff);
	CHECK("32->1555 black", accel_convert_pixel(ACCEL_FMT_32BPP,
	    ACCEL_FMT_1555, 0x00000000), 0x0000);
	/* Red &FF -> top 5 bits = &1F. Green and blue zero. */
	CHECK("32->1555 red", accel_convert_pixel(ACCEL_FMT_32BPP,
	    ACCEL_FMT_1555, 0x000000ff), 0x001f);
	/* Red &F8 is exactly &1F<<3, so it truncates to &1F with nothing lost. */
	CHECK("32->1555 r F8", accel_convert_pixel(ACCEL_FMT_32BPP,
	    ACCEL_FMT_1555, 0x000000f8), 0x001f);
	/* Red &F7 is one below that and truncates DOWN to &1E, not up. */
	CHECK("32->1555 r F7", accel_convert_pixel(ACCEL_FMT_32BPP,
	    ACCEL_FMT_1555, 0x000000f7), 0x001e);
	/* 5:6:5 green has six bits; to 1:5:5:5 it loses its lowest one. */
	CHECK("565->1555 green", accel_convert_pixel(ACCEL_FMT_565,
	    ACCEL_FMT_1555, 0x07e0), 0x03e0);
	/* Green &3E is 62, whose top five bits are 31: still full green. */
	CHECK("565->1555 g 3e", accel_convert_pixel(ACCEL_FMT_565,
	    ACCEL_FMT_1555, 0x07c0), 0x03e0);
	/* And back the other way green gains a bit by replication. */
	CHECK("1555->565 green", accel_convert_pixel(ACCEL_FMT_1555,
	    ACCEL_FMT_565, 0x03e0), 0x07e0);
	CHECK("1555->565 white", accel_convert_pixel(ACCEL_FMT_1555,
	    ACCEL_FMT_565, 0x7fff), 0xffff);

	/*
	 * ★ Widening and narrowing must be each other's inverse where they can be.
	 * Every 5-bit value widened to 8 and narrowed back must come home, or the
	 * two rules disagree and a picture copied between depths drifts.
	 */
	printf("widen then narrow is a round trip\n");
	{
		int v, bad = 0;

		for (v = 0; v < 32; v++) {
			const uint32_t up = accel_convert_pixel(ACCEL_FMT_1555,
			    ACCEL_FMT_32BPP, (uint32_t) v);
			const uint32_t down = accel_convert_pixel(ACCEL_FMT_32BPP,
			    ACCEL_FMT_1555, up);

			if (down != (uint32_t) v) {
				bad++;
			}
		}
		CHECK("1555 round trip", bad, 0);
		for (v = 0; v < 16; v++) {
			const uint32_t up = accel_convert_pixel(ACCEL_FMT_4444,
			    ACCEL_FMT_32BPP, (uint32_t) v);
			const uint32_t down = accel_convert_pixel(ACCEL_FMT_32BPP,
			    ACCEL_FMT_1555, up);
			const uint32_t direct = accel_convert_pixel(ACCEL_FMT_4444,
			    ACCEL_FMT_1555, (uint32_t) v);

			if (down != direct) {
				bad++;
			}
		}
		CHECK("4444 via 32 == direct", bad, 0);
	}

	/* Same layout both ends leaves the pixel entirely alone, T bit included -
	   the screen wants those exact bytes. */
	CHECK("565 copied", accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_565,
	    0x8410), 0x8410);
	CHECK("1555 copied", accel_convert_pixel(ACCEL_FMT_1555, ACCEL_FMT_1555,
	    0xc210), 0xc210);

	CHECK("bytes 32", accel_format_bytes(ACCEL_FMT_32BPP), 4);
	CHECK("bytes 565", accel_format_bytes(ACCEL_FMT_565), 2);
	CHECK("bytes 1555", accel_format_bytes(ACCEL_FMT_1555), 2);
	CHECK("bytes none", accel_format_bytes(ACCEL_FMT_NONE), 0);

	/*
	 * A clipped 16bpp plot, end to end, against an image written out by hand.
	 *
	 * The sprite is three pixels wide, which at 16bpp is 48 bits and pads to
	 * two whole words - so its rows are 8 bytes apart while its pixels take 6.
	 * That gap is the point of the case: a plan that took the stride to be the
	 * width times the depth would start each row two bytes early and lean the
	 * picture further left the further down it went.
	 */
	printf("copying a clipped 16bpp sprite\n");
	{
		/* Six 565 values, one per pixel, distinct in all three channels so a
		   shear, a flip or a swapped channel all show up. Row 0 then row 1. */
		static const uint16_t src[2][4] = {
			{ 0x001f, 0x07e0, 0xf800, 0x0000 },	/* red, green, blue, pad */
			{ 0xffff, 0x8410, 0x0001, 0x0000 },	/* white, mid grey, red 1 */
		};
		/* What each of those becomes, worked out above. */
		static const uint32_t want[2][3] = {
			{ 0x000000ff, 0x0000ff00, 0x00ff0000 },
			{ 0x00ffffff, 0x00848284, 0x00000008 },
		};
		uint32_t screen[3 * 5];
		uint32_t expected[3 * 5];
		int row, col;

		for (row = 0; row < 3 * 5; row++) {
			screen[row] = 0xeeeeeeeeu;
			expected[row] = 0xeeeeeeeeu;
		}

		/*
		 * Screen 5x3. Graphics window columns 1-4, rows 0-1 up from the
		 * bottom, so framebuffer rows 3-1-1 = 1 to 3-1-0 = 2.
		 *
		 * Plot at OS (0,0), eig 0,0: left 0, bottom 0, so the sprite's top row
		 * is 3 - 0 - 2 = 1 and it wants columns 0-2, rows 1-2.
		 *
		 * Clipped to the window: columns 1-2, rows 1-2. The sprite's leftmost
		 * column is cut off, both rows survive.
		 */
		visible = accel_plot_rect(5, 3, 1, 0, 4, 1, 0, 0, 0, 0,
		    0, 0, 3, 2, &r);
		CHECK("visible", visible, 1);
		CHECK("left", r.left, 1);
		CHECK("right", r.right, 2);
		CHECK("top", r.top, 1);
		CHECK("bottom", r.bottom, 2);
		CHECK("origin_left", r.origin_left, 0);
		CHECK("origin_top", r.origin_top, 1);

		for (row = r.top; row <= r.bottom; row++) {
			uint32_t off = 0, pixels = 0;
			uint32_t i;

			/* Stride 8 bytes a row, 2 bytes a pixel. */
			accel_blit_row_plan(&r, row, 8, 2, &off, &pixels);
			CHECK("pixels", pixels, 2);
			/* Row 1 starts at the sprite's row 0, one pixel in: offset 2.
			   Row 2 starts at row 1, one pixel in: 8 + 2 = 10. */
			CHECK("offset", off, (row == 1) ? 2 : 10);

			for (i = 0; i < pixels; i++) {
				const uint8_t *p = (const uint8_t *) src + off + i * 2;

				screen[row * 5 + r.left + i] =
				    accel_convert_pixel(ACCEL_FMT_565, ACCEL_FMT_32BPP,
				        (uint32_t) p[0] | ((uint32_t) p[1] << 8));
			}
		}

		/*
		 * By hand: screen row 1 takes sprite row 0 columns 1 and 2 (green then
		 * blue) into screen columns 1 and 2; screen row 2 takes sprite row 1
		 * columns 1 and 2 (mid grey then red 1). Row 0, column 0 and columns
		 * 3-4 are untouched.
		 */
		expected[1 * 5 + 1] = want[0][1];
		expected[1 * 5 + 2] = want[0][2];
		expected[2 * 5 + 1] = want[1][1];
		expected[2 * 5 + 2] = want[1][2];

		for (row = 0; row < 3; row++) {
			for (col = 0; col < 5; col++) {
				const int i = row * 5 + col;

				if (screen[i] != expected[i]) {
					printf("  FAIL pixel (%d,%d) got &%08x, wanted &%08x\n",
					       col, row, screen[i], expected[i]);
					failures++;
				}
			}
		}
	}

	if (failures != 0) {
		printf("FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("PASS\n");
	return EXIT_SUCCESS;
}
