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

#ifndef VNC_POINTER_H
#define VNC_POINTER_H

/*
 * Where a VNC client's pointer lands, in the coordinates MouseMove() wants.
 *
 * A one-line conversion in its own header so that tests/test_vnc_pointer.c can
 * check it without linking libvncserver, and so the reasoning below sits next
 * to the arithmetic rather than being rediscovered.
 *
 * THE TWO COORDINATE SPACES. VncServer::updateFramebuffer() serves the
 * framebuffer at the size the frame arrives with, which is the guest's own
 * pixels - 320x256 in MODE 9. But mouse.x/mouse.y, which is what MouseMove()
 * sets, are in HOST DISPLAY pixels, because mouse_hack_get_pos() in keyboard.c
 * turns them into OS units with a single shift:
 *
 *     temp_x = mouse.x << 1;
 *
 * and the guest's own OS units per pixel is its eigen factor, which is 2 for a
 * low-resolution mode and 1 for the rest. The two agree only because VIDC
 * doubles a low-resolution mode up for display, so the host display has twice
 * as many pixels across as the guest does - CalculateScaling() in
 * emulator_panel.cpp is explicit about it:
 *
 *     host_xsize_ = image_width_ * 2;   // when double_size_ & VIDC_DOUBLE_X
 *
 * The native window converts window pixels into that space in
 * EmulatorPanel::PanelPointToHost() before calling MouseMove(). This is the
 * same conversion for the VNC path, which had none.
 *
 * WHAT IT COST. Without it, a client in MODE 9 could only reach the top-left
 * quarter of the screen: pointing at the far corner of a 320x256 framebuffer
 * put the guest's pointer at (159, 127), every click landed at half the
 * distance from the origin, and the right and bottom edges could not be
 * reached at all. In a mode VIDC does not double - 640x480 and up - the factor
 * is 1 and nothing was ever wrong, which is why this went unnoticed.
 *
 * Each axis is doubled independently because VIDC doubles them independently.
 */
static inline void
vnc_pointer_to_host(int x, int y, int double_x, int double_y,
                    int *host_x, int *host_y)
{
	*host_x = double_x ? x * 2 : x;
	*host_y = double_y ? y * 2 : y;
}

#endif
