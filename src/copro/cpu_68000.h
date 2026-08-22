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

#ifndef CPU_68000_H
#define CPU_68000_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motorola 68000.
 *
 * A CPU and nothing else, on the same terms as the other cores here: a flat byte
 * array, no knowledge of OPEN Bus or the emulator, and its own tests. It reaches
 * the Macintosh Plus and SE, the Atari ST, the Amiga and the Mega Drive, which is
 * the widest single step this card can take.
 *
 * The whole design, and why each decision went the way it did, is in
 * docs/copro-68000.md. What matters when reading this file:
 *
 * ★ THE SYSTEM SIDE IS NOT OPTIONAL ON THIS PART. Both privilege levels, the
 * vector table at address 0, the three exception groups and their frames, and
 * seven interrupt levels. An Atari ST or Mac Plus ROM installs a vector table and
 * runs its operating system in supervisor mode within a few instructions of
 * reset, so a user-mode-only core could not begin to run one - and running one is
 * the reason to want this processor.
 *
 * ★ DECODED THROUGH A GENERATED 65,536-ENTRY TABLE. Not a switch. The 6809's
 * opcode map is a grid and can be taken apart arithmetically; this one cannot -
 * group &4xxx alone holds twenty unrelated instructions. The table is built once
 * at initialisation by asking, of every one of the 65,536 encodings, what it is;
 * the cycle costs are built in the same pass from the same rules, so an encoding
 * cannot end up valid with no cost. See cpu_68000_priv.h.
 *
 * ★ HOW A PROGRAM STOPS: the STOP instruction, which needs no special case. If
 * nothing interrupts the core it is finished and D0 is the exit code, as A is on
 * the Motorola 8-bits. If the guest offers an interrupt it resumes, which is what
 * the instruction means. SYNC and CWAI fault on the 6809, and WAI on the 6800,
 * only because those cards have no interrupt model to wait on.
 *
 * WHAT IS NOT HERE: undocumented behaviour, the 68010 and 68020, the FPU and any
 * MMU. RESET asserts a line no card here has, so it costs its cycles and does
 * nothing. The 68008 differs from this only in bus timing, so it is not offered
 * separately.
 *
 * THE ADDRESS SPACE is 16MB and addresses wrap at 24 bits, as the part's 24
 * address lines require. A card gives the core the RAM it was configured with and
 * the core still bounds-checks, because a smaller array is what a test hands it.
 */

/** Status register bits. The low byte is the condition codes. */
#define CPU68000_FLAG_C		0x0001	/**< carry */
#define CPU68000_FLAG_V		0x0002	/**< overflow */
#define CPU68000_FLAG_Z		0x0004	/**< zero */
#define CPU68000_FLAG_N		0x0008	/**< negative */
#define CPU68000_FLAG_X		0x0010	/**< extend: carry, but kept across moves */
#define CPU68000_SR_IMASK	0x0700	/**< interrupt mask, three bits */
#define CPU68000_SR_S		0x2000	/**< supervisor */
#define CPU68000_SR_T		0x8000	/**< trace */

/** The bits of the status register that exist. The rest read as zero. */
#define CPU68000_SR_VALID	(0x001fu | CPU68000_SR_IMASK | \
				 CPU68000_SR_S | CPU68000_SR_T)

/** Fault causes. These are the core giving up, not the 68000's own exceptions. */
#define CPU68000_FAULT_ILLEGAL	1	/**< not an instruction on this part */
#define CPU68000_FAULT_ACCESS	2	/**< outside the array it was given */
#define CPU68000_FAULT_NO_HANDLER 3	/**< an exception whose vector is unset */
#define CPU68000_FAULT_DOUBLE	4	/**< a fault while taking an exception */

/** Which instruction stopped the core. Only one does. */
#define CPU68000_HALT_STOP	1

/*
 * The exception vectors this core can take, by vector number. The table itself
 * is 256 entries at address 0 and a guest may use any of them; these are the ones
 * the core raises for itself.
 */
#define CPU68000_VEC_RESET_SSP	0	/**< read by hardware, not by us: see reset */
#define CPU68000_VEC_BUS_ERROR	2
#define CPU68000_VEC_ADDRESS_ERROR 3
#define CPU68000_VEC_ILLEGAL	4
#define CPU68000_VEC_DIV_ZERO	5
#define CPU68000_VEC_CHK	6
#define CPU68000_VEC_TRAPV	7
#define CPU68000_VEC_PRIVILEGE	8
#define CPU68000_VEC_TRACE	9
#define CPU68000_VEC_LINE_A	10	/**< an unimplemented &Axxx opcode */
#define CPU68000_VEC_LINE_F	11	/**< an unimplemented &Fxxx opcode */
#define CPU68000_VEC_AUTOVECTOR	24	/**< plus the level, 25 to 31 */
#define CPU68000_VEC_TRAP	32	/**< plus the TRAP number, 32 to 47 */

/** Operand sizes, in bytes, which is also what the encoding's size field means. */
typedef enum {
	M68K_BYTE = 1,
	M68K_WORD = 2,
	M68K_LONG = 4
} m68k_size;

typedef struct cpu68000_state {
	uint32_t d[8];		/**< data registers */
	uint32_t a[8];		/**< address registers; a[7] is the active stack */
	uint32_t usp;		/**< the inactive one is kept here or in ssp */
	uint32_t ssp;
	uint32_t pc;
	uint16_t sr;

	uint8_t *ram;		/**< the core's address space */
	uint32_t ram_size;	/**< its length */

	int halted;		/**< set by STOP with nothing to interrupt it */
	int halt_reason;	/**< CPU68000_HALT_* when halted */
	uint32_t exit_code;	/**< D0 as the core halted */

	int faulted;
	uint32_t fault_cause;	/**< CPU68000_FAULT_* */
	uint32_t fault_addr;	/**< the address, or the opcode if illegal */

	uint64_t cycles;

	cpu_mem_hook mem;

	int stalled;
	uint32_t stall_addr;
	int stall_is_write;

	/*
	 * ★ STOPPED IS NOT HALTED. STOP with interrupts still enabled leaves the
	 * core waiting rather than finished: it executes nothing, but an interrupt
	 * will start it again. The card reports it as halted so a guest that never
	 * interrupts sees a finished program, and clearing this is what an
	 * interrupt does.
	 */
	int stopped;

	/* What the current instruction has charged itself, beyond its table entry:
	   an effective address's cost, and a taken branch's. */
	int extra_cycles;

	/*
	 * ★ HOW FAR A MULTI-TRANSFER INSTRUCTION GOT. MOVEM moves up to sixteen
	 * registers in sixteen accesses and MOVEP up to four, so whole-instruction
	 * retry would repeat the ones already done - eight hardware registers
	 * written twice, with nothing reporting it. Those instructions read this on
	 * entry and continue from it, and it is the one piece of state a stall
	 * deliberately does NOT roll back. Zero means "start at the beginning".
	 */
	unsigned transfer_done;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. Only kept when the hook says it can stall: this
	   is 72 bytes, against 11 on the 6809. */
	struct {
		uint32_t d[8];
		uint32_t a[8];
		uint32_t usp, ssp, pc;
		uint16_t sr;
	} saved;
} cpu68000_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu68000_init(cpu68000_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Route this core's accesses through a hook instead of straight into its RAM.
 * Pass NULL to go back to the flat array. See cpu_mem.h.
 */
void cpu68000_set_mem_hook(cpu68000_state *s, const cpu_mem_hook *hook);

/**
 * Reset the core, with @entry as the first instruction.
 *
 * A real 68000 fetches the supervisor stack pointer from &000000 and the program
 * counter from &000004. The entry is passed in instead, as it is for every core
 * here, and the status register comes up supervisor with the interrupt mask at 7,
 * which is the real reset state. A guest booting a genuine ROM reads the reset
 * vector itself and sets the entry and the stack pointer; see docs/copro-68000.md.
 */
void cpu68000_reset(cpu68000_state *s, uint32_t entry);

/**
 * Offer an interrupt at @level, 1 to 7.
 *
 * Taken when the level is above the status register's mask, or when it is 7,
 * which is effectively non-maskable on this part. @vector is used when @autovec
 * is zero; otherwise the autovector for the level is used, which is what most
 * hardware does.
 *
 * Also restarts a core stopped by STOP, which is the other half of that
 * instruction's meaning.
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu68000_interrupt(cpu68000_state *s, unsigned level, uint8_t vector,
                       int autovec);

/**
 * Execute one instruction.
 *
 * @return what it cost in cycles, or 0 if the core is halted, faulted, stopped or
 *         waiting on a stalled access.
 */
int cpu68000_step(cpu68000_state *s);

/**
 * Run for up to @cycles cycles, stopping early on a halt, a fault or a stall.
 *
 * An instruction is indivisible, so this reaches the budget and then overshoots
 * by whatever the last instruction cost; it never stops short of it.
 *
 * @return the cycles actually used.
 */
int cpu68000_run(cpu68000_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu68000_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_68000_H */
