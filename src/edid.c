/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 */

/*
 * Synthesised EDID for the emulated monitor.
 *
 * On a Risc PC there is no DDC bus, so RISC OS's "Auto" monitor detection is
 * answered by an EDID block the video driver makes up in software. Stock builds
 * make up an empty one (no usable timings), so Auto learns nothing. This module
 * generates a populated block instead: a preferred timing describing the mode we
 * want the desktop to open in, plus a spread of standard and established modes
 * below it, so a machine configured for MonitorType Auto simply comes up right.
 *
 * The timing numbers use fixed reduced-blanking-style porches. RPCEmu draws the
 * display in software and ignores the blanking intervals, so only the active
 * pixel counts and a plausible dot clock matter; the fixed porches keep the
 * generator deterministic for any requested size.
 */

#include <string.h>

#include "edid.h"

/* Fixed blanking geometry (reduced-blanking flavour) used for the generated
   preferred timing. */
#define HBLANK_PIXELS	160u	/* 48 front + 32 sync + 80 back */
#define HFRONT_PIXELS	48u
#define HSYNC_PIXELS	32u
#define VFRONT_LINES	3u
#define VSYNC_LINES	6u
#define VBACK_LINES	29u
#define VBLANK_LINES	(VFRONT_LINES + VSYNC_LINES + VBACK_LINES)

/* Standard-timing aspect-ratio codes (EDID byte 2, bits 7-6). */
#define ASPECT_16_10	0u
#define ASPECT_4_3	1u
#define ASPECT_5_4	2u
#define ASPECT_16_9	3u

/* A mode this block can advertise, and where the bit that advertises it lives.
   Both the established-timings bitmap and the Established Timings III
   descriptor are bitmaps over a fixed list of modes, so one shape does for
   both: the byte within the bitmap, the bit within that byte, and the size the
   bit stands for. */
typedef struct {
	uint8_t		byte;	/**< Index into the bitmap */
	uint8_t		bit;	/**< Bit within that byte */
	uint16_t	x;	/**< Mode width, for deciding whether to set it */
	uint16_t	y;	/**< Mode height */
} timing_bit_t;

/* The seventeen modes of the base block's established-timings bitmap (bytes
   0x23-0x25). Only the progressive, non-Apple entries are listed: 640x480@67,
   1024x768@87 interlaced and 720x400@88 are left out deliberately, being
   either interlaced or for hardware this never emulates.

   These are the small legacy modes. They overlap the Established Timings III
   list below hardly at all, which is why both are worth filling in. */
static const timing_bit_t established_timings[] = {
	{ 0, 7,  720, 400 },	/* 720x400 @ 70 */
	{ 0, 5,  640, 480 },	/* 640x480 @ 60 */
	{ 0, 3,  640, 480 },	/* 640x480 @ 72 */
	{ 0, 2,  640, 480 },	/* 640x480 @ 75 */
	{ 0, 1,  800, 600 },	/* 800x600 @ 56 */
	{ 0, 0,  800, 600 },	/* 800x600 @ 60 */
	{ 1, 7,  800, 600 },	/* 800x600 @ 72 */
	{ 1, 6,  800, 600 },	/* 800x600 @ 75 */
	{ 1, 5,  832, 624 },	/* 832x624 @ 75 */
	{ 1, 3, 1024, 768 },	/* 1024x768 @ 60 */
	{ 1, 2, 1024, 768 },	/* 1024x768 @ 70 */
	{ 1, 1, 1024, 768 },	/* 1024x768 @ 75 */
	{ 1, 0, 1280, 1024 },	/* 1280x1024 @ 75 */
	{ 2, 7, 1152, 870 },	/* 1152x870 @ 75 */
};

/* The Established Timings III descriptor (tag 0xF7): a six-byte bitmap over a
   fixed list of VESA DMT modes, in one 18-byte descriptor slot.

   This is where the mode list gets its size. The base block can only carry
   eight two-byte standard timings, and those encode a width and an aspect-ratio
   code, so a height that is not one of four ratios of a width cannot be said at
   all - 1920x1200 has no standard-timing encoding, and there was no ninth slot
   for it in any case. The bitmap below says forty-four modes in eighteen bytes
   and is not bound by aspect ratio, which is what makes a full mode list
   possible.

   The order is fixed by the standard and mirrors established_timings3[] in
   RISC OS's ScreenModes (Video/UserI/ScrModes): bit 7 of the first bitmap byte
   is the first entry, and each bit indexes a DMT mode number. The sizes here
   are those DMT modes, so a bit is only set when its mode is one this machine
   could actually display.

   Note 2560x1440 is absent: DMT has no such mode (it jumps from 1920x1440 to
   2560x1600), so the card's largest mode can still only be advertised as the
   preferred detailed timing. */
static const timing_bit_t established_timings3[] = {
	{ 0, 7,  640,  350 }, { 0, 6,  640,  400 },
	{ 0, 5,  720,  400 }, { 0, 4,  640,  480 },
	{ 0, 3,  848,  480 }, { 0, 2,  800,  600 },
	{ 0, 1, 1024,  768 }, { 0, 0, 1152,  864 },
	{ 1, 7, 1280,  768 }, { 1, 6, 1280,  768 },
	{ 1, 5, 1280,  768 }, { 1, 4, 1280,  768 },
	{ 1, 3, 1280,  960 }, { 1, 2, 1280,  960 },
	{ 1, 1, 1280, 1024 }, { 1, 0, 1280, 1024 },
	{ 2, 7, 1360,  768 }, { 2, 6, 1440,  900 },
	{ 2, 5, 1440,  900 }, { 2, 4, 1440,  900 },
	{ 2, 3, 1440,  900 }, { 2, 2, 1400, 1050 },
	{ 2, 1, 1400, 1050 }, { 2, 0, 1400, 1050 },
	{ 3, 7, 1400, 1050 }, { 3, 6, 1680, 1050 },
	{ 3, 5, 1680, 1050 }, { 3, 4, 1680, 1050 },
	{ 3, 3, 1680, 1050 }, { 3, 2, 1600, 1200 },
	{ 3, 1, 1600, 1200 }, { 3, 0, 1600, 1200 },
	{ 4, 7, 1600, 1200 }, { 4, 6, 1600, 1200 },
	{ 4, 5, 1792, 1344 }, { 4, 4, 1792, 1344 },
	{ 4, 3, 1856, 1392 }, { 4, 2, 1856, 1392 },
	{ 4, 1, 1920, 1200 }, { 4, 0, 1920, 1200 },
	{ 5, 7, 1920, 1200 }, { 5, 6, 1920, 1200 },
	{ 5, 5, 1920, 1440 }, { 5, 4, 1920, 1440 },
};

/* Where the Established Timings III bitmap starts within its descriptor, and
   the revision byte the standard requires ahead of it. */
#define ET3_TAG			0xf7u
#define ET3_REVISION		0x0au
#define ET3_BITMAP_OFFSET	6u
#define ET3_BITMAP_BYTES	6u

/* Descriptor slots, as byte offsets into the block. The first carries the
   preferred timing; the rest are dummies in every ROM block we have seen, so
   the second is free for the mode bitmap. */
#define DESC_PREFERRED		0x36u
#define DESC_SECOND		0x48u
#define DESC_THIRD		0x5au
#define DESC_FOURTH		0x6cu

/**
 * Set the bits of a timing bitmap for every listed mode that fits.
 *
 * A mode is advertised when it is no larger than the preferred mode in either
 * direction. That single test covers screen memory as well as the host display:
 * the preferred mode has already been chosen to fit both (see
 * display_mode_fit), and a mode no wider and no taller cannot need more
 * framebuffer than one that does. Advertising anything else would offer the
 * user a mode RISC OS then refuses as "not suitable for displaying the
 * desktop".
 *
 * @param bitmap  Bitmap to set bits in
 * @param bytes   Its length, so a table entry cannot write past it
 * @param table   Modes and their bit positions
 * @param count   Entries in that table
 * @param x       Preferred mode width
 * @param y       Preferred mode height
 */
static void
set_timing_bits(uint8_t *bitmap, unsigned bytes,
                const timing_bit_t *table, unsigned count,
                unsigned x, unsigned y)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		if (table[i].byte >= bytes) {
			continue;
		}
		if (table[i].x > x || table[i].y > y) {
			continue;
		}
		bitmap[table[i].byte] |= (uint8_t) (1u << table[i].bit);
	}
}

int
edid_block_is_valid(const uint8_t block[EDID_BLOCK_SIZE])
{
	static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	unsigned sum = 0;
	int i;

	if (memcmp(block, header, sizeof(header)) != 0) {
		return 0;
	}
	if (block[18] != 1) {		/* EDID major version 1 */
		return 0;
	}
	for (i = 0; i < EDID_BLOCK_SIZE; i++) {
		sum += block[i];
	}
	return (sum & 0xff) == 0;
}

/* Encode one 18-byte detailed timing descriptor for x*y @ hz. */
static void
build_detailed_timing(uint8_t d[18], unsigned x, unsigned y, unsigned hz)
{
	const unsigned htotal = x + HBLANK_PIXELS;
	const unsigned vtotal = y + VBLANK_LINES;
	/* Dot clock in 10 kHz units (EDID stores it that way). */
	unsigned long clock10k = ((unsigned long) htotal * vtotal * hz) / 10000ul;

	if (clock10k > 0xffff) {
		clock10k = 0xffff;
	}

	memset(d, 0, 18);

	d[0] = (uint8_t) (clock10k & 0xff);
	d[1] = (uint8_t) ((clock10k >> 8) & 0xff);

	d[2] = (uint8_t) (x & 0xff);
	d[3] = (uint8_t) (HBLANK_PIXELS & 0xff);
	d[4] = (uint8_t) (((x >> 8) << 4) | ((HBLANK_PIXELS >> 8) & 0x0f));

	d[5] = (uint8_t) (y & 0xff);
	d[6] = (uint8_t) (VBLANK_LINES & 0xff);
	d[7] = (uint8_t) (((y >> 8) << 4) | ((VBLANK_LINES >> 8) & 0x0f));

	d[8] = (uint8_t) (HFRONT_PIXELS & 0xff);
	d[9] = (uint8_t) (HSYNC_PIXELS & 0xff);
	d[10] = (uint8_t) (((VFRONT_LINES & 0x0f) << 4) | (VSYNC_LINES & 0x0f));
	d[11] = 0;	/* all porch high bits zero for these values */

	/* d[12..16] image size / border left at 0. */
	d[17] = 0x1e;	/* separate sync, H+/V+ */
}

/* Encode one 2-byte standard-timing identifier (or 0x0101 for "unused"). */
static void
set_standard_timing(uint8_t *p, unsigned x, unsigned aspect, unsigned hz)
{
	if (x < 256 || x > 2288 || hz < 60) {
		p[0] = 0x01;
		p[1] = 0x01;
		return;
	}
	p[0] = (uint8_t) ((x / 8) - 31);
	p[1] = (uint8_t) (((aspect & 0x03) << 6) | ((hz - 60) & 0x3f));
}

/* Modes no timing bitmap can express, largest first.
   DMT has no entry for any of them, and a standard timing cannot describe them
   either: that field stores (width / 8) - 31 in a byte, so it stops at 2288
   pixels wide. A detailed timing descriptor is the only way to declare one, and
   there are exactly two spare descriptor slots, so at most the largest two that
   fit are advertised. Without this the mode chooser offers 2560x1440 and RISC OS
   refuses it, which is the 1440p half of the display reports. */
static const struct { unsigned x, y; } detail_only_modes[] = {
	{ 3840, 2160 },
	{ 3440, 1440 },
	{ 2560, 1440 },
};

void
edid_build_from_base(uint8_t out[EDID_BLOCK_SIZE],
                     const uint8_t base[EDID_BLOCK_SIZE],
                     unsigned x, unsigned y,
                     unsigned max_x, unsigned max_y, unsigned hz)
{
	unsigned sum = 0;
	int i;

	/* The ceiling can never be below the preferred mode: the monitor plainly
	   shows the mode it says is native, and a caller that passes a smaller
	   ceiling means "no ceiling worth applying" rather than "hide the native
	   mode". */
	if (max_x < x) {
		max_x = x;
	}
	if (max_y < y) {
		max_y = y;
	}

	memcpy(out, base, EDID_BLOCK_SIZE);

	/* Declare EDID 1.3 and flag that the first detailed timing is the
	   monitor's native/preferred mode. */
	out[19] = 3;			/* revision -> 1.3 */
	out[24] |= 0x02;		/* feature byte: preferred timing is native */

	/* Established timings: the legacy bitmap in the base block, filled in with
	   every one of its modes this machine can display rather than the four that
	   used to be hard-coded here. These are the small modes - 640x480 through
	   1152x870 - and they cost three bytes that are in the block regardless. */
	out[0x23] = 0;
	out[0x24] = 0;
	out[0x25] = 0;
	set_timing_bits(&out[0x23], 3, established_timings,
	                (unsigned) (sizeof(established_timings) /
	                            sizeof(established_timings[0])), max_x, max_y);

	/* Standard timings: a ladder of widescreen/legacy modes.
	 *
	 * The Established Timings III descriptor below now carries the bulk of the
	 * list, and says most of these as well. They are kept because they cost
	 * nothing - the eight slots are in the block whether used or not - and
	 * because they are the older mechanism: a ScreenModes that does not read the
	 * 0xF7 descriptor still finds a usable ladder here.
	 *
	 * This is also the only place 1920x1080 is said, DMT having no entry for it,
	 * so the ladder is not purely redundant.
	 *
	 * Note a width appears more than once with different aspects: the height is
	 * derived from the aspect code, so 1280 gives 1024 at 5:4, 960 at 4:3 and 720
	 * at 16:9.
	 *
	 * Only the modes this machine can display are written, on the same test the
	 * bitmaps use. The ladder used to be written out whole regardless, which
	 * offered a 1080-tall display 1600x1200 and a 1024x768 one 1280x1024 - modes
	 * the chooser would list and RISC OS would then refuse. Entries left over
	 * are filled with the "unused" code. */
	{
		static const struct {
			unsigned x, y, aspect;
		} ladder[] = {
			{ 1280, 1024, ASPECT_5_4 },
			{ 1440,  900, ASPECT_16_10 },
			{ 1600, 1200, ASPECT_4_3 },
			{ 1680, 1050, ASPECT_16_10 },
			{ 1920, 1080, ASPECT_16_9 },
			{ 1152,  864, ASPECT_4_3 },
			{ 1280,  960, ASPECT_4_3 },
			{ 1280,  720, ASPECT_16_9 },
		};
		const unsigned slots = 8;
		unsigned slot = 0, e;

		for (e = 0; e < sizeof(ladder) / sizeof(ladder[0]) && slot < slots; e++) {
			if (ladder[e].x > max_x || ladder[e].y > max_y) {
				continue;
			}
			set_standard_timing(&out[0x26 + slot * 2], ladder[e].x,
			                    ladder[e].aspect, 60);
			slot++;
		}
		for (; slot < slots; slot++) {
			out[0x26 + slot * 2] = 0x01;
			out[0x26 + slot * 2 + 1] = 0x01;
		}
	}

	/* First detailed timing descriptor = the preferred (native) mode. */
	build_detailed_timing(&out[DESC_PREFERRED], x, y, hz);

	/* Second descriptor = Established Timings III, the bitmap that carries the
	   rest of the mode list. Every ROM block seen has all four descriptor slots
	   filled with dummies (tag 0x10, all zero), so this displaces nothing.
	   Descriptors 3 and 4 are still inherited unchanged. */
	{
		uint8_t *d = &out[DESC_SECOND];

		memset(d, 0, 18);
		d[3] = ET3_TAG;
		/* d[4] stays zero: ScreenModes' get_extd_type() treats a non-zero
		   byte there as an undefined descriptor and skips the whole slot. */
		d[5] = ET3_REVISION;
		set_timing_bits(&d[ET3_BITMAP_OFFSET], ET3_BITMAP_BYTES,
		                established_timings3,
		                (unsigned) (sizeof(established_timings3) /
		                            sizeof(established_timings3[0])),
		                max_x, max_y);
	}

	/* Descriptors 3 and 4 = the largest modes that fit the ceiling but which no
	   bitmap can express. Both slots are dummies (tag 0x10, all zero) in every
	   ROM block seen, so this displaces nothing, and a slot is left inherited
	   when there is nothing to put in it rather than being blanked. The
	   preferred mode is skipped: it already has descriptor 1. */
	{
		const unsigned slot_at[2] = { DESC_THIRD, DESC_FOURTH };
		unsigned used = 0;
		size_t m;

		for (m = 0; m < sizeof(detail_only_modes) /
		                sizeof(detail_only_modes[0]) && used < 2; m++) {
			const unsigned dx = detail_only_modes[m].x;
			const unsigned dy = detail_only_modes[m].y;

			if (dx > max_x || dy > max_y) {
				continue;
			}
			if (dx == x && dy == y) {
				continue;	/* already the preferred timing */
			}
			build_detailed_timing(&out[slot_at[used]], dx, dy, hz);
			used++;
		}
	}

	/* Recompute the checksum so the whole block sums to a multiple of 256.
	   The driver's runtime sync-bit fixup adds to byte 0x14 and subtracts the
	   same amount from the checksum, so a base-block checksum stays valid. */
	out[0x7f] = 0;
	for (i = 0; i < EDID_BLOCK_SIZE - 1; i++) {
		sum += out[i];
	}
	out[0x7f] = (uint8_t) ((256 - (sum & 0xff)) & 0xff);
}

/*
 * Read a block back and say whether it declares one particular mode.
 *
 * Decoding rather than remembering: the question "will RISC OS accept this
 * size?" has to be answered from the block the guest is actually reading, or a
 * caller ends up re-implementing the builder's rules and the two drift. The
 * established bitmap and the standard-timing ladder are read straight out of
 * the base block's fixed offsets; the other two mechanisms live in descriptors,
 * which have to be walked because their order is not fixed.
 */
static void
detailed_timing_size(const uint8_t d[18], unsigned *x, unsigned *y)
{
	*x = (unsigned) d[2] | (((unsigned) d[4] & 0xf0u) << 4);
	*y = (unsigned) d[5] | (((unsigned) d[7] & 0xf0u) << 4);
}

int
edid_block_declares(const uint8_t block[EDID_BLOCK_SIZE],
                    unsigned width, unsigned height)
{
	/* The seventeen established-bitmap modes, at their fixed bit positions. */
	static const struct { unsigned w, h, byte, bit; } established[] = {
		{  720, 400, 0x23, 7 }, {  640, 480, 0x23, 5 },
		{  640, 480, 0x23, 3 }, {  640, 480, 0x23, 2 },
		{  800, 600, 0x23, 1 }, {  800, 600, 0x23, 0 },
		{  800, 600, 0x24, 7 }, {  800, 600, 0x24, 6 },
		{  832, 624, 0x24, 5 }, { 1024, 768, 0x24, 3 },
		{ 1024, 768, 0x24, 2 }, { 1024, 768, 0x24, 1 },
		{ 1280, 1024, 0x24, 0 }, { 1152, 870, 0x25, 7 },
	};
	/* A standard timing stores the width and an aspect code; the height is
	   derived, so it is never in the block. */
	static const unsigned aspect_num[4] = { 16, 4, 5, 16 };
	static const unsigned aspect_den[4] = { 10, 3, 4,  9 };
	unsigned i;

	if (block == NULL || width == 0 || height == 0) {
		return 0;
	}

	for (i = 0; i < sizeof(established) / sizeof(established[0]); i++) {
		if (established[i].w == width && established[i].h == height &&
		    (block[established[i].byte] &
		     (1u << established[i].bit)) != 0) {
			return 1;
		}
	}

	for (i = 0x26; i < 0x36; i += 2) {
		unsigned w, h, aspect;

		if (block[i] == 0x01 && block[i + 1] == 0x01) {
			continue;	/* unused slot */
		}
		w = ((unsigned) block[i] + 31u) * 8u;
		aspect = ((unsigned) block[i + 1] >> 6) & 0x03u;
		h = w * aspect_den[aspect] / aspect_num[aspect];

		if (w == width && h == height) {
			return 1;
		}
	}

	for (i = 0; i < 4; i++) {
		const uint8_t *d = block + DESC_PREFERRED + i * 18;

		/* A descriptor with a zero pixel clock is a display descriptor;
		   byte 3 is then its tag. Anything else is a detailed timing. */
		if (d[0] != 0 || d[1] != 0) {
			unsigned dx = 0, dy = 0;

			detailed_timing_size(d, &dx, &dy);
			if (dx == width && dy == height) {
				return 1;
			}
			continue;
		}

		if (d[3] == ET3_TAG && d[4] == 0) {
			const uint8_t *bitmap = d + ET3_BITMAP_OFFSET;
			unsigned e;

			for (e = 0; e < sizeof(established_timings3) /
			                sizeof(established_timings3[0]); e++) {
				if (established_timings3[e].x == width &&
				    established_timings3[e].y == height &&
				    established_timings3[e].byte < ET3_BITMAP_BYTES &&
				    (bitmap[established_timings3[e].byte] &
				     (1u << established_timings3[e].bit)) != 0) {
					return 1;
				}
			}
		}
	}

	return 0;
}

/* The block in force, for anything that must answer for the same monitor. Kept
   here rather than in either caller, because both the ROM patch that installs it
   and the graphics card that serves it over DDC are downstream of this file. */
static uint8_t edid_in_force[EDID_BLOCK_SIZE];
static int edid_in_force_valid;

void
edid_publish(const uint8_t block[EDID_BLOCK_SIZE])
{
	memcpy(edid_in_force, block, EDID_BLOCK_SIZE);
	edid_in_force_valid = 1;
}

const uint8_t *
edid_published(void)
{
	return edid_in_force_valid ? edid_in_force : NULL;
}
