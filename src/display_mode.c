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
 * display_mode.c - choosing a display mode the machine can actually show
 *
 * Deliberately self-contained: no emulator state, no configuration, nothing to
 * initialise. Both the synthesised monitor EDID and the mode offered to the
 * guest at run time go through here, so the two always agree, and it can be
 * tested without linking the emulator core.
 */

#include "display_mode.h"

/* Standard display modes, largest area first. These are sizes RISC OS and
   monitors both expect, rather than arbitrary geometry: a computed 1385x779
   would be a strange thing to advertise and less likely to be accepted.
   The largest entry also sets the ceiling for everything that calls here.
   Areas are in the comments, since the order is what makes "largest that fits"
   work and it is not obvious by eye that 1920x1080 beats 1600x1200.

   This list is kept in step with what edid.c advertises, because a guest
   validates a mode against the monitor definition in force: a mode we may ask it
   to adopt later has to be one the EDID declares. Everything here is an
   established or standard timing in that block, except the top two - those
   appear only when chosen at boot, as the preferred timing.

   1400x1050 and 1366x768 were dropped for that reason. There is no spare
   standard-timing slot for the first, and the second cannot be a standard timing
   at all: the width field counts in units of 8 and 1366 is not a multiple of 8.
   Their rungs are covered by the neighbours either side.

   The list runs above 2560 wide even though no framestore we currently offer can
   hold such a mode, because the top entry is a ceiling on everything that calls
   here and a ceiling is the wrong place to record a memory limit. The budget
   argument already refuses a mode that will not fit, so on today's 15MB card
   these two are simply never chosen; on a 4K or ultrawide host they stop the
   answer being silently truncated to 1440p the moment there is room for more.
   Both are inside what a preferred timing can describe: build_detailed_timing()
   carries 12 bits of horizontal active, so up to 4095, and clamps the dot clock
   rather than overflowing it. */
static const struct {
	unsigned width;
	unsigned height;
} display_modes[] = {
	{ 3840, 2160 },	/* 8294400 - preferred timing only */
	{ 3440, 1440 },	/* 4953600 - preferred timing only */
	{ 2560, 1440 },	/* 3686400 - preferred timing only */
	{ 1920, 1200 },	/* 2304000 - preferred timing only */
	{ 1920, 1080 },	/* 2073600 */
	{ 1600, 1200 },	/* 1920000 */
	{ 1680, 1050 },	/* 1764000 */
	{ 1280, 1024 },	/* 1310720 */
	{ 1440,  900 },	/* 1296000 */
	{ 1280,  960 },	/* 1228800 */
	{ 1152,  864 },	/*  995328 */
	{ 1280,  720 },	/*  921600 */
	{ 1024,  768 },	/*  786432 */
	{  800,  600 },	/*  480000 */
	{  640,  480 },	/*  307200 */
};

/* One flag per entry above: set when the guest has refused that mode. Static
   storage, so every mode starts available. */
static unsigned char mode_unavailable[sizeof(display_modes) / sizeof(display_modes[0])];

static size_t
mode_index(unsigned width, unsigned height)
{
	size_t i;

	for (i = 0; i < sizeof(display_modes) / sizeof(display_modes[0]); i++) {
		if (display_modes[i].width == width &&
		    display_modes[i].height == height)
		{
			return i;
		}
	}

	return (size_t) -1;
}

void
display_mode_mark_unavailable(unsigned width, unsigned height)
{
	const size_t i = mode_index(width, height);

	if (i != (size_t) -1) {
		mode_unavailable[i] = 1;
	}
}

int
display_mode_is_unavailable(unsigned width, unsigned height)
{
	const size_t i = mode_index(width, height);

	return i != (size_t) -1 && mode_unavailable[i] != 0;
}

void
display_mode_clear_unavailable(void)
{
	size_t i;

	for (i = 0; i < sizeof(mode_unavailable) / sizeof(mode_unavailable[0]); i++) {
		mode_unavailable[i] = 0;
	}
}

int
display_mode_fit(unsigned max_width, unsigned max_height,
                 unsigned bytes_per_pixel, size_t budget_bytes,
                 unsigned *width, unsigned *height)
{
	size_t i;

	for (i = 0; i < sizeof(display_modes) / sizeof(display_modes[0]); i++) {
		const unsigned w = display_modes[i].width;
		const unsigned h = display_modes[i].height;

		if (mode_unavailable[i]) {
			continue;	/* The guest has already refused this one */
		}
		if (w > max_width || h > max_height) {
			continue;
		}
		if (budget_bytes != 0 && bytes_per_pixel != 0) {
			const size_t needed = (size_t) w * h * bytes_per_pixel;

			if (needed > budget_bytes) {
				continue;
			}
		}

		*width = w;
		*height = h;
		return 1;
	}

	return 0;
}

size_t
display_mode_count(void)
{
	return sizeof(display_modes) / sizeof(display_modes[0]);
}

int
display_mode_get(size_t index, unsigned *width, unsigned *height)
{
	if (index >= display_mode_count()) {
		return 0;
	}

	*width = display_modes[index].width;
	*height = display_modes[index].height;
	return 1;
}
