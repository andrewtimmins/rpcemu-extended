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

#ifndef REMOTE_DISPLAY_GEOMETRY_H
#define REMOTE_DISPLAY_GEOMETRY_H

/*
 * Where a machine's screen goes inside the Manager's panel, and how a point in
 * that panel becomes a point on that screen.
 *
 * Plain arithmetic in a header of its own, with no wx types, for one reason:
 * these two answers have to agree with each other. When they did not - the
 * paint letterboxing the picture while the pointer mapping measured against the
 * whole panel - the guest pointer moved at a fraction of the speed of the hand
 * moving it, and since the host pointer is hidden while a machine is shown,
 * that was the only pointer the user could see. Both callers now come here, and
 * tests/test_remote_display.c holds them to it.
 */

struct remote_display_rect {
	int x, y, w, h;
};

/*
 * Fit frame_w x frame_h inside client_w x client_h, keeping its shape, centred.
 *
 * Whichever edge runs out first decides the scale, so one pair of sides gets
 * bars. A panel that is exactly the guest's shape gets none, and the result is
 * then the whole panel.
 */
static inline struct remote_display_rect
remote_display_rect_for(int client_w, int client_h, int frame_w, int frame_h)
{
	struct remote_display_rect rect;
	double scale_x, scale_y, scale;

	if (client_w <= 0 || client_h <= 0 || frame_w <= 0 || frame_h <= 0) {
		rect.x = 0;
		rect.y = 0;
		rect.w = 0;
		rect.h = 0;
		return rect;
	}

	scale_x = (double) client_w / (double) frame_w;
	scale_y = (double) client_h / (double) frame_h;
	scale = scale_x < scale_y ? scale_x : scale_y;

	rect.w = (int) ((double) frame_w * scale);
	rect.h = (int) ((double) frame_h * scale);
	if (rect.w < 1) {
		rect.w = 1;
	}
	if (rect.h < 1) {
		rect.h = 1;
	}
	rect.x = (client_w - rect.w) / 2;
	rect.y = (client_h - rect.h) / 2;

	return rect;
}

/*
 * A point in the panel to a pixel on the guest's screen.
 *
 * Measured from the drawn rectangle, not the panel, which is the whole point of
 * this file. A point in the bars is clamped to the nearest edge of the screen,
 * which is what a guest pointer pushed against the side of its own screen does.
 */
static inline void
remote_display_point_to_guest(struct remote_display_rect rect,
                              int frame_w, int frame_h,
                              int panel_x, int panel_y,
                              int *guest_x, int *guest_y)
{
	int gx, gy;

	if (rect.w <= 0 || rect.h <= 0 || frame_w <= 0 || frame_h <= 0) {
		*guest_x = 0;
		*guest_y = 0;
		return;
	}

	/*
	 * Both ends of the picture map to both ends of the guest's screen: the
	 * first pixel of the picture is column 0 and the last is the last column,
	 * with everything in between proportional.
	 *
	 * Worth spelling out, because the two obvious ways of writing this each
	 * lose an edge when the picture is scaled down. Truncating
	 * (panel * frame / rect) never reaches the far edge - 1600 guest columns
	 * across a 1200-pixel panel stops at 1598 - and sampling the middle of
	 * each panel pixel never reaches the near one, putting the top row out of
	 * reach at 1:2. Either way a row or column of the guest screen cannot be
	 * pointed at, and RISC OS keeps its scroll bars and its icon bar exactly
	 * there.
	 *
	 * Dividing by rect - 1 is what makes the far edge land: there are rect
	 * pixels but rect - 1 steps between the first and the last.
	 */
	if (rect.w > 1) {
		gx = ((panel_x - rect.x) * (frame_w - 1) + (rect.w - 1) / 2) / (rect.w - 1);
	} else {
		gx = 0;
	}
	if (rect.h > 1) {
		gy = ((panel_y - rect.y) * (frame_h - 1) + (rect.h - 1) / 2) / (rect.h - 1);
	} else {
		gy = 0;
	}

	if (gx < 0) {
		gx = 0;
	}
	if (gx > frame_w - 1) {
		gx = frame_w - 1;
	}
	if (gy < 0) {
		gy = 0;
	}
	if (gy > frame_h - 1) {
		gy = frame_h - 1;
	}

	*guest_x = gx;
	*guest_y = gy;
}

/*
 * A movement in the panel to a movement on the guest's screen, for captured-
 * pointer mode - where the guest is sent how far the mouse went rather than
 * where it ended up.
 *
 * Scaled by the drawn rectangle, like the mapping above: a hand movement across
 * the picture should be a pointer movement across the guest's screen whatever
 * size the picture is being shown at. Without that the pointer crawls on a
 * scaled-down display, the guest screen being larger than its picture.
 *
 * ★ The remainder is carried between calls, which is the part that cannot be
 * left out. A movement smaller than one guest pixel - every single-pixel
 * movement when the picture is magnified - divides to zero, so dropping the
 * remainder would make slow, careful movement do nothing at all while fast
 * movement worked, and in a game that is exactly when precision is wanted.
 * Truncation toward zero and a remainder that keeps the numerator's sign are
 * what make it come out symmetrically for movement in either direction.
 *
 * carry_x/carry_y must be the same pair of variables on every call for one
 * captured session, zeroed when capture begins (a stale remainder is at most one
 * guest pixel, so this matters for tidiness rather than correctness).
 */
static inline void
remote_display_delta_to_guest(struct remote_display_rect rect,
                              int frame_w, int frame_h,
                              int panel_dx, int panel_dy,
                              int *carry_x, int *carry_y,
                              int *guest_dx, int *guest_dy)
{
	if (rect.w <= 0 || rect.h <= 0 || frame_w <= 0 || frame_h <= 0) {
		*guest_dx = 0;
		*guest_dy = 0;
		return;
	}

	{
		const int numerator_x = panel_dx * frame_w + *carry_x;
		const int numerator_y = panel_dy * frame_h + *carry_y;

		*guest_dx = numerator_x / rect.w;
		*carry_x = numerator_x % rect.w;
		*guest_dy = numerator_y / rect.h;
		*carry_y = numerator_y % rect.h;
	}
}

#endif /* REMOTE_DISPLAY_GEOMETRY_H */
