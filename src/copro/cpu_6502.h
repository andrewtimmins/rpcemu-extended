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

	uint64_t cycles;	/**< instructions retired since reset */
} cpu6502_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu6502_init(cpu6502_state *s, uint8_t *ram, uint32_t ram_size);

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
 * @return 1 if one executed, 0 if the core is halted or faulted. As with the
 *         other cores here the count is instructions retired, not bus cycles:
 *         see the note in openbus_coproc.h about why the card counts them that
 *         way and what that means for a program that measures itself.
 */
int cpu6502_step(cpu6502_state *s);

/** Execute up to @cycles instructions, stopping early on halt or fault. */
int cpu6502_run(cpu6502_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu6502_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_6502_H */
