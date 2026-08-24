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

/**
 * The RISC OS version a ROM set carries, read from the MOS title string.
 *
 * Separate from rom_probe_addressing(), which stops looking once an image is
 * large enough to settle 26-bit against 32-bit on size alone and so reports no
 * version for the very images most likely to need one.
 *
 * @param rom_dir ROM subdirectory/file within roms/, or empty for all of roms/
 * @param major   Filled in with the major version, may be NULL
 * @param minor   Filled in with the minor version, may be NULL
 * @return non-zero if a version was found
 */
int rom_probe_os_version(const char *rom_dir, int *major, int *minor);

/**
 * Can this ROM drive the emulated graphics card?
 *
 * The card is a GraphicsV display driver, and GraphicsV exists only in RISC OS
 * 5. Note that this is not "5 or later": RISC OS 6 is a different line, from
 * RISC OS 4, and has no display driver interface at all.
 *
 * A ROM whose version cannot be read is allowed, so an unrecognised one is
 * never locked out of a feature it might support.
 *
 * @param msg     Filled in with the reason when the answer is no, may be NULL
 * @return 1 if the card can be used, 0 if this ROM certainly cannot drive it
 */
int rom_supports_gfxcard(const char *rom_dir, char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* ROMLOAD_H */
