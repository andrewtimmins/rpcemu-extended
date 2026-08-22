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

#ifndef CPU_6809_H
#define CPU_6809_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motorola 6809.
 *
 * A CPU and nothing else, on the same terms as the other cores here: a flat byte
 * array, no knowledge of OPEN Bus or the emulator, and its own unit test. It is
 * the largest of the 8-bit cores by a wide margin, and the reason is the
 * addressing rather than the instruction count.
 *
 * ★ WHY THIS ONE IS NOT A 6502 WITH DIFFERENT MNEMONICS. Its opcode map is
 * regular - four addressing modes down the columns of one table - but one of
 * those four is "indexed", and indexed on a 6809 is a whole addressing language
 * in a postbyte: a constant offset of five, eight or sixteen bits from any of
 * four registers, an offset taken from A, B or D, auto-increment and
 * auto-decrement by one or two, program-counter relative, and an indirect form
 * of nearly all of them. That is a decode of its own, and it is where a 6809
 * spends its complexity.
 *
 * WHAT IS HERE. The documented instruction set: all three pages, both
 * accumulators and the 16-bit D they form, X, Y, both stack pointers, the direct
 * page register, and every documented addressing mode including the whole
 * postbyte language above. The interrupts are here too, all three lines with
 * their own vectors and their own idea of how much state to push.
 *
 * WHAT IS NOT. Undocumented opcodes fault as illegal rather than doing whatever
 * a real part happened to do. SYNC and CWAI fault as well, and that is worth
 * saying plainly rather than burying: both of them stop the processor until an
 * interrupt arrives, which is a state this card does not model, exactly as WAI
 * is left out of the 65C02 for the same reason. Software written for a Dragon or
 * a CoCo that idles in SYNC will stop on it.
 *
 * ★ HOW A PROGRAM STOPS. SWI halts the core, with A as the exit code, which is
 * the same bargain BRK makes on the 6502: on real hardware it pushes a frame and
 * vectors, but there is no operating system here to vector to, and "stop, and
 * hand back an accumulator" is what a program on a co-processor card wants at
 * its end. SWI2 and SWI3 do vector, because those are the two a guest operating
 * system would claim and a program that calls one expects to come back.
 *
 * THE ADDRESS SPACE is the 6809's own 64K and addresses wrap at 16 bits, as the
 * hardware does. A card gives the core a full 64K so a fault cannot arise, but
 * the core still checks, because a smaller array is exactly what a test wants to
 * hand it.
 */

/** Condition code register bits, in their architectural positions. */
#define CPU6809_FLAG_C		0x01	/**< carry, or borrow after a subtract */
#define CPU6809_FLAG_V		0x02	/**< overflow */
#define CPU6809_FLAG_Z		0x04	/**< zero */
#define CPU6809_FLAG_N		0x08	/**< negative */
#define CPU6809_FLAG_I		0x10	/**< IRQ mask */
#define CPU6809_FLAG_H		0x20	/**< half carry, for DAA */
#define CPU6809_FLAG_F		0x40	/**< FIRQ mask */
#define CPU6809_FLAG_E		0x80	/**< entire state was pushed */

/** Fault causes. */
#define CPU6809_FAULT_ILLEGAL	1	/**< an undocumented opcode */
#define CPU6809_FAULT_ACCESS	2	/**< outside the array it was given */
#define CPU6809_FAULT_POSTBYTE	3	/**< an indexed postbyte with no meaning */
#define CPU6809_FAULT_WAIT	4	/**< SYNC or CWAI; see the note above */

/** Which instruction stopped the core. Only one does. */
#define CPU6809_HALT_SWI	1

/** The vectors, at the top of memory as the hardware puts them. */
#define CPU6809_VEC_SWI3	0xfff2u
#define CPU6809_VEC_SWI2	0xfff4u
#define CPU6809_VEC_FIRQ	0xfff6u
#define CPU6809_VEC_IRQ		0xfff8u
#define CPU6809_VEC_SWI		0xfffau
#define CPU6809_VEC_NMI		0xfffcu
#define CPU6809_VEC_RESET	0xfffeu

typedef struct cpu6809_state {
	uint8_t a;		/**< accumulator A, the high half of D */
	uint8_t b;		/**< accumulator B, the low half of D */
	uint16_t x;		/**< index X */
	uint16_t y;		/**< index Y */
	uint16_t u;		/**< user stack pointer */
	uint16_t s;		/**< hardware stack pointer */
	uint8_t dp;		/**< direct page: the high byte of a direct address */
	uint8_t cc;		/**< condition codes */
	uint16_t pc;		/**< program counter */

	uint8_t *ram;		/**< the core's address space */
	uint32_t ram_size;	/**< its length; 65536 for a real address space */

	int halted;		/**< set by SWI */
	int halt_reason;	/**< CPU6809_HALT_* when halted */
	uint32_t exit_code;	/**< accumulator A as the core halted */

	int faulted;
	uint32_t fault_cause;	/**< CPU6809_FAULT_* */
	uint32_t fault_addr;	/**< the address, or the opcode if illegal */

	uint64_t cycles;	/**< cycles since reset, per the documented timing */

	/* Where accesses go when something other than a flat array is behind
	   them. Zeroed means the RAM below. See cpu_mem.h. */
	cpu_mem_hook mem;

	int stalled;
	uint32_t stall_addr;	/**< the address that stalled */
	int stall_is_write;

	/* Costs the instruction worked out for itself, added to its table entry:
	   what an indexed postbyte charged, how many bytes a stack instruction
	   moved, and the nine extra a full RTI pays. None of them can live in a
	   table, because the opcode alone does not decide them. */
	int extra_cycles;
	/* Set by a long conditional branch that was taken, and hence cost one
	   more than the table's entry. The short branches cost the same either
	   way, which is why this is not set for them. */
	int branch_taken;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. Only kept when the hook says it can stall. */
	struct {
		uint8_t a, b, dp, cc;
		uint16_t x, y, u, s, pc;
	} saved;
} cpu6809_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu6809_init(cpu6809_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Route this core's accesses through a hook instead of straight into its RAM.
 * Pass NULL to go back to the flat array. See cpu_mem.h, including what a
 * stalling hook assumes about the instruction that stalls.
 */
void cpu6809_set_mem_hook(cpu6809_state *s, const cpu_mem_hook *hook);

/**
 * Reset the core, with @entry as the first instruction.
 *
 * A real 6809 fetches its entry point from the vector at &FFFE; the entry is
 * passed in instead, for the reason the other cores here take one - a card is
 * told where to start by the guest, and insisting on a vector would mean every
 * program had to plant one. The direct page becomes zero and both interrupt
 * masks are set, which is the state a real part comes up in.
 */
void cpu6809_reset(cpu6809_state *s, uint16_t entry);

/**
 * Raise the maskable interrupt. Ignored while the I flag is set, as the hardware
 * ignores it. Pushes the entire register state, sets I, and vectors through
 * &FFF8.
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu6809_irq(cpu6809_state *s);

/**
 * Raise the fast interrupt. Ignored while the F flag is set.
 *
 * ★ Fast because of what it does NOT push: the program counter and the condition
 * codes only, with E left clear to record that, where IRQ and NMI push all
 * eleven bytes. RTI reads E to know how much to take back off, so a handler that
 * pushes more itself and does not restore it will return to the wrong place.
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu6809_firq(cpu6809_state *s);

/**
 * Raise the non-maskable interrupt, through the vector at &FFFC. Never ignored,
 * which is what non-maskable means; pushes the entire state and sets both masks.
 */
void cpu6809_nmi(cpu6809_state *s);

/**
 * Execute one instruction.
 *
 * @return what it cost in cycles, or 0 if the core is halted, faulted or waiting
 *         on a stalled access. Charged per the documented timing, including what
 *         an indexed operand's postbyte costs and the extra cycle a taken long
 *         branch pays. See the timing note in openbus_coproc.h.
 */
int cpu6809_step(cpu6809_state *s);

/**
 * Run for up to @cycles cycles, stopping early on a halt, a fault or a stall.
 *
 * An instruction is indivisible, so this reaches the budget and then overshoots
 * by whatever the last instruction cost; it never stops short of it.
 *
 * @return the cycles actually used.
 */
int cpu6809_run(cpu6809_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu6809_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_6809_H */
