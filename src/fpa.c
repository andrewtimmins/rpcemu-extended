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

/*
 * FPA10 floating-point coprocessor emulation.
 *
 * RISC OS pairs an FPA10 with the FPASC support code (shipped as the
 * "FPEmulator" module): the chip does a subset of operations in hardware and
 * traps the rest - transcendentals, packed decimal, and abnormal operands - to
 * software. The support code detects the chip from the FPSR System ID byte
 * (0x81 = hardware FPA10) and, in hardware mode, shuttles register state to and
 * from memory in the 80-bit extended (E) format via LDFE/STFE and LFM/SFM.
 *
 * The registers are therefore held here in the exact 80-bit extended layout
 * (three 32-bit words), so LDFE/STFE/LFM/SFM are byte-exact copies and the
 * support code's register spills round-trip perfectly. Arithmetic is done in
 * host `long double`, which carries at least the 64-bit extended mantissa on
 * every target we build for (x86/x86-64 use the native 80-bit type; AArch64
 * uses 128-bit), so no precision is lost. Operations the real FPA10 does not
 * implement in hardware are raised as undefined instructions so the support
 * code handles them, exactly as on real hardware.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "mem.h"
#include "arm.h"
#include "savestate.h"

/**
 * One FPA register in the FPA10 80-bit extended (E) format (datasheet 5.1.4):
 *   w[0]: bit 31 sign, bits 14..0 the 15-bit exponent (bias 16383)
 *   w[1]: bit 31 the integer bit J, bits 30..0 the top of the fraction
 *   w[2]: the low 32 bits of the fraction
 * This is the format used by LDFE/STFE/LFM/SFM, so holding registers in it
 * makes those transfers exact.
 */
typedef struct {
	uint32_t w[3];
} FPReg;

static FPReg fparegs[8];
static uint32_t fpsr = 0, fpcr = 0;

/* Consume cycles for FPA operations - real FPA is slow */
#define FPA_CYCLES_LOAD_STORE   10
#define FPA_CYCLES_SIMPLE       8    /* MVF, MNF, ABS */
#define FPA_CYCLES_ARITH        20   /* ADD, SUB, MUL */
#define FPA_CYCLES_DIV          70   /* DVF, RDF */
#define FPA_CYCLES_TRANSCEND    150  /* SIN, COS, TAN, etc */
#define FPA_CYCLES_COMPARE      10
#define FPA_CYCLES_FIX_FLT      15

void resetfpa(void)
{
	fpsr = 0x81000000; /* System ID byte 0x81: hardware FPA10 present */
	fpcr = 0;
	memset(fparegs, 0, sizeof(fparegs));
}

#define FD ((opcode >> 12) & 7)
#define FN ((opcode >> 16) & 7)

static const long double fconstants[8] = {
	0.0L, 1.0L, 2.0L, 3.0L, 4.0L, 5.0L, 0.5L, 10.0L
};

/* ---- conversions between the extended register format and host types ----- */

/**
 * Convert an FPA extended register to a host long double. long double holds at
 * least the 64-bit extended mantissa on all supported hosts, so this is exact.
 */
static long double
ext_to_ld(const FPReg *r)
{
	const unsigned sign = r->w[0] >> 31;
	const unsigned exp  = r->w[0] & 0x7fff;
	const uint64_t mant = ((uint64_t) r->w[1] << 32) | r->w[2];
	long double v;

	if (exp == 0x7fff) {
		/* Infinity (fraction 0, integer bit aside) or NaN. */
		if ((mant & 0x7fffffffffffffffULL) == 0) {
			v = HUGE_VALL;
		} else {
			v = nanl("");
		}
	} else if (exp == 0 && mant == 0) {
		v = 0.0L;
	} else {
		/* value = mant * 2^(exp - bias - 63); exp 0 is a denormal (biased 1). */
		int e = (int) (exp ? exp : 1) - 16383 - 63;
		v = ldexpl((long double) mant, e);
	}

	return sign ? -v : v;
}

/**
 * Convert a host long double to the FPA extended register format.
 */
static void
ld_to_ext(long double v, FPReg *r)
{
	const uint32_t sign = signbit(v) ? 0x80000000u : 0u;

	if (isnan(v)) {
		r->w[0] = sign | 0x7fff;
		r->w[1] = 0xc0000000u; /* integer bit + top fraction bit: quiet NaN */
		r->w[2] = 0;
		return;
	}

	v = fabsl(v);

	if (isinf(v)) {
		r->w[0] = sign | 0x7fff;
		r->w[1] = 0x80000000u; /* integer bit set, fraction 0 */
		r->w[2] = 0;
		return;
	}
	if (v == 0.0L) {
		r->w[0] = sign;
		r->w[1] = 0;
		r->w[2] = 0;
		return;
	}

	{
		int e;
		const long double m = frexpl(v, &e); /* v = m * 2^e, m in [0.5, 1) */
		int exp = e + 16382;                  /* mant = m*2^64 so bias shifts by 64-63-1 */

		if (exp >= 0x7fff) {
			/* Overflow to infinity. */
			r->w[0] = sign | 0x7fff;
			r->w[1] = 0x80000000u;
			r->w[2] = 0;
			return;
		}

		if (exp <= 0) {
			/* Denormal (or underflow to zero): shift the mantissa down. */
			const int shift = 1 - exp;
			uint64_t mant;

			if (shift >= 64) {
				r->w[0] = sign;
				r->w[1] = 0;
				r->w[2] = 0;
				return;
			}
			mant = (uint64_t) ldexpl(m, 64) >> shift;
			r->w[0] = sign;
			r->w[1] = (uint32_t) (mant >> 32);
			r->w[2] = (uint32_t) mant;
			return;
		}

		{
			const uint64_t mant = (uint64_t) ldexpl(m, 64); /* in [2^63, 2^64) */
			r->w[0] = sign | (uint32_t) exp;
			r->w[1] = (uint32_t) (mant >> 32);
			r->w[2] = (uint32_t) mant;
		}
	}
}

/* IEEE single/double stored in FPA memory order (most significant word first). */

static long double
single_to_ld(uint32_t w)
{
	float f;

	memcpy(&f, &w, sizeof(f));
	return (long double) f;
}

static uint32_t
ld_to_single(long double v)
{
	float f = (float) v;
	uint32_t w;

	memcpy(&w, &f, sizeof(w));
	return w;
}

static long double
double_to_ld(uint32_t hi, uint32_t lo)
{
	uint64_t bits = ((uint64_t) hi << 32) | lo;
	double d;

	memcpy(&d, &bits, sizeof(d));
	return (long double) d;
}

static void
ld_to_double(long double v, uint32_t *hi, uint32_t *lo)
{
	double d = (double) v;
	uint64_t bits;

	memcpy(&bits, &d, sizeof(bits));
	*hi = (uint32_t) (bits >> 32);
	*lo = (uint32_t) bits;
}

/* ---- packed decimal (P / EP) --------------------------------------------- */

/*
 * Packed decimal is how RISC OS converts between floating point and decimal
 * text (used by BASIC's PRINT and the C library). The FPA10 stores a value as
 * a sign, a BCD exponent and a BCD mantissa normalised to d.ddd... (datasheet
 * 5.1.5): value = +/- mantissa * 10^(+/- exponent). P is three words with a
 * 19-digit mantissa and 4-digit exponent; EP (FPSR.EP set) is four words with a
 * 24-digit mantissa and 7-digit exponent. The exponent field is all-Fs for
 * infinity and NaN.
 */

/**
 * Store an FPA register value to memory as packed decimal (STFP/STFEP).
 */
static void
store_packed(uint32_t addr, long double v, int expanded)
{
	const int nmant = expanded ? 24 : 19;  /* mantissa digits */
	const int nexp  = expanded ? 7 : 4;    /* exponent digits */
	const int words = expanded ? 4 : 3;
	const unsigned msign = signbit(v) ? 1u : 0u;
	int digit[24];
	int expd[7];
	uint32_t w[4] = { 0, 0, 0, 0 };
	int nib[32];        /* whole word set as a nibble stream, MS first */
	int i, e, esign;

	memset(digit, 0, sizeof(digit));
	memset(expd, 0, sizeof(expd));

	v = fabsl(v);

	if (isnan(v) || isinf(v)) {
		/* Exponent all-Fs; NaN carries a non-zero mantissa, infinity zero. */
		for (i = 0; i < nexp; i++) expd[i] = 0xf;
		if (isnan(v)) digit[0] = 9;
		esign = 0;
		e = 0; /* unused - expd already filled */
	} else if (v == 0.0L) {
		esign = 0; /* all zero already */
	} else {
		char buf[80];
		char *p = buf;

		/* "d.ddd...e+/-XX": one digit, then nmant-1 fraction digits. The
		   decimal separator is skipped whatever it is, so a non-C locale
		   (comma) does not matter. */
		snprintf(buf, sizeof(buf), "%.*Le", nmant - 1, v);
		for (i = 0; i < nmant && *p; i++) {
			if (*p < '0' || *p > '9') p++;   /* skip the decimal separator */
			if (*p < '0' || *p > '9') break;
			digit[i] = *p++ - '0';
		}
		while (*p && *p != 'e' && *p != 'E') p++;
		if (*p) p++;                          /* skip 'e' */
		esign = (*p == '-') ? 1 : 0;
		if (*p == '+' || *p == '-') p++;
		e = atoi(p);
		for (i = nexp - 1; i >= 0; i--) { expd[i] = e % 10; e /= 10; }
	}

	/* Lay the value out as a stream of nibbles, most significant first:
	   nibble 0 = sign nibble, then exponent digits, then mantissa digits. */
	for (i = 0; i < words * 8; i++) nib[i] = 0;
	nib[0] = (int) ((msign << 3) | (esign << 2));
	for (i = 0; i < nexp; i++) nib[1 + i] = expd[i];
	for (i = 0; i < nmant; i++) nib[1 + nexp + i] = digit[i];

	for (i = 0; i < words; i++) {
		w[i] = ((uint32_t) nib[i * 8 + 0] << 28) | ((uint32_t) nib[i * 8 + 1] << 24)
		     | ((uint32_t) nib[i * 8 + 2] << 20) | ((uint32_t) nib[i * 8 + 3] << 16)
		     | ((uint32_t) nib[i * 8 + 4] << 12) | ((uint32_t) nib[i * 8 + 5] << 8)
		     | ((uint32_t) nib[i * 8 + 6] << 4)  | ((uint32_t) nib[i * 8 + 7]);
		mem_write32(addr + (uint32_t) (i * 4), w[i]);
	}
}

/**
 * Load a packed decimal value from memory into a host long double (LDFP/LDFEP).
 */
static long double
load_packed(uint32_t addr, int expanded)
{
	const int nmant = expanded ? 24 : 19;
	const int nexp  = expanded ? 7 : 4;
	const int words = expanded ? 4 : 3;
	int nib[32];
	uint32_t w;
	unsigned msign, esign;
	int i, allf = 1, e;
	long double v, mant;

	for (i = 0; i < words; i++) {
		w = mem_read32(addr + (uint32_t) (i * 4));
		nib[i * 8 + 0] = (int) ((w >> 28) & 0xf);
		nib[i * 8 + 1] = (int) ((w >> 24) & 0xf);
		nib[i * 8 + 2] = (int) ((w >> 20) & 0xf);
		nib[i * 8 + 3] = (int) ((w >> 16) & 0xf);
		nib[i * 8 + 4] = (int) ((w >> 12) & 0xf);
		nib[i * 8 + 5] = (int) ((w >> 8) & 0xf);
		nib[i * 8 + 6] = (int) ((w >> 4) & 0xf);
		nib[i * 8 + 7] = (int) (w & 0xf);
	}

	msign = (unsigned) (nib[0] >> 3) & 1u;
	esign = (unsigned) (nib[0] >> 2) & 1u;

	for (i = 0; i < nexp; i++) if (nib[1 + i] != 0xf) allf = 0;
	if (allf) {
		/* Infinity or NaN. */
		int nan = 0;
		for (i = 0; i < nmant; i++) if (nib[1 + nexp + i] != 0) nan = 1;
		v = nan ? nanl("") : HUGE_VALL;
		return msign ? -v : v;
	}

	/* value = mantissa * 10^(exponent - (digits-1)); computed arithmetically so
	   there is no dependence on the C locale's decimal separator. */
	mant = 0.0L;
	for (i = 0; i < nmant; i++) mant = mant * 10.0L + (long double) (nib[1 + nexp + i] & 0xf);
	e = 0;
	for (i = 0; i < nexp; i++) e = e * 10 + (nib[1 + i] & 0xf);
	if (esign) e = -e;

	v = mant * powl(10.0L, (long double) (e - (nmant - 1)));
	return msign ? -v : v;
}

/* ---- operand helpers ----------------------------------------------------- */

/* A data-operation source is either a register (Fm) or a constant (i bit). */
static long double
fpa_operand(uint32_t opcode)
{
	if (opcode & 8) {
		return fconstants[opcode & 7];
	}
	return ext_to_ld(&fparegs[opcode & 7]);
}

static void
fpa_src_reg(uint32_t opcode, FPReg *out)
{
	if (opcode & 8) {
		ld_to_ext(fconstants[opcode & 7], out);
	} else {
		*out = fparegs[opcode & 7];
	}
}

/**
 * Set the ARM condition flags from an FPA compare of op1 against op2, matching
 * the FPA10 definition (N less, Z equal, C greater-or-equal, V unordered) with
 * the AC bit clear (the RISC OS default).
 */
static void
fpa_compare(long double op1, long double op2)
{
	arm.reg[cpsr] &= 0x0fffffff;

	if (isnan(op1) || isnan(op2)) {
		arm.reg[cpsr] |= VFLAG; /* unordered */
	} else if (op1 == op2) {
		arm.reg[cpsr] |= ZFLAG | CFLAG;
	} else if (op1 < op2) {
		arm.reg[cpsr] |= NFLAG;
	} else {
		arm.reg[cpsr] |= CFLAG;
	}
}

/**
 * Raise an ARM Undefined Instruction exception for an FPA opcode the FPA10 does
 * not implement in hardware (transcendentals, packed decimal, and so on). On
 * real hardware these trap to the FPASC support code, which emulates them; the
 * PC fixup matches the coprocessor undefined path in the ARM core.
 */
static void
fpa_undefined(uint32_t opcode)
{
	rpclog("FPA: opcode %08X at %07X not implemented in hardware - trapping to support code\n",
	       opcode, PC);
	arm.reg[15] += 8;
	arm_exception_undefined();
}

/*
 * Instruction types:
 *   Opcodes Cx/Dx, CP1 - LDF/STF
 *   Opcodes Cx/Dx, CP2 - LFM/SFM
 *   Opcodes Ex, bit 4 clear - Data processing
 *   Opcodes Ex, bit 4 set   - Register transfer
 *   Opcodes Ex, bit 4 set, RD=15 - Compare
 */

/* Number of registers transferred by an LFM/SFM, from the length field. */
static int
lfm_sfm_count(uint32_t opcode)
{
	switch (opcode & 0x408000) {
	case 0x008000: return 1;
	case 0x400000: return 2;
	case 0x408000: return 3;
	default:       return 4; /* 0x000000 */
	}
}

void fpaopcode(uint32_t opcode)
{
	uint32_t addr;

	switch ((opcode >> 24) & 0xF) {
	case 0xC:
	case 0xD:
		if (opcode & 0x100) {
			/* LDF/STF: single register, one of five formats. */
			const int load = opcode & 0x100000;

			addr = GETADDR(RN);
			if (opcode & 0x1000000) {
				if (opcode & 0x800000) addr += (opcode & 0xFF) << 2;
				else                   addr -= (opcode & 0xFF) << 2;
			}

			switch (opcode & 0x408000) {
			case 0x000000: /* IEEE single */
				if (load) {
					ld_to_ext(single_to_ld(mem_read32(addr)), &fparegs[FD]);
				} else {
					mem_write32(addr, ld_to_single(ext_to_ld(&fparegs[FD])));
				}
				break;

			case 0x008000: /* IEEE double */
				if (load) {
					uint32_t hi = mem_read32(addr);
					uint32_t lo = mem_read32(addr + 4);
					ld_to_ext(double_to_ld(hi, lo), &fparegs[FD]);
				} else {
					uint32_t hi, lo;
					ld_to_double(ext_to_ld(&fparegs[FD]), &hi, &lo);
					mem_write32(addr, hi);
					mem_write32(addr + 4, lo);
				}
				break;

			case 0x400000: /* IEEE double extended - exact 3-word transfer */
				if (load) {
					fparegs[FD].w[0] = mem_read32(addr);
					fparegs[FD].w[1] = mem_read32(addr + 4);
					fparegs[FD].w[2] = mem_read32(addr + 8);
				} else {
					mem_write32(addr,     fparegs[FD].w[0]);
					mem_write32(addr + 4, fparegs[FD].w[1]);
					mem_write32(addr + 8, fparegs[FD].w[2]);
				}
				break;

			case 0x408000: /* Packed (P) or expanded packed (EP) decimal */
				if (load) {
					ld_to_ext(load_packed(addr, fpsr & 0x800), &fparegs[FD]);
				} else {
					store_packed(addr, ext_to_ld(&fparegs[FD]), fpsr & 0x800);
				}
				break;

			default:
				fpa_undefined(opcode);
				return;
			}

			if (!(opcode & 0x1000000)) {
				if (opcode & 0x800000) addr += (opcode & 0xFF) << 2;
				else                   addr -= (opcode & 0xFF) << 2;
			}
			if (opcode & 0x200000) arm.reg[RN] = addr;
			linecyc -= FPA_CYCLES_LOAD_STORE;
			return;
		} else {
			/* LFM/SFM: 1-4 registers, always in extended format - exact. */
			const int load = opcode & 0x100000;
			const int count = lfm_sfm_count(opcode);
			int i;

			addr = GETADDR(RN);
			if (opcode & 0x1000000) {
				if (opcode & 0x800000) addr += (opcode & 0xFF) << 2;
				else                   addr -= (opcode & 0xFF) << 2;
			}

			for (i = 0; i < count; i++) {
				const int reg = (FD + i) & 7;
				const uint32_t a = addr + (uint32_t) (i * 12);

				if (load) {
					fparegs[reg].w[0] = mem_read32(a);
					fparegs[reg].w[1] = mem_read32(a + 4);
					fparegs[reg].w[2] = mem_read32(a + 8);
				} else {
					mem_write32(a,     fparegs[reg].w[0]);
					mem_write32(a + 4, fparegs[reg].w[1]);
					mem_write32(a + 8, fparegs[reg].w[2]);
				}
			}

			if (!(opcode & 0x1000000)) {
				if (opcode & 0x800000) addr += (opcode & 0xFF) << 2;
				else                   addr -= (opcode & 0xFF) << 2;
			}
			if (opcode & 0x200000) arm.reg[RN] = addr;
			linecyc -= FPA_CYCLES_LOAD_STORE;
			return;
		}

	case 0xE:
		if (opcode & 0x10) {
			/* Register transfer / compare. */
			if (RD == 15 && (opcode & 0x100000)) {
				/* Compare operations. */
				const long double op1 = ext_to_ld(&fparegs[FN]);
				const long double op2 = fpa_operand(opcode);

				switch ((opcode >> 21) & 7) {
				case 4: /* CMF */
				case 6: /* CMFE */
					fpa_compare(op1, op2);
					linecyc -= FPA_CYCLES_COMPARE;
					return;
				case 5: /* CNF */
				case 7: /* CNFE */
					fpa_compare(op1, -op2);
					linecyc -= FPA_CYCLES_COMPARE;
					return;
				}
				fpa_undefined(opcode);
				return;
			}

			switch ((opcode >> 20) & 0xF) {
			case 0: /* FLT - integer to floating point */
				ld_to_ext((long double) (int32_t) arm.reg[RD], &fparegs[FN]);
				linecyc -= FPA_CYCLES_FIX_FLT;
				return;

			case 1: { /* FIX - floating point to integer */
				const long double val = ext_to_ld(&fparegs[opcode & 7]);
				long double r;

				switch ((opcode >> 5) & 3) {
				case 1:  r = ceill(val);  break; /* +infinity */
				case 2:  r = floorl(val); break; /* -infinity */
				case 3:  r = truncl(val); break; /* zero */
				default: r = rintl(val);  break; /* nearest */
				}
				arm.reg[RD] = (uint32_t) (int32_t) r;
				linecyc -= FPA_CYCLES_FIX_FLT;
				return;
			}

			case 2: /* WFS - write floating point status (keep System ID) */
				fpsr = (arm.reg[RD] & 0x00ffffff) | (fpsr & 0xff000000);
				linecyc -= FPA_CYCLES_SIMPLE;
				return;

			case 3: /* RFS - read floating point status */
				arm.reg[RD] = fpsr;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;

			case 4: /* WFC - write floating point control */
				fpcr = arm.reg[RD];
				linecyc -= FPA_CYCLES_SIMPLE;
				return;

			case 5: /* RFC - read floating point control */
				arm.reg[RD] = fpcr;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;
			}
			fpa_undefined(opcode);
			return;
		} else {
			/* Data processing. */
			const long double m = fpa_operand(opcode);
			long double res;

			switch (opcode & 0xF08000) {
			case 0x000000: /* ADF */
				res = ext_to_ld(&fparegs[FN]) + m;
				linecyc -= FPA_CYCLES_ARITH;
				break;
			case 0x100000: /* MUF */
			case 0x900000: /* FML (fast multiply) */
				res = ext_to_ld(&fparegs[FN]) * m;
				linecyc -= FPA_CYCLES_ARITH;
				break;
			case 0x200000: /* SUF */
				res = ext_to_ld(&fparegs[FN]) - m;
				linecyc -= FPA_CYCLES_ARITH;
				break;
			case 0x300000: /* RSF */
				res = m - ext_to_ld(&fparegs[FN]);
				linecyc -= FPA_CYCLES_ARITH;
				break;
			case 0x400000: /* DVF */
			case 0xA00000: /* FDV (fast divide) */
				res = ext_to_ld(&fparegs[FN]) / m;
				linecyc -= FPA_CYCLES_DIV;
				break;
			case 0x500000: /* RDF - reverse divide */
			case 0xB00000: /* FRD (fast reverse divide) */
				res = m / ext_to_ld(&fparegs[FN]);
				linecyc -= FPA_CYCLES_DIV;
				break;
			case 0x600000: /* POW - power */
				res = powl(ext_to_ld(&fparegs[FN]), m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x700000: /* RPW - reverse power */
				res = powl(m, ext_to_ld(&fparegs[FN]));
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x800000: /* RMF - IEEE remainder */
				res = remainderl(ext_to_ld(&fparegs[FN]), m);
				linecyc -= FPA_CYCLES_DIV;
				break;
			case 0xC00000: /* POL - polar angle */
				res = atan2l(ext_to_ld(&fparegs[FN]), m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;

			case 0x008000: { /* MVF - move (exact copy preserves the value) */
				FPReg tmp;
				fpa_src_reg(opcode, &tmp);
				fparegs[FD] = tmp;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;
			}
			case 0x108000: { /* MNF - move negated */
				FPReg tmp;
				fpa_src_reg(opcode, &tmp);
				tmp.w[0] ^= 0x80000000u;
				fparegs[FD] = tmp;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;
			}
			case 0x208000: { /* ABS */
				FPReg tmp;
				fpa_src_reg(opcode, &tmp);
				tmp.w[0] &= 0x7fffffffu;
				fparegs[FD] = tmp;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;
			}
			case 0x308000: /* RND - round to integral value */
				switch ((opcode >> 5) & 3) {
				case 1:  res = ceill(m);  break;
				case 2:  res = floorl(m); break;
				case 3:  res = truncl(m); break;
				default: res = rintl(m);  break;
				}
				linecyc -= FPA_CYCLES_SIMPLE;
				break;
			case 0x408000: /* SQT - square root */
				res = sqrtl(m);
				linecyc -= FPA_CYCLES_DIV;
				break;
			case 0x508000: /* LOG - log base 10 */
				res = log10l(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x608000: /* LGN - log base e */
				res = logl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x708000: /* EXP - exponent */
				res = expl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x808000: /* SIN */
				res = sinl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0x908000: /* COS */
				res = cosl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0xA08000: /* TAN */
				res = tanl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0xB08000: /* ASN - arc sine */
				res = asinl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0xC08000: /* ACS - arc cosine */
				res = acosl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0xD08000: /* ATN - arc tangent */
				res = atanl(m);
				linecyc -= FPA_CYCLES_TRANSCEND;
				break;
			case 0xE08000: /* URD - unnormalised round */
				switch ((opcode >> 5) & 3) {
				case 1:  res = ceill(m);  break;
				case 2:  res = floorl(m); break;
				case 3:  res = truncl(m); break;
				default: res = rintl(m);  break;
				}
				linecyc -= FPA_CYCLES_SIMPLE;
				break;
			case 0xF08000: { /* NRM - normalise (exact move here) */
				FPReg tmp;
				fpa_src_reg(opcode, &tmp);
				fparegs[FD] = tmp;
				linecyc -= FPA_CYCLES_SIMPLE;
				return;
			}
			default:
				fpa_undefined(opcode);
				return;
			}

			ld_to_ext(res, &fparegs[FD]);
			return;
		}
	}

	fpa_undefined(opcode);
}

/**
 * The FPA's state, for the debugger.
 *
 * Worth saying where these values live, because it is not where a RISC OS
 * programmer would expect. On a machine with no FPA the registers belong to
 * FPEmulator and sit in its workspace, and nothing outside the module knows
 * where. Here there is a chip: resetfpa() puts 0x81 in the FPSR system ID byte,
 * the support code reads that, and from then on F0-F7 are the ones below.
 *
 * Two things follow, both of which the debugger has to be honest about.
 *
 * The registers are the chip's, so they are what the machine will use next -
 * but while the support code is servicing an operation the FPA10 does not do
 * in hardware, the working value is in its workspace and these are the
 * operands it was handed. Stopped between instructions, which is where a
 * breakpoint puts you, that distinction does not arise.
 *
 * And the FPSR's low bits are whatever the support code last wrote with WFS.
 * This FPA does not raise the exception flags itself, so do not read them as
 * the chip's account of what has happened. The system ID byte is ours.
 */
void
fpa_get_state(FPADebugState *state)
{
	int c;

	for (c = 0; c < 8; c++) {
		state->reg[c].w[0] = fparegs[c].w[0];
		state->reg[c].w[1] = fparegs[c].w[1];
		state->reg[c].w[2] = fparegs[c].w[2];
		state->reg[c].value = (double) ext_to_ld(&fparegs[c]);
	}
	state->fpsr = fpsr;
	state->fpcr = fpcr;
}

/**
 * Write the FPA state to a suspend snapshot.
 */
void
fpa_savestate(FILE *f)
{
	int c;

	for (c = 0; c < 8; c++) {
		savestate_write_u32(f, fparegs[c].w[0]);
		savestate_write_u32(f, fparegs[c].w[1]);
		savestate_write_u32(f, fparegs[c].w[2]);
	}
	savestate_write_u32(f, fpsr);
	savestate_write_u32(f, fpcr);
}

/**
 * Restore the FPA state from a suspend snapshot.
 */
void
fpa_loadstate(FILE *f)
{
	int c;

	for (c = 0; c < 8; c++) {
		fparegs[c].w[0] = savestate_read_u32(f);
		fparegs[c].w[1] = savestate_read_u32(f);
		fparegs[c].w[2] = savestate_read_u32(f);
	}
	fpsr = savestate_read_u32(f);
	fpcr = savestate_read_u32(f);
}
