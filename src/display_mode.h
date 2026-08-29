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

/*
 * The two depths worth budgeting for, named rather than written as bare numbers
 * at each call, because which one a caller wants is a decision and 1 or 4 on its
 * own does not read as one.
 *
 * DISPLAY_BPP_SAFE is 32bpp, for choosing a size on the user's behalf: whatever
 * is picked automatically should still work when RISC OS lands in the deepest
 * mode it has.
 *
 * DISPLAY_BPP_SHALLOWEST is 8bpp, for a size the user has asked for by name.
 * The depth is the guest's choice, not ours, and budgeting a request at 32bpp
 * refuses modes that work perfectly well in 256 colours - which is issue #207,
 * where 3840x2160 needs 33MB at 32bpp and 8MB at 8bpp, so it was filtered out of
 * every machine we can configure while being entirely displayable on a 16MB one.
 * Someone who names a size is telling us what they want; the deeper modes at
 * that size may not be available, and that is theirs to find out.
 */
#define DISPLAY_BPP_SAFE        4
#define DISPLAY_BPP_SHALLOWEST  1

/**
 * How many standard modes there are, for walking the list.
 *
 * The machine editor offers a fixed screen size from these, filtered by what
 * the machine's display memory can hold. Offering a mode that will not fit
 * would put the user back where this whole redesign started: a control that
 * looks as though it works and then does not.
 */
extern size_t display_mode_count(void);

/**
 * The width and height of one standard mode. Ordered largest area first, the
 * same order display_mode_fit() searches, so a list built from this reads from
 * the biggest mode down.
 *
 * @param index Mode index, below display_mode_count()
 * @return non-zero if the index was in range and the outputs were written
 */
extern int display_mode_get(size_t index, unsigned *width, unsigned *height);

/*
 * Modes RISC OS has refused, so nothing offers them again.
 *
 * The list above is what a monitor is normally capable of, and it is NOT what
 * the guest will accept. RISC OS validates a mode against the monitor definition
 * in force, and that is often a monitor definition file rather than the EDID the
 * emulator synthesises - on a machine with the graphics card fitted, measured
 * directly, only six of the thirteen modes that fit its display memory were
 * accepted, and the refusals were not the ones anybody would guess: 1920x1200
 * yes, 1920x1080 no.
 *
 * There is no way to ask from the host side which definition is loaded or what
 * it declares, so the answer is learned. A request whose mode the guest never
 * adopts is marked here, and from then on display_mode_fit() steps over it and
 * the fixed-size menu stops offering it.
 *
 * Marks last for the session. The monitor definition belongs to the guest and
 * changes when it is reconfigured, so remembering refusals for ever would
 * outlive the reason for them.
 */
extern void display_mode_mark_unavailable(unsigned width, unsigned height);

/** Has this mode been refused by the guest? */
extern int display_mode_is_unavailable(unsigned width, unsigned height);

/** Forget every refusal, for a machine that has just restarted or switched. */
extern void display_mode_clear_unavailable(void);

#endif /* DISPLAY_MODE_H */
