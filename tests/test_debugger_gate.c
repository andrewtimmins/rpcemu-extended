/*
 * The debugger's fast gate, and the one way it can fail silently.
 *
 * WHY THIS EXISTS. The interpreter used to call debugger_instruction_hook() on
 * every single instruction, whether or not a debugger was attached - a function
 * call in the innermost loop of the emulator, doing nothing, forever. That is
 * now gated on debugger_hook_active, a cached flag that says whether anything
 * currently wants to see each instruction.
 *
 * Caching it buys speed and introduces exactly one new way to be wrong: a
 * debugger state change that forgets to recompute the flag. Every other bug in
 * this area is loud. This one is not. If a mutator misses its refresh, the flag
 * stays zero, the interpreter skips the hook, and breakpoints simply never
 * fire - no error, no log line, no crash. The person debugging concludes their
 * breakpoint address was wrong and goes looking in the wrong place.
 *
 * So this walks every route into and out of "something is watching":
 * breakpoints, watchpoints, single-step, pause and resume, and the halting
 * traps. In each case the flag must be set on the way in and clear again on the
 * way out. The clearing half matters as much as the setting half - a flag that
 * latches on would cost the emulator its recompiler for the rest of the
 * session, since the dynarec drops to interpretation whenever it is set.
 *
 * Links rpcemu_core because the debugger state lives in rpcemu.c and is
 * deliberately file-static; the public API is the only way to reach it, which
 * is precisely what wants testing.
 */

#include <stdio.h>
#include <string.h>

#include "rpcemu.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/**
 * The flag and the predicate it caches must never disagree.
 */
static void
check_gate(const char *what, int expected)
{
	char buf[128];

	snprintf(buf, sizeof(buf), "%s: gate is %s", what, expected ? "set" : "clear");
	check(buf, (debugger_hook_active != 0) == (expected != 0));

	snprintf(buf, sizeof(buf), "%s: flag agrees with the accessor", what);
	check(buf, (debugger_hook_active != 0) ==
	           (debugger_requires_instruction_hook() != 0));
}

/**
 * Put the debugger back to a known idle state.
 */
static void
reset_debugger(void)
{
	DebugTraceConfig cfg;

	debugger_resume();
	debugger_clear_breakpoints();
	debugger_clear_watchpoints();
	memset(&cfg, 0, sizeof(cfg));
	debugger_set_trace_config(&cfg);
}

static void
test_idle(void)
{
	printf("Idle\n");

	reset_debugger();
	check_gate("nothing attached", 0);
}

static void
test_breakpoints(void)
{
	printf("Breakpoints\n");

	reset_debugger();

	debugger_add_breakpoint(0x8000);
	check_gate("one breakpoint set", 1);

	debugger_add_breakpoint(0x9000);
	check_gate("two breakpoints set", 1);

	/* Removing one is not enough to stand down */
	debugger_remove_breakpoint(0x8000);
	check_gate("one of two removed", 1);

	debugger_remove_breakpoint(0x9000);
	check_gate("last breakpoint removed", 0);

	/* The bulk clear has to refresh too */
	debugger_add_breakpoint(0x8000);
	debugger_add_breakpoint(0x9000);
	check_gate("breakpoints set again", 1);
	debugger_clear_breakpoints();
	check_gate("breakpoints cleared in bulk", 0);

	/* Removing an address that was never set must not disturb the gate */
	debugger_add_breakpoint(0x8000);
	debugger_remove_breakpoint(0x1234);
	check_gate("removing an absent breakpoint", 1);

	reset_debugger();
}

static void
test_watchpoints(void)
{
	printf("Watchpoints\n");

	reset_debugger();

	debugger_add_watchpoint(0x10000, 4, 1, 1, 0);
	check_gate("one watchpoint set", 1);

	debugger_remove_watchpoint(0x10000, 4, 1, 1);
	check_gate("watchpoint removed", 0);

	/* A logging watchpoint still needs the hooked path */
	debugger_add_watchpoint(0x10000, 4, 1, 1, 1);
	check_gate("logging watchpoint set", 1);

	debugger_clear_watchpoints();
	check_gate("watchpoints cleared in bulk", 0);

	reset_debugger();
}

static void
test_pause_and_step(void)
{
	printf("Pause, resume and step\n");

	reset_debugger();

	debugger_request_pause(DebugPauseReason_User);
	check_gate("pause requested", 1);

	debugger_resume();
	check_gate("resumed", 0);

	debugger_single_step(1);
	check_gate("single step armed", 1);

	debugger_resume();
	check_gate("resumed from step", 0);

	/* A step of zero instructions is a no-op and must not arm the gate */
	debugger_single_step(0);
	check_gate("zero-instruction step", 0);

	/* Stepping down to zero stands the gate back up as a pause request */
	debugger_single_step(2);
	check_gate("two-instruction step armed", 1);
	debugger_after_instruction(0x8000, 0xE1A00000);
	check_gate("one instruction consumed", 1);
	debugger_after_instruction(0x8004, 0xE1A00000);
	check_gate("step complete, pause pending", 1);

	reset_debugger();
	check_gate("reset after stepping", 0);
}

static void
test_traps(void)
{
	DebugTraceConfig cfg;

	printf("Halting traps\n");

	reset_debugger();

	/* Each halting trap independently needs the hooked path */
	memset(&cfg, 0, sizeof(cfg));
	cfg.trap_undefined = 1;
	debugger_set_trace_config(&cfg);
	check_gate("trap on undefined instruction", 1);

	memset(&cfg, 0, sizeof(cfg));
	cfg.trap_data_abort = 1;
	debugger_set_trace_config(&cfg);
	check_gate("trap on data abort", 1);

	memset(&cfg, 0, sizeof(cfg));
	cfg.trap_prefetch_abort = 1;
	debugger_set_trace_config(&cfg);
	check_gate("trap on prefetch abort", 1);

	memset(&cfg, 0, sizeof(cfg));
	cfg.swi_trace_halt = 1;
	debugger_set_trace_config(&cfg);
	check_gate("halt on SWI", 1);

	/* Logging without halting must NOT engage the per-instruction path -
	   that is the whole point of the logging-only paths being cheap. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.swi_trace_enabled = 1;
	debugger_set_trace_config(&cfg);
	check_gate("SWI logging without halting", 0);

	memset(&cfg, 0, sizeof(cfg));
	cfg.log_exceptions = 1;
	debugger_set_trace_config(&cfg);
	check_gate("exception logging without trapping", 0);

	memset(&cfg, 0, sizeof(cfg));
	debugger_set_trace_config(&cfg);
	check_gate("trace config cleared", 0);

	reset_debugger();
}

static void
test_combinations(void)
{
	printf("Overlapping reasons\n");

	reset_debugger();

	/* Clearing one reason must not clear the gate while another still holds */
	debugger_add_breakpoint(0x8000);
	debugger_add_watchpoint(0x10000, 4, 1, 1, 0);
	check_gate("breakpoint and watchpoint", 1);

	debugger_clear_breakpoints();
	check_gate("breakpoint gone, watchpoint remains", 1);

	debugger_clear_watchpoints();
	check_gate("both gone", 0);

	/* Same again, the other way round */
	debugger_add_breakpoint(0x8000);
	debugger_single_step(1);
	check_gate("breakpoint and step", 1);
	debugger_resume();
	check_gate("step done, breakpoint remains", 1);

	reset_debugger();
	check_gate("all clear", 0);
}

int
main(void)
{
	printf("Debugger instruction-hook gate\n\n");

	test_idle();
	test_breakpoints();
	test_watchpoints();
	test_pause_and_step();
	test_traps();
	test_combinations();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
