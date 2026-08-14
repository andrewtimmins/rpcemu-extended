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

/*
 * What the CPU hands an exception handler.
 *
 * WHY THIS EXISTS. exception() decides four things every time it runs: which
 * mode is entered, what return address the handler gets in R14, what the saved
 * status register holds, and whether interrupts end up disabled. Get any of them
 * wrong and the handler runs but returns to the wrong place, which does not look
 * like a CPU bug - it looks like the guest going mad some time later, usually as
 * a wild branch a long way from the cause.
 *
 * None of it was covered by a test. It was reviewed by reading, three times,
 * and called correct all three times, which is worth exactly nothing: the whole
 * point of a return-address bug is that reading the code that computes it tells
 * you what the author intended, not what the architecture requires.
 *
 * THE CASE THAT MATTERS MOST is the middle one below: 26-bit code interrupted on
 * a 32-bit-configured CPU. That is what every Archimedes game under ADFFS does,
 * thousands of times a second, and it became far hotter this year when the MMU
 * started raising the permission faults ADFFS's JIT relies on. It is also the
 * least exercised path in the emulator, because nothing else reaches it: RISC OS
 * 5 is 32-bit throughout and RISC OS 3.1x is 26-bit throughout.
 *
 * Expected values are from the ARM ARM (DDI 0100, rev E for the 26-bit detail),
 * not from the emulator: a test that copies the implementation's arithmetic
 * agrees with it by construction and checks nothing.
 *
 * Note there are two exception() implementations, arm.c for the interpreter and
 * arm_dynarec.c for the recompiler. Whichever is linked is the one tested, so
 * running the suite in both configurations covers both.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "arm_common.h"
#include "mem.h"
#include "cp15.h"

/* Vector values the emulator's callers pass. They are the architectural vector
   plus four, because the interpreter's main loop adds four to R15 after the
   instruction, so PC lands on the vector itself. */
#define VEC_UNDEFINED	0x08	/* architectural 0x04 */
#define VEC_SWI		0x0c	/* architectural 0x08 */
#define VEC_PREFETCH	0x10	/* architectural 0x0c */
#define VEC_DATA_ABORT	0x14	/* architectural 0x10 */
#define VEC_IRQ		0x1c	/* architectural 0x18 */
#define VEC_FIQ		0x20	/* architectural 0x1c */

/* The return-address adjustment each exception requires, DDI 0100 rev E:
   a data abort and an interrupt take the instruction's R15 unaltered, while a
   prefetch abort, an undefined instruction and a SWI want four less. */
#define DIFF_DATA_ABORT	0
#define DIFF_IRQ	0
#define DIFF_PREFETCH	4
#define DIFF_UNDEFINED	4
#define DIFF_SWI	4

#define PSR_N		0x80000000u
#define PSR_C		0x20000000u
#define R15_26_I	0x08000000u	/* bit 27 in a 26-bit R15 */
#define R15_26_F	0x04000000u	/* bit 26 in a 26-bit R15 */
#define CPSR_I		0x80u
#define CPSR_F		0x40u

static int failures = 0;

static void
check(const char *what, uint32_t got, uint32_t want)
{
	int ok = (got == want);

	printf("  %-58s %s", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf("  (got %08x, want %08x)", got, want);
		failures++;
	}
	printf("\n");
}

static void
check_bool(const char *what, int ok)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/**
 * Put the core into a known 32-bit mode with known flags.
 *
 * @param mode  Full 32-bit mode value, e.g. 0x10 for User
 * @param pc    Value for R15, which in a 32-bit mode is the address alone
 * @param flags Extra CPSR bits to set beyond the mode
 */
static void
enter_32bit(uint32_t mode, uint32_t pc, uint32_t flags)
{
	arm_reset(CPUModel_SA110);
	prog32 = 1;
	updatemode(mode);
	arm.reg[16] = mode | flags;
	arm.reg[15] = pc;
}

/**
 * Put the core into a 26-bit mode. R15 carries the flags, the interrupt
 * disables and the mode, as it does on a 26-bit CPU.
 *
 * @param prog32_on Whether the CPU is in 32-bit program configuration - the
 *                  difference between RISC OS 3.1x (0) and a 26-bit task under
 *                  RISC OS 5 (1)
 * @param mode      26-bit mode, 0 User, 1 FIQ, 2 IRQ, 3 Supervisor
 * @param pc        Address part of R15
 * @param psr       Flag and interrupt-disable bits to place in R15
 */
static void
enter_26bit(int prog32_on, uint32_t mode, uint32_t pc, uint32_t psr)
{
	arm_reset(CPUModel_SA110);
	prog32 = prog32_on;
	updatemode(mode);
	arm.reg[15] = (pc & 0x3fffffc) | psr | mode;
	arm.reg[16] = (arm.reg[16] & ~0x1fu) | mode;
}

/* ------------------------------------------------------------------ 32-bit */

static void
test_32bit_data_abort(void)
{
	const uint32_t pc = 0x00008100;
	uint32_t entry_cpsr;

	printf("32-bit config: data abort from User mode\n");

	enter_32bit(0x10, pc, PSR_N | PSR_C);
	entry_cpsr = arm.reg[16];

	exception(ABORT, VEC_DATA_ABORT, DIFF_DATA_ABORT);

	check("enters Abort mode (0x17)", arm.mode, 0x17);
	/* R14_abt is the aborting instruction's R15 unaltered. Anything else and
	   the handler retries the wrong instruction, or skips it. */
	check("R14_abt is the aborting instruction's R15", arm.reg[14], pc);
	check("SPSR_abt is the CPSR as it was", arm.spsr[ABORT], entry_cpsr);
	check("PC is the data abort vector", arm.reg[15], VEC_DATA_ABORT);
	check_bool("IRQs are disabled on entry", (arm.reg[16] & CPSR_I) != 0);
	check_bool("FIQs are NOT disabled by a data abort",
	           (arm.reg[16] & CPSR_F) == 0);
	check_bool("the saved PSR did not have interrupts disabled",
	           (arm.spsr[ABORT] & CPSR_I) == 0);
}

static void
test_32bit_prefetch_abort_and_undefined(void)
{
	const uint32_t pc = 0x00008100;

	printf("32-bit config: prefetch abort and undefined instruction\n");

	enter_32bit(0x10, pc, 0);
	exception(ABORT, VEC_PREFETCH, DIFF_PREFETCH);
	check("prefetch abort: R14 is four less than a data abort's",
	      arm.reg[14], pc - 4);
	check("prefetch abort: enters Abort mode", arm.mode, 0x17);
	check("prefetch abort: PC is its own vector", arm.reg[15], VEC_PREFETCH);

	enter_32bit(0x10, pc, 0);
	exception(UNDEFINED, VEC_UNDEFINED, DIFF_UNDEFINED);
	check("undefined: enters Undefined mode (0x1b)", arm.mode, 0x1b);
	check("undefined: R14 is four less than R15", arm.reg[14], pc - 4);
	check("undefined: uses its own SPSR bank",
	      arm.spsr[UNDEFINED], 0x10);
}

static void
test_32bit_interrupts(void)
{
	const uint32_t pc = 0x00008100;

	printf("32-bit config: IRQ and FIQ\n");

	enter_32bit(0x10, pc, 0);
	exception(IRQ, VEC_IRQ, DIFF_IRQ);
	check("IRQ: enters IRQ mode (0x12)", arm.mode, 0x12);
	check("IRQ: R14_irq is R15 unaltered", arm.reg[14], pc);
	check_bool("IRQ: IRQs disabled", (arm.reg[16] & CPSR_I) != 0);
	check_bool("IRQ: FIQs left enabled", (arm.reg[16] & CPSR_F) == 0);

	enter_32bit(0x10, pc, 0);
	exception(FIQ, VEC_FIQ, DIFF_IRQ);
	check("FIQ: enters FIQ mode (0x11)", arm.mode, 0x11);
	/* A FIQ masks both, which is the whole point of it. */
	check_bool("FIQ: BOTH IRQs and FIQs disabled",
	           (arm.reg[16] & (CPSR_I | CPSR_F)) == (CPSR_I | CPSR_F));
}

/* ------------------------------- 26-bit code on a 32-bit-configured CPU ---- */

static void
test_prog32_data_abort_from_user26(void)
{
	const uint32_t pc = 0x00125200;
	const uint32_t psr = PSR_N | PSR_C;

	printf("26-bit task on a 32-bit OS: data abort from User26  [the ADFFS case]\n");

	enter_26bit(1, 0 /* User26 */, pc, psr);

	exception(ABORT, VEC_DATA_ABORT, DIFF_DATA_ABORT);

	/* The CPU enters the 32-BIT flavour of the exception mode: that is what
	   prog32 means. Entering a 26-bit mode here would leave the handler with
	   no SPSR to return through. */
	check("enters Abort mode (0x17), the 32-bit flavour", arm.mode, 0x17);

	/* R14 must be the return ADDRESS only. The PSR bits that shared R15 in
	   26-bit mode belong in the SPSR now, and leaving them in R14 would send
	   the handler's return to an address with flag bits in the top of it -
	   which is one way to end up branching into page zero. */
	check("R14 is the return address with no PSR bits in it",
	      arm.reg[14], pc);
	check_bool("R14 has nothing above bit 25",
	           (arm.reg[14] & ~0x3fffffcu) == 0);

	/* The SPSR has to record that the interrupted code was 26-bit User, or
	   the eventual return puts it in the wrong mode. Bit 4 clear is what
	   says "a 26-bit mode". */
	check("SPSR records the interrupted 26-bit mode (User26)",
	      arm.spsr[ABORT] & 0x1f, 0);
	check_bool("SPSR says 26-bit, i.e. bit 4 clear",
	           (arm.spsr[ABORT] & 0x10) == 0);
	check("SPSR keeps the flags R15 was carrying",
	      arm.spsr[ABORT] & 0xf0000000, psr);
	check_bool("SPSR records interrupts as they were, enabled",
	           (arm.spsr[ABORT] & (CPSR_I | CPSR_F)) == 0);

	check("PC is the data abort vector", arm.reg[15], VEC_DATA_ABORT);
	check_bool("IRQs disabled on entry", (arm.reg[16] & CPSR_I) != 0);
}

static void
test_prog32_preserves_disabled_interrupts(void)
{
	const uint32_t pc = 0x00125200;

	printf("26-bit task on a 32-bit OS: interrupt state is carried across\n");

	/* Interrupted code that already had IRQs off must come back with them
	   off. Losing this re-enables interrupts inside a critical section on
	   return, which corrupts whatever the guest was protecting. */
	enter_26bit(1, 0, pc, R15_26_I);
	exception(ABORT, VEC_DATA_ABORT, DIFF_DATA_ABORT);
	check_bool("R15's I bit becomes the SPSR's I bit",
	           (arm.spsr[ABORT] & CPSR_I) != 0);

	enter_26bit(1, 0, pc, R15_26_F);
	exception(ABORT, VEC_DATA_ABORT, DIFF_DATA_ABORT);
	check_bool("R15's F bit becomes the SPSR's F bit",
	           (arm.spsr[ABORT] & CPSR_F) != 0);
}

static void
test_prog32_irq_from_user26(void)
{
	const uint32_t pc = 0x00126400;

	printf("26-bit task on a 32-bit OS: IRQ from User26\n");

	enter_26bit(1, 0, pc, 0);
	exception(IRQ, VEC_IRQ, DIFF_IRQ);

	check("enters IRQ mode (0x12), the 32-bit flavour", arm.mode, 0x12);
	check("R14_irq is the return address", arm.reg[14], pc);
	check("SPSR records User26", arm.spsr[IRQ] & 0x1f, 0);
	check("PC is the IRQ vector", arm.reg[15], VEC_IRQ);
}

static void
test_prog32_irq_from_svc26(void)
{
	const uint32_t pc = 0x00126400;

	printf("26-bit task on a 32-bit OS: IRQ from Supervisor26\n");

	/* Not the same as from User26: the mode the SPSR must record differs, and
	   getting it wrong returns privileged code to unprivileged mode - or the
	   reverse, which is worse. */
	enter_26bit(1, 3 /* SVC26 */, pc, 0);
	exception(IRQ, VEC_IRQ, DIFF_IRQ);

	check("enters IRQ mode (0x12)", arm.mode, 0x12);
	check("SPSR records Supervisor26, not User26",
	      arm.spsr[IRQ] & 0x1f, 3);
	check_bool("SPSR still says 26-bit", (arm.spsr[IRQ] & 0x10) == 0);
	check("R14_irq is the return address", arm.reg[14], pc);
}

/* --------------------------------------------- pure 26-bit configuration -- */

static void
test_26bit_data_abort_enters_svc(void)
{
	const uint32_t pc = 0x00008100;
	const uint32_t psr = PSR_N;

	printf("26-bit config (RISC OS 3.1x): data abort enters Supervisor26\n");

	enter_26bit(0, 0 /* User26 */, pc, psr);

	exception(ABORT, VEC_DATA_ABORT, DIFF_DATA_ABORT);

	/* A 26-bit CPU has only User, FIQ, IRQ and Supervisor. Aborts and
	   undefined instructions therefore enter Supervisor, and a test that
	   expected an Abort mode here would be testing a mode that does not
	   exist. */
	check("enters Supervisor mode", arm.mode, SUPERVISOR);
	check("R14_svc keeps the PSR bits, as a 26-bit R15 does",
	      arm.reg[14], (pc & 0x3fffffc) | psr);
	check("R15's mode bits become Supervisor", arm.reg[15] & 3, 3);
	check("R15's address is the vector", arm.reg[15] & 0x3fffffc,
	      VEC_DATA_ABORT);
	check("R15 keeps the flags it had", arm.reg[15] & 0xf0000000, psr);
	check_bool("R15's I bit is set, disabling IRQs",
	           (arm.reg[15] & R15_26_I) != 0);
	check_bool("R15's F bit is untouched by a data abort",
	           (arm.reg[15] & R15_26_F) == 0);
}

static void
test_26bit_undefined_enters_svc(void)
{
	const uint32_t pc = 0x00008100;

	printf("26-bit config: undefined instruction also enters Supervisor26\n");

	enter_26bit(0, 0, pc, 0);
	exception(UNDEFINED, VEC_UNDEFINED, DIFF_UNDEFINED);

	check("enters Supervisor mode", arm.mode, SUPERVISOR);
	check("R14 is four less than R15", arm.reg[14] & 0x3fffffc, pc - 4);
	check("R15's address is the undefined vector",
	      arm.reg[15] & 0x3fffffc, VEC_UNDEFINED);
}

static void
test_26bit_irq_enters_irq26(void)
{
	const uint32_t pc = 0x00008100;

	printf("26-bit config: IRQ enters IRQ26, not Supervisor\n");

	enter_26bit(0, 0, pc, 0);
	exception(IRQ, VEC_IRQ, DIFF_IRQ);

	/* IRQ and FIQ do exist in 26-bit, so they must NOT be folded into
	   Supervisor the way aborts are. */
	check("enters IRQ mode", arm.mode, IRQ);
	check("R15's mode bits become IRQ", arm.reg[15] & 3, IRQ);

	enter_26bit(0, 0, pc, 0);
	exception(FIQ, VEC_FIQ, DIFF_IRQ);
	check("FIQ enters FIQ mode", arm.mode, FIQ);
	check("R15's mode bits become FIQ", arm.reg[15] & 3, FIQ);
	check_bool("FIQ sets both I and F in R15",
	           (arm.reg[15] & (R15_26_I | R15_26_F)) == (R15_26_I | R15_26_F));
}

/* ------------------------------------------------------- banked registers -- */

static void
test_link_register_is_banked(void)
{
	const uint32_t pc = 0x00008100;

	printf("The return address goes in the handler's own R14, not the caller's\n");

	enter_32bit(0x10, pc, 0);
	arm.reg[14] = 0xdeadbeef;	/* the interrupted code's R14 */

	exception(IRQ, VEC_IRQ, DIFF_IRQ);

	check("R14_irq holds the return address", arm.reg[14], pc);

	/* And the interrupted mode's R14 must still be there when we go back,
	   or the exception has quietly destroyed a live register. */
	updatemode(0x10);
	check("User mode's R14 survived the exception", arm.reg[14], 0xdeadbeef);
}

static void
test_caller_constants(void)
{
	const uint32_t pc = 0x00008100;

	printf("The one caller that can be reached directly\n");

	/* Everything above tests exception() given a vector and an adjustment.
	   This pins one real caller's choice of both, which is the other half of
	   being correct: arm_exception_undefined() is the only one reachable
	   without executing an instruction. */
	enter_32bit(0x10, pc, 0);
	arm_exception_undefined();

	check("arm_exception_undefined enters Undefined mode", arm.mode, 0x1b);
	check("arm_exception_undefined sets R14 to R15 - 4", arm.reg[14], pc - 4);
	check("arm_exception_undefined uses the undefined vector",
	      arm.reg[15], VEC_UNDEFINED);
}

int
main(void)
{
	printf("Exception delivery\n\n");

	/* The core's own setup, as tests/test_mmu_perms.c does: updatemode()
	   reaches into the memory maps to follow the privilege level. */
	mem_init();
	mem_reset(16, 2);
	cp15_reset(CPUModel_SA110);

	test_32bit_data_abort();
	test_32bit_prefetch_abort_and_undefined();
	test_32bit_interrupts();

	test_prog32_data_abort_from_user26();
	test_prog32_preserves_disabled_interrupts();
	test_prog32_irq_from_user26();
	test_prog32_irq_from_svc26();

	test_26bit_data_abort_enters_svc();
	test_26bit_undefined_enters_svc();
	test_26bit_irq_enters_irq26();

	test_link_register_is_banked();
	test_caller_constants();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
