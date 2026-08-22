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

#ifndef CPU_Z80_H
#define CPU_Z80_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zilog Z80.
 *
 * A CPU and nothing else, on the same terms as cpu_rv32i.h and cpu_6502.h. The
 * Z80 is here because it is the third distinct shape of processor: a 32-bit
 * load/store RISC, an 8-bit accumulator machine, and this - an 8-bit machine with
 * a second register set, 16-bit arithmetic, two index registers, prefixed opcode
 * pages and a separate 256-port input/output space. If the card interface can
 * carry all three then it is a card interface rather than a wrapper around one
 * core.
 *
 * WHAT IS HERE. The documented instruction set: the main page, the CB page of
 * rotates and bit operations, the ED page including the block move, block
 * compare and block input/output instructions, and the DD and FD pages that turn
 * HL into IX or IY, up to and including the DDCB and FDCB bit operations on
 * (IX+d). The second register set and EX AF,AF' / EXX with it.
 *
 * WHAT IS NOT, and this is worth reading before assuming a program will run. The
 * undocumented opcodes are faults, not behaviour: SLL, the IXH and IXL halves of
 * the index registers, ED pages that a real part treats as no-ops, and OUT
 * (C),0. Nothing interrupts the core, so IM 0/1/2, DI, EI, RETI and RETN keep the
 * state they set and no interrupt ever arrives to use it; I and R exist as
 * registers but R does not count refreshes, because there is no refresh here to
 * count. The undocumented flag bits 3 and 5 are maintained where the documented
 * ones are being computed anyway, since it costs nothing, but they are not the
 * point and are not what the tests pin down.
 *
 * HOW A PROGRAM STOPS. HALT halts the core, and the accumulator becomes the exit
 * code. On real hardware HALT executes no-ops until an interrupt arrives; here
 * nothing would ever arrive, so a core that faithfully spun would only burn the
 * host's timeslice for ever.
 *
 * THE INPUT/OUTPUT SPACE is 256 ports, and a card is what decides what they mean:
 * point io_in and io_out at the card and a port becomes a mailbox, a console byte
 * or whatever else that card wants. Left NULL they read and write a latch inside
 * this struct, which is what a unit test wants and what a program that only talks
 * to itself gets.
 */

/** Flag register bits, in their architectural positions. */
#define CPU_Z80_FLAG_C		0x01	/**< carry */
#define CPU_Z80_FLAG_N		0x02	/**< add/subtract */
#define CPU_Z80_FLAG_PV		0x04	/**< parity or overflow */
#define CPU_Z80_FLAG_X		0x08	/**< undocumented, copies result bit 3 */
#define CPU_Z80_FLAG_H		0x10	/**< half carry */
#define CPU_Z80_FLAG_Y		0x20	/**< undocumented, copies result bit 5 */
#define CPU_Z80_FLAG_Z		0x40	/**< zero */
#define CPU_Z80_FLAG_S		0x80	/**< sign */

/** Fault causes. */
#define CPU_Z80_FAULT_ILLEGAL	1	/**< an undocumented opcode */
#define CPU_Z80_FAULT_ACCESS	2	/**< outside the array it was given */

/** Which instruction stopped the core. Only one does. */
#define CPU_Z80_HALT_HALT	1

typedef struct cpu_z80_state {
	/* Main register set. Kept as separate bytes rather than a union over a
	   16-bit word, so that nothing here depends on the host's endianness. */
	uint8_t a, f;
	uint8_t b, c, d, e, h, l;

	/* The alternate set, reached by EX AF,AF' and EXX. */
	uint8_t a2, f2;
	uint8_t b2, c2, d2, e2, h2, l2;

	uint16_t ix, iy;
	uint16_t sp;
	uint16_t pc;

	uint8_t i;		/**< interrupt vector base */
	uint8_t r;		/**< refresh register; not incremented, see above */
	uint8_t iff1, iff2;	/**< interrupt enable latches */
	uint8_t im;		/**< interrupt mode, 0 to 2 */

	uint8_t *ram;		/**< the core's address space */
	uint32_t ram_size;	/**< its length; 65536 for a real address space */

	/**
	 * Input and output, if a card wants to provide them.
	 *
	 * @ctx is handed back untouched. Both NULL means the ports array below
	 * is the whole of the input/output space.
	 */
	uint8_t (*io_in)(void *ctx, uint8_t port);
	void (*io_out)(void *ctx, uint8_t port, uint8_t val);
	void *io_ctx;

	uint8_t ports[256];	/**< the latch used when io_in/io_out are NULL */

	/* Where accesses go when something other than a flat array is behind
	   them; ports come through here too, marked with CPU_MEM_PORT. Zeroed
	   means the RAM and the ports array above, as it always was. */
	cpu_mem_hook mem;

	/*
	 * ★ 8080 OR Z80, which is more than a question of which opcodes exist.
	 *
	 * Set for an Intel 8080. The Z80 is a superset of its instruction set, so
	 * one core covers both - but two behaviours differ and both are visible to
	 * real software:
	 *
	 *  - the parity/overflow bit is ALWAYS parity on an 8080, where the Z80
	 *    redefined it as overflow for arithmetic;
	 *  - DAA on an 8080 always adds, having no subtract flag to consult.
	 *
	 * Plus the obvious: the Z80's own instructions - the prefixes, the relative
	 * jumps, the alternate register set - do not exist and fault here.
	 */
	int i8080;

	/* What the last instruction cost in T-states, and whether a conditional
	   one was taken - the decode records it and the step charges for it. */
	uint8_t last_cycles;
	int branch_taken;

	int stalled;
	uint32_t stall_addr;	/**< the address that stalled, CPU_MEM_PORT and all */
	int stall_is_write;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. See cpu_mem.h. */
	struct {
		uint8_t a, f, b, c, d, e, h, l;
		uint8_t a2, f2, b2, c2, d2, e2, h2, l2;
		uint16_t ix, iy, sp, pc;
		uint8_t i, r, iff1, iff2, im;
	} saved;

	int halted;		/**< set by HALT */
	int halt_reason;	/**< CPU_Z80_HALT_* when halted */
	uint32_t exit_code;	/**< the accumulator as the core halted */

	int faulted;
	uint32_t fault_cause;	/**< CPU_Z80_FAULT_* */
	uint32_t fault_addr;	/**< the address, or the opcode if illegal */

	uint64_t cycles;	/**< T-states since reset, per the documented timing */
} cpu_z80_state;

/** Attach a core to its memory. @ram is not owned and must outlive the core. */
void cpu_z80_init(cpu_z80_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Reset the core, with @entry as the first instruction.
 *
 * A real Z80 starts at 0x0000 with no way to say otherwise; the entry is passed
 * in because a card is told where to start. Otherwise this is a Z80 reset: AF and
 * SP become 0xffff, interrupts are disabled, interrupt mode 0, and both register
 * sets are cleared.
 */
/**
 * Route this core's accesses through a hook instead of straight into its RAM and
 * ports array. NULL goes back to those. See cpu_mem.h.
 */
void cpu_z80_set_mem_hook(cpu_z80_state *s, const cpu_mem_hook *hook);

/**
 * Choose 8080 or Z80 behaviour. Call it after cpu_z80_init and before running.
 *
 * ★ An 8080 is NOT just a Z80 with fewer instructions. See the note on i8080 in
 * the state above for what else changes; a core that only rejected the extra
 * opcodes would make 8080 software appear to work and then get its flags wrong.
 */
void cpu_z80_set_8080(cpu_z80_state *s, int i8080);

/**
 * Raise the maskable interrupt, with @vector as the byte a device would put on
 * the data bus. Ignored unless IFF1 is set, as the hardware ignores it.
 *
 * Mode 1 jumps to &0038 and mode 2 reads the address from the table at I<<8 with
 * @vector as the index. Mode 0 would execute whatever instruction the device
 * placed on the bus; nothing here can place one, so it is treated as mode 1,
 * which is what mode 0 devices almost always arranged for anyway (an RST 38H).
 *
 * @return non-zero if the interrupt was taken.
 */
int cpu_z80_interrupt(cpu_z80_state *s, uint8_t vector);

/** Raise the non-maskable interrupt: &0066, and IFF1 clear. Never ignored. */
void cpu_z80_nmi(cpu_z80_state *s);

void cpu_z80_reset(cpu_z80_state *s, uint16_t entry);

/**
 * Execute one instruction, prefixes included.
 *
 * @return 1 if one executed, 0 if the core is halted or faulted. A prefixed
 *         instruction counts as one, not as one per byte.
 */
int cpu_z80_step(cpu_z80_state *s);

/** Execute up to @cycles instructions, stopping early on halt or fault. */
int cpu_z80_run(cpu_z80_state *s, int cycles);

/** Name of a fault cause, for logs and *commands. Never NULL. */
const char *cpu_z80_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_Z80_H */
