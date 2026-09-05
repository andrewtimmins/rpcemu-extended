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

/* The Established Timings III bitmap, as RISC OS reads it: the descriptor
   tagged 0xF7 carries six bytes of flags over a fixed list of VESA DMT modes,
   most significant bit first. Listed here independently of edid.c so that a
   mistake in one is not repeated by the other. */
static const struct { unsigned w, h, byte, bit; } est3[] = {
	{  640,  350, 0, 7 }, {  640,  400, 0, 6 },
	{  720,  400, 0, 5 }, {  640,  480, 0, 4 },
	{  848,  480, 0, 3 }, {  800,  600, 0, 2 },
	{ 1024,  768, 0, 1 }, { 1152,  864, 0, 0 },
	{ 1280,  768, 1, 7 }, { 1280,  768, 1, 6 },
	{ 1280,  768, 1, 5 }, { 1280,  768, 1, 4 },
	{ 1280,  960, 1, 3 }, { 1280,  960, 1, 2 },
	{ 1280, 1024, 1, 1 }, { 1280, 1024, 1, 0 },
	{ 1360,  768, 2, 7 }, { 1440,  900, 2, 6 },
	{ 1440,  900, 2, 5 }, { 1440,  900, 2, 4 },
	{ 1440,  900, 2, 3 }, { 1400, 1050, 2, 2 },
	{ 1400, 1050, 2, 1 }, { 1400, 1050, 2, 0 },
	{ 1400, 1050, 3, 7 }, { 1680, 1050, 3, 6 },
	{ 1680, 1050, 3, 5 }, { 1680, 1050, 3, 4 },
	{ 1680, 1050, 3, 3 }, { 1600, 1200, 3, 2 },
	{ 1600, 1200, 3, 1 }, { 1600, 1200, 3, 0 },
	{ 1600, 1200, 4, 7 }, { 1600, 1200, 4, 6 },
	{ 1792, 1344, 4, 5 }, { 1792, 1344, 4, 4 },
	{ 1856, 1392, 4, 3 }, { 1856, 1392, 4, 2 },
	{ 1920, 1200, 4, 1 }, { 1920, 1200, 4, 0 },
	{ 1920, 1200, 5, 7 }, { 1920, 1200, 5, 6 },
	{ 1920, 1440, 5, 5 }, { 1920, 1440, 5, 4 },
};

/* Find the descriptor carrying the Established Timings III bitmap, or NULL.
   The four 18-byte descriptors start at 0x36; one whose first three bytes are
   zero is a display descriptor, and byte 3 is then its tag. */
static const uint8_t *
edid_est3_bitmap(const uint8_t *block)
{
	int i;

	for (i = 0; i < 4; i++) {
		const uint8_t *d = block + 0x36 + i * 18;

		if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xf7 && d[4] == 0) {
			return d + 6;
		}
	}
	return NULL;
}

/* Does the synthesised EDID declare this mode as an established, standard or
   Established Timings III timing? Decoded from the block rather than assumed,
   so the two files cannot drift apart unnoticed. */
static int
edid_advertises(const uint8_t *block, unsigned want_w, unsigned want_h)
{
	static const struct { unsigned w, h, byte, bit; } established[] = {
		{  720, 400, 0x23, 7 },
		{  640, 480, 0x23, 5 },
		{  640, 480, 0x23, 3 },
		{  640, 480, 0x23, 2 },
		{  800, 600, 0x23, 1 },
		{  800, 600, 0x23, 0 },
		{  800, 600, 0x24, 7 },
		{  800, 600, 0x24, 6 },
		{  832, 624, 0x24, 5 },
		{ 1024, 768, 0x24, 3 },
		{ 1024, 768, 0x24, 2 },
		{ 1024, 768, 0x24, 1 },
		{ 1280, 1024, 0x24, 0 },
		{ 1152, 870, 0x25, 7 },
	};
	const uint8_t *bitmap = edid_est3_bitmap(block);
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

	if (bitmap != NULL) {
		for (i = 0; i < sizeof(est3) / sizeof(est3[0]); i++) {
			if (est3[i].w == want_w && est3[i].h == want_h &&
			    (bitmap[est3[i].byte] & (1u << est3[i].bit)) != 0) {
				return 1;
			}
		}
	}

	return 0;
}

/* The Established Timings III descriptor is what lifts the mode list past what
   eight standard timings can say. Two things have to hold: a mode the machine
   can display is advertised, and one it cannot is not - a block that offered
   everything would put the user back to picking modes RISC OS then refuses.

   1920x1200 is the case worth naming. It has no standard-timing encoding at
   all, so before this descriptor existed it could only ever be the preferred
   timing, and a machine whose preferred mode was something else could not be
   asked to switch to it. */
static void
check_edid_est3_scales_with_the_preferred_mode(void)
{
	static const struct {
		unsigned pref_w, pref_h;	/* preferred (native) mode */
		unsigned w, h;			/* mode being asked about */
		int advertised;			/* should the block declare it? */
	} cases[] = {
		{ 2560, 1440, 1920, 1200, 1 },
		{ 2560, 1440, 1920, 1440, 1 },
		{ 2560, 1440, 1600, 1200, 1 },
		{ 1920, 1200, 1920, 1200, 1 },
		{ 1920, 1200, 1680, 1050, 1 },
		{ 1920, 1200, 1920, 1440, 0 },	/* taller than the display */
		{ 1920, 1080, 1600, 1200, 0 },	/* taller than the display */
		{ 1920, 1080, 1680, 1050, 1 },
		{ 1024,  768, 1280, 1024, 0 },	/* larger than the display */
		{ 1024,  768,  800,  600, 1 },
		{  800,  600,  848,  480, 0 },	/* wider than the display */
	};
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t i;

	printf("\nthe mode bitmap scales with the advertising ceiling\n");

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char label[80];
		int got;

		memset(base, 0, sizeof(base));
		memset(base + 1, 0xff, 6);
		base[18] = 1;
		edid_build_from_base(block, base, cases[i].pref_w, cases[i].pref_h,
		                     cases[i].pref_w, cases[i].pref_h, 60);

		if (edid_est3_bitmap(block) == NULL) {
			printf("  native %ux%u: FAIL (no 0xF7 descriptor in the block)\n",
			       cases[i].pref_w, cases[i].pref_h);
			failures++;
			continue;
		}
		if (!edid_block_is_valid(block)) {
			printf("  native %ux%u: FAIL (block checksum/header invalid)\n",
			       cases[i].pref_w, cases[i].pref_h);
			failures++;
			continue;
		}

		snprintf(label, sizeof(label), "native %ux%u, %ux%u %s",
		         cases[i].pref_w, cases[i].pref_h, cases[i].w, cases[i].h,
		         cases[i].advertised ? "offered" : "withheld");

		got = edid_advertises(block, cases[i].w, cases[i].h);
		if (got != cases[i].advertised) {
			printf("  %-44s FAIL (%s)\n", label,
			       got ? "advertised but should not be"
			           : "not advertised but should be");
			failures++;
			continue;
		}
		printf("  %-44s ok\n", label);
	}
}

/* Every mode the chooser can pick has to be one the guest will accept, because
   RISC OS validates a mode against the monitor definition in force.

   Three are exempt, and only because nothing in EDID short of a detailed timing
   can say them: 3840x2160 and 3440x1440 are larger than the block is ever built
   for, and 2560x1440 is the preferred timing itself (DMT has no such mode - it
   goes from 1920x1440 to 2560x1600). 1920x1200 used to be exempt for the same
   reason, having no standard-timing encoding either; the Established Timings III
   descriptor says it now. */
static void
check_edid_advertises_every_mode(void)
{
	static const struct { unsigned w, h; int preferred_only; } modes[] = {
		{ 3840, 2160, 1 },
		{ 3440, 1440, 1 },
		{ 2560, 1440, 1 },
		{ 1920, 1440, 0 },
		{ 1856, 1392, 0 },
		{ 1792, 1344, 0 },
		{ 1920, 1200, 0 },
		{ 1920, 1080, 0 },
		{ 1600, 1200, 0 },
		{ 1680, 1050, 0 },
		{ 1400, 1050, 0 },
		{ 1280, 1024, 0 },
		{ 1440,  900, 0 },
		{ 1280,  960, 0 },
		{ 1360,  768, 0 },
		{ 1152,  870, 0 },
		{ 1152,  864, 0 },
		{ 1280,  768, 0 },
		{ 1280,  720, 0 },
		{ 1024,  768, 0 },
		{  832,  624, 0 },
		{  800,  600, 0 },
		{  848,  480, 0 },
		{  640,  480, 0 },
		{  720,  400, 0 },
		{  640,  400, 0 },
		{  640,  350, 0 },
	};
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t i;

	/* A minimal well-formed base: the builder inherits from it and rewrites the
	   timings and checksum. */
	memset(base, 0, sizeof(base));
	memset(base + 1, 0xff, 6);
	base[18] = 1;			/* EDID version 1 */

	/* Built for the largest display the emulator advertises, because the block
	   now describes one particular monitor rather than every monitor: a mode
	   larger than the preferred one in either direction is deliberately left
	   out. Asking at 2560x1440 is therefore asking "of everything the chooser
	   can pick, is anything missing that would fit?". */
	edid_build_from_base(block, base, 2560, 1440, 2560, 1440, 60);

	printf("\nevery mode the chooser can pick is one the EDID advertises\n");

	/*
	 * The table above says what is expected; this says what is actually there.
	 * Both, because listing the modes by hand is how the two files came apart in
	 * the first place: edid.c grew the descriptor that declares forty-four DMT
	 * modes, display_mode.c kept the thirteen the old standard timings could
	 * express, and nothing failed - RISC OS offered twenty-five sizes while the
	 * emulator's own menus offered thirteen. A count taken from the real table
	 * is what notices that.
	 */
	if (display_mode_count() != sizeof(modes) / sizeof(modes[0])) {
		printf("  %-44s FAIL (table has %zu, this test lists %zu - a mode was "
		       "added or removed without updating both)\n",
		       "table and test agree on the mode count",
		       display_mode_count(), sizeof(modes) / sizeof(modes[0]));
		failures++;
	} else {
		printf("  %-44s ok (%zu modes)\n", "table and test agree on the count",
		       display_mode_count());
	}

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		const unsigned w = modes[i].w, h = modes[i].h;
		char label[64];
		unsigned tw = 0, th = 0;

		/* In the table, in this order: the order is what makes "largest that
		   fits" work, so a mode arriving in the wrong place is a real fault even
		   though every mode is still present. */
		if (display_mode_get(i, &tw, &th) && (tw != w || th != h)) {
			printf("  %ux%u: FAIL (table has %ux%u at this position)\n",
			       w, h, tw, th);
			failures++;
			continue;
		}

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

/*
 * The bug this pair exists for.
 *
 * check_edid_advertises_every_mode() builds its block at 2560x1440, the largest
 * mode there is, where nothing can be above the ceiling and the invariant it
 * checks cannot fail. Every real machine has a preferred mode well below that,
 * and until the ceiling became its own argument the list was bounded by the
 * preferred mode - so a machine configured for 1280x1024 advertised nothing
 * larger, the chooser went on offering the bigger sizes, and RISC OS refused
 * every one of them as "unsuitable for displaying the desktop".
 *
 * So: build at a SMALL preferred mode with a LARGE ceiling, and check the list
 * follows the ceiling. With the ceiling argument removed these all fail.
 */
static void
check_small_preferred_mode_still_advertises_the_ceiling(void)
{
	static const struct { unsigned w, h; } want[] = {
		{ 1920, 1080 },		/* Derek's case: standard-timing ladder */
		{ 1920, 1200 },		/* Established Timings III */
		{ 1680, 1050 },
		{ 1280, 1024 },		/* the preferred mode itself */
		{ 1024,  768 },		/* established bitmap */
	};
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t i;

	memset(base, 0, sizeof(base));
	memset(base + 1, 0xff, 6);
	base[18] = 1;

	/* A 1920x1200 monitor, a machine configured for 1280x1024. */
	edid_build_from_base(block, base, 1280, 1024, 1920, 1200, 60);

	printf("\na small preferred mode still advertises up to the ceiling\n");

	if (!edid_block_is_valid(block)) {
		printf("  FAIL (block checksum/header invalid)\n");
		failures++;
		return;
	}

	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		char label[80];

		snprintf(label, sizeof(label), "%ux%u offered below a 1920x1200 ceiling",
		         want[i].w, want[i].h);
		if (!edid_advertises(block, want[i].w, want[i].h)) {
			printf("  %-52s FAIL\n", label);
			failures++;
			continue;
		}
		printf("  %-52s ok\n", label);
	}

	/* And the ceiling still means something: above it, nothing is offered. */
	if (edid_advertises(block, 1920, 1440)) {
		printf("  %-52s FAIL\n", "1920x1440 withheld above the ceiling");
		failures++;
	} else {
		printf("  %-52s ok\n", "1920x1440 withheld above the ceiling");
	}
}

/*
 * 2560x1440 and its neighbours cannot be said by any bitmap - DMT has no such
 * mode and a standard timing stops at 2288 pixels wide - so they are carried in
 * the two spare detailed-timing descriptors. Without that, picking 1440p from
 * the chooser is refused by RISC OS however large the ceiling is, which is the
 * 1440p half of the display reports.
 */
static void
check_detail_only_modes_are_declared(void)
{
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];

	memset(base, 0, sizeof(base));
	memset(base + 1, 0xff, 6);
	base[18] = 1;

	printf("\nmodes no bitmap can express are carried as detailed timings\n");

	/* A 1440p monitor, machine configured for 1920x1080. */
	edid_build_from_base(block, base, 1920, 1080, 2560, 1440, 60);

	if (!edid_block_is_valid(block)) {
		printf("  FAIL (block checksum/header invalid)\n");
		failures++;
		return;
	}
	if (!edid_block_declares(block, 2560, 1440)) {
		printf("  %-52s FAIL\n", "2560x1440 declared below a 1440p ceiling");
		failures++;
	} else {
		printf("  %-52s ok\n", "2560x1440 declared below a 1440p ceiling");
	}
	/* The preferred mode keeps its own descriptor either way. */
	if (!edid_block_declares(block, 1920, 1080)) {
		printf("  %-52s FAIL\n", "1920x1080 still declared as preferred");
		failures++;
	} else {
		printf("  %-52s ok\n", "1920x1080 still declared as preferred");
	}
	/* Above the ceiling it stays out. */
	if (edid_block_declares(block, 3840, 2160)) {
		printf("  %-52s FAIL\n", "3840x2160 withheld above the ceiling");
		failures++;
	} else {
		printf("  %-52s ok\n", "3840x2160 withheld above the ceiling");
	}
}

/*
 * edid_block_declares() is what the size chooser asks before deciding whether a
 * refusal was the emulator's fault or the guest's, so it has to agree with the
 * decoder this file already had. Two independent readers of the same block: if
 * they disagree, one of them is wrong and the dialogue gives the wrong advice.
 */
static void
check_declares_agrees_with_the_test_decoder(void)
{
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t i;

	memset(base, 0, sizeof(base));
	memset(base + 1, 0xff, 6);
	base[18] = 1;
	edid_build_from_base(block, base, 1280, 1024, 1920, 1200, 60);

	printf("\nedid_block_declares() agrees with the test's own decoder\n");

	for (i = 0; i < display_mode_count(); i++) {
		unsigned w = 0, h = 0;
		int mine, theirs;

		if (!display_mode_get(i, &w, &h)) {
			continue;
		}
		mine = edid_block_declares(block, w, h) ? 1 : 0;
		theirs = edid_advertises(block, w, h) ? 1 : 0;
		if (mine != theirs) {
			printf("  %ux%u: FAIL (edid_block_declares says %d, decoder says %d)\n",
			       w, h, mine, theirs);
			failures++;
		}
	}
	printf("  every mode in the table agrees\n");
}

int
main(void)
{
	printf("mode selection bounded by VRAM, budgeting 32bpp\n");

	/* A Kinetic is clamped to 2MB, so 1600x1200 at 32bpp (7.68MB) is out of
	   reach and the biggest standard mode that fits is 832x624 (1.98MB). */
	expect_mode("Kinetic, 2MB, 1080p host", 1920, 1080, 4, 2, 832, 624);

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

	/* Bounded by an unusually short display rather than by VRAM: the widest
	   mode no taller than 515 lines, which is 848x480. */
	expect_mode("2MB, 1920x515 host", 1920, 515, 4, 2, 848, 480);

	/* 1MB at 32bpp reaches the bottom of the ladder and no further: 640x400 is
	   1000KB and fits, 640x480 at 1200KB does not. It used to fit nothing at
	   all, before the small DMT modes were added to match what the EDID
	   declares. */
	expect_mode("1MB, only the bottom rung fits", 1920, 1080, 4, 1, 640, 400);

	/* A machine with no VRAM fitted takes screen memory from DRAM, so there is
	   no figure to reason about: budget 0 means the limit does not apply. */
	expect_mode("no VRAM figure, budget skipped", 1920, 1080, 4, 0, 1920, 1080);

	printf("\nthe same machines were the desktop 16bpp\n");
	expect_mode("Kinetic 2MB at 16bpp", 1920, 1080, 2, 2, 1360, 768);
	expect_mode("8MB at 16bpp", 1920, 1080, 2, 8, 1920, 1080);

	/*
	 * The same machines with the desktop in 256 colours, which is what a size
	 * asked for by name is budgeted at since issue #207.
	 *
	 * 3840x2160 is the case in the report: 33MB at 32bpp, so no machine we can
	 * configure could reach it, against 8MB at 8bpp, which a 16MB VRAM machine
	 * holds with room to spare. The mode was there in the list all along and
	 * the budget was the only thing keeping it out.
	 */
	printf("\nand the same machines at 8bpp, which is what a named size gets\n");
	expect_mode("16MB at 8bpp reaches 4K", 3840, 2160, 1, 16, 3840, 2160);
	/* The same machine budgeted at 32bpp stops at 1440p, which is the case
	   asserted above and the whole of the difference the report was about. */
	/* 8MB gets there too, with 92KB to spare: 3840x2160 is 8100KB against a
	   budget of 8192KB. Worth pinning, because it is the closest fit in the
	   whole table and an off-by-a-little in the budget arithmetic would move
	   it. */
	expect_mode("8MB at 8bpp reaches 4K, barely", 3840, 2160, 1, 8, 3840, 2160);
	/* Depth buys headroom, it does not remove the limit: 2MB still cannot show
	   4K in 256 colours, and stops at the largest mode under two megabytes. */
	expect_mode("2MB at 8bpp is still bounded", 3840, 2160, 1, 2, 1920, 1080);

	printf("\nbounds are respected exactly\n");
	expect_mode("host exactly 800x600", 800, 600, 4, 8, 800, 600);
	expect_mode("host one pixel under 800x600", 799, 599, 4, 8, 640, 480);
	expect_nothing("host smaller than any mode", 320, 200, 4, 8);

	check_edid_advertises_every_mode();
	check_small_preferred_mode_still_advertises_the_ceiling();
	check_detail_only_modes_are_declared();
	check_declares_agrees_with_the_test_decoder();
	check_edid_est3_scales_with_the_preferred_mode();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
