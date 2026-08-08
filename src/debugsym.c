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
 * debugsym.c - names for guest addresses
 *
 * A sorted array searched by bisection. Loading allocates and sorts; lookup
 * does neither, because it is called from disassembly and from the backtrace
 * walk, both of which run on the emulator thread with the machine stopped
 * between instructions.
 *
 * The one judgement call in here is what NOT to name. A table of symbols
 * covers the addresses its author knew about, and the debugger will be asked
 * about plenty of others - ROM, module workspace, whatever a wild branch
 * landed on. Attributing those to whichever symbol happens to sit below them
 * would produce confident nonsense like "main+0x3f21c" for an address in the
 * kernel, and a name that cannot be trusted is worse than a bare address,
 * because a bare address is obviously a question and a wrong name looks like
 * an answer. So a symbol reaches only as far as the next one, and the last
 * symbol in the table reaches SYM_LAST_EXTENT and no further.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugsym.h"

/** Longest symbol name kept, including the terminator. */
#define SYM_NAME_MAX 64

/* An upper bound on a table, so a file that is not a symbol file cannot eat
   the host's memory before being recognised as one. Comfortably more than any
   real RISC OS link map. */
#define SYM_MAX_COUNT 200000u

/* How far the last symbol in the table is taken to extend. Every other symbol
   ends where the next begins; the last one has nothing to bound it, and
   without a limit it would swallow the whole address space above it. */
#define SYM_LAST_EXTENT 0x10000u

typedef struct {
	uint32_t address;
	char name[SYM_NAME_MAX];
} Symbol;

static Symbol *symbols;
static uint32_t symbol_count;
static uint32_t symbol_capacity;

static int
symbol_compare(const void *a, const void *b)
{
	const Symbol *sa = a;
	const Symbol *sb = b;

	if (sa->address < sb->address) {
		return -1;
	}
	if (sa->address > sb->address) {
		return 1;
	}
	return 0;
}

void
debugsym_clear(void)
{
	free(symbols);
	symbols = NULL;
	symbol_count = 0;
	symbol_capacity = 0;
}

uint32_t
debugsym_count(void)
{
	return symbol_count;
}

static int
symbol_append(uint32_t address, const char *name)
{
	if (symbol_count >= SYM_MAX_COUNT) {
		return 0;
	}

	if (symbol_count == symbol_capacity) {
		uint32_t capacity = symbol_capacity ? symbol_capacity * 2 : 256;
		Symbol *grown = realloc(symbols, capacity * sizeof(Symbol));

		if (grown == NULL) {
			return 0;
		}
		symbols = grown;
		symbol_capacity = capacity;
	}

	symbols[symbol_count].address = address;
	strncpy(symbols[symbol_count].name, name, SYM_NAME_MAX - 1);
	symbols[symbol_count].name[SYM_NAME_MAX - 1] = '\0';
	symbol_count++;

	return 1;
}

/**
 * Parse one line into an address and a name.
 *
 * @return 1 if a symbol was parsed, 0 for a blank or comment line, -1 if the
 *         line was neither
 */
static int
parse_line(char *line, uint32_t *address, const char **name)
{
	char *p = line;
	char *end;
	char *start;
	unsigned long value;

	/* Strip a trailing newline and any comment */
	p = strpbrk(line, "\r\n");
	if (p != NULL) {
		*p = '\0';
	}
	p = strpbrk(line, "#;");
	if (p != NULL) {
		*p = '\0';
	}

	p = line;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '\0') {
		return 0;		/* blank, or a whole-line comment */
	}

	/* Address: hex, with or without an 0x or & prefix */
	if (*p == '&') {
		p++;
	} else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
	}

	value = strtoul(p, &end, 16);
	if (end == p) {
		return -1;		/* no address here */
	}
	if (*end != ' ' && *end != '\t') {
		return -1;		/* address runs into something else */
	}

	/* Name: the rest of the line, trimmed at both ends */
	start = end;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (*start == '\0') {
		return -1;		/* an address with no name */
	}

	end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*end = '\0';

	*address = (uint32_t) value;
	*name = start;
	return 1;
}

int
debugsym_load_file(const char *path, uint32_t *count, const char **error)
{
	FILE *f;
	char line[512];
	uint32_t loaded = 0;
	unsigned long line_number = 0;

	if (error != NULL) {
		*error = NULL;
	}
	if (count != NULL) {
		*count = 0;
	}
	if (path == NULL || path[0] == '\0') {
		if (error != NULL) {
			*error = "no path given";
		}
		return 0;
	}

	f = fopen(path, "r");
	if (f == NULL) {
		if (error != NULL) {
			*error = "cannot open the file";
		}
		return 0;
	}

	/* Only discarded once the file is known to be readable, so a failed
	   load leaves the previous table in place rather than in pieces. */
	debugsym_clear();

	while (fgets(line, sizeof(line), f) != NULL) {
		uint32_t address = 0;
		const char *name = NULL;
		int parsed;

		line_number++;
		parsed = parse_line(line, &address, &name);

		if (parsed == 0) {
			continue;
		}
		if (parsed < 0) {
			/* A file that is not a symbol file should be rejected
			   rather than silently yielding a handful of symbols
			   from whichever of its lines happened to parse. */
			debugsym_clear();
			fclose(f);
			if (error != NULL) {
				*error = "not a symbol file "
				         "(expected '<hex address> <name>')";
			}
			return 0;
		}

		if (!symbol_append(address, name)) {
			debugsym_clear();
			fclose(f);
			if (error != NULL) {
				*error = "too many symbols, or out of memory";
			}
			return 0;
		}
		loaded++;
	}

	fclose(f);

	if (loaded == 0) {
		debugsym_clear();
		if (error != NULL) {
			*error = "the file contains no symbols";
		}
		return 0;
	}

	qsort(symbols, symbol_count, sizeof(Symbol), symbol_compare);

	if (count != NULL) {
		*count = loaded;
	}
	return 1;
}

const char *
debugsym_lookup(uint32_t address, uint32_t *offset)
{
	uint32_t low = 0;
	uint32_t high = symbol_count;
	uint32_t found;
	uint32_t limit;

	if (symbol_count == 0) {
		return NULL;
	}
	if (address < symbols[0].address) {
		return NULL;
	}

	/* Greatest symbol address not above the one asked about */
	while (low + 1 < high) {
		uint32_t mid = low + (high - low) / 2;

		if (symbols[mid].address <= address) {
			low = mid;
		} else {
			high = mid;
		}
	}
	found = low;

	/* A symbol reaches as far as the next one, and no further - see the
	   note at the top of this file about not naming what we do not know. */
	if (found + 1 < symbol_count) {
		limit = symbols[found + 1].address;
	} else {
		limit = symbols[found].address + SYM_LAST_EXTENT;
	}

	if (address >= limit) {
		return NULL;
	}

	if (offset != NULL) {
		*offset = address - symbols[found].address;
	}
	return symbols[found].name;
}

int
debugsym_resolve(const char *name, uint32_t *address)
{
	uint32_t i;

	if (name == NULL || name[0] == '\0') {
		return 0;
	}

	/* Linear: resolving a name happens when a person types one, not on any
	   path that runs per instruction. */
	for (i = 0; i < symbol_count; i++) {
		if (strcmp(symbols[i].name, name) == 0) {
			if (address != NULL) {
				*address = symbols[i].address;
			}
			return 1;
		}
	}
	return 0;
}

const char *
debugsym_disasm_lookup(uint32_t address, uint32_t *offset, void *ctx)
{
	(void) ctx;
	return debugsym_lookup(address, offset);
}
