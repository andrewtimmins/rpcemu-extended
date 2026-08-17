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
 * guest_cursor.h - the guest's pointer as a host cursor.
 *
 * Shared by the machine window's panel and the Manager's, which draw the same
 * pointer from the same shape and differ only in how it reaches them: in
 * process, or over the control channel from a machine running in another one.
 * Hence the pack/unpack pair as well as the building.
 *
 * Header-only and arithmetic plus one wx call, like remote_display_scale.h next
 * to it, so neither panel needs a library and the pair can be reasoned about in
 * one place.
 */

#ifndef GUEST_CURSOR_H
#define GUEST_CURSOR_H

#include <cstdint>
#include <cstring>

#include <wx/cursor.h>
#include <wx/image.h>

#include "host_types.h"

/*
 * A shape held by value, for a panel to keep between frames.
 *
 * The bits are copied in rather than pointed at: the emulator hands over a
 * pointer good only for the duration of the call, and the Manager's copy arrives
 * in a message that is gone by the time anything paints.
 */
struct GuestCursor {
	/* 64 rows of a 64-pixel-wide shape at two bits per pixel. RISC OS pointers
	   are 32 wide and rarely over 32 tall; this is roomy and still small. */
	static const int kMaxRowBytes = 16;
	static const int kMaxRows = 64;

	uint8_t bits[kMaxRowBytes * kMaxRows] = {};
	int row_bytes = 0;
	uint32_t palette[4] = {};
	int width = 0;
	int height = 0;
	int hotspot_x = 0;
	int hotspot_y = 0;
	bool valid = false;

	bool Set(const PointerShape &shape)
	{
		valid = false;
		if (shape.bits == nullptr || shape.width <= 0 || shape.height <= 0 ||
		    shape.row_bytes <= 0 || shape.row_bytes > kMaxRowBytes ||
		    shape.height > kMaxRows) {
			return false;
		}
		row_bytes = shape.row_bytes;
		width = shape.width;
		height = shape.height;
		hotspot_x = shape.hotspot_x;
		hotspot_y = shape.hotspot_y;
		std::memcpy(palette, shape.palette, sizeof(palette));
		std::memcpy(bits, shape.bits, (size_t) row_bytes * (size_t) height);
		valid = true;
		return true;
	}
};

/*
 * Build the host cursor, at the size one guest pixel currently occupies.
 *
 * `num`/`den` is that size as a fraction - the panel's scaled width over the
 * guest's width - because the host draws a cursor at its native size and an
 * unscaled pointer in a doubled or zoomed mode would be half the size of
 * everything around it.
 *
 * Answers an invalid cursor rather than a wrong one, and the caller is expected
 * to fall back to something visible: see the note on platforms below.
 */
inline wxCursor guest_cursor_build(const GuestCursor &shape, int num, int den)
{
	if (!shape.valid || num <= 0 || den <= 0) {
		return wxCursor();
	}

	wxImage image(shape.width, shape.height);

	image.InitAlpha();

	bool any = false;

	for (int y = 0; y < shape.height; y++) {
		for (int x = 0; x < shape.width; x++) {
			const uint8_t byte = shape.bits[(size_t) y * (size_t) shape.row_bytes +
			                                (size_t) (x >> 2)];
			const unsigned colour = (unsigned) ((byte >> ((x & 3) * 2)) & 3);
			const uint32_t p = shape.palette[colour];

			/* Colour 0 is transparent, which is also how RISC OS hides the
			   pointer: a wholly transparent shape. */
			image.SetRGB(x, y, (unsigned char) ((p >> 16) & 0xff),
			    (unsigned char) ((p >> 8) & 0xff), (unsigned char) (p & 0xff));
			image.SetAlpha(x, y, colour == 0 ? 0 : 255);
			if (colour != 0) {
				any = true;
			}
		}
	}

	if (!any) {
		return wxCursor();
	}

	int w = shape.width, h = shape.height;
	int hx = shape.hotspot_x, hy = shape.hotspot_y;

	if (num != den) {
		const int sw = (shape.width * num) / den;
		const int sh = (shape.height * num) / den;

		/*
		 * Only scaled up when the result is a sane size for a cursor. Windows
		 * takes its cursor size from the system metrics and refuses much else,
		 * so a large one comes back invalid; past this the pointer is drawn at
		 * native size, which is small against a zoomed picture but is in the
		 * right place and can be seen.
		 */
		if (sw >= 1 && sh >= 1 && sw <= 128 && sh <= 128) {
			/* Nearest neighbour on purpose: a pointer is a few pixels of solid
			   colour with a hard edge, and smoothing turns the tip into a
			   smudge. */
			image = image.Scale(sw, sh, wxIMAGE_QUALITY_NEAREST);
			w = sw;
			h = sh;
			hx = (shape.hotspot_x * num) / den;
			hy = (shape.hotspot_y * num) / den;
		}
	}

	image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_X, hx);
	image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_Y, hy);

	wxCursor cursor(image);

	/*
	 * ★ Platforms disagree about what a cursor may look like.
	 *
	 * GTK and macOS take an arbitrary size with full alpha. Windows wants the
	 * size the system asks for - 32x32 on nearly every machine - and hands back
	 * nothing for anything much larger, and it reduces alpha to a mask, which
	 * costs this shape nothing because two-bits-per-pixel has no partial
	 * transparency to lose.
	 *
	 * So a scaled cursor is retried at native size rather than given up on: a
	 * pointer of the wrong size beats no pointer, and the caller's own fallback
	 * beats both being silent.
	 */
	if (!cursor.IsOk() && (w != shape.width || h != shape.height)) {
		wxImage native(shape.width, shape.height);

		native.InitAlpha();
		for (int y = 0; y < shape.height; y++) {
			for (int x = 0; x < shape.width; x++) {
				const uint8_t byte = shape.bits[(size_t) y *
				    (size_t) shape.row_bytes + (size_t) (x >> 2)];
				const unsigned colour = (unsigned) ((byte >> ((x & 3) * 2)) & 3);
				const uint32_t p = shape.palette[colour];

				native.SetRGB(x, y, (unsigned char) ((p >> 16) & 0xff),
				    (unsigned char) ((p >> 8) & 0xff),
				    (unsigned char) (p & 0xff));
				native.SetAlpha(x, y, colour == 0 ? 0 : 255);
			}
		}
		native.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_X, shape.hotspot_x);
		native.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_Y, shape.hotspot_y);
		cursor = wxCursor(native);
	}

	return cursor;
}

/* ----------------------------------------------------------------------
 * Crossing to the Manager
 * -------------------------------------------------------------------- */

/*
 * The wire form, for the fixed field an IpcEvent already carries.
 *
 * Little-endian and byte-packed by hand rather than memcpy of the struct: the
 * two processes are the same build today, but a layout that depends on the
 * compiler is a trap waiting for the day they are not.
 */
inline int guest_cursor_pack(const PointerShape &shape, char *out, size_t len)
{
	const int rows = shape.height;
	const size_t bits_len = (size_t) shape.row_bytes * (size_t) rows;
	const size_t total = 24 + bits_len;	/* 24-byte header, then the rows */
	size_t i;

	if (shape.bits == nullptr || rows <= 0 || shape.row_bytes <= 0 ||
	    total > len) {
		return 0;
	}

	std::memset(out, 0, len);
	out[0] = (char) (shape.width & 0xff);
	out[1] = (char) ((shape.width >> 8) & 0xff);
	out[2] = (char) (rows & 0xff);
	out[3] = (char) ((rows >> 8) & 0xff);
	out[4] = (char) (shape.row_bytes & 0xff);
	out[5] = (char) (shape.hotspot_x & 0xff);
	out[6] = (char) (shape.hotspot_y & 0xff);
	out[7] = (char) ((shape.visible ? 1 : 0) | (shape.host_side ? 2 : 0));
	for (i = 0; i < 4; i++) {
		const uint32_t p = shape.palette[i];

		out[8 + i * 4 + 0] = (char) (p & 0xff);
		out[8 + i * 4 + 1] = (char) ((p >> 8) & 0xff);
		out[8 + i * 4 + 2] = (char) ((p >> 16) & 0xff);
		out[8 + i * 4 + 3] = (char) ((p >> 24) & 0xff);
	}
	std::memcpy(out + 24, shape.bits, bits_len);
	return (int) (24 + bits_len);
}

/* Answers false on anything that does not describe a shape this can hold. */
inline bool guest_cursor_unpack(const char *in, size_t len, GuestCursor *shape,
                                bool *visible, bool *host_side)
{
	if (in == nullptr || len < 24) {
		return false;
	}

	const unsigned char *u = (const unsigned char *) in;
	const int width = u[0] | (u[1] << 8);
	const int rows = u[2] | (u[3] << 8);
	const int row_bytes = u[4];
	size_t i;

	*visible = (u[7] & 1) != 0;
	*host_side = (u[7] & 2) != 0;

	if (width <= 0 || rows <= 0 || row_bytes <= 0 ||
	    row_bytes > GuestCursor::kMaxRowBytes || rows > GuestCursor::kMaxRows ||
	    24 + (size_t) row_bytes * (size_t) rows > len) {
		shape->valid = false;
		return false;
	}

	shape->width = width;
	shape->height = rows;
	shape->row_bytes = row_bytes;
	shape->hotspot_x = u[5];
	shape->hotspot_y = u[6];
	for (i = 0; i < 4; i++) {
		shape->palette[i] = (uint32_t) u[8 + i * 4 + 0] |
		    ((uint32_t) u[8 + i * 4 + 1] << 8) |
		    ((uint32_t) u[8 + i * 4 + 2] << 16) |
		    ((uint32_t) u[8 + i * 4 + 3] << 24);
	}
	std::memcpy(shape->bits, in + 24, (size_t) row_bytes * (size_t) rows);
	shape->valid = true;
	return true;
}

#endif /* GUEST_CURSOR_H */
