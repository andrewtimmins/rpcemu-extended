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
 * test_gfxrom.c - the graphics card's driver module, as RISC OS will read it
 *
 * The driver is assembled by hand (build.sh --podules) and the result committed,
 * because the ARM toolchain is not part of an ordinary build. That makes a stale
 * or truncated image a real possibility, and the symptom would be a guest that
 * fails to boot rather than anything a compiler would catch. So this reads the
 * committed image the way the module system does - header offsets, title, help
 * string, command table - and checks it hangs together.
 *
 * It also checks the driver and the emulated card still agree on the card's
 * identity, which is the one constant that has to match across the two halves
 * for the driver to recognise the hardware at all.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfxcard.h"

static int failures;

static void
check(int cond, const char *what)
{
	printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

static uint8_t *rom;
static size_t rom_size;

static uint32_t
word_at(size_t off)
{
	if (off + 4 > rom_size) {
		return 0xffffffffu;
	}
	/* The module is little-endian, as the ARM that runs it is here. */
	return (uint32_t) rom[off] | ((uint32_t) rom[off + 1] << 8) |
	       ((uint32_t) rom[off + 2] << 16) | ((uint32_t) rom[off + 3] << 24);
}

/** Is off a null-terminated string wholly inside the image? */
static int
string_at(size_t off, const char **out)
{
	size_t i;

	for (i = off; i < rom_size; i++) {
		if (rom[i] == 0) {
			*out = (const char *) &rom[off];
			return 1;
		}
	}
	return 0;
}

/** Does the image contain this word anywhere, aligned? */
static int
contains_word(uint32_t val)
{
	size_t off;

	for (off = 0; off + 4 <= rom_size; off += 4) {
		if (word_at(off) == val) {
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	/* Module header word offsets (RISC OS PRM 1-201). */
	enum { HDR_START, HDR_INIT, HDR_FINAL, HDR_SERVICE, HDR_TITLE, HDR_HELP,
	       HDR_TABLE, HDR_SWIBASE, HDR_SWIHANDLER, HDR_SWITABLE, HDR_SWICODE,
	       HDR_MESSAGES, HDR_FLAGS, HDR_WORDS };
	const char *path = (argc > 1) ? argv[1] : "gfxroms/RPCEmuGfx,ffa";
	const char *str;
	unsigned commands = 0;
	size_t off;
	FILE *f;
	long len;

	f = fopen(path, "rb");
	if (f == NULL) {
		printf("FAILED: cannot open '%s'\n", path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	if (len <= 0 || (rom = malloc((size_t) len)) == NULL ||
	    fread(rom, 1, (size_t) len, f) != (size_t) len)
	{
		printf("FAILED: cannot read '%s'\n", path);
		fclose(f);
		return 1;
	}
	fclose(f);
	rom_size = (size_t) len;

	printf("%s (%zu bytes)\n", path, rom_size);
	check(rom_size > HDR_WORDS * 4, "larger than a module header");
	check((rom_size & 3) == 0, "a whole number of words");

	printf("\nthe header points inside the module\n");
	check(word_at(HDR_INIT * 4) > 0 && word_at(HDR_INIT * 4) < rom_size,
	      "initialisation entry");
	check(word_at(HDR_FINAL * 4) > 0 && word_at(HDR_FINAL * 4) < rom_size,
	      "finalisation entry");
	check(word_at(HDR_TITLE * 4) > 0 && word_at(HDR_TITLE * 4) < rom_size,
	      "title string");
	check(word_at(HDR_HELP * 4) > 0 && word_at(HDR_HELP * 4) < rom_size,
	      "help string");
	check(word_at(HDR_TABLE * 4) > 0 && word_at(HDR_TABLE * 4) < rom_size,
	      "command table");
	check(word_at(HDR_SWIBASE * 4) == 0, "no SWI chunk, as this driver has no SWIs");

	printf("\nthe module identifies itself\n");
	check(string_at(word_at(HDR_TITLE * 4), &str) && strcmp(str, "RPCEmuGfx") == 0,
	      "title is RPCEmuGfx");
	check(string_at(word_at(HDR_HELP * 4), &str) && strstr(str, "RPCEmu Graphics") != NULL,
	      "help string names the module");
	check(str != NULL && strchr(str, '\t') != NULL,
	      "help string has the version after a tab, as *Modules expects");

	printf("\nthe module is marked 32-bit compatible\n");
	off = word_at(HDR_FLAGS * 4);
	check(off > 0 && off < rom_size && (word_at(off) & 1) != 0,
	      "flags word sets bit 0");

	printf("\nthe command table is well formed\n");
	off = word_at(HDR_TABLE * 4);
	while (off < rom_size && rom[off] != 0) {
		const char *name = (const char *) &rom[off];
		size_t entry;

		if (!string_at(off, &str)) {
			check(0, "command name is terminated");
			break;
		}
		off += strlen(name) + 1;
		off = (off + 3) & ~(size_t) 3;
		entry = off;
		off += 16;
		if (off > rom_size) {
			check(0, "command entry is inside the image");
			break;
		}
		printf("  *%s\n", name);
		check(word_at(entry) > 0 && word_at(entry) < rom_size,
		      "  code offset is inside the module");
		check(word_at(entry + 12) > 0 && word_at(entry + 12) < rom_size &&
		      string_at(word_at(entry + 12), &str) && str[0] == '*',
		      "  help text starts with the command");
		commands++;
	}
	check(commands == 4, "four commands: status, on, off and vars");

	printf("\nthe driver and the card agree\n");
	check(contains_word(GFXCARD_ID),
	      "the card's identity is in the driver image");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	free(rom);

	return failures ? 1 : 0;
}
