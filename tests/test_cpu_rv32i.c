/*
 * The RV32IM core that goes in the OPEN Bus co-processor card.
 *
 * WHY THIS CAN BE TESTED AT ALL, and cheaply: src/copro/cpu_rv32i.c executes
 * out of a flat byte array and knows nothing about the bus, the emulator or
 * wxWidgets, so a few hundred bytes of hand-assembled machine code and a plain
 * WHY THIS CAN BE TESTED AT ALL, cheaply: src/copro/cpu_rv32i.c executes
 *
 * ★ THE TEST'S OWN ASSEMBLER IS THE INSTRUMENT, so it is checked first. The
 * encoders below are an independent implementation of the same encoding the core
 * decodes, which is exactly the shape that can hide a fault: an encoder and a
 * decoder that are wrong the same way agree with each other. So the first group
 * of checks pins six instruction words against their known encodings, taken from
 * the RISC-V manual's own examples, before anything is executed. If those pass,
 * an executed program that gives the right answer means the decoder is right.
 *
 * What is pinned down here is the part that would be expensive to get wrong: the
 * arithmetic C leaves undefined and RISC-V defines (divide by zero, the most
 * negative value divided by -1, shifts, arithmetic right shift), x0 being
 * hardwired, sign extension on narrow loads, the faults, and that a run stops on
 * a budget and resumes where it left off.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_rv32i.h"

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
		printf("      got 0x%08x, wanted 0x%08x\n", got, want);
		failures++;
	}
}

/* ---- an assembler ------------------------------------------------------- */

/* Registers, by their ABI names where a name is clearer than a number. */
#define ZERO	0
#define RA	1
#define SP	2
#define T0	5
#define T1	6
#define T2	7
#define A0	10
#define A1	11
#define A2	12

static uint32_t
enc_r(unsigned funct7, unsigned rs2, unsigned rs1, unsigned funct3, unsigned rd,
      unsigned opcode)
{
	return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
	       (rd << 7) | opcode;
}

static uint32_t
enc_i(int imm, unsigned rs1, unsigned funct3, unsigned rd, unsigned opcode)
{
	return (((uint32_t) imm & 0xfffu) << 20) | (rs1 << 15) | (funct3 << 12) |
	       (rd << 7) | opcode;
}

static uint32_t
enc_s(int imm, unsigned rs2, unsigned rs1, unsigned funct3, unsigned opcode)
{
	const uint32_t u = (uint32_t) imm;

	return ((u & 0xfe0u) << 20) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
	       ((u & 0x1fu) << 7) | opcode;
}

static uint32_t
enc_b(int imm, unsigned rs2, unsigned rs1, unsigned funct3, unsigned opcode)
{
	const uint32_t u = (uint32_t) imm;

	return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3fu) << 25) |
	       (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
	       (((u >> 1) & 0xfu) << 8) | (((u >> 11) & 1u) << 7) | opcode;
}

static uint32_t
enc_u(uint32_t imm20, unsigned rd, unsigned opcode)
{
	return (imm20 << 12) | (rd << 7) | opcode;
}

static uint32_t
enc_j(int imm, unsigned rd, unsigned opcode)
{
	const uint32_t u = (uint32_t) imm;

	return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3ffu) << 21) |
	       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xffu) << 12) |
	       (rd << 7) | opcode;
}

/* The instructions the programs below are built from. */
#define ADDI(rd, rs1, imm)	enc_i(imm, rs1, 0, rd, 0x13)
#define SLTI(rd, rs1, imm)	enc_i(imm, rs1, 2, rd, 0x13)
#define XORI(rd, rs1, imm)	enc_i(imm, rs1, 4, rd, 0x13)
#define ORI(rd, rs1, imm)	enc_i(imm, rs1, 6, rd, 0x13)
#define ANDI(rd, rs1, imm)	enc_i(imm, rs1, 7, rd, 0x13)
#define SLLI(rd, rs1, sh)	enc_i(sh, rs1, 1, rd, 0x13)
#define SRLI(rd, rs1, sh)	enc_i(sh, rs1, 5, rd, 0x13)
#define SRAI(rd, rs1, sh)	enc_i(0x400 | (sh), rs1, 5, rd, 0x13)
#define ADD(rd, rs1, rs2)	enc_r(0x00, rs2, rs1, 0, rd, 0x33)
#define SUB(rd, rs1, rs2)	enc_r(0x20, rs2, rs1, 0, rd, 0x33)
#define SLL(rd, rs1, rs2)	enc_r(0x00, rs2, rs1, 1, rd, 0x33)
#define SLT(rd, rs1, rs2)	enc_r(0x00, rs2, rs1, 2, rd, 0x33)
#define SLTU(rd, rs1, rs2)	enc_r(0x00, rs2, rs1, 3, rd, 0x33)
#define SRL(rd, rs1, rs2)	enc_r(0x00, rs2, rs1, 5, rd, 0x33)
#define SRA(rd, rs1, rs2)	enc_r(0x20, rs2, rs1, 5, rd, 0x33)
#define MUL(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 0, rd, 0x33)
#define MULH(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 1, rd, 0x33)
#define MULHSU(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 2, rd, 0x33)
#define MULHU(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 3, rd, 0x33)
#define DIV(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 4, rd, 0x33)
#define DIVU(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 5, rd, 0x33)
#define REM(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 6, rd, 0x33)
#define REMU(rd, rs1, rs2)	enc_r(0x01, rs2, rs1, 7, rd, 0x33)
#define LB(rd, rs1, imm)	enc_i(imm, rs1, 0, rd, 0x03)
#define LH(rd, rs1, imm)	enc_i(imm, rs1, 1, rd, 0x03)
#define LW(rd, rs1, imm)	enc_i(imm, rs1, 2, rd, 0x03)
#define LBU(rd, rs1, imm)	enc_i(imm, rs1, 4, rd, 0x03)
#define LHU(rd, rs1, imm)	enc_i(imm, rs1, 5, rd, 0x03)
#define SB(rs2, rs1, imm)	enc_s(imm, rs2, rs1, 0, 0x23)
#define SH(rs2, rs1, imm)	enc_s(imm, rs2, rs1, 1, 0x23)
#define SW(rs2, rs1, imm)	enc_s(imm, rs2, rs1, 2, 0x23)
#define BEQ(rs1, rs2, off)	enc_b(off, rs2, rs1, 0, 0x63)
#define BNE(rs1, rs2, off)	enc_b(off, rs2, rs1, 1, 0x63)
#define BLT(rs1, rs2, off)	enc_b(off, rs2, rs1, 4, 0x63)
#define BGE(rs1, rs2, off)	enc_b(off, rs2, rs1, 5, 0x63)
#define BLTU(rs1, rs2, off)	enc_b(off, rs2, rs1, 6, 0x63)
#define LUI(rd, imm20)		enc_u(imm20, rd, 0x37)
#define AUIPC(rd, imm20)	enc_u(imm20, rd, 0x17)
#define JAL(rd, off)		enc_j(off, rd, 0x6f)
#define JALR(rd, rs1, imm)	enc_i(imm, rs1, 0, rd, 0x67)
#define FENCE			enc_i(0, 0, 0, 0, 0x0f)
#define ECALL			enc_i(0, 0, 0, 0, 0x73)
#define EBREAK			enc_i(1, 0, 0, 0, 0x73)

/* ---- the machine under test -------------------------------------------- */

#define RAM_SIZE	4096
#define DATA		0x400		/* well clear of any program below */

static uint8_t ram[RAM_SIZE];
static rv32i_state cpu;

/** Load a program at address zero and reset to it. */
static void
load(const uint32_t *prog, unsigned words)
{
	unsigned i;

	memset(ram, 0, sizeof(ram));
	for (i = 0; i < words; i++) {
		ram[i * 4 + 0] = (uint8_t) prog[i];
		ram[i * 4 + 1] = (uint8_t) (prog[i] >> 8);
		ram[i * 4 + 2] = (uint8_t) (prog[i] >> 16);
		ram[i * 4 + 3] = (uint8_t) (prog[i] >> 24);
	}
	rv32i_init(&cpu, ram, RAM_SIZE);
	rv32i_reset(&cpu, 0);
}

static uint32_t
data_word(uint32_t addr)
{
	return (uint32_t) ram[addr] | ((uint32_t) ram[addr + 1] << 8) |
	       ((uint32_t) ram[addr + 2] << 16) | ((uint32_t) ram[addr + 3] << 24);
}

/** Run to a halt or a fault, with a bound so a runaway test cannot hang. */
static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 1000; slices++) {
		if (rv32i_run(&cpu, 1000) == 0) {
			return 1;
		}
	}
	return 0;
}

/* ---- the encoder, checked before it is trusted ------------------------- */

static void
test_encoder(void)
{
	printf("The test's own assembler, against known instruction words:\n");

	/* Six words whose encodings are not in doubt. If the assembler below is
	   wrong, every executed result after this is meaningless, so this comes
	   first and nothing else is believed until it passes. */
	check_u32("addi x1, x0, 1 encodes as 0x00100093",
	          ADDI(RA, ZERO, 1), 0x00100093u);
	check_u32("addi x0, x0, 0 (nop) encodes as 0x00000013",
	          ADDI(ZERO, ZERO, 0), 0x00000013u);
	check_u32("ecall encodes as 0x00000073", ECALL, 0x00000073u);
	check_u32("ebreak encodes as 0x00100073", EBREAK, 0x00100073u);
	check_u32("add x1, x2, x3 encodes as 0x003100b3",
	          ADD(RA, SP, 3), 0x003100b3u);
	check_u32("lui x1, 0x12345 encodes as 0x123450b7",
	          LUI(RA, 0x12345u), 0x123450b7u);

	/* A negative immediate, which is where a sign-extension mistake in the
	   assembler would show up: addi x1, x0, -1 is 0xfff00093. */
	check_u32("addi x1, x0, -1 encodes as 0xfff00093",
	          ADDI(RA, ZERO, -1), 0xfff00093u);
}

/* ---- executing something ----------------------------------------------- */

static void
test_sum_loop(void)
{
	/* Sum 1 to 10, store the total, and stop with it in a0. Exercises a
	   backward conditional branch, which is the one branch a straight-line
	   program never tests. */
	const uint32_t prog[] = {
		ADDI(A0, ZERO, 0),		/* total = 0 */
		ADDI(T0, ZERO, 1),		/* i = 1 */
		ADDI(T1, ZERO, 11),		/* limit */
		ADDI(T2, ZERO, DATA),		/* where to put the answer */
		ADD(A0, A0, T0),		/* loop: total += i */
		ADDI(T0, T0, 1),
		BLT(T0, T1, -8),		/* back to the ADD */
		SW(A0, T2, 0),
		ECALL
	};

	printf("\nA program that loops, stores and stops:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	check("it reaches a stop", run_to_stop());
	check("it halted rather than faulted", cpu.halted && !cpu.faulted);
	check("the stop was an ecall", cpu.halt_reason == RV32I_HALT_ECALL);
	check_u32("1 to 10 summed to 55 in a0", cpu.x[A0], 55);
	check_u32("and 55 reached memory", data_word(DATA), 55);
	check("cycles were counted", cpu.cycles == 4 + 10 * 3 + 2);

	/* An ebreak is a distinguishable stop, which a debugger would need. */
	{
		const uint32_t p2[] = { ADDI(A0, ZERO, 7), EBREAK };

		load(p2, 2);
		(void) run_to_stop();
		check("ebreak halts too", cpu.halted);
		check("and says it was an ebreak",
		      cpu.halt_reason == RV32I_HALT_EBREAK);
		check_u32("with a0 as the exit code", cpu.exit_code, 7);
	}
}

static void
test_x0_and_immediates(void)
{
	const uint32_t prog[] = {
		ADDI(ZERO, ZERO, 42),		/* must be discarded */
		ADDI(T0, ZERO, -1),		/* sign-extended immediate */
		ANDI(T1, T0, 0xff),		/* 0xffffffff & 0xff */
		ORI(T2, ZERO, 0x7ff),
		XORI(A0, T0, -1),		/* ~(-1) == 0 */
		SLTI(A1, T0, 0),		/* -1 < 0 signed */
		LUI(A2, 0xabcdeu),
		ECALL
	};

	printf("\nRegister zero, and immediates:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	(void) run_to_stop();
	check_u32("x0 discards a write", cpu.x[ZERO], 0);
	check_u32("addi sign-extends its immediate", cpu.x[T0], 0xffffffffu);
	check_u32("andi masks", cpu.x[T1], 0xffu);
	check_u32("ori of the largest positive immediate", cpu.x[T2], 0x7ffu);
	check_u32("xori with -1 complements", cpu.x[A0], 0);
	check_u32("slti is signed", cpu.x[A1], 1);
	check_u32("lui puts its immediate in the top 20 bits", cpu.x[A2], 0xabcde000u);
}

static void
test_shifts(void)
{
	const uint32_t prog[] = {
		ADDI(T0, ZERO, -16),		/* 0xfffffff0 */
		SRAI(T1, T0, 4),		/* arithmetic: stays negative */
		SRLI(T2, T0, 4),		/* logical: becomes positive */
		ADDI(A0, ZERO, 1),
		SLLI(A0, A0, 31),		/* 0x80000000 */
		ADDI(A1, ZERO, 36),		/* a shift count over 31 */
		SLL(A2, A0, A1),		/* only the low five bits count */
		ECALL
	};

	printf("\nShifts, including the two that C leaves to the implementation:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	(void) run_to_stop();
	check_u32("srai keeps the sign", cpu.x[T1], 0xffffffffu);
	check_u32("srli does not", cpu.x[T2], 0x0fffffffu);
	check_u32("slli by 31", cpu.x[A0], 0x80000000u);
	/* 0x80000000 << (36 & 31) == 0x80000000 << 4 == 0 */
	check_u32("a shift amount is masked to five bits", cpu.x[A2], 0);
}

static void
test_muldiv(void)
{
	const uint32_t prog[] = {
		ADDI(T0, ZERO, -7),
		ADDI(T1, ZERO, 3),
		MUL(A0, T0, T1),		/* -21 */
		MULH(A1, T0, T1),		/* high half of -21 is all ones */
		DIV(A2, T0, T1),		/* -7 / 3 == -2, truncated toward zero */
		REM(T2, T0, T1),		/* -7 % 3 == -1 */
		ECALL
	};

	printf("\nThe M extension:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	(void) run_to_stop();
	check_u32("mul of a negative", cpu.x[A0], (uint32_t) -21);
	check_u32("mulh gives the high half", cpu.x[A1], 0xffffffffu);
	check_u32("div truncates toward zero", cpu.x[A2], (uint32_t) -2);
	check_u32("rem takes the sign of the dividend", cpu.x[T2], (uint32_t) -1);

	/* The two cases RISC-V defines and C does not. Getting these wrong is
	   either a wrong answer or, with the sanitisers on, a crash. */
	{
		const uint32_t p2[] = {
			ADDI(T0, ZERO, 5),
			ADD(T1, ZERO, ZERO),	/* divisor of zero */
			DIV(A0, T0, T1),
			REM(A1, T0, T1),
			DIVU(A2, T0, T1),
			ECALL
		};

		load(p2, sizeof(p2) / sizeof(p2[0]));
		(void) run_to_stop();
		check_u32("div by zero gives all ones", cpu.x[A0], 0xffffffffu);
		check_u32("rem by zero gives the dividend", cpu.x[A1], 5);
		check_u32("divu by zero gives all ones", cpu.x[A2], 0xffffffffu);
	}

	{
		/* The most negative value divided by -1 overflows. RISC-V says the
		   quotient is that same value and the remainder is zero. */
		const uint32_t p3[] = {
			ADDI(T0, ZERO, 1),
			SLLI(T0, T0, 31),	/* 0x80000000 */
			ADDI(T1, ZERO, -1),
			DIV(A0, T0, T1),
			REM(A1, T0, T1),
			ECALL
		};

		load(p3, sizeof(p3) / sizeof(p3[0]));
		(void) run_to_stop();
		check_u32("the most negative divided by -1 is itself",
		          cpu.x[A0], 0x80000000u);
		check_u32("and its remainder is zero", cpu.x[A1], 0);
	}

	{
		/* MULHSU is the one nobody gets right by accident: signed times
		   unsigned. -1 * 0xffffffff, high half. As a 64-bit product that is
		   -4294967295, whose high word is 0xffffffff. */
		const uint32_t p4[] = {
			ADDI(T0, ZERO, -1),
			ADDI(T1, ZERO, -1),	/* read as unsigned 0xffffffff */
			MULHSU(A0, T0, T1),
			MULHU(A1, T0, T1),
			ECALL
		};

		load(p4, sizeof(p4) / sizeof(p4[0]));
		(void) run_to_stop();
		check_u32("mulhsu of -1 and 0xffffffff", cpu.x[A0], 0xffffffffu);
		check_u32("mulhu of 0xffffffff squared", cpu.x[A1], 0xfffffffeu);
	}
}

static void
test_loads_and_stores(void)
{
	const uint32_t prog[] = {
		ADDI(T0, ZERO, DATA),
		ADDI(T1, ZERO, -1),
		SB(T1, T0, 0),			/* 0xff */
		SH(T1, T0, 2),			/* 0xffff */
		LB(A0, T0, 0),			/* sign-extended */
		LBU(A1, T0, 0),			/* not */
		LH(A2, T0, 2),			/* sign-extended */
		LHU(T2, T0, 2),			/* not */
		ECALL
	};

	printf("\nNarrow loads and stores, and sign extension:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	(void) run_to_stop();
	check("nothing faulted", !cpu.faulted);
	check_u32("lb sign-extends", cpu.x[A0], 0xffffffffu);
	check_u32("lbu does not", cpu.x[A1], 0xffu);
	check_u32("lh sign-extends", cpu.x[A2], 0xffffffffu);
	check_u32("lhu does not", cpu.x[T2], 0xffffu);
	check_u32("sb wrote one byte only", (uint32_t) ram[DATA + 1], 0);
}

static void
test_jumps(void)
{
	const uint32_t prog[] = {
		/*  0 */ JAL(RA, 12),		/* call forward to 12, linking 4 */
		/*  4 */ ADDI(A0, ZERO, 1),	/* only reached by the return */
		/*  8 */ JAL(ZERO, 12),		/* and then out to the ecall at 20 */
		/* 12 */ ADDI(A1, ZERO, 9),	/* the subroutine */
		/* 16 */ JALR(ZERO, RA, 0),	/* return to 4 */
		/* 20 */ ECALL
	};

	printf("\nJumps and the link register:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	check("it stops", run_to_stop());
	check_u32("jal linked the following instruction", cpu.x[RA], 4);
	check_u32("the jump landed where it should", cpu.x[A1], 9);
	/* a0 is only ever set by the instruction at 4, which nothing but the
	   return can reach: proof that the return really went back. */
	check_u32("jalr returned to the linked address", cpu.x[A0], 1);

	{
		/* AUIPC is relative to its own address, which is the whole point. */
		const uint32_t p2[] = { ADDI(ZERO, ZERO, 0), AUIPC(A0, 1), ECALL };

		load(p2, 3);
		(void) run_to_stop();
		check_u32("auipc adds its immediate to its own pc",
		          cpu.x[A0], 0x1000u + 4u);
	}
}

static void
test_faults(void)
{
	printf("\nFaults, which stop the core rather than being ignored:\n");

	{
		const uint32_t prog[] = { ADDI(T0, ZERO, DATA + 1), LW(A0, T0, 0), ECALL };

		load(prog, 3);
		(void) run_to_stop();
		check("a misaligned word load faults", cpu.faulted && !cpu.halted);
		check_u32("with the right cause", cpu.fault_cause,
		          RV32I_FAULT_LOAD_MISALIGNED);
		check_u32("and the offending address", cpu.fault_addr, DATA + 1);
		check_u32("the pc stays on the faulting instruction", cpu.pc, 4);
	}

	{
		const uint32_t prog[] = { 0xffffffffu };

		load(prog, 1);
		(void) run_to_stop();
		check("an undefined instruction faults", cpu.faulted);
		check_u32("as an illegal instruction", cpu.fault_cause,
		          RV32I_FAULT_ILLEGAL);
	}

	{
		/* A CSR instruction: a real encoding this core does not implement,
		   which is a more honest test of the illegal path than a word of
		   all ones. csrrw x1, mstatus, x0 is 0x30001073. */
		const uint32_t prog[] = { 0x30001073u };

		load(prog, 1);
		(void) run_to_stop();
		check("a CSR instruction faults, as the header says it will",
		      cpu.faulted && cpu.fault_cause == RV32I_FAULT_ILLEGAL);
	}

	{
		/* Jumping past the end of RAM: an instruction fetch fault. */
		const uint32_t prog[] = { JAL(ZERO, 0), ECALL };

		load(prog, 2);
		rv32i_reset(&cpu, RAM_SIZE);
		(void) run_to_stop();
		check("fetching outside the core's memory faults",
		      cpu.faulted && cpu.fault_cause == RV32I_FAULT_INSN_ACCESS);
	}

	{
		const uint32_t prog[] = { ECALL };

		load(prog, 1);
		rv32i_reset(&cpu, 2);		/* a misaligned entry point */
		(void) run_to_stop();
		check("a misaligned program counter faults",
		      cpu.faulted && cpu.fault_cause == RV32I_FAULT_INSN_MISALIGNED);
	}

	check("every fault cause has a name",
	      rv32i_fault_name(RV32I_FAULT_ILLEGAL)[0] != '\0' &&
	      rv32i_fault_name(999)[0] != '\0');
}

static void
test_budget(void)
{
	/* A long-running program, so that a budget really does cut it short. The
	   card gives a core a slice at a time, so stopping and resuming exactly
	   is what makes a co-processor share the machine rather than seize it. */
	const uint32_t prog[] = {
		ADDI(T0, ZERO, 0),
		ADDI(T1, ZERO, 100),
		ADDI(T0, T0, 1),		/* loop */
		BLT(T0, T1, -4),
		ECALL
	};
	int used_first, used_second;

	printf("\nRunning on a budget, stopping and resuming:\n");

	load(prog, sizeof(prog) / sizeof(prog[0]));
	used_first = rv32i_run(&cpu, 10);
	check("a run uses exactly the budget it was given", used_first == 10);
	check("and is not finished", !cpu.halted && !cpu.faulted);
	check_u32("having got 4 iterations in", cpu.x[T0], 4);

	used_second = rv32i_run(&cpu, 10);
	check("a second run continues from there", used_second == 10);
	check_u32("reaching 9 iterations", cpu.x[T0], 9);

	check("and it eventually finishes", run_to_stop() && cpu.halted);
	check_u32("with the loop having run to its limit", cpu.x[T0], 100);

	/* A halted core consumes nothing, so a card need not special-case it. */
	check("a halted core uses no cycles", rv32i_run(&cpu, 100) == 0);
	check("and a step of a halted core does nothing", rv32i_step(&cpu) == 0);
	check("a run with no budget executes nothing", rv32i_run(&cpu, 0) == 0);
}

static void
test_fence(void)
{
	const uint32_t prog[] = { FENCE, ADDI(A0, ZERO, 3), ECALL };

	printf("\nFENCE:\n");

	load(prog, 3);
	(void) run_to_stop();
	check("fence executes as a no-op rather than faulting",
	      cpu.halted && !cpu.faulted);
	check_u32("and the instruction after it ran", cpu.x[A0], 3);
}

int
main(void)
{
	printf("RV32IM core\n");

	test_encoder();
	test_sum_loop();
	test_x0_and_immediates();
	test_shifts();
	test_muldiv();
	test_loads_and_stores();
	test_jumps();
	test_faults();
	test_budget();
	test_fence();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
