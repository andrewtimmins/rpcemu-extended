/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

#ifndef ROMLOAD_H
#define ROMLOAD_H

#include "rpcemu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	RomAddressing_26Bit,
	RomAddressing_32Bit,
	RomAddressing_Unknown
} RomAddressing;

extern uint32_t rom_loaded_size; /**< Size in bytes of the loaded ROM image */
extern uint32_t rom_loaded_crc;  /**< CRC32 of the raw ROM image, before rom_patch.c
                                      applies host-dependent patches. Used for
                                      snapshot ROM-identity validation. */

void loadroms(void);

/** Non-zero if this machine model can run 32-bit ROM images (RISC OS 5+). */
int model_supports_32bit_rom(Model model);

/**
 * Inspect ROM files and guess whether they require 26-bit or 32-bit CPU support.
 *
 * @param rom_dir  ROM subdirectory/file within roms/, or empty for all of roms/
 * @param detail   Optional buffer for a short description (e.g. "RISC OS 5.30")
 * @param detail_len Length of detail buffer
 */
RomAddressing rom_probe_addressing(const char *rom_dir, char *detail, size_t detail_len);

/**
 * Check whether a ROM set is compatible with the selected machine model.
 *
 * @return 1 if compatible, 0 if not
 */
int rom_model_is_compatible(Model model, const char *rom_dir, char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* ROMLOAD_H */
