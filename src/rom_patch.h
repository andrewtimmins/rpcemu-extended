/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025 Andy Timmins

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

#ifndef ROM_PATCH_H
#define ROM_PATCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply every ROM patch appropriate to the image currently loaded in rom[].
 *
 * This is the single place all of RPCEmu's ROM modifications live. It is called
 * once by loadroms() after the ROM has been read (and endian-swapped on
 * big-endian hosts). Each individual patch decides for itself whether it
 * applies, keyed on the ROM contents and the current machine/config, and does
 * nothing (leaving the ROM untouched) when it does not match - so passing a ROM
 * that none of the patches recognise is always safe.
 *
 * @param rom_bytes Number of bytes actually loaded into the ROM image
 */
void rom_patch_apply(size_t rom_bytes);

/**
 * Rebuild and republish the monitor EDID for the screen size now configured.
 *
 * For a size changed while the machine runs. The rebuilt block is what the
 * graphics card serves over DDC from now on; RISC OS reads the EDID when its
 * display driver starts, so the change reaches the guest on the next driver
 * start or the next boot, not immediately.
 */
void rom_patch_refresh_monitor_edid(void);

#ifdef __cplusplus
}
#endif

#endif /* ROM_PATCH_H */
