/*
  RPCEmu - randomised differential fuzzer for the ARM dynamic recompiler.

  The other jit_* tests check a fixed, hand-picked matrix of cases. This one
  instead generates large numbers of RANDOM instructions (across every class
  the recompiler handles: data processing, multiply, single data transfer, and
  block transfer) with random register and memory inputs, runs each one through
  both the interpreter opcode handler (the reference) and a freshly recompiled
  native block, and asserts the resulting registers, flags and memory match.

  It is architecture-generic (validates whichever native backend is built) and
  is meant to be run under qemu-aarch64 to shake out arm64 codegen bugs that the
  targeted tests miss. A real RISC OS boot executes billions of instructions
  with value combinations no hand-written test covers, so a divergence here is a
  candidate for the "recompiler boots but the screen is blank" reports (#30).

  It cannot find real-silicon-only faults (I-cache/D-cache coherence, memory
  ordering): qemu does not model those. A clean run is therefore evidence the
  remaining #30 fault is such an effect rather than a codegen logic error.

  Deterministic: the RNG seed (argv[1], default 1) and iteration count
  (argv[2], default 2000000) are printed so any failure is reproducible.

  Built only when RPCEMU_JIT_TEST is defined (see tests/CMakeLists.txt).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"

extern void *codegen_test_compile(uint32_t opcode);
extern void codegen_test_interp(uint32_t opcode);

#define COND_AL 0xeu

/* One guest page mapped straight at a host buffer, for the memory ops. */
#define GBASE   0x00020000u
static uint8_t membuf[4096] __attribute__((aligned(4096)));
static uint8_t membuf_seed[4096];

static long checks = 0;
static int failures = 0;

/* ---- RNG (xorshift64*) --------------------------------------------------- */

static uint64_t rng_state = 1;

static uint32_t
rng32(void)
{
	uint64_t x = rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return (uint32_t) ((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/* Random 32-bit value, biased towards awkward boundary values. */
static const uint32_t boundary[] = {
	0x00000000u, 0x00000001u, 0x00000002u, 0x0000000fu, 0x00000010u,
	0x0000007fu, 0x00000080u, 0x000000ffu, 0x00007fffu, 0x00008000u,
	0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x40000000u,
	0x55555555u, 0xaaaaaaaau, 0x00010000u, 0x000000ffu,
};
#define NBOUND ((int) (sizeof(boundary) / sizeof(boundary[0])))

static uint32_t
rand_val(void)
{
	if ((rng32() & 3) != 0) {
		return boundary[rng32() % NBOUND];
	}
	return rng32();
}

/* Register 0..12 (never r13/r14/r15: r15 is the PC and would make the
   interpreter-vs-block comparison meaningless, and keeping clear of r13/r14
   avoids incidental stack/link surprises). */
static int
rand_reg(void)
{
	return (int) (rng32() % 13);
}

/* ---- state ---------------------------------------------------------------- */

static uint32_t regs_in[16];
static uint32_t cpsr_in;

static void
apply_state(void)
{
	int i;

	for (i = 0; i < 15; i++) {
		arm.reg[i] = regs_in[i];
	}
	arm.reg[cpsr] = cpsr_in;
	arm.event = 0;
	memcpy(membuf, membuf_seed, sizeof(membuf));
	/* host = guest + disp; bits 0-1 clear => readable and writable fast path */
	{
		intptr_t disp = (intptr_t) membuf - (intptr_t) GBASE;
		vraddrl[GBASE >> 12] = (uintptr_t) disp;
		vwaddrl[GBASE >> 12] = (uintptr_t) disp;
	}
}

/* ---- opcode generators ---------------------------------------------------- */

/* Returns an opcode; sets *membase to a register that must point into the
   mapped page (or -1). */
static uint32_t
gen_data_processing(void)
{
	const int op = (int) (rng32() & 0xf);
	const int rn = rand_reg();
	const int rd = rand_reg();
	/* TST/TEQ/CMP/CMN (op 8-11) require S=1; with S=0 those encodings are the
	   miscellaneous instruction space (MRS/MSR/BX/multiply/halfword), not data
	   processing, so keep S set for them. */
	const int s  = (op >= 8 && op <= 11) ? 1 : (int) (rng32() & 1);
	uint32_t opc = (COND_AL << 28) | ((uint32_t) op << 21) | ((uint32_t) s << 20)
	             | ((uint32_t) rn << 16) | ((uint32_t) rd << 12);

	if (rng32() & 1) {
		/* Immediate operand: rotate + imm8. */
		const uint32_t rot = rng32() & 0xf;
		const uint32_t imm = rng32() & 0xff;
		opc |= (1u << 25) | (rot << 8) | imm;
	} else {
		const int rm = rand_reg();
		const uint32_t stype = rng32() & 3;
		if (rng32() & 1) {
			/* Immediate shift amount. */
			const uint32_t amt = rng32() & 0x1f;
			opc |= (amt << 7) | (stype << 5) | (uint32_t) rm;
		} else {
			/* Register shift amount (bit 4 set, bit 7 clear -> not a multiply). */
			const int rs = rand_reg();
			opc |= ((uint32_t) rs << 8) | (stype << 5) | (1u << 4) | (uint32_t) rm;
		}
	}
	return opc;
}

static uint32_t
gen_multiply(void)
{
	const int s = (int) (rng32() & 1);

	if (rng32() & 1) {
		/* MUL / MLA. */
		const int rd = rand_reg();   /* bits 16-19 */
		const int rn = rand_reg();   /* accumulator, bits 12-15 */
		const int rs = rand_reg();   /* bits 8-11 */
		const int rm = rand_reg();   /* bits 0-3 */
		const uint32_t a = rng32() & 1; /* MLA if set */
		return (COND_AL << 28) | (a << 21) | ((uint32_t) s << 20)
		     | ((uint32_t) rd << 16) | ((uint32_t) rn << 12)
		     | ((uint32_t) rs << 8) | (9u << 4) | (uint32_t) rm;
	} else {
		/* UMULL / UMLAL / SMULL / SMLAL. */
		const int rdhi = rand_reg();
		int rdlo = rand_reg();
		const int rs = rand_reg();
		const int rm = rand_reg();
		const uint32_t u = rng32() & 1; /* signed if clear */
		const uint32_t a = rng32() & 1; /* accumulate */
		if (rdlo == rdhi) rdlo = (rdhi + 1) % 13; /* RdHi != RdLo (else unpredictable) */
		return (COND_AL << 28) | (1u << 23) | (u << 22) | (a << 21) | ((uint32_t) s << 20)
		     | ((uint32_t) rdhi << 16) | ((uint32_t) rdlo << 12)
		     | ((uint32_t) rs << 8) | (9u << 4) | (uint32_t) rm;
	}
}

static uint32_t
gen_single_transfer(int *membase)
{
	const int load = (int) (rng32() & 1);
	const int byte = (int) (rng32() & 1);
	const int pre  = (int) (rng32() & 1);
	const int up   = (int) (rng32() & 1);
	const int wb   = (int) (rng32() & 1);
	int rn = rand_reg();
	int rd = rand_reg();
	uint32_t opc;

	/* Keep the base out of Rd so a load's writeback vs loaded-value ordering is
	   unambiguous when comparing. */
	if (rd == rn) rn = (rn + 1) % 13;

	opc = (COND_AL << 28) | (1u << 26) | ((uint32_t) pre << 24) | ((uint32_t) up << 23)
	    | ((uint32_t) byte << 22) | ((uint32_t) wb << 21) | ((uint32_t) load << 20)
	    | ((uint32_t) rn << 16) | ((uint32_t) rd << 12);

	if (rng32() & 1) {
		/* Immediate offset, kept small so pre-indexing stays in the page. */
		opc |= (rng32() % 0x80);
	} else {
		/* Register offset Rm (also kept small via its input value below). */
		int rm = rand_reg();
		if (rm == rn) rm = (rm + 1) % 13;
		opc |= (1u << 25) | (uint32_t) rm;
		/* The register offset value is set to something small in the caller. */
		regs_in[rm] = rng32() % 0x80;
	}

	/* Base points into the middle of the page. */
	regs_in[rn] = GBASE + 0x800u + (rng32() % 0x40u) * 4u;
	*membase = rn;
	return opc;
}

static uint32_t
gen_block_transfer(int *membase)
{
	const int load = (int) (rng32() & 1);
	const int pre  = (int) (rng32() & 1);
	const int up   = (int) (rng32() & 1);
	const int wb   = (int) (rng32() & 1);
	const int rn   = rand_reg();
	uint32_t list = rng32() & 0xff; /* r0-r7 only: at most 32 bytes, fits the page */
	uint32_t opc;

	if (list == 0) list = 1;
	list &= ~(1u << rn); /* base not in the list (writeback would be unpredictable) */
	if (list == 0) list = (rn == 0) ? 2u : 1u;

	opc = (COND_AL << 28) | (1u << 27) | ((uint32_t) pre << 24) | ((uint32_t) up << 23)
	    | ((uint32_t) wb << 21) | ((uint32_t) load << 20) | ((uint32_t) rn << 16) | list;

	/* Base mid-page so up to 8 registers either side stay mapped. */
	regs_in[rn] = GBASE + 0x800u;
	*membase = rn;
	return opc;
}

/* Evaluate an ARM condition code against a flag word (NZCV in bits 31-28). */
static int
cond_true(int cond, uint32_t flags)
{
	const int N = (int) ((flags >> 31) & 1);
	const int Z = (int) ((flags >> 30) & 1);
	const int C = (int) ((flags >> 29) & 1);
	const int V = (int) ((flags >> 28) & 1);

	switch (cond) {
	case 0:  return Z;                 /* EQ */
	case 1:  return !Z;                /* NE */
	case 2:  return C;                 /* CS */
	case 3:  return !C;                /* CC */
	case 4:  return N;                 /* MI */
	case 5:  return !N;                /* PL */
	case 6:  return V;                 /* VS */
	case 7:  return !V;                /* VC */
	case 8:  return C && !Z;           /* HI */
	case 9:  return !C || Z;           /* LS */
	case 10: return N == V;            /* GE */
	case 11: return N != V;            /* LT */
	case 12: return !Z && (N == V);    /* GT */
	case 13: return Z || (N != V);     /* LE */
	default: return 1;                 /* AL */
	}
}

/* ---- one fuzz iteration --------------------------------------------------- */

static void
one(void)
{
	uint32_t opcode;
	int membase = -1;
	int i;
	int cond_exec;
	uint32_t iref[15], iref_cpsr, iref_event;
	uint8_t imem[4096];
	void (*fn)(void);

	/* Random inputs for r0..r12 (r13/r14 kept fixed and out of the way). */
	for (i = 0; i < 13; i++) regs_in[i] = rand_val();
	regs_in[13] = 0x00001000u;
	regs_in[14] = 0x00002000u;
	cpsr_in = (0x10u | 0x03u) /* 32-bit SVC */ | (rng32() & 0xf0000000u);
	for (i = 0; i < 4096; i++) membuf_seed[i] = (uint8_t) (rng32() & 0xff);

	switch (rng32() % 4) {
	case 0:  opcode = gen_data_processing();      break;
	case 1:  opcode = gen_multiply();             break;
	case 2:  opcode = gen_single_transfer(&membase); break;
	default: opcode = gen_block_transfer(&membase);  break;
	}
	(void) membase;

	/* Exercise conditional execution: give each instruction a random condition
	   (0..14; skip NV) so the recompiler's flag test is covered. The recompiled
	   block conditionalises itself (codegen_test_compile adds the flag test for
	   cond != AL); gate the interpreter reference by the same condition. */
	{
		const int cond = (int) (rng32() % 15);
		opcode = (opcode & 0x0fffffffu) | ((uint32_t) cond << 28);
		cond_exec = cond_true(cond, cpsr_in);
	}

	/* --- interpreter reference --- */
	apply_state();
	if (cond_exec) {
		codegen_test_interp(opcode);
	}
	for (i = 0; i < 15; i++) iref[i] = arm.reg[i];
	iref_cpsr = arm.reg[cpsr];
	iref_event = arm.event;
	memcpy(imem, membuf, sizeof(imem));

	/* --- freshly recompiled native block --- */
	apply_state();
	linecyc = 0; /* return after the single instruction */
	fn = (void (*)(void)) codegen_test_compile(opcode);
	fn();

	checks++;

	/* Compare everything except r15 (the block advances the cached PC, the
	   interpreter handler does not). */
	{
		int bad = 0;
		for (i = 0; i < 15; i++) {
			if (arm.reg[i] != iref[i]) bad = 1;
		}
		if (arm.reg[cpsr] != iref_cpsr) bad = 1;
		if (arm.event != iref_event) bad = 1;
		if (memcmp(membuf, imem, sizeof(imem)) != 0) bad = 1;

		if (bad) {
			if (failures < 30) {
				fprintf(stderr, "MISMATCH op=%08x cpsr_in=%08x\n", opcode, cpsr_in);
				for (i = 0; i < 15; i++) {
					if (arm.reg[i] != iref[i]) {
						fprintf(stderr, "  r%-2d in=%08x  interp=%08x  jit=%08x\n",
						        i, regs_in[i], iref[i], arm.reg[i]);
					}
				}
				if (arm.reg[cpsr] != iref_cpsr) {
					fprintf(stderr, "  cpsr    interp=%08x  jit=%08x\n", iref_cpsr, arm.reg[cpsr]);
				}
				if (arm.event != iref_event) {
					fprintf(stderr, "  event   interp=%08x  jit=%08x\n", iref_event, arm.event);
				}
				if (memcmp(membuf, imem, sizeof(imem)) != 0) {
					fprintf(stderr, "  memory differs\n");
				}
			}
			failures++;
		}
	}
}

int
main(int argc, char **argv)
{
	long iterations = 2000000;
	long n;

	rng_state = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1;
	if (rng_state == 0) rng_state = 1;
	if (argc > 2) iterations = strtol(argv[2], NULL, 0);

	printf("jit fuzzer: seed=%llu iterations=%ld\n",
	       (unsigned long long) rng_state, iterations);

	arm_init();
	arm_reset(CPUModel_SA110);
	initcodeblocks();
	updatemode(0x10 | SUPERVISOR); /* 32-bit SVC, as RISC OS 5 runs */

	for (n = 0; n < iterations; n++) {
		one();
	}

	printf("%ld checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
