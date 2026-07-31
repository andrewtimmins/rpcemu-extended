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

/* See vdu_filter.h for why this exists. */

#include "vdu_filter.h"

/*
 * How many parameter bytes each VDU code consumes.
 *
 * These are the VDU queue lengths RISC OS itself uses, and getting them right is
 * the whole point of doing this properly: VDU 19 is followed by five bytes of
 * palette, any of which can be 10 or 13. A filter that dropped the 19 and then
 * treated a parameter as a line ending would split a line in half for reasons
 * nobody could see.
 *
 * 10 (line feed) and 13 (carriage return) are handled by the newline rules
 * instead, so their entries here are never consulted.
 */
static const unsigned char vdu_params[32] = {
	0, /*  0 null                        */
	1, /*  1 next byte to printer only   */
	0, /*  2 printer on                  */
	0, /*  3 printer off                 */
	0, /*  4 text at text cursor         */
	0, /*  5 text at graphics cursor     */
	0, /*  6 enable VDU                  */
	0, /*  7 bell                        */
	0, /*  8 cursor left                 */
	0, /*  9 cursor right                */
	0, /* 10 line feed                   */
	0, /* 11 cursor up                   */
	0, /* 12 clear text window           */
	0, /* 13 carriage return             */
	0, /* 14 paged mode on               */
	0, /* 15 paged mode off              */
	0, /* 16 clear graphics window       */
	1, /* 17 set text colour             */
	2, /* 18 set graphics colour         */
	5, /* 19 set palette                 */
	0, /* 20 restore default palette     */
	0, /* 21 disable VDU                 */
	1, /* 22 select screen mode          */
	9, /* 23 misc / define character     */
	8, /* 24 define graphics window      */
	5, /* 25 PLOT                        */
	0, /* 26 restore default windows     */
	0, /* 27 next byte to VDU            */
	4, /* 28 define text window          */
	4, /* 29 set graphics origin         */
	0, /* 30 home cursor                 */
	2, /* 31 tab to x,y                  */
};

void
vdu_filter_init(VduFilter *f)
{
	f->params = 0;
	f->prev = 0;
}

size_t
vdu_filter(VduFilter *f, const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i, w = 0;

	for (i = 0; i < n; i++) {
		const uint8_t b = in[i];

		/* Mid-sequence: swallow the parameter, whatever it happens to be. */
		if (f->params > 0) {
			f->params--;
			continue;
		}

		/*
		 * Newlines. RISC OS writes LF then CR, other things write CR then LF,
		 * and a few write one alone, so all four cases collapse to a single
		 * newline. A carriage return is held back until the next byte is known,
		 * because only then is it clear whether it ends a line by itself or is
		 * the tail of a LF CR pair.
		 */
		if (b == '\n') {
			if (f->prev == '\r') {
				f->prev = 0;	/* CR LF: the newline is already out */
			} else {
				out[w++] = '\n';
				f->prev = '\n';
			}
			continue;
		}
		if (b == '\r') {
			if (f->prev == '\n') {
				f->prev = 0;	/* LF CR: the newline is already out */
			} else {
				out[w++] = '\n';
				f->prev = '\r';
			}
			continue;
		}

		f->prev = 0;

		/* Any other control code, with its parameters. */
		if (b < 32) {
			f->params = vdu_params[b];
			continue;
		}

		out[w++] = b;
	}

	return w;
}

size_t
vdu_filter_finish(VduFilter *f, uint8_t *out)
{
	(void) f;
	(void) out;

	/* Nothing is outstanding: a carriage return emits its newline immediately
	   and only records that it did, so a stream ending on one is already
	   complete. The function exists so callers need not know that, and so the
	   rule can change without touching them. */
	return 0;
}
