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
 * A CPU mode the architecture reserves, and what the emulator does about it.
 *
 * WHY THIS EXISTS. Nine of the sixteen values the mode field can hold are
 * reserved: 4, 5, 6, 8, 9, 10, 12, 13 and 14. Writing one is UNPREDICTABLE on
 * real hardware, so anything that does it is a bug in the guest - and the guest
 * is exactly where somebody would be looking when it happens. updatemode() used
 * to answer with fatal(), which took the whole emulator down and with it the
 * registers, the memory and the disassembly that would have said which
 * instruction did it. That is issue #227.
 *
 * The machine now halts into the debugger instead, so the state at the moment
 * of the write survives to be looked at. This checks the halt happens, that it
 * names the reserved mode and the instruction, and - the half that is easy to
 * lose - that the register banks are still coherent afterwards, because a halt
 * the user cannot single-step away from is not much better than an exit.
 *
 * ALL NINE reserved values are covered, not the one that was hand-tested, and
 * so are the seven legal modes: the change is a new arm of a switch that every
 * mode change in the emulator runs through, and the way to get that wrong is to
 * catch a mode that was working before.
 *
 * Links rpcemu_core for the real updatemode(), register banks and debugger
 * state, all of which are file-static; the public API is the only way in.
 */

#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "arm_common.h"
#include "mem.h"
#include "cp15.h"	/* dcache: isblockvalid() keys off it, so 0 is the interpreter */

/* Where the test pretends the offending instruction is. R15 leads the
   instruction by eight in a 32-bit mode, which is what PC in arm.h undoes. */
#define TEST_PC		0x00108000u
#define TEST_R15	(TEST_PC + 8u)

/* Values planted in the user bank, to be found again in a reserved mode. */
#define USER_R13	0x0d0d0d0du
#define USER_R14	0x0e0e0e0eu

/* Where an instruction that is actually executed lives. RAM, so it can be
   written and fetched; the machine is set to 32-bit Supervisor for it. */
#define CODE_PC		0x10000000u

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_hex(const char *what, uint32_t got, uint32_t want)
{
	const int ok = (got == want);

	printf("  %-64s %s", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf("  (got %08x, want %08x)", got, want);
		failures++;
	}
	printf("\n");
}

/**
 * A 32-bit Supervisor machine with a known PC and a known user bank, and a
 * debugger that is enabled, idle and holding no trace events.
 */
static void
reset_machine(void)
{
	DebugTraceConfig cfg;
	DebugTraceEvent drain[8];
	uint32_t dropped;

	arm_reset(CPUModel_SA110);
	prog32 = 1;
	updatemode(0x10 | SUPERVISOR);
	arm.reg[15] = TEST_R15;
	arm.reg[16] = 0x10 | SUPERVISOR;

	/* R13 and R14 of the user bank are not aliased in Supervisor mode, so
	   these survive until something switches to a mode that uses them. */
	arm.user_reg[13] = USER_R13;
	arm.user_reg[14] = USER_R14;

	config.debug_enabled = 1;

	debugger_resume();
	debugger_clear_breakpoints();
	debugger_clear_watchpoints();
	memset(&cfg, 0, sizeof(cfg));
	debugger_set_trace_config(&cfg);

	while (debugger_drain_trace_events(drain, 8, &dropped) > 0) {
		/* discard whatever an earlier case left behind */
	}
}

/**
 * One reserved mode value: the machine must halt rather than exit, and must
 * still be a machine afterwards.
 */
static void
test_reserved_mode(uint32_t mode_bits)
{
	DebuggerStatus status;
	DebugTraceEvent events[8];
	uint32_t dropped = 0;
	uint32_t count;
	char what[128];

	printf("Reserved mode %u (CPSR mode %02x)\n", mode_bits, 0x10u | mode_bits);

	reset_machine();

	/* Reaching the next line at all is the fix: this used to call fatal(),
	   which the test stub turns into exit(2). */
	updatemode(0x10 | mode_bits);

	debugger_get_status(&status);

	snprintf(what, sizeof(what), "mode %02x: the machine is halted", 0x10u | mode_bits);
	check(what, status.paused != 0);

	snprintf(what, sizeof(what), "mode %02x: halted because of the mode", 0x10u | mode_bits);
	check(what, status.reason == DebugPauseReason_BadMode);

	snprintf(what, sizeof(what), "mode %02x: halt names the instruction", 0x10u | mode_bits);
	check_hex(what, status.halt_pc, TEST_PC);

	/* The trace ring carries the mode itself, which the halt PC cannot. */
	count = debugger_drain_trace_events(events, 8, &dropped);
	snprintf(what, sizeof(what), "mode %02x: one trace event, no drops", 0x10u | mode_bits);
	check(what, count == 1 && dropped == 0);

	if (count == 1) {
		snprintf(what, sizeof(what), "mode %02x: event is an exception", 0x10u | mode_bits);
		check(what, events[0].type == TraceEvent_Exception);

		snprintf(what, sizeof(what), "mode %02x: event kind is the reserved mode",
		         0x10u | mode_bits);
		check(what, events[0].arg0 == TraceException_BadMode);

		snprintf(what, sizeof(what), "mode %02x: event carries the mode asked for",
		         0x10u | mode_bits);
		check_hex(what, events[0].arg1, 0x10u | mode_bits);

		snprintf(what, sizeof(what), "mode %02x: event carries the PC", 0x10u | mode_bits);
		check_hex(what, events[0].pc, TEST_PC);
	}

	/*
	 * Coherence. A halt is only useful if the machine can be looked at and
	 * then stepped, so the banked registers must hold something defined: the
	 * user bank, as User and System mode get.
	 */
	snprintf(what, sizeof(what), "mode %02x: R13 comes from the user bank", 0x10u | mode_bits);
	check_hex(what, arm.reg[13], USER_R13);

	snprintf(what, sizeof(what), "mode %02x: R14 comes from the user bank", 0x10u | mode_bits);
	check_hex(what, arm.reg[14], USER_R14);

	snprintf(what, sizeof(what), "mode %02x: usrregs point at the live registers",
	         0x10u | mode_bits);
	check(what, usrregs[13] == &arm.reg[13] && usrregs[14] == &arm.reg[14]);

	/* Leaving again must work, or a halt cannot be recovered from. */
	updatemode(0x10 | SUPERVISOR);
	snprintf(what, sizeof(what), "mode %02x: Supervisor can be re-entered", 0x10u | mode_bits);
	check_hex(what, arm.mode, 0x10u | SUPERVISOR);
}

/**
 * The seven legal modes, which this change must not have caught.
 */
static void
test_legal_modes(void)
{
	static const struct {
		uint32_t bits;
		const char *name;
	} modes[] = {
		{ USER,		"User" },
		{ FIQ,		"FIQ" },
		{ IRQ,		"IRQ" },
		{ SUPERVISOR,	"Supervisor" },
		{ ABORT,	"Abort" },
		{ UNDEFINED,	"Undefined" },
		{ SYSTEM,	"System" },
	};

	printf("Legal modes\n");

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		DebuggerStatus status;
		char what[128];

		reset_machine();
		updatemode(0x10 | modes[i].bits);
		debugger_get_status(&status);

		snprintf(what, sizeof(what), "%s: entered", modes[i].name);
		check_hex(what, arm.mode, 0x10u | modes[i].bits);

		snprintf(what, sizeof(what), "%s: the machine keeps running", modes[i].name);
		check(what, status.paused == 0 && status.pause_requested == 0);

		snprintf(what, sizeof(what), "%s: nothing recorded in the trace ring",
		         modes[i].name);
		check(what, debugger_trace_pending() == 0);
	}
}

/**
 * With the debugger switched off there is nothing to halt into, so the old
 * behaviour has to stand: debugger_bad_mode() declines and its caller ends the
 * emulator. Checked through the return value rather than by provoking fatal(),
 * which would take this process with it.
 */
static void
test_debugger_disabled(void)
{
	printf("Debugger disabled\n");

	reset_machine();
	config.debug_enabled = 0;

	check("declines the halt, leaving the caller to end the emulator",
	      debugger_bad_mode(0x14, TEST_PC) == 0);
	check("and does not halt the machine", debugger_is_paused() == 0);

	config.debug_enabled = 1;
	check("enabled again, it takes the halt", debugger_bad_mode(0x14, TEST_PC) == 1);
	check("and the machine is halted", debugger_is_paused() != 0);
}

/*
 * The address reported when the mode is written by an instruction that also
 * writes R15 - which is how a guest usually gets into a reserved mode.
 *
 * Everything above calls updatemode() directly with R15 parked at TEST_R15,
 * so PC is whatever the test put there and the reported address cannot be
 * wrong. A real machine does not arrive that way. `MOVS PC, R14` and
 * `LDM {..., PC}^` restore the PSR *and* branch, and arm_write_r15() writes
 * R15 before it calls updatemode(), so by the time the halt is taken the
 * address the debugger derives from R15 belongs to the branch target rather
 * than to the instruction that did it.
 *
 * Reported by JonAbbott2 on discussion #223: an address whose instruction did
 * not touch CPSR, and once FFFFFFF8, which is (0 - 8) masked - a PC computed
 * from an R15 that had just been loaded with zero.
 *
 * So this runs the instruction rather than describing it.
 */
static void
test_mode_change_by_a_branching_instruction(int dyn)
{
	DebuggerStatus status;

	printf("A reserved mode written by MOVS PC, R14 (%s)\n",
	       dyn ? "compiled block" : "instruction at a time");

	reset_machine();

	arm_reset(CPUModel_SA110);
	initcodeblocks();
	/*
	 * Both ways round. isblockvalid() keys off dcache, so 0 runs the
	 * instruction on its own and 1 runs it from a compiled block - and the
	 * mode change is applied in a different place in each. Which core this
	 * links is the build's choice: with RPCEMU_DYNAREC on, as it is here and
	 * for every shipped binary, it is arm_dynarec.c. arm.c carries the same
	 * updatemode() and is built by the interpreter configuration, which CI
	 * builds too.
	 */
	dcache = dyn ? 1 : 0;
	resetcodeblocks();
	prog32 = 1;
	updatemode(0x10 | SUPERVISOR);

	ram00[0] = 0xe1b0f00eu;		/* MOVS PC, R14 */
	ram00[1] = 0xeafffffeu;		/* B . */

	/* The branch target is a long way from the instruction, so an address
	   derived from it cannot be mistaken for the right answer. */
	arm.reg[14] = CODE_PC + 0x800u;
	arm.reg[15] = CODE_PC + 8u;
	arm.reg[16] = 0x10 | SUPERVISOR;
	arm.spsr[SUPERVISOR] = 0x14;	/* reserved, and what MOVS PC restores */

	config.debug_enabled = 1;
	debugger_resume();

	arm_exec();

	debugger_get_status(&status);

	check("the machine is halted", status.paused != 0);
	check("halted because of the mode", status.reason == DebugPauseReason_BadMode);
	check_hex("the halt names the instruction, not its branch target",
	          status.halt_pc, CODE_PC);
}

int
main(void)
{
	/* The reserved values, all nine of them. */
	static const uint32_t reserved[] = { 4, 5, 6, 8, 9, 10, 12, 13, 14 };

	arm_init();
	mem_init();
	machine.model = Model_RPCSA110;
	mem_reset(16, 2);

	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		test_reserved_mode(reserved[i]);
	}
	test_legal_modes();
	test_mode_change_by_a_branching_instruction(0);
	test_mode_change_by_a_branching_instruction(1);
	test_debugger_disabled();

	printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
