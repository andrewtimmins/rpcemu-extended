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

#ifndef REMOTE_DISPLAY_SCALE_H
#define REMOTE_DISPLAY_SCALE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A machine's 0x00RRGGBB framebuffer, scaled down and written out as the RGB24
 * triples wxImage wants, in one pass.
 *
 * Why this is here rather than left to the toolkit. The Manager scales a
 * machine's screen to whatever size its panel is, and on a platform whose
 * graphics context is hardware accelerated - Direct2D on Windows - the toolkit
 * is the right answer and is what remote_emulator_panel.cpp uses. Where it is
 * not accelerated the toolkit is the most expensive option there is. Measured on
 * GTK/Cairo, 1600x1200 into a 933x700 panel, one core:
 *
 *     wxGraphicsContext::DrawBitmap, BEST     30.5 ms
 *     wxImage::Scale, HIGH                    13.2 ms
 *     this                                     7.9 ms
 *     StretchBlit (nearest neighbour)          0.7 ms
 *
 * and this replaces the separate copy-and-convert passes rather than adding to
 * them, where the others need the whole frame converted to RGB24 first. Cairo is
 * quick at 1:1 and at exactly 1:2 and slow at every ratio in between, which is
 * every size a window actually gets dragged to; at 30ms a frame the Manager
 * could not exceed about 30fps with a core saturated, and mouse motion is
 * handled on the same thread, so the pointer lagged the hand as well.
 *
 * A box average: each source pixel contributes to exactly one destination pixel,
 * and each destination pixel is the mean of the source pixels that fall in it.
 * That is the right filter for a downscale, and unlike a fixed four-tap bilinear
 * it does not thin single-pixel text as the scale factor grows.
 *
 * Row-oriented deliberately: when the frame event grows a dirty rectangle, only
 * the destination rows it covers need doing.
 *
 * Scaling UP is not this function's job - a box average has nothing to average
 * when a destination pixel covers less than one source pixel. The caller sends
 * that case elsewhere.
 */
/*
 * The destination rows that a change to source rows [src_top, src_bottom)
 * affects, as a half-open range.
 *
 * The bottom rounds up: a source row that contributes any part of a destination
 * row means that destination row has to be recomputed, and rounding it down left
 * a one-pixel line of the previous frame behind at the bottom edge of everything
 * that moved.
 */
static inline void
remote_display_scale_rows_for(int src_top, int src_bottom, int src_height,
                              int dst_height, int *dst_top, int *dst_bottom)
{
	int top, bottom;

	if (src_height <= 0 || dst_height <= 0) {
		*dst_top = 0;
		*dst_bottom = 0;
		return;
	}

	if (src_top < 0) {
		src_top = 0;
	}
	if (src_bottom > src_height) {
		src_bottom = src_height;
	}
	if (src_bottom <= src_top) {
		*dst_top = 0;
		*dst_bottom = 0;
		return;
	}

	top = src_top * dst_height / src_height;
	bottom = (src_bottom * dst_height + src_height - 1) / src_height;

	if (bottom > dst_height) {
		bottom = dst_height;
	}
	if (top > bottom) {
		top = bottom;
	}

	*dst_top = top;
	*dst_bottom = bottom;
}

/*
 * As remote_display_scale_argb(), but only destination rows
 * [dst_row_top, dst_row_bottom).
 *
 * The rest of the destination is left exactly as it was, which is the whole
 * point: a guest that redrew one band of its screen costs one band of work here
 * rather than a whole screen.
 */
static inline int
remote_display_scale_argb_rows(const uint32_t *src, int src_width, int src_height,
                               unsigned char *dst, int dst_width, int dst_height,
                               int dst_row_top, int dst_row_bottom)
{
	uint32_t *accumulator;
	uint32_t *count;
	int *column;
	int sx, dy;

	if (src == NULL || dst == NULL || src_width <= 0 || src_height <= 0 ||
	    dst_width <= 0 || dst_height <= 0) {
		return 0;
	}

	if (dst_row_top < 0) {
		dst_row_top = 0;
	}
	if (dst_row_bottom > dst_height) {
		dst_row_bottom = dst_height;
	}
	if (dst_row_bottom <= dst_row_top) {
		return 1;
	}

	/* Allocated here rather than passed in. Three small allocations per frame
	   are nothing beside the pass itself, and a caller holding buffers it has
	   to keep the right size is a buffer overrun waiting for the first guest
	   that changes screen mode. */
	accumulator = (uint32_t *) malloc((size_t) dst_width * 3 * sizeof(*accumulator));
	count = (uint32_t *) malloc((size_t) dst_width * sizeof(*count));
	column = (int *) malloc((size_t) src_width * sizeof(*column));

	if (accumulator == NULL || count == NULL || column == NULL) {
		free(accumulator);
		free(count);
		free(column);
		return 0;
	}

	/* Which destination column each source column lands in. Worked out once:
	   it depends on nothing that changes as the rows are walked. */
	for (sx = 0; sx < src_width; sx++) {
		const int c = sx * dst_width / src_width;

		column[sx] = c < dst_width ? c : dst_width - 1;
	}

	for (dy = dst_row_top; dy < dst_row_bottom; dy++) {
		int y0 = dy * src_height / dst_height;
		int y1 = (dy + 1) * src_height / dst_height;
		int sy, dx;
		unsigned char *out;

		/* Every destination row has to average at least one source row: with
		   the two heights close together the arithmetic above can otherwise
		   produce an empty range, and that row would come out black. */
		if (y1 <= y0) {
			y1 = y0 + 1;
		}

		memset(accumulator, 0, (size_t) dst_width * 3 * sizeof(*accumulator));
		memset(count, 0, (size_t) dst_width * sizeof(*count));

		for (sy = y0; sy < y1 && sy < src_height; sy++) {
			const uint32_t *row = src + (size_t) sy * (size_t) src_width;
			int sx;

			for (sx = 0; sx < src_width; sx++) {
				const uint32_t pixel = row[sx];
				const int c = column[sx];

				accumulator[3 * (size_t) c + 0] += (pixel >> 16) & 0xff;
				accumulator[3 * (size_t) c + 1] += (pixel >> 8) & 0xff;
				accumulator[3 * (size_t) c + 2] += pixel & 0xff;
				count[c]++;
			}
		}

		out = dst + (size_t) dy * (size_t) dst_width * 3;

		for (dx = 0; dx < dst_width; dx++) {
			const uint32_t n = count[dx] != 0 ? count[dx] : 1;

			out[3 * dx + 0] = (unsigned char) (accumulator[3 * (size_t) dx + 0] / n);
			out[3 * dx + 1] = (unsigned char) (accumulator[3 * (size_t) dx + 1] / n);
			out[3 * dx + 2] = (unsigned char) (accumulator[3 * (size_t) dx + 2] / n);
		}
	}

	free(accumulator);
	free(count);
	free(column);

	return 1;
}

/* The whole destination, which is what a new screen size or a first frame
   wants. */
static inline int
remote_display_scale_argb(const uint32_t *src, int src_width, int src_height,
                          unsigned char *dst, int dst_width, int dst_height)
{
	return remote_display_scale_argb_rows(src, src_width, src_height,
	    dst, dst_width, dst_height, 0, dst_height);
}

/*
 * The cheap one: nearest neighbour, one source pixel read per destination pixel.
 *
 * For use while the picture is moving. The box average above reads every source
 * pixel, so it costs what the guest's screen costs - 15ms for 1920x1080,
 * measured, which at sixty frames a second is most of a GUI thread and left the
 * pointer being updated about thirty times a second because pointer events are
 * handled on that same thread. This reads only what it puts on screen, so it
 * costs what the panel costs instead, and the panel is smaller. It is grainier -
 * that is the trade - so the caller uses it while the user is moving things and
 * the box average once they stop.
 *
 * Also the right filter when the guest is being scaled UP: a box average has
 * nothing to average when a destination pixel covers less than one source pixel.
 */
static inline int
remote_display_scale_argb_nearest_rows(const uint32_t *src, int src_width, int src_height,
                                       unsigned char *dst, int dst_width, int dst_height,
                                       int dst_row_top, int dst_row_bottom)
{
	int dy;

	if (src == NULL || dst == NULL || src_width <= 0 || src_height <= 0 ||
	    dst_width <= 0 || dst_height <= 0) {
		return 0;
	}

	if (dst_row_top < 0) {
		dst_row_top = 0;
	}
	if (dst_row_bottom > dst_height) {
		dst_row_bottom = dst_height;
	}

	for (dy = dst_row_top; dy < dst_row_bottom; dy++) {
		int sy = dy * src_height / dst_height;
		const uint32_t *row;
		unsigned char *out;
		int dx;

		if (sy >= src_height) {
			sy = src_height - 1;
		}
		row = src + (size_t) sy * (size_t) src_width;
		out = dst + (size_t) dy * (size_t) dst_width * 3;

		for (dx = 0; dx < dst_width; dx++) {
			int sx = dx * src_width / dst_width;
			uint32_t pixel;

			if (sx >= src_width) {
				sx = src_width - 1;
			}
			pixel = row[sx];
			out[3 * dx + 0] = (unsigned char) ((pixel >> 16) & 0xff);
			out[3 * dx + 1] = (unsigned char) ((pixel >> 8) & 0xff);
			out[3 * dx + 2] = (unsigned char) (pixel & 0xff);
		}
	}

	return 1;
}

#endif /* REMOTE_DISPLAY_SCALE_H */
