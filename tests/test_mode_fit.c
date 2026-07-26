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
 * test_mode_fit.c - display mode selection against a VRAM budget
 *
 * Screen memory on a Risc PC comes out of VRAM, so a mode is only displayable if
 * its framebuffer fits. Offer the guest one that does not and RISC OS answers
 * "not suitable for displaying the desktop", which is what a Kinetic clamped to
 * 2MB does when handed 2560x1440.
 *
 * rpcemu_display_mode_fit() is what keeps the synthesised monitor EDID, and any
 * mode requested later, inside that limit. The arithmetic is easy to break
 * without noticing, hence these.
 */

#include <stdio.h>
#include <stddef.h>

#include "rpcemu.h"

static int failures;

static void
expect_mode(const char *what, unsigned max_w, unsigned max_h, unsigned bpp,
            unsigned vram_mb, unsigned want_w, unsigned want_h)
{
	unsigned w = 0, h = 0;
	const int ok = rpcemu_display_mode_fit(max_w, max_h, bpp,
	                                       (size_t) vram_mb * 1024 * 1024, &w, &h);

	if (!ok || w != want_w || h != want_h) {
		printf("  %-44s FAIL (got %s%ux%u, wanted %ux%u)\n", what,
		       ok ? "" : "nothing, ", w, h, want_w, want_h);
		failures++;
		return;
	}

	/* Whatever was chosen must genuinely fit: always the bounds, and the budget
	   too unless there is none to respect (vram_mb 0 means "unknown", so the
	   framebuffer limit does not apply). */
	if (w > max_w || h > max_h ||
	    (vram_mb != 0 && (size_t) w * h * bpp > (size_t) vram_mb * 1024 * 1024))
	{
		printf("  %-44s FAIL (%ux%u does not actually fit)\n", what, w, h);
		failures++;
		return;
	}

	printf("  %-44s ok (%ux%u)\n", what, w, h);
}

static void
expect_nothing(const char *what, unsigned max_w, unsigned max_h, unsigned bpp,
               unsigned vram_mb)
{
	unsigned w = 0, h = 0;
	const int ok = rpcemu_display_mode_fit(max_w, max_h, bpp,
	                                       (size_t) vram_mb * 1024 * 1024, &w, &h);

	if (ok) {
		printf("  %-44s FAIL (offered %ux%u)\n", what, w, h);
		failures++;
		return;
	}

	printf("  %-44s ok (declined)\n", what);
}

int
main(void)
{
	printf("mode selection bounded by VRAM, budgeting 32bpp\n");

	/* A Kinetic is clamped to 2MB, so 1600x1200 at 32bpp (7.68MB) is out of
	   reach and the biggest standard mode that fits is 800x600 (1.83MB). */
	expect_mode("Kinetic, 2MB, 1080p host", 1920, 1080, 4, 2, 800, 600);

	/* 1920x1080 at 32bpp is 7.91MB, which just fits 8MB. */
	expect_mode("stock 8MB, 1080p host", 1920, 1080, 4, 8, 1920, 1080);

	/* 2560x1440 at 32bpp is 14.06MB: fits 16MB, and is the table's ceiling, so
	   a 4K host is bounded by the table rather than by VRAM. */
	expect_mode("16MB, 4K host", 3840, 2160, 4, 16, 2560, 1440);
	expect_mode("16MB, 1440p host", 2560, 1440, 4, 16, 2560, 1440);

	/* Bounded by an unusually short display rather than by VRAM. */
	expect_mode("2MB, 1920x515 host", 1920, 515, 4, 2, 640, 480);

	/* Nothing standard fits 1MB at 32bpp; the caller must leave well alone
	   rather than advertise something undisplayable. */
	expect_nothing("1MB, nothing fits", 1920, 1080, 4, 1);

	/* A machine with no VRAM fitted takes screen memory from DRAM, so there is
	   no figure to reason about: budget 0 means the limit does not apply. */
	expect_mode("no VRAM figure, budget skipped", 1920, 1080, 4, 0, 1920, 1080);

	printf("\nthe same machines were the desktop 16bpp\n");
	expect_mode("Kinetic 2MB at 16bpp", 1920, 1080, 2, 2, 1152, 864);
	expect_mode("8MB at 16bpp", 1920, 1080, 2, 8, 1920, 1080);

	printf("\nbounds are respected exactly\n");
	expect_mode("host exactly 800x600", 800, 600, 4, 8, 800, 600);
	expect_mode("host one pixel under 800x600", 799, 599, 4, 8, 640, 480);
	expect_nothing("host smaller than any mode", 320, 200, 4, 8);

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
