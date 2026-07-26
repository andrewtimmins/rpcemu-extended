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
   The largest entry also sets the ceiling for everything that calls here. */
static const struct {
	unsigned width;
	unsigned height;
} display_modes[] = {
	{ 2560, 1440 },	/* 3686400 */
	{ 1920, 1200 },	/* 2304000 */
	{ 1920, 1080 },	/* 2073600 */
	{ 1600, 1200 },	/* 1920000 */
	{ 1680, 1050 },	/* 1764000 */
	{ 1400, 1050 },	/* 1470000 */
	{ 1280, 1024 },	/* 1310720 */
	{ 1440,  900 },	/* 1296000 */
	{ 1280,  960 },	/* 1228800 */
	{ 1366,  768 },	/* 1049088 */
	{ 1152,  864 },	/*  995328 */
	{ 1280,  720 },	/*  921600 */
	{ 1024,  768 },	/*  786432 */
	{  800,  600 },	/*  480000 */
	{  640,  480 },	/*  307200 */
};

int
display_mode_fit(unsigned max_width, unsigned max_height,
                 unsigned bytes_per_pixel, size_t budget_bytes,
                 unsigned *width, unsigned *height)
{
	size_t i;

	for (i = 0; i < sizeof(display_modes) / sizeof(display_modes[0]); i++) {
		const unsigned w = display_modes[i].width;
		const unsigned h = display_modes[i].height;

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
