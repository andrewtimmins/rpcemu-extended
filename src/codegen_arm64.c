/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2026 Andy Timmins

  The AArch64 (arm64) dynarec backend is new code by Andy Timmins, based on the
  RPCEmu dynamic recompiler and its x86/amd64 code generators by Sarah Walker.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * AArch64 (arm64) dynarec backend.
 *
 * Emits AArch64 machine code for the recompiler's block cache, natively
 * recompiling the common instruction classes (data-processing, MUL/MLA/UMULL,
 * LDR/STR/LDRB/STRB, LDM/STM and B/BL) and calling the C interpreter helper
 * (arm_opcode_fn) for everything else. See docs/arm64-dynarec.md for the design.
 *
 * Host register assignment (mirrors the amd64 backend's fixed-register scheme):
 *   x19 = &arm (ARMState *)          [callee-saved]   (amd64 r15)
 *   x20 = guest R15 cache            [callee-saved]   (amd64 r12)
 *   x21 = &vwaddrl[0]  (write TLB)   [callee-saved]   (amd64 r14)
 *   x22 = &vraddrl[0]  (read TLB)    [callee-saved]   (amd64 r13)
 *   x23 = memory address, preserved across C-helper calls (amd64 rbx)
 *   x9..x14 = codegen scratch
 *   x0..x2  = C-helper arguments (AAPCS64)
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "arm_common.h"
#include "codegen_arm64.h"
#include "mem.h"

#if defined(__APPLE__) && defined(__aarch64__)
/* Apple Silicon under the hardened runtime rejects RWX on ordinary memory, so
   the code cache is an mmap(MAP_JIT) region rather than a static array, and each
   thread switches between writing it and executing it with
   pthread_jit_write_protect_np(). See initcodeblocks() and the codegen_jit_*
   helpers below. */
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

int lastflagchange;

#ifdef RPCEMU_JIT_TEST
/* Counts gen_native_flags() emissions so the differential tester can confirm
   the inline native path was taken rather than a C-helper fallback. */
unsigned long codegen_test_flag_emits = 0;
#endif

#if defined(__APPLE__) && defined(__aarch64__)
uint8_t (*rcodeblock)[1792];	/* MAP_JIT code cache, allocated in initcodeblocks() */
#else
uint8_t rcodeblock[BLOCKS][1792] __attribute__ ((aligned (4096)));
#endif
uint32_t codeblockpc[0x8000];
int codeblocknum[0x8000];
static const void *codeblockaddr[BLOCKS];	/* base pointer of each block, for linking */
static uint8_t codeblockpresent[0x10000];

static int blocknum;
static int tempinscount;

static int codeblockpos;
static int lastjumppos;

static int blockpoint, blockpoint2;
static uint32_t blocks[BLOCKS];
static int pcinc;
static int lastrecompiled;
static int block_enter;

/* Host registers */
enum {
	A64_X0 = 0, A64_X1 = 1, A64_X2 = 2,
	A64_X9 = 9, A64_X10 = 10, A64_X11 = 11, A64_X12 = 12, A64_X13 = 13, A64_X14 = 14,
	A64_ARM = 19,	/* &arm */
	A64_R15 = 20,	/* guest R15 cache */
	A64_VWP = 21,	/* &vwaddrl[0] */
	A64_VRP = 22,	/* &vraddrl[0] */
	A64_MADDR = 23,	/* memory address, preserved across C-helper calls (like amd64 rbx) */
	A64_X24 = 24,	/* pairs with x23 in the prologue save (keeps SP 16-aligned) */
	A64_FP = 29, A64_LR = 30, A64_SP = 31, A64_ZR = 31,
};

/* AArch64 condition codes (B.cond) */
#define A64_EQ 0x0
#define A64_NE 0x1

/* Guest R15 (PC+flags) lives at arm.reg[15]; its byte offset in ARMState. */
#define R15_OFF ((int) (offsetof(ARMState, reg) + 15 * 4))

/* ---- raw instruction emission -------------------------------------------- */

static inline void
addword(uint32_t w)
{
	memcpy(&rcodeblock[blockpoint2][codeblockpos], &w, sizeof(uint32_t));
	codeblockpos += 4;
}

/* Overwrite the 32-bit instruction word at byte position pos. */
static inline uint32_t
readword(int pos)
{
	uint32_t w;
	memcpy(&w, &rcodeblock[blockpoint2][pos], sizeof(uint32_t));
	return w;
}

static inline void
writeword(int pos, uint32_t w)
{
	memcpy(&rcodeblock[blockpoint2][pos], &w, sizeof(uint32_t));
}

/* ---- A64 encoders (all verified against aarch64-linux-gnu-objdump) -------- */

static inline void a64_movz(int rd, uint32_t imm16, int shift) { addword(0xD2800000u | ((uint32_t) (shift / 16) << 21) | ((imm16 & 0xffff) << 5) | rd); }
static inline void a64_movk(int rd, uint32_t imm16, int shift) { addword(0xF2800000u | ((uint32_t) (shift / 16) << 21) | ((imm16 & 0xffff) << 5) | rd); }

/* Load a full 64-bit immediate (e.g. a helper or table address) into Xd. */
static void
a64_load_imm64(int rd, uint64_t v)
{
	a64_movz(rd, (uint32_t) (v & 0xffff), 0);
	if ((v >> 16) & 0xffff) { a64_movk(rd, (uint32_t) ((v >> 16) & 0xffff), 16); }
	if ((v >> 32) & 0xffff) { a64_movk(rd, (uint32_t) ((v >> 32) & 0xffff), 32); }
	if ((v >> 48) & 0xffff) { a64_movk(rd, (uint32_t) ((v >> 48) & 0xffff), 48); }
}

static inline void a64_mov_w(int rd, int rm) { addword(0x2A0003E0u | (rm << 16) | rd); } /* MOV Wd,Wm */
static inline void a64_ldr_w(int rt, int rn, uint32_t off) { addword(0xB9400000u | (((off / 4) & 0xfff) << 10) | (rn << 5) | rt); }
static inline void a64_str_w(int rt, int rn, uint32_t off) { addword(0xB9000000u | (((off / 4) & 0xfff) << 10) | (rn << 5) | rt); }
static inline void a64_ldrb_reg(int rt, int rn, int rm) { addword(0x38606800u | (rm << 16) | (rn << 5) | rt); } /* LDRB Wt,[Xn,Xm] */
static inline void a64_add_w_imm(int rd, int rn, uint32_t imm) { addword(0x11000000u | ((imm & 0xfff) << 10) | (rn << 5) | rd); }
static inline void a64_add_x_imm(int rd, int rn, uint32_t imm) { addword(0x91000000u | ((imm & 0xfff) << 10) | (rn << 5) | rd); }
static inline void a64_sub_x_imm(int rd, int rn, uint32_t imm) { addword(0xD1000000u | ((imm & 0xfff) << 10) | (rn << 5) | rd); }
static inline void a64_lsr_w_imm(int rd, int rn, int sh) { addword(0x53000000u | ((uint32_t) sh << 16) | (31u << 10) | (rn << 5) | rd); }
static inline void a64_stp_pre(int t1, int t2, int rn, int imm) { addword(0xA9800000u | (((uint32_t) ((imm / 8) & 0x7f)) << 15) | (t2 << 10) | (rn << 5) | t1); }
static inline void a64_ldp_post(int t1, int t2, int rn, int imm) { addword(0xA8C00000u | (((uint32_t) ((imm / 8) & 0x7f)) << 15) | (t2 << 10) | (rn << 5) | t1); }
static inline void a64_blr(int rn) { addword(0xD63F0000u | (rn << 5)); }
static inline void a64_ret(void) { addword(0xD65F03C0u); }

/* Unconditional branch to a byte position within the current block. */
static inline void
a64_b_to(int target_pos)
{
	int rel = target_pos - codeblockpos;
	addword(0x14000000u | (((uint32_t) (rel / 4)) & 0x03ffffff));
}

/* CBZ Wt, <forward> with a placeholder target; returns the byte position of
   the emitted instruction so it can be completed with a64_patch_here(). */
static inline int
a64_cbz_w_forward(int rt)
{
	int pos = codeblockpos;
	addword(0x34000000u | rt);
	return pos;
}

/* CBNZ Wt, <byte position within block> (used for backward branch to epilogue). */
static inline void
a64_cbnz_w_to(int rt, int target_pos)
{
	int rel = target_pos - codeblockpos;
	addword(0x35000000u | ((((uint32_t) (rel / 4)) & 0x7ffff) << 5) | rt);
}

/* Complete a forward CBZ/CBNZ (imm19) emitted at byte position pos so its
   target is the current code position. */
static inline void
a64_patch_here(int pos)
{
	uint32_t w = readword(pos);
	int rel = codeblockpos - pos;
	w = (w & ~(0x7ffffu << 5)) | ((((uint32_t) (rel / 4)) & 0x7ffff) << 5);
	writeword(pos, w);
}

/* Data-processing (shifted register, shift=0) — all verified via objdump. */
static inline void a64_and(int d, int a, int b)  { addword(0x0A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_ands(int d, int a, int b) { addword(0x6A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_orr(int d, int a, int b)  { addword(0x2A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_eor(int d, int a, int b)  { addword(0x4A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_bic(int d, int a, int b)  { addword(0x0A200000u | (b << 16) | (a << 5) | d); }
static inline void a64_bics(int d, int a, int b) { addword(0x6A200000u | (b << 16) | (a << 5) | d); }
static inline void a64_orn(int d, int a, int b)  { addword(0x2A200000u | (b << 16) | (a << 5) | d); }
static inline void a64_add(int d, int a, int b)  { addword(0x0B000000u | (b << 16) | (a << 5) | d); }
static inline void a64_adds(int d, int a, int b) { addword(0x2B000000u | (b << 16) | (a << 5) | d); }
static inline void a64_sub(int d, int a, int b)  { addword(0x4B000000u | (b << 16) | (a << 5) | d); }
static inline void a64_subs(int d, int a, int b) { addword(0x6B000000u | (b << 16) | (a << 5) | d); }
static inline void a64_adc(int d, int a, int b)  { addword(0x1A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_adcs(int d, int a, int b) { addword(0x3A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_sbc(int d, int a, int b)  { addword(0x5A000000u | (b << 16) | (a << 5) | d); }
static inline void a64_sbcs(int d, int a, int b) { addword(0x7A000000u | (b << 16) | (a << 5) | d); }

/* Shifts by immediate (UBFM/SBFM/EXTR aliases). */
static inline void a64_lsl_i(int d, int a, int nn) { addword(0x53000000u | ((uint32_t)((32 - nn) & 31) << 16) | ((uint32_t)(31 - nn) << 10) | (a << 5) | d); }
static inline void a64_asr_i(int d, int a, int nn) { addword(0x13000000u | ((uint32_t)nn << 16) | (31u << 10) | (a << 5) | d); }
static inline void a64_ror_i(int d, int a, int nn) { addword(0x13800000u | (a << 16) | ((uint32_t)nn << 10) | (a << 5) | d); }

/* Condition-flag transfer. MRS Xt,NZCV / MSR NZCV,Xt move NZCV to/from bits
   31:28 of a GPR — AArch64's flags line up bit-for-bit with the ARM PSR. */
static inline void a64_mrs_nzcv(int t) { addword(0xD53B4200u | t); }
static inline void a64_msr_nzcv(int t) { addword(0xD51B4200u | t); }

/* Extra forms for block linking. */
static inline void a64_adds_w_imm(int d, int nn, uint32_t imm) { addword(0x31000000u | ((imm & 0xfff) << 10) | (nn << 5) | d); }
static inline void a64_add_x(int d, int nn, int m) { addword(0x8B000000u | (m << 16) | (nn << 5) | d); } /* ADD Xd,Xn,Xm */
static inline void a64_sub_w_imm(int d, int nn, uint32_t imm) { addword(0x51000000u | ((imm & 0xfff) << 10) | (nn << 5) | d); }
static inline void a64_subs_w_imm(int d, int nn, uint32_t imm) { addword(0x71000000u | ((imm & 0xfff) << 10) | (nn << 5) | d); }
static inline void a64_br(int rn) { addword(0xD61F0000u | (rn << 5)); }
static inline void a64_ldr_w_reg(int t, int nn, int m) { addword(0xB8607800u | (m << 16) | (nn << 5) | t); } /* LDR Wt,[Xn,Xm,LSL#2] */
static inline void a64_ldr_x_imm(int t, int nn, unsigned off8) { addword(0xF9400000u | ((off8 >> 3) << 10) | (nn << 5) | t); } /* LDR Xt,[Xn,#off] */
static inline void a64_ldr_x_reg(int t, int nn, int m) { addword(0xF8607800u | (m << 16) | (nn << 5) | t); } /* LDR Xt,[Xn,Xm,LSL#3] */

/* AArch64 condition codes. */
#define A64_CC_NE 0x1
#define A64_CC_CS 0x2
#define A64_CC_MI 0x4

/* Forward conditional branch with a placeholder target (patched via
   a64_patch_here, which fills the imm19 field). */
static inline int a64_b_cond_forward(int cond) { int p = codeblockpos; addword(0x54000000u | (cond & 0xf)); return p; }

/* Conditional branch to a byte position within the current block. */
static inline void
a64_b_cond_to(int cond, int target_pos)
{
	int rel = target_pos - codeblockpos;
	addword(0x54000000u | ((((uint32_t) (rel / 4)) & 0x7ffff) << 5) | (cond & 0xf));
}

/* Multiply encoders. */
static inline void a64_mul(int d, int a, int m) { addword(0x1B007C00u | (m << 16) | (a << 5) | d); }
static inline void a64_madd(int d, int a, int m, int ra) { addword(0x1B000000u | (m << 16) | (ra << 10) | (a << 5) | d); }
static inline void a64_umull(int d, int a, int m) { addword(0x9BA07C00u | (m << 16) | (a << 5) | d); } /* Xd = Wn*Wm unsigned */
static inline void a64_lsr_x_imm(int d, int a, int sh) { addword(0xD3400000u | ((uint32_t) sh << 16) | (63u << 10) | (a << 5) | d); }

/* Memory-access encoders. */
static inline void a64_rorv(int d, int a, int m) { addword(0x1AC02C00u | (m << 16) | (a << 5) | d); } /* ROR Wd,Wn,Wm */
static inline void a64_ldr_w_regoff(int t, int nn, int m) { addword(0xB8606800u | (m << 16) | (nn << 5) | t); } /* LDR Wt,[Xn,Xm] */
static inline void a64_str_w_regoff(int t, int nn, int m) { addword(0xB8206800u | (m << 16) | (nn << 5) | t); } /* STR Wt,[Xn,Xm] */
static inline void a64_strb_regoff(int t, int nn, int m) { addword(0x38206800u | (m << 16) | (nn << 5) | t); } /* STRB Wt,[Xn,Xm] */

/* Forward branches with a placeholder target, completed by the matching patch. */
static inline int a64_b_forward(void) { int p = codeblockpos; addword(0x14000000u); return p; }
static inline void a64_patch_b_here(int pos) {
	uint32_t w = readword(pos); int rel = codeblockpos - pos;
	writeword(pos, (w & 0xfc000000u) | (((uint32_t) (rel / 4)) & 0x03ffffffu));
}
static inline int a64_cbnz_w_forward(int rt) { int p = codeblockpos; addword(0x35000000u | rt); return p; }
static inline int a64_tbnz_w0_forward(int rt) { int p = codeblockpos; addword(0x37000000u | rt); return p; }
static inline void a64_patch_tbnz_here(int pos) {
	uint32_t w = readword(pos); int rel = codeblockpos - pos;
	writeword(pos, (w & ~(0x3fffu << 5)) | ((((uint32_t) (rel / 4)) & 0x3fff) << 5));
}

/* Call a C helper: load its absolute address into a scratch and BLR. AAPCS64
   passes arguments in x0.., so (unlike the Windows amd64 path) no register
   shuffling or shadow space is required. */
static inline void
gen_call_c_function(const void *addr)
{
	a64_load_imm64(A64_X9, (uintptr_t) addr);
	a64_blr(A64_X9);
}

/*
 * A MAP_JIT region can be written or executed by a thread, but not both at
 * once. Code is generated between initcodeblock() and endblock() and executed
 * outside that window, so a block enables writing on entry and returns the
 * cache to executable once it is complete. On every other platform the cache is
 * a plain W+X array and these are no-ops.
 */
#if defined(__APPLE__) && defined(__aarch64__)
static inline void codegen_jit_writable(void)   { pthread_jit_write_protect_np(0); }
static inline void codegen_jit_executable(void) { pthread_jit_write_protect_np(1); }
#else
static inline void codegen_jit_writable(void)   { }
static inline void codegen_jit_executable(void) { }
#endif

/* ---- block cache management ---------------------------------------------- */

void
initcodeblocks(void)
{
	const size_t rcodeblock_size = (size_t) BLOCKS * sizeof(rcodeblock[0]);
	int c;

	memset(codeblockpc, 0xff, sizeof(codeblockpc));
	memset(blocks, 0xff, sizeof(blocks));

#if defined(__APPLE__) && defined(__aarch64__)
	/* Allocate the code cache as MAP_JIT memory (a static RWX array is refused
	   under the hardened runtime). Allocated once; it lives for the process. */
	if (rcodeblock == NULL) {
		rcodeblock = mmap(NULL, rcodeblock_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		                  MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (rcodeblock == MAP_FAILED) {
			fatal("Unable to allocate MAP_JIT code cache: %s", strerror(errno));
		}
	}
#endif

	for (c = 0; c < BLOCKS; c++) {
		codeblockaddr[c] = &rcodeblock[c][0];
	}
	blockpoint = 0;

#if !(defined(__APPLE__) && defined(__aarch64__))
	/* Make the code buffer executable (NX defeat); the arm64 build additionally
	   needs I-cache maintenance after each block is written - see endblock(). The
	   Apple Silicon MAP_JIT mapping above is already executable. */
	set_memory_executable(rcodeblock, rcodeblock_size);
#endif
}

void
resetcodeblocks(void)
{
	int c;

	blockpoint = 0;

	for (c = 0; c < BLOCKS; c++) {
		if (blocks[c] != 0xffffffff) {
			codeblockpc[blocks[c] & 0x7fff] = 0xffffffff;
			codeblocknum[blocks[c] & 0x7fff] = 0xffffffff;
			blocks[c] = 0xffffffff;
		}
	}
}

void
cacheclearpage(uint32_t a)
{
	int c, d;

	if (!codeblockpresent[a & 0xffff]) {
		return;
	}
	codeblockpresent[a & 0xffff] = 0;
	d = HASH(a << 12);
	for (c = 0; c < 0x400; c++) {
		if ((codeblockpc[c + d] >> 12) == a) {
			codeblockpc[c + d] = 0xffffffff;
		}
	}
}

void
initcodeblock(uint32_t l)
{
	codegen_jit_writable();		/* about to write generated code into the block */

	codeblockpresent[(l >> 12) & 0xffff] = 1;
	tempinscount = 0;
	blockpoint++;
	blockpoint &= (BLOCKS - 1);
	if (blocks[blockpoint] != 0xffffffff) {
		codeblockpc[blocks[blockpoint] & 0x7fff] = 0xffffffff;
		codeblocknum[blocks[blockpoint] & 0x7fff] = 0xffffffff;
	}
	blocknum = HASH(l);
	codeblockpos = 0;
	codeblockpc[blocknum] = l;
	codeblocknum[blocknum] = blockpoint;
	blocks[blockpoint] = blocknum;
	blockpoint2 = blockpoint;

	/* Block epilogue (offset 0): write the R15 cache back, restore the
	   callee-saved registers in reverse push order, and return. Branched to on
	   every block exit. */
	a64_str_w(A64_R15, A64_ARM, R15_OFF);	/* arm.reg[15] = x20 */
	a64_ldp_post(A64_MADDR, A64_X24, A64_SP, 16);
	a64_ldp_post(A64_VWP, A64_VRP, A64_SP, 16);
	a64_ldp_post(A64_ARM, A64_R15, A64_SP, 16);
	a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
	a64_ret();

	/* Block prologue (offset BLOCKSTART): establish the stack frame, preserve
	   the callee-saved registers we use, load the fixed base pointers, and cache
	   guest R15 in x20. Execution enters here (as void (*)(void)) and falls into
	   the body. */
	assert(codeblockpos <= BLOCKSTART);
	codeblockpos = BLOCKSTART;
	a64_stp_pre(A64_FP, A64_LR, A64_SP, -16);
	a64_add_x_imm(A64_FP, A64_SP, 0);		/* mov x29, sp */
	a64_stp_pre(A64_ARM, A64_R15, A64_SP, -16);
	a64_stp_pre(A64_VWP, A64_VRP, A64_SP, -16);
	a64_stp_pre(A64_MADDR, A64_X24, A64_SP, -16);
	a64_load_imm64(A64_ARM, (uintptr_t) &arm);
	/* vwaddrl/vraddrl are pointers to the current privilege level's maps, so
	   load their current value rather than a baked-in array address. Once per
	   block is enough: MSR/MRS are not recompiled and exceptions leave the
	   block, so privilege cannot change part way through one. */
	a64_load_imm64(A64_VWP, (uintptr_t) &vwaddrl);
	a64_ldr_x_imm(A64_VWP, A64_VWP, 0);
	a64_load_imm64(A64_VRP, (uintptr_t) &vraddrl);
	a64_ldr_x_imm(A64_VRP, A64_VRP, 0);
	a64_ldr_w(A64_R15, A64_ARM, R15_OFF);		/* x20 = arm.reg[15] */
	block_enter = codeblockpos;
}

/* Scratch host registers used by the recompiled data-processing path. */
#define W_OP2 A64_X9	/* shifter/immediate operand */
#define W_RN  A64_X10	/* Rn (and reused as flag-word scratch) */
#define W_TMP A64_X11	/* general scratch / mask / MRS target */
#define W_RES A64_X12	/* operation result (preserved across gen_native_flags) */

static void
gen_load_reg(int reg, int wr)
{
	if (reg == 15) {
		a64_mov_w(wr, A64_R15);	/* R15 lives in the x20 cache */
	} else {
		a64_ldr_w(wr, A64_ARM, (uint32_t) (reg * 4));
	}
}

static void
gen_save_reg(int reg, int wr)
{
	if (reg == 15) {
		a64_mov_w(A64_R15, wr);
	} else {
		a64_str_w(wr, A64_ARM, (uint32_t) (reg * 4));
	}
}

/*
 * Materialise the shifter operand into W_OP2. Returns 1 on success, 0 to fall
 * back (register-specified shifts, multiplies and RRX are declined). Like the
 * amd64 backend this computes only the operand value, not the shifter carry;
 * recompile() declines flag-setting logical ops that carry a real shift so the
 * carry is never mis-modelled.
 */
static int
generate_shift(uint32_t opcode)
{
	uint32_t sa;

	if (opcode & 0x10) {
		return 0; /* register-specified shift / multiply / misc */
	}
	if ((opcode & 0xff0) == 0) {
		gen_load_reg(RM, W_OP2); /* LSL #0 */
		return 1;
	}
	sa = (opcode >> 7) & 0x1f;
	switch (opcode & 0x60) {
	case 0x00: /* LSL */
		gen_load_reg(RM, W_OP2);
		if (sa != 0) {
			a64_lsl_i(W_OP2, W_OP2, (int) sa);
		}
		return 1;
	case 0x20: /* LSR (#0 means #32 -> 0) */
		if (sa != 0) {
			gen_load_reg(RM, W_OP2);
			a64_lsr_w_imm(W_OP2, W_OP2, (int) sa);
		} else {
			a64_movz(W_OP2, 0, 0);
		}
		return 1;
	case 0x40: /* ASR (#0 means #32 -> sign) */
		gen_load_reg(RM, W_OP2);
		a64_asr_i(W_OP2, W_OP2, sa ? (int) sa : 31);
		return 1;
	default: /* ROR (#0 is RRX -> decline) */
		if (sa == 0) {
			return 0;
		}
		gen_load_reg(RM, W_OP2);
		a64_ror_i(W_OP2, W_OP2, (int) sa);
		return 1;
	}
}

/*
 * Merge the host NZCV flags (already reflecting the preceding operation) into
 * the ARM flag word. AArch64's NZCV line up bit-for-bit with the ARM PSR, and
 * SUBS sets carry as NOT-borrow exactly like ARM, so no translation is needed.
 *
 * @param mode26  Flag word is the cached R15 (x20); else arm.reg[16] in memory.
 * @param set_cv  Arithmetic op: take N,Z,C,V. Logical op: take only N,Z and
 *                preserve the existing C,V.
 */
static void
gen_native_flags(int mode26, int set_cv)
{
	uint32_t take = set_cv ? 0xf0000000u : 0xc0000000u;
	uint32_t keep = ~take;

#ifdef RPCEMU_JIT_TEST
	codegen_test_flag_emits++;
#endif

	a64_mrs_nzcv(W_TMP);	/* w11 = host NZCV in bits 31:28 */
	if (mode26) {
		a64_load_imm64(W_OP2, keep); a64_and(A64_R15, A64_R15, W_OP2);
		a64_load_imm64(W_OP2, take); a64_and(W_TMP, W_TMP, W_OP2);
		a64_orr(A64_R15, A64_R15, W_TMP);
	} else {
		a64_ldr_w(W_RN, A64_ARM, 16 * 4);	/* arm.reg[16] (cpsr) */
		a64_load_imm64(W_OP2, keep); a64_and(W_RN, W_RN, W_OP2);
		a64_load_imm64(W_OP2, take); a64_and(W_TMP, W_TMP, W_OP2);
		a64_orr(W_RN, W_RN, W_TMP);
		a64_str_w(W_RN, A64_ARM, 16 * 4);
	}
}

/* Exit the block to the dispatcher if an IRQ is pending (arm.event & 0x40). */
static void
gen_test_armirq(void)
{
	a64_ldr_w(A64_X9, A64_ARM, (uint32_t) offsetof(ARMState, event));
	a64_load_imm64(A64_X10, 0x40);
	a64_and(A64_X9, A64_X9, A64_X10);
	a64_cbnz_w_to(A64_X9, 0);
}

/* Call a memory C helper: args already staged in w0(/w1), sync and reload the
   R15 cache around the call. */
static void
gen_mem_c_call(const void *fn)
{
	a64_str_w(A64_R15, A64_ARM, R15_OFF);
	a64_load_imm64(A64_X12, (uintptr_t) fn);
	a64_blr(A64_X12);
	a64_ldr_w(A64_R15, A64_ARM, R15_OFF);
}

/*
 * Memory access helpers, mirroring the amd64 backend. The address is in x23
 * (A64_MADDR, preserved across the C-helper call); load results / store data
 * pass through w9. Each tries the read/write TLB (x22/x21) inline and falls
 * back to the C helper on a miss. host_addr = guest_addr + tlb_entry, with the
 * low bit(s) of the entry flagging "not accessible" (bit 0 for reads, bits 0-1
 * for writes) - matching readmemfl()/writememfl() in mem.h.
 */
static void
genldr(void)
{
	int miss, done;

	a64_lsr_w_imm(A64_X10, A64_MADDR, 12);				/* TLB index */
	a64_lsr_w_imm(A64_X11, A64_MADDR, 2); a64_lsl_i(A64_X11, A64_X11, 2); /* addr & ~3 */
	a64_ldr_x_reg(A64_X12, A64_VRP, A64_X10);			/* vraddrl[idx] */
	miss = a64_tbnz_w0_forward(A64_X12);
	a64_ldr_w_regoff(A64_X9, A64_X12, A64_X11);
	done = a64_b_forward();
	a64_patch_tbnz_here(miss);
	a64_mov_w(A64_X0, A64_X11);
	gen_mem_c_call((const void *) readmemfl);
	a64_mov_w(A64_X9, A64_X0);
	if (arm.abort_base_restored) { gen_test_armirq(); }
	a64_patch_b_here(done);
	/* Rotate right by (addr & 3) * 8 for unaligned words. */
	a64_lsl_i(A64_X10, A64_MADDR, 30); a64_lsr_w_imm(A64_X10, A64_X10, 30);
	a64_lsl_i(A64_X10, A64_X10, 3);
	a64_rorv(A64_X9, A64_X9, A64_X10);
}

static void
genldrb(void)
{
	int miss, done;

	a64_lsr_w_imm(A64_X10, A64_MADDR, 12);
	a64_ldr_x_reg(A64_X12, A64_VRP, A64_X10);
	miss = a64_tbnz_w0_forward(A64_X12);
	a64_ldrb_reg(A64_X9, A64_X12, A64_MADDR);
	done = a64_b_forward();
	a64_patch_tbnz_here(miss);
	a64_mov_w(A64_X0, A64_MADDR);
	gen_mem_c_call((const void *) readmemfb);
	a64_mov_w(A64_X9, A64_X0);
	if (arm.abort_base_restored) { gen_test_armirq(); }
	a64_patch_b_here(done);
}

static void
genstr(void)
{
	int miss, done;

	a64_lsr_w_imm(A64_X10, A64_MADDR, 12);
	a64_lsr_w_imm(A64_X11, A64_MADDR, 2); a64_lsl_i(A64_X11, A64_X11, 2); /* addr & ~3 */
	a64_ldr_x_reg(A64_X12, A64_VWP, A64_X10);			/* vwaddrl[idx] */
	a64_lsl_i(A64_X13, A64_X12, 30); a64_lsr_w_imm(A64_X13, A64_X13, 30); /* entry & 3 */
	miss = a64_cbnz_w_forward(A64_X13);
	a64_str_w_regoff(A64_X9, A64_X12, A64_X11);
	done = a64_b_forward();
	a64_patch_here(miss);
	a64_mov_w(A64_X0, A64_X11);
	a64_mov_w(A64_X1, A64_X9);
	gen_mem_c_call((const void *) writememfl);
	if (arm.abort_base_restored) { gen_test_armirq(); }
	a64_patch_b_here(done);
}

static void
genstrb(void)
{
	int miss, done;

	a64_lsr_w_imm(A64_X10, A64_MADDR, 12);
	a64_ldr_x_reg(A64_X12, A64_VWP, A64_X10);
	a64_lsl_i(A64_X13, A64_X12, 30); a64_lsr_w_imm(A64_X13, A64_X13, 30);
	miss = a64_cbnz_w_forward(A64_X13);
	a64_strb_regoff(A64_X9, A64_X12, A64_MADDR);
	done = a64_b_forward();
	a64_patch_here(miss);
	a64_mov_w(A64_X0, A64_MADDR);
	a64_mov_w(A64_X1, A64_X9);
	gen_mem_c_call((const void *) writememfb);
	if (arm.abort_base_restored) { gen_test_armirq(); }
	a64_patch_b_here(done);
}

/*
 * LDM/STM support (S-flag clear), mirroring the amd64 backend. x10 carries the
 * (aligned) transfer address, x11 the base-writeback value; both are computed
 * up front so the C-helper fallback can be handed the guest address unchanged.
 */
static void
gen_arm_ldm_stm_decrement(uint32_t opcode, uint32_t offset)
{
	gen_load_reg(RN, A64_X10);
	a64_sub_w_imm(A64_X10, A64_X10, offset);
	a64_mov_w(A64_X11, A64_X10);			/* writeback value */
	if (!(opcode & (1u << 24))) {			/* Decrement After */
		a64_add_w_imm(A64_X10, A64_X10, 4);
	}
	a64_lsr_w_imm(A64_X10, A64_X10, 2); a64_lsl_i(A64_X10, A64_X10, 2); /* align */
}

static void
gen_arm_ldm_stm_increment(uint32_t opcode, uint32_t offset)
{
	gen_load_reg(RN, A64_X10);
	a64_mov_w(A64_X11, A64_X10);
	a64_add_w_imm(A64_X11, A64_X11, offset);	/* writeback value */
	if (opcode & (1u << 24)) {			/* Increment Before */
		a64_add_w_imm(A64_X10, A64_X10, 4);
	}
	a64_lsr_w_imm(A64_X10, A64_X10, 2); a64_lsl_i(A64_X10, A64_X10, 2); /* align */
}

/* Fall back to the C helper: args opcode/address/writeback in w0/w1/w2. */
static void
gen_call_ldm_stm_helper(uint32_t opcode, const void *fn)
{
	a64_load_imm64(A64_X0, opcode);
	a64_mov_w(A64_X1, A64_X10);
	a64_mov_w(A64_X2, A64_X11);
	a64_str_w(A64_R15, A64_ARM, R15_OFF);
	a64_load_imm64(A64_X9, (uintptr_t) fn);
	a64_blr(A64_X9);
	a64_ldr_w(A64_R15, A64_ARM, R15_OFF);
	gen_test_armirq();
}

static void
gen_arm_store_multiple(uint32_t opcode, uint32_t offset)
{
	int cross, miss, done;
	uint32_t mask, d;
	int c;

	/* Cross a (sub-)page boundary? -> helper. */
	a64_load_imm64(A64_X12, 0xfffffc00);
	a64_orr(A64_X9, A64_X10, A64_X12);
	a64_adds_w_imm(A64_X9, A64_X9, offset - 1);
	cross = a64_b_cond_forward(A64_CC_CS);

	/* Write-TLB lookup (entry & 3 != 0 -> not writable -> helper). */
	a64_lsr_w_imm(A64_X9, A64_X10, 12);
	a64_ldr_x_reg(A64_X13, A64_VWP, A64_X9);
	a64_lsl_i(A64_X12, A64_X13, 30); a64_lsr_w_imm(A64_X12, A64_X12, 30);
	miss = a64_cbnz_w_forward(A64_X12);

	a64_add_x(A64_X10, A64_X10, A64_X13);		/* guest -> host address */

	mask = 1; d = 0;
	for (c = 0; c < 15; c++) {
		if (opcode & mask) {
			gen_load_reg(c, A64_X9);
			a64_str_w(A64_X9, A64_X10, d);
			d += 4; c++; mask <<= 1;
			break;
		}
		mask <<= 1;
	}
	if (!arm.stm_writeback_at_end && (opcode & (1u << 21)) && RN != 15) {
		gen_save_reg(RN, A64_X11);
	}
	for (; c < 16; c++) {
		if (opcode & mask) {
			gen_load_reg(c, A64_X9);
			if (c == 15 && arm.r15_diff != 0) {
				a64_add_w_imm(A64_X9, A64_X9, arm.r15_diff);
			}
			a64_str_w(A64_X9, A64_X10, d);
			d += 4;
		}
		mask <<= 1;
	}
	if (arm.stm_writeback_at_end && (opcode & (1u << 21)) && RN != 15) {
		gen_save_reg(RN, A64_X11);
	}
	done = a64_b_forward();

	a64_patch_here(cross);
	a64_patch_here(miss);
	gen_call_ldm_stm_helper(opcode, (const void *) arm_store_multiple);
	a64_patch_b_here(done);
}

static void
gen_arm_load_multiple(uint32_t opcode, uint32_t offset)
{
	int cross, miss, done;
	uint32_t mask, d;
	int c;

	a64_load_imm64(A64_X12, 0xfffffc00);
	a64_orr(A64_X9, A64_X10, A64_X12);
	a64_adds_w_imm(A64_X9, A64_X9, offset - 1);
	cross = a64_b_cond_forward(A64_CC_CS);

	/* Read-TLB lookup (bit 0 set -> not readable -> helper). */
	a64_lsr_w_imm(A64_X9, A64_X10, 12);
	a64_ldr_x_reg(A64_X13, A64_VRP, A64_X9);
	miss = a64_tbnz_w0_forward(A64_X13);

	a64_add_x(A64_X10, A64_X10, A64_X13);

	if ((opcode & (1u << 21)) && RN != 15) {
		gen_save_reg(RN, A64_X11);		/* writeback before loads */
	}
	mask = 1; d = 0;
	for (c = 0; c < 16; c++) {
		if (opcode & mask) {
			a64_ldr_w(A64_X9, A64_X10, d);
			if (c == 15) {
				/* R15 = ((loaded + 4) & r15_mask) | (R15 & ~r15_mask) */
				a64_load_imm64(A64_X12, arm.r15_mask);
				gen_load_reg(15, A64_X13);
				a64_add_w_imm(A64_X9, A64_X9, 4);
				a64_and(A64_X9, A64_X9, A64_X12);
				a64_orn(A64_X14, A64_ZR, A64_X12);
				a64_and(A64_X13, A64_X13, A64_X14);
				a64_orr(A64_X9, A64_X9, A64_X13);
			}
			gen_save_reg(c, A64_X9);
			d += 4;
		}
		mask <<= 1;
	}
	done = a64_b_forward();

	a64_patch_here(cross);
	a64_patch_tbnz_here(miss);
	gen_call_ldm_stm_helper(opcode, (const void *) arm_load_multiple);
	a64_patch_b_here(done);
}

/* Dispatch entry for the block data transfer (LDM/STM) class. */
static int
gen_block_transfer(uint32_t opcode)
{
	uint32_t offset = (uint32_t) countbitstable[opcode & 0xffff];

	if (RN == 15) {
		return 0;
	}
	if (opcode & 0x400000) {
		return 0;	/* S-flag (user-mode / PSR) forms -> fall back */
	}

	if (opcode & (1u << 23)) {
		gen_arm_ldm_stm_increment(opcode, offset);
	} else {
		gen_arm_ldm_stm_decrement(opcode, offset);
	}
	if (opcode & 0x100000) {
		gen_arm_load_multiple(opcode, offset);
	} else {
		gen_arm_store_multiple(opcode, offset);
	}

	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

/*
 * Native code generation for the ARM single data transfer class (LDR/STR/
 * LDRB/STRB), covering pre/post-index, up/down, register/immediate offset and
 * base writeback. Address is built in x23 (MADDR); a register offset that must
 * survive the access (post-index writeback) is preserved in x24. Returns 1 on
 * success, 0 to fall back.
 */
static int
gen_single_data_transfer(uint32_t opcode)
{
	const int is_load = (opcode & 0x100000) != 0;
	const int is_byte = (opcode & 0x400000) != 0;
	const int pre     = (opcode & 0x1000000) != 0;
	const int up      = (opcode & 0x800000) != 0;
	const int regoff  = (opcode & 0x2000000) != 0;
	const int writeback = (opcode & 0x200000) != 0;
	const uint32_t imm = opcode & 0xfff;

	if (RD == 15) {
		return 0;
	}
	if (!pre && RN == 15) {
		return 0; /* post-index with R15 base -> fall back */
	}

	if (regoff) {
		if (!generate_shift(opcode)) {
			return 0; /* register-specified shift -> fall back */
		}
	}

	if (pre) {
		/* address = Rn +/- offset */
		gen_load_reg(RN, A64_MADDR);
		if (RN == 15) {
			a64_load_imm64(A64_X13, arm.r15_mask);
			a64_and(A64_MADDR, A64_MADDR, A64_X13);
		}
		if (regoff) {
			if (up) { a64_add(A64_MADDR, A64_MADDR, W_OP2); }
			else    { a64_sub(A64_MADDR, A64_MADDR, W_OP2); }
		} else if (imm != 0) {
			if (up) { a64_add_w_imm(A64_MADDR, A64_MADDR, imm); }
			else    { a64_sub_w_imm(A64_MADDR, A64_MADDR, imm); }
		}
	} else {
		/* post-index: access at Rn, offset applied afterwards */
		if (regoff) {
			a64_mov_w(A64_X24, W_OP2); /* preserve offset across the access */
		}
		gen_load_reg(RN, A64_MADDR);
	}

	if (is_load) {
		if (is_byte) { genldrb(); } else { genldr(); } /* result in w9 */
	} else {
		gen_load_reg(RD, A64_X9);                        /* store data */
		if (is_byte) { genstrb(); } else { genstr(); }
	}

	if (pre) {
		if (writeback) {
			gen_save_reg(RN, A64_MADDR);
		}
	} else {
		/* post-index always writes back Rn = original Rn +/- offset */
		if (regoff) {
			if (up) { a64_add(A64_X10, A64_MADDR, A64_X24); }
			else    { a64_sub(A64_X10, A64_MADDR, A64_X24); }
		} else if (up) {
			a64_add_w_imm(A64_X10, A64_MADDR, imm);
		} else {
			a64_sub_w_imm(A64_X10, A64_MADDR, imm);
		}
		gen_save_reg(RN, A64_X10);
	}

	if (!arm.abort_base_restored) {
		gen_test_armirq();
	}
	if (is_load) {
		gen_save_reg(RD, A64_X9);
	}

	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

/*
 * Native code generation for MUL / MLA (32-bit) and UMULL (64-bit unsigned),
 * the multiply forms the amd64 backend recompiles. S-forms, signed and
 * accumulating long multiplies fall back. Note the ARM multiply operand fields
 * swap the usual RD/RN positions (MULRD at bits 19:16).
 */
static int
gen_multiply(uint32_t opcode)
{
	const int A  = (opcode & 0x200000) != 0;	/* MLA */
	const int rd = (opcode >> 16) & 0xf;
	const int rn = (opcode >> 12) & 0xf;
	const int rs = (opcode >> 8) & 0xf;
	const int rm = opcode & 0xf;

	if (opcode & 0x100000) {
		return 0;	/* flag-setting multiply -> fall back */
	}
	if (rd == rm || rd == 15) {
		return 0;	/* UNPREDICTABLE / PC destination -> fall back */
	}

	gen_load_reg(rm, A64_X9);
	gen_load_reg(rs, A64_X10);
	if (A) {
		gen_load_reg(rn, A64_X11);
		a64_madd(A64_X9, A64_X9, A64_X10, A64_X11);
	} else {
		a64_mul(A64_X9, A64_X9, A64_X10);
	}
	gen_save_reg(rd, A64_X9);

	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

static int
gen_multiply_long(uint32_t opcode)
{
	const int rdhi = (opcode >> 16) & 0xf;
	const int rdlo = (opcode >> 12) & 0xf;
	const int rs = (opcode >> 8) & 0xf;
	const int rm = opcode & 0xf;

	/* Only UMULL (unsigned, no accumulate, no S) — matches the amd64 backend. */
	if ((opcode & 0x0ff000f0) != 0x00800090) {
		return 0;
	}
	if (rdhi == 15 || rdlo == 15 || rdhi == rdlo) {
		return 0;
	}

	gen_load_reg(rm, A64_X9);
	gen_load_reg(rs, A64_X10);
	a64_umull(A64_X9, A64_X9, A64_X10);	/* x9 = rm * rs (unsigned 64-bit) */
	gen_save_reg(rdlo, A64_X9);
	a64_lsr_x_imm(A64_X9, A64_X9, 32);
	gen_save_reg(rdhi, A64_X9);

	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

/*
 * Native code generation for B / BL. Mirrors the amd64 backend: in the common
 * case just add the (compile-time) offset to the R15 cache (and, for BL, save
 * the return address in R14); the slow path additionally re-merges the 26-bit
 * PC field with the preserved PSR/mode bits. Ends the block.
 */
static int
gen_branch(uint32_t opcode, uint32_t *pcpsr)
{
	int32_t offset = ((int32_t) (opcode << 8)) >> 6;
	int link, fast;

	NOT_USED(pcpsr);
	offset += 4;
	link = (opcode & 0x01000000) != 0;	/* BL sets the link bit */
	fast = (((PC + (uint32_t) offset) & 0xfc000000) == 0) || (arm.r15_mask == 0xfffffffc);

	if (link) {
		a64_mov_w(A64_X9, A64_R15);
		a64_sub_w_imm(A64_X9, A64_X9, 4);	/* return address = R15 - 4 */
	}

	if (fast) {
		if (link) {
			gen_save_reg(14, A64_X9);
		}
		a64_load_imm64(A64_X10, (uint32_t) offset);
		a64_add(A64_R15, A64_R15, A64_X10);
	} else {
		if (link) {
			gen_save_reg(14, A64_X9);	/* R14 = R15 - 4 */
			a64_mov_w(A64_X10, A64_X9);	/* PSR source */
			a64_add_w_imm(A64_X9, A64_X9, 4);	/* PC source = R15 */
		} else {
			a64_mov_w(A64_X9, A64_R15);
			a64_mov_w(A64_X10, A64_X9);
		}
		a64_load_imm64(A64_X11, 0xfc000003); a64_and(A64_X10, A64_X10, A64_X11);
		a64_load_imm64(A64_X11, (uint32_t) offset); a64_add(A64_X9, A64_X9, A64_X11);
		a64_load_imm64(A64_X11, 0x03fffffc); a64_and(A64_X9, A64_X9, A64_X11);
		a64_orr(A64_X9, A64_X9, A64_X10);
		a64_mov_w(A64_R15, A64_X9);
	}

	blockend = 1;
	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

/*
 * Native code generation for the ARM data-processing instruction class. Returns
 * 1 if the instruction was fully compiled, 0 to fall back to the C interpreter
 * helper (via generatecall). Handles AND/EOR/SUB/RSB/ADD/ORR/MOV/BIC/MVN and
 * the TST/TEQ/CMP/CMN compares, register (immediate shift) and immediate forms,
 * with correct NZCV. ADC/SBC/RSC and PC-destination forms are declined (as on
 * the amd64 backend, gated by canrecompile[]).
 */
static int
recompile(uint32_t opcode, uint32_t *pcpsr)
{
	int op, S, I, mode26, is_logical, writes_rd;

	/* v4 halfword / signed-byte transfers alias into the data-processing space;
	   not yet recompiled. */
	if (arm.arch_v4) {
		if ((opcode & 0xe0000f0) == 0xb0) {
			return 0;
		}
		if ((opcode & 0xe1000d0) == 0x1000d0) {
			return 0;
		}
	}

	/* Single data transfer (LDR/STR/LDRB/STRB) — class 01. */
	if ((opcode & 0x0c000000) == 0x04000000) {
		return gen_single_data_transfer(opcode);
	}

	/* Block data transfer (LDM/STM) — class 10, bit 25 clear. */
	if ((opcode & 0x0e000000) == 0x08000000) {
		return gen_block_transfer(opcode);
	}

	/* Branch / Branch-with-link — class 10, bit 25 set. */
	if ((opcode & 0x0e000000) == 0x0a000000) {
		return gen_branch(opcode, pcpsr);
	}

	/* Only the data-processing class (bits 27:26 == 00) remains below. */
	if ((opcode & 0x0c000000) != 0) {
		return 0;
	}

	/* Multiply forms (bits 7:4 == 1001 within the data-processing space). */
	if ((opcode & 0x0fc000f0) == 0x00000090) {
		return gen_multiply(opcode);
	}
	if (arm.arch_v4 && (opcode & 0x0f8000f0) == 0x00800090) {
		return gen_multiply_long(opcode);
	}

	op = (opcode >> 21) & 0xf;
	S = (opcode >> 20) & 1;
	I = (opcode >> 25) & 1;
	mode26 = (pcpsr == &arm.reg[15]);
	is_logical = (op == 0 || op == 1 || op == 8 || op == 9 ||
	              op == 12 || op == 13 || op == 14 || op == 15);
	writes_rd = !(op >= 8 && op <= 11);
	/* ADC/SBC/RSC use carry-in; decline (also excluded by canrecompile[]). */
	if (op == 5 || op == 6 || op == 7) {
		return 0;
	}
	/* TST/TEQ/CMP/CMN without S are PSR-transfer ops, not compares. */
	if (!writes_rd && !S) {
		return 0;
	}
	if (writes_rd && RD == 15) {
		return 0; /* PC destination -> fall back */
	}
	/* A compare with RD == 15 and S set is the 26-bit PSR transfer form
	   (TSTP/TEQP/CMPP/CMNP): it writes the PSR and can change processor mode.
	   The interpreter handles it through arm_compare_rd15(); compiling it as an
	   ordinary compare would set the flags and drop the mode change silently. */
	if (!writes_rd && RD == 15) {
		return 0;
	}

	/* Compute the operand into W_OP2. */
	if (I) {
		uint32_t imm8 = opcode & 0xff;
		uint32_t rot = ((opcode >> 8) & 0xf) * 2;
		uint32_t op2 = rot ? ((imm8 >> rot) | (imm8 << (32 - rot))) : imm8;

		/* A non-zero rotate sets the shifter carry for flag-setting logical
		   ops; decline so C is never mis-set (LSL#0/rot0 leaves C alone). */
		if (S && is_logical && rot != 0) {
			return 0;
		}
		a64_load_imm64(W_OP2, op2);
	} else {
		if (S && is_logical && (opcode & 0xff0) != 0) {
			return 0; /* shifted flag-setting logical needs shifter carry */
		}
		if (!generate_shift(opcode)) {
			return 0;
		}
	}

	/* Load Rn (masked when it is R15); MOV/MVN ignore Rn. */
	if (op != 13 && op != 15) {
		gen_load_reg(RN, W_RN);
		if (RN == 15) {
			a64_load_imm64(W_TMP, arm.r15_mask);
			a64_and(W_RN, W_RN, W_TMP);
		}
	}

	switch (op) {
	case 0x0: /* AND */
		if (S) { a64_ands(W_RES, W_RN, W_OP2); } else { a64_and(W_RES, W_RN, W_OP2); }
		break;
	case 0x1: /* EOR */
		a64_eor(W_RES, W_RN, W_OP2);
		if (S) { a64_ands(A64_ZR, W_RES, W_RES); } /* TST to set N,Z */
		break;
	case 0x2: /* SUB */
		if (S) { a64_subs(W_RES, W_RN, W_OP2); } else { a64_sub(W_RES, W_RN, W_OP2); }
		break;
	case 0x3: /* RSB */
		if (S) { a64_subs(W_RES, W_OP2, W_RN); } else { a64_sub(W_RES, W_OP2, W_RN); }
		break;
	case 0x4: /* ADD */
		if (S) { a64_adds(W_RES, W_RN, W_OP2); } else { a64_add(W_RES, W_RN, W_OP2); }
		break;
	case 0x8: /* TST */
		a64_ands(A64_ZR, W_RN, W_OP2);
		break;
	case 0x9: /* TEQ */
		a64_eor(W_RES, W_RN, W_OP2);
		a64_ands(A64_ZR, W_RES, W_RES);
		break;
	case 0xa: /* CMP */
		a64_subs(A64_ZR, W_RN, W_OP2);
		break;
	case 0xb: /* CMN */
		a64_adds(A64_ZR, W_RN, W_OP2);
		break;
	case 0xc: /* ORR */
		a64_orr(W_RES, W_RN, W_OP2);
		if (S) { a64_ands(A64_ZR, W_RES, W_RES); }
		break;
	case 0xd: /* MOV */
		a64_mov_w(W_RES, W_OP2);
		if (S) { a64_ands(A64_ZR, W_RES, W_RES); }
		break;
	case 0xe: /* BIC */
		if (S) { a64_bics(W_RES, W_RN, W_OP2); } else { a64_bic(W_RES, W_RN, W_OP2); }
		break;
	case 0xf: /* MVN */
		a64_orn(W_RES, A64_ZR, W_OP2);
		if (S) { a64_ands(A64_ZR, W_RES, W_RES); }
		break;
	default:
		return 0;
	}

	if (S) {
		gen_native_flags(mode26, is_logical ? 0 : 1);
	}
	if (writes_rd) {
		gen_save_reg(RD, W_RES);
	}

	lastrecompiled = 1;
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
	return 1;
}

/* Which (opcode>>20)&0xff values recompile() handles — same set as the amd64
   backend. 0x00-0x1f are register-form, 0x20-0x3f immediate-form; index is
   (op<<1)|S within each. ADC/SBC/RSC and the PSR-transfer encodings are 0. */
static const int canrecompile[256] = {
	1,1,1,1,1,1,0,1,1,1,0,0,0,0,0,0, // 00 (reg)  AND EOR SUB RSB ADD ADC SBC RSC
	0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1, // 10 (reg)  TST TEQ CMP CMN ORR MOV BIC MVN
	1,1,1,1,1,1,0,1,1,1,0,0,0,0,0,0, // 20 (imm)
	0,1,0,1,0,1,0,1,1,1,1,1,1,1,1,1, // 30 (imm)

	1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0, // 40 LDR/STR post-index
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // 50 LDR/STR pre-index
	1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0, // 60 LDR/STR post-index (reg offset)
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // 70 LDR/STR pre-index (reg offset)
	1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0, // 80 STM/LDM (S-clear)
	1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0, // 90 STM/LDM (S-clear)
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // a0 B
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // b0 BL
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // c0
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // d0
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // e0
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // f0
};

void
generatecall(OpFn addr, uint32_t opcode, uint32_t *pcpsr)
{
	lastrecompiled = 0;

	if (canrecompile[(opcode >> 20) & 0xff]) {
		if (recompile(opcode, pcpsr)) {
			return;
		}
	}

	/* arg1 = opcode; sync the R15 cache to memory (the helper may read/write
	   arm.reg[15]); call the helper; reload the cache. */
	a64_load_imm64(A64_X0, opcode);
	a64_str_w(A64_R15, A64_ARM, R15_OFF);
	gen_call_c_function((const void *) addr);
	a64_ldr_w(A64_R15, A64_ARM, R15_OFF);

	/* Complete the conditional skip emitted by generateflagtestandbranch, so a
	   failed guest condition jumps past this call. */
	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
}

void
generateupdatepc(void)
{
	if (pcinc != 0) {
		a64_add_w_imm(A64_R15, A64_R15, (uint32_t) pcinc);	/* x20 += pcinc */
		pcinc = 0;
	}
}

void
generateupdateinscount(void)
{
	if (tempinscount != 0) {
		/* inscount += tempinscount (tempinscount is small: bounded by block
		   length, always < 4096). */
		assert(tempinscount < 4096);
		a64_load_imm64(A64_X9, (uintptr_t) &inscount);
		a64_ldr_w(A64_X10, A64_X9, 0);
		a64_add_w_imm(A64_X10, A64_X10, (uint32_t) tempinscount);
		a64_str_w(A64_X10, A64_X9, 0);
		tempinscount = 0;
	}
}

void
generatepcinc(void)
{
	lastjumppos = 0;
	tempinscount++;
	pcinc += 4;
	if (pcinc == 124) {
		generateupdatepc();
	}
	if (codeblockpos >= 1500) {
		blockend = 1;
	}
}

/* Instruction-cache maintenance for a freshly written block. Normally the
   toolchain builtin, which on AArch64 expands to the dc cvau / ic ivau / dsb /
   isb sequence that makes a data write visible to the instruction fetcher.

   Setting RPCEMU_ARM64_ICACHE_PARANOID replaces the builtin with the same
   sequence coded by hand, reading the cache line sizes from CTR_EL0. This is a
   diagnostic for issue #30: a recompiler-only blank screen on real arm64
   hardware while the interpreter and qemu-aarch64 are fine. qemu re-translates
   modified code automatically, so it cannot reproduce a genuine I-cache
   coherency fault - and if a target's __builtin___clear_cache is weak or
   stubbed, this hand-rolled path both proves and fixes it. The choice is read
   once and cached. Apple Silicon keeps the builtin (dc/ic from EL0 is not the
   sanctioned path there; sys_icache_invalidate would be, and that route is a
   separate concern from the Linux/Pi report). */
static void
codegen_flush_icache(void *start, void *end)
{
#if !defined(__APPLE__)
	static int paranoid = -1;

	if (paranoid < 0) {
		const char *e = getenv("RPCEMU_ARM64_ICACHE_PARANOID");

		paranoid = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
		if (paranoid) {
			rpclog("codegen_arm64: hand-rolled (paranoid) I-cache maintenance enabled\n");
		}
	}

	if (paranoid) {
		const uintptr_t s = (uintptr_t) start;
		const uintptr_t e = (uintptr_t) end;
		uint64_t ctr;
		uintptr_t dline, iline, p;

		__asm__ __volatile__("mrs %0, ctr_el0" : "=r" (ctr));
		dline = (uintptr_t) 4u << ((ctr >> 16) & 0xf);	/* CTR_EL0.DminLine */
		iline = (uintptr_t) 4u << (ctr & 0xf);		/* CTR_EL0.IminLine */

		for (p = s & ~(dline - 1); p < e; p += dline) {
			__asm__ __volatile__("dc cvau, %0" : : "r" (p) : "memory");
		}
		__asm__ __volatile__("dsb ish" : : : "memory");
		for (p = s & ~(iline - 1); p < e; p += iline) {
			__asm__ __volatile__("ic ivau, %0" : : "r" (p) : "memory");
		}
		__asm__ __volatile__("dsb ish\n\tisb" : : : "memory");
		return;
	}
#endif

	__builtin___clear_cache((char *) start, (char *) end);
}

void
endblock(uint32_t opcode)
{
	NOT_USED(opcode);

	generateupdatepc();
	generateupdateinscount();

	/* Block linking: decrement the cycle budget, and unless it has run out or
	   an event is pending, look the next block up by PC and jump straight into
	   it (bypassing its prologue via block_enter) so the R15 cache in x20 stays
	   live and we avoid a round-trip through the dispatcher. Any miss falls
	   through to the epilogue, which writes x20 back and returns. */
	a64_load_imm64(A64_X9, (uintptr_t) &linecyc);
	a64_ldr_w(A64_X10, A64_X9, 0);
	a64_subs_w_imm(A64_X10, A64_X10, 1);
	a64_str_w(A64_X10, A64_X9, 0);
	a64_b_cond_to(A64_CC_MI, 0);			/* linecyc < 0 -> exit */

	a64_ldr_w(A64_X10, A64_ARM, (uint32_t) offsetof(ARMState, event));
	a64_load_imm64(A64_X11, 0xff); a64_and(A64_X10, A64_X10, A64_X11);
	a64_cbnz_w_to(A64_X10, 0);			/* event pending -> exit */

	/* next PC = (R15 - 8) & r15_mask */
	a64_sub_w_imm(A64_X10, A64_R15, 8);
	a64_load_imm64(A64_X11, arm.r15_mask); a64_and(A64_X9, A64_X10, A64_X11);

	/* hash = (PC >> 2) & 0x7fff */
	a64_lsr_w_imm(A64_X11, A64_X9, 2);
	a64_load_imm64(A64_X12, 0x7fff); a64_and(A64_X11, A64_X11, A64_X12);

	/* if (codeblockpc[hash] != PC) exit */
	a64_load_imm64(A64_X12, (uintptr_t) &codeblockpc[0]);
	a64_ldr_w_reg(A64_X13, A64_X12, A64_X11);
	a64_subs(A64_ZR, A64_X13, A64_X9);		/* CMP */
	a64_b_cond_to(A64_CC_NE, 0);			/* miss -> exit */

	/* base = codeblockaddr[codeblocknum[hash]]; jump base + block_enter */
	a64_load_imm64(A64_X12, (uintptr_t) &codeblocknum[0]);
	a64_ldr_w_reg(A64_X13, A64_X12, A64_X11);
	a64_load_imm64(A64_X12, (uintptr_t) &codeblockaddr[0]);
	a64_ldr_x_reg(A64_X14, A64_X12, A64_X13);
	a64_add_x_imm(A64_X14, A64_X14, (uint32_t) block_enter);
	a64_br(A64_X14);

	/* Block complete: return the cache to executable (a no-op off Apple Silicon)
	   before the I-cache maintenance below, matching Apple's write-then-execute
	   ordering. */
	codegen_jit_executable();

	/* AArch64 does not keep I- and D-caches coherent: make the freshly written
	   block visible to the instruction fetcher before it is executed. This is
	   the last write to the block, so flushing the whole range here covers the
	   epilogue, prologue and body. */
	codegen_flush_icache(&rcodeblock[blockpoint2][0],
	                     &rcodeblock[blockpoint2][codeblockpos]);
}

void
generateflagtestandbranch(uint32_t opcode, uint32_t *pcpsr)
{
	if ((opcode >> 28) == 0xe) {
		/* 'always' - no test needed */
		return;
	}

	/* Generic path (mirrors the amd64 default case): load the flag nibble,
	   index flaglookup[cond][nibble], and skip the instruction if it is 0. */
	if (pcpsr == &arm.reg[15]) {
		a64_mov_w(A64_X9, A64_R15);		/* w9 = R15 cache (holds flags in 26-bit mode) */
	} else {
		a64_load_imm64(A64_X10, (uintptr_t) pcpsr);
		a64_ldr_w(A64_X9, A64_X10, 0);
	}
	a64_lsr_w_imm(A64_X9, A64_X9, 28);		/* w9 = flag nibble (0..15) */
	a64_load_imm64(A64_X10, (uintptr_t) &flaglookup[opcode >> 28][0]);
	a64_ldrb_reg(A64_X9, A64_X10, A64_X9);		/* w9 = flaglookup[cond][nibble] */
	lastjumppos = a64_cbz_w_forward(A64_X9);	/* skip instruction if 0 */
}

void
generateirqtest(void)
{
	if (lastrecompiled) {
		lastrecompiled = 0;
		return;
	}

	/* The interpreter helper returns non-zero in w0 if it raised an
	   abort/exception; leave the block so the dispatcher can service it. */
	a64_cbnz_w_to(A64_X0, 0);	/* if (w0 != 0) branch to epilogue */

	if (lastjumppos != 0) {
		a64_patch_here(lastjumppos);
	}
}
