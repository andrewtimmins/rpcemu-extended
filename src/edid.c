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

void
edid_build_from_base(uint8_t out[EDID_BLOCK_SIZE],
                     const uint8_t base[EDID_BLOCK_SIZE],
                     unsigned x, unsigned y, unsigned hz)
{
	unsigned sum = 0;
	int i;

	memcpy(out, base, EDID_BLOCK_SIZE);

	/* Declare EDID 1.3 and flag that the first detailed timing is the
	   monitor's native/preferred mode. */
	out[19] = 3;			/* revision -> 1.3 */
	out[24] |= 0x02;		/* feature byte: preferred timing is native */

	/* Established timings: 640x480@60, 800x600@60, 1024x768@60, 1280x1024@75. */
	out[0x23] = 0x21;
	out[0x24] = 0x09;
	out[0x25] = 0x00;

	/* Standard timings: a ladder of widescreen/legacy modes.
	 *
	 * All eight slots are used, and deliberately: between these and the
	 * established timings above, every mode display_mode.c can choose is
	 * advertised here except the two largest. That matters because the guest
	 * validates a mode against the monitor definition in force, so a mode we
	 * might ask it to adopt later has to be one this block declares - not just
	 * the preferred timing, which only covers whichever mode was chosen at boot.
	 *
	 * The exceptions are 2560x1440 and 1920x1200, which appear only when they are
	 * the preferred timing. 2560x1440 cannot be expressed as a standard timing at
	 * all (the width field stops at 2288), and there is no ninth slot for
	 * 1920x1200. A host display that grows to one of those mid-session therefore
	 * cannot be followed; that needs the monitor definition reloading, which is a
	 * bigger job than this.
	 *
	 * Note a width appears more than once with different aspects: the height is
	 * derived from the aspect code, so 1280 gives 1024 at 5:4, 960 at 4:3 and 720
	 * at 16:9. */
	set_standard_timing(&out[0x26], 1280, ASPECT_5_4,  60);	/* 1280x1024 */
	set_standard_timing(&out[0x28], 1440, ASPECT_16_10, 60);	/* 1440x900  */
	set_standard_timing(&out[0x2a], 1600, ASPECT_4_3,  60);	/* 1600x1200 */
	set_standard_timing(&out[0x2c], 1680, ASPECT_16_10, 60);	/* 1680x1050 */
	set_standard_timing(&out[0x2e], 1920, ASPECT_16_9,  60);	/* 1920x1080 */
	set_standard_timing(&out[0x30], 1152, ASPECT_4_3,  60);	/* 1152x864  */
	set_standard_timing(&out[0x32], 1280, ASPECT_4_3,  60);	/* 1280x960  */
	set_standard_timing(&out[0x34], 1280, ASPECT_16_9,  60);	/* 1280x720  */

	/* First detailed timing descriptor = the preferred (native) mode. */
	build_detailed_timing(&out[0x36], x, y, hz);

	/* Descriptors 2-4 (0x48..0x7d) are inherited unchanged from the base
	   block (harmless dummy monitor descriptors). */

	/* Recompute the checksum so the whole block sums to a multiple of 256.
	   The driver's runtime sync-bit fixup adds to byte 0x14 and subtracts the
	   same amount from the checksum, so a base-block checksum stays valid. */
	out[0x7f] = 0;
	for (i = 0; i < EDID_BLOCK_SIZE - 1; i++) {
		sum += out[i];
	}
	out[0x7f] = (uint8_t) ((256 - (sum & 0xff)) & 0xff);
}
