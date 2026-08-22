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

#ifndef CPU_6502_H
#define CPU_6502_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MOS 6502, the NMOS part.
 *
 * A CPU and nothing else, on the same terms as cpu_rv32i.h: a flat byte array,
 * no knowledge of OPEN Bus or the emulator, and its own unit test. There is a
 * pleasing symmetry in a Risc PC hosting one, since the machine's ancestry runs
 * back through the Electron and the BBC Micro to this processor, and the second
 * processor idea itself is the BBC's Tube.
 *
 * WHAT IS HERE. All 151 documented opcodes, thirteen addressing modes, binary
 * and decimal arithmetic, and the NMOS indirect-JMP page-crossing bug, which is
 * emulated on purpose and noted where it happens - a 6502 that gets that wrong
 * is not the processor anybody is nostalgic about.
 *
 * WHAT IS NOT. The undocumented opcodes (SLO, RLA, LAX and the rest of them)
 * fault as illegal rather than doing what a real NMOS part happened to do; there
 * are no interrupts into the core, so IRQ, NMI and the vectors at $FFFA-$FFFF
 * are unused; and RTI exists but has nothing to return from unless a program has
 * pushed a frame itself.
 *
 * HOW A PROGRAM STOPS. BRK halts the core. On real hardware it pushes a frame
 * and vectors through $FFFE, but there is no operating system here to vector to,
 * and "stop, and hand back the accumulator" is what a program on a co-processor
 * card wants to do at its end. The accumulator becomes the exit code.
 *
 * THE ADDRESS SPACE is the 6502's own 64K, and addresses wrap at 16 bits as the
 * hardware does. A card gives the core a full 64K so a fault cannot arise, but
 * the core still checks: a smaller array is exactly what a test wants to hand it.
 */

/** Status register bits, in their architectural positions. */
#define CPU6502_FLAG_C		0x01	/**< carry */
#define CPU6502_FLAG_Z		0x02	/**< zero */
#define CPU6502_FLAG_I		0x04	/**< interrupt disable */
#define CPU6502_FLAG_D		0x08	/**< decimal mode */
#define CPU6502_FLAG_B		0x10	/**< break, only ever seen on the stack */
#define CPU6502_FLAG_U		0x20	/**< unused, reads as one */
#define CPU6502_FLAG_V		0x40	/**< overflow */
#define CPU6502_FLAG_N		0x80	/**< negative */

/** Fault causes. */
#define CPU6502_FAULT_ILLEGAL	1	/**< an undocumented opcode */
#define CPU6502_FAULT_ACCESS	2	/**< outside the array it was given */

/** Which instruction stopped the core. Only one does. */
#define CPU6502_HALT_BRK	1

typedef struct cpu6502_state {
	uint8_t a;		/**< accumulator */
	uint8_t x;		/**< index X */
	uint8_t y;		/**< index Y */
	uint8_t sp;		/**< stack pointer, stack lives at $0100 + sp */
	uint8_t p;		/**< status register */
	uint16_t pc;		/**< program counter */

	uint8_t *ram;		/**< the core's address space */
	uint32_t ram_size;	/**< its length; 65536 for a real address space */

	int halted;		/**< set by BRK */
	int halt_reason;	/**< CPU6502_HALT_* when halted */
	uint32_t exit_code;	/**< the accumulator as the core halted */

	int faulted;
	uint32_t fault_cause;	/**< CPU6502_FAULT_* */
	uint32_t fault_addr;	/**< the address, or the opcode if illegal */

	uint64_t cycles;	/**< cycles since reset, per the documented timing */

	/* Where accesses go when something other than a flat array is behind
	   them. Zeroed means the RAM below, as it always was. See cpu_mem.h. */
	cpu_mem_hook mem;

	/* A stalled access: the instruction was abandoned and will be retried
	   once the guest has answered. See the note in cpu_mem.h. */
	/*
	 * ★ CMOS OR NMOS, which is the difference between a 6502 and a 65C02.
	 *
	 * Set for a 65C02: it adds instructions the NMOS part does not have, fixes
	 * the JMP (abs) page-boundary bug, and sets N and Z properly after decimal
	 * arithmetic. One core with a flag rather than a copy of the file, because
	 * the two share every one of the 151 documented NMOS opcodes and a copy
	 * would be 700 lines that have to be fixed twice.
	 */
	int cmos;

	/* Set by an indexed addressing mode when the index carried into the high
	   byte of the address, which costs a real 6502 a cycle on a read. */
	int page_crossed;
	/* Set by a branch that was taken, and hence cost more than its base. */
	int branch_taken;

	int stalled;
	uint32_t stall_addr;	/**< the address that stalled */
	int stall_is_write;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. Only kept when the hook says it can stall. */
	struct {
		uint8_t a, x, y, sp, p;
		uint16_t pc;
	} saved;
} cpu6502_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu6502_init(cpu6502_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Route this core's accesses through a hook instead of straight into its RAM.
 * Pass NULL to go back to the flat array. See cpu_mem.h, including what a
 * stalling hook assumes about the instruction that stalls.
 */
void cpu6502_set_mem_hook(cpu6502_state *s, const cpu_mem_hook *hook);

/**
 * Choose CMOS (65C02) or NMOS (6502) behaviour. Call it after cpu6502_init and
 * before running: it changes which opcodes exist, so switching it under a
 * running program would change the meaning of code already fetched.
 *
 * ★ What a 65C02 adds: BRA, PHX/PHY/PLX/PLY, STZ, TRB/TSB, INC A / DEC A, BIT
 * with immediate and indexed addressing, the (zp) indirect forms of the ALU
 * operations, JMP (abs,X) and STP. What it fixes: JMP (abs) no longer reads its
 * high byte from the wrong page, and N and Z are meaningful after decimal
 * arithmetic.
 *
 * ★ What it does NOT add: the Rockwell RMB/SMB/BBR/BBS bit instructions, which
 * are an R65C02 extension rather than part of the CMOS part every 65C02 has, and
 * WAI, which waits for an interrupt - a state this card does not model. Both
 * fault rather than doing something plausible.
 */
void cpu6502_set_cmos(cpu6502_state *s, int cmos);

/**
 * Raise the maskable interrupt. Ignored when the I flag is set, as the hardware
 * ignores it: pushes the program counter and the status register, sets I, and
 * jumps through the vector at &FFFE.
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu6502_irq(cpu6502_state *s);

/**
 * Raise the non-maskable interrupt, through the vector at &FFFA. Never ignored,
 * which is what non-maskable means.
 */
void cpu6502_nmi(cpu6502_state *s);

/**
 * Reset the core, with @entry as the first instruction.
 *
 * A real 6502 fetches its entry point from the vector at $FFFC; the entry is
 * passed in instead because a card is told where to start by the guest, and
 * insisting on a vector would mean every program had to plant one. Registers
 * become zero, the stack pointer $FD and the status register the interrupt
 * disable plus the always-set bit, which is the state a real part comes up in.
 */
void cpu6502_reset(cpu6502_state *s, uint16_t entry);

/**
 * Execute one instruction.
 *
 * @return what it cost in cycles, or 0 if the core is halted, faulted or waiting
 *         on a stalled access. Charged per the documented NMOS timing, including
 *         the extra cycle an indexed read pays for a carry into the high byte and
 *         the cost of a taken branch. See the timing note in openbus_coproc.h.
 */
int cpu6502_step(cpu6502_state *s);

/**
 * Run for up to @cycles cycles, stopping early on a halt, a fault or a stall.
 *
 * An instruction is indivisible, so this reaches the budget and then overshoots
 * by whatever the last instruction cost; it never stops short of it.
 *
 * @return the cycles actually used.
 */
int cpu6502_run(cpu6502_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu6502_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_6502_H */
