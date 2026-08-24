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
 */

/*
 * test_rom_gfxcard.c - which ROMs may be offered the graphics card
 *
 * The card is a GraphicsV display driver and GraphicsV is RISC OS 5's, so the
 * machine editor greys the card out for a ROM that cannot drive it. The test
 * worth having is the one that looks wrong: RISC OS 6 must be refused. It is a
 * higher number than 5 and a different operating system - RISCOS Ltd's line,
 * descended from 4 - with no display driver interface at all. A ">= 5" written
 * anywhere in that decision passes every other case here and fails the machine
 * in front of the user.
 *
 * The version comes from the MOS title string, so these build ROM images
 * carrying one. The classic titles have a build date in parentheses and the
 * scanner insists on it, which is itself worth pinning: without it a line of
 * help text mentioning a version would be read as the version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rpcemu.h"
/* For mkdir(): the Windows CRT's _mkdir() takes no permission mode, and this is
   where the core's shim for that lives. */
#include "rpcemu-win.h"
#include "romload.h"

static int failures;
static char datadir[512];

/* --- what romload.c needs from the rest of the emulator ------------------- */

const char *
rpcemu_get_datadir(void)
{
	return datadir;
}

void
rpclog(const char *format, ...)
{
	(void) format;
}

void
fatal(const char *format, ...)
{
	(void) format;
	exit(1);
}

void
error(const char *format, ...)
{
	(void) format;
}

/* Only loadroms() and rom_model_is_compatible() reach these, and this test calls
   neither. They exist so romload.c links on its own. */
Config config;
Machine machine;
const Model_Details models[Model_MAX];
uint8_t *romb;

void
rom_patch_apply(size_t rom_bytes)
{
	(void) rom_bytes;
}

uint32_t
savestate_crc32(const void *data, size_t len)
{
	(void) data;
	(void) len;
	return 0;
}

const char *
rpcemu_file_get_extension(const char *filename)
{
	const char *dot = strrchr(filename, '.');

	return (dot == NULL) ? filename + strlen(filename) : dot + 1;
}

/* --- the ROMs -------------------------------------------------------------*/

/*
 * Write a ROM image carrying a MOS title, padded to `size`.
 *
 * The padding matters for two of these: the real RISC OS 4.39 and 6.16 images
 * are 6MB, which is the size at which rom_probe_addressing() stops looking for
 * a version because 26-bit against 32-bit is already settled. Reading the
 * version has to keep working there, so the sizes here are the real ones.
 */
static int
write_rom(const char *name, const char *title, size_t size)
{
	/* Generously bigger than the data directory, so that what is appended here
	   - "roms/", a ROM name, "/rom" - demonstrably fits whatever length that
	   directory turns out to be. GCC reasons about exactly this and warned
	   that "roms" might not fit when the two buffers were the same size; clang
	   did not, so it took CI to find it.

	   Checked as well as sized. A truncated path would create a directory
	   somewhere other than intended and the test would then be probing a ROM
	   it had not written, which is a worse failure than not running. */
	char dir[sizeof(datadir) + 512];
	char path[sizeof(dir) + 64];
	FILE *f;
	size_t written;

	if (snprintf(dir, sizeof(dir), "%sroms", datadir) >= (int) sizeof(dir)) {
		printf("  data directory path too long\n");
		return 0;
	}
	mkdir(dir, 0755);
	if (snprintf(dir, sizeof(dir), "%sroms/%s", datadir, name) >= (int) sizeof(dir)) {
		printf("  ROM directory path too long\n");
		return 0;
	}
	mkdir(dir, 0755);
	if (snprintf(path, sizeof(path), "%s/rom", dir) >= (int) sizeof(path)) {
		printf("  ROM file path too long\n");
		return 0;
	}

	f = fopen(path, "wb");
	if (f == NULL) {
		printf("  could not write %s\n", path);
		return 0;
	}

	/* A little way in, as in a real image: the scanner takes the earliest
	   match, so a title at offset zero would not exercise that. */
	for (written = 0; written < 64; written++) {
		fputc(0, f);
	}
	fwrite(title, 1, strlen(title), f);
	written += strlen(title);

	while (written < size) {
		fputc(0, f);
		written++;
	}
	fclose(f);
	return 1;
}

static void
expect(const char *what, const char *rom_dir, int want_supported,
       int want_major, int want_minor)
{
	char msg[512] = "";
	int major = -1, minor = -1;
	const int got = rom_supports_gfxcard(rom_dir, msg, sizeof(msg));
	const int found = rom_probe_os_version(rom_dir, &major, &minor);

	if (want_major > 0) {
		if (!found || major != want_major || minor != want_minor) {
			printf("  %-40s FAIL (version read as %d.%02d, wanted %d.%02d)\n",
			       what, found ? major : 0, found ? minor : 0,
			       want_major, want_minor);
			failures++;
			return;
		}
	}

	if (got != want_supported) {
		printf("  %-40s FAIL (card %s, should be %s)\n", what,
		       got ? "offered" : "refused",
		       want_supported ? "offered" : "refused");
		failures++;
		return;
	}

	/* A refusal has to say why: the message becomes the tooltip on the greyed
	   control, and a blank one leaves the user with a dead checkbox and no
	   explanation. */
	if (!want_supported && msg[0] == '\0') {
		printf("  %-40s FAIL (refused with no reason given)\n", what);
		failures++;
		return;
	}

	printf("  %-40s ok\n", what);
}

int
main(void)
{
	const char *tmp = getenv("TMPDIR");
	char base[400];

	snprintf(base, sizeof(base), "%srpcemu-romtest-%d/",
	         (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp/", (int) getpid());
	mkdir(base, 0755);
	snprintf(datadir, sizeof(datadir), "%s", base);

	printf("which ROMs may be offered the graphics card\n");

	/* The classic MOS titles carry a build date; the scanner requires it. */
	write_rom("R371", "RISC OS\t3.71 (25 Sep 1996)", 4u * 1024 * 1024);
	write_rom("R439", "RISC OS\t4.39 (10 Jan 2009)", 6u * 1024 * 1024);
	write_rom("R530", "RISC OS\t\t5.30", 4u * 1024 * 1024);
	write_rom("R616", "RISC OS\t6.16 (28 Feb 2012)", 6u * 1024 * 1024);
	write_rom("RNONE", "no operating system here at all", 2u * 1024 * 1024);

	expect("RISC OS 3.71 - refused", "R371", 0, 3, 71);
	expect("RISC OS 4.39 - refused, 6MB image", "R439", 0, 4, 39);
	expect("RISC OS 5.30 - offered", "R530", 1, 5, 30);

	/* The one that matters. */
	expect("RISC OS 6.16 - refused, though 6 > 5", "R616", 0, 6, 16);

	/* An unreadable ROM must not be locked out of a feature it might support:
	   the guest-side driver declines by itself if it turns out it cannot. */
	expect("unknown ROM - allowed", "RNONE", 1, 0, 0);

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
