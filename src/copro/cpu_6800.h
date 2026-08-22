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

#ifndef CPU_6800_H
#define CPU_6800_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motorola 6800, and with it the 6802 and the 6808.
 *
 * A CPU and nothing else, on the same terms as the other cores here: a flat byte
 * array, no knowledge of OPEN Bus or the emulator, and its own unit test.
 *
 * ★ ONE CORE COVERS ALL THREE PARTS, and that is a fact about the hardware
 * rather than a shortcut. The 6802 is a 6800 with 128 bytes of RAM and a clock
 * oscillator on the chip; the 6808 is a 6802 without the RAM. Neither added an
 * instruction, a register or an addressing mode, so to a program there is
 * nothing to tell apart - and a card carries its own RAM here anyway, so even
 * the 6802's on-chip RAM is not a difference this can express. Offering the same
 * instruction set three times under three names would be three entries in a menu
 * that do the same thing.
 *
 * ★ AND WHY IT IS NOT A FLAG ON THE 6809, which is how the 65C02 and the 8080
 * are done. The 6809 was source compatible with the 6800: an assembler would
 * take 6800 source and produce 6809 binaries. It was NOT object-code compatible,
 * and the two disagree about most of the map:
 *
 *      opcode   on a 6800        on a 6809
 *      &08      INX              ASL direct
 *      &30      TSX              LEAX
 *      &36      PSHA             PSHU
 *      &8E      LDS              LDX
 *      &CE      LDX              LDU
 *
 * A flag would therefore have to change what nearly every opcode means, which is
 * not a flag, it is a second core wearing one. There are other structural
 * differences behind those: the 6800 has no direct-page register, so its direct
 * addressing is page zero and nowhere else; its indexed addressing is one form,
 * an unsigned 8-bit offset from X, where the 6809's is fourteen forms in a
 * postbyte; and it has one stack, one index register and no 16-bit accumulator.
 *
 * WHAT IS HERE. The 197 valid opcodes, all four addressing modes, both
 * accumulators, and the interrupts with their own vectors.
 *
 * WHAT IS NOT. Undocumented opcodes fault. WAI faults, for the reason SYNC does
 * on the 6809 and WAI does on the 65C02: it stops the processor until an
 * interrupt arrives, which is a state this card does not model.
 *
 * ★ HOW A PROGRAM STOPS. SWI halts the core with A as the exit code, as BRK does
 * on the 6502 and SWI does on the 6809. Unlike the 6809 there is no second or
 * third software interrupt to leave vectoring, so this is the only one.
 *
 * ★ THE QUIRKS WORTH HAVING THE PART FOR, since a processor that gets these
 * wrong is not the one anybody's software was written against:
 *
 *   - CPX sets N, Z and V but NOT the carry, which makes an unsigned 16-bit
 *     comparison genuinely awkward and is the single most complained-about thing
 *     about this processor. The 6809 fixed it.
 *   - INX and DEX affect Z and nothing else. No N, no V.
 *   - TST clears the carry, where the 6809's leaves it alone.
 *   - The shifts and rotates define V as N exclusive-or C AFTER the operation,
 *     which for LSR means V follows the bit shifted out - where the 6809 leaves
 *     V alone on an LSR entirely.
 *   - The stack pointer addresses the next FREE byte, one below the top of the
 *     stack, so a push stores and then decrements. TSX and TXS carry the
 *     resulting off-by-one on their faces: X becomes SP plus one, and SP becomes
 *     X minus one.
 *   - There is no BRN. &21 is not an instruction; the always-fail branch was a
 *     6809 addition.
 */

/** Condition code register bits, in their architectural positions. */
#define CPU6800_FLAG_C		0x01	/**< carry, or borrow after a subtract */
#define CPU6800_FLAG_V		0x02	/**< overflow */
#define CPU6800_FLAG_Z		0x04	/**< zero */
#define CPU6800_FLAG_N		0x08	/**< negative */
#define CPU6800_FLAG_I		0x10	/**< interrupt mask */
#define CPU6800_FLAG_H		0x20	/**< half carry, for DAA */

/** The top two bits of the condition codes are not used and read as ones. */
#define CPU6800_CC_ALWAYS	0xc0

/** Fault causes. */
#define CPU6800_FAULT_ILLEGAL	1	/**< an opcode that is not an instruction */
#define CPU6800_FAULT_ACCESS	2	/**< outside the array it was given */
#define CPU6800_FAULT_WAIT	3	/**< WAI; see the note above */

/** Which instruction stopped the core. Only one does. */
#define CPU6800_HALT_SWI	1

/** The vectors, at the top of memory as the hardware puts them. */
#define CPU6800_VEC_IRQ		0xfff8u
#define CPU6800_VEC_SWI		0xfffau
#define CPU6800_VEC_NMI		0xfffcu
#define CPU6800_VEC_RESET	0xfffeu

typedef struct cpu6800_state {
	uint8_t a;		/**< accumulator A */
	uint8_t b;		/**< accumulator B */
	uint16_t x;		/**< the index register, and there is only one */
	uint16_t sp;		/**< stack pointer, addressing the next free byte */
	uint8_t cc;		/**< condition codes */
	uint16_t pc;		/**< program counter */

	uint8_t *ram;		/**< the core's address space */
	uint32_t ram_size;	/**< its length; 65536 for a real address space */

	int halted;		/**< set by SWI */
	int halt_reason;	/**< CPU6800_HALT_* when halted */
	uint32_t exit_code;	/**< accumulator A as the core halted */

	int faulted;
	uint32_t fault_cause;	/**< CPU6800_FAULT_* */
	uint32_t fault_addr;	/**< the address, or the opcode if illegal */

	uint64_t cycles;	/**< cycles since reset, per the documented timing */

	/* Where accesses go when something other than a flat array is behind
	   them. Zeroed means the RAM below. See cpu_mem.h. */
	cpu_mem_hook mem;

	int stalled;
	uint32_t stall_addr;	/**< the address that stalled */
	int stall_is_write;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. Only kept when the hook says it can stall. */
	struct {
		uint8_t a, b, cc;
		uint16_t x, sp, pc;
	} saved;
} cpu6800_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu6800_init(cpu6800_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Route this core's accesses through a hook instead of straight into its RAM.
 * Pass NULL to go back to the flat array. See cpu_mem.h.
 */
void cpu6800_set_mem_hook(cpu6800_state *s, const cpu_mem_hook *hook);

/**
 * Reset the core, with @entry as the first instruction.
 *
 * A real 6800 fetches its entry point from the vector at &FFFE; the entry is
 * passed in instead, for the reason the other cores here take one. The interrupt
 * mask is set, which is the state a real part comes up in.
 */
void cpu6800_reset(cpu6800_state *s, uint16_t entry);

/**
 * Raise the maskable interrupt. Ignored while the I flag is set. Pushes the
 * program counter, the index register, both accumulators and the condition
 * codes - seven bytes - sets I, and vectors through &FFF8.
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu6800_irq(cpu6800_state *s);

/**
 * Raise the non-maskable interrupt, through the vector at &FFFC. Never ignored,
 * and pushes the same seven bytes.
 */
void cpu6800_nmi(cpu6800_state *s);

/**
 * Execute one instruction.
 *
 * @return what it cost in cycles, or 0 if the core is halted, faulted or waiting
 *         on a stalled access. Charged per the documented timing. Note that on
 *         this processor an INDEXED instruction costs more than the extended
 *         form of the same thing, the offset having to be added; on the 6809 it
 *         is the other way round.
 */
int cpu6800_step(cpu6800_state *s);

/**
 * Run for up to @cycles cycles, stopping early on a halt, a fault or a stall.
 *
 * An instruction is indivisible, so this reaches the budget and then overshoots
 * by whatever the last instruction cost; it never stops short of it.
 *
 * @return the cycles actually used.
 */
int cpu6800_run(cpu6800_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu6800_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_6800_H */
