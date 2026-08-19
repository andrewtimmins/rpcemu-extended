/*
 * The 6502 core that goes in the OPEN Bus co-processor card.
 *
 * Testable for the same reason as the RV32I one: src/copro/cpu_6502.c executes
 * out of a flat byte array and knows nothing about the bus or the emulator, so a
 * byte array and some hand-written machine code is the whole harness.
 *
 * ★ WHAT IS WORTH PINNING DOWN ON A 6502, given that the instruction set is well
 * trodden and a wrong answer in LDA would be noticed by anybody in a minute:
 *
 *   - the flags nobody gets right first time. Overflow on ADC and SBC is checked
 *     against the eight-case table every 6502 programmer's tutorial uses, and
 *     decimal mode is checked in both directions.
 *   - the hardware's own quirks, which are the reason to model this processor
 *     rather than something like it: the indirect-JMP page-crossing bug, the
 *     zero-page wrap on an indirect pointer, and TXS setting no flags.
 *   - the boundaries: an undocumented opcode faults, memory outside the array
 *     faults, and a run that hits its budget resumes exactly.
 *
 * Programs live at 0x0300 so that the zero page, the stack and page one are free
 * for the tests that need to write there.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_6502.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_u32(const char *what, uint32_t got, uint32_t want)
{
	const int ok = (got == want);

	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf("      got 0x%02x, wanted 0x%02x\n", got, want);
		failures++;
	}
}

/* ---- the machine under test -------------------------------------------- */

#define RAM_SIZE	0x10000		/* the whole 64K address space */
#define PROG_BASE	0x0300

static uint8_t ram[RAM_SIZE];
static cpu6502_state cpu;

static void
load(const uint8_t *prog, unsigned len)
{
	memset(ram, 0, sizeof(ram));
	memcpy(ram + PROG_BASE, prog, len);
	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_reset(&cpu, PROG_BASE);
}

/** Run to a halt or a fault, bounded so a runaway test cannot hang. */
static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 1000; slices++) {
		if (cpu6502_run(&cpu, 1000) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Opcodes used below, named so the programs read as assembler. */
#define LDA_IMM	0xa9
#define LDA_ZP	0xa5
#define LDA_ABS	0xad
#define LDA_INDY 0xb1
#define LDA_ZPX	0xb5
#define LDX_IMM	0xa2
#define LDY_IMM	0xa0
#define STA_ZP	0x85
#define STA_ABS	0x8d
#define STX_ZP	0x86
#define ADC_IMM	0x69
#define ADC_ZP	0x65
#define SBC_IMM	0xe9
#define AND_IMM	0x29
#define CMP_IMM	0xc9
#define BIT_ZP	0x24
#define ASL_A	0x0a
#define LSR_A	0x4a
#define ROL_A	0x2a
#define ROR_A	0x6a
#define INX	0xe8
#define DEX	0xca
#define INY	0xc8
#define TXS	0x9a
#define TSX	0xba
#define TAX	0xaa
#define TXA	0x8a
#define PHA	0x48
#define PLA	0x68
#define PHP	0x08
#define PLP	0x28
#define CLC	0x18
#define SEC	0x38
#define CLD	0xd8
#define SED	0xf8
#define JSR	0x20
#define RTS	0x60
#define JMP_ABS	0x4c
#define JMP_IND	0x6c
#define BNE	0xd0
#define BEQ	0xf0
#define BRK	0x00
#define NOP	0xea

/* ---- a program that does some work ------------------------------------- */

static void
test_sum_loop(void)
{
	const uint8_t prog[] = {
		LDA_IMM, 0x00,		/* total = 0 */
		LDX_IMM, 0x0a,		/* x = 10 */
		/* loop: */
		STX_ZP, 0x80,		/* the zero page as scratch */
		CLC,
		ADC_ZP, 0x80,		/* total += x */
		DEX,
		BNE, 0xf8,		/* back to the STX */
		STA_ABS, 0x00, 0x02,	/* the answer at 0x0200 */
		BRK
	};

	printf("A program that loops, stores and stops:\n");

	load(prog, sizeof(prog));
	check("it reaches a stop", run_to_stop());
	check("it halted rather than faulted", cpu.halted && !cpu.faulted);
	check("the stop was a BRK", cpu.halt_reason == CPU6502_HALT_BRK);
	check_u32("10 down to 1 summed to 55", cpu.a, 55);
	check_u32("and 55 reached memory", ram[0x0200], 55);
	check_u32("the accumulator is the exit code", cpu.exit_code, 55);
	check("cycles were counted", cpu.cycles == 2 + 10 * 5 + 2);
	/* BRK is two bytes on real hardware even though nothing reads the second,
	   so a core single-stepped past a halt resumes in the right place. */
	check_u32("BRK consumed its second byte",
	          cpu.pc, PROG_BASE + sizeof(prog) + 1);
}

/* ---- the flags, which are the part worth testing ----------------------- */

/**
 * Run one arithmetic instruction with a known accumulator and carry.
 *
 * Assembled rather than poked into the state, so the instruction really is
 * decoded and executed: LDA sets no carry of its own, so the CLC or SEC after it
 * is what fixes the input carry.
 */
static void
alu_case(uint8_t a, int carry_in, int decimal, uint8_t opcode, uint8_t operand)
{
	const uint8_t prog[] = {
		decimal ? SED : CLD,
		LDA_IMM, a,
		(uint8_t) (carry_in ? SEC : CLC),
		opcode, operand,
		BRK
	};

	load(prog, sizeof(prog));
	(void) run_to_stop();
}

static int
flag(uint8_t f)
{
	return (cpu.p & f) != 0;
}

static void
test_adc_overflow_table(void)
{
	printf("\nADC, against the eight-case overflow table:\n");

	/* The table every 6502 tutorial uses. Overflow means "the sign of the
	   result is not the sign the operands' signs imply", which is not the same
	   as carry, and conflating the two is the classic mistake. */
	alu_case(0x50, 0, 0, ADC_IMM, 0x10);
	check("0x50 + 0x10: no overflow, no carry",
	      cpu.a == 0x60 && !flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0x50, 0, 0, ADC_IMM, 0x50);
	check("0x50 + 0x50: overflow, no carry",
	      cpu.a == 0xa0 && flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0x50, 0, 0, ADC_IMM, 0x90);
	check("0x50 + 0x90: no overflow, no carry",
	      cpu.a == 0xe0 && !flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0x50, 0, 0, ADC_IMM, 0xd0);
	check("0x50 + 0xd0: no overflow, carry",
	      cpu.a == 0x20 && !flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	alu_case(0xd0, 0, 0, ADC_IMM, 0x10);
	check("0xd0 + 0x10: no overflow, no carry",
	      cpu.a == 0xe0 && !flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0xd0, 0, 0, ADC_IMM, 0x50);
	check("0xd0 + 0x50: no overflow, carry",
	      cpu.a == 0x20 && !flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	alu_case(0xd0, 0, 0, ADC_IMM, 0x90);
	check("0xd0 + 0x90: overflow, carry",
	      cpu.a == 0x60 && flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	alu_case(0xd0, 0, 0, ADC_IMM, 0xd0);
	check("0xd0 + 0xd0: no overflow, carry",
	      cpu.a == 0xa0 && !flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	/* The incoming carry is added, which is what makes multi-byte arithmetic
	   work at all. */
	alu_case(0x01, 1, 0, ADC_IMM, 0x01);
	check_u32("the incoming carry is added", cpu.a, 0x03);

	alu_case(0xff, 0, 0, ADC_IMM, 0x01);
	check("a result of zero sets Z and C",
	      cpu.a == 0x00 && flag(CPU6502_FLAG_Z) && flag(CPU6502_FLAG_C));
}

static void
test_sbc_overflow_table(void)
{
	printf("\nSBC, the same table the other way round:\n");

	/* Carry set means no borrow, which is the convention that surprises
	   people coming from other processors. */
	alu_case(0x50, 1, 0, SBC_IMM, 0xf0);
	check("0x50 - 0xf0: no overflow, borrow",
	      cpu.a == 0x60 && !flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0x50, 1, 0, SBC_IMM, 0xb0);
	check("0x50 - 0xb0: overflow, borrow",
	      cpu.a == 0xa0 && flag(CPU6502_FLAG_V) && !flag(CPU6502_FLAG_C));

	alu_case(0x50, 1, 0, SBC_IMM, 0x30);
	check("0x50 - 0x30: no overflow, no borrow",
	      cpu.a == 0x20 && !flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	alu_case(0xd0, 1, 0, SBC_IMM, 0x70);
	check("0xd0 - 0x70: overflow, no borrow",
	      cpu.a == 0x60 && flag(CPU6502_FLAG_V) && flag(CPU6502_FLAG_C));

	alu_case(0x50, 0, 0, SBC_IMM, 0x00);
	check_u32("a clear carry subtracts the borrow as well", cpu.a, 0x4f);
}

static void
test_decimal_mode(void)
{
	printf("\nDecimal mode, in both directions:\n");

	alu_case(0x28, 0, 1, ADC_IMM, 0x14);
	check_u32("0x28 + 0x14 in decimal is 0x42", cpu.a, 0x42);

	alu_case(0x50, 0, 1, ADC_IMM, 0x50);
	check("0x50 + 0x50 in decimal is 0x00 with a carry",
	      cpu.a == 0x00 && flag(CPU6502_FLAG_C));

	alu_case(0x99, 0, 1, ADC_IMM, 0x01);
	check("0x99 + 0x01 in decimal wraps to 0x00 with a carry",
	      cpu.a == 0x00 && flag(CPU6502_FLAG_C));

	alu_case(0x09, 0, 1, ADC_IMM, 0x01);
	check_u32("a low-nibble carry is corrected", cpu.a, 0x10);

	alu_case(0x42, 1, 1, SBC_IMM, 0x14);
	check_u32("0x42 - 0x14 in decimal is 0x28", cpu.a, 0x28);

	alu_case(0x00, 1, 1, SBC_IMM, 0x01);
	check("0x00 - 0x01 in decimal is 0x99 with a borrow",
	      cpu.a == 0x99 && !flag(CPU6502_FLAG_C));

	/* Decimal mode must not leak into a later binary operation. */
	alu_case(0x09, 0, 0, ADC_IMM, 0x01);
	check_u32("with decimal mode clear the same sum is 0x0a", cpu.a, 0x0a);
}

/* ---- the hardware's own quirks ----------------------------------------- */

static void
test_jmp_indirect_page_bug(void)
{
	const uint8_t prog[] = { JMP_IND, 0xff, 0x10 };

	printf("\nThe NMOS indirect-JMP page-crossing bug:\n");

	load(prog, sizeof(prog));

	/* The low byte of the target is at 0x10ff. A part that incremented the
	   whole address would take the high byte from 0x1100; an NMOS 6502
	   increments only the low byte and takes it from 0x1000. The two
	   candidates are made deliberately different so the test cannot pass by
	   accident. */
	ram[0x10ff] = 0x34;
	ram[0x1000] = 0x12;	/* what the hardware really uses */
	ram[0x1100] = 0x99;	/* what a fixed part would use */

	(void) cpu6502_step(&cpu);
	check_u32("the high byte comes from the start of the same page",
	          cpu.pc, 0x1234);
}

static void
test_zero_page_wrap(void)
{
	const uint8_t prog[] = { LDY_IMM, 0x01, LDA_INDY, 0xff, BRK };

	printf("\nZero-page wrap on an indirect pointer:\n");

	load(prog, sizeof(prog));

	/* The pointer is at 0xff, so its high byte comes from 0x00 and not from
	   0x0100. Both candidates are populated and they differ. */
	ram[0x00ff] = 0x00;
	ram[0x0000] = 0x30;	/* high byte from the zero page: 0x3000 */
	ram[0x0100] = 0x40;	/* what a part that did not wrap would use */
	ram[0x3001] = 0x5a;	/* 0x3000 + Y */
	ram[0x4001] = 0xa5;

	(void) run_to_stop();
	check_u32("the pointer's high byte comes from 0x00", cpu.a, 0x5a);

	{
		/* Zero page,X wraps inside the page too. */
		const uint8_t p2[] = { LDX_IMM, 0x02, LDA_ZPX, 0xff, BRK };

		load(p2, sizeof(p2));
		ram[0x0001] = 0x77;	/* 0xff + 2, wrapped */
		ram[0x0101] = 0x88;	/* not wrapped */
		(void) run_to_stop();
		check_u32("zero page,X wraps inside the zero page", cpu.a, 0x77);
	}
}

static void
test_transfers_and_stack(void)
{
	printf("\nTransfers, the stack, and the one transfer that sets no flags:\n");

	{
		/* TXS is the only transfer that does not set flags. Z is set first
		   by loading zero into X, then TXS must leave it set even though
		   the value moved is zero either way - so the check is that the
		   FLAGS did not change, using a non-zero A to make Z observable. */
		const uint8_t prog[] = {
			LDX_IMM, 0x80,		/* sets N */
			LDA_IMM, 0x01,		/* clears N, clears Z */
			TXS,			/* must not touch N or Z */
			BRK
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("txs moved x to the stack pointer", cpu.sp, 0x80);
		check("and set no flags",
		      !flag(CPU6502_FLAG_N) && !flag(CPU6502_FLAG_Z));
	}

	{
		/* TAX does set them, which is the contrast that makes the above
		   mean something. */
		const uint8_t prog[] = { LDA_IMM, 0x80, TAX, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("tax sets N from the value moved", flag(CPU6502_FLAG_N));
	}

	{
		const uint8_t prog[] = {
			LDA_IMM, 0x5a,
			PHA,
			LDA_IMM, 0x00,		/* prove the pull really pulls */
			PLA,
			BRK
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("pha then pla round-trips through the stack", cpu.a, 0x5a);
	}

	{
		/* PHP pushes the break flag set, whatever the register holds; it is
		   the only place that bit is ever visible. */
		const uint8_t prog[] = { SEC, PHP, PLA, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("php pushes the break and unused bits set",
		      (cpu.a & CPU6502_FLAG_B) != 0 &&
		      (cpu.a & CPU6502_FLAG_U) != 0 &&
		      (cpu.a & CPU6502_FLAG_C) != 0);
	}

	{
		const uint8_t prog[] = {
			JSR, 0x0a, 0x03,	/* to 0x030a */
			LDA_IMM, 0x02,		/* runs after the return */
			BRK,
			NOP, NOP, NOP, NOP,
			/* 0x030a: */
			LDX_IMM, 0x09,
			RTS
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("jsr reached the subroutine", cpu.x, 0x09);
		check_u32("and rts came back to the instruction after it", cpu.a, 0x02);
	}
}

static void
test_shifts_and_bit(void)
{
	printf("\nShifts through the carry, and BIT:\n");

	{
		const uint8_t prog[] = { LDA_IMM, 0x81, ASL_A, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("asl shifts the top bit into the carry",
		      cpu.a == 0x02 && flag(CPU6502_FLAG_C));
	}

	{
		const uint8_t prog[] = { LDA_IMM, 0x01, LSR_A, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("lsr shifts the bottom bit into the carry",
		      cpu.a == 0x00 && flag(CPU6502_FLAG_C) && flag(CPU6502_FLAG_Z));
	}

	{
		const uint8_t prog[] = { SEC, LDA_IMM, 0x00, ROL_A, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("rol rotates the carry in at the bottom", cpu.a, 0x01);
	}

	{
		const uint8_t prog[] = { SEC, LDA_IMM, 0x00, ROR_A, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ror rotates the carry in at the top", cpu.a, 0x80);
	}

	{
		/* BIT is the odd one out: N and V come from the OPERAND, not from
		   the result, and only Z reflects the AND. */
		const uint8_t prog[] = { LDA_IMM, 0x01, BIT_ZP, 0x90, BRK };

		load(prog, sizeof(prog));
		ram[0x90] = 0xc0;	/* bits 7 and 6 set, bit 0 clear */
		(void) run_to_stop();
		check("bit takes N and V from the operand and Z from the AND",
		      flag(CPU6502_FLAG_N) && flag(CPU6502_FLAG_V) &&
		      flag(CPU6502_FLAG_Z));
		check_u32("and leaves the accumulator alone", cpu.a, 0x01);
	}

	{
		const uint8_t prog[] = { LDA_IMM, 0x10, CMP_IMM, 0x20, BRK };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("cmp of a smaller value clears the carry",
		      !flag(CPU6502_FLAG_C) && !flag(CPU6502_FLAG_Z));
		check_u32("and does not change the accumulator", cpu.a, 0x10);
	}
}

/* ---- boundaries -------------------------------------------------------- */

static void
test_faults(void)
{
	printf("\nFaults:\n");

	{
		/* 0x02 is one of the undocumented opcodes. A real NMOS part jams on
		   it; this core faults, which is more use than either. */
		const uint8_t prog[] = { 0x02 };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("an undocumented opcode faults", cpu.faulted && !cpu.halted);
		check_u32("with the opcode as the fault address", cpu.fault_addr, 0x02);
		check_u32("and the right cause", cpu.fault_cause,
		          CPU6502_FAULT_ILLEGAL);
	}

	{
		/* A core given less than the full address space still checks. A
		   card always gives it 64K, so this can only happen to a test -
		   which is exactly why the check has to be tested. */
		static uint8_t small[512];
		const uint8_t prog[] = { LDA_ABS, 0x00, 0x40, BRK };

		memset(small, 0, sizeof(small));
		memcpy(small, prog, sizeof(prog));
		cpu6502_init(&cpu, small, sizeof(small));
		cpu6502_reset(&cpu, 0);
		(void) run_to_stop();
		check("reading outside the core's memory faults",
		      cpu.faulted && cpu.fault_cause == CPU6502_FAULT_ACCESS);
		check_u32("naming the address", cpu.fault_addr, 0x4000);
	}

	check("both fault causes have names",
	      cpu6502_fault_name(CPU6502_FAULT_ILLEGAL)[0] != '\0' &&
	      cpu6502_fault_name(999)[0] != '\0');
}

static void
test_budget(void)
{
	const uint8_t prog[] = {
		LDX_IMM, 0x64,		/* 100 */
		DEX,			/* loop */
		BNE, 0xfd,
		BRK
	};

	printf("\nRunning on a budget, stopping and resuming:\n");

	load(prog, sizeof(prog));
	check("a run uses exactly the budget it was given",
	      cpu6502_run(&cpu, 9) == 9);
	check("and is not finished", !cpu.halted && !cpu.faulted);
	check_u32("having got four iterations in", cpu.x, 0x64 - 4);

	check("a second run continues from there", cpu6502_run(&cpu, 10) == 10);
	check_u32("reaching nine", cpu.x, 0x64 - 9);

	check("and it eventually finishes", run_to_stop() && cpu.halted);
	check_u32("with the loop having counted down to zero", cpu.x, 0);
	check("a halted core uses no cycles", cpu6502_run(&cpu, 100) == 0);
}

static void
test_reset(void)
{
	const uint8_t prog[] = { LDA_IMM, 0x7f, BRK };

	printf("\nReset:\n");

	load(prog, sizeof(prog));
	(void) run_to_stop();
	check("the core is halted before the reset", cpu.halted);

	cpu6502_reset(&cpu, PROG_BASE);
	check("reset clears the halt", !cpu.halted && !cpu.faulted);
	check_u32("and the program counter is back at the entry", cpu.pc, PROG_BASE);
	check_u32("the stack pointer comes up where a real part leaves it",
	          cpu.sp, 0xfd);
	check("the interrupt disable and unused bits are set",
	      flag(CPU6502_FLAG_I) && flag(CPU6502_FLAG_U));
	check_u32("memory survives the reset, as it does on hardware",
	          ram[PROG_BASE + 1], 0x7f);
	check("and it runs again", run_to_stop() && cpu.a == 0x7f);
}

int
main(void)
{
	printf("MOS 6502 core\n");

	test_sum_loop();
	test_adc_overflow_table();
	test_sbc_overflow_table();
	test_decimal_mode();
	test_jmp_indirect_page_bug();
	test_zero_page_wrap();
	test_transfers_and_stack();
	test_shifts_and_bit();
	test_faults();
	test_budget();
	test_reset();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
