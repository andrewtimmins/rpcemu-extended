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

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef HOST_TYPES_H
#define HOST_TYPES_H

#include <cstdint>

struct VideoUpdate {
	const uint32_t *buffer = nullptr;
	int xsize = 0;
	int ysize = 0;
	int yl = 0;
	int yh = 0;
	int double_size = 0;
	int host_xsize = 0;
	int host_ysize = 0;
};

struct MouseMoveUpdate {
	int16_t x = 0;
	int16_t y = 0;
};

/*
 * The guest's pointer, for the front end to draw as the host's own cursor.
 *
 * The pointer used to be composited into the frame by the emulator, which tied
 * it to the frame pipeline: it could only move once per emulated frame and it
 * inherited every hop's latency on the way to the screen. Handing its shape over
 * instead lets the host draw it, at the host's refresh rate and with the host's
 * latency, which is what makes it track the hand.
 *
 * Carried in the shape's own format rather than as pixels: two bits per pixel,
 * four to a byte, colour 0 transparent and 1 to 3 from `palette`. That is what
 * both VIDC and the graphics card hold, it is a few hundred bytes rather than a
 * few thousand, and it is small enough to send to the Manager over the control
 * channel without a segment of its own. `row_bytes` is the stride, which is not
 * always width/4 - the card pads its rows.
 *
 * The hotspot is where the guest's active point sits inside the shape. RISC OS
 * swaps between several pointer shapes, each with its own (mouse_hack's
 * activex[]/activey[]), so it travels with the shape.
 *
 * `host_side` is the front end's instruction, not a hint: false means the
 * emulator is still compositing the pointer into the frame and the front end
 * must NOT draw one of its own, or there would be two. It goes false when the
 * guest detaches the pointer from the mouse (OS_Word 21), when the mouse is not
 * in follow-host mode, while a VNC client is watching, and on any front end that
 * has not said it can draw a cursor at all.
 */
struct PointerShape {
	const uint8_t *bits = nullptr;
	int row_bytes = 0;
	uint32_t palette[4] = {};
	int width = 0;
	int height = 0;
	int hotspot_x = 0;
	int hotspot_y = 0;
	bool visible = false;
	bool host_side = false;
};

#endif
