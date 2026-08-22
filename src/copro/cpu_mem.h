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

#ifndef CPU_MEM_H
#define CPU_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How a core reaches memory when something other than a flat array is behind
 * it.
 *
 * Every core here funnels all of its accesses through one read and one write
 * helper, so a machine emulated on top of a core - a Spectrum, a BBC, a Mac -
 * only needs those two to go somewhere else. With no hook set a core indexes its
 * RAM directly exactly as it always has, which is what keeps the cores usable on
 * their own and their existing tests honest.
 *
 * The cores stay ignorant of what is behind the hook: copro_bus.c is what knows
 * about regions, logs and latches, and it is not mentioned here.
 */

/**
 * OR'd into the address to mark an access to the PORT space rather than memory.
 *
 * Only the Z80 has a separate input/output space, and it is 16 bits wide at
 * most, so bit 16 of the address is free and one hook pair covers both spaces.
 * A hook that does not care can mask it off; a hook that does is spared a second
 * pair of function pointers it would have to keep in step with the first.
 */
#define CPU_MEM_PORT	0x10000u

/** What an access through a hook did. */
typedef enum {
	CPU_MEM_OK = 0,		/**< done; a read's value is in *value */
	CPU_MEM_STALL,		/**< the core must stop and be resumed */
	CPU_MEM_BUSERROR	/**< nothing is mapped there */
} cpu_mem_result;

typedef cpu_mem_result (*cpu_mem_read_fn)(void *ctx, uint32_t addr, uint8_t *value);
typedef cpu_mem_result (*cpu_mem_write_fn)(void *ctx, uint32_t addr, uint8_t value);

/**
 * A core's memory hook. read and write both NULL, which is what a zeroed state
 * gives, means "index the RAM directly".
 *
 * @can_stall says whether the thing behind the hook can ever answer
 * CPU_MEM_STALL. When it cannot - which is every use of these cores that does
 * not map a stalling region - the core skips saving its registers before each
 * instruction, and that is the whole reason the flag exists rather than being
 * inferred.
 */
typedef struct {
	void *ctx;
	cpu_mem_read_fn read;
	cpu_mem_write_fn write;
	int can_stall;
} cpu_mem_hook;

/*
 * ★ HOW A STALL WORKS, AND THE ONE THING IT ASSUMES.
 *
 * A core cannot suspend half way through an instruction: the instruction is a C
 * function and its progress lives in that function's locals. Doing it properly
 * would mean a coroutine with its own stack per core, or writing every
 * instruction as a state machine.
 *
 * So instead a stalled access ABANDONS the instruction and it is retried. The
 * core saves its registers before each instruction, and when an access stalls it
 * restores them, puts the program counter back, and stops without counting the
 * instruction. The guest supplies the value, and when the core runs again the
 * same instruction executes from the beginning - the retried access finds the
 * guest's answer waiting for it and the instruction completes normally.
 *
 * ★ WHAT THAT ASSUMES: that a stalling access is the first thing in the
 * instruction to have an effect outside the registers. Restoring registers
 * undoes register changes, and putting the program counter back undoes the
 * fetch, but nothing can undo a write that already reached memory. An
 * instruction that stores to RAM and only then reads a stalling address would
 * perform that store twice.
 *
 * That is not a theoretical worry to wave away, it is a real limit, and it is
 * narrow for a real reason: hardware registers are read as an instruction's
 * operand, before it computes or stores anything. A read-modify-write on a
 * stalling address is safe, because the stall happens on its read. Two different
 * stalling addresses in one instruction is the case to avoid, and the guest
 * chooses which regions stall, so it can.
 */

#ifdef __cplusplus
}
#endif

#endif
