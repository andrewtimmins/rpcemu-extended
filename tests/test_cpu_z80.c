/*
 * The Z80 core that goes in the OPEN Bus co-processor card.
 *
 * Testable on the same terms as the other two cores: a flat byte array, no bus,
 * no emulator, no display.
 *
 * ★ WHAT IS WORTH PINNING DOWN ON A Z80, which is the largest of the three
 * instruction sets and the one with the most places to go wrong:
 *
 *   - the DECODE, because src/copro/cpu_z80.c decodes by the opcode map's own
 *     field structure rather than as 256 cases. That is the right way round, but it
 *     means one mistake in splitting an opcode moves a whole row of the table at
 *     once, so instructions are checked from every quadrant of the map and from
 *     each prefixed page.
 *   - the one rule that is easy to get wrong: under a DD prefix, (HL) becomes
 *     (IX+d) and H and L stay REAL when a displacement is present, but become the
 *     undocumented index halves when it is not. Both halves of that are checked,
 *     including that the undocumented form faults.
 *   - that a repeating instruction is INTERRUPTIBLE. LDIR steps the program
 *     counter back over itself rather than looping inside one instruction, so a
 *     64K block move spread over a card's timeslices cannot stall the host. That
 *     property is the reason it is written that way, so it is asserted directly.
 *   - flags, on the cases where a plausible implementation is wrong: the
 *     half-carry, overflow as distinct from carry, parity, and DAA.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_z80.h"

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

#define RAM_SIZE	0x10000
#define PROG_BASE	0x0100		/* clear of the restart vectors */

static uint8_t ram[RAM_SIZE];
static cpu_z80_state cpu;

static void
load(const uint8_t *prog, unsigned len)
{
	memset(ram, 0, sizeof(ram));
	memcpy(ram + PROG_BASE, prog, len);
	cpu_z80_init(&cpu, ram, RAM_SIZE);
	cpu_z80_reset(&cpu, PROG_BASE);
}

static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 1000; slices++) {
		if (cpu_z80_run(&cpu, 1000) == 0) {
			return 1;
		}
	}
	return 0;
}

static int
flag(uint8_t f)
{
	return (cpu.f & f) != 0;
}

static uint16_t
hl(void)
{
	return (uint16_t) ((cpu.h << 8) | cpu.l);
}

static uint16_t
de(void)
{
	return (uint16_t) ((cpu.d << 8) | cpu.e);
}

static uint16_t
bc(void)
{
	return (uint16_t) ((cpu.b << 8) | cpu.c);
}

/* Opcodes, so the programs below read as assembler. */
#define HALT		0x76
#define NOP		0x00
#define XOR_A		0xaf
#define LD_A_N		0x3e
#define LD_B_N		0x06
#define LD_C_N		0x0e
#define LD_H_N		0x26
#define LD_L_N		0x2e
#define LD_BC_NN	0x01
#define LD_DE_NN	0x11
#define LD_HL_NN	0x21
#define LD_SP_NN	0x31
#define LD_NN_A		0x32
#define LD_A_HL		0x7e
#define LD_HL_A		0x77
#define LD_HL_N		0x36
#define LD_A_B		0x78
#define LD_B_A		0x47
#define ADD_A_B		0x80
#define ADD_A_N		0xc6
#define ADC_A_N		0xce
#define SUB_N		0xd6
#define AND_N		0xe6
#define OR_N		0xf6
#define CP_N		0xfe
#define INC_A		0x3c
#define INC_BC		0x03
#define ADD_HL_DE	0x19
#define DAA		0x27
#define CPL		0x2f
#define SCF		0x37
#define CCF		0x3f
#define EX_AF		0x08
#define EXX		0xd9
#define EX_DE_HL	0xeb
#define DJNZ		0x10
#define JR		0x18
#define JR_NZ		0x20
#define JP_NN		0xc3
#define CALL_NN		0xcd
#define RET		0xc9
#define RST_08		0xcf
#define PUSH_BC		0xc5
#define POP_BC		0xc1
#define PUSH_AF		0xf5
#define POP_AF		0xf1
#define OUT_N_A		0xd3
#define IN_A_N		0xdb
#define PREFIX_CB	0xcb
#define PREFIX_DD	0xdd
#define PREFIX_FD	0xfd
#define PREFIX_ED	0xed

/* ---- a program that does some work ------------------------------------- */

static void
test_djnz_loop(void)
{
	const uint8_t prog[] = {
		XOR_A,			/* a = 0, and clears the flags */
		LD_B_N, 0x0a,		/* b = 10 */
		/* loop: */
		ADD_A_B,		/* a += b */
		DJNZ, 0xfd,		/* back to the ADD */
		LD_NN_A, 0x00, 0x20,	/* the answer at 0x2000 */
		HALT
	};

	printf("A program that loops with DJNZ, stores and stops:\n");

	load(prog, sizeof(prog));
	check("it reaches a stop", run_to_stop());
	check("it halted rather than faulted", cpu.halted && !cpu.faulted);
	check("the stop was a HALT", cpu.halt_reason == CPU_Z80_HALT_HALT);
	check_u32("10 down to 1 summed to 55", cpu.a, 55);
	check_u32("and 55 reached memory", ram[0x2000], 55);
	check_u32("the accumulator is the exit code", cpu.exit_code, 55);
	check("cycles were counted", cpu.cycles == 2 + 10 * 2 + 2);
	check_u32("and b counted down to zero", cpu.b, 0);
}

/* ---- decoding across the map ------------------------------------------- */

static void
test_decode_coverage(void)
{
	printf("\nInstructions from every quadrant of the opcode map:\n");

	{
		/* Quadrant 1 is the whole 8-bit register-to-register matrix, which
		   one mistake in splitting an opcode would move wholesale. Three
		   corners of it, plus both (HL) forms. */
		const uint8_t prog[] = {
			LD_A_N, 0x11,
			LD_B_A,			/* b = a */
			LD_HL_NN, 0x00, 0x30,
			LD_HL_N, 0x22,		/* (0x3000) = 0x22 */
			LD_A_HL,		/* a = (hl) */
			LD_C_N, 0x33,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ld b,a moved the accumulator", cpu.b, 0x11);
		check_u32("ld (hl),n wrote through hl", ram[0x3000], 0x22);
		check_u32("ld a,(hl) read it back", cpu.a, 0x22);
		check_u32("ld c,n loaded an immediate", cpu.c, 0x33);
		check_u32("and hl is where it was put", hl(), 0x3000);
	}

	{
		/* 16-bit pairs, the increments that set no flags, and the exchanges. */
		const uint8_t prog[] = {
			LD_BC_NN, 0xff, 0x00,	/* bc = 0x00ff */
			INC_BC,			/* 0x0100: carries across the bytes */
			LD_DE_NN, 0x34, 0x12,
			LD_HL_NN, 0x78, 0x56,
			EX_DE_HL,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("inc bc carries from c into b", bc(), 0x0100);
		check_u32("ex de,hl swapped them one way", de(), 0x5678);
		check_u32("and the other", hl(), 0x1234);
	}

	{
		/* The alternate register set. */
		const uint8_t prog[] = {
			LD_A_N, 0xaa,
			EX_AF,			/* aa goes away */
			LD_A_N, 0xbb,
			EX_AF,			/* and comes back */
			LD_BC_NN, 0x01, 0x00,
			EXX,
			LD_BC_NN, 0x02, 0x00,
			EXX,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("ex af,af' brought the first accumulator back", cpu.a, 0xaa);
		check_u32("exx brought the first bc back", bc(), 0x0001);
	}

	{
		/* Quadrant 3: calls, returns, restarts and the stack. */
		const uint8_t prog[] = {
			LD_SP_NN, 0x00, 0xff,
			CALL_NN, 0x0a, 0x01,	/* to 0x010a */
			LD_A_N, 0x02,		/* runs after the return */
			HALT,
			NOP,
			/* 0x010a: */
			LD_B_N, 0x09,
			RET
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("call reached the subroutine", cpu.b, 0x09);
		check_u32("and ret came back after it", cpu.a, 0x02);
		check_u32("the stack pointer is balanced again", cpu.sp, 0xff00);
	}

	{
		const uint8_t prog[] = {
			LD_SP_NN, 0x00, 0xff,
			LD_BC_NN, 0xcd, 0xab,
			PUSH_BC,
			LD_BC_NN, 0x00, 0x00,	/* prove the pop really pops */
			POP_BC,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("push then pop round-trips a pair", bc(), 0xabcd);
	}

	{
		/* A restart is a call to a fixed low address, so the program has to
		   live where one can reach it. */
		const uint8_t prog[] = { LD_SP_NN, 0x00, 0xff, RST_08, HALT };

		load(prog, sizeof(prog));
		ram[0x0008] = LD_A_N;
		ram[0x0009] = 0x5a;
		ram[0x000a] = RET;
		(void) run_to_stop();
		check_u32("rst 08 called the vector at 0x0008", cpu.a, 0x5a);
	}

	{
		/* A relative jump, forwards and conditionally. */
		const uint8_t prog[] = {
			LD_A_N, 0x01,
			CP_N, 0x01,		/* sets Z */
			JR_NZ, 0x02,		/* not taken */
			LD_B_N, 0x07,
			JR, 0x02,		/* over the next two bytes */
			LD_B_N, 0x08,		/* skipped */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("a conditional relative jump was correctly not taken",
		          cpu.b, 0x07);
	}
}

/* ---- flags -------------------------------------------------------------- */

static void
test_flags(void)
{
	printf("\nFlags, on the cases a plausible implementation gets wrong:\n");

	{
		/* 0x7f + 1 is the classic signed overflow: the result is negative
		   although both operands were positive. Carry is clear, which is
		   what makes overflow a different flag rather than a synonym. */
		const uint8_t prog[] = { LD_A_N, 0x7f, ADD_A_N, 0x01, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("0x7f + 1 sets sign, overflow and half carry, not carry",
		      cpu.a == 0x80 && flag(CPU_Z80_FLAG_S) &&
		      flag(CPU_Z80_FLAG_PV) && flag(CPU_Z80_FLAG_H) &&
		      !flag(CPU_Z80_FLAG_C) && !flag(CPU_Z80_FLAG_N));
	}

	{
		const uint8_t prog[] = { LD_A_N, 0xff, ADD_A_N, 0x01, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("0xff + 1 sets zero, carry and half carry, not overflow",
		      cpu.a == 0x00 && flag(CPU_Z80_FLAG_Z) &&
		      flag(CPU_Z80_FLAG_C) && flag(CPU_Z80_FLAG_H) &&
		      !flag(CPU_Z80_FLAG_PV));
	}

	{
		/* ★ A half carry that could ONLY come from bit 3 of the sum.
		   0x7f + 1 and 0xff + 1 both carry out of bit 2 as well, so an
		   implementation reading the wrong bit agrees with them and a test
		   built only from those two cannot tell. 8 + 8 carries out of bit 3
		   and out of nothing lower. */
		const uint8_t prog[] = { LD_A_N, 0x08, ADD_A_N, 0x08, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("8 + 8 sets the half carry, which only bit 3 can explain",
		      cpu.a == 0x10 && flag(CPU_Z80_FLAG_H));
	}

	{
		/* And one that must NOT set it, so the flag is not simply always on. */
		const uint8_t prog[] = { LD_A_N, 0x01, ADD_A_N, 0x01, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("1 + 1 does not", cpu.a == 0x02 && !flag(CPU_Z80_FLAG_H));
	}

	{
		/* Subtraction sets the add/subtract flag, which exists so that DAA
		   knows which way it is correcting. */
		const uint8_t prog[] = { LD_A_N, 0x00, SUB_N, 0x01, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("0x00 - 1 borrows, and sets the add/subtract flag",
		      cpu.a == 0xff && flag(CPU_Z80_FLAG_C) &&
		      flag(CPU_Z80_FLAG_N) && flag(CPU_Z80_FLAG_H) &&
		      flag(CPU_Z80_FLAG_S));
	}

	{
		/* The incoming carry, so that multi-byte arithmetic works. */
		const uint8_t prog[] = { SCF, LD_A_N, 0x01, ADC_A_N, 0x01, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("adc adds the incoming carry", cpu.a, 0x03);
	}

	{
		/* On the logical operations the parity/overflow flag is PARITY, and
		   AND sets the half carry where OR clears it. 0x0f has four bits
		   set, so parity is even. */
		const uint8_t prog[] = { LD_A_N, 0xff, AND_N, 0x0f, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("and sets the half carry and reports even parity",
		      cpu.a == 0x0f && flag(CPU_Z80_FLAG_H) &&
		      flag(CPU_Z80_FLAG_PV) && !flag(CPU_Z80_FLAG_C));

		{
			const uint8_t p2[] = { LD_A_N, 0x00, OR_N, 0x07, HALT };

			load(p2, sizeof(p2));
			(void) run_to_stop();
			check("or clears the half carry and reports odd parity",
			      cpu.a == 0x07 && !flag(CPU_Z80_FLAG_H) &&
			      !flag(CPU_Z80_FLAG_PV));
		}
	}

	{
		/* CP is a subtraction whose result is thrown away. */
		const uint8_t prog[] = { LD_A_N, 0x10, CP_N, 0x20, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("cp borrows without changing the accumulator",
		      cpu.a == 0x10 && flag(CPU_Z80_FLAG_C) &&
		      !flag(CPU_Z80_FLAG_Z));
	}

	{
		/* The 16-bit add's half carry comes out of bit 11, not bit 3, and
		   it leaves sign and zero alone - which is why ADC HL,ss exists
		   separately. */
		const uint8_t prog[] = {
			LD_HL_NN, 0xff, 0x0f,	/* 0x0fff */
			LD_DE_NN, 0x01, 0x00,
			ADD_HL_DE,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("add hl,de carries out of bit 11 into the half carry",
		      hl() == 0x1000 && flag(CPU_Z80_FLAG_H) &&
		      !flag(CPU_Z80_FLAG_C) && !flag(CPU_Z80_FLAG_N));
	}

	{
		/* ★ The case that distinguishes bit 11 from bit 3. 0x0fff + 1
		   carries out of both, so it cannot tell them apart; 0x0f00 + 0x0100
		   carries out of bit 11 and out of nothing in the low nibble. */
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x0f,	/* 0x0f00 */
			LD_DE_NN, 0x00, 0x01,	/* 0x0100 */
			ADD_HL_DE,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("with no low nibble involved, so only bit 11 explains it",
		      hl() == 0x1000 && flag(CPU_Z80_FLAG_H));
	}

	{
		/* ED 5A is ADC HL,DE, which unlike ADD HL,DE sets every flag.
		   The XOR A is not decoration: a Z80 comes up with F all ones, so
		   the carry is SET at reset and an ADC written without clearing it
		   first adds one more than the test means. That cost a wrong
		   expectation here before it was noticed. */
		const uint8_t prog[] = {
			XOR_A,			/* clears the carry the reset set */
			LD_HL_NN, 0xff, 0xff,
			LD_DE_NN, 0x01, 0x00,
			PREFIX_ED, 0x5a,	/* adc hl,de */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("adc hl,de sets zero and carry on a 16-bit wrap",
		      hl() == 0x0000 && flag(CPU_Z80_FLAG_Z) &&
		      flag(CPU_Z80_FLAG_C));
	}

	{
		/* ED 44 is NEG, which is a subtraction from zero. */
		const uint8_t prog[] = { LD_A_N, 0x01, PREFIX_ED, 0x44, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("neg subtracts from zero",
		      cpu.a == 0xff && flag(CPU_Z80_FLAG_N) &&
		      flag(CPU_Z80_FLAG_C));
	}

	{
		/* CCF complements the carry and puts the OLD carry in the half
		   carry, which is easy to leave out. */
		const uint8_t prog[] = { SCF, CCF, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("ccf clears the carry it found and remembers it in H",
		      !flag(CPU_Z80_FLAG_C) && flag(CPU_Z80_FLAG_H));
	}

	{
		const uint8_t prog[] = { LD_A_N, 0x0f, CPL, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("cpl complements and sets H and N",
		      cpu.a == 0xf0 && flag(CPU_Z80_FLAG_H) &&
		      flag(CPU_Z80_FLAG_N));
	}
}

static void
test_daa(void)
{
	printf("\nDAA, which is the reason the half carry is a flag at all:\n");

	{
		const uint8_t prog[] = {
			LD_A_N, 0x15,
			ADD_A_N, 0x27,		/* 0x3c in binary */
			DAA,			/* 0x42 in decimal */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("15 + 27 in packed BCD is 42", cpu.a, 0x42);
	}

	{
		const uint8_t prog[] = {
			LD_A_N, 0x90,
			ADD_A_N, 0x90,		/* 0x20 with a carry */
			DAA,			/* 0x80 with a carry: 90+90=180 */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("90 + 90 in packed BCD is 80 with a carry",
		      cpu.a == 0x80 && flag(CPU_Z80_FLAG_C));
	}

	{
		const uint8_t prog[] = {
			LD_A_N, 0x42,
			SUB_N, 0x14,		/* 0x2e in binary */
			DAA,			/* 0x28 in decimal */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("42 - 14 in packed BCD is 28", cpu.a, 0x28);
	}
}

/* ---- the CB page -------------------------------------------------------- */

static void
test_cb_page(void)
{
	printf("\nThe CB page: bit operations and rotates:\n");

	{
		const uint8_t prog[] = {
			LD_A_N, 0x80,
			PREFIX_CB, 0x7f,	/* bit 7,a */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("bit 7,a of a set bit clears zero and sets sign",
		      !flag(CPU_Z80_FLAG_Z) && flag(CPU_Z80_FLAG_S) &&
		      flag(CPU_Z80_FLAG_H));
		check_u32("and leaves the accumulator alone", cpu.a, 0x80);
	}

	{
		const uint8_t prog[] = {
			LD_A_N, 0x7f,
			PREFIX_CB, 0x7f,	/* bit 7,a of a clear bit */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("bit of a clear bit sets zero and parity together",
		      flag(CPU_Z80_FLAG_Z) && flag(CPU_Z80_FLAG_PV));
	}

	{
		const uint8_t prog[] = {
			LD_B_N, 0x00,
			PREFIX_CB, 0xc0,	/* set 0,b */
			PREFIX_CB, 0xf8,	/* set 7,b */
			PREFIX_CB, 0x80,	/* res 0,b */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("set and res reach the bit they name", cpu.b, 0x80);
	}

	{
		const uint8_t prog[] = {
			LD_A_N, 0x81,
			PREFIX_CB, 0x07,	/* rlc a: unlike RLCA, sets all flags */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("rlc a rotates the top bit round and into the carry",
		      cpu.a == 0x03 && flag(CPU_Z80_FLAG_C));
	}

	{
		/* A bit operation on memory through (HL). */
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x40,
			LD_HL_N, 0x0f,
			PREFIX_CB, 0xfe,	/* set 7,(hl) */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("set 7,(hl) wrote back through hl", ram[0x4000], 0x8f);
	}
}

/* ---- the index prefixes ------------------------------------------------- */

static void
test_index_prefixes(void)
{
	printf("\nThe DD and FD prefixes, and the rule that is easy to get wrong:\n");

	{
		const uint8_t prog[] = {
			PREFIX_DD, LD_HL_NN, 0x00, 0x40,	/* ld ix,0x4000 */
			PREFIX_DD, LD_A_HL, 0x05,		/* ld a,(ix+5) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x4005] = 0x5a;
		(void) run_to_stop();
		check_u32("ld ix,nn loaded the index register", cpu.ix, 0x4000);
		check_u32("ld a,(ix+5) read at the displacement", cpu.a, 0x5a);
	}

	{
		/* A negative displacement, which a signed byte is there for. */
		const uint8_t prog[] = {
			PREFIX_DD, LD_HL_NN, 0x00, 0x40,
			PREFIX_DD, LD_A_HL, 0xfe,		/* ld a,(ix-2) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x3ffe] = 0xa5;
		(void) run_to_stop();
		check_u32("the displacement is signed", cpu.a, 0xa5);
	}

	{
		/* ★ The rule. LD H,(IX+d) refers to (HL), so the H in it is the
		   REAL H and not the undocumented IXH. Both are checked: this one
		   works, and the form without a displacement faults below. */
		const uint8_t prog[] = {
			PREFIX_DD, LD_HL_NN, 0x00, 0x40,	/* ld ix,0x4000 */
			PREFIX_DD, 0x66, 0x01,			/* ld h,(ix+1) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x4001] = 0x77;
		(void) run_to_stop();
		check("ld h,(ix+d) is not a fault", !cpu.faulted && cpu.halted);
		check_u32("and it loaded the real H", cpu.h, 0x77);
		check_u32("leaving ix alone", cpu.ix, 0x4000);
	}

	{
		/* The other half of the rule: DD 24 would be INC IXH, which is
		   undocumented, so it faults rather than doing something plausible. */
		const uint8_t prog[] = { PREFIX_DD, 0x24, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("inc ixh faults as undocumented",
		      cpu.faulted && cpu.fault_cause == CPU_Z80_FAULT_ILLEGAL);
	}

	{
		/* ★ And a form that only READS an index half, which is a separate
		   path from the one above. INC does both, so the write path's
		   refusal was hiding whether the read path refused at all: a
		   mutation that allowed the read went unnoticed until this was
		   added. DD 7C is LD A,IXH. */
		const uint8_t prog[] = { PREFIX_DD, 0x7c, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("ld a,ixh faults too, so the read path refuses as well",
		      cpu.faulted && cpu.fault_cause == CPU_Z80_FAULT_ILLEGAL);
	}

	{
		/* A read-modify-write through the index register, which must fetch
		   the displacement once and not twice. */
		const uint8_t prog[] = {
			PREFIX_DD, LD_HL_NN, 0x00, 0x40,
			PREFIX_DD, 0x34, 0x03,			/* inc (ix+3) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x4003] = 0x41;
		(void) run_to_stop();
		check_u32("inc (ix+3) incremented the byte", ram[0x4003], 0x42);
		check("and the instruction ended where it should",
		      cpu.halted && !cpu.faulted);
	}

	{
		/* DDCB: the displacement comes BEFORE the opcode on this page,
		   which is the other ordering trap. */
		const uint8_t prog[] = {
			PREFIX_DD, LD_HL_NN, 0x00, 0x40,
			PREFIX_DD, PREFIX_CB, 0x02, 0x7e,	/* bit 7,(ix+2) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x4002] = 0x80;
		(void) run_to_stop();
		check("bit 7,(ix+2) found the bit set",
		      !cpu.faulted && !flag(CPU_Z80_FLAG_Z));
	}

	{
		/* FD is the same page against IY, so one check that it is wired to
		   the other register and not to IX. */
		const uint8_t prog[] = {
			PREFIX_FD, LD_HL_NN, 0x00, 0x50,	/* ld iy,0x5000 */
			PREFIX_FD, LD_A_HL, 0x00,		/* ld a,(iy+0) */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x5000] = 0x3c;
		(void) run_to_stop();
		check_u32("fd addresses iy", cpu.iy, 0x5000);
		check_u32("and reads through it", cpu.a, 0x3c);
		check_u32("leaving ix at zero", cpu.ix, 0x0000);
	}

	{
		/* A run of prefixes is legal and only the last one counts, because
		   each is a complete fetch in its own right. */
		const uint8_t prog[] = {
			PREFIX_DD, PREFIX_DD, PREFIX_FD, LD_HL_NN, 0x11, 0x22,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("in a run of prefixes the last one wins", cpu.iy, 0x2211);
		check_u32("and the others did nothing", cpu.ix, 0x0000);
	}
}

/* ---- the ED page and the block instructions ---------------------------- */

static void
test_block_instructions(void)
{
	printf("\nThe block instructions, and that they are interruptible:\n");

	{
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x40,	/* source */
			LD_DE_NN, 0x00, 0x50,	/* destination */
			LD_BC_NN, 0x05, 0x00,	/* five bytes */
			PREFIX_ED, 0xb0,	/* ldir */
			HALT
		};
		int i;
		int copied = 1;

		load(prog, sizeof(prog));
		for (i = 0; i < 5; i++) {
			ram[0x4000 + i] = (uint8_t) (0xa0 + i);
		}
		(void) run_to_stop();

		for (i = 0; i < 5; i++) {
			if (ram[0x5000 + i] != (uint8_t) (0xa0 + i)) {
				copied = 0;
			}
		}
		check("ldir copied the block", copied);
		check_u32("bc counted down to zero", bc(), 0);
		check_u32("hl ended past the source", hl(), 0x4005);
		check_u32("de ended past the destination", de(), 0x5005);
		check("and it cleared the parity flag when it finished",
		      !flag(CPU_Z80_FLAG_PV));
		check_u32("nothing was written past the block", ram[0x5005], 0);
	}

	{
		/* ★ The property the implementation exists for. Three instructions
		   of setup and then LDIR; a budget of six must therefore get three
		   iterations in and no more, which can only happen if each
		   iteration is a separate instruction. If LDIR looped internally,
		   the whole 0x1000-byte move would happen inside one cycle. */
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x40,
			LD_DE_NN, 0x00, 0x50,
			LD_BC_NN, 0x00, 0x10,	/* 4096 bytes */
			PREFIX_ED, 0xb0,	/* ldir */
			HALT
		};

		load(prog, sizeof(prog));
		check("a budget of six is used exactly", cpu_z80_run(&cpu, 6) == 6);
		check_u32("three iterations happened, so ldir is interruptible",
		          bc(), 0x1000 - 3);
		check("and it has not finished", !cpu.halted);
		check_u32("the program counter is back on the ldir itself",
		          cpu.pc, PROG_BASE + 9);

		check("it finishes when given the time", run_to_stop() && cpu.halted);
		check_u32("having moved the whole block", bc(), 0);
	}

	{
		/* CPIR stops early on a match, which is the whole point of it. */
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x40,
			LD_BC_NN, 0x10, 0x00,	/* up to sixteen bytes */
			LD_A_N, 0x42,		/* what to look for */
			PREFIX_ED, 0xb1,	/* cpir */
			HALT
		};

		load(prog, sizeof(prog));
		ram[0x4002] = 0x42;
		(void) run_to_stop();
		check("cpir stopped on the match", flag(CPU_Z80_FLAG_Z));
		check_u32("with hl just past it", hl(), 0x4003);
		check_u32("and bc showing what was left", bc(), 0x10 - 3);
	}

	{
		/* And reports honestly when there is no match. */
		const uint8_t prog[] = {
			LD_HL_NN, 0x00, 0x40,
			LD_BC_NN, 0x04, 0x00,
			LD_A_N, 0x99,
			PREFIX_ED, 0xb1,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("cpir with no match ends with zero clear and bc exhausted",
		      !flag(CPU_Z80_FLAG_Z) && bc() == 0);
	}
}

/* ---- input and output --------------------------------------------------- */

static void
test_ports(void)
{
	printf("\nThe port space, with no card behind it:\n");

	{
		const uint8_t prog[] = {
			LD_A_N, 0x5a,
			OUT_N_A, 0x07,
			LD_A_N, 0x00,		/* prove the read really reads */
			IN_A_N, 0x07,
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("out then in round-trips through the latch", cpu.a, 0x5a);
		check_u32("and the latch holds it", cpu.ports[0x07], 0x5a);
		check_u32("without disturbing another port", cpu.ports[0x08], 0x00);
	}

	{
		/* ED 41 is OUT (C),B, and ED 40 is IN B,(C): the register-indirect
		   forms, which take the port from C. */
		const uint8_t prog[] = {
			LD_C_N, 0x20,
			LD_B_N, 0x99,
			PREFIX_ED, 0x41,	/* out (c),b */
			LD_B_N, 0x00,
			PREFIX_ED, 0x40,	/* in b,(c) */
			HALT
		};

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check_u32("out (c),b used c as the port", cpu.ports[0x20], 0x99);
		check_u32("and in b,(c) read it back", cpu.b, 0x99);
	}
}

/* ---- boundaries --------------------------------------------------------- */

static void
test_faults_and_reset(void)
{
	printf("\nUndocumented opcodes, faults and reset:\n");

	{
		/* CB 30 is SLL B: it exists on the hardware and Zilog never
		   published it, so it faults here rather than being modelled. */
		const uint8_t prog[] = { PREFIX_CB, 0x30, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("sll faults as undocumented",
		      cpu.faulted && cpu.fault_cause == CPU_Z80_FAULT_ILLEGAL);
		check_u32("naming the opcode", cpu.fault_addr, 0x30);
	}

	{
		/* ED 71 would be OUT (C),0, which differs between NMOS and CMOS
		   parts, so there is no right answer to give. */
		const uint8_t prog[] = { PREFIX_ED, 0x71, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("out (c),0 faults", cpu.faulted);
	}

	{
		/* ED 00 is one of the pages a real part treats as a no-op. */
		const uint8_t prog[] = { PREFIX_ED, 0x00, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("an undefined ED opcode faults rather than doing nothing",
		      cpu.faulted);
	}

	{
		static uint8_t small[256];
		const uint8_t prog[] = { LD_HL_NN, 0x00, 0x40, LD_A_HL, HALT };

		memset(small, 0, sizeof(small));
		memcpy(small, prog, sizeof(prog));
		cpu_z80_init(&cpu, small, sizeof(small));
		cpu_z80_reset(&cpu, 0);
		(void) run_to_stop();
		check("reading outside the core's memory faults",
		      cpu.faulted && cpu.fault_cause == CPU_Z80_FAULT_ACCESS);
		check_u32("naming the address", cpu.fault_addr, 0x4000);
	}

	check("both fault causes have names",
	      cpu_z80_fault_name(CPU_Z80_FAULT_ILLEGAL)[0] != '\0' &&
	      cpu_z80_fault_name(999)[0] != '\0');

	{
		const uint8_t prog[] = { LD_A_N, 0x33, HALT };

		load(prog, sizeof(prog));
		(void) run_to_stop();
		check("the core is halted before the reset", cpu.halted);

		cpu_z80_reset(&cpu, PROG_BASE);
		check("reset clears the halt", !cpu.halted && !cpu.faulted);
		check_u32("af comes up all ones, as on a real part",
		          (uint32_t) ((cpu.a << 8) | cpu.f), 0xffff);
		check_u32("and so does the stack pointer", cpu.sp, 0xffff);
		check_u32("the program counter is at the entry given", cpu.pc, PROG_BASE);
		check("and it runs again", run_to_stop() && cpu.a == 0x33);
	}
}

static void
test_budget(void)
{
	const uint8_t prog[] = {
		LD_B_N, 0x64,		/* 100 */
		DJNZ, 0xfe,		/* back to itself */
		HALT
	};

	printf("\nRunning on a budget:\n");

	load(prog, sizeof(prog));
	check("a run uses exactly the budget it was given",
	      cpu_z80_run(&cpu, 5) == 5);
	check_u32("four iterations of DJNZ happened", cpu.b, 0x64 - 4);
	check("and it finishes when given the time", run_to_stop() && cpu.halted);
	check_u32("with b counted down to zero", cpu.b, 0);
	check("a halted core uses no cycles", cpu_z80_run(&cpu, 100) == 0);
}

int
main(void)
{
	printf("Zilog Z80 core\n");

	test_djnz_loop();
	test_decode_coverage();
	test_flags();
	test_daa();
	test_cb_page();
	test_index_prefixes();
	test_block_instructions();
	test_ports();
	test_faults_and_reset();
	test_budget();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
