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

#ifndef DISPLAY_ACCELERATION_H
#define DISPLAY_ACCELERATION_H

/*
 * Whether a machine shown in the Manager is drawn through the platform's
 * accelerated display path.
 *
 * What that path IS differs by platform, which is the point of having one
 * setting rather than three:
 *
 *   Windows        Direct2D, through wxGraphicsContext. Hardware accelerated,
 *                  and already what the Manager uses.
 *   Linux, macOS   an OpenGL texture (gl_display_canvas.cpp), because the
 *                  wxGraphicsContext there is Cairo or Core Graphics and does
 *                  the scaling in software - measured on GTK at 30ms a frame,
 *                  which is worse than doing it ourselves.
 *
 * Turning it off gives every platform the same software path: convert, scale on
 * the CPU, blit. That is what the setting means to the user, and why it is
 * phrased as "hardware acceleration" rather than "use OpenGL".
 *
 * The rule below is separated out so it can be tested without a toolkit or a
 * preference store, because it is precedence and precedence gets inverted: the
 * command line has to beat the stored preference, not the other way round, or
 * `--no-gl` would do nothing for anyone who had ever ticked the box.
 */

#define DISPLAY_ACCELERATION_NO_OVERRIDE (-1)

/**
 * @param override_state DISPLAY_ACCELERATION_NO_OVERRIDE, or 0/1 from the
 *                       command line for this session only.
 * @param stored         What the user's preferences say (1 by default: a
 *                       machine with no accelerated path falls back on its own,
 *                       so defaulting to off would slow down every user who
 *                       never finds the setting).
 * @return non-zero if the accelerated path should be used.
 */
static inline int
display_acceleration_decide(int override_state, int stored)
{
	if (override_state != DISPLAY_ACCELERATION_NO_OVERRIDE) {
		return override_state != 0;
	}

	return stored != 0;
}

#endif /* DISPLAY_ACCELERATION_H */
