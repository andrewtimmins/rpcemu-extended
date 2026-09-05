/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef EMULATOR_SNAPSHOT_H
#define EMULATOR_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

#include "machine_snapshot.h"

MachineSnapshot emulator_take_snapshot();

/**
 * The result of a debug read: the bytes, and which of them could be read at all.
 *
 * Unmapped bytes come back as zero, which is indistinguishable from a page of
 * genuine zeros, so the caller is told which is which rather than being left to
 * present one as the other. Issue #258: a hex dump of zeros for an address that
 * is simply not mapped reads as "the debugger is wrong" and is how the
 * untranslated read went unnoticed.
 */
struct MemoryRead {
	std::vector<uint8_t> data;	/**< Byte values, zero where unmapped */
	std::vector<uint8_t> mapped;	/**< 1 per readable byte, parallel to data */
};

/**
 * Read guest memory for display, without disturbing the machine.
 *
 * Virtual by default, which is what every other address in the inspector means
 * and what the debug socket's `mem` command does; pass @physical for a raw
 * read of the same number. Never fires a watchpoint and never leaves an abort
 * pending, so looking at memory cannot change what the machine then does.
 *
 * @param address  Virtual address, or physical when @physical is set
 * @param length   Bytes to read, capped at 4096
 * @param physical Treat @address as already physical
 */
MemoryRead emulator_read_memory(uint32_t address, uint32_t length,
                                bool physical = false);

std::string emulator_disassemble_at(uint32_t address, int count);

#endif
