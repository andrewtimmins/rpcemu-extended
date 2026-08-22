/*
 * The 6800 core that goes in the OPEN Bus co-processor card, and with it the
 * 6802 and the 6808.
 *
 * ★ WHAT IS WORTH PINNING DOWN ON A 6800. Almost none of it is the instruction
 * set, which is small and unsurprising. It is the quirks - because a processor
 * that gets these wrong is not the one anybody's software was written against,
 * and every one of them would pass a test that only looked at results:
 *
 *   - CPX sets N, Z and V and leaves the CARRY alone, which is the single most
 *     complained-about thing about this part and one of the reasons the 6809
 *     exists. Checked in both directions, since a core that simply never touched
 *     carry anywhere would pass a one-sided check.
 *   - INX and DEX affect Z and nothing else. A loop counting an index register
 *     down and branching on BPL does not work on a 6800 and does on a 6809.
 *   - TST clears the carry, where the 6809's leaves it.
 *   - The shifts define V as N exclusive-or C after the operation, so LSR sets V
 *     from the bit it shifted out. A 6809 leaves V alone on an LSR entirely.
 *   - The stack pointer addresses the next FREE byte, so a push stores and then
 *     decrements, and TSX and TXS carry the off-by-one. A matched push and pull
 *     agree with each other under either convention, so the check has to look at
 *     the memory and at the pointer, not at the round trip.
 *   - Direct addressing is page zero and nowhere else, and indexed is one form
 *     with an UNSIGNED offset, so it reaches forwards only.
 *   - There is no BRN, and no 16-bit accumulator, so opcodes the 6809 uses for
 *     those must fault here.
 *
 * ★ AND THE OPCODES THE TWO PARTS DISAGREE ABOUT, which is why this is a core of
 * its own rather than a flag on the 6809: &08 is INX here and ASL direct there.
 * That check is the one that would catch somebody later deciding the two could
 * share a decoder after all.
 *
 * Programs live at &0300, leaving page zero free for the direct-addressing tests.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_6800.h"

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

#define RAM_SIZE	0x10000
#define PROG_BASE	0x0300

static uint8_t ram[RAM_SIZE];
static cpu6800_state cpu;

static void
load(const uint8_t *prog, unsigned len)
{
	memset(ram, 0, sizeof(ram));
	memcpy(ram + PROG_BASE, prog, len);
	cpu6800_init(&cpu, ram, RAM_SIZE);
	cpu6800_reset(&cpu, PROG_BASE);
}

static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 1000; slices++) {
		if (cpu6800_run(&cpu, 1000) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
step_n(int n)
{
	int i;

	for (i = 0; i < n; i++) {
		(void) cpu6800_step(&cpu);
	}
}

static int
flag(uint8_t bit)
{
	return (cpu.cc & bit) != 0;
}

/* Opcodes, named so the programs read as assembler. */
#define LDAA_IMM	0x86
#define LDAA_DIR	0x96
#define LDAA_IDX	0xa6
#define LDAA_EXT	0xb6
#define LDAB_IMM	0xc6
#define STAA_DIR	0x97
#define STAA_IDX	0xa7
#define STAA_EXT	0xb7
#define LDX_IMM		0xce
#define STX_DIR		0xdf
#define STX_EXT		0xff
#define LDS_IMM		0x8e
#define STS_DIR		0x9f
#define CPX_IMM		0x8c
#define ADDA_IMM	0x8b
#define ADCA_IMM	0x89
#define SUBA_IMM	0x80
#define SBCA_IMM	0x82
#define CMPA_IMM	0x81
#define ANDA_IMM	0x84
#define ORAA_IMM	0x8a
#define EORA_IMM	0x88
#define NOP		0x01
#define TAP		0x06
#define TPA		0x07
#define INX		0x08
#define DEX		0x09
#define CLV		0x0a
#define SEV		0x0b
#define CLC		0x0c
#define SEC		0x0d
#define CLI		0x0e
#define SEI		0x0f
#define SBA		0x10
#define CBA		0x11
#define TAB		0x16
#define TBA		0x17
#define DAA		0x19
#define ABA		0x1b
#define BRA		0x20
#define BNE		0x26
#define BEQ		0x27
#define BPL		0x2a
#define TSX		0x30
#define INS		0x31
#define PULA		0x32
#define PULB		0x33
#define DES		0x34
#define TXS		0x35
#define PSHA		0x36
#define PSHB		0x37
#define RTS		0x39
#define RTI		0x3b
#define WAI		0x3e
#define SWI		0x3f
#define NEGA		0x40
#define COMA		0x43
#define LSRA		0x44
#define RORA		0x46
#define ASRA		0x47
#define ASLA		0x48
#define ROLA		0x49
#define DECA		0x4a
#define INCA		0x4c
#define TSTA		0x4d
#define CLRA		0x4f
#define NEG_IDX		0x60
#define JMP_IDX		0x6e
#define CLR_IDX		0x6f
#define INC_EXT		0x7c
#define JMP_EXT		0x7e
#define BSR		0x8d
#define JSR_EXT		0xbd

/* ---------------------------------------------------------------- the tests */

static void
test_sum_loop(void)
{
	static const uint8_t prog[] = {
		LDAA_IMM, 0x00,
		LDAB_IMM, 10,
		ADDA_IMM, 3,		/* the loop starts here */
		DECA + 0x10,		/* DECB */
		BNE, 0xfb,
		SWI
	};

	printf("A counted loop, which needs most of the core to be right:\n");
	load(prog, sizeof(prog));
	check("it stops", run_to_stop());
	check("halted rather than faulted", cpu.halted && !cpu.faulted);
	check_u32("ten times three", cpu.a, 30);
	check_u32("the counter ran out", cpu.b, 0);
	check_u32("SWI is what stopped it", cpu.halt_reason, CPU6800_HALT_SWI);
	check_u32("and A is the exit code", cpu.exit_code, 30);
}

/*
 * ★ CPX LEAVES THE CARRY ALONE. Both directions, because a core that never
 * touched carry at all would pass a check that only set it beforehand.
 */
static void
test_cpx_leaves_carry(void)
{
	printf("\nCPX, and the defect the 6809 was built to fix:\n");

	{
		static const uint8_t prog[] = {
			SEC,
			LDX_IMM, 0x10, 0x00,
			CPX_IMM, 0x10, 0x00,	/* equal, so no borrow */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("CPX says they are equal", flag(CPU6800_FLAG_Z));
		check("★ and leaves a carry that was set, set", flag(CPU6800_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			CLC,
			LDX_IMM, 0x00, 0x00,
			CPX_IMM, 0x10, 0x00,	/* would borrow */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("a comparison that would borrow is not equal",
		      !flag(CPU6800_FLAG_Z));
		check("★ and still leaves a clear carry clear",
		      !flag(CPU6800_FLAG_C));
	}

	{
		/* It does set the others, so this is a defect and not an
		   instruction that reports nothing. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x00, 0x00,
			CPX_IMM, 0x00, 0x01,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("but it does set N", flag(CPU6800_FLAG_N));
	}
}

/* ★ INX and DEX affect Z and nothing else. */
static void
test_inx_affects_z_only(void)
{
	printf("\nINX and DEX, which report only whether the answer is zero:\n");

	{
		/* ★ The flags are set AFTER the load, not before it: LDX clears
		   V itself, so setting V first would have it cleared before INX
		   ever ran and the check would pass for the wrong reason. */
		static const uint8_t prog[] = {
			LDX_IMM, 0x7f, 0xff,
			SEC,
			SEV,
			INX,			/* &8000, which is negative */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("INX increments", cpu.x, 0x8000);
		check("★ and does not set N, though the result is negative",
		      !flag(CPU6800_FLAG_N));
		check("nor touches V", flag(CPU6800_FLAG_V));
		check("nor carry", flag(CPU6800_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0xff, 0xff,
			INX,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("it wraps", cpu.x, 0x0000);
		check("and Z is the one flag it does set", flag(CPU6800_FLAG_Z));
	}

	{
		/*
		 * The consequence, which is what actually breaks software: a loop
		 * counting X down and branching on BPL never ends, because N never
		 * changes. Branching on BNE does end. Both are run here so the
		 * difference is the thing being checked and not a bounded loop.
		 */
		static const uint8_t prog[] = {
			LDX_IMM, 0x00, 0x03,
			DEX,			/* &0303 */
			BNE, 0xfd,
			LDAA_IMM, 0x5a,
			SWI
		};

		load(prog, sizeof(prog));
		check("a loop that branches on BNE ends", run_to_stop());
		check_u32("having counted all the way down", cpu.x, 0);
		check_u32("and reached the end", cpu.exit_code, 0x5a);
	}
}

/* ★ TST clears the carry, where the 6809's leaves it alone. */
static void
test_tst_clears_carry(void)
{
	static const uint8_t prog[] = {
		SEC,
		LDAA_IMM, 0x01,
		TSTA,
		SWI
	};

	printf("\nTST, which clears the carry on this part:\n");
	load(prog, sizeof(prog));
	(void) run_to_stop();
	check("TST reports the value", !flag(CPU6800_FLAG_Z) &&
	      !flag(CPU6800_FLAG_N));
	check("★ and clears a carry that was set", !flag(CPU6800_FLAG_C));
}

/*
 * ★ The shifts define V as N exclusive-or C AFTER the operation. LSR is where
 * that is visible, since N is always clear afterwards and so V simply follows
 * the bit shifted out - a 6809 leaves V alone on an LSR.
 */
static void
test_shift_overflow_rule(void)
{
	printf("\nThe shifts, whose V is N exclusive-or C:\n");

	{
		static const uint8_t prog[] = {
			CLC, CLV,
			LDAA_IMM, 0x01,
			LSRA,			/* result 0, carry 1 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("LSR shifts down", cpu.a, 0x00);
		check("carrying out the bottom bit", flag(CPU6800_FLAG_C));
		check("N is clear, as it always is after an LSR",
		      !flag(CPU6800_FLAG_N));
		check("★ so V follows the carry, which a 6809 would not do",
		      flag(CPU6800_FLAG_V));
	}

	{
		static const uint8_t prog[] = {
			CLC, SEV,
			LDAA_IMM, 0x02,
			LSRA,			/* result 1, carry 0 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and shifting an even value", cpu.a, 0x01);
		check("does not carry", !flag(CPU6800_FLAG_C));
		check("so V is cleared, N and C agreeing", !flag(CPU6800_FLAG_V));
	}

	{
		/* On ASL the rule happens to agree with the 6809's, and it is
		   worth showing that it does rather than assuming. */
		static const uint8_t prog[] = {
			LDAA_IMM, 0x40,
			ASLA,			/* &80: sign changed, no carry */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ASL shifts up", cpu.a, 0x80);
		check("and its V still means the sign changed",
		      flag(CPU6800_FLAG_V) && !flag(CPU6800_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			SEC,
			LDAA_IMM, 0x00,
			RORA,			/* the carry rotates in at the top */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ROR brings the carry in at the top", cpu.a, 0x80);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x81,
			ASRA,			/* the sign is kept */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ASR keeps the sign", cpu.a, 0xc0);
		check("and carries the bottom bit out", flag(CPU6800_FLAG_C));
	}
}

/*
 * ★ The stack pointer addresses the next FREE byte. Checked by looking at where
 * the byte landed and where the pointer ended, because a matched push and pull
 * agree with each other under either convention.
 */
static void
test_stack_convention(void)
{
	printf("\nThe stack pointer, which addresses the next free byte:\n");

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x00, 0xff,
			LDAA_IMM, 0x5a,
			PSHA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("★ a push stores AT the pointer", ram[0x00ff], 0x5a);
		check_u32("and then decrements it", cpu.sp, 0x00fe);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x00, 0xff,
			LDAA_IMM, 0x11,
			LDAB_IMM, 0x22,
			PSHA,
			PSHB,
			CLRA,
			LDAB_IMM, 0x00,
			PULB,
			PULA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a push and a pull round trip, A", cpu.a, 0x11);
		check_u32("and B", cpu.b, 0x22);
		check_u32("with the pointer back where it started", cpu.sp, 0x00ff);
	}

	{
		/* ★ TSX and TXS carry the off-by-one on their faces. */
		static const uint8_t prog[] = {
			LDS_IMM, 0x1f, 0xff,
			TSX,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("TSX loads X with the pointer PLUS ONE", cpu.x, 0x2000);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x20, 0x00,
			TXS,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and TXS sets it to X MINUS ONE", cpu.sp, 0x1fff);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x20, 0x00,
			INS,
			DES,
			DES,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("INS and DES move it by one each", cpu.sp, 0x1fff);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x00, 0xff,
			BSR, 0x02,
			SWI,
			NOP,
			LDAA_IMM, 0x3c,		/* &0307 */
			RTS
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("BSR and RTS make a subroutine", cpu.exit_code, 0x3c);
		check_u32("and the stack is balanced", cpu.sp, 0x00ff);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x00, 0xff,
			JSR_EXT, 0x04, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0400] = LDAA_IMM;
		ram[0x0401] = 0x2d;
		ram[0x0402] = RTS;
		(void) run_to_stop();
		check_u32("JSR reaches an extended address", cpu.exit_code, 0x2d);
	}
}

/*
 * ★ Direct addressing is page zero, and indexed is an unsigned offset from X.
 * Both are what the 6809 was designed to get away from, and both would be
 * invisible in a test that only used small addresses and small offsets.
 */
static void
test_addressing(void)
{
	printf("\nThe two addressing modes with no choices in them:\n");

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x5a,
			STAA_DIR, 0x40,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("★ a direct address is in page zero", ram[0x0040], 0x5a);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x10, 0x00,
			LDAA_IDX, 0xff,		/* &10ff, not &0fff */
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x10ff] = 0x77;
		ram[0x0fff] = 0x88;
		(void) run_to_stop();
		check_u32("★ an indexed offset is unsigned, so it reaches forwards",
		          cpu.a, 0x77);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x20, 0x00,
			LDAA_IMM, 0x3c,
			STAA_IDX, 0x10,
			STAA_EXT, 0x30, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("an indexed store", ram[0x2010], 0x3c);
		check_u32("and an extended one", ram[0x3000], 0x3c);
	}

	{
		/* Sixteen bits in memory, high byte first, as on the 6809. */
		static const uint8_t prog[] = {
			LDX_IMM, 0xbe, 0xef,
			STX_EXT, 0x40, 0x00,
			STX_DIR, 0x50,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a sixteen-bit store puts the high byte lower",
		          (uint32_t) ((ram[0x4000] << 8) | ram[0x4001]), 0xbeef);
		check_u32("and a direct one lands in page zero",
		          (uint32_t) ((ram[0x0050] << 8) | ram[0x0051]), 0xbeef);
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x04, 0x00,
			JMP_IDX, 0x00,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x0400] = LDAA_IMM;
		ram[0x0401] = 0x6b;
		ram[0x0402] = SWI;
		(void) run_to_stop();
		check_u32("JMP indexed goes there", cpu.exit_code, 0x6b);
	}

	{
		static const uint8_t prog[] = { JMP_EXT, 0x04, 0x00, SWI };

		load(prog, sizeof(prog));
		ram[0x0400] = LDAA_IMM;
		ram[0x0401] = 0x4c;
		ram[0x0402] = SWI;
		(void) run_to_stop();
		check_u32("and so does JMP extended", cpu.exit_code, 0x4c);
	}
}

/*
 * ★ WHAT THIS PART DOES NOT HAVE, which is the check that would catch somebody
 * later deciding a 6800 could be a flag on the 6809 after all. The two agree
 * about mnemonics and disagree about opcodes.
 */
static void
test_not_a_6809(void)
{
	printf("\nWhere this part and the 6809 disagree about an opcode:\n");

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x00, 0x05,
			0x08,			/* INX here; ASL direct on a 6809 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("★ &08 is INX, not a shift of a direct address",
		          cpu.x, 6);
		check("and it did not fault", !cpu.faulted);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x20, 0x00,
			0x30,			/* TSX here; LEAX on a 6809 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("&30 is TSX, and takes no postbyte", cpu.x, 0x2001);
	}

	{
		static const uint8_t prog[] = {
			0x8e, 0x20, 0x00,	/* LDS here; LDX on a 6809 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("&8e loads the STACK POINTER, not X", cpu.sp, 0x2000);
		check_u32("and X is untouched", cpu.x, 0x0000);
	}

	{
		static const uint8_t prog[] = {
			0xce, 0x20, 0x00,	/* LDX here; LDU on a 6809 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("&ce loads X, there being no second stack to load",
		          cpu.x, 0x2000);
	}

	{
		/* There is no 16-bit accumulator, so the 6809's SUBD faults. */
		static const uint8_t prog[] = { 0x83, 0x00, 0x00, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("there is no sixteen-bit accumulator, so &83 faults",
		      cpu.faulted && cpu.fault_cause == CPU6800_FAULT_ILLEGAL);
	}

	{
		/* ★ And no BRN, which was a 6809 addition. */
		static const uint8_t prog[] = { 0x21, 0x00, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("★ and no BRN: &21 is not an instruction",
		      cpu.faulted && cpu.fault_cause == CPU6800_FAULT_ILLEGAL &&
		      cpu.fault_addr == 0x21);
	}

	{
		/* Nor a prefix byte: there are no second or third pages. */
		static const uint8_t prog[] = { 0x10, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("and &10 is SBA rather than a prefix", !cpu.faulted);
	}
}

/* The condition codes, whose top two bits are not there and read as ones. */
static void
test_condition_codes(void)
{
	printf("\nThe condition codes, and the two bits that are always set:\n");

	{
		static const uint8_t prog[] = { TPA, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("★ TPA reads the two unused bits as ones",
		          (uint32_t) (cpu.a & 0xc0u), 0xc0);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x00,
			TAP,			/* try to clear everything */
			TPA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and TAP cannot clear them", cpu.a, 0xc0);
		/* ★ The register ITSELF, not what TPA reports. TPA forces the two
		   bits on the way out, so a core that let TAP clear them would
		   still read &c0 back and this check is the only one that can
		   tell - which a mutation proved by surviving the one above. */
		check_u32("and the register really holds them",
		          (uint32_t) (cpu.cc & 0xc0u), 0xc0);
	}

	{
		static const uint8_t prog[] = {
			SEC, SEV, SEI,
			CLC, CLV, CLI,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("the set and clear instructions do both",
		      !flag(CPU6800_FLAG_C) && !flag(CPU6800_FLAG_V) &&
		      !flag(CPU6800_FLAG_I));
	}
}

/* The two-accumulator instructions, which no other core here has. */
static void
test_two_accumulators(void)
{
	printf("\nThe instructions that work on both accumulators at once:\n");

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x10,
			LDAB_IMM, 0x03,
			ABA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ABA adds B into A", cpu.a, 0x13);
		check_u32("and leaves B alone", cpu.b, 0x03);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x10,
			LDAB_IMM, 0x03,
			SBA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("SBA subtracts it", cpu.a, 0x0d);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x42,
			LDAB_IMM, 0x42,
			CBA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("CBA keeps neither", cpu.a, 0x42);
		check("but says they were equal", flag(CPU6800_FLAG_Z));
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x7f,
			TAB,
			LDAA_IMM, 0x00,
			TBA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("TAB and TBA move a byte between them", cpu.a, 0x7f);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x08,
			ADDA_IMM, 0x09,		/* &11, and a half carry */
			DAA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("DAA fixes up from the half carry, the digits looking legal",
		          cpu.a, 0x17);
	}
}

/* Carry as a borrow, and the eight-case overflow table. */
static void
test_arithmetic(void)
{
	static const struct {
		uint8_t a, m, result;
		int v, c;
	} cases[] = {
		{ 0x50, 0x10, 0x60, 0, 0 },
		{ 0x50, 0x50, 0xa0, 1, 0 },
		{ 0x50, 0x90, 0xe0, 0, 0 },
		{ 0x50, 0xd0, 0x20, 0, 1 },
		{ 0xd0, 0x10, 0xe0, 0, 0 },
		{ 0xd0, 0x50, 0x20, 0, 1 },
		{ 0xd0, 0x90, 0x60, 1, 1 },
		{ 0xd0, 0xd0, 0xa0, 0, 1 },
	};
	unsigned i;

	printf("\nArithmetic: carry is a borrow, and all eight overflow cases:\n");

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x10,
			SUBA_IMM, 0x20,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a subtraction that wraps", cpu.a, 0xf0);
		check("sets carry, meaning it borrowed", flag(CPU6800_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x00,
			SUBA_IMM, 0x01,		/* borrows */
			LDAA_IMM, 0x03,
			SBCA_IMM, 0x00,		/* takes the borrow */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("and SBC takes it back in", cpu.a, 0x02);
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const uint8_t prog[] = {
			LDAA_IMM, cases[i].a,
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
		      flag(CPU6800_FLAG_V) == cases[i].v &&
		      flag(CPU6800_FLAG_C) == cases[i].c);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0xf0,
			ANDA_IMM, 0x3c,		/* &30 */
			ORAA_IMM, 0x03,		/* &33 */
			EORA_IMM, 0x11,		/* &22 */
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("AND, OR and EOR are not each other", cpu.a, 0x22);
	}

	{
		static const uint8_t prog[] = {
			LDAA_IMM, 0x0f,
			CLC,
			COMA,
			SWI
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("COM complements", cpu.a, 0xf0);
		check("and always sets carry", flag(CPU6800_FLAG_C));
	}

	{
		static const uint8_t prog[] = {
			LDX_IMM, 0x20, 0x00,
			NEG_IDX, 0x00,
			INC_EXT, 0x30, 0x00,
			CLR_IDX, 0x01,
			SWI
		};

		load(prog, sizeof(prog));
		ram[0x2000] = 0x01;
		ram[0x3000] = 0x41;
		ram[0x2001] = 0xff;
		(void) run_to_stop();
		check_u32("NEG on an indexed address", ram[0x2000], 0xff);
		check_u32("INC on an extended one", ram[0x3000], 0x42);
		check_u32("and CLR clears", ram[0x2001], 0x00);
	}
}

/*
 * The interrupts. One frame size, seven bytes, and no fast interrupt: the 6809's
 * shorter frame was an addition and there is nothing here to choose between.
 */
static void
test_interrupts(void)
{
	printf("\nThe interrupts, which push one frame of seven bytes:\n");

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x20, 0x00,
			CLI,
			NOP,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6800_VEC_IRQ] = 0x04;
		ram[CPU6800_VEC_IRQ + 1] = 0x00;
		step_n(2);
		check("an unmasked interrupt is taken", cpu6800_irq(&cpu) != 0);
		check_u32("it vectors through &fff8", cpu.pc, 0x0400);
		check_u32("and pushes seven bytes", 0x2000 - cpu.sp, 7);
		check("with the mask set, so it cannot recurse",
		      flag(CPU6800_FLAG_I));
	}

	{
		static const uint8_t prog[] = { LDS_IMM, 0x20, 0x00, SWI };

		load(prog, sizeof(prog));
		step_n(1);
		check("a masked one is refused, reset having masked it",
		      cpu6800_irq(&cpu) == 0);
	}

	{
		static const uint8_t prog[] = { LDS_IMM, 0x20, 0x00, SWI };

		load(prog, sizeof(prog));
		ram[CPU6800_VEC_NMI] = 0x06;
		ram[CPU6800_VEC_NMI + 1] = 0x00;
		step_n(1);
		cpu6800_nmi(&cpu);
		check_u32("the non-maskable one is taken even masked", cpu.pc,
		          0x0600);
		check_u32("and pushes the same seven", 0x2000 - cpu.sp, 7);
	}

	{
		static const uint8_t prog[] = {
			LDS_IMM, 0x20, 0x00,
			CLI,
			LDAA_IMM, 0x01,
			LDAB_IMM, 0x02,		/* interrupted before this */
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6800_VEC_IRQ] = 0x04;
		ram[CPU6800_VEC_IRQ + 1] = 0x00;
		ram[0x0400] = LDAA_IMM;		/* the handler clobbers A */
		ram[0x0401] = 0xff;
		ram[0x0402] = RTI;
		step_n(3);
		(void) cpu6800_irq(&cpu);
		(void) run_to_stop();
		check_u32("RTI restores what the handler clobbered", cpu.a, 0x01);
		check_u32("and the interrupted program carries on", cpu.b, 0x02);
		check_u32("with the stack balanced", cpu.sp, 0x2000);
	}

	{
		/* The index register is in the frame too, which the 6502's is not
		   and which a handler is therefore free to use. */
		static const uint8_t prog[] = {
			LDS_IMM, 0x20, 0x00,
			CLI,
			LDX_IMM, 0x12, 0x34,
			LDAB_IMM, 0x02,
			SWI
		};

		load(prog, sizeof(prog));
		ram[CPU6800_VEC_IRQ] = 0x04;
		ram[CPU6800_VEC_IRQ + 1] = 0x00;
		ram[0x0400] = LDX_IMM;
		ram[0x0401] = 0xff;
		ram[0x0402] = 0xff;
		ram[0x0403] = RTI;
		step_n(3);
		(void) cpu6800_irq(&cpu);
		(void) run_to_stop();
		check_u32("the index register is restored as well", cpu.x, 0x1234);
	}
}

/* The boundaries. */
static void
test_faults(void)
{
	printf("\nWhat faults, and what it says when it does:\n");

	{
		static const uint8_t prog[] = { 0x00, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("an opcode that does not exist faults",
		      cpu.faulted && cpu.fault_cause == CPU6800_FAULT_ILLEGAL);
		check_u32("and says which one", cpu.fault_addr, 0x00);
	}

	{
		static const uint8_t prog[] = { WAI, SWI };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("★ WAI faults rather than pretending to wait",
		      cpu.faulted && cpu.fault_cause == CPU6800_FAULT_WAIT);
	}

	{
		static uint8_t small[0x400];
		static const uint8_t prog[] = { LDAA_EXT, 0x80, 0x00, SWI };

		memset(small, 0, sizeof(small));
		memcpy(small + 0x300, prog, sizeof(prog));
		cpu6800_init(&cpu, small, sizeof(small));
		cpu6800_reset(&cpu, 0x300);
		(void) run_to_stop();
		check("an access outside the array faults",
		      cpu.faulted && cpu.fault_cause == CPU6800_FAULT_ACCESS);
		check_u32("naming the address", cpu.fault_addr, 0x8000);
	}

	check("a fault cause always has a name",
	      cpu6800_fault_name(CPU6800_FAULT_WAIT) != NULL &&
	      cpu6800_fault_name(999) != NULL);
}

/*
 * Cycle counts. ★ Including the one that looks like a mistake and is not: an
 * indexed instruction costs MORE than the extended form of the same thing on
 * this processor, the offset having to be added, where on a 6809 it is the other
 * way round.
 */
static void
test_cycles(void)
{
	printf("\nCycle counts:\n");

	{
		static const uint8_t prog[] = { NOP, SWI };

		load(prog, sizeof(prog));
		check_u32("NOP is two", (uint32_t) cpu6800_step(&cpu), 2);
	}

	{
		static const uint8_t prog[] = { LDAA_IMM, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("an immediate load is two",
		          (uint32_t) cpu6800_step(&cpu), 2);
	}

	{
		static const uint8_t prog[] = { LDAA_DIR, 0x10, SWI };

		load(prog, sizeof(prog));
		check_u32("a direct load is three",
		          (uint32_t) cpu6800_step(&cpu), 3);
	}

	{
		static const uint8_t prog[] = { LDAA_EXT, 0x30, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("an extended load is four",
		          (uint32_t) cpu6800_step(&cpu), 4);
	}

	{
		static const uint8_t prog[] = { LDAA_IDX, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("★ and an indexed load is FIVE, more than the extended one",
		          (uint32_t) cpu6800_step(&cpu), 5);
	}

	{
		static const uint8_t prog[] = { INX, SWI };

		load(prog, sizeof(prog));
		check_u32("INX is four", (uint32_t) cpu6800_step(&cpu), 4);
	}

	{
		/* ★ The read-modify-write row has the same inversion and its own
		   numbers, so it needs its own checks: nothing above would notice
		   the whole indexed row being timed as the extended one. */
		static const uint8_t prog[] = { NEG_IDX, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("a read-modify-write, indexed, is seven",
		          (uint32_t) cpu6800_step(&cpu), 7);
	}

	{
		static const uint8_t prog[] = { INC_EXT, 0x30, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("and extended is six, one FEWER",
		          (uint32_t) cpu6800_step(&cpu), 6);
	}

	{
		static const uint8_t prog[] = { JMP_IDX, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("JMP indexed is four", (uint32_t) cpu6800_step(&cpu), 4);
	}

	{
		static const uint8_t prog[] = { JMP_EXT, 0x30, 0x00, SWI };

		load(prog, sizeof(prog));
		check_u32("and JMP extended is three",
		          (uint32_t) cpu6800_step(&cpu), 3);
	}

	{
		static const uint8_t prog[] = { SWI };

		load(prog, sizeof(prog));
		check_u32("SWI is twelve", (uint32_t) cpu6800_step(&cpu), 12);
	}

	{
		/* Branches cost the same either way on this part, so neither
		   polarity needs an adjustment. */
		static const uint8_t prog[] = { SEC, 0x25, 0x00, SWI };

		load(prog, sizeof(prog));
		(void) cpu6800_step(&cpu);
		check_u32("a branch taken is four",
		          (uint32_t) cpu6800_step(&cpu), 4);
	}

	{
		static const uint8_t prog[] = { CLC, 0x25, 0x00, SWI };

		load(prog, sizeof(prog));
		(void) cpu6800_step(&cpu);
		check_u32("and not taken is also four",
		          (uint32_t) cpu6800_step(&cpu), 4);
	}

	{
		static const uint8_t prog[] = {
			NOP, NOP, NOP,
			LDAA_IMM, 0x5e,
			SWI
		};

		load(prog, sizeof(prog));
		check_u32("a run stops at its budget and overshoots by at most one",
		          (uint32_t) cpu6800_run(&cpu, 5), 6);
		(void) run_to_stop();
		check_u32("and what is left runs to the end", cpu.exit_code, 0x5e);
		check_u32("with every cycle accounted for",
		          (uint32_t) cpu.cycles, 3 * 2 + 2 + 12);
	}
}

static void
test_reset(void)
{
	static const uint8_t prog[] = { SWI };

	printf("\nReset:\n");
	load(prog, sizeof(prog));
	cpu.a = 0xff;
	cpu.cc = 0;
	cpu6800_reset(&cpu, 0x1234);
	check_u32("the entry point is where it was told", cpu.pc, 0x1234);
	check("the interrupt comes up masked", flag(CPU6800_FLAG_I));
	check_u32("the two unused bits are set", (uint32_t) (cpu.cc & 0xc0u), 0xc0);
	check_u32("and the cycle count starts again", (uint32_t) cpu.cycles, 0);
}

int
main(void)
{
	printf("Motorola 6800, 6802 and 6808 core\n");

	test_sum_loop();
	test_cpx_leaves_carry();
	test_inx_affects_z_only();
	test_tst_clears_carry();
	test_shift_overflow_rule();
	test_stack_convention();
	test_addressing();
	test_not_a_6809();
	test_condition_codes();
	test_two_accumulators();
	test_arithmetic();
	test_interrupts();
	test_faults();
	test_cycles();
	test_reset();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
