/*
  RPCEmu - An Acorn system emulator

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

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Suspend/resume machine state snapshots.

   The snapshot file is a fixed header followed by tagged chunks. Each
   emulation module provides a pair of functions writing/reading its guest
   state field-by-field with the helpers below (never raw structs - they may
   contain host pointers and padding). The format is little-endian.

   Serialization errors are latched in savestate_error rather than checked
   per call; state_save()/state_load() reset and inspect the latch. */

extern int savestate_error;

extern void savestate_write(FILE *f, const void *data, size_t len);
extern void savestate_write_u8(FILE *f, uint8_t v);
extern void savestate_write_u16(FILE *f, uint16_t v);
extern void savestate_write_u32(FILE *f, uint32_t v);
extern void savestate_write_i32(FILE *f, int32_t v);
extern void savestate_write_u64(FILE *f, uint64_t v);
extern void savestate_write_f64(FILE *f, double v);

/* Run-length-encoded variants for large, mostly-uniform payloads
   (RAM, VRAM, disc buffers); no external compression library needed */
extern void savestate_write_rle(FILE *f, const void *data, size_t len);
extern void savestate_read_rle(FILE *f, void *data, size_t len);

extern void savestate_read(FILE *f, void *data, size_t len);
extern uint8_t savestate_read_u8(FILE *f);
extern uint16_t savestate_read_u16(FILE *f);
extern uint32_t savestate_read_u32(FILE *f);
extern int32_t savestate_read_i32(FILE *f);
extern uint64_t savestate_read_u64(FILE *f);
extern double savestate_read_f64(FILE *f);

/* Write the complete machine state to 'path'. Must be called from the
   emulator thread between execrpcemu() calls. Returns 0 on success. */
extern int state_save(const char *path);

/* Check that a snapshot file exists and matches the current configuration
   (model, RAM/VRAM size, ROM image). Returns 0 if it can be resumed;
   otherwise fills errbuf with a reason. */
extern int state_check(const char *path, char *errbuf, size_t errbuf_len);

/* Read the machine name stored in a snapshot's header, without loading it.
   Returns 0 on success. */
extern int state_get_machine_name(const char *path, char *name, size_t name_len);

/* Restore the machine state from 'path'. Must be called from the emulator
   thread (or before it starts) after rpcemu_start(). Performs a machine
   reset first; on failure the machine is left cold-booted. Returns 0 on
   success. */
extern int state_load(const char *path);

/* Per-module state serialization, defined in the module owning the state */
extern void arm_savestate(FILE *f);      /* arm_common.c (both CPU cores) */
extern void arm_loadstate(FILE *f);
extern void cp15_savestate(FILE *f);
extern void cp15_loadstate(FILE *f);
extern void fpa_savestate(FILE *f);
extern void fpa_loadstate(FILE *f);
extern void mem_savestate(FILE *f);
extern void mem_loadstate(FILE *f);
extern void iomd_savestate(FILE *f);
extern void iomd_loadstate(FILE *f);
extern void vidc20_savestate(FILE *f);
extern void vidc20_loadstate(FILE *f);
extern void superio_savestate(FILE *f);
extern void superio_loadstate(FILE *f);
extern void i8042_savestate(FILE *f);
extern void i8042_loadstate(FILE *f);
extern void keyboard_savestate(FILE *f);
extern void keyboard_loadstate(FILE *f);
extern void ide_savestate(FILE *f);
extern void ide_loadstate(FILE *f);
extern void icside_savestate(FILE *f);
extern void icside_loadstate(FILE *f);
extern void fdc_savestate(FILE *f);
extern void fdc_loadstate(FILE *f);
extern void disc_savestate(FILE *f);
extern void disc_loadstate(FILE *f);
extern void adf_savestate(FILE *f);
extern void adf_loadstate(FILE *f);
extern void hfe_savestate(FILE *f);
extern void hfe_loadstate(FILE *f);
extern void cmos_savestate(FILE *f);
extern void cmos_loadstate(FILE *f);
extern void sound_savestate(FILE *f);
extern void sound_loadstate(FILE *f);
extern void rpcemu_savestate(FILE *f);  /* execution-loop state (rpcemu.c) */
extern void rpcemu_loadstate(FILE *f);
extern void hostfs_savestate(FILE *f);
extern void hostfs_loadstate(FILE *f);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* SAVESTATE_H */
