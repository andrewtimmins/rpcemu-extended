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
 * The breakpoint SWI: a guest stopping the machine from its own source.
 *
 * WHY THIS EXISTS. `SWI &FFFFFF` halts the machine in the debugger and is
 * swallowed on the way through, so RISC OS never sees a SWI it does not know.
 * Asked for in discussion #223 by somebody debugging a module, where an address
 * breakpoint has to be re-found every time the code is rebuilt and moves.
 *
 * Three things have to hold together and each fails differently:
 *
 *  - it must HALT, or the breakpoint does nothing;
 *  - it must be SWALLOWED, or the guest takes an error for an unknown SWI -
 *    which would be worse than no feature at all, because the breakpoint would
 *    then change the behaviour of the program being debugged;
 *  - the halt must name the SWI's own address, not R15, which leads it by
 *    eight. The first version of this reported the address plus eight and
 *    would have had the reporter looking at the wrong line.
 *
 * It also must not fire for anything else: the number is matched on the whole
 * 24-bit comment field, because opSWI()'s own `swinum` is masked to 0xdffff and
 * other SWIs would alias onto it.
 *
 * Links rpcemu_core for the real opSWI() and the real debugger state.
 */

#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "arm_common.h"
#include "mem.h"

#define AT_PC		0x00108000u
#define AT_R15		(AT_PC + 8u)

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

/** A 32-bit Supervisor machine at a known PC, with an idle enabled debugger. */
static void
reset_machine(void)
{
	DebugTraceEvent drain[8];
	uint32_t dropped;

	arm_reset(CPUModel_SA110);
	prog32 = 1;
	updatemode(0x10 | SUPERVISOR);
	arm.reg[15] = AT_R15;
	arm.reg[16] = 0x10 | SUPERVISOR;

	config.debug_enabled = 1;
	debugger_resume();
	debugger_clear_breakpoints();
	debugger_clear_watchpoints();
	while (debugger_drain_trace_events(drain, 8, &dropped) > 0) {
		/* discard */
	}
}

static void
test_halts_and_is_swallowed(void)
{
	DebuggerStatus status;
	int rc;

	printf("SWI &FFFFFF stops the machine\n");

	reset_machine();
	rc = opSWI(0xefffffffu);

	check("opSWI reports it handled, so the SWI is not passed on", rc == 0);
	check("and clears V, as a SWI the emulator answers itself does",
	      (arm.reg[cpsr] & VFLAG) == 0);

	debugger_get_status(&status);
	check("the machine is halted", status.paused != 0);
	check("and says why", status.reason == DebugPauseReason_BreakSwi);
	check_hex("the halt names the SWI, not R15 which leads it by eight",
	          status.halt_pc, AT_PC);
}

static void
test_leaves_a_trace_entry(void)
{
	DebugTraceEvent events[4];
	uint32_t dropped = 0;
	uint32_t count;

	printf("and leaves a record of where it stopped\n");

	reset_machine();
	arm.reg[0] = 0x12345678u;
	opSWI(0xefffffffu);

	count = debugger_drain_trace_events(events, 4, &dropped);
	check("one trace event, no drops", count == 1 && dropped == 0);
	if (count == 1) {
		check("recorded as a SWI", events[0].type == TraceEvent_Swi);
		check_hex("carrying the SWI number", events[0].arg0,
		          RPCEMU_SWI_BREAKPOINT);
		check_hex("and the PC", events[0].pc, AT_PC);
		check_hex("and R0, as SWI tracing does", events[0].arg1, 0x12345678u);
	}
}

static void
test_only_this_number(void)
{
	DebuggerStatus status;

	printf("and nothing else is caught by it\n");

	/*
	 * opSWI() masks its own swinum to 0xdffff, so a number differing only in
	 * the bits that mask away must NOT be taken for the breakpoint. &DFFFFF is
	 * exactly that: it has the same masked value as &FFFFFF.
	 */
	reset_machine();
	opSWI(0xefdfffffu);
	debugger_get_status(&status);
	check("SWI &DFFFFF, which masks to the same swinum, does not halt",
	      status.paused == 0);

	reset_machine();
	opSWI(0xef000011u);		/* OS_Exit */
	debugger_get_status(&status);
	check("an ordinary SWI does not halt", status.paused == 0);
}

static void
test_without_a_debugger(void)
{
	DebuggerStatus status;
	int rc;

	printf("with the debugger switched off\n");

	reset_machine();
	config.debug_enabled = 0;

	rc = opSWI(0xefffffffu);
	debugger_get_status(&status);

	check("it is still swallowed, so the guest takes no error", rc == 0);
	check("but nothing is halted, because there is nothing to halt into",
	      status.paused == 0);

	config.debug_enabled = 1;
}

int
main(void)
{
	arm_init();
	mem_init();
	machine.model = Model_RPCSA110;
	mem_reset(16, 2);

	test_halts_and_is_swallowed();
	test_leaves_a_trace_entry();
	test_only_this_number();
	test_without_a_debugger();

	printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
