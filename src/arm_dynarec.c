/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

int blockend;

/* FPA10 floating-point coprocessor emulation (see fpa.c). */
#define FPA

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined __linux__ || defined __MACH__
#	include <unistd.h>
#	include <sys/mman.h>
#elif defined _WIN32
#	include <windows.h>
#endif

#include "rpcemu.h"
#include "arm.h"
#include "cp15.h"
#include "mem.h"

#if defined __amd64__
#	include "codegen_amd64.h"
#elif defined i386 || defined __i386 || defined __i386__ || defined _X86_
#	include "codegen_x86.h"
#elif defined __aarch64__
#	include "codegen_arm64.h"
#else
#	error "Fatal error : no recompiler available for this architecture"
#endif

ARMState arm;

uint32_t inscount;
int cpsr;
uint32_t *pcpsr;

uint8_t flaglookup[16][16];

uint32_t *usrregs[16];
int prog32;

/**
 * 1 when the CPU is in 32-bit program mode, 0 in 26-bit.
 *
 * Recompiled blocks are only reused when they were compiled for the current
 * word size; see codeblockword[] in the backend header. Maintained by
 * updatemode(), which is the only thing that changes arm.r15_mask.
 */
int jit_word32;

static int unpredictable_count = 1000; ///< Limit logging of unpredictable instructions

#define NFSET	((arm.reg[cpsr] & NFLAG) ? 1u : 0)
#define ZFSET	((arm.reg[cpsr] & ZFLAG) ? 1u : 0)
#define CFSET	((arm.reg[cpsr] & CFLAG) ? 1u : 0)
#define VFSET	((arm.reg[cpsr] & VFLAG) ? 1u : 0)

#define refillpipeline() blockend=1;

#include "arm_common.h"

uint32_t pccache;
static const uint32_t *pccache2;

/**
 * Return true if this ARM core is the dynarec version
 *
 * @return 1 yes this is dynarec
 */
int
arm_is_dynarec(void)
{
	return 1;
}

void updatemode(uint32_t m)
{
        updatemode_at(m, PC);
}

void updatemode_at(uint32_t m, uint32_t pc)
{
        uint32_t c, om = arm.mode;

        usrregs[15] = &arm.reg[15];
        switch (arm.mode & 0xf) { /* Store back registers */
            case USER:
            case SYSTEM: /* System (ARMv4) shares same bank as User mode */
                for (c=8;c<15;c++) arm.user_reg[c] = arm.reg[c];
                break;

            case IRQ:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.irq_reg[0] = arm.reg[13];
                arm.irq_reg[1] = arm.reg[14];
                break;

            case FIQ:
                for (c=8;c<15;c++) arm.fiq_reg[c] = arm.reg[c];
                break;

            case SUPERVISOR:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.super_reg[0] = arm.reg[13];
                arm.super_reg[1] = arm.reg[14];
                break;

            case ABORT:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.abort_reg[0] = arm.reg[13];
                arm.abort_reg[1] = arm.reg[14];
                break;

            case UNDEFINED:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.undef_reg[0] = arm.reg[13];
                arm.undef_reg[1] = arm.reg[14];
                break;
        }
        arm.mode = m;

        switch (m&15)
        {
            case USER:
            case SYSTEM:
                for (c=8;c<15;c++) arm.reg[c] = arm.user_reg[c];
                for (c=0;c<15;c++) usrregs[c] = &arm.reg[c];
                break;

            case IRQ:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.irq_reg[0];
                arm.reg[14] = arm.irq_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;
            
            case FIQ:
                for (c=8;c<15;c++) arm.reg[c] = arm.fiq_reg[c];
                for (c=0;c<8;c++)  usrregs[c] = &arm.reg[c];
                for (c=8;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            case SUPERVISOR:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.super_reg[0];
                arm.reg[14] = arm.super_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;
            
            case ABORT:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.abort_reg[0];
                arm.reg[14] = arm.abort_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            case UNDEFINED:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.undef_reg[0];
                arm.reg[14] = arm.undef_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            default:
                /*
                 * One of the nine reserved mode values (4, 5, 6, 8, 9, 10, 12,
                 * 13, 14). Writing one is UNPREDICTABLE on real hardware, so
                 * this is a fault in the guest - which is what somebody would
                 * want to look at, and what fatal() used to take away with the
                 * rest of the process. Issue #227.
                 *
                 * The address comes from the caller rather than from R15,
                 * because the instructions that usually get a guest here -
                 * `MOVS PC, R14`, `LDM {..., PC}^` - have already written R15
                 * with the branch target by this point. Deriving it here named
                 * an instruction that had nothing to do with it, and named
                 * FFFFFFF8 when the value loaded was zero. Reported on
                 * discussion #223.
                 */
                if (!debugger_bad_mode(arm.mode, pc)) {
                        fatal("Bad mode %i\n", arm.mode);
                }

                /*
                 * Bank as User and System mode do, so the halted machine has
                 * defined registers to show and can be stepped afterwards.
                 *
                 * The store-back switch above deliberately has no matching arm:
                 * leaving a reserved mode discards whatever it did to R8-R14
                 * rather than writing it into the user bank, so a guest that
                 * strays into one and comes back does not find User mode's
                 * registers rewritten underneath it. Both halves are undefined
                 * behaviour on hardware; this is the half that damages less.
                 */
                for (c=8;c<15;c++) arm.reg[c] = arm.user_reg[c];
                for (c=0;c<15;c++) usrregs[c] = &arm.reg[c];
                break;
        }

        if (ARM_MODE_32(arm.mode)) {
                arm.mmask = 0x1f;
                cpsr=16;
                pcpsr = &arm.reg[16];
                arm.r15_mask = 0xfffffffc;
                if (!ARM_MODE_32(om)) {
			/* Change from 26-bit to 32-bit mode */
                        arm.reg[16] = (arm.reg[15] & 0xf0000000) | arm.mode;
                        arm.reg[16] |= ((arm.reg[15] & 0xc000000) >> 20);
                        arm.reg[15] &= 0x3fffffc;
                }
        }
        else
        {
                arm.mmask = 3;
                cpsr=15;
                pcpsr = &arm.reg[15];
                arm.r15_mask = 0x3fffffc;
                arm.reg[16] = (arm.reg[16] & 0xffffffe0) | arm.mode;
                if (ARM_MODE_32(om)) {
                        arm.reg[15] &= arm.r15_mask;
                        arm.reg[15] |= (arm.mode & 3);
                        arm.reg[15] |= (arm.reg[16] & 0xf0000000);
                        arm.reg[15] |= ((arm.reg[16] & 0xc0) << 20);
                }
        }

	/*
	 * ★ A 26/32-bit switch changes which recompiled blocks may be used.
	 *
	 * Each block records the word size it was compiled for and is only reused
	 * when that matches, so both sets survive the change. What follows is why
	 * they have to be told apart at all.
	 *
	 * The code generators bake arm.r15_mask into generated code as an immediate,
	 * and in places use it at generation time to decide whether to mask R15 at
	 * all. A block compiled while the CPU was in 32-bit mode therefore either
	 * masks with 0xfffffffc or does not mask, and running it in 26-bit mode
	 * leaves the PSR bits of R15 in what should be an address. Nothing else
	 * invalidated the cache on a mode change, so those blocks were reused.
	 *
	 * That is issue #154: under a 26-bit OS, a PC-relative LDR aborted because
	 * the computed address had the flags in it. It only happened with the
	 * recompiler - the interpreter reads arm.r15_mask afresh every time - and it
	 * went away with the debugger active, because that forces interpretation.
	 *
	 * Throwing the whole cache away was the first answer and it was ruinous.
	 * "The mask only changes when the CPU changes word size, which is rare" is
	 * true of RISC OS 5 and false of everything older: RISC OS 3.7 and 4.x set
	 * PROG32 in the CP15 control register (measured, 3.71 settles on &127D)
	 * while running in 26-bit modes, so exception() takes its prog32 branch
	 * into SVC32 and the return comes back to SVC26. That is twice per SWI -
	 * some 63,000 round trips a second measured on RISC OS 3.71 - so the cache
	 * was emptied around 126,000 times a second, and the recompiler compiled
	 * 3.7 million blocks a second only to discard them. Recording the word size
	 * per block instead took that to 10,000 a second and raised cache hits from
	 * 0.23M/s to 0.97M/s.
	 *
	 * The interpreter needs none of this: it reads arm.r15_mask afresh every
	 * time.
	 */
	jit_word32 = (arm.r15_mask == 0xfffffffc) ? 1 : 0;

	/* Update memory access mode based on privilege level of ARM mode. Goes
	   through mem_set_privilege() so the fast maps follow the privilege level;
	   see the comment on vraddrl_mode[] in cp15.c. */
	mem_set_privilege(ARM_MODE_PRIV(arm.mode) ? 1 : 0);
}

static int stmlookup[256];

int countbitstable[65536];

/**
 * Opcode classes the recompiler has been told to leave to the interpreter.
 *
 * Indexed exactly as canrecompile[] is, by (opcode >> 20) & 0xff, so an entry
 * covers one instruction class with one set of condition-independent bits. Zero
 * throughout unless RPCEMU_JIT_DENY says otherwise, and read only while a block
 * is being compiled, so it costs nothing at run time.
 *
 * This exists to bisect a backend fault that only shows on a real boot. If the
 * machine misbehaves under the recompiler but is well under the interpreter, the
 * difference is one of these classes, and denying half of them at a time finds
 * which in about eight boots. That works on hardware we do not have, which is
 * the point: a tester can bisect a fault on their own machine without building
 * anything.
 */
uint8_t jit_deny[256];

/**
 * Read RPCEMU_JIT_DENY and mark the classes it names.
 *
 * Accepts a comma-separated list of decimal or 0x-prefixed indices and lo-hi
 * ranges, or "all" for every class:
 *
 *   RPCEMU_JIT_DENY=all           nothing is recompiled (so, the interpreter)
 *   RPCEMU_JIT_DENY=0x00-0x7f     the bottom half of the table
 *   RPCEMU_JIT_DENY=0x31,0x3b     TST immediate and MOV immediate alone
 *
 * A malformed entry is reported and ignored rather than being taken as zero: a
 * typo that silently denied class 0 would send a bisection off in the wrong
 * direction entirely.
 */
static void
jit_deny_init(void)
{
	const char *spec = getenv("RPCEMU_JIT_DENY");
	unsigned denied = 0;
	const char *p;

	memset(jit_deny, 0, sizeof(jit_deny));

	if (spec == NULL || *spec == '\0') {
		return;
	}

	if (strcmp(spec, "all") == 0) {
		memset(jit_deny, 1, sizeof(jit_deny));
		rpclog("JIT: RPCEMU_JIT_DENY=all, recompiling nothing\n");
		return;
	}

	p = spec;
	while (*p != '\0') {
		char *end;
		unsigned long lo, hi;

		while (*p == ',' || *p == ' ') {
			p++;
		}
		if (*p == '\0') {
			break;
		}

		lo = strtoul(p, &end, 0);
		if (end == p) {
			rpclog("JIT: RPCEMU_JIT_DENY: cannot parse \"%s\", ignoring the rest\n", p);
			break;
		}
		hi = lo;
		if (*end == '-') {
			p = end + 1;
			hi = strtoul(p, &end, 0);
			if (end == p) {
				rpclog("JIT: RPCEMU_JIT_DENY: range with no end at \"%s\", ignoring the rest\n", p);
				break;
			}
		}
		p = end;

		if (lo > 0xff || hi > 0xff || lo > hi) {
			rpclog("JIT: RPCEMU_JIT_DENY: %lu-%lu is not a valid class range, ignored\n", lo, hi);
			continue;
		}
		for (unsigned long i = lo; i <= hi; i++) {
			if (!jit_deny[i]) {
				jit_deny[i] = 1;
				denied++;
			}
		}
	}

	rpclog("JIT: RPCEMU_JIT_DENY=%s, %u opcode classes left to the interpreter\n",
	    spec, denied);
}

void
arm_init(void)
{
	unsigned c, d, exec;

	for (c = 0; c < 256; c++) {
		stmlookup[c] = 0;
		for (d = 0; d < 8; d++) {
			if (c & (1u << d)) {
				stmlookup[c] += 4;
			}
		}
	}
	for (c = 0; c < 65536; c++) {
		countbitstable[c] = 0;
		for (d = 0; d < 16; d++) {
			if (c & (1u << d)) {
				countbitstable[c] += 4;
			}
		}
	}

	cpsr = 15;
	for (c = 0; c < 16; c++) {
		for (d = 0; d < 16; d++) {
			arm.reg[15] = d << 28;
			switch (c) {
			case 0:  /* EQ */ exec = ZFSET; break;
			case 1:  /* NE */ exec = !ZFSET; break;
			case 2:  /* CS */ exec = CFSET; break;
			case 3:  /* CC */ exec = !CFSET; break;
			case 4:  /* MI */ exec = NFSET; break;
			case 5:  /* PL */ exec = !NFSET; break;
			case 6:  /* VS */ exec = VFSET; break;
			case 7:  /* VC */ exec = !VFSET; break;
			case 8:  /* HI */ exec = (CFSET && !ZFSET); break;
			case 9:  /* LS */ exec = (!CFSET || ZFSET); break;
			case 10: /* GE */ exec = (NFSET == VFSET); break;
			case 11: /* LT */ exec = (NFSET != VFSET); break;
			case 12: /* GT */ exec = (!ZFSET && (NFSET == VFSET)); break;
			case 13: /* LE */ exec = (ZFSET || (NFSET != VFSET)); break;
			case 14: /* AL */ exec = 1; break;
			case 15: /* NV */ exec = 0; break;
			}
			flaglookup[c][d] = (uint8_t) exec;
		}
	}

	jit_deny_init();
}

/**
 * Reset the ARM core to initial state. The CPU model must be selected at this
 * point.
 *
 * @param cpu_model CPU model to emulate
 */
void
arm_reset(CPUModel cpu_model)
{
	memset(&arm, 0, sizeof(arm));

        arm.r15_mask = 0x3fffffc;
        pccache=0xFFFFFFFF;
        updatemode(SUPERVISOR);
        cpsr=15;
//        prog32=1;

        arm.reg[15] = 0x0c000008 | 3;
        arm.reg[16] = SUPERVISOR | 0xd0;
        arm.mode = SUPERVISOR;
        pccache=0xFFFFFFFF;
	if (cpu_model == CPUModel_SA110 || cpu_model == CPUModel_ARM810) {
		arm.r15_diff = 0;
		arm.abort_base_restored = 1;
		arm.stm_writeback_at_end = 1;
		arm.arch_v4 = 1;
	} else {
		arm.r15_diff = 4;
		arm.abort_base_restored = 0;
		arm.stm_writeback_at_end = 0;
		arm.arch_v4 = 0;
	}

	/* See arm_interpret_only. The ARMv3 CPUs are exactly the ones that reset
	   into 26-bit configuration, which is the configuration that storms. */
	arm_interpret_only = arm.arch_v4 ? 0 : 1;
	if (arm_interpret_only) {
		const char *force = getenv("RPCEMU_FORCE_DYNAREC");

		if (force != NULL && *force != '\0' && *force != '0') {
			arm_interpret_only = 0;
			rpclog("CPU: RPCEMU_FORCE_DYNAREC set, recompiling on a 26-bit CPU "
			       "- expect the data abort storm this guards against\n");
		} else {
			rpclog("CPU: interpreting only on this CPU, the recompiler wedges "
			       "26-bit machines (RPCEMU_FORCE_DYNAREC=1 overrides)\n");
		}
	}
}

void
arm_dump(void)
{
	char s[1024];

	sprintf(s, "r0 = %08x    r4 = %08x    r8  = %08x    r12 = %08x\n"
	           "r1 = %08x    r5 = %08x    r9  = %08x    r13 = %08x\n"
	           "r2 = %08x    r6 = %08x    r10 = %08x    r14 = %08x\n"
	           "r3 = %08x    r7 = %08x    r11 = %08x    r15 = %08x\n"
	           "pc = %08x\n"
	           "%s\n",
	           arm.reg[0], arm.reg[4], arm.reg[8], arm.reg[12],
	           arm.reg[1], arm.reg[5], arm.reg[9], arm.reg[13],
	           arm.reg[2], arm.reg[6], arm.reg[10], arm.reg[14],
	           arm.reg[3], arm.reg[7], arm.reg[11], arm.reg[15],
	           PC,
	           mmu ? "MMU enabled" : "MMU disabled");
	rpclog("%s", s);
	printf("%s", s);
}

static uint32_t
shift3(uint32_t opcode)
{
	uint32_t shiftmode = opcode & 0x60;
	uint32_t shiftamount;
	uint32_t temp;
	uint32_t cflag = CFSET;

	if (opcode & 0x10) {
		shiftamount = arm.reg[(opcode >> 8) & 0xf] & 0xff;
	} else {
		shiftamount = (opcode >> 7) & 0x1f;
	}
	temp = arm.reg[RM];
	if (shiftamount != 0) {
		arm.reg[cpsr] &= ~CFLAG;
	}
	switch (shiftmode) {
	case 0: /* LSL */
		if (shiftamount == 0) {
			return temp;
		}
		if (shiftamount == 32) {
			if (temp & 1) {
				arm.reg[cpsr] |= CFLAG;
			}
			return 0;
		}
		if (shiftamount > 32) {
			return 0;
		}
		if ((temp << (shiftamount - 1)) & 0x80000000) {
			arm.reg[cpsr] |= CFLAG;
		}
		return temp << shiftamount;

	case 0x20: /* LSR */
		if (shiftamount == 0 && !(opcode & 0x10)) {
			shiftamount = 32;
		}
		if (shiftamount == 0) {
			return temp;
		}
		if (shiftamount == 32) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			} else {
				arm.reg[cpsr] &= ~CFLAG;
			}
			return 0;
		}
		if (shiftamount > 32) {
			return 0;
		}
		if ((temp >> (shiftamount - 1)) & 1) {
			arm.reg[cpsr] |= CFLAG;
		}
		return temp >> shiftamount;

	case 0x40: /* ASR */
		if (shiftamount == 0) {
			if (opcode & 0x10) {
				return temp;
			}
		}
		if (shiftamount >= 32 || shiftamount == 0) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			} else {
				arm.reg[cpsr] &= ~CFLAG;
			}
			if (temp & 0x80000000) {
				return 0xffffffff;
			}
			return 0;
		}
		if (((int32_t) temp >> (shiftamount - 1)) & 1) {
			arm.reg[cpsr] |= CFLAG;
		}
		return (uint32_t) ((int32_t) temp >> shiftamount);

	default: /* ROR */
		arm.reg[cpsr] &= ~CFLAG;
		if (shiftamount == 0 && !(opcode & 0x10)) {
			/* RRX */
			if (temp & 1) {
				arm.reg[cpsr] |= CFLAG;
			}
			return (cflag << 31) | (temp >> 1);
		}
		if (shiftamount == 0) {
			arm.reg[cpsr] |= (cflag << 29);
			return temp;
		}
		if ((shiftamount & 0x1f) == 0) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			}
			return temp;
		}
		shiftamount &= 0x1f;
		if (rotate_right32(temp, shiftamount) & 0x80000000) {
			arm.reg[cpsr] |= CFLAG;
		}
		return rotate_right32(temp, shiftamount);
	}
}

#define shift(o)  ((o & 0xff0) ? shift3(o) : arm.reg[RM])
#define shift2(o) ((o & 0xff0) ? shift4(o) : arm.reg[RM])
#define shift_ldrstr(o) shift2(o)

static uint32_t
shift5(uint32_t opcode, uint32_t shiftmode, uint32_t shiftamount, uint32_t rm)
{
	switch (shiftmode) {
	case 0: /* LSL */
		if (shiftamount == 0) {
			return rm;
		}
		return 0; /* shiftamount >= 32 */

	case 0x20: /* LSR */
		if (shiftamount == 0 && (opcode & 0x10)) {
			return rm;
		}
		return 0; /* shiftamount >= 32 */

	case 0x40: /* ASR */
		if (shiftamount == 0 && !(opcode & 0x10)) {
			shiftamount = 32;
		}
		if (shiftamount >= 32) {
			if (rm & 0x80000000) {
				return 0xffffffff;
			}
			return 0;
		}
		return (uint32_t) ((int32_t) rm >> shiftamount);

	default: /* ROR */
		if (!(opcode & 0x10)) {
			/* RRX */
			return (CFSET << 31) | (rm >> 1);
		}
		shiftamount &= 0x1f;
		return rotate_right32(rm, shiftamount);
	}
}

static inline uint32_t
shift4(uint32_t opcode)
{
	uint32_t shiftmode = opcode & 0x60;
	uint32_t shiftamount;
	uint32_t rm = arm.reg[RM];

	if (opcode & 0x10) {
		shiftamount = arm.reg[(opcode >> 8) & 0xf] & 0xff;
	} else {
		shiftamount = (opcode >> 7) & 0x1f;
	}

	if ((shiftamount - 1) >= 31) {
		return shift5(opcode, shiftmode, shiftamount, rm);
	}

	switch (shiftmode) {
	case 0: /* LSL */
		return rm << shiftamount;
	case 0x20: /* LSR */
		return rm >> shiftamount;
	case 0x40: /* ASR */
		return (uint32_t) ((int32_t) rm >> shiftamount);
	default: /* ROR */
		return rotate_right32(rm, shiftamount);
	}
}

void
exception(uint32_t mmode, uint32_t address, uint32_t diff)
{
	uint32_t link;
	uint32_t irq_disable;

	/* Debugger: trap/log exceptions before any CPU state is changed */
	debugger_exception_hook(mmode, address, arm.reg[15]);

	/* If FIQ exception, disable FIQ and IRQ, otherwise disable just IRQ */
	if (mmode == FIQ) {
		irq_disable = (0x80 | 0x40);
	} else {
		irq_disable = 0x80;
	}

	link = arm.reg[15] - diff;

	if (ARM_MODE_32(arm.mode)) {
		arm.spsr[mmode] = arm.reg[16];
		updatemode(0x10 | mmode);
		arm.reg[14] = link;
		arm.reg[16] &= ~0x1fu;
		arm.reg[16] |= 0x10 | mmode | irq_disable;
		arm.reg[15] = address;
	} else if (prog32) {
		updatemode(0x10 | mmode);
		arm.reg[14] = link & 0x3fffffc;
		arm.spsr[mmode] = (arm.reg[16] & ~0x1fu) | (link & 3);
		arm.spsr[mmode] &= ~0x10u;
		arm.reg[16] |= irq_disable;
		arm.reg[15] = address;
	} else {
		/* A 26-bit CPU has only User, FIQ, IRQ and Supervisor, so Abort
		   and Undefined exceptions enter mode SVC_26. */
		const uint32_t new_mode = (mmode >= SUPERVISOR) ? SUPERVISOR : mmode;

		updatemode(new_mode);
		arm.reg[14] = link;
		/* Keep N, Z, C, V, I and F; the PC and the mode are replaced.
		   The mode has to be the mode actually entered: this used to set
		   the bits to Supervisor for every exception, so an IRQ or an FIQ
		   left arm.mode saying IRQ/FIQ - which is what the register banks
		   follow - while the guest-visible PSR in R15 said Supervisor.
		   Anything reading its own mode back got the wrong answer, and a
		   mode change derived from R15 would swap R13 and R14 to the
		   wrong bank. Abort and Undefined are unaffected either way,
		   since they do enter Supervisor. */
		arm.reg[15] &= 0xfc000000;
		arm.reg[15] |= (irq_disable << 20) | address | new_mode;
	}
	refillpipeline();
}

/**
 * An instruction with unpredictable behaviour has been encountered.
 *
 * On real hardware these can have very odd behaviour, so log these in case
 * software is depending on them.
 *
 * @param opcode Opcode of instruction being emulated
 */
static void
arm_unpredictable(uint32_t opcode)
{
	if (unpredictable_count != 0) {
		unpredictable_count--;
		rpclog("ARM: Unpredictable opcode %08x at %08x\n", opcode, PC);
	}
}

#if defined(__linux__) || defined(__APPLE__)
/**
 * Grant executable privilege to a region of memory.
 *
 * macOS note: neither code generator calls this any more. Both allocate their
 * code cache with mmap(MAP_JIT) instead, because macOS refuses to make a static
 * array RWX: under Rosetta and under the hardened runtime this function fails
 * with EACCES. See codegen_amd64.c and codegen_arm64.c. It is still used by
 * Linux and by the 32-bit x86 backend.
 *
 * @param ptr Pointer to region of memory
 * @param len Length of region of memory
 */
void
set_memory_executable(void *ptr, size_t len)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	const long page_mask = ~(page_size - 1);
	void *start;
	long end;

	start = (void *) ((long) ptr & page_mask);
	end = ((long) ptr + len + page_size - 1) & page_mask;
	len = (size_t) (end - (long) start);

	if (mprotect(start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		perror("mprotect");
		fatal("mprotect failed making code cache executable: %s", strerror(errno));
	}
}
#elif defined _WIN32
/**
 * Grant executable privilege to a region of memory.
 *
 * The JIT buffer is a static, page-aligned array (W+X, no fault-based paging),
 * so VirtualProtect flips its protection exactly like mprotect does on Linux.
 *
 * @param ptr Pointer to region of memory
 * @param len Length of region of memory
 */
void
set_memory_executable(void *ptr, size_t len)
{
	SYSTEM_INFO si;
	DWORD page_size;
	uintptr_t page_mask;
	void *start;
	uintptr_t end;
	DWORD old_protect;

	GetSystemInfo(&si);
	page_size = si.dwPageSize;
	page_mask = ~((uintptr_t) page_size - 1);

	start = (void *) ((uintptr_t) ptr & page_mask);
	end = ((uintptr_t) ptr + len + page_size - 1) & page_mask;
	len = (size_t) (end - (uintptr_t) start);

	if (!VirtualProtect(start, len, PAGE_EXECUTE_READWRITE, &old_protect)) {
		fatal("VirtualProtect failed: error %lu", (unsigned long) GetLastError());
	}
}
#else
#error "RPCEmu dynarec requires Linux, macOS, or Windows"
#endif

#include "arm_dynarec_ops.h"

static const OpFn opcodes[256] = {
	opANDreg,  opANDregS, opEORreg,  opEORregS, opSUBreg,  opSUBregS, opRSBreg,  opRSBregS, // 00
	opADDreg,  opADDregS, opADCreg,  opADCregS, opSBCreg,  opSBCregS, opRSCreg,  opRSCregS, // 08
	opSWPword, opTSTreg,  opMSRcreg, opTEQreg,  opSWPbyte, opCMPreg,  opMSRsreg, opCMNreg,  // 10
	opORRreg,  opORRregS, opMOVreg,  opMOVregS, opBICreg,  opBICregS, opMVNreg,  opMVNregS, // 18

	opANDimm,  opANDimmS, opEORimm,  opEORimmS, opSUBimm,  opSUBimmS, opRSBimm,  opRSBimmS, // 20
	opADDimm,  opADDimmS, opADCimm,  opADCimmS, opSBCimm,  opSBCimmS, opRSCimm,  opRSCimmS, // 28
	opUNALLOC, opTSTimm,  opMSRcimm, opTEQimm,  opUNALLOC, opCMPimm,  opMSRsimm, opCMNimm,  // 30
	opORRimm,  opORRimmS, opMOVimm,  opMOVimmS, opBICimm,  opBICimmS, opMVNimm,  opMVNimmS, // 38

	opSTR,    opLDR,    opSTRT,   opLDRT,   opSTRB,   opLDRB,   opSTRBT,  opLDRBT,   // 40
	opSTR,    opLDR,    opSTRT,   opLDRT,   opSTRB,   opLDRB,   opSTRBT,  opLDRBT,   // 48
	opSTR,    opLDR,    opSTR,    opLDR,    opSTRB,   opLDRB,   opSTRB,   opLDRB,    // 50
	opSTR,    opLDR,    opSTR,    opLDR,    opSTRB,   opLDRB,   opSTRB,   opLDRB,    // 58

	opSTR,    opLDR,    opSTRT,   opLDRT,   opSTRB,   opLDRB,   opSTRBT,  opLDRBT,   // 60
	opSTR,    opLDR,    opSTRT,   opLDRT,   opSTRB,   opLDRB,   opSTRBT,  opLDRBT,   // 68
	opSTR,    opLDR,    opSTR,    opLDR,    opSTRB,   opLDRB,   opSTRB,   opLDRB,    // 70
	opSTR,    opLDR,    opSTR,    opLDR,    opSTRB,   opLDRB,   opSTRB,   opLDRB,    // 78

	opSTMD,   opLDMD,   opSTMD,   opLDMD,   opSTMDS,  opLDMDS,  opSTMDS,  opLDMDS,   // 80
	opSTMI,   opLDMI,   opSTMI,   opLDMI,   opSTMIS,  opLDMIS,  opSTMIS,  opLDMIS,   // 88
	opSTMD,   opLDMD,   opSTMD,   opLDMD,   opSTMDS,  opLDMDS,  opSTMDS,  opLDMDS,   // 90
	opSTMI,   opLDMI,   opSTMI,   opLDMI,   opSTMIS,  opLDMIS,  opSTMIS,  opLDMIS,   // 98

	opB,      opB,      opB,      opB,      opB,      opB,      opB,      opB,       // a0
	opB,      opB,      opB,      opB,      opB,      opB,      opB,      opB,       // a8
	opBL,     opBL,     opBL,     opBL,     opBL,     opBL,     opBL,     opBL,      // b0
	opBL,     opBL,     opBL,     opBL,     opBL,     opBL,     opBL,     opBL,      // b8

	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   // c0
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   // c8
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   // d0
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   // d8

	opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,     // e0
	opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,     // e8
	opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI,     // f0
	opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI,    opSWI      // f8
};

int linecyc=0;

/**
 * Non-zero to interpret every block instead of recompiling it.
 *
 * Set for the ARMv3-era CPUs - ARM610, ARM710, ARM7500 and ARM7500FE - which are
 * the four that come out of reset in 26-bit program configuration and therefore
 * the four that boot a 26-bit RISC OS. On those, running recompiled blocks wedges
 * the machine in an endless data abort storm: a black screen at 100% CPU, part way
 * through the RISC OS 3.71 boot. How often it fires depends on timing, measured
 * 10 out of 10 launches on an A7000 and 8 out of 10 on a Risc PC ARM610, so it is
 * not every launch and not a clean failure to spot.
 *
 * This is a workaround and not a diagnosis. What is established:
 *
 *  - It is not a regression. A build from before the 26-bit exception fix storms
 *    identically, so it is long standing; our performance work raised the rate at
 *    which it fires rather than introducing it.
 *  - It is not opcode semantics and not per-instruction codegen. Both are covered
 *    by tests/test_jit_26bit.c (8.0M checks) and tests/test_jit_26bit_seq.c (180k
 *    programs, comparing registers, mode and every banked register bank), and both
 *    are clean and mutation-proven.
 *  - Taking this path is what makes the machines work: forcing it by hand booted
 *    an A7000 and an RPC610 cleanly with no aborts, where the recompiled path
 *    stormed on the same build and the same ROM.
 *
 * The cost is speed on the four slowest models RPCEmu emulates. A wedged machine
 * is worth less than a slow one. StrongARM, ARM810, Kinetic and Phoebe are
 * untouched and keep the recompiler, which is every RISC OS 5 configuration.
 *
 * RPCEMU_FORCE_DYNAREC=1 puts the recompiler back for these CPUs, to work on the
 * underlying fault. Resolved once here rather than per block: this is the hot
 * path, and a getenv() on it would be its own performance bug.
 */
int arm_interpret_only;

static inline int
arm_opcode_needs_pc(uint32_t opcode)
{
	// Is this a load, store, branch, co-pro or SWI?
	if (opcode & 0xc000000) {
		return 1;
	}
	// Is this a swap, status register transfer, or unallocated instruction?
	if ((opcode & 0xd900000) == 0x1000000) {
		return 1;
	}
	// Is this a data-processing operation that uses PC?
	if (RN == 15 || RD == 15 || ((opcode & 0x2000000) == 0 && (RM == 15))) {
		return 1;
	}
	// Is this a load/store extension?
	if (arm.arch_v4 && (((opcode & 0xe0000f0) == 0xb0) || ((opcode & 0xe1000d0) == 0x1000d0))) {
		return 1;
	}
	return 0;
}

static inline int
arm_opcode_may_abort(uint32_t opcode)
{
	/* Is this a single or multiple data transfer? */
	if (((opcode + 0x6000000) & 0xf000000) >= 0xa000000) {
		return 1;
	}
	/* Is this a swap? */
	if ((opcode & 0x0fb000f0) == 0x01000090) {
		return 1;
	}
	// Is this a load/store extension?
	if (arm.arch_v4) {
		if (((opcode & 0xe0000f0) == 0xb0) || ((opcode & 0xe1000d0) == 0x1000d0)) {
			return 1;
		}
	}

	return 0;
}

/**
 * Select and return pointer to opcode function.
 *
 * @param opcode Opcode
 * @return Pointer to function
 */
static inline OpFn
arm_opcode_fn(uint32_t opcode)
{
	if (arm.arch_v4) {
		if ((opcode & 0xe0000f0) == 0xb0) {
			// LDRH/STRH
			if (opcode & 0x100000) {
				return opLDRH;
			} else {
				return opSTRH;
			}
		} else if ((opcode & 0xe1000d0) == 0x1000d0) {
			// LDRSB/LDRSH
			if ((opcode & 0xf0) == 0xd0) {
				return opLDRSB;
			} else {
				return opLDRSH;
			}
		}
	}

	return opcodes[(opcode >> 20) & 0xff];
}

/**
 * Execute several ARM instructions.
 *
 * @return A hint roughly proportional to the amount of instructions executed.
 */
int
arm_exec(void)
{
	for (linecyc = 256; linecyc >= 0; linecyc--) {
		if (arm_interpret_only || !isblockvalid(PC) || debugger_hook_active) {
			// Interpret block
			if ((PC >> 12) != pccache) {
				pccache = PC >> 12;
				pccache2 = getpccache(PC);
				if (pccache2 == NULL) {
					// Prefetch Abort
					pccache = 0xffffffff;
					exception(ABORT, 0x10, 4);
					arm.reg[15] += 4;
					continue;
				}
			}
			blockend = 0;
			do {
				const uint32_t opcode = pccache2[PC >> 2];
				const int debug_active = debugger_hook_active;

				if (debug_active) {
					if (debugger_instruction_hook(PC, opcode)) {
						return 1000;
					}
				}

				if ((opcode & 0x0e000000) == 0x0a000000) { blockend = 1; } /* Always end block on branches */
				if ((opcode & 0x0c000000) == 0x0c000000) { blockend = 1; } /* And SWIs and copro stuff */
				if (!(opcode & 0x0c000000) && (RD == 15)) { blockend = 1; } /* End if R15 can be modified */
				if ((opcode & 0x0e108000) == 0x08108000) { blockend = 1; } /* End if R15 reloaded from LDM */
				if ((opcode & 0x0c100000) == 0x04100000 && (RD == 15)) { blockend = 1; } /* End if R15 reloaded from LDR */
				if (flaglookup[opcode >> 28][(*pcpsr) >> 28]) {
					OpFn fn = arm_opcode_fn(opcode);
					fn(opcode);
				}
				if (debug_active) {
					debugger_after_instruction(PC, opcode);
				}
				// if ((opcode & 0x0e000000) == 0x0a000000) blockend = 1; /* Always end block on branches */
				// if ((opcode & 0x0c000000) == 0x0c000000) blockend = 1; /* And SWIs and copro stuff */
				arm.reg[15] += 4;
				if ((PC & 0xffc) == 0) {
					blockend = 1;
				}
				inscount++;
			} while (!blockend && !(arm.event & 0x40));
		} else {
			const uint32_t hash = HASH(PC);
			/* if (pagedirty[PC>>9])
			{
				pagedirty[PC>>9]=0;
				cacheclearpage(PC>>9);
			}
			else */ if (!debugger_hook_active && codeblockpc[hash] == PC &&
			           codeblockword[hash] == (uint8_t) jit_word32) {
				const uint32_t templ = codeblocknum[hash];
				void (*gen_func)(void);

				gen_func = (void *) (&rcodeblock[templ][BLOCKSTART]);
				// gen_func=(void *)(&codeblock[blocks[templ]>>24][blocks[templ]&0xFFF][4]);
				gen_func();
				if (arm.event & 0x40) {
					arm.reg[15] += 4;
				}
				if ((arm.reg[cpsr] & arm.mmask) != arm.mode) {
					updatemode(arm.reg[cpsr] & arm.mmask);
				}
			} else {
				uint32_t opcode;

				if ((PC >> 12) != pccache) {
					pccache = PC >> 12;
					pccache2 = getpccache(PC);
					if (pccache2 == NULL) {
						// Prefetch Abort
						pccache = 0xffffffff;
						exception(ABORT, 0x10, 4);
						arm.reg[15] += 4;
						continue;
					}
				}
				initcodeblock(PC);
				blockend = 0;
				do {
					opcode = pccache2[PC >> 2];
					const int debug_active = debugger_hook_active;

					if (debug_active) {
						if (debugger_instruction_hook(PC, opcode)) {
							return 1000;
						}
					}
					if ((opcode >> 28) == 0xf) {
						// NV condition code
						generatepcinc();
					} else {
						if (arm_opcode_needs_pc(opcode)) {
							generateupdatepc();
						}
						generatepcinc();
						if ((opcode & 0x0e000000) == 0x0a000000) {
							generateupdateinscount();
						}
						if ((opcode >> 28) != 0xe) {
							generateflagtestandbranch(opcode, pcpsr);//,flaglookup);
						} else {
							lastflagchange = 0;
						}
						generatecall(arm_opcode_fn(opcode), opcode, pcpsr);
						if (arm_opcode_may_abort(opcode)) {
							generateirqtest();
						}
						// if ((opcode & 0x0e000000) == 0x0a000000) blockend = 1; /* Always end block on branches */
						if ((opcode & 0x0c000000) == 0x0c000000) blockend = 1; /* And SWIs and copro stuff */
						if (!(opcode & 0x0c000000) && (RD == 15)) blockend = 1; /* End if R15 can be modified */
						if ((opcode & 0x0e108000) == 0x08108000) blockend = 1; /* End if R15 reloaded from LDM */
						if ((opcode & 0x0c100000) == 0x04100000 && (RD == 15)) blockend=1; /* End if R15 reloaded from LDR */
						if (flaglookup[opcode >> 28][(*pcpsr) >> 28]) {
							OpFn fn = arm_opcode_fn(opcode);
							fn(opcode);
						}
					}
					if (debug_active) {
						debugger_after_instruction(PC, opcode);
					}
					arm.reg[15] += 4;
					if ((PC & 0xffc) == 0) {
						blockend = 1;
					}
				} while (!blockend && !(arm.event & 0x40));
				endblock(opcode);
			}
		}

		if (arm.event != 0) {
			if (!ARM_MODE_32(arm.mode)) {
				arm.reg[16] &= ~0xc0u;
				arm.reg[16] |= ((arm.reg[15] & 0xc000000) >> 20);
			}

			if (arm.event & 0x40) {
				// Data Abort
				uint32_t fault_addr = 0;
				uint32_t fault_status = 0;
				static int data_abort_log_budget = 8;

				cp15_get_fault(&fault_addr, &fault_status);
				if (data_abort_log_budget > 0) {
					data_abort_log_budget--;
					rpclog("Data Abort: PC=%08x model=%s mem=%uMB fault_addr=%08x fault_status=%08x\n",
					       arm.reg[15],
					       models[machine.model].name_config,
					       config.mem_size,
					       fault_addr,
					       fault_status);
					rpclog("  regs r0-r7:  %08x %08x %08x %08x %08x %08x %08x %08x\n",
					       arm.reg[0], arm.reg[1], arm.reg[2], arm.reg[3],
					       arm.reg[4], arm.reg[5], arm.reg[6], arm.reg[7]);
					rpclog("  regs r8-r15: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					       arm.reg[8], arm.reg[9], arm.reg[10], arm.reg[11],
					       arm.reg[12], arm.reg[13], arm.reg[14], arm.reg[15]);
					if (data_abort_log_budget == 0) {
						rpclog("Data Abort: further faults will not be logged\n");
					}
				}
				arm.reg[15] -= 4;
				exception(ABORT, 0x14, 0);
				arm.reg[15] += 4;
				arm.event &= ~0x40u;
			} else if ((arm.event & 2) && !(arm.reg[16] & 0x40)) {
				// FIQ
				arm.reg[15] -= 4;
				exception(FIQ, 0x20, 0);
				arm.reg[15] += 4;
			} else if ((arm.event & 1) && !(arm.reg[16] & 0x80)) {
				// IRQ
				arm.reg[15] -= 4;
				exception(IRQ, 0x1c, 0);
				arm.reg[15] += 4;
			}
		}
	}

	return 1000;
}

#ifdef RPCEMU_JIT_TEST
/*
 * Test-only hooks used by tests/test_jit_flags.c to validate the recompiler's
 * native flag generation against the interpreter. Not built into the shipping
 * emulator (guarded by RPCEMU_JIT_TEST).
 */

/**
 * Recompile a single ARM instruction into a fresh JIT block, mirroring the
 * per-instruction sequence used by arm_exec() when building blocks, and return
 * a pointer to the block entry point (callable as void (*)(void)).
 */
void *
codegen_test_compile(uint32_t opcode)
{
	const uint32_t pc = 0x00010000;

	initcodeblock(pc);
	generatepcinc(); /* clears lastjumppos, advances pcinc/inscount */
	if ((opcode >> 28) != 0xe) {
		generateflagtestandbranch(opcode, pcpsr);
	}
	generatecall(arm_opcode_fn(opcode), opcode, pcpsr);
	endblock(opcode);

	return (void *) &rcodeblock[codeblocknum[HASH(pc)]][BLOCKSTART];
}

/**
 * Execute a single ARM instruction through the interpreter opcode handler -
 * the reference implementation the recompiled code must match.
 */
void
codegen_test_interp(uint32_t opcode)
{
	arm_opcode_fn(opcode)(opcode);
}
#endif /* RPCEMU_JIT_TEST */
