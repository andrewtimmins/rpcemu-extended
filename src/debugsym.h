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
 * debugsym.h - names for guest addresses
 *
 * Everything the debugger reports is an address. A backtrace of four hex
 * numbers is a fact about the machine but not an answer about the program, and
 * turning it into one means going away and reading a link map by hand. This
 * loads that map instead, so addresses can be reported as name+offset.
 *
 * Symbols are loaded from a text file the user supplies, one per line:
 *
 *     00008000 main
 *     &8100    parse_options     ; & is the RISC OS hex prefix
 *     0x8200   # comments and blank lines are skipped
 *
 * The address is hex, with or without an 0x or & prefix; the rest of the line
 * is the name, trimmed. There is no size field, deliberately - a second hex
 * column would be ambiguous with a name that begins with hex digits, and the
 * extent of a symbol is better taken from where the next one starts.
 *
 * A file is the whole of it. Reading the module chain out of the running guest
 * was considered and left out: it means depending on kernel workspace layout
 * that differs between RISC OS versions, and a symbol table that is confidently
 * wrong is worse than no symbol table - the entire value here is that the name
 * beside an address can be trusted.
 */

#ifndef DEBUGSYM_H
#define DEBUGSYM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load symbols from a file, replacing anything already loaded.
 *
 * @param path   File to read
 * @param[out] count  Number of symbols loaded, may be NULL
 * @param[out] error  Static description on failure, may be NULL
 * @return Non-zero on success
 */
int debugsym_load_file(const char *path, uint32_t *count, const char **error);

/** Discard all loaded symbols. */
void debugsym_clear(void);

/** How many symbols are loaded. */
uint32_t debugsym_count(void);

/**
 * Name the symbol containing an address.
 *
 * The containing symbol is the one with the greatest address not above the one
 * asked about, bounded by where the next symbol starts - so an address past
 * the end of the table does not get attributed to the last symbol in it.
 *
 * Safe to call while the machine is stopped mid-disassembly: it is a binary
 * search over a fixed array and allocates nothing.
 *
 * @param address Address to name
 * @param[out] offset  How far past the symbol's base, may be NULL
 * @return The name, or NULL if no symbol covers this address. Valid until the
 *         table is next loaded or cleared.
 */
const char *debugsym_lookup(uint32_t address, uint32_t *offset);

/**
 * Find a symbol by name, for turning "main" into an address.
 *
 * @param name  Exact name, case sensitive
 * @param[out] address  The symbol's address
 * @return Non-zero if the name was found
 */
int debugsym_resolve(const char *name, uint32_t *address);

/**
 * debugsym_lookup() in the shape arm_disasm_sym() wants.
 */
const char *debugsym_disasm_lookup(uint32_t address, uint32_t *offset, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DEBUGSYM_H */
