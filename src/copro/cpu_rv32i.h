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

#ifndef CPU_RV32I_H
#define CPU_RV32I_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RV32IM - the 32-bit RISC-V base integer instruction set, plus the M extension
 * for multiply and divide.
 *
 * This is a CPU and nothing else: it executes out of a flat byte array handed to
 * it and knows nothing about OPEN Bus, the emulator or wxWidgets. That is what
 * lets tests/test_cpu_rv32i.c run it against a few hundred bytes of hand-written
 * machine code on every platform. openbus_coproc.c is what makes it a card.
 *
 * Why RV32I and not a second ARM: a second ARM would mean making our own core
 * re-entrant, since ARMState is one global, and OPEN Bus serialises the bus
 * anyway so there would be nothing to show for it. A CPU we have never
 * implemented brings its own small struct and collides with nothing. RISC-V in
 * particular has under fifty instructions in its base set, and the payload we
 * run on it is a file we compile and ship ourselves rather than somebody else's
 * ROM image.
 *
 * WHAT IS HERE. The whole of RV32I and the whole of RV32M, user-mode, with
 * strict natural alignment on loads and stores. Little-endian, as RISC-V is.
 *
 * WHAT IS NOT, said plainly rather than left to be discovered: no privileged
 * architecture, no CSRs (so no Zicsr, and `csrrw` is an illegal instruction), no
 * A, F, D or C extensions, no interrupts into the core, and no memory ordering
 * to get wrong - FENCE executes as a no-op, which is a correct implementation on
 * a core that completes every access before the next instruction. Compile with
 * `-march=rv32im -mabi=ilp32` and nothing here will surprise you.
 *
 * HOW A PROGRAM STOPS. `ecall` and `ebreak` both halt the core rather than
 * trapping, because there is no machine mode here to trap to. a0 (x10) is kept
 * as the exit code, which is the calling convention a C `main` already uses, and
 * halt_reason says which of the two it was. A card raises its interrupt when the
 * core halts, so the guest learns of it without polling.
 */

/** Number of integer registers. x0 reads as zero and discards writes. */
#define RV32I_REGS		32

/** Fault causes. These are RISC-V mcause exception codes, for familiarity. */
#define RV32I_FAULT_INSN_MISALIGNED	0
#define RV32I_FAULT_INSN_ACCESS		1
#define RV32I_FAULT_ILLEGAL		2
#define RV32I_FAULT_LOAD_MISALIGNED	4
#define RV32I_FAULT_LOAD_ACCESS		5
#define RV32I_FAULT_STORE_MISALIGNED	6
#define RV32I_FAULT_STORE_ACCESS	7

/** Which instruction stopped the core. */
#define RV32I_HALT_ECALL	1
#define RV32I_HALT_EBREAK	2

typedef struct rv32i_state {
	uint32_t x[RV32I_REGS];	/**< integer registers; x[0] is always zero */
	uint32_t pc;		/**< address of the next instruction to execute */

	uint8_t *ram;		/**< the core's whole address space */
	uint32_t ram_size;	/**< its length in bytes */

	int halted;		/**< set by ecall or ebreak; nothing runs after */
	int halt_reason;	/**< RV32I_HALT_* when halted */
	uint32_t exit_code;	/**< a0 as it was when the core halted */

	int faulted;		/**< set on a fault; the core stops as well */
	uint32_t fault_cause;	/**< RV32I_FAULT_* */
	uint32_t fault_addr;	/**< the address or instruction that faulted */

	uint64_t cycles;	/**< instructions retired since reset; see the note
	                             on timing in openbus_coproc.h for why this core
	                             counts instructions and the 8-bit ones do not */
	/* Where accesses go when something other than a flat array is behind
	   them. Zeroed means the RAM above, as it always was. See cpu_mem.h. */
	cpu_mem_hook mem;

	int stalled;
	uint32_t stall_addr;
	int stall_is_write;

	/* The registers as they were before the current instruction, restored if
	   it has to be abandoned. */
	struct {
		uint32_t x[32];
		uint32_t pc;
	} saved;

} rv32i_state;

/**
 * Attach a core to its memory.
 *
 * @ram must outlive the core and is not owned by it. Contents are left alone, so
 * a caller can load a program before or after this call.
 */
void rv32i_init(rv32i_state *s, uint8_t *ram, uint32_t ram_size);

/**
 * Reset the core, with @entry as the address of the first instruction.
 *
 * Every register becomes zero, exactly as the state a RISC-V harness starts a
 * program in. The stack pointer is NOT set up: a program that wants a stack sets
 * sp itself, or the loader does it through rv32i_state.
 */
/**
 * Route this core's accesses through a hook instead of straight into its RAM.
 * NULL goes back to the array. See cpu_mem.h.
 */
void rv32i_set_mem_hook(rv32i_state *s, const cpu_mem_hook *hook);

void rv32i_reset(rv32i_state *s, uint32_t entry);

/**
 * Execute one instruction.
 *
 * @return cycles consumed, which is 1 for anything that executed and 0 if the
 *         core is halted or faulted. One instruction is one cycle here: this is
 *         a co-processor on a bus that stalls the host while it runs, so a
 *         pretence of per-instruction timing would only make the host slower for
 *         no gain in fidelity.
 */
int rv32i_step(rv32i_state *s);

/**
 * Execute until @cycles are used up, or the core halts or faults.
 *
 * @return how many were used, so a caller can charge them against its own
 *         budget.
 */
int rv32i_run(rv32i_state *s, int cycles);

/** Name of a fault cause, for logs and the module's *command. Never NULL. */
const char *rv32i_fault_name(uint32_t cause);

#ifdef __cplusplus
}
#endif

#endif /* CPU_RV32I_H */
