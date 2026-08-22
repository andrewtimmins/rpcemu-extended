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
	/* Cycles, not instructions: LDA #n 2, LDX #n 2, then ten iterations of
	   STX zp 3, CLC 2, ADC zp 3, DEX 2 with a BNE at 3 taken nine times and
	   2 on the tenth, then STA abs 4 and BRK 7. */
	check("cycles were counted",
	      cpu.cycles == 2 + 2 + 10 * (3 + 2 + 3 + 2) + 9 * 3 + 2 + 4 + 7);
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
	{
		/* LDX #n is 2 and an iteration is DEX 2 plus a taken BNE 3, so a
		   budget of 12 is the setup and two iterations. An instruction
		   cannot be split, so a run reaches at least its budget and
		   overshoots by at most the cost of its last instruction. */
		const int used = cpu6502_run(&cpu, 12);

		check("a run uses at least its budget", used >= 12 && used <= 12 + 3);
		check("and is not finished", !cpu.halted && !cpu.faulted);
		check_u32("having got two iterations in", cpu.x, 0x64 - 2);
	}

	{
		const int used = cpu6502_run(&cpu, 10);

		check("a second run continues from there",
		      used >= 10 && used <= 10 + 3);
		check_u32("reaching four", cpu.x, 0x64 - 4);
	}

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

/* ---- the 65C02, which is this core with its CMOS flag set --------------- */

/*
 * ★ WHAT MAKES THIS WORTH TESTING SEPARATELY. The CMOS part is not "the same
 * chip with more instructions": it also changes two behaviours that existing
 * NMOS code could depend on. So the cases below are in three groups - the new
 * instructions do the right thing, the fixed behaviours are fixed, and the new
 * instructions do NOT exist on an NMOS part. That last group matters most: a
 * core that quietly executed 65C02 opcodes on a 6502 would make software look
 * portable when it is not.
 */
static void
load_cmos(const uint8_t *prog, size_t len)
{
	load(prog, len);
	cpu6502_set_cmos(&cpu, 1);
	cpu6502_reset(&cpu, PROG_BASE);
}

static void
test_cmos_instructions(void)
{
	printf("\n65C02: the instructions the NMOS part does not have:\n");

	{
		/* BRA is an unconditional branch, which the NMOS part lacks
		   entirely - a program wanting one had to use a flag it knew. */
		const uint8_t prog[] = { 0x80, 0x02, 0xa9, 0xff, 0xa9, 0x2a, 0x00 };

		load_cmos(prog, sizeof(prog));
		check("BRA branches", run_to_stop() && cpu.halted);
		check_u32("over the instruction it skipped", cpu.a, 0x2a);
	}

	{
		/* STZ: store zero without spending a register on it. */
		const uint8_t prog[] = { 0xa9, 0xff, 0x85, 0x40, 0x64, 0x40, 0x00 };

		load_cmos(prog, sizeof(prog));
		check("STZ runs", run_to_stop());
		check_u32("and zeroed the byte", ram[0x0040], 0);
	}

	{
		/* INC A and DEC A: the NMOS part can only increment memory or the
		   index registers. */
		const uint8_t prog[] = { 0xa9, 0x10, 0x1a, 0x1a, 0x3a, 0x00 };

		load_cmos(prog, sizeof(prog));
		check("INC A and DEC A run", run_to_stop());
		check_u32("10 + 1 + 1 - 1 = 11", cpu.a, 0x11);
	}

	{
		/* PHX/PLY and friends: the NMOS part can only push A and P. */
		const uint8_t prog[] = {
			0xa2, 0x77,		/* LDX #&77 */
			0xda,			/* PHX      */
			0x7a,			/* PLY      */
			0x98,			/* TYA      */
			0x00
		};

		load_cmos(prog, sizeof(prog));
		check("PHX then PLY runs", run_to_stop());
		check_u32("X went out and came back in Y", cpu.a, 0x77);
	}

	{
		/* TSB sets the bits of A in memory and reports whether any were
		   already there. */
		const uint8_t prog[] = {
			0xa9, 0x0f, 0x85, 0x50,	/* &50 = &0f */
			0xa9, 0xf0,		/* A = &f0   */
			0x04, 0x50,		/* TSB &50   */
			0x00
		};

		load_cmos(prog, sizeof(prog));
		check("TSB runs", run_to_stop());
		check_u32("the bits are now set", ram[0x0050], 0xff);
		check("and Z said none of them were already", flag(CPU6502_FLAG_Z));
	}

	{
		/* TRB clears them again. */
		const uint8_t prog[] = {
			0xa9, 0xff, 0x85, 0x51,
			0xa9, 0x0f,
			0x14, 0x51,		/* TRB &51 */
			0x00
		};

		load_cmos(prog, sizeof(prog));
		check("TRB runs", run_to_stop());
		check_u32("the bits are cleared", ram[0x0051], 0xf0);
		check("and Z said they had been set", !flag(CPU6502_FLAG_Z));
	}

	{
		/* LDA (zp): indirect with no index, the addition most code uses. */
		const uint8_t prog[] = {
			0xa9, 0x00, 0x85, 0x60,	/* &60 = &3000 */
			0xa9, 0x30, 0x85, 0x61,
			0xb2, 0x60,		/* LDA (&60) */
			0x00
		};

		/* After load_cmos, which clears the RAM. */
		load_cmos(prog, sizeof(prog));
		ram[0x3000] = 0x5c;
		check("LDA (zp) runs", run_to_stop());
		check_u32("and read through the pointer", cpu.a, 0x5c);
	}

	{
		/* JMP (abs,X): a jump table without writing to your own code. */
		const uint8_t prog[] = {
			0xa2, 0x02,		/* LDX #2, so the second entry */
			0x7c, 0x00, 0x31,	/* JMP (&3100,X) */
			0x00
		};

		load_cmos(prog, sizeof(prog));
		ram[0x3102] = 0x00;
		ram[0x3103] = 0x32;	/* -> &3200 */
		ram[0x3200] = 0xa9;	/* LDA #&99 */
		ram[0x3201] = 0x99;
		ram[0x3202] = 0x00;	/* BRK */
		check("JMP (abs,X) runs", run_to_stop());
		check_u32("through the table entry X selected", cpu.a, 0x99);
	}

	{
		/* STP stops the core, which on this card is what "finished" means. */
		const uint8_t prog[] = { 0xa9, 0x41, 0xdb };

		load_cmos(prog, sizeof(prog));
		check("STP stops the core", run_to_stop() && cpu.halted);
		check_u32("with the accumulator as the result", cpu.exit_code, 0x41);
	}
}

/* The two landing sites for the JMP (abs) test, and the pointer that reaches
   one or the other depending on which part is running. */
static void
plant_jmp_targets(void)
{
	ram[0x30ff] = 0x00;
	ram[0x3000] = 0x40;	/* where an NMOS part looks for the high byte */
	ram[0x3100] = 0x50;	/* where a CMOS part looks                    */
	ram[0x4000] = 0xa9;	/* LDA #&11 : BRK, at &4000                   */
	ram[0x4001] = 0x11;
	ram[0x4002] = 0x00;
	ram[0x5000] = 0xa9;	/* LDA #&22 : BRK, at &5000                   */
	ram[0x5001] = 0x22;
	ram[0x5002] = 0x00;
}

static void
test_cmos_fixes(void)
{
	printf("\n65C02: the two behaviours it changes:\n");

	{
		/*
		 * ★ THE JMP (abs) PAGE-BOUNDARY BUG. An NMOS part reads the high
		 * byte of the target from the same page as the low one, so
		 * JMP (&30FF) takes its high byte from &3000 rather than &3100. The
		 * CMOS part fixed it. Both are checked, because a core that fixed
		 * it for the NMOS part too would break every program written to
		 * work around it.
		 */
		const uint8_t prog[] = { 0x6c, 0xff, 0x30 };

		/* Planted after each load, which clears the RAM. */
		load(prog, sizeof(prog));	/* NMOS */
		plant_jmp_targets();
		check("an NMOS JMP (abs) runs", run_to_stop());
		check_u32("and wrapped within the page, as the part does", cpu.a, 0x11);

		load_cmos(prog, sizeof(prog));
		plant_jmp_targets();
		check("a CMOS JMP (abs) runs", run_to_stop());
		check_u32("and read the high byte from the next page", cpu.a, 0x22);
	}

	{
		/*
		 * Decimal arithmetic: the CMOS part sets N and Z from the BCD
		 * result. On the NMOS part Z comes from the binary sum, which is
		 * why 0x99 + 0x01 leaves it clear there and set here.
		 */
		const uint8_t prog[] = {
			0xf8,			/* SED      */
			0xa9, 0x99,		/* LDA #&99 */
			0x18,			/* CLC      */
			0x69, 0x01,		/* ADC #&01 */
			0x00
		};

		load_cmos(prog, sizeof(prog));
		check("CMOS decimal ADC runs", run_to_stop());
		check_u32("99 + 01 = 00 in BCD", cpu.a, 0x00);
		check("with carry out", flag(CPU6502_FLAG_C));
		check("and Z set from the BCD result", flag(CPU6502_FLAG_Z));
	}
}

static void
test_cmos_only_on_cmos(void)
{
	printf("\n65C02: and none of it exists on an NMOS part:\n");

	{
		static const uint8_t cmos_only[] = {
			0x80, 0x1a, 0x3a, 0x5a, 0x7a, 0xda, 0xfa, 0x64, 0x74,
			0x9c, 0x9e, 0x04, 0x0c, 0x14, 0x1c, 0x89, 0x34, 0x3c,
			0x12, 0x32, 0x52, 0x72, 0x92, 0xb2, 0xd2, 0xf2, 0x7c,
			0xdb
		};
		unsigned i;
		int all_faulted = 1;

		for (i = 0; i < sizeof(cmos_only); i++) {
			const uint8_t prog[] = { cmos_only[i], 0x00, 0x00, 0x00 };

			load(prog, sizeof(prog));	/* NMOS, not CMOS */
			(void) cpu6502_step(&cpu);
			if (!cpu.faulted) {
				printf("      &%02x did not fault on an NMOS part\n",
				       cmos_only[i]);
				all_faulted = 0;
			}
		}
		check("every CMOS-only opcode faults on a 6502", all_faulted);
	}

	{
		/* And the same opcode is fine on the CMOS part, so the guard is
		   about the part and not about the opcode being unimplemented. */
		const uint8_t prog[] = { 0x80, 0x00, 0x00 };

		load_cmos(prog, sizeof(prog));
		(void) cpu6502_step(&cpu);
		check("while BRA is fine on a 65C02", !cpu.faulted);
	}
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
	test_cmos_instructions();
	test_cmos_fixes();
	test_cmos_only_on_cmos();
	test_reset();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
