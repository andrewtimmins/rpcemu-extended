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
 * A VNC client's pointer must land on the pixel it was pointed at.
 *
 * WHY THIS EXISTS. VncServer::updateFramebuffer() serves the framebuffer at the
 * guest's own pixel size, and vnc_ptr_callback() used to pass those coordinates
 * straight to MouseMove(). But mouse.x is in HOST DISPLAY pixels, because
 * mouse_hack_get_pos() turns it into OS units with a single "<< 1" - and the
 * host display has twice the guest's pixels across in any mode VIDC doubles up.
 * The native window does that conversion in PanelPointToHost(); the VNC path
 * had none, and so was short by exactly the doubling factor.
 *
 * What that looked like from a viewer, and why it took a while to believe it was
 * the emulator rather than the guest: in MODE 9 only the top-left QUARTER of the
 * screen could be reached. Every click landed at half the distance from the
 * origin, the guest's pointer fell further behind the client's the further it
 * was moved, and the right and bottom edges could not be reached at all - so in
 * a 320x256 desktop the icon bar along the bottom was simply unclickable. In a
 * mode VIDC does not double it was always correct, which is why this survived:
 * it looks like a fault in whichever guest happens to be in a low-resolution
 * mode rather than in the server.
 *
 * The invariant is stated here the whole way through rather than as the shift
 * alone, because the shift on its own is impossible to argue about: take a
 * framebuffer coordinate, convert it the way vnc_ptr_callback() does, then put
 * it through the same arithmetic mouse_hack_get_pos() uses to hand the guest a
 * position, and the guest must name the pixel the client pointed at. That
 * composite is what a user experiences, and it is what regressed.
 */

#include <stdio.h>
#include <stdlib.h>

#include "vnc_pointer.h"

static int failures = 0;

static void
check(const char *what, int ok)
{
	printf("  %-68s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/**
 * The guest's own view of where the pointer is, in guest pixels.
 *
 * mouse_hack_get_pos() computes OS units as mouse.x << 1, and the guest divides
 * by its mode's eigen factor to get a pixel - 2 for a doubled (low-resolution)
 * mode, 1 otherwise. Reproduced here rather than called so that this test needs
 * no emulator state; it is four lines and the point is the composition.
 */
static int
guest_pixel(int host_coordinate, int doubled)
{
	const int os_units = host_coordinate << 1;
	const int eigen = doubled ? 2 : 1;

	return os_units >> eigen;
}

/**
 * Point at (x, y) of a framebuffer served for a mode with this doubling, and
 * report the pixel the guest would say the pointer is on.
 */
static void
round_trip(int x, int y, int double_x, int double_y, int *out_x, int *out_y)
{
	int host_x;
	int host_y;

	vnc_pointer_to_host(x, y, double_x, double_y, &host_x, &host_y);
	*out_x = guest_pixel(host_x, double_x);
	*out_y = guest_pixel(host_y, double_y);
}

/**
 * Every pixel of a framebuffer of this size must round-trip to itself.
 */
static int
every_pixel_round_trips(int width, int height, int double_x, int double_y)
{
	int x;
	int y;

	for (x = 0; x < width; x++) {
		int got_x;
		int got_y;

		round_trip(x, 0, double_x, double_y, &got_x, &got_y);
		if (got_x != x) {
			printf("    x %d came back as %d\n", x, got_x);
			return 0;
		}
	}
	for (y = 0; y < height; y++) {
		int got_x;
		int got_y;

		round_trip(0, y, double_x, double_y, &got_x, &got_y);
		if (got_y != y) {
			printf("    y %d came back as %d\n", y, got_y);
			return 0;
		}
	}
	return 1;
}

/**
 * The converted coordinate must also stay inside the host display, whose width
 * is the guest's pixels times the doubling factor. A conversion that overshot
 * would be clamped by the bounding box rather than reported, so it is worth
 * stating separately from the round trip.
 */
static int
stays_on_the_host_display(int width, int height, int double_x, int double_y)
{
	const int host_w = double_x ? width * 2 : width;
	const int host_h = double_y ? height * 2 : height;
	int host_x;
	int host_y;

	vnc_pointer_to_host(0, 0, double_x, double_y, &host_x, &host_y);
	if (host_x != 0 || host_y != 0) {
		return 0;
	}
	vnc_pointer_to_host(width - 1, height - 1, double_x, double_y,
	                    &host_x, &host_y);
	return host_x >= 0 && host_x < host_w && host_y >= 0 && host_y < host_h;
}

int
main(void)
{
	int gx;
	int gy;

	printf("VNC pointer coordinates\n\n");

	/*
	 * MODE 9: 320x256 guest pixels, doubled both ways for display, 1280x1024
	 * OS units. The mode the bug was found in.
	 */
	round_trip(0, 0, 1, 1, &gx, &gy);
	check("MODE 9: the origin is the origin", gx == 0 && gy == 0);

	round_trip(100, 100, 1, 1, &gx, &gy);
	check("MODE 9: (100,100) reaches the pixel at (100,100)",
	      gx == 100 && gy == 100);

	/* The exact case that used to fail: before the conversion this returned
	   (50, 50), which is what "every click lands at half the distance" is. */
	round_trip(200, 150, 1, 1, &gx, &gy);
	check("MODE 9: (200,150) reaches the pixel at (200,150), not (100,75)",
	      gx == 200 && gy == 150);

	round_trip(319, 255, 1, 1, &gx, &gy);
	check("MODE 9: the far corner is reachable at all",
	      gx == 319 && gy == 255);

	check("MODE 9: every pixel of 320x256 round-trips to itself",
	      every_pixel_round_trips(320, 256, 1, 1));

	check("MODE 9: the conversion stays inside the 640x512 host display",
	      stays_on_the_host_display(320, 256, 1, 1));

	/*
	 * A mode VIDC does not double. These were always correct, and the point
	 * of testing them is that the fix must not disturb them.
	 */
	round_trip(0, 0, 0, 0, &gx, &gy);
	check("640x480: the origin is the origin", gx == 0 && gy == 0);

	round_trip(200, 150, 0, 0, &gx, &gy);
	check("640x480: (200,150) reaches the pixel at (200,150)",
	      gx == 200 && gy == 150);

	round_trip(639, 479, 0, 0, &gx, &gy);
	check("640x480: the far corner is reachable", gx == 639 && gy == 479);

	check("640x480: every pixel of 640x480 round-trips to itself",
	      every_pixel_round_trips(640, 480, 0, 0));

	check("640x480: an undoubled mode is passed through untouched",
	      stays_on_the_host_display(640, 480, 0, 0));

	/*
	 * VIDC doubles the two axes independently, so a mode may be stretched one
	 * way only. Handling them together would leave half of such a mode wrong,
	 * which is the sort of thing that is easy to write and hard to notice.
	 */
	round_trip(200, 150, 1, 0, &gx, &gy);
	check("doubled in X only: both axes still land correctly",
	      gx == 200 && gy == 150);

	round_trip(200, 150, 0, 1, &gx, &gy);
	check("doubled in Y only: both axes still land correctly",
	      gx == 200 && gy == 150);

	check("doubled in X only: every pixel of 320x480 round-trips",
	      every_pixel_round_trips(320, 480, 1, 0));

	check("doubled in Y only: every pixel of 640x256 round-trips",
	      every_pixel_round_trips(640, 256, 0, 1));

	printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
	       failures, failures == 1 ? "" : "s");

	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
