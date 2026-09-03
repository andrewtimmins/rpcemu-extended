/*
 * Stepping over, out and to.
 *
 * WHY THIS EXISTS. These three are the only debugger operations that deal with
 * failure by RUNNING THE MACHINE. A breakpoint that is wrong stops in the wrong
 * place; a step-over that is wrong does not stop at all, and the guest is gone
 * - past the line being examined, through however much code came next, until it
 * hits something else or the person gives up and pulls the pause. Whatever
 * state they were looking at is destroyed, and getting back to it means
 * rebooting and reproducing.
 *
 * That asymmetry decides the design, and this file is what holds the design in
 * place:
 *
 *   - STEP-OVER MUST DEGRADE TOWARDS STEPPING IN. If the instruction cannot be
 *     read, or is not recognised as a call, the answer is a single step, not a
 *     guess at where the call returns. Stepping into a function you meant to
 *     step over is a mild annoyance. Running away is not.
 *
 *   - THE TEMPORARY BREAKPOINT MUST NOT SURVIVE. Step-over plants a target and
 *     resumes. If the machine stops somewhere else first - a real breakpoint, a
 *     trap - that target is now stale, and firing it later would stop the
 *     machine at an address nobody asked about, minutes later, with no
 *     explanation. So any resume or step clears it.
 *
 *   - IT MUST BE SEPARATE FROM THE USER'S BREAKPOINTS. The obvious
 *     implementation puts the target in the breakpoint table, where it either
 *     overwrites a breakpoint the user had already set at that address or has
 *     to refuse to step. Both are worse than keeping one address to one side,
 *     and the test for it is that stepping over an address the user has
 *     breakpointed leaves their breakpoint exactly as it was.
 *
 * The distinction the checks turn on is that single-stepping sets step_active
 * while running to a target does not, so the two strategies are told apart from
 * outside without reaching into the debugger's private state.
 *
 * The MMU is left off, so virtual addresses are physical and code can be put
 * where the CPU will be told to look by writing words into RAM.
 */

#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

#define CODE	0x10030000u	/* where the fake instruction goes */
#define STACK	0x10020000u

/* A few encodings, by name so the tests read as intentions */
#define OP_BL		0xEB000000u	/* BL pc+8    - a call */
#define OP_B		0xEA000000u	/* B  pc+8    - not a call */
#define OP_MOV		0xE1A00000u	/* MOV R0, R0 - not a call */
#define OP_MOV_PC_LR	0xE1A0F00Eu	/* MOV PC, LR - a return, not a call */
#define OP_SWI		0xEF000002u	/* SWI OS_Write0 - a call into the OS */

/**
 * Put the machine at `pc`, executing `opcode`, and stopped.
 *
 * Pausing goes through the instruction hook because that is the only way in
 * from outside: request a pause, then let the hook observe one instruction and
 * act on the request, exactly as the emulator thread would.
 */
static void
halt_at(uint32_t pc, uint32_t opcode)
{
	debugger_resume();
	debugger_clear_breakpoints();
	debugger_clear_watchpoints();

	mem_phys_write32(pc, opcode);

	arm.r15_mask = 0xfffffffcu;
	arm.reg[15] = pc + 8;		/* R15 leads by one pipeline stage */
	arm.reg[14] = 0;
	arm.reg[13] = STACK;
	arm.reg[11] = 0;		/* no frame pointer unless a test sets one */

	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(pc, opcode);
}

/**
 * Is the debugger single-stepping, as opposed to running to a target?
 */
static int
is_stepping(void)
{
	DebuggerStatus st;

	debugger_get_status(&st);
	return st.step_active != 0;
}

static int
is_paused(void)
{
	DebuggerStatus st;

	debugger_get_status(&st);
	return st.paused != 0;
}

/* ---- step over ----------------------------------------------------------- */

static void
test_step_over_a_call(void)
{
	printf("Stepping over a call\n");

	halt_at(CODE, OP_BL);
	check("the machine is stopped to begin with", is_paused());

	check("step over a BL is accepted", debugger_step_over());
	check("... and runs the machine", !is_paused());
	check("... to a target rather than by stepping", !is_stepping());
	check("... with the instruction hook engaged",
	      debugger_requires_instruction_hook());

	/* Arriving at the instruction after the call must stop the machine */
	debugger_instruction_hook(CODE + 4, OP_MOV);
	check("... and stops at the instruction after the call", is_paused());

	/* And the target is consumed: passing it again must not stop anything */
	debugger_resume();
	debugger_instruction_hook(CODE + 4, OP_MOV);
	check("the target does not fire a second time", !is_paused());
}

/*
 * A SWI is stepped over too.
 *
 * WHY THIS EXISTS. It was not, and the reply to discussion #223 said it was.
 * arm_decode() sets is_call for BL and for nothing else, so step over a SWI
 * fell through to a plain single step and dropped the person debugging on the
 * hardware vector at &00000008, several thousand instructions of kernel
 * dispatch away from the code they were reading.
 */
static void
test_step_over_a_swi(void)
{
	printf("Stepping over a SWI\n");

	halt_at(CODE, OP_SWI);
	check("step over a SWI is accepted", debugger_step_over());
	check("... and runs the machine", !is_paused());
	check("... to a target rather than stepping into the vector",
	      !is_stepping());

	/* The vector, and then a handler in RAM: neither stops it. */
	debugger_instruction_hook(0x00000008, OP_B);
	check("... not stopping on the vector", !is_paused());
	debugger_instruction_hook(0x01c04f30, OP_MOV);
	check("... nor in a handler in RAM", !is_paused());

	debugger_instruction_hook(CODE + 4, OP_MOV);
	check("... and stops at the instruction after the SWI", is_paused());
}

static void
test_step_over_a_non_call(void)
{
	printf("Stepping over what is not a call\n");

	/* Anything that is not a call is an ordinary single step. Getting this
	   wrong the other way - treating a branch as a call - would plant a
	   target at pc+4 that the branch never reaches, and the machine would
	   run away. */
	halt_at(CODE, OP_MOV);
	check("step over a MOV is accepted", debugger_step_over());
	check("... by single-stepping", is_stepping());

	halt_at(CODE, OP_B);
	check("step over a plain B is accepted", debugger_step_over());
	check("... by single-stepping, since B never returns to pc+4",
	      is_stepping());

	halt_at(CODE, OP_MOV_PC_LR);
	check("step over a return is accepted", debugger_step_over());
	check("... by single-stepping", is_stepping());
}

static void
test_step_over_needs_a_stopped_machine(void)
{
	printf("Stepping a running machine\n");

	halt_at(CODE, OP_BL);
	debugger_resume();

	check("step over is refused while running", !debugger_step_over());
	check("step out is refused while running", !debugger_step_out());
	check("run to is refused while running", !debugger_run_to(CODE + 0x100));
}

/* ---- step out ------------------------------------------------------------ */

static void
test_step_out(void)
{
	printf("Stepping out\n");

	/* With a frame chain, the return address comes from the frame - R14
	   belongs to the current function and may have been saved and reused */
	halt_at(CODE, OP_MOV);
	mem_phys_write32(STACK + 0x100 - 12, 0);		/* caller's FP */
	mem_phys_write32(STACK + 0x100 - 8, STACK + 0x1f0);	/* caller's SP */
	mem_phys_write32(STACK + 0x100 - 4, CODE + 0x40);	/* return address */
	arm.reg[11] = STACK + 0x100;
	arm.reg[14] = CODE + 0x999;	/* deliberately not the frame's answer */

	check("step out is accepted", debugger_step_out());
	check("... and runs the machine", !is_paused());
	check("... to a target rather than by stepping", !is_stepping());

	/* The frame's return address wins over R14 */
	debugger_instruction_hook(CODE + 0x999, OP_MOV);
	check("... not to the address in R14", !is_paused());
	debugger_instruction_hook(CODE + 0x40, OP_MOV);
	check("... but to the frame's return address", is_paused());
}

static void
test_step_out_without_a_frame(void)
{
	printf("Stepping out with no frame\n");

	/* No frame pointer: R14 is the only candidate left, and is better than
	   refusing */
	halt_at(CODE, OP_MOV);
	arm.reg[11] = 0;
	arm.reg[14] = CODE + 0x80;

	check("step out falls back to R14", debugger_step_out());
	debugger_instruction_hook(CODE + 0x80, OP_MOV);
	check("... and stops there", is_paused());

	/* Nothing to return to at all must be refused rather than run away */
	halt_at(CODE, OP_MOV);
	arm.reg[11] = 0;
	arm.reg[14] = 0;
	check("step out is refused when there is no return address",
	      !debugger_step_out());
	check("... leaving the machine stopped", is_paused());

	/* A return address of "here" would stop instantly and look like a
	   no-op; refusing says what actually happened */
	halt_at(CODE, OP_MOV);
	arm.reg[11] = 0;
	arm.reg[14] = CODE;
	check("step out is refused when the return address is the current PC",
	      !debugger_step_out());
}

/* ---- run to -------------------------------------------------------------- */

static void
test_run_to(void)
{
	printf("Running to an address\n");

	halt_at(CODE, OP_MOV);
	check("run to is accepted", debugger_run_to(CODE + 0x200));
	check("... and runs the machine", !is_paused());

	debugger_instruction_hook(CODE + 0x100, OP_MOV);
	check("... passing addresses that are not the target", !is_paused());

	debugger_instruction_hook(CODE + 0x200, OP_MOV);
	check("... and stopping at the target", is_paused());
}

/* ---- the target must not linger ------------------------------------------ */

static void
test_target_is_abandoned(void)
{
	printf("Abandoning a target\n");

	/* Resuming by hand means the step-over is over. Its target must not
	   fire later, at an address nobody asked about, with no explanation. */
	halt_at(CODE, OP_BL);
	debugger_step_over();
	debugger_resume();

	check("resume stands the instruction hook down",
	      !debugger_requires_instruction_hook());
	debugger_instruction_hook(CODE + 4, OP_MOV);
	check("an abandoned target does not fire", !is_paused());

	/* Same for a single step */
	halt_at(CODE, OP_BL);
	debugger_step_over();
	debugger_single_step(1);
	debugger_instruction_hook(CODE + 4, OP_MOV);
	check("a target abandoned by stepping does not fire as a target",
	      is_stepping() || !is_paused());
}

static void
test_user_breakpoints_are_untouched(void)
{
	const DebugBreakpointInfo *bp;

	printf("The user's breakpoints\n");

	/* Stepping over an address the user has already breakpointed must not
	   disturb their breakpoint - not its condition, not its counts, and it
	   must still be there afterwards. */
	halt_at(CODE, OP_BL);
	debugger_add_breakpoint_ex(CODE + 4, "r0 == 0x1234", 7, 0);

	debugger_step_over();

	bp = debugger_get_breakpoint(CODE + 4);
	check("the user's breakpoint still exists", bp != NULL);
	check("... with its condition intact",
	      bp != NULL && bp->has_condition &&
	      strcmp(bp->condition, "r0 == 0x1234") == 0);
	check("... with its ignore count intact", bp != NULL && bp->ignore_count == 7);
	check("... and is not a one-shot", bp != NULL && !bp->one_shot);

	debugger_clear_breakpoints();
}

int
main(void)
{
	printf("Stepping over, out and to\n\n");

	mem_init();
	mem_reset(16, 2);	/* megabytes, not bytes - see mem_reset() */

	if (mmu) {
		printf("FAIL: the MMU is on, so addresses are not physical\n");
		return 1;
	}

	test_step_over_a_call();
	test_step_over_a_swi();
	test_step_over_a_non_call();
	test_step_over_needs_a_stopped_machine();
	test_step_out();
	test_step_out_without_a_frame();
	test_run_to();
	test_target_is_abandoned();
	test_user_breakpoints_are_untouched();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
