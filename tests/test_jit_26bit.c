/*
  RPCEmu - differential test for the recompiler in 26-bit program configuration.

  Every other jit_* test, including the fuzzer, runs one configuration:

      updatemode(0x10 | SUPERVISOR);   / * 32-bit SVC, as RISC OS 5 runs * /

  and test_jit_fuzz says of its register choice "never r13/r14/r15: r15 is the PC
  and would make the interpreter-vs-block comparison meaningless", then compares
  "everything except r15". So R15 has never been an input to a differential test,
  never been a destination, and never been compared.

  That leaves a hole the shape of an entire CPU configuration. In 26-bit mode R15
  is not just the PC: updatemode() sets pcpsr = &arm.reg[15], so N, Z, C, V, I and
  F live in bits 31-26 of R15 and the processor mode in bits 1-0. Conditional
  execution reads them there, every flag-setting instruction writes them there,
  and MOVS PC,R14 restores them from there - which is how RISC OS 3.71 returns
  from an exception and changes mode. The recompiler caches R15 in a host register
  and therefore carries the whole PSR in it, through code paths no test covers.
  That configuration is what an ARM610, ARM710, ARM7500 or ARM7500FE boots into,
  which is to say every machine in the A7000 and pre-StrongARM Risc PC range.

  This test closes that hole. It runs each instruction twice from identical state,
  once through the interpreter opcode handler (the reference) and once through a
  freshly recompiled native block, and compares the registers, the PSR bits of
  R15, arm.event and memory.

  R15 is compared in full, all 32 bits, after undoing the one difference that is
  legitimate: a compiled block advances the cached PC by its own length (exactly
  one instruction, so 4, here) and the bare opcode handler does not. Subtracting
  that 4 is better than masking the PC field off, for two reasons. It still checks
  the PC, so a block that computes the wrong one is caught. And masking gave false
  failures of its own: when an instruction leaves the PC at the top of the 26-bit
  range, the block's +4 carries out of bit 25 and into the PSR bits, so a
  PSR-only comparison reported a mismatch where subtracting 4 cancels exactly.

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

/* The flag word the recompiler is told to use. In 26-bit mode updatemode() points
   it at R15; it is a plain global in arm_dynarec.c with no header of its own, and
   this test checks it to prove the configuration is the one intended. */
extern uint32_t *pcpsr;

/* One guest page mapped straight at a host buffer, for the memory ops. */
#define GBASE   0x00020000u
static uint8_t membuf[4096] __attribute__((aligned(4096)));
static uint8_t membuf_seed[4096];

/* The block is compiled for this PC, so R15 must read back as PC + 8. */
#define BLOCK_PC   0x00010000u

/* The bits of R15 that are the PSR in 26-bit mode: NZCV, I, F and the mode. */
#define R15_PSR    0xfc000003u

/* Flag bits, in their 26-bit R15 positions. */
#define NFLAG 0x80000000u
#define ZFLAG 0x40000000u
#define CFLAG 0x20000000u
#define VFLAG 0x10000000u
#define IFLAG 0x08000000u
#define FFLAG 0x04000000u

static long checks;
static int failures;

/* ---- RNG (xorshift64*), so a failure is reproducible from the seed --------- */

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
	0x0000007fu, 0x00000080u, 0x000000ffu, 0x00007fffu, 0x00008000u,
	0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x40000000u,
	0x55555555u, 0xaaaaaaaau, 0x00010000u, GBASE,
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

/* ---- state ---------------------------------------------------------------- */

static uint32_t regs_in[15];	/* r0..r14 */
static uint32_t psr_in;		/* the PSR half of R15, mode included */

static void
apply_state(void)
{
	int i;

	/* The mode and the banked registers are state too, and an instruction under
	   test may change mode - TSTP/TEQP/CMPP/CMNP and MOVS PC,R14 all can, which
	   is the point of testing them. updatemode() then swaps R13/R14 (and R8-R14
	   for FIQ) with the bank for the new mode, so leaving the banks as the
	   previous run left them would start the two runs from different states and
	   report a mismatch that is the harness's fault rather than the emulator's.
	   Enter the mode first, then flatten every bank, then load the registers. */
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
	/* R15 = PC + 8, with the PSR in the bits the 26-bit CPU keeps it in. */
	arm.reg[15] = ((BLOCK_PC + 8) & arm.r15_mask) | psr_in;
	arm.event = 0;
	memcpy(membuf, membuf_seed, sizeof(membuf));
	{
		intptr_t disp = (intptr_t) membuf - (intptr_t) GBASE;

		/* Both privilege levels; the maps are per-privilege (see cp15.c). */
		for (int m = 0; m < 2; m++) {
			mem_read_map(m)[GBASE >> 12] = (uintptr_t) disp;
			mem_write_map(m)[GBASE >> 12] = (uintptr_t) disp;
		}
	}
}

/* ---- one comparison ------------------------------------------------------- */

/**
 * Run one opcode through both engines from identical state and compare.
 *
 * @param name   What the case is, for a failure message.
 * @param opcode The instruction.
 * @param exec   Non-zero if the condition code is satisfied by psr_in, so the
 *               interpreter reference should run it. The compiled block tests the
 *               condition itself, which is the point of passing this separately.
 */
static void
compare(const char *name, uint32_t opcode, int exec)
{
	uint32_t iref[15], iref_r15, iref_event;
	uint8_t imem[4096];
	void (*fn)(void);
	int i, bad = 0;

	/* --- interpreter reference --- */
	apply_state();
	if (exec) {
		codegen_test_interp(opcode);
	}
	for (i = 0; i < 15; i++) {
		iref[i] = arm.reg[i];
	}
	iref_r15 = arm.reg[15];
	iref_event = arm.event;
	memcpy(imem, membuf, sizeof(imem));

	/* --- freshly recompiled native block --- */
	apply_state();
	linecyc = 0;	/* come back after the one instruction */
	fn = (void (*)(void)) codegen_test_compile(opcode);
	fn();

	checks++;

	for (i = 0; i < 15; i++) {
		if (arm.reg[i] != iref[i]) {
			bad = 1;
		}
	}
	/* The block advanced the cached PC by one instruction; the reference did
	   not. Undo it and the two must agree bit for bit, PSR and PC alike. */
	if ((arm.reg[15] - 4u) != iref_r15) {
		bad = 1;
	}
	if (arm.event != iref_event) {
		bad = 1;
	}
	if (memcmp(membuf, imem, sizeof(imem)) != 0) {
		bad = 1;
	}

	if (bad) {
		failures++;
		if (failures > 25) {
			return;
		}
		fprintf(stderr, "MISMATCH %s: op=%08x psr_in=%08x exec=%d\n",
		        name, opcode, psr_in, exec);
		for (i = 0; i < 15; i++) {
			if (arm.reg[i] != iref[i]) {
				fprintf(stderr, "   r%-2d in=%08x  interp=%08x  jit=%08x\n",
				        i, regs_in[i], iref[i], arm.reg[i]);
			}
		}
		if ((arm.reg[15] - 4u) != iref_r15) {
			fprintf(stderr, "   R15  interp=%08x  jit-4=%08x  (jit raw %08x, PSR %08x vs %08x)\n",
			        iref_r15, arm.reg[15] - 4u, arm.reg[15],
			        iref_r15 & R15_PSR, (arm.reg[15] - 4u) & R15_PSR);
		}
		if (arm.event != iref_event) {
			fprintf(stderr, "   event  interp=%08x  jit=%08x\n",
			        iref_event, arm.event);
		}
		if (memcmp(membuf, imem, sizeof(imem)) != 0) {
			fprintf(stderr, "   memory differs\n");
		}
	}
}

/* ---- the fixed matrix ----------------------------------------------------- */

/* Instructions whose behaviour depends on R15 carrying the PSR. Hand-assembled
   so what is being tested is legible; the comment is the disassembly. */
static const struct {
	const char	*name;
	uint32_t	opcode;
} cases[] = {
	/* R15 read as an operand: in 26-bit mode this yields PC + PSR, and the
	   ROM's own IRQ dispatch does exactly this to save a return address. */
	{ "MOV R14,R15",       0xe1a0e00fu },
	{ "ADD R0,R1,R15",     0xe081000fu },
	{ "ADDS R0,R1,R15",    0xe091000fu },
	{ "SUB R0,R15,#4",     0xe24f0004u },
	{ "ORR R0,R15,R15",    0xe180000fu },

	/* R15 as a destination: a branch (S clear) and the PSR transfer forms
	   (Rd == 15 with S set), which is how 26-bit code changes mode. */
	{ "ADD R15,R15,#4",    0xe28ff004u },
	{ "MOVS PC,R14",       0xe1b0f00eu },
	{ "TEQP R0,#0",        0xe330f000u },
	{ "TSTP R0,#0",        0xe310f000u },
	{ "CMPP R0,#0",        0xe350f000u },
	{ "CMNP R0,#0",        0xe370f000u },

	/* Flag-setting with a rotated immediate: the carry comes from the barrel
	   shifter and is folded in at compile time, into the cached R15 here. */
	{ "TST R0,#0x80000000", 0xe3100102u },
	{ "TEQ R0,#0x80000000", 0xe3300102u },
	{ "ORRS R0,R1,#0x80000000", 0xe3910102u },
	{ "ANDS R0,R1,#0x3fc",  0xe2110effu },
	{ "MOVS R0,#0x80000000", 0xe3b00102u },
	{ "BICS R0,R1,#0xff000000", 0xe3d104ffu },

	/* Arithmetic flags, for contrast: C and V come from the host flags rather
	   than from the shifter. */
	{ "ADDS R0,R1,R2",     0xe0910002u },
	{ "SUBS R0,R1,R2",     0xe0510002u },
	{ "CMP R1,R2",         0xe1510002u },
	{ "RSCS R0,R1,R2",     0xe0f10002u },

	/* Block and single transfers with R15 in play: STM stores R15 + r15_diff,
	   which is 4 on an ARMv3 and 0 on a StrongARM. */
	{ "STMIA R13!,{R0,R14,R15}", 0xe8adc001u },
	{ "STMDB R13!,{R0-R3,R15}",  0xe92d800fu },
	{ "LDMIA R13,{R0,R15}",      0xe89d8001u },
	{ "STR R0,[R13,#4]",         0xe58d0004u },
	{ "LDR R0,[R13,#4]",         0xe59d0004u },
	{ "STR R15,[R13,#8]",        0xe58df008u },
};
#define NCASES ((int) (sizeof(cases) / sizeof(cases[0])))

/* PSR values to run the matrix under: every flag combination that matters, and
   both a privileged and an unprivileged mode, because arm_write_r15() and
   arm_compare_rd15() branch on ARM_MODE_PRIV. */
static const uint32_t psrs[] = {
	SUPERVISOR,
	SUPERVISOR | NFLAG,
	SUPERVISOR | ZFLAG,
	SUPERVISOR | CFLAG,
	SUPERVISOR | VFLAG,
	SUPERVISOR | NFLAG | ZFLAG | CFLAG | VFLAG,
	SUPERVISOR | IFLAG | FFLAG,
	SUPERVISOR | NFLAG | CFLAG | IFLAG,
	USER,
	USER | NFLAG | ZFLAG | CFLAG | VFLAG,
	IRQ,
	IRQ | ZFLAG | CFLAG,
	FIQ | NFLAG,
};
#define NPSRS ((int) (sizeof(psrs) / sizeof(psrs[0])))

/* Does the condition code hold for this PSR? The compiled block decides this
   itself; the reference has to be told. */
static int
cond_true(int cond, uint32_t psr)
{
	const int n = (psr & NFLAG) != 0;
	const int z = (psr & ZFLAG) != 0;
	const int c = (psr & CFLAG) != 0;
	const int v = (psr & VFLAG) != 0;

	switch (cond) {
	case 0x0: return z;
	case 0x1: return !z;
	case 0x2: return c;
	case 0x3: return !c;
	case 0x4: return n;
	case 0x5: return !n;
	case 0x6: return v;
	case 0x7: return !v;
	case 0x8: return c && !z;
	case 0x9: return !c || z;
	case 0xa: return n == v;
	case 0xb: return n != v;
	case 0xc: return !z && (n == v);
	case 0xd: return z || (n != v);
	default:  return 1;	/* AL */
	}
}

int
main(int argc, char **argv)
{
	long sweep = 200000;
	int c, p;
	long n;

	rng_state = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1;
	if (rng_state == 0) {
		rng_state = 1;
	}
	if (argc > 2) {
		sweep = strtol(argv[2], NULL, 0);
	}

	arm_init();
	arm_reset(CPUModel_ARM610);
	initcodeblocks();

	/* 26-bit SVC, which is what these CPUs come out of reset into. */
	updatemode(SUPERVISOR);

	/* Assert the configuration actually took. Without this the test could pass
	   by running the 32-bit path, which is precisely how the existing jit tests
	   missed all of this - so it is checked rather than assumed. */
	if (cpsr != 15 || pcpsr != &arm.reg[15] || arm.r15_mask != 0x3fffffcu
	    || arm.mmask != 3 || prog32 != 0) {
		fprintf(stderr,
		        "SETUP FAILED: not in 26-bit configuration "
		        "(cpsr=%u pcpsr=%s r15_mask=%08x mmask=%08x prog32=%d)\n",
		        cpsr, (pcpsr == &arm.reg[15]) ? "R15" : "reg16",
		        arm.r15_mask, arm.mmask, prog32);
		return 1;
	}
	printf("26-bit differential test: seed=%llu sweep=%ld\n",
	       (unsigned long long) rng_state, sweep);

	/* --- the fixed matrix, under every PSR --- */
	for (p = 0; p < NPSRS; p++) {
		psr_in = psrs[p];
		for (c = 0; c < NCASES; c++) {
			int i;

			for (i = 0; i < 15; i++) {
				regs_in[i] = rand_val();
			}
			/* R13 and R14 point into the mapped page so the transfers
			   land somewhere real. Room either side for STMDB/LDMIA. */
			regs_in[13] = GBASE + 0x800;
			regs_in[14] = GBASE + 0x900;
			for (i = 0; i < 4096; i++) {
				membuf_seed[i] = (uint8_t) (rng32() & 0xff);
			}
			compare(cases[c].name, cases[c].opcode, 1);
		}
	}

	/* --- the same matrix, conditionally executed --- */
	for (p = 0; p < NPSRS; p++) {
		psr_in = psrs[p];
		for (c = 0; c < NCASES; c++) {
			int cond;

			for (cond = 0; cond < 15; cond++) {
				uint32_t op = (cases[c].opcode & 0x0fffffffu)
				            | ((uint32_t) cond << 28);
				int i;

				for (i = 0; i < 15; i++) {
					regs_in[i] = rand_val();
				}
				regs_in[13] = GBASE + 0x800;
				regs_in[14] = GBASE + 0x900;
				for (i = 0; i < 4096; i++) {
					membuf_seed[i] = (uint8_t) (rng32() & 0xff);
				}
				compare(cases[c].name, op,
				        cond_true(cond, psr_in));
			}
		}
	}

	/* --- randomised sweep: data processing with R15 anywhere --- */
	for (n = 0; n < sweep; n++) {
		const int op = (int) (rng32() & 0xf);
		const int rn = (int) (rng32() % 16);
		const int rd = (int) (rng32() % 16);
		const int s  = (op >= 8 && op <= 11) ? 1 : (int) (rng32() & 1);
		const int cond = (int) (rng32() % 15);
		uint32_t opcode = ((uint32_t) cond << 28)
		                | ((uint32_t) op << 21) | ((uint32_t) s << 20)
		                | ((uint32_t) rn << 16) | ((uint32_t) rd << 12);
		int i;

		if (rng32() & 1) {
			/* Immediate, with a rotate half the time. */
			opcode |= 1u << 25;
			opcode |= rng32() & 0xffu;
			if (rng32() & 1) {
				opcode |= (rng32() & 0xfu) << 8;
			}
		} else {
			/* Register, with a constant shift (register shifts are
			   not recompiled, so they add nothing here). */
			opcode |= (uint32_t) (rng32() % 16);
			opcode |= (rng32() & 0x1fu) << 7;
			opcode |= (rng32() & 3u) << 5;
		}

		psr_in = psrs[rng32() % NPSRS];
		for (i = 0; i < 15; i++) {
			regs_in[i] = rand_val();
		}
		regs_in[13] = GBASE + 0x800;
		for (i = 0; i < 4096; i++) {
			membuf_seed[i] = (uint8_t) (rng32() & 0xff);
		}
		compare("sweep", opcode, cond_true(cond, psr_in));
	}

	printf("%ld checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
