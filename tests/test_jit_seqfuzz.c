/*
  RPCEmu - randomised multi-instruction (sequence) differential fuzzer.

  The single-instruction fuzzer (test_jit_fuzz) recompiles one instruction at a
  time, so it cannot exercise the parts of the recompiler that only appear when
  running a real program: branches, the program counter across block
  boundaries, and above all block *linking* (chaining one recompiled block
  directly to the next). This fuzzer fills a code region with random programs
  (data processing + branches, ending in self-branch halts) and runs each one
  through the full execution loop, arm_exec(), under both the interpreter
  (dcache = 0) and the recompiler (dcache = 1), then asserts the final register
  and flag state matches.

  The data-processing mix spans all sixteen ALU opcodes, so it includes the
  forms the recompiler declines and hands to the C interpreter (ADC/SBC/RSC,
  which read carry in, and flag-setting logicals carrying a register shift).
  Those appear interleaved with fully compiled instructions, so the boundary
  between compiled code and a C-helper call - where host-register and flag
  state must survive the call - is exercised in context, not just in isolation.

  It is architecture-generic and meant to be run under qemu-aarch64 to shake out
  arm64 codegen/linking bugs that a real RISC OS boot would hit (issue #30).
  Deterministic: RNG seed (argv[1]) and iteration count (argv[2]) are printed.

  Built only when RPCEMU_JIT_TEST is defined.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "cp15.h"

extern int arm_exec(void);

#define CODE_BASE   0x10000000u   /* ram00 maps here */
#define CODE_WORDS  1024          /* region filled with halts; program at the start */
#define PROG_MIN    4
#define PROG_MAX    40
#define STEPS       128           /* arm_exec calls before we give up */

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
	return (int) (rng32() % 11); /* r0-r10, never the PC or the odd banked regs */
}

static uint32_t regs_in[16];
static uint32_t cpsr_in;

/* Write a random program of `n` instructions at the start of the (already
   halt-filled) code region. Data-processing and branches only; branches target
   within the program or the halt fillers, so control flow stays in bounds. */
static void
gen_program(int n)
{
	int i;

	for (i = 0; i < n; i++) {
		const uint32_t cond = rng32() % 15; /* 0..14; skip NV */
		uint32_t op;

		if ((rng32() % 4) == 0) {
			/* Branch (B / occasional BL). Targets are strictly forward, into
			   the rest of the program or the halt fillers, so control flow
			   always makes progress and every program terminates. Backward
			   branches would build data-dependent loops, and arm_exec() retires
			   a different number of guest instructions per call in the
			   interpreter than in the recompiler, so cutting a loop off after a
			   fixed number of arm_exec() calls samples the two engines at
			   different points - a harness artefact, not a codegen bug. A
			   re-run backward loop is just an already-linked block executed
			   again, so nothing in the codegen is left uncovered. */
			const int span = (n + 16) - (i + 1);
			const int target = i + 1 + (int) (rng32() % (uint32_t) span);
			const int off = target - i - 2; /* B is PC(+8) relative */
			const uint32_t link = ((rng32() & 7) == 0) ? (1u << 24) : 0u;
			op = (cond << 28) | (5u << 25) | link | ((uint32_t) off & 0x00ffffffu);
		} else {
			/* Data processing. TST/TEQ/CMP/CMN need S=1; others random. */
			const int dop = (int) (rng32() & 0xf);
			const int s = (dop >= 8 && dop <= 11) ? 1 : (int) (rng32() & 1);
			const int rn = rand_reg();
			const int rd = rand_reg();

			op = (cond << 28) | ((uint32_t) dop << 21) | ((uint32_t) s << 20)
			   | ((uint32_t) rn << 16) | ((uint32_t) rd << 12);
			if (rng32() & 1) {
				op |= (1u << 25) | ((rng32() & 0xf) << 8) | (rng32() & 0xff);
			} else {
				const int rm = rand_reg();
				const uint32_t st = rng32() & 3;
				if (rng32() & 1) {
					op |= ((rng32() & 0x1f) << 7) | (st << 5) | (uint32_t) rm;
				} else {
					const int rs = rand_reg();
					op |= ((uint32_t) rs << 8) | (st << 5) | (1u << 4) | (uint32_t) rm;
				}
			}
		}
		ram00[i] = op;
	}
}

/* Returns 1 if the program settled on a halt within the STEPS budget, 0 if it
   ran to the backstop (which, with forward-only branches, should not happen). */
static int
run_engine(int dyn, uint32_t *out_regs, uint32_t *out_cpsr)
{
	int i, s;
	int halted = 0;

	dcache = dyn ? 1 : 0; /* isblockvalid() keys off dcache */
	resetcodeblocks();

	for (i = 0; i < 15; i++) {
		arm.reg[i] = regs_in[i];
	}
	arm.reg[15] = CODE_BASE + 8; /* PC pipeline offset, as test_jit_e2e uses */
	arm.reg[cpsr] = cpsr_in;
	arm.event = 0;

	/* Run until the program settles on a self-branch halt (PC stops moving
	   between two consecutive timeslices) or the STEPS backstop trips. The
	   halt fillers are B . , so a fixed PC after a full arm_exec() means we
	   have run to completion; spinning the whole STEPS budget every time is
	   just wasted work. A genuine divergence still shows up because the two
	   engines then settle on different PCs and the final compare catches it. */
	{
		uint32_t prev_pc = ~arm.reg[15];
		for (s = 0; s < STEPS && !arm.event; s++) {
			arm_exec();
			if (arm.reg[15] == prev_pc) {
				halted = 1;
				break;
			}
			prev_pc = arm.reg[15];
		}
	}

	for (i = 0; i < 16; i++) {
		out_regs[i] = arm.reg[i];
	}
	*out_cpsr = arm.reg[cpsr];
	return halted;
}

int
main(int argc, char **argv)
{
	long iterations = 200000;
	long n;
	long checks = 0;
	long skipped = 0;
	int failures = 0;

	rng_state = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1;
	if (rng_state == 0) rng_state = 1;
	if (argc > 2) iterations = strtol(argv[2], NULL, 0);

	printf("seq fuzzer: seed=%llu iterations=%ld\n",
	       (unsigned long long) rng_state, iterations);

	arm_init();
	mem_init();
	machine.model = Model_RPCARM710;
	machine.iomd_type = IOMDType_IOMD;
	mem_reset(16, 2);
	arm_reset(CPUModel_SA110);
	initcodeblocks();
	updatemode(0x10 | SUPERVISOR);

	for (n = 0; n < iterations; n++) {
		const int plen = PROG_MIN + (int) (rng32() % (PROG_MAX - PROG_MIN));
		uint32_t ir[16], ic, jr[16], jc;
		int i, bad = 0;
		const uint64_t prog_seed = rng_state; /* RNG state before this program */

		for (i = 0; i < CODE_WORDS; i++) {
			ram00[i] = 0xeafffffeu; /* B . (halt) filler */
		}
		gen_program(plen);

		for (i = 0; i < 15; i++) {
			regs_in[i] = rand_val();
		}
		regs_in[15] = 0;
		cpsr_in = (0x10u | 0x03u) | (rng32() & 0xf0000000u);

		{
			const int ih = run_engine(0, ir, &ic);
			const int jh = run_engine(1, jr, &jc);

			/* If neither engine reached a halt, both were cut off at the STEPS
			   backstop and their states are not comparable at a common point;
			   skip. With forward-only branches this should be vanishingly rare
			   (reported at the end so it is never a silent gap). A halt/no-halt
			   split IS a real divergence and falls through to the compare. */
			if (!ih && !jh) {
				skipped++;
				continue;
			}
		}

		checks++;

		for (i = 0; i < 16; i++) {
			if (ir[i] != jr[i]) bad = 1;
		}
		if (ic != jc) bad = 1;

		if (bad) {
			if (failures < 20) {
				fprintf(stderr, "MISMATCH iter=%ld plen=%d state-before-prog=0x%016llx\n",
				        n, plen, (unsigned long long) prog_seed);
				for (i = 0; i < plen; i++) {
					fprintf(stderr, "  code[%2d]=%08x\n", i, ram00[i]);
				}
				for (i = 0; i < 16; i++) {
					if (ir[i] != jr[i]) {
						fprintf(stderr, "  r%-2d interp=%08x jit=%08x\n", i, ir[i], jr[i]);
					}
				}
				if (ic != jc) {
					fprintf(stderr, "  cpsr interp=%08x jit=%08x\n", ic, jc);
				}
			}
			failures++;
		}
	}

	printf("%ld checks, %d failures, %ld skipped (non-terminating)\n",
	       checks, failures, skipped);
	return failures ? 1 : 0;
}
