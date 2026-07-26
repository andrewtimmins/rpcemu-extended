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

#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

#include <stddef.h>

/**
 * Find the largest standard display mode that fits both within the given bounds
 * and within a framebuffer budget.
 *
 * The second limit is the one that catches people out. Screen memory on a Risc PC
 * comes out of VRAM, so a mode is only displayable if its framebuffer fits: at 32
 * bits per pixel 1600x1200 needs 7.68MB, which a 2MB machine cannot show at any
 * refresh rate. Asking for one regardless earns "not suitable for displaying the
 * desktop" from RISC OS, so both the mode advertised in the synthesised EDID and
 * any mode requested of the guest later have to respect it.
 *
 * Kept free of any other dependency so it can be tested on its own, which also
 * keeps it away from the emulator's link-time baggage.
 *
 * @param max_width       Largest acceptable width (typically the host display)
 * @param max_height      Largest acceptable height
 * @param bytes_per_pixel Depth to budget for; 4 (32bpp) is the safe assumption
 *                        when the depth the guest will choose is not known
 * @param budget_bytes    Framebuffer bytes available, or 0 if unknown/unlimited
 * @param[out] width      Chosen width
 * @param[out] height     Chosen height
 *
 * @return non-zero if a mode was found, zero if nothing fits
 */
extern int display_mode_fit(unsigned max_width, unsigned max_height,
                            unsigned bytes_per_pixel, size_t budget_bytes,
                            unsigned *width, unsigned *height);

#endif /* DISPLAY_MODE_H */
