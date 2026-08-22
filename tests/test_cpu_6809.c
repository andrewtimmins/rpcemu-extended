/*
 * The 6809 core that goes in the OPEN Bus co-processor card.
 *
 * Testable for the same reason as the other cores: src/copro/cpu_6809.c executes
 * out of a flat byte array and knows nothing about the bus or the emulator, so a
 * byte array and some hand-written machine code is the whole harness.
 *
 * ★ WHAT IS WORTH PINNING DOWN ON A 6809. Not that LDA loads: what is peculiar
 * to this processor and what the shape of the code here could get wrong.
 *
 *   - the postbyte, every form of it, because "indexed" on a 6809 is fourteen
 *     addressing modes wearing one opcode and the decode is the largest single
 *     piece of the core. Each form is checked for the address it produces AND
 *     for what it did to the register on the way, since an auto-increment that
 *     moves by the wrong amount reads correctly exactly once.
 *   - the forms that do NOT exist, which must fault. An indirect ",R+" would be
 *     reading half of one pointer and half of the next, and a core that quietly
 *     allowed it would make a program look portable when it is not.
 *   - carry meaning BORROW after a subtraction, which is the 6502's convention
 *     inverted. Every multi-byte subtraction in every 6809 program depends on
 *     it, and getting it backwards would still pass a test that only looked at
 *     the result.
 *   - the grid, because the core decodes &80-&FF by taking the opcode apart
 *     rather than by listing 128 cases. That is the right shape for this
 *     processor and it has its own failure mode: one cell quietly holding the
 *     operation from the cell beside it. So the corners of the grid are checked
 *     against each other - the same operation in all four addressing modes, and
 *     the A and B halves of a column.
 *   - big-endian memory, since this is the only core here that is.
 *   - two stacks, and the bit in a push mask that means "the other one".
 *   - the interrupts, all three, because how much state each pushes differs and
 *     RTI has to read the E flag to know what to take back.
 *
 * Programs live at &0300, leaving the direct page and the space below it free
 * for the tests that write there.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_6809.h"

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
		printf("      got 0x%04x, wanted 0x%04x\n", got, want);
		failures++;
	}
}

/* ---- the machine under test -------------------------------------------- */

#define RAM_SIZE	0x10000		/* the whole 64K address space */
#define PROG_BASE	0x0300

static uint8_t ram[RAM_SIZE];
static cpu6809_state cpu;

static void
load(const uint8_t *prog, unsigned len)
{
	memset(ram, 0, sizeof(ram));
	memcpy(ram + PROG_BASE, prog, len);
	cpu6809_init(&cpu, ram, RAM_SIZE);
	cpu6809_reset(&cpu, PROG_BASE);
}

/** Run to a halt or a fault, bounded so a runaway test cannot hang. */
static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 1000; slices++) {
		if (cpu6809_run(&cpu, 1000) == 0) {
			return 1;
		}
	}
	return 0;
}

/** Execute exactly @n instructions, for the tests that interrupt mid-program. */
static void
step_n(int n)
{
	int i;

	for (i = 0; i < n; i++) {
		(void) cpu6809_step(&cpu);
	}
}

static int
flag(uint8_t bit)
{
	return (cpu.cc & bit) != 0;
}

/* Opcodes used below, named so the programs read as assembler. */
#define LDA_IMM		0x86
#define LDA_DIR		0x96
#define LDA_IDX		0xa6
#define LDA_EXT		0xb6
#define LDB_IMM		0xc6
#define LDB_IDX		0xe6
#define STA_DIR		0x97
#define STA_IDX		0xa7
#define STA_EXT		0xb7
#define LDD_IMM		0xcc
#define LDD_IDX		0xec
#define STD_DIR		0xdd
#define LDX_IMM		0x8e
#define LDX_IDX		0xae
#define STX_EXT		0xbf
#define LDU_IMM		0xce
#define ADDA_IMM	0x8b
#define ADCA_IMM	0x89
#define SUBA_IMM	0x80
#define SBCA_IMM	0x82
#define CMPA_IMM	0x81
#define ANDA_IMM	0x84
#define ORA_IMM		0x8a
#define EORA_IMM	0x88
#define BITA_IMM	0x85
#define ADDD_IMM	0xc3
#define SUBD_IMM	0x83
#define CMPX_IMM	0x8c
#define NEGA		0x40
#define COMA		0x43
#define LSRA		0x44
#define ASLA		0x48
#define DECA		0x4a
#define DECB		0x5a
#define INCA		0x4c
#define CLRA		0x4f
#define CLRB		0x5f
#define TSTA		0x4d
#define NEG_DIR		0x00
#define INC_EXT		0x7c
#define CLR_IDX		0x6f
#define NOP		0x12
#define SYNC		0x13
#define CWAI		0x3c
#define LBRA		0x16
#define LBSR		0x17
#define BSR		0x8d
#define JSR_EXT		0xbd
#define RTS		0x39
#define RTI		0x3b
#define DAA		0x19
#define ORCC		0x1a
#define ANDCC		0x1c
#define SEX		0x1d
#define EXG		0x1e
#define TFR		0x1f
#define MUL		0x3d
#define ABX		0x3a
#define LEAX		0x30
#define LEAS		0x32
#define PSHS		0x34
#define PULS		0x35
#define PSHU		0x36
#define PULU		0x37
#define BRA		0x20
#define BNE		0x26
#define BEQ		0x27
#define BGT		0x2e
#define BLT		0x2d
#define SWI		0x3f
#define PAGE2		0x10
#define PAGE3		0x11

/* ---------------------------------------------------------------- the tests */

/*
 * The smallest program that proves the core runs at all: a counted loop with a
 * conditional branch backwards, which needs the fetch, an accumulator, a
 * decrement setting Z, a signed branch offset and a halt all to work.
 */
static void
test_sum_loop(void)
{
	static const uint8_t prog[] = {
		LDA_IMM, 0x00,
		LDB_IMM, 10,
		ADDA_IMM, 3,		/* the loop starts here */
		DECB,
		BNE, 0xfb,		/* back five bytes, to the ADDA */
		SWI
	};

	printf("A counted loop, which needs most of the core to be right:\n");
	load(prog, sizeof(prog));
	check("it stops", run_to_stop());
	check("halted rather than faulted", cpu.halted && !cpu.faulted);
	check_u32("ten times three", cpu.a, 30);
	check_u32("the counter ran out", cpu.b, 0);
	check_u32("SWI is what stopped it", cpu.halt_reason, CPU6809_HALT_SWI);
	check_u32("and A is the exit code", cpu.exit_code, 30);
}

/*
 * ★ The only big-endian core here. A 16-bit store puts its HIGH byte at the
 * lower address, and a program that reads a word back a byte at a time depends
 * on that; the other three cores in this directory do the opposite.
 */
static void
test_big_endian(void)
{
	static const uint8_t prog[] = {
		LDD_IMM, 0x12, 0x34,
		STD_DIR, 0x40,
		LDX_IMM, 0x00, 0x40,
		LDA_IDX, 0x84,		/* ,X - the high byte */
		LDB_IDX, 0x01,		/* 1,X - the low byte */
		SWI
	};

	printf("\nSixteen bits in memory, high byte first:\n");
	load(prog, sizeof(prog));
	check("it stops", run_to_stop());
	check_u32("the high byte is at the lower address", ram[0x40], 0x12);
	check_u32("and the low byte above it", ram[0x41], 0x34);
	check_u32("reading the first byte back gives the high half", cpu.a, 0x12);
	check_u32("and the second the low half", cpu.b, 0x34);
}

/*
 * ★ Every postbyte form, checked for the address it produced and for what it
 * left the register at. An auto-increment that moves by the wrong amount reads
 * the right byte the first time round a loop and the wrong one for ever after,
 * which is why the register matters as much as the value.
 */
static void
test_postbyte_forms(void)
{
	printf("\nThe postbyte, which is where a 6809 keeps its complexity:\n");

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x05,		/* 5,X as a five-bit offset */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1005] = 0x5a;
		(void) run_to_stop();
		check_u32("a five-bit offset", cpu.a, 0x5a);
		check_u32("and it left X alone", cpu.x, 0x1000);
	}

	{
		/* &1f is -1 in five bits, which is the form a program uses to
		   reach the byte before a pointer. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x1f,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0fff] = 0x77;
		(void) run_to_stop();
		check_u32("a five-bit offset is signed", cpu.a, 0x77);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x80,		/* ,X+  */
			LDB_IDX, 0x80,		/* ,X+ again */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0x11;
		ram[0x1001] = 0x22;
		(void) run_to_stop();
		check_u32(",X+ reads before it increments", cpu.a, 0x11);
		check_u32("and the second read sees the next byte", cpu.b, 0x22);
		check_u32("X advanced by one per access", cpu.x, 0x1002);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDD_IDX, 0x81,		/* ,X++ */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0xab;
		ram[0x1001] = 0xcd;
		(void) run_to_stop();
		check_u32(",X++ reads a word", (uint32_t) ((cpu.a << 8) | cpu.b),
		          0xabcd);
		check_u32("and advances by two", cpu.x, 0x1002);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x02,
			LDA_IDX, 0x82,		/* ,-X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1001] = 0x33;
		(void) run_to_stop();
		check_u32(",-X decrements before it reads", cpu.a, 0x33);
		check_u32("and X is left at what it read", cpu.x, 0x1001);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x02,
			LDD_IDX, 0x83,		/* ,--X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0x44;
		ram[0x1001] = 0x55;
		(void) run_to_stop();
		check_u32(",--X takes two off first",
		          (uint32_t) ((cpu.a << 8) | cpu.b), 0x4455);
		check_u32("and X is left there", cpu.x, 0x1000);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x84,		/* ,X with no offset */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0x66;
		(void) run_to_stop();
		check_u32("a bare ,X", cpu.a, 0x66);
	}

	{
		/* B is signed in B,R, which is what makes it useful for walking
		   backwards through a table. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDB_IMM, 0xff,
			LDA_IDX, 0x85,		/* B,X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0fff] = 0x88;
		(void) run_to_stop();
		check_u32("B,X takes B as signed", cpu.a, 0x88);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IMM, 0x02,
			LDA_IDX, 0x86,		/* A,X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1002] = 0x99;
		(void) run_to_stop();
		check_u32("A,X", cpu.a, 0x99);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x88, 0xf0,	/* -16,X as an eight-bit offset */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0ff0] = 0xaa;
		(void) run_to_stop();
		check_u32("an eight-bit offset, signed", cpu.a, 0xaa);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, 0x89, 0x10, 0x00,	/* 4096,X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x2000] = 0xbb;
		(void) run_to_stop();
		check_u32("a sixteen-bit offset", cpu.a, 0xbb);
	}

	{
		/* D is UNSIGNED in D,R, where B on its own is signed. The two
		   forms sit next to each other in the encoding and disagree. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDD_IMM, 0x00, 0xff,
			LDA_IDX, 0x8b,		/* D,X */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x10ff] = 0xcc;
		(void) run_to_stop();
		check_u32("D,X takes D as unsigned", cpu.a, 0xcc);
	}

	{
		/*
		 * Program-counter relative, which is how position-independent
		 * code reaches its own data. The offset is measured from the byte
		 * AFTER the offset itself: the LDA occupies &0300-&0302, so the
		 * program counter is &0303 when the offset is applied, and an
		 * offset of 4 reaches &0307.
		 */
		static const uint8_t prog[] = {
			LDA_IDX, 0x8c, 0x04,	/* 4,PCR */
			SWI,
			0, 0, 0,
			0xde			/* &0307 */
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("an eight-bit offset from the program counter",
		          cpu.a, 0xde);
	}

	{
		static const uint8_t prog[] = {
			LDA_IDX, 0x8d, 0x00, 0x05,	/* 5,PCR, sixteen bits */
			SWI,
			0, 0, 0, 0,
			0xed				/* &0309 */
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a sixteen-bit offset from the program counter",
		          cpu.a, 0xed);
	}

	{
		/* [n]: the address is in memory, and the register field of the
		   postbyte means nothing at all. */
		static const uint8_t prog[] = {
			LDA_IDX, 0x9f, 0x20, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x2000] = 0x30;		/* the pointer, big-endian */
		ram[0x2001] = 0x40;
		ram[0x3040] = 0xfe;
		(void) run_to_stop();
		check_u32("extended indirect follows the pointer", cpu.a, 0xfe);
	}

	{
		/* An indirect auto-increment, which IS allowed in its by-two
		   form: it walks a table of pointers. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x20, 0x00,
			LDA_IDX, 0x91,		/* [,X++] */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x2000] = 0x30;
		ram[0x2001] = 0x40;
		ram[0x3040] = 0x7e;
		(void) run_to_stop();
		check_u32("[,X++] follows a table of pointers", cpu.a, 0x7e);
		check_u32("and still advances by two", cpu.x, 0x2002);
	}

	{
		/* Which register the postbyte indexes from, since getting the
		   two-bit field the wrong way round would be invisible on X. */
		static const uint8_t prog[] = {
			LDU_IMM, 0x10, 0x00,
			LDA_IDX, 0xc4,		/* ,U */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0x5b;
		(void) run_to_stop();
		check_u32("the register field selects U", cpu.a, 0x5b);
	}
}

/*
 * ★ The postbyte forms that do not exist. An indirect ",R+" would read one byte
 * of one pointer and one of the next, which is not something to invent a meaning
 * for: they fault, exactly as a Z80 opcode does on the 8080 core.
 */
static void
test_postbyte_faults(void)
{
	static const struct {
		uint8_t postbyte;
		const char *what;
	} bad[] = {
		{ 0x87, "postbyte &87 has no form" },
		{ 0x8a, "postbyte &8a has no form" },
		{ 0x8e, "postbyte &8e has no form" },
		{ 0x8f, "a bare &8f faults; only its indirect form exists" },
		{ 0x90, "there is no indirect ,X+" },
		{ 0x92, "there is no indirect ,-X" },
	};
	unsigned i;

	printf("\nThe forms that do not exist, which must not be guessed at:\n");
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDA_IDX, bad[i].postbyte, 0x00, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check(bad[i].what,
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_POSTBYTE &&
		      cpu.fault_addr == bad[i].postbyte);
	}
}

/*
 * ★ Carry means BORROW after a subtraction, which is the 6502's convention
 * inverted. Every multi-byte subtraction depends on it and a test that only
 * looked at the result would pass either way.
 */
static void
test_borrow_convention(void)
{
	printf("\nCarry after a subtraction is a borrow, not the 6502's carry:\n");

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x10,
			SUBA_IMM, 0x20,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("the result wraps", cpu.a, 0xf0);
		check("and a subtraction that needed a borrow sets C",
		      flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x20,
			SUBA_IMM, 0x10,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and one that did not", cpu.a, 0x10);
		check("leaves C clear", !flag(CPU6809_FLAG_C));
	}

	{
		/* SBC takes the borrow back in, so a two-byte subtraction that
		   borrowed out of the low half must lose one from the high. */
		static const uint8_t prog[] = {
			LDA_IMM, 0x00,		/* low half of &0300 */
			SUBA_IMM, 0x01,		/* borrows */
			LDA_IMM, 0x03,		/* high half */
			SBCA_IMM, 0x00,		/* takes the borrow */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("SBC takes the borrow back in", cpu.a, 0x02);
	}

	{
		/* CMP is a subtraction that keeps nothing but the flags. */
		static const uint8_t prog[] = {
			LDA_IMM, 0x42,
			CMPA_IMM, 0x42,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("CMP leaves the accumulator alone", cpu.a, 0x42);
		check("and says they were equal", flag(CPU6809_FLAG_Z));
	}
}

/* The eight-case overflow table, which is the flag nobody gets right first go. */
static void
test_overflow_table(void)
{
	static const struct {
		uint8_t a, m, result;
		int v, c;
	} cases[] = {
		{ 0x50, 0x10, 0x60, 0, 0 },	/* positive, no overflow */
		{ 0x50, 0x50, 0xa0, 1, 0 },	/* positive overflowing */
		{ 0x50, 0x90, 0xe0, 0, 0 },	/* different signs cannot */
		{ 0x50, 0xd0, 0x20, 0, 1 },
		{ 0xd0, 0x10, 0xe0, 0, 0 },
		{ 0xd0, 0x50, 0x20, 0, 1 },
		{ 0xd0, 0x90, 0x60, 1, 1 },	/* negative overflowing */
		{ 0xd0, 0xd0, 0xa0, 0, 1 },
	};
	unsigned i;

	printf("\nOverflow and carry on an eight-bit add, all eight cases:\n");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const uint8_t prog[] = {
			LDA_IMM, cases[i].a,
			ADDA_IMM, cases[i].m,
			SWI
		};
		char what[80];

		load(prog, sizeof(prog));
		(void) run_to_stop();
		snprintf(what, sizeof(what), "&%02x + &%02x = &%02x, V=%d C=%d",
		         cases[i].a, cases[i].m, cases[i].result,
		         cases[i].v, cases[i].c);
		check(what, cpu.a == cases[i].result &&
		      flag(CPU6809_FLAG_V) == cases[i].v &&
		      flag(CPU6809_FLAG_C) == cases[i].c);
	}
}

/*
 * The read-modify-write column, and its one genuine oddity: COM sets carry
 * whatever it was, which is a 6809 peculiarity and not a mistake.
 */
static void
test_rmw_column(void)
{
	printf("\nThe read-modify-write operations:\n");

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x0f,
			ANDCC, 0xfe,		/* clear C, so the change is visible */
			COMA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("COM complements", cpu.a, 0xf0);
		check("and always sets C, which is the 6809's own oddity",
		      flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x01,
			NEGA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("NEG negates", cpu.a, 0xff);
		check("and borrows, since it subtracts from zero",
		      flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x00,
			NEGA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("negating zero does not borrow", !flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x80,
			NEGA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("negating &80 overflows, being the one value that cannot",
		      flag(CPU6809_FLAG_V));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x7f,
			INCA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("INC increments", cpu.a, 0x80);
		check("and overflows only at &7f", flag(CPU6809_FLAG_V));
	}

	{
		/* INC leaves carry alone, which is what lets it be used inside a
		   multi-byte addition. */
		static const uint8_t prog[] = {
			ORCC, 0x01,		/* set C */
			LDA_IMM, 0x01,
			INCA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("INC does not touch carry", flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0xc0,
			ASLA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ASL shifts up", cpu.a, 0x80);
		check("carrying out the top bit", flag(CPU6809_FLAG_C));
		/* ★ V is not "did it carry", it is "did the sign change". &c0
		   shifts to &80 and stays negative, so V stays clear even though
		   C is set - which is the pair of checks that tells the two
		   apart. */
		check("and V stays clear when the sign did not change",
		      !flag(CPU6809_FLAG_V));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x40,
			ASLA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("shifting &40 up", cpu.a, 0x80);
		check("does not carry", !flag(CPU6809_FLAG_C));
		check("but does change the sign, which is what V reports",
		      flag(CPU6809_FLAG_V));
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x81,
			LSRA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("LSR shifts down", cpu.a, 0x40);
		check("carrying out the bottom bit", flag(CPU6809_FLAG_C));
		check("and always clearing N", !flag(CPU6809_FLAG_N));
	}

	{
		/* CLR sets Z and clears everything else it touches, and it is the
		   only member of the column that ignores what was there. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			CLR_IDX, 0x84,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x1000] = 0xff;
		(void) run_to_stop();
		check_u32("CLR clears memory", ram[0x1000], 0x00);
		check("and says so in Z", flag(CPU6809_FLAG_Z));
	}

	{
		/* TST is in the same column and must NOT write back. */
		static const uint8_t prog[] = {
			LDA_IMM, 0x00,
			TSTA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("TST sets Z from what it read", flag(CPU6809_FLAG_Z));
	}

	{
		/* The same operation reached through memory rather than the
		   accumulator, which is the other half of the column. */
		static const uint8_t prog[] = {
			NEG_DIR, 0x40,
			INC_EXT, 0x10, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0040] = 0x01;
		ram[0x1000] = 0x41;
		(void) run_to_stop();
		check_u32("NEG on a direct address", ram[0x0040], 0xff);
		check_u32("INC on an extended one", ram[0x1000], 0x42);
	}
}

/*
 * ★ The grid, checked at its corners. The core decodes &80-&FF by taking the
 * opcode apart, so the failure to look for is one cell holding the operation
 * from the cell beside it: the same operation in all four addressing modes must
 * agree, and the A and B halves of a column must differ only in the register.
 */
static void
test_grid(void)
{
	printf("\nThe grid: one operation reached four ways, and its two halves:\n");

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x01,
			ADDA_IMM, 0x01,		/* immediate */
			ADDA_IMM + 0x10, 0x40,	/* direct, &0040 */
			ADDA_IMM + 0x20, 0x84,	/* indexed, ,X */
			ADDA_IMM + 0x30, 0x10, 0x00,	/* extended */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0040] = 0x01;
		ram[0x1000] = 0x01;
		cpu.x = 0x1000;
		(void) run_to_stop();
		check_u32("ADDA is one operation in four modes", cpu.a, 0x05);
	}

	{
		/* The B half of the same column, and the two must not share a
		   register. */
		static const uint8_t prog[] = {
			LDA_IMM, 0x10,
			LDB_IMM, 0x20,
			ADDA_IMM, 0x01,
			ADDA_IMM + 0x40, 0x02,	/* ADDB */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ADDA touched only A", cpu.a, 0x11);
		check_u32("and ADDB only B", cpu.b, 0x22);
	}

	{
		/* Every 8-bit column of the grid, so a swapped pair shows up. */
		static const uint8_t prog[] = {
			LDA_IMM, 0xf0,
			ANDA_IMM, 0x3c,		/* &30 */
			ORA_IMM, 0x03,		/* &33 */
			EORA_IMM, 0x11,		/* &22 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("AND, OR and EOR are not each other", cpu.a, 0x22);
	}

	{
		/* BIT is AND that keeps nothing, next door to AND in the grid. */
		static const uint8_t prog[] = {
			LDA_IMM, 0xf0,
			BITA_IMM, 0x0f,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("BIT keeps no result", cpu.a, 0xf0);
		check("but sets Z", flag(CPU6809_FLAG_Z));
	}

	{
		/* The 16-bit columns, and which register each one reaches. */
		static const uint8_t prog[] = {
			LDD_IMM, 0x00, 0x01,
			ADDD_IMM, 0x00, 0xff,	/* &0100 */
			LDX_IMM, 0x12, 0x34,
			LDU_IMM, 0x56, 0x78,
			PAGE2, LDX_IMM, 0x9a, 0xbc,	/* LDY */
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ADDD works on the pair", (uint32_t) ((cpu.a << 8) | cpu.b),
		          0x0100);
		check_u32("LDX reaches X", cpu.x, 0x1234);
		check_u32("LDU reaches U", cpu.u, 0x5678);
		check_u32("the same column on page two is LDY", cpu.y, 0x9abc);
		check_u32("and its other half is LDS", cpu.s, 0x2000);
	}

	{
		/* ★ Column &C is a different comparison on each of the three
		   pages, which is the whole reason the pages exist. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x11, 0x11,
			CMPX_IMM, 0x11, 0x11,		/* page one: CMPX */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("CMPX compares X", flag(CPU6809_FLAG_Z));
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDX_IMM, 0x22, 0x22,	/* LDY */
			PAGE2, CMPX_IMM, 0x22, 0x22,	/* CMPY */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("the same column on page two compares Y",
		      flag(CPU6809_FLAG_Z));
	}

	{
		static const uint8_t prog[] = {
			LDU_IMM, 0x33, 0x33,
			PAGE3, SUBD_IMM, 0x33, 0x33,	/* CMPU */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("and on page three, column three compares U",
		      flag(CPU6809_FLAG_Z));
	}

	{
		/* The store columns, which write rather than read and have no
		   immediate form at all. */
		static const uint8_t prog[] = {
			LDA_IMM, 0x5a,
			STA_DIR, 0x40,
			LDX_IMM, 0x20, 0x00,
			STA_IDX, 0x84,
			STA_EXT, 0x30, 0x00,
			LDX_IMM, 0xbe, 0xef,
			STX_EXT, 0x40, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a store, direct", ram[0x0040], 0x5a);
		check_u32("indexed", ram[0x2000], 0x5a);
		check_u32("and extended", ram[0x3000], 0x5a);
		check_u32("and a sixteen-bit store, high byte first",
		          (uint32_t) ((ram[0x4000] << 8) | ram[0x4001]), 0xbeef);
	}
}

/* The odds and ends, each of which is its own small rule. */
static void
test_odds_and_ends(void)
{
	printf("\nThe instructions that are each their own rule:\n");

	{
		static const uint8_t prog[] = {
			LDA_IMM, 12,
			LDB_IMM, 12,
			MUL,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("MUL puts a 16-bit product in the pair",
		          (uint32_t) ((cpu.a << 8) | cpu.b), 144);
		check("and reports bit seven of the low half in C, for rounding",
		      flag(CPU6809_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDB_IMM, 0xf0,
			SEX,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("SEX fills A from B's sign", cpu.a, 0xff);
		check("and sets N from the pair", flag(CPU6809_FLAG_N));
	}

	{
		/* ★ B is UNSIGNED in ABX, where it is signed in the B,R postbyte.
		   The two are a byte apart in a listing and disagree. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDB_IMM, 0xff,
			ABX,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ABX takes B as unsigned", cpu.x, 0x10ff);
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x19,
			ADDA_IMM, 0x01,		/* &1a, and a half carry */
			DAA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("DAA fixes up a decimal addition", cpu.a, 0x20);
	}

	{
		/*
		 * ★ THE CASE THAT NEEDS THE HALF CARRY, and the reason H exists
		 * at all. 8 + 9 is &11 in binary, whose low nibble is a perfectly
		 * legal 1: nothing about the result says a fix-up is due. Only H,
		 * set because the low nibble carried, knows. A DAA that looked
		 * only at the digits would leave this as &11 and be wrong by six,
		 * and the &19 + 1 case above would not notice.
		 */
		static const uint8_t prog[] = {
			LDA_IMM, 0x08,
			ADDA_IMM, 0x09,		/* &11, and a half carry */
			DAA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and does so from the half carry when the digits look legal",
		          cpu.a, 0x17);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x12, 0x34,
			TFR, 0x12,		/* TFR X,Y */
			LDA_IMM, 0xaa,
			TFR, 0x89,		/* TFR A,B */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("TFR moves a sixteen-bit register", cpu.y, 0x1234);
		check_u32("and an eight-bit one", cpu.b, 0xaa);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x11, 0x11,
			LDU_IMM, 0x22, 0x22,
			EXG, 0x13,		/* EXG X,U */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("EXG swaps both ways, X", cpu.x, 0x2222);
		check_u32("and U", cpu.u, 0x1111);
	}

	{
		/* ★ Mixing widths faults rather than inventing an answer. */
		static const uint8_t prog[] = {
			TFR, 0x81,		/* TFR A,X - eight into sixteen */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("transferring between different widths faults",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_POSTBYTE);
	}

	{
		/* ★ LEAX sets Z and LEAS does not, which is not arbitrary: the
		   index registers are how a program walks a list, and the stack
		   pointers must be adjustable without disturbing a flag. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x00, 0x00,
			LEAX, 0x84,		/* LEAX ,X - the result is zero */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("LEAX sets Z when its result is zero", flag(CPU6809_FLAG_Z));
	}

	{
		static const uint8_t prog[] = {
			ORCC, 0x04,		/* set Z by hand */
			LEAS, 0x61,		/* LEAS 1,S - a non-zero result */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("LEAS leaves Z alone, whatever its result",
		      flag(CPU6809_FLAG_Z));
	}

	{
		static const uint8_t prog[] = {
			ORCC, 0x01,
			ANDCC, 0xfe,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("ORCC then ANDCC leave the flag as the pair says",
		      !flag(CPU6809_FLAG_C));
	}
}

/*
 * ★ Two stacks, and the bit in a push mask that means "the other one". PSHS
 * pushes U and PSHU pushes S, so a mask bit that reached the wrong register
 * would corrupt the stack it was pushing onto.
 */
static void
test_stacks(void)
{
	printf("\nBoth stacks, and the bit that means the other one:\n");

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			LDA_IMM, 0x11,
			LDB_IMM, 0x22,
			LDX_IMM, 0x33, 0x44,
			PSHS, 0x16,		/* A, B and X */
			CLRA,
			CLRB,
			LDX_IMM, 0x00, 0x00,
			PULS, 0x16,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("A comes back", cpu.a, 0x11);
		check_u32("B comes back", cpu.b, 0x22);
		check_u32("X comes back whole", cpu.x, 0x3344);
		check_u32("and the stack pointer is where it started",
		          cpu.s, 0x2000);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			LDU_IMM, 0xbe, 0xef,
			PSHS, 0x40,		/* the U bit, onto the S stack */
			LDU_IMM, 0x00, 0x00,
			PULS, 0x40,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("PSHS's fourth bit pushes U", cpu.u, 0xbeef);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			LDU_IMM, 0x28, 0x00,
			LDA_IMM, 0x77,
			PSHU, 0x02,		/* A onto the USER stack */
			CLRA,
			PULU, 0x02,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("the user stack works the same way", cpu.a, 0x77);
		check_u32("and it is U that moved", cpu.u, 0x2800);
		check_u32("with S untouched", cpu.s, 0x2000);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			BSR, 0x03,		/* to &0307 */
			SWI,
			NOP,
			NOP,
			LDA_IMM, 0x5a,		/* &0307 */
			RTS,
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("BSR and RTS make a subroutine", cpu.exit_code, 0x5a);
		check_u32("and the stack is balanced afterwards", cpu.s, 0x2000);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			JSR_EXT, 0x04, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0400] = LDA_IMM;
		ram[0x0401] = 0x3c;
		ram[0x0402] = RTS;
		(void) run_to_stop();
		check_u32("JSR reaches an extended address", cpu.exit_code, 0x3c);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			LBSR, 0x00, 0x02,	/* to &0309 */
			SWI,
			NOP,
			LDA_IMM, 0x2d,		/* &0309 */
			RTS,
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and LBSR reaches a sixteen-bit displacement",
		          cpu.exit_code, 0x2d);
	}
}

/* The signed branch conditions, which are the ones that get confused. */
static void
test_branches(void)
{
	static const struct {
		uint8_t a, m, op;
		int taken;
		const char *what;
	} cases[] = {
		{ 0x05, 0x03, BGT, 1, "BGT when it is greater" },
		{ 0x03, 0x05, BGT, 0, "BGT when it is not" },
		{ 0x80, 0x7f, BLT, 1, "BLT compares as signed, so &80 is less than &7f" },
		{ 0x7f, 0x80, BGT, 1, "and &7f is greater than &80" },
		{ 0x05, 0x05, BEQ, 1, "BEQ on equal" },
		{ 0x05, 0x06, BNE, 1, "BNE on unequal" },
	};
	unsigned i;

	printf("\nThe branch conditions, chiefly the signed ones:\n");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const uint8_t prog[] = {
			LDA_IMM, cases[i].a,
			CMPA_IMM, cases[i].m,
			cases[i].op, 0x02,
			LDA_IMM, 0x00,		/* skipped if the branch is taken */
			LDB_IMM, 0x01,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check(cases[i].what,
		      cases[i].taken ? (cpu.a == cases[i].a) : (cpu.a == 0x00));
	}

	{
		/* A long branch reaches further and is the only branch whose cost
		   depends on being taken. */
		static const uint8_t prog[] = {
			LBRA, 0x00, 0x02,
			SWI,
			NOP,
			LDA_IMM, 0x6b,		/* &0305 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("LBRA jumps by a sixteen-bit displacement",
		          cpu.exit_code, 0x6b);
	}

	{
		static const uint8_t prog[] = {
			LDA_IMM, 0x01,
			CMPA_IMM, 0x01,
			PAGE2, BEQ, 0x00, 0x02,		/* LBEQ */
			SWI,
			NOP,
			LDA_IMM, 0x4c,			/* &030a */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and a long conditional branch does too",
		          cpu.exit_code, 0x4c);
	}
}

/*
 * ★ The interrupts, all three, because how much state each pushes differs and
 * RTI reads the E flag to know what to take back. A FIRQ that pushed twelve
 * bytes, or an RTI that took twelve off a two-byte frame, would return to a
 * plausible-looking wrong address.
 */
static void
test_interrupts(void)
{
	printf("\nThree interrupt lines, and how much each one pushes:\n");

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			ANDCC, 0xaf,			/* clear I and F */
			NOP,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_IRQ] = 0x04;		/* the handler, big-endian */
		ram[CPU6809_VEC_IRQ + 1] = 0x00;
		step_n(2);				/* set up, then unmask */
		check("an unmasked IRQ is taken", cpu6809_irq(&cpu) != 0);
		check_u32("it vectors through &fff8", cpu.pc, 0x0400);
		check("and records that it pushed everything",
		      (cpu.cc & CPU6809_FLAG_E) != 0);
		check_u32("which is twelve bytes", 0x2000 - cpu.s, 12);
		check("with the mask now set, so it cannot recurse",
		      (cpu.cc & CPU6809_FLAG_I) != 0);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		step_n(1);
		/* Reset leaves both masks set, which is the state a real part
		   comes up in, so this one must be refused. */
		check("a masked IRQ is refused", cpu6809_irq(&cpu) == 0);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			ANDCC, 0xaf,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_FIRQ] = 0x05;
		ram[CPU6809_VEC_FIRQ + 1] = 0x00;
		step_n(2);
		check("a fast interrupt is taken", cpu6809_firq(&cpu) != 0);
		check_u32("through its own vector", cpu.pc, 0x0500);
		check("★ and pushes only two things, saying so by leaving E clear",
		      (cpu.cc & CPU6809_FLAG_E) == 0);
		check_u32("which is three bytes", 0x2000 - cpu.s, 3);
		check("masking both lines, not just its own",
		      (cpu.cc & CPU6809_FLAG_F) != 0 &&
		      (cpu.cc & CPU6809_FLAG_I) != 0);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_NMI] = 0x06;
		ram[CPU6809_VEC_NMI + 1] = 0x00;
		step_n(1);
		cpu6809_nmi(&cpu);
		check_u32("the non-maskable one is taken even masked", cpu.pc,
		          0x0600);
		check_u32("and pushes everything", 0x2000 - cpu.s, 12);
	}

	{
		/* An interrupt and a return from it, end to end: the handler must
		   come back to the instruction that was next. */
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			ANDCC, 0xaf,
			LDA_IMM, 0x01,
			LDB_IMM, 0x02,			/* interrupted before this */
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_IRQ] = 0x04;
		ram[CPU6809_VEC_IRQ + 1] = 0x00;
		ram[0x0400] = LDA_IMM;			/* the handler clobbers A */
		ram[0x0401] = 0xff;
		ram[0x0402] = RTI;
		step_n(3);
		(void) cpu6809_irq(&cpu);
		(void) run_to_stop();
		check_u32("RTI restores what the handler clobbered", cpu.a, 0x01);
		check_u32("and the interrupted program carries on", cpu.b, 0x02);
		check_u32("with the stack balanced", cpu.s, 0x2000);
	}

	{
		/* RTI from a FIRQ frame must take two bytes off and not twelve,
		   which is the E flag doing its job. */
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			ANDCC, 0xaf,
			LDA_IMM, 0x01,
			LDB_IMM, 0x02,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_FIRQ] = 0x05;
		ram[CPU6809_VEC_FIRQ + 1] = 0x00;
		ram[0x0500] = RTI;
		step_n(3);
		(void) cpu6809_firq(&cpu);
		(void) run_to_stop();
		check_u32("RTI from a fast frame takes back only what was pushed",
		          cpu.b, 0x02);
		check_u32("and the stack is balanced", cpu.s, 0x2000);
	}
}

/*
 * ★ SWI stops the core and SWI2 and SWI3 vector. That is a decision rather than
 * an accident: a program on a card needs a way to say "finished, here is the
 * answer", and the other two are what a guest operating system would claim.
 */
static void
test_swi_family(void)
{
	printf("\nSWI stops; SWI2 and SWI3 vector:\n");

	{
		static const uint8_t prog[] = { LDA_IMM, 0x2a, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("SWI halts", cpu.halted && !cpu.faulted);
		check_u32("with A as the exit code", cpu.exit_code, 0x2a);
	}

	/*
	 * ★ A handler cannot hand a value back in a register: RTI restores every
	 * one of them, which is the whole point of it. So the handler leaves its
	 * answer in memory and the interrupted program picks it up, which is also
	 * how a real one would communicate.
	 */
	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,	/* LDS #&2000 */
			PAGE2, SWI,			/* SWI2 */
			LDA_DIR, 0x40,			/* what the handler left */
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_SWI2] = 0x04;
		ram[CPU6809_VEC_SWI2 + 1] = 0x00;
		ram[0x0400] = LDA_IMM;
		ram[0x0401] = 0x07;
		ram[0x0402] = STA_DIR;
		ram[0x0403] = 0x40;
		ram[0x0404] = RTI;
		(void) run_to_stop();
		check_u32("SWI2 vectors, runs a handler and comes back",
		          cpu.exit_code, 0x07);
		check_u32("and the stack is balanced by the RTI", cpu.s, 0x2000);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			PAGE3, SWI,			/* SWI3 */
			LDA_DIR, 0x40,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_SWI3] = 0x04;
		ram[CPU6809_VEC_SWI3 + 1] = 0x00;
		ram[0x0400] = LDA_IMM;
		ram[0x0401] = 0x09;
		ram[0x0402] = STA_DIR;
		ram[0x0403] = 0x40;
		ram[0x0404] = RTI;
		(void) run_to_stop();
		check_u32("and SWI3 through its own vector", cpu.exit_code, 0x09);
	}
}

/* The boundaries: what faults, and what a fault says. */
static void
test_faults(void)
{
	printf("\nWhat faults, and what it says when it does:\n");

	{
		static const uint8_t prog[] = { 0x01, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("an opcode that does not exist faults",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_ILLEGAL);
		check_u32("and says which one", cpu.fault_addr, 0x01);
	}

	{
		/* ★ The prefix is kept, so a log can tell &1083 from &83. */
		static const uint8_t prog[] = { PAGE2, 0x01, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("an opcode that does not exist on page two faults",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_ILLEGAL);
		check_u32("reported with its prefix", cpu.fault_addr, 0x1001);
	}

	{
		/* ★ SYNC and CWAI stop the processor until an interrupt arrives,
		   which this card does not model. Faulting says so; running on
		   would silently pretend the wait had happened. */
		static const uint8_t prog[] = { SYNC, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("SYNC faults rather than pretending to wait",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_WAIT);
	}

	{
		static const uint8_t prog[] = { CWAI, 0xff, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("and so does CWAI",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_WAIT);
	}

	{
		/* A core given less than the full address space still checks:
		   a smaller array is what a test hands it. */
		static uint8_t small[0x400];
		static const uint8_t prog[] = {
			LDA_EXT, 0x80, 0x00,
			SWI
		};

		memset(small, 0, sizeof(small));
		memcpy(small + 0x300, prog, sizeof(prog));
		cpu6809_init(&cpu, small, sizeof(small));
		cpu6809_reset(&cpu, 0x300);
		(void) run_to_stop();
		check("an access outside the array faults",
		      cpu.faulted && cpu.fault_cause == CPU6809_FAULT_ACCESS);
		check_u32("naming the address", cpu.fault_addr, 0x8000);
	}

	{
		check("a fault cause always has a name",
		      cpu6809_fault_name(CPU6809_FAULT_POSTBYTE) != NULL &&
		      cpu6809_fault_name(999) != NULL);
	}
}

/*
 * The cycle counts. These are what "run one frame" is built on, so a wrong entry
 * makes an emulated machine run at the wrong speed rather than fail.
 */
static void
test_cycles(void)
{
	printf("\nCycle counts, including the ones a table cannot hold:\n");

	{
		static const uint8_t prog[] = { NOP, SWI };

		load(prog, sizeof(prog));
		check_u32("NOP is two", (uint32_t) cpu6809_step(&cpu), 2);
	}

	{
		static const uint8_t prog[] = { LDA_IMM, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("an immediate load is two",
		          (uint32_t) cpu6809_step(&cpu), 2);
	}

	{
		static const uint8_t prog[] = { LDA_EXT, 0x10, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("an extended load is five",
		          (uint32_t) cpu6809_step(&cpu), 5);
	}

	{
		/* ★ An indexed instruction's cost is its opcode PLUS what the
		   postbyte charged, which is why the table alone cannot say. */
		static const uint8_t prog[] = { LDA_IDX, 0x84, SWI };

		load(prog, sizeof(prog));
		check_u32("an indexed load with no offset is four",
		          (uint32_t) cpu6809_step(&cpu), 4);
	}

	{
		static const uint8_t prog[] = { LDA_IDX, 0x05, SWI };

		load(prog, sizeof(prog));
		check_u32("a five-bit offset costs one more",
		          (uint32_t) cpu6809_step(&cpu), 5);
	}

	{
		static const uint8_t prog[] = { LDA_IDX, 0x89, 0x00, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("a sixteen-bit offset costs four more",
		          (uint32_t) cpu6809_step(&cpu), 8);
	}

	{
		/* Indirection adds three to whatever the form already cost. */
		static const uint8_t prog[] = { LDA_IDX, 0x91, SWI };

		load(prog, sizeof(prog));
		check_u32("[,X++] is four plus three plus three",
		          (uint32_t) cpu6809_step(&cpu), 10);
	}

	{
		static const uint8_t prog[] = { MUL, SWI };

		load(prog, sizeof(prog));
		check_u32("MUL is eleven", (uint32_t) cpu6809_step(&cpu), 11);
	}

	{
		static const uint8_t prog[] = { SWI };

		load(prog, sizeof(prog));
		check_u32("SWI is nineteen", (uint32_t) cpu6809_step(&cpu), 19);
	}

	{
		/* ★ A short branch costs the same taken or not; a long one does
		   not. Both directions are checked, because a table entry that
		   held the taken cost would be one out on every miss. */
		static const uint8_t prog[] = {
			ORCC, 0x04,		/* set Z, so BEQ is taken */
			BEQ, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("a short branch taken is three",
		          (uint32_t) cpu6809_step(&cpu), 3);
	}

	{
		static const uint8_t prog[] = {
			ANDCC, 0xfb,		/* clear Z, so BEQ is not taken */
			BEQ, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("and not taken is also three",
		          (uint32_t) cpu6809_step(&cpu), 3);
	}

	{
		static const uint8_t prog[] = {
			ORCC, 0x04,
			PAGE2, BEQ, 0x00, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("a long branch taken is six",
		          (uint32_t) cpu6809_step(&cpu), 6);
	}

	{
		static const uint8_t prog[] = {
			ANDCC, 0xfb,
			PAGE2, BEQ, 0x00, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("and not taken is five",
		          (uint32_t) cpu6809_step(&cpu), 5);
	}

	{
		/* ★ A push costs five plus a byte per byte moved, so the register
		   list is part of the price. */
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			PSHS, 0x02,		/* A alone: one byte */
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("pushing one byte is six",
		          (uint32_t) cpu6809_step(&cpu), 6);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			PSHS, 0xff,		/* everything: twelve bytes */
			SWI
		};

		load(prog, sizeof(prog));
		(void) cpu6809_step(&cpu);
		check_u32("and pushing all twelve is seventeen",
		          (uint32_t) cpu6809_step(&cpu), 17);
	}

	{
		/* ★ RTI costs six or fifteen, and the E flag it has just pulled
		   is what decides. The table cannot know. */
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			ANDCC, 0xaf,
			NOP,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_IRQ] = 0x04;
		ram[CPU6809_VEC_IRQ + 1] = 0x00;
		ram[0x0400] = RTI;
		step_n(2);
		(void) cpu6809_irq(&cpu);
		check_u32("RTI from a full frame is fifteen",
		          (uint32_t) cpu6809_step(&cpu), 15);
	}

	{
		static const uint8_t prog[] = {
			PAGE2, LDU_IMM, 0x20, 0x00,
			ANDCC, 0xaf,
			NOP,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6809_VEC_FIRQ] = 0x05;
		ram[CPU6809_VEC_FIRQ + 1] = 0x00;
		ram[0x0500] = RTI;
		step_n(2);
		(void) cpu6809_firq(&cpu);
		check_u32("and from a fast one is six",
		          (uint32_t) cpu6809_step(&cpu), 6);
	}

	{
		/* The running total, and that a bounded run resumes exactly where
		   it stopped rather than losing or repeating an instruction. */
		static const uint8_t prog[] = {
			NOP, NOP, NOP, NOP, NOP,
			LDA_IMM, 0x5e,
			SWI
		};

		load(prog, sizeof(prog));
		check_u32("a run stops at its budget and overshoots by at most one",
		          (uint32_t) cpu6809_run(&cpu, 5), 6);
		(void) run_to_stop();
		check_u32("and what is left runs to the end", cpu.exit_code, 0x5e);
		check_u32("with every cycle accounted for", (uint32_t) cpu.cycles,
		          2 * 5 + 2 + 19);
	}
}

/* Reset, which has to leave a real part's state and not a zeroed struct. */
static void
test_reset(void)
{
	static const uint8_t prog[] = { SWI };

	printf("\nReset:\n");
	load(prog, sizeof(prog));
	cpu.a = 0xff;
	cpu.dp = 0x40;
	cpu.cc = 0;
	cpu6809_reset(&cpu, 0x1234);
	check_u32("the entry point is where it was told", cpu.pc, 0x1234);
	check_u32("the direct page is zero", cpu.dp, 0x00);
	check("both interrupts come up masked, as on the real part",
	      (cpu.cc & CPU6809_FLAG_I) != 0 && (cpu.cc & CPU6809_FLAG_F) != 0);
	check_u32("and the cycle count starts again", (uint32_t) cpu.cycles, 0);
}

int
main(void)
{
	printf("Motorola 6809 core\n");

	test_sum_loop();
	test_big_endian();
	test_postbyte_forms();
	test_postbyte_faults();
	test_borrow_convention();
	test_overflow_table();
	test_rmw_column();
	test_grid();
	test_odds_and_ends();
	test_stacks();
	test_branches();
	test_interrupts();
	test_swi_family();
	test_faults();
	test_cycles();
	test_reset();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
