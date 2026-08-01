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
 * text_screen - draw text into a 32-bit framebuffer.
 *
 * The smallest thing that lets the emulator put words on a screen it owns. It
 * exists for what has to be shown when there is no machine yet and so nothing
 * else to draw: a VNC client connecting to a headless emulator that has not been
 * told which machine to run.
 *
 * Deliberately not a widget toolkit. Clear, fill, draw a string, draw a string
 * centred. Everything above that - a list, a selection, key handling - is the
 * caller's business, which keeps this file trivially testable and means the
 * pixel format is the only thing it knows about the world.
 *
 * Pixels are 0x00RRGGBB in a row-major buffer of `stride` pixels per row, which
 * is what the VNC server's framebuffer already is.
 */

#ifndef TEXT_SCREEN_H
#define TEXT_SCREEN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t *pixels;
	int width;
	int height;
	int stride;	/**< pixels per row, usually equal to width */
	int scale;	/**< glyph magnification; 0 or 1 both mean unscaled */
} TextScreen;

#ifdef __cplusplus
extern "C" {
#endif

/** Fill the whole screen with one colour. */
extern void text_screen_clear(TextScreen *s, uint32_t colour);

/** Fill a rectangle, clipped to the screen. */
extern void text_screen_fill(TextScreen *s, int x, int y, int w, int h,
                             uint32_t colour);

/**
 * Draw a string at a pixel position, clipped to the screen.
 *
 * Characters outside the font's range are drawn as a full stop rather than
 * indexing past the table, so a machine name with an accent in it cannot walk off
 * the end of the font.
 *
 * @return the x position one pixel past the last glyph drawn.
 */
extern int text_screen_string(TextScreen *s, int x, int y, const char *text,
                              uint32_t fg);

/** As text_screen_string(), with a background painted behind the glyphs. */
extern int text_screen_string_bg(TextScreen *s, int x, int y, const char *text,
                                 uint32_t fg, uint32_t bg);

/** Draw a string horizontally centred on the screen. */
extern void text_screen_centre(TextScreen *s, int y, const char *text,
                               uint32_t fg);

/**
 * Set the scale glyphs are drawn at, in whole pixels; 1 is the raw 8x16 cell.
 *
 * A fixed cell that reads well at 640x480 is lost on a 1920x1080 client, and the
 * guest chooses the resolution, so anything the emulator draws over it has to
 * cope with both. Whole numbers only: a bitmap font scaled by a fraction looks
 * like a mistake.
 */
extern void text_screen_set_scale(TextScreen *s, int scale);

/** A sensible scale for a screen of this size. */
extern int text_screen_auto_scale(const TextScreen *s);

/** Columns and rows the screen can hold, for a caller laying out a list. */
extern int text_screen_columns(const TextScreen *s);
extern int text_screen_rows(const TextScreen *s);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_SCREEN_H */
