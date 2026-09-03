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
#include "arm.h"		/* arm.mode, and the mode numbers the filter tests */

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


/*
 * A step leaves the machine RUNNING until the step finishes, and any front end
 * that samples the debugger at that moment sees "not stopped".
 *
 * WHY THIS EXISTS. This is not a fault - it is what stepping means, and the
 * pause arrives a moment later through the instruction hook. It is here because
 * the Machine Inspector used to read the machine on a half-second timer and
 * refresh itself immediately after asking for a step, so it caught exactly this
 * transient and greyed out every button that needs a stopped machine. Nothing
 * re-read until the next tick, which capped stepping at about two instructions
 * a second and was reported that way in discussion #223.
 *
 * The window now refreshes when the debugger's state actually changes rather
 * than on a clock. This locks down the fact that made the old arrangement
 * wrong, so nobody reintroduces "ask for a step, then sample" and wonders why
 * the buttons flicker.
 */
static void
test_step_is_not_immediately_paused(void)
{
	DebuggerStatus status;

	printf("A step is not a pause\n");

	reset_debugger();
	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xe1a00000);

	debugger_get_status(&status);
	check("stopped to begin with", status.paused != 0);

	debugger_single_step(1);
	debugger_get_status(&status);
	check("asking for a step leaves the machine running", status.paused == 0);
	check("and says a step is in progress", status.step_active != 0);
	check_gate("mid-step", 1);

	/* The instruction runs, and the pause lands on the far side of it. */
	debugger_instruction_hook(0x8000, 0xe1a00000);
	debugger_after_instruction(0x8000, 0xe1a00000);
	debugger_instruction_hook(0x8004, 0xe1a00000);

	debugger_get_status(&status);
	check("stopped again once the step has been taken", status.paused != 0);
	check("and the step is over", status.step_active == 0);
}

/*
 * A step lands where the user said they did not want to look, and keeps going.
 *
 * WHY THIS EXISTS. Asked for in discussion #223: stepping through application
 * code is useless when every few steps drops into the timer interrupt or into
 * the middle of the ROM, and the way out - step, step, step until the PC comes
 * back - is exactly the tedium the filter is meant to remove.
 *
 * The filter must apply to STEPS ONLY. A breakpoint set inside the ROM is an
 * explicit request for that address; silently not stopping there would be the
 * same silent failure the gate test above exists to prevent, and worse, because
 * the person set the breakpoint deliberately. So the last two cases here matter
 * as much as the first two.
 */
static void
test_step_filters(void)
{
	DebuggerStatus status;
	DebugTraceConfig cfg;

	printf("Stepping past IRQs and the OS\n");

	/* Interrupt mode, filter on. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_irq = 1;
	debugger_set_trace_config(&cfg);

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xe1a00000);
	arm.mode = IRQ;
	debugger_single_step(1);
	debugger_after_instruction(0x8000, 0xe1a00000);
	debugger_instruction_hook(0x8004, 0xe1a00000);

	debugger_get_status(&status);
	check("a step into IRQ mode does not stop", status.paused == 0);
	check("and another step is armed", status.step_active != 0);

	/* Back in user code, and it stops. */
	arm.mode = USER;
	debugger_after_instruction(0x8004, 0xe1a00000);
	debugger_instruction_hook(0x8008, 0xe1a00000);

	debugger_get_status(&status);
	check("it stops once the mode is back to the user's", status.paused != 0);
	check("at the first instruction outside the interrupt",
	    status.halt_pc == 0x8008);

	/* The OS half of the same idea, on the address rather than the mode. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_os = 1;
	debugger_set_trace_config(&cfg);
	arm.mode = USER;

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xe1a00000);
	debugger_single_step(1);
	debugger_after_instruction(0x8000, 0xe1a00000);
	debugger_instruction_hook(0xfc001000, 0xe1a00000);

	debugger_get_status(&status);
	check("a step into the ROM does not stop", status.paused == 0);

	/* The vector page is the OS too, and is in low RAM rather than the ROM;
	   filtering on the ROM address alone let every step land on a vector. */
	debugger_after_instruction(0xfc001000, 0xe1a00000);
	debugger_instruction_hook(0x00000008, 0xea000000);
	debugger_get_status(&status);
	check("nor does it stop on a hardware vector", status.paused == 0);

	debugger_after_instruction(0x00000008, 0xea000000);
	debugger_instruction_hook(0x8004, 0xe1a00000);
	debugger_get_status(&status);
	check("it stops on the way back out", status.paused != 0);
	check("at the address returned to", status.halt_pc == 0x8004);

	/* And now the half that must NOT be filtered. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_os = 1;
	cfg.step_skip_irq = 1;
	debugger_set_trace_config(&cfg);
	arm.mode = IRQ;
	debugger_add_breakpoint(0xfc002000);
	debugger_instruction_hook(0xfc002000, 0xe1a00000);

	debugger_get_status(&status);
	check("a breakpoint in the ROM still fires", status.paused != 0);
	check("and reports its own address", status.halt_pc == 0xfc002000);

	reset_debugger();
	arm.mode = USER;
}

/*
 * A step that executes a SWI runs it to completion.
 *
 * WHY THIS EXISTS. The two filters above are written on where the PC is, which
 * covers the OS's own SWIs because their handlers are in the ROM. Discussion
 * #223 asked for the other half: "unless it's an SWI you've actively trapped,
 * these all execute to completion" - which has to hold for a SWI belonging to a
 * module in RAM as well, since that is the one somebody debugging their own
 * module keeps landing inside.
 *
 * Two things here are easy to get wrong and are pinned. The return address is
 * taken from what the hook recorded on the way in, not from the PC afterwards:
 * a SWI has moved R15 to the hardware vector by the time the instruction is
 * over, and using it would arm the run-to somewhere in the kernel. And a SWI
 * that something else has stopped on must not be run through - that is what
 * "actively trapped" means.
 */
static void
test_stepping_past_swis(void)
{
	DebuggerStatus status;
	DebugTraceConfig cfg;

	printf("Stepping past SWIs\n");

	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_swi = 1;
	debugger_set_trace_config(&cfg);
	arm.mode = USER;

	/* Paused on a SWI, and one step of it. */
	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xef000002);	/* SWI OS_Write0 */
	debugger_single_step(1);
	debugger_after_instruction(0x00000008, 0xef000002);

	debugger_get_status(&status);
	check("a step that executes a SWI does not stop", status.paused == 0);

	/* Wherever the handler goes, it is passed through. */
	debugger_instruction_hook(0x00000008, 0xea000000);
	debugger_instruction_hook(0x01c04f30, 0xe1a00000);	/* a module, in RAM */
	debugger_get_status(&status);
	check("nor inside a handler in RAM, which no address filter covers",
	    status.paused == 0);

	/* And it stops at the instruction after the SWI. */
	debugger_instruction_hook(0x8004, 0xe1a00000);
	debugger_get_status(&status);
	check("it stops when the SWI returns", status.paused != 0);
	check("at the instruction after the SWI, not eight bytes on",
	    status.halt_pc == 0x8004);

	/* Off by default: an unconfigured debugger steps into the vector. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	debugger_set_trace_config(&cfg);

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xef000002);
	debugger_single_step(1);
	debugger_after_instruction(0x00000008, 0xef000002);
	debugger_instruction_hook(0x00000008, 0xea000000);

	debugger_get_status(&status);
	check("with the filter off the step lands on the vector",
	    status.paused != 0 && status.halt_pc == 0x00000008);

	/* A trapped SWI outranks the filter. This is the halting SWI trap's
	   deferred pause, which is requested while the instruction is running. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_swi = 1;
	debugger_set_trace_config(&cfg);

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xef000002);
	debugger_single_step(1);
	debugger_request_pause(DebugPauseReason_Swi);
	debugger_after_instruction(0x00000008, 0xef000002);
	debugger_instruction_hook(0x00000008, 0xea000000);

	debugger_get_status(&status);
	check("a trapped SWI still stops", status.paused != 0);
	check("at its handler, which is what the trap is for",
	    status.halt_pc == 0x00000008);

	/* A breakpoint inside the handler is an explicit request for that
	   address and fires even though the SWI is being passed through. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_swi = 1;
	debugger_set_trace_config(&cfg);
	debugger_add_breakpoint(0x01c04f30);

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xef000002);
	debugger_single_step(1);
	debugger_after_instruction(0x00000008, 0xef000002);
	debugger_instruction_hook(0x01c04f30, 0xe1a00000);

	debugger_get_status(&status);
	check("a breakpoint in the handler still fires", status.paused != 0);
	check("and reports its own address", status.halt_pc == 0x01c04f30);

	/* Anything that is not a SWI is stepped as usual. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.step_skip_swi = 1;
	debugger_set_trace_config(&cfg);

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000, 0xe1a00000);
	debugger_single_step(1);
	debugger_after_instruction(0x8000, 0xe1a00000);
	debugger_instruction_hook(0x8004, 0xe1a00000);

	debugger_get_status(&status);
	check("an ordinary instruction still steps one at a time",
	    status.paused != 0 && status.halt_pc == 0x8004);

	reset_debugger();
	arm.mode = USER;
}

/*
 * A trapped exception reports the instruction that faulted, not the vector.
 *
 * WHY THIS EXISTS. The halt is deferred on purpose - exception() has to finish
 * building the handler's state before the machine can usefully stop - so the
 * instruction hook that actually performs the halt is looking at the handler's
 * first instruction, several thousand instructions away from anything the
 * person debugging wrote. Reporting THAT address is technically where the
 * machine is and practically useless, which is what discussion #223 said: the
 * useful answer is which instruction aborted.
 */
static void
test_exception_reports_the_faulting_instruction(void)
{
	DebuggerStatus status;
	DebugTraceConfig cfg;

	printf("A trapped exception reports the faulting instruction\n");

	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.trap_data_abort = 1;
	debugger_set_trace_config(&cfg);

	/* The load that aborts, seen by the hook first. */
	debugger_instruction_hook(0x9000, 0xe5901000);
	debugger_exception_hook(ABORT, 0x14, 0x9000);

	check_gate("exception pending", 1);

	/* exception() has now vectored, and the hook next sees the handler. */
	debugger_instruction_hook(0x00000014, 0xea000000);

	debugger_get_status(&status);
	check("stopped", status.paused != 0);
	check("the reason is an exception",
	    status.reason == DebugPauseReason_Exception);
	check("the address is the load, not the vector", status.halt_pc == 0x9000);
	check("and the opcode is the load's", status.halt_opcode == 0xe5901000);

	/* A second, untrapped exception must not re-report the first one's
	   address: the recorded PC is consumed by the halt it caused. */
	reset_debugger();
	memset(&cfg, 0, sizeof(cfg));
	cfg.trap_undefined = 1;
	debugger_set_trace_config(&cfg);
	debugger_instruction_hook(0xa000, 0xe7f000f0);
	debugger_exception_hook(UNDEFINED, 0x08, 0xa000);
	debugger_instruction_hook(0x00000008, 0xea000000);
	debugger_get_status(&status);
	check("an undefined instruction reports its own address",
	    status.halt_pc == 0xa000);

	reset_debugger();
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
	test_step_is_not_immediately_paused();
	test_step_filters();
	test_stepping_past_swis();
	test_exception_reports_the_faulting_instruction();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
