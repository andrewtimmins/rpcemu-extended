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
 * display_options.h - the one place the display choices are named.
 *
 * The Settings menu and the machine editor both offer these choices, and each
 * used to spell them out for itself: "Pixel Perfect" in the menu against "Pixel
 * perfect (whole-number scaling)" in the dialog, "Follow Host Display Size"
 * against "Follow the host display size". Two names for one setting reads as two
 * settings, which is half of why these options were confusing.
 *
 * Naming each one once makes the two views mirror each other by construction,
 * rather than by somebody remembering to change both. The explanatory text is
 * here for the same reason: the menu shows it in the status bar and the dialog
 * shows it as a tooltip, and there is no reason for them to say different things.
 */

#ifndef DISPLAY_OPTIONS_H
#define DISPLAY_OPTIONS_H

#include <cstddef>
#include <utility>
#include <vector>

#include <wx/string.h>

extern "C" {
#include "rpcemu.h"
#include "display_mode.h"
}

namespace DisplayOptions {

/* --- how many pixels the RISC OS desktop has ----------------------------- */

inline const char *ScreenSizeGroup() { return "RISC OS Screen Size"; }

inline const char *ScreenSizeHelp()
{
	return "The size of the RISC OS desktop. Only modes this machine's display "
	       "memory can hold are offered; fit more VRAM, or the graphics card, for "
	       "the larger ones. The biggest sizes need a lower colour depth to fit, "
	       "so RISC OS may not offer 32 thousand or 16 million colours there.";
}

/* --- how that desktop is drawn in the window ----------------------------- */

inline const char *ScalingGroup() { return "Show In Window"; }

inline const char *ScalingActualSize() { return "Actual Size"; }
inline const char *ScalingActualSizeHelp()
{
	return "One RISC OS pixel per screen pixel, never scaled. The window is the "
	       "size of the desktop.";
}

inline const char *ScalingWholeMultiples() { return "Whole Multiples Only"; }
inline const char *ScalingWholeMultiplesHelp()
{
	return "Resize the window freely, but only ever draw at 2x, 3x and so on, so "
	       "pixels stay square and perfectly sharp. The desktop is centred, so a "
	       "border appears when the window is between two multiples.";
}

/* --- and full screen, which is neither of the above --------------------- */

inline const char *FullScreen() { return "Fullscreen"; }
inline const char *FullScreenHelp()
{
	return "Give the whole display to RISC OS. Alt+Enter leaves again.";
}

/* --- validation ---------------------------------------------------------- */

/*
 * A configuration file is text somebody may have edited, and an out-of-range
 * value here would index straight off the end of a menu or a radio group.
 */
inline int ClampDisplayScaling(int value)
{
	return value == DisplayScaling_WholeMultiples
	    ? DisplayScaling_WholeMultiples : DisplayScaling_ActualSize;
}

/* --- the fixed-size list ------------------------------------------------- */

/** "1920 x 1080", the one spelling of a mode used everywhere it is shown. */
inline wxString ModeLabel(unsigned width, unsigned height)
{
	return wxString::Format("%u x %u", width, height);
}

/**
 * The screen sizes a machine with this much display memory can show, largest
 * first.
 *
 * Budgeted at 32 bits per pixel because that is the deepest the desktop may
 * choose, and a mode offered here has to work whichever depth is configured.
 * A machine with no VRAM takes screen memory from DRAM instead, so there is no
 * figure to reason about and every mode is offered.
 */
inline void FixedModes(size_t display_memory,
                       std::vector<std::pair<unsigned, unsigned>> &out)
{
	out.clear();
	for (size_t i = 0; i < display_mode_count(); i++) {
		unsigned w = 0, h = 0;

		if (!display_mode_get(i, &w, &h)) {
			continue;
		}
		/* Budgeted at the shallowest depth, not the deepest. The user is
		   choosing a size, and the colour depth at that size is the guest's
		   choice afterwards - see DISPLAY_BPP_SHALLOWEST. Budgeting the list at
		   32bpp hid 3840x2160 from every machine we can configure, including
		   16MB VRAM ones that display it quite happily in 256 colours, which is
		   issue #207. What stays out is a mode that fits at no depth at all. */
		if (display_memory != 0 &&
		    (size_t) w * (size_t) h * DISPLAY_BPP_SHALLOWEST > display_memory)
		{
			continue;
		}
		/* And nothing the guest has already refused. RISC OS only accepts modes
		   the monitor definition in force declares, which is not something the
		   host can read, so refusals are learned as they happen - see
		   display_mode_mark_unavailable(). Offering one again would be offering a
		   choice that is known not to work. */
		if (display_mode_is_unavailable(w, h)) {
			continue;
		}
		out.emplace_back(w, h);
	}
}

} /* namespace DisplayOptions */

#endif /* DISPLAY_OPTIONS_H */
