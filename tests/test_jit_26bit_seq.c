/*
  RPCEmu - multi-instruction (sequence) differential test in 26-bit configuration.

  test_jit_26bit compares one instruction at a time and found nothing over eight
  million checks, which rules out opcode semantics but cannot reach what only
  appears when a program runs: branches, the PC across block boundaries, block
  linking (one recompiled block chained straight into the next), and above all a
  change of processor mode part way through a run. test_jit_seqfuzz does reach all
  of that, but like every other jit test it runs 32-bit SVC only.

  This is the 26-bit counterpart. It fills a code region with random programs and
  runs each one through arm_exec() twice, once with dcache = 0 (interpret every
  block) and once with dcache = 1 (recompile and link), then compares the whole
  architectural state.

  Two things it does that the 32-bit sequence fuzzer does not, both aimed at the
  A7000/RPC610 abort storm:

    - The programs contain TSTP/TEQP/CMPP/CMNP - the 26-bit PSR transfer forms,
      Rd == 15 with S set. In privileged mode those write all of N, Z, C, V, I, F
      and the two mode bits straight into R15 and call updatemode(), so a program
      changes processor mode mid-run without the PC going anywhere. That is how
      RISC OS 3.71 changes mode, and it is the one thing a single-instruction test
      cannot show interacting with a chained block.

    - It compares arm.mode and every banked register bank, not just the sixteen
      visible registers. A mode recorded differently by the two engines, or R13
      left in the wrong bank, would otherwise hide until the guest returned from
      an exception and used the wrong stack.

  The code lives in VRAM, at 0x02000000, and that is forced rather than chosen. In
  26-bit configuration arm.r15_mask is 0x3fffffc, so the PC cannot reach 64MB, and
  with the MMU off getpccache() (cp15.c) serves only 0x00000000 (ROM, which is not
  writable) and 0x02000000 (VRAM) below that line - RAM starts at 0x10000000.
  test_jit_seqfuzz puts its programs at 0x10000000, which a 26-bit PC cannot
  address at all.

  Deterministic: the RNG seed (argv[1]) and iteration count (argv[2]) are printed,
  and each mismatch reports the RNG state from before its program so that one
  program can be regenerated on its own.

  Built only when RPCEMU_JIT_TEST is defined (see tests/CMakeLists.txt).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "cp15.h"

extern int arm_exec(void);

/* In 26-bit mode updatemode() points the flag word at R15; checked at startup so
   this test cannot quietly pass by running the 32-bit path. */
extern uint32_t *pcpsr;

#define CODE_BASE   0x02000000u   /* VRAM: the only writable, fetchable region a
                                     26-bit PC can reach (see the note above) */
#define CODE_WORDS  1024          /* filled with halts; the program goes at the start */
#define PROG_MIN    4
#define PROG_MAX    40
#define STEPS       128           /* arm_exec calls before giving up */

#define HALT        0xeafffffeu    /* B . */

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

static const uint32_t boundary[] = {
	0x00000000u, 0x00000001u, 0x00000002u, 0x0000000fu, 0x00000010u,
	0x0000007fu, 0x000000ffu, 0x7fffffffu, 0x80000000u, 0xffffffffu,
	0x40000000u, 0x55555555u,
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

static int
rand_reg(void)
{
	return (int) (rng32() % 11);	/* r0-r10: not the PC, not the banked pair */
}

/* ---- the state both engines start from ------------------------------------- */

static uint32_t regs_in[15];	/* r0..r14 */
static uint32_t psr_in;		/* PSR half of R15: NZCV, I, F and the mode */

/* Everything the two runs must agree on afterwards. */
struct outcome {
	uint32_t	reg[16];
	uint32_t	mode;
	uint32_t	user_reg[15];
	uint32_t	fiq_reg[15];
	uint32_t	irq_reg[2];
	uint32_t	super_reg[2];
	uint32_t	abort_reg[2];
	uint32_t	undef_reg[2];
	int		halted;
};

/* ---- program generation --------------------------------------------------- */

/**
 * Write a random program of n instructions at the start of the halt-filled region.
 *
 * Branches are strictly forward, for the reason test_jit_seqfuzz gives: a
 * backward branch builds a data-dependent loop, and arm_exec() retires a
 * different number of guest instructions per call in each engine, so cutting a
 * loop off after a fixed number of calls samples the two engines at different
 * points. That is a harness artefact, not a codegen bug. Re-running a linked
 * block covers nothing new anyway.
 */
static void
gen_program(int n)
{
	int i;

	for (i = 0; i < n; i++) {
		const uint32_t cond = rng32() % 15;	/* 0..14, skip NV */
		const uint32_t roll = rng32() % 8;
		uint32_t op;

		if (roll == 0) {
			/* Branch, occasionally with link. */
			const int span = (n + 16) - (i + 1);
			const int target = i + 1 + (int) (rng32() % (uint32_t) span);
			const int off = target - i - 2;	/* B is PC(+8) relative */
			const uint32_t link = ((rng32() & 7) == 0) ? (1u << 24) : 0u;

			op = (cond << 28) | (5u << 25) | link
			   | ((uint32_t) off & 0x00ffffffu);
		} else if (roll == 1) {
			/* A 26-bit PSR transfer: TSTP/TEQP/CMPP/CMNP, Rd == 15 with S.
			   In a privileged mode this rewrites the flags AND the mode, so
			   the run changes mode here and updatemode() swaps banks. The
			   immediate is deliberately small and unrotated, so the mode bits
			   it lands on are spread across all four 26-bit modes rather than
			   always the same one. */
			const uint32_t dop = 8u + (rng32() % 4u);	/* TST/TEQ/CMP/CMN */
			const int rn = rand_reg();

			op = (cond << 28) | (1u << 25) | (dop << 21) | (1u << 20)
			   | ((uint32_t) rn << 16) | (15u << 12) | (rng32() & 0xffu);
		} else {
			/* Data processing. TST/TEQ/CMP/CMN need S = 1; the rest random.
			   Rd is allowed to be 15 here as well, which without S is a
			   branch-by-write and with S is another PSR transfer. */
			const int dop = (int) (rng32() & 0xf);
			/* Rn may be 15: reading R15 as an operand yields PC + PSR in 26-bit
			   mode and is safe here because Rd is not 15, so nothing is written
			   to the PC. Without this the recompiler's masking of an R15 base was
			   never reached and a mutation removing it went unnoticed. */
			const int rn = ((rng32() % 6) == 0) ? 15 : rand_reg();
			/* Rd is never 15 here. Only TST/TEQ/CMP/CMN (dop 8-11) treat Rd == 15
			   as a PSR-only transfer; for the other twelve ALU operations Rd == 15
			   writes the computed value into the PC as well, restoring the flags
			   from it - a return instruction with, here, a random target. Those
			   left the PC outside the code region and cost a third of the
			   iterations to the non-terminating skip. The PSR transfers are
			   generated deliberately above, where the operation is known to be one
			   of the four that leave the PC alone. */
			const int rd = rand_reg();
			const int s = (dop >= 8 && dop <= 11) ? 1 : (int) (rng32() & 1);

			op = (cond << 28) | ((uint32_t) dop << 21) | ((uint32_t) s << 20)
			   | ((uint32_t) rn << 16) | ((uint32_t) rd << 12);
			if (rng32() & 1) {
				op |= (1u << 25) | ((rng32() & 0xf) << 8) | (rng32() & 0xff);
			} else {
				const int rm = rand_reg();
				const uint32_t st = rng32() & 3;

				if (rng32() & 1) {
					op |= ((rng32() & 0x1f) << 7) | (st << 5)
					    | (uint32_t) rm;
				} else {
					const int rs = rand_reg();

					op |= ((uint32_t) rs << 8) | (st << 5) | (1u << 4)
					    | (uint32_t) rm;
				}
			}
		}
		vram[i] = op;
	}
}

/* ---- running one engine --------------------------------------------------- */

/**
 * Run the program under one engine and collect the whole state afterwards.
 *
 * @param dyn  Non-zero to recompile and link blocks, zero to interpret them all.
 *             isblockvalid() keys off dcache, which is what selects the path.
 * @param warm Non-zero to keep the code cache from the previous call.
 *
 * A block is COMPILED on its first visit and only EXECUTED as native code on a
 * later one - the build path in arm_exec() interprets each instruction as it
 * emits it. These programs are straight-line with forward-only branches, so every
 * PC is visited once and, on a single run, the recompiled code is built and never
 * entered. That made the whole comparison a second interpreter run: it did not
 * notice the compile-time carry being inverted, a mutation the single-instruction
 * test catches hundreds of times over. So the caller runs the recompiling engine
 * twice, once to fill the cache and again with warm != 0, and compares the second.
 */
static void
run_engine(int dyn, struct outcome *out, int warm)
{
	int i, s;
	uint32_t prev_pc;

	dcache = dyn ? 1 : 0;
	if (!warm) {
		resetcodeblocks();
	}

	/* Enter the starting mode, then flatten every bank, so both runs begin from
	   an identical state even though the previous run may have changed mode and
	   left a bank behind it. */
	updatemode(psr_in & 3u);
	memset(arm.user_reg, 0, sizeof(arm.user_reg));
	memset(arm.fiq_reg, 0, sizeof(arm.fiq_reg));
	memset(arm.irq_reg, 0, sizeof(arm.irq_reg));
	memset(arm.super_reg, 0, sizeof(arm.super_reg));
	memset(arm.abort_reg, 0, sizeof(arm.abort_reg));
	memset(arm.undef_reg, 0, sizeof(arm.undef_reg));
	memset(arm.spsr, 0, sizeof(arm.spsr));

	for (i = 0; i < 15; i++) {
		arm.reg[i] = regs_in[i];
	}
	arm.reg[15] = ((CODE_BASE + 8u) & arm.r15_mask) | psr_in;
	arm.event = 0;

	/* Run until the PC stops moving between timeslices (the halt fillers are
	   B . , so a settled PC means the program finished) or the backstop trips. */
	out->halted = 0;
	prev_pc = ~arm.reg[15];
	for (s = 0; s < STEPS && !arm.event; s++) {
		arm_exec();
		if (arm.reg[15] == prev_pc) {
			out->halted = 1;
			break;
		}
		prev_pc = arm.reg[15];
	}

	for (i = 0; i < 16; i++) {
		out->reg[i] = arm.reg[i];
	}
	out->mode = arm.mode;
	memcpy(out->user_reg, arm.user_reg, sizeof(out->user_reg));
	memcpy(out->fiq_reg, arm.fiq_reg, sizeof(out->fiq_reg));
	memcpy(out->irq_reg, arm.irq_reg, sizeof(out->irq_reg));
	memcpy(out->super_reg, arm.super_reg, sizeof(out->super_reg));
	memcpy(out->abort_reg, arm.abort_reg, sizeof(out->abort_reg));
	memcpy(out->undef_reg, arm.undef_reg, sizeof(out->undef_reg));
}

/* ---- comparison ----------------------------------------------------------- */

static int
report(const struct outcome *ir, const struct outcome *jr, long iter, int plen,
       uint64_t prog_seed)
{
	int i, bad = 0;

	for (i = 0; i < 16; i++) {
		if (ir->reg[i] != jr->reg[i]) {
			bad = 1;
		}
	}
	if (ir->mode != jr->mode || ir->halted != jr->halted) {
		bad = 1;
	}
	if (memcmp(ir->user_reg, jr->user_reg, sizeof(ir->user_reg)) != 0
	    || memcmp(ir->fiq_reg, jr->fiq_reg, sizeof(ir->fiq_reg)) != 0
	    || memcmp(ir->irq_reg, jr->irq_reg, sizeof(ir->irq_reg)) != 0
	    || memcmp(ir->super_reg, jr->super_reg, sizeof(ir->super_reg)) != 0
	    || memcmp(ir->abort_reg, jr->abort_reg, sizeof(ir->abort_reg)) != 0
	    || memcmp(ir->undef_reg, jr->undef_reg, sizeof(ir->undef_reg)) != 0) {
		bad = 1;
	}
	if (!bad) {
		return 0;
	}

	fprintf(stderr, "MISMATCH iter=%ld plen=%d psr_in=%08x state-before-prog=0x%016llx\n",
	        iter, plen, psr_in, (unsigned long long) prog_seed);
	for (i = 0; i < plen; i++) {
		fprintf(stderr, "   code[%2d]=%08x\n", i, vram[i]);
	}
	for (i = 0; i < 16; i++) {
		if (ir->reg[i] != jr->reg[i]) {
			fprintf(stderr, "   r%-2d  interp=%08x  jit=%08x\n",
			        i, ir->reg[i], jr->reg[i]);
		}
	}
	if (ir->mode != jr->mode) {
		fprintf(stderr, "   arm.mode  interp=%02x  jit=%02x\n",
		        ir->mode, jr->mode);
	}
	if (ir->halted != jr->halted) {
		fprintf(stderr, "   halted    interp=%d  jit=%d\n",
		        ir->halted, jr->halted);
	}
	for (i = 0; i < 2; i++) {
		if (ir->irq_reg[i] != jr->irq_reg[i]) {
			fprintf(stderr, "   irq_reg[%d]   interp=%08x  jit=%08x\n",
			        i, ir->irq_reg[i], jr->irq_reg[i]);
		}
		if (ir->super_reg[i] != jr->super_reg[i]) {
			fprintf(stderr, "   super_reg[%d] interp=%08x  jit=%08x\n",
			        i, ir->super_reg[i], jr->super_reg[i]);
		}
		if (ir->abort_reg[i] != jr->abort_reg[i]) {
			fprintf(stderr, "   abort_reg[%d] interp=%08x  jit=%08x\n",
			        i, ir->abort_reg[i], jr->abort_reg[i]);
		}
		if (ir->undef_reg[i] != jr->undef_reg[i]) {
			fprintf(stderr, "   undef_reg[%d] interp=%08x  jit=%08x\n",
			        i, ir->undef_reg[i], jr->undef_reg[i]);
		}
	}
	for (i = 0; i < 15; i++) {
		if (ir->user_reg[i] != jr->user_reg[i]) {
			fprintf(stderr, "   user_reg[%2d] interp=%08x  jit=%08x\n",
			        i, ir->user_reg[i], jr->user_reg[i]);
		}
		if (ir->fiq_reg[i] != jr->fiq_reg[i]) {
			fprintf(stderr, "   fiq_reg[%2d]  interp=%08x  jit=%08x\n",
			        i, ir->fiq_reg[i], jr->fiq_reg[i]);
		}
	}
	return 1;
}

/* The modes to start in. Privileged ones let the PSR transfers change mode;
   User is there because arm_compare_rd15() and arm_write_r15() take a different
   branch when unprivileged, and only NZCV may then be written. */
static const uint32_t start_modes[] = { SUPERVISOR, IRQ, FIQ, USER };
#define NSTART ((int) (sizeof(start_modes) / sizeof(start_modes[0])))

int
main(int argc, char **argv)
{
	long iterations = 50000;
	long n, checks = 0, skipped = 0;
	int failures = 0;

	rng_state = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1;
	if (rng_state == 0) {
		rng_state = 1;
	}
	if (argc > 2) {
		iterations = strtol(argv[2], NULL, 0);
	}

	printf("26-bit sequence test: seed=%llu iterations=%ld\n",
	       (unsigned long long) rng_state, iterations);

	arm_init();
	mem_init();
	machine.model = Model_RPCARM610;
	machine.iomd_type = IOMDType_IOMD;
	mem_reset(16, 2);		/* 16MB RAM, 2MB VRAM - the code goes in VRAM */
	arm_reset(CPUModel_ARM610);
	initcodeblocks();
	updatemode(SUPERVISOR);		/* 26-bit SVC, as these CPUs reset into */

	if (cpsr != 15 || pcpsr != &arm.reg[15] || arm.r15_mask != 0x3fffffcu
	    || arm.mmask != 3 || prog32 != 0) {
		fprintf(stderr,
		        "SETUP FAILED: not in 26-bit configuration "
		        "(cpsr=%u pcpsr=%s r15_mask=%08x mmask=%08x prog32=%d)\n",
		        cpsr, (pcpsr == &arm.reg[15]) ? "R15" : "reg16",
		        arm.r15_mask, arm.mmask, prog32);
		return 1;
	}
	if (vram == NULL || mem_vrammask < (CODE_WORDS * 4u - 1u)) {
		fprintf(stderr, "SETUP FAILED: VRAM too small for the code region\n");
		return 1;
	}

	for (n = 0; n < iterations; n++) {
		const int plen = PROG_MIN + (int) (rng32() % (PROG_MAX - PROG_MIN));
		const uint64_t prog_seed = rng_state;
		struct outcome ir, jr;
		int i;

		for (i = 0; i < CODE_WORDS; i++) {
			vram[i] = HALT;
		}
		gen_program(plen);

		for (i = 0; i < 15; i++) {
			regs_in[i] = rand_val();
		}
		psr_in = start_modes[rng32() % (uint32_t) NSTART]
		       | (rng32() & 0xfc000000u & ~3u);

		run_engine(0, &ir, 0);
		{
			struct outcome warmup;

			run_engine(1, &warmup, 0);	/* compile the blocks */
			run_engine(1, &jr, 1);		/* and now execute them */
		}

		/* Neither engine settled: both were cut off at the backstop, at
		   different points in the program, so their states are not comparable.
		   Counted and reported rather than passed over silently. A halt/no-halt
		   split is a real divergence and is compared. */
		if (!ir.halted && !jr.halted) {
			skipped++;
			continue;
		}

		checks++;
		if (report(&ir, &jr, n, plen, prog_seed)) {
			failures++;
			if (failures >= 20) {
				printf("%ld checks, %d failures, %ld skipped - stopping early\n",
				       checks, failures, skipped);
				return 1;
			}
		}
	}

	printf("%ld checks, %d failures, %ld skipped (non-terminating)\n",
	       checks, failures, skipped);
	return failures ? 1 : 0;
}
