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

/* See text_screen.h. */

#include <string.h>

#include "console_font.h"
#include "text_screen.h"

/* Everything clips. A machine name can be any length the user likes and a VNC
   client can be any size, so nothing here may assume it fits. */

void
text_screen_clear(TextScreen *s, uint32_t colour)
{
	text_screen_fill(s, 0, 0, s->width, s->height, colour);
}

void
text_screen_fill(TextScreen *s, int x, int y, int w, int h, uint32_t colour)
{
	int py;

	if (s->pixels == NULL) {
		return;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s->width)  { w = s->width - x; }
	if (y + h > s->height) { h = s->height - y; }
	if (w <= 0 || h <= 0) {
		return;
	}

	for (py = y; py < y + h; py++) {
		uint32_t *row = s->pixels + (size_t) py * (size_t) s->stride + x;
		int px;

		for (px = 0; px < w; px++) {
			row[px] = colour;
		}
	}
}

/**
 * One glyph, optionally over a background.
 *
 * @param bg_used 0 to leave the pixels behind the glyph alone.
 */
static void
draw_glyph(TextScreen *s, int x, int y, unsigned char ch, uint32_t fg,
           uint32_t bg, int bg_used)
{
	const uint8_t *glyph;
	int row;

	/* Outside the table becomes a full stop: a name with an accent or a tab in
	   it should look odd, not read past the end of the font. */
	if (ch < CONSOLE_FONT_FIRST || ch > CONSOLE_FONT_LAST) {
		ch = '.';
	}
	glyph = console_font_8x16[ch - CONSOLE_FONT_FIRST];

	for (row = 0; row < CONSOLE_FONT_HEIGHT; row++) {
		const int py = y + row;
		uint32_t *line;
		int col;

		if (py < 0 || py >= s->height) {
			continue;
		}
		line = s->pixels + (size_t) py * (size_t) s->stride;

		for (col = 0; col < CONSOLE_FONT_WIDTH; col++) {
			const int px = x + col;

			if (px < 0 || px >= s->width) {
				continue;
			}
			if (glyph[row] & (0x80u >> col)) {
				line[px] = fg;
			} else if (bg_used) {
				line[px] = bg;
			}
		}
	}
}

static int
draw_string(TextScreen *s, int x, int y, const char *text, uint32_t fg,
            uint32_t bg, int bg_used)
{
	const unsigned char *p;

	if (s->pixels == NULL || text == NULL) {
		return x;
	}
	for (p = (const unsigned char *) text; *p != '\0'; p++) {
		draw_glyph(s, x, y, *p, fg, bg, bg_used);
		x += CONSOLE_FONT_WIDTH;
	}
	return x;
}

int
text_screen_string(TextScreen *s, int x, int y, const char *text, uint32_t fg)
{
	return draw_string(s, x, y, text, fg, 0, 0);
}

int
text_screen_string_bg(TextScreen *s, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg)
{
	return draw_string(s, x, y, text, fg, bg, 1);
}

void
text_screen_centre(TextScreen *s, int y, const char *text, uint32_t fg)
{
	int w;

	if (text == NULL) {
		return;
	}
	w = (int) strlen(text) * CONSOLE_FONT_WIDTH;
	text_screen_string(s, (s->width - w) / 2, y, text, fg);
}

int
text_screen_columns(const TextScreen *s)
{
	return s->width / CONSOLE_FONT_WIDTH;
}

int
text_screen_rows(const TextScreen *s)
{
	return s->height / CONSOLE_FONT_HEIGHT;
}
