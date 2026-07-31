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
 * display_mode_fit() is what keeps the synthesised monitor EDID, and any
 * mode requested later, inside that limit. The arithmetic is easy to break
 * without noticing, hence these.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display_mode.h"
#include "edid.h"

static int failures;

static void
expect_mode(const char *what, unsigned max_w, unsigned max_h, unsigned bpp,
            unsigned vram_mb, unsigned want_w, unsigned want_h)
{
	unsigned w = 0, h = 0;
	const int ok = display_mode_fit(max_w, max_h, bpp,
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
	const int ok = display_mode_fit(max_w, max_h, bpp,
	                                       (size_t) vram_mb * 1024 * 1024, &w, &h);

	if (ok) {
		printf("  %-44s FAIL (offered %ux%u)\n", what, w, h);
		failures++;
		return;
	}

	printf("  %-44s ok (declined)\n", what);
}


/* Is this mode in display_mode.c's table? Ask the chooser for it exactly, with
   no framebuffer limit: it can only answer with a table entry.

   This doubles as a reachability check. A "no" also catches an entry that is in
   the table but can never be chosen, because one listed earlier fits inside the
   same bounds - dead weight worth knowing about either way. */
static int
table_has(unsigned want_w, unsigned want_h)
{
	unsigned w = 0, h = 0;

	return display_mode_fit(want_w, want_h, 0, 0, &w, &h)
	       && w == want_w && h == want_h;
}

/* Does the synthesised EDID declare this mode as an established or standard
   timing? Decoded from the block rather than assumed, so the two files cannot
   drift apart unnoticed. */
static int
edid_advertises(const uint8_t *block, unsigned want_w, unsigned want_h)
{
	static const struct { unsigned w, h, byte, bit; } established[] = {
		{  640, 480, 0x23, 5 },
		{  800, 600, 0x23, 0 },
		{ 1024, 768, 0x24, 3 },
		{ 1280, 1024, 0x24, 0 },
	};
	/* Standard timings: height comes from the aspect code, not the block. */
	static const unsigned aspect_num[4] = { 16, 4, 5, 16 };
	static const unsigned aspect_den[4] = { 10, 3, 4,  9 };
	size_t i;

	for (i = 0; i < sizeof(established) / sizeof(established[0]); i++) {
		if (established[i].w == want_w && established[i].h == want_h
		    && (block[established[i].byte] & (1u << established[i].bit)) != 0) {
			return 1;
		}
	}

	for (i = 0x26; i < 0x36; i += 2) {
		unsigned w, h, aspect;

		if (block[i] == 0x01 && block[i + 1] == 0x01) {
			continue;			/* unused slot */
		}
		w = (block[i] + 31u) * 8u;
		aspect = (block[i + 1] >> 6) & 0x03u;
		h = w * aspect_den[aspect] / aspect_num[aspect];

		if (w == want_w && h == want_h) {
			return 1;
		}
	}

	return 0;
}

/* Every mode the chooser can pick has to be one the guest will accept, because
   RISC OS validates a mode against the monitor definition in force. The four
   largest are exempt: they appear only as the preferred timing, which covers
   whichever mode was chosen at boot. */
static void
check_edid_advertises_every_mode(void)
{
	static const struct { unsigned w, h; int preferred_only; } modes[] = {
		{ 3840, 2160, 1 },
		{ 3440, 1440, 1 },
		{ 2560, 1440, 1 },
		{ 1920, 1200, 1 },
		{ 1920, 1080, 0 },
		{ 1600, 1200, 0 },
		{ 1680, 1050, 0 },
		{ 1280, 1024, 0 },
		{ 1440,  900, 0 },
		{ 1280,  960, 0 },
		{ 1152,  864, 0 },
		{ 1280,  720, 0 },
		{ 1024,  768, 0 },
		{  800,  600, 0 },
		{  640,  480, 0 },
	};
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t i;

	/* A minimal well-formed base: the builder inherits from it and rewrites the
	   timings and checksum. */
	memset(base, 0, sizeof(base));
	memset(base + 1, 0xff, 6);
	base[18] = 1;			/* EDID version 1 */
	edid_build_from_base(block, base, 1920, 1080, 60);

	printf("\nevery mode the chooser can pick is one the EDID advertises\n");

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		const unsigned w = modes[i].w, h = modes[i].h;
		char label[64];

		snprintf(label, sizeof(label), "%ux%u%s", w, h,
		         modes[i].preferred_only ? " (preferred timing only)" : "");

		if (!table_has(w, h)) {
			printf("  %-44s FAIL (not offered by the chooser)\n", label);
			failures++;
			continue;
		}
		if (!modes[i].preferred_only && !edid_advertises(block, w, h)) {
			printf("  %-44s FAIL (chooser can pick it, EDID does not declare it)\n",
			       label);
			failures++;
			continue;
		}
		printf("  %-44s ok\n", label);
	}
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

	/* 2560x1440 at 32bpp is 14.06MB, so 16MB reaches 1440p and no further: the
	   next rungs up need 18.9MB and 31.6MB. A 4K host is therefore bounded by
	   memory here, which is the honest reason, and not by the table. */
	expect_mode("16MB, 4K host", 3840, 2160, 4, 16, 2560, 1440);
	expect_mode("16MB, 1440p host", 2560, 1440, 4, 16, 2560, 1440);

	/* Given the memory, the chooser must go above 1440p rather than stopping
	   there. These are the cases an artificial ceiling in the table used to
	   truncate: an ultrawide or a 4K panel was answered with 2560x1440 however
	   much framestore was available. */
	expect_mode("64MB, 4K host", 3840, 2160, 4, 64, 3840, 2160);
	expect_mode("32MB, ultrawide host", 3440, 1440, 4, 32, 3440, 1440);
	expect_mode("24MB, 4K host (2160p needs 31.6MB)", 3840, 2160, 4, 24, 3440, 1440);

	/* An ultrawide host with only the card's 15MB still gets a mode that fits
	   inside its width rather than one that overhangs it. */
	expect_mode("15MB, ultrawide host", 3440, 1440, 4, 15, 2560, 1440);

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

	check_edid_advertises_every_mode();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
