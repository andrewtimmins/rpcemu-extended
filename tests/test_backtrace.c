/*
 * Walking the call stack.
 *
 * WHY THIS EXISTS. A backtrace is the one debugger feature people read without
 * checking. Registers and memory get cross-examined; a call stack is believed.
 * That makes a wrong one uniquely expensive - it does not look wrong, it looks
 * like an explanation, and the person debugging goes and reads a function that
 * was never on the stack.
 *
 * The frame chain this walks is a convention, not a fact about the hardware.
 * APCS code keeps a frame pointer in R11 and a four-word record below it; a
 * great deal of RISC OS is hand-written assembler that keeps neither. So the
 * walk will regularly be handed a register that is not a frame pointer at all,
 * and what it does then is the whole substance of this test:
 *
 *   - it must not loop forever on a chain that points at itself, or back down
 *     the stack, which is exactly what garbage in R11 produces;
 *   - it must not invent frames out of whatever RAM happened to contain;
 *   - it must say when it gave up, because a short honest stack and a short
 *     complete stack mean entirely different things and look identical.
 *
 * The 26-bit case is here because it is silent when wrong. On the ARM610/710
 * the link register carries the processor mode and flags in the top six bits
 * and the bottom two, so an unmasked saved LR is not an address - it is an
 * address plus about 0xFC000003. A backtrace that forgets the mask does not
 * fail, it reports plausible-looking addresses that are all wrong, on exactly
 * the machines whose software is oldest and hardest to reason about.
 *
 * The MMU is left off, so virtual addresses are physical and a frame can be
 * built by writing words into RAM.
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

/* Somewhere in SIMM 0, well clear of anything else the core cares about. */
#define STACK_BASE	0x10020000u
#define CODE_BASE	0x00008000u

#define MASK_32		0xfffffffcu
#define MASK_26		0x03fffffcu

/**
 * Write one APCS frame record.
 *
 * The record sits below the frame pointer: the caller's frame pointer at
 * [fp,#-12], its stack pointer at [fp,#-8] and the return address at [fp,#-4].
 */
static void
write_frame(uint32_t fp, uint32_t caller_fp, uint32_t caller_sp, uint32_t return_addr)
{
	mem_phys_write32(fp - 12, caller_fp);
	mem_phys_write32(fp - 8, caller_sp);
	mem_phys_write32(fp - 4, return_addr);
	mem_phys_write32(fp, 0);	/* saved PC, unused by the walk */
}

/**
 * Point the CPU at a given innermost frame.
 */
static void
set_registers(uint32_t pc, uint32_t lr, uint32_t sp, uint32_t fp, uint32_t mask)
{
	arm.r15_mask = mask;
	arm.reg[11] = fp;
	arm.reg[13] = sp;
	arm.reg[14] = lr;
	arm.reg[15] = pc + 8;	/* R15 leads by one pipeline stage */
}

/* ---- tests --------------------------------------------------------------- */

static void
test_innermost_frame(void)
{
	DebugFrame frames[8];
	uint32_t count;
	int truncated = 1;

	printf("The innermost frame\n");

	/* No frame pointer at all: one frame, from the registers, cleanly */
	set_registers(CODE_BASE, 0x9000, STACK_BASE, 0, MASK_32);
	count = debugger_backtrace(frames, 8, &truncated);

	check("a null frame pointer yields exactly one frame", count == 1);
	check("frame 0 PC comes from R15", frames[0].pc == CODE_BASE);
	check("frame 0 LR comes from R14", frames[0].lr == 0x9000);
	check("frame 0 SP comes from R13", frames[0].sp == STACK_BASE);
	check("frame 0 FP comes from R11", frames[0].fp == 0);
	check("a null frame pointer is a clean end, not a failure", !truncated);
}

static void
test_full_chain(void)
{
	DebugFrame frames[8];
	uint32_t count;
	int truncated = 1;

	printf("A complete chain\n");

	/* Three frames. The stack grows down, so each caller sits above the
	   function it called. */
	write_frame(STACK_BASE + 0x100, STACK_BASE + 0x200, STACK_BASE + 0x1f0, 0x8100);
	write_frame(STACK_BASE + 0x200, 0,                  STACK_BASE + 0x2f0, 0x8200);

	set_registers(CODE_BASE, 0x8100, STACK_BASE + 0xf0, STACK_BASE + 0x100, MASK_32);
	count = debugger_backtrace(frames, 8, &truncated);

	check("three frames are found", count == 3);
	check("the walk ended cleanly", !truncated);

	check("frame 0 is where the CPU is", frames[0].pc == CODE_BASE);
	check("frame 1 is the caller", count > 1 && frames[1].pc == 0x8100);
	check("frame 2 is the caller's caller", count > 2 && frames[2].pc == 0x8200);

	check("frame 1 carries the caller's SP",
	      count > 1 && frames[1].sp == STACK_BASE + 0x1f0);
	check("frame 1 carries the caller's FP",
	      count > 1 && frames[1].fp == STACK_BASE + 0x200);

	/* Only the innermost frame has a live link register */
	check("frame 1 reports no link register", count > 1 && frames[1].lr == 0);
}

static void
test_26bit_masking(void)
{
	DebugFrame frames[4];
	uint32_t count;

	printf("26-bit mode\n");

	/* A saved LR on a 26-bit core: address 0x8100, supervisor mode in the
	   low two bits, N and C set in the top. Unmasked this reads as
	   0xA0008103, which is not an address. */
	write_frame(STACK_BASE + 0x100, 0, STACK_BASE + 0x1f0, 0xA0008103);

	set_registers(CODE_BASE, 0, STACK_BASE + 0xf0, STACK_BASE + 0x100, MASK_26);
	count = debugger_backtrace(frames, 4, NULL);

	check("two frames are found", count == 2);
	check("the saved LR has its PSR bits stripped",
	      count > 1 && frames[1].pc == 0x8100);

	/* And the same value must NOT be stripped when the core is 32-bit,
	   where those really are address bits */
	write_frame(STACK_BASE + 0x100, 0, STACK_BASE + 0x1f0, 0xA0008103);
	set_registers(CODE_BASE, 0, STACK_BASE + 0xf0, STACK_BASE + 0x100, MASK_32);
	count = debugger_backtrace(frames, 4, NULL);

	check("a 32-bit core keeps the high address bits",
	      count > 1 && frames[1].pc == 0xA0008100);
}

static void
test_bad_chains(void)
{
	DebugFrame frames[DEBUGGER_MAX_FRAMES];
	uint32_t count;
	int truncated;

	printf("Chains that are not chains\n");

	/* A frame pointing at itself. Without the monotonic check this walks
	   forever, which in a debugger means the emulator hangs. */
	write_frame(STACK_BASE + 0x100, STACK_BASE + 0x100, STACK_BASE, 0x8100);
	set_registers(CODE_BASE, 0, STACK_BASE, STACK_BASE + 0x100, MASK_32);
	truncated = 0;
	count = debugger_backtrace(frames, DEBUGGER_MAX_FRAMES, &truncated);

	check("a self-referencing frame terminates", count <= DEBUGGER_MAX_FRAMES);
	check("a self-referencing frame is reported as truncated", truncated);

	/* A chain running back down the stack, which cannot be real */
	write_frame(STACK_BASE + 0x200, STACK_BASE + 0x100, STACK_BASE, 0x8100);
	write_frame(STACK_BASE + 0x100, STACK_BASE + 0x200, STACK_BASE, 0x8200);
	set_registers(CODE_BASE, 0, STACK_BASE, STACK_BASE + 0x200, MASK_32);
	truncated = 0;
	count = debugger_backtrace(frames, DEBUGGER_MAX_FRAMES, &truncated);

	check("a descending chain terminates", count < DEBUGGER_MAX_FRAMES);
	check("a descending chain is reported as truncated", truncated);

	/* A misaligned frame pointer is not a frame pointer */
	set_registers(CODE_BASE, 0, STACK_BASE, STACK_BASE + 0x102, MASK_32);
	truncated = 0;
	count = debugger_backtrace(frames, 8, &truncated);

	check("a misaligned frame pointer gives only frame 0", count == 1);
	check("a misaligned frame pointer is reported as truncated", truncated);
}

static void
test_limits(void)
{
	DebugFrame frames[8];
	uint32_t count;

	printf("Limits and arguments\n");

	/* A long but valid chain, asked for two frames */
	write_frame(STACK_BASE + 0x100, STACK_BASE + 0x200, STACK_BASE, 0x8100);
	write_frame(STACK_BASE + 0x200, STACK_BASE + 0x300, STACK_BASE, 0x8200);
	write_frame(STACK_BASE + 0x300, 0,                  STACK_BASE, 0x8300);
	set_registers(CODE_BASE, 0, STACK_BASE, STACK_BASE + 0x100, MASK_32);

	count = debugger_backtrace(frames, 2, NULL);
	check("max is respected", count == 2);

	count = debugger_backtrace(frames, 8, NULL);
	check("the whole chain is walked when there is room", count == 4);

	/* Degenerate arguments must be refused rather than crash */
	check("a null buffer yields nothing", debugger_backtrace(NULL, 8, NULL) == 0);
	check("a zero maximum yields nothing", debugger_backtrace(frames, 0, NULL) == 0);

	/* truncated may be NULL */
	count = debugger_backtrace(frames, 8, NULL);
	check("a null truncated flag is accepted", count == 4);
}

int
main(void)
{
	printf("Call stack walking\n\n");

	mem_init();
	mem_reset(16, 2);	/* megabytes, not bytes - see mem_reset() */

	if (mmu) {
		printf("FAIL: the MMU is on, so addresses are not physical\n");
		return 1;
	}

	test_innermost_frame();
	test_full_chain();
	test_26bit_masking();
	test_bad_chains();
	test_limits();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
