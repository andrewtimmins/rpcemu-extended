/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

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
 * cpu_6502.c - MOS 6502, the documented instruction set.
 *
 * What is here and what is not are in cpu_6502.h.
 *
 * Two conventions worth knowing before reading the switch. Every addressing mode
 * is a function returning the EFFECTIVE ADDRESS, immediate mode included - it
 * returns the address of the operand byte and steps the program counter over it -
 * so each instruction is one line, and the mode is named rather than open-coded
 * eight times. And a fault does not unwind: it stops the core, and the registers
 * are left wherever they had got to. That is a card whose window is smaller than
 * the address space, which is a configuration mistake rather than something a
 * program is entitled to recover from.
 */

#include "cpu_6502.h"

#include <stddef.h>

#define FLAG_C	CPU6502_FLAG_C
#define FLAG_Z	CPU6502_FLAG_Z
#define FLAG_I	CPU6502_FLAG_I
#define FLAG_D	CPU6502_FLAG_D
#define FLAG_B	CPU6502_FLAG_B
#define FLAG_U	CPU6502_FLAG_U
#define FLAG_V	CPU6502_FLAG_V
#define FLAG_N	CPU6502_FLAG_N

static void
fault(cpu6502_state *s, uint32_t cause, uint32_t addr)
{
	s->faulted = 1;
	s->fault_cause = cause;
	s->fault_addr = addr;
}

/* ------------------------------------------------------------------ memory */

static uint8_t
rd8(cpu6502_state *s, uint16_t addr)
{
	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6502_FAULT_ACCESS, addr);
		return 0;
	}
	return s->ram[addr];
}

static void
wr8(cpu6502_state *s, uint16_t addr, uint8_t val)
{
	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6502_FAULT_ACCESS, addr);
		return;
	}
	s->ram[addr] = val;
}

static uint8_t
fetch8(cpu6502_state *s)
{
	const uint8_t val = rd8(s, s->pc);

	s->pc = (uint16_t) (s->pc + 1);
	return val;
}

static uint16_t
fetch16(cpu6502_state *s)
{
	const uint16_t lo = fetch8(s);
	const uint16_t hi = fetch8(s);

	return (uint16_t) (lo | (hi << 8));
}

/**
 * Read a 16-bit pointer from the zero page, wrapping inside it.
 *
 * Used by (zp,X) and (zp),Y. The wrap is real hardware behaviour: the high byte
 * of a pointer at $ff comes from $00, not from $0100.
 */
static uint16_t
rd16_zp(cpu6502_state *s, uint8_t zp)
{
	const uint16_t lo = rd8(s, zp);
	const uint16_t hi = rd8(s, (uint8_t) (zp + 1));

	return (uint16_t) (lo | (hi << 8));
}

/**
 * Read the pointer for JMP (indirect), page-crossing bug included.
 *
 * On an NMOS 6502 the second byte is fetched with only the low byte of the
 * address incremented, so JMP ($10ff) takes its high byte from $1000 rather than
 * $1100. Emulated deliberately: it is the best known quirk of the part, code in
 * the wild worked around it, and a 6502 that quietly does the sensible thing
 * instead is not the processor it claims to be. The 65C02 fixed it; this is not
 * a 65C02.
 */
static uint16_t
rd16_jmp_bug(cpu6502_state *s, uint16_t addr)
{
	const uint16_t lo = rd8(s, addr);
	const uint16_t hi = rd8(s, (uint16_t) ((addr & 0xff00u) | ((addr + 1u) & 0x00ffu)));

	return (uint16_t) (lo | (hi << 8));
}

static void
push8(cpu6502_state *s, uint8_t val)
{
	wr8(s, (uint16_t) (0x0100u + s->sp), val);
	s->sp = (uint8_t) (s->sp - 1);
}

static uint8_t
pop8(cpu6502_state *s)
{
	s->sp = (uint8_t) (s->sp + 1);
	return rd8(s, (uint16_t) (0x0100u + s->sp));
}

/* ------------------------------------------------------- addressing modes */

/* Immediate: the operand is the byte the program counter is on. Returning its
   address rather than its value is what lets every read instruction below be one
   line whatever its mode. */
static uint16_t
am_imm(cpu6502_state *s)
{
	const uint16_t addr = s->pc;

	s->pc = (uint16_t) (s->pc + 1);
	return addr;
}

static uint16_t
am_zp(cpu6502_state *s)
{
	return fetch8(s);
}

static uint16_t
am_zpx(cpu6502_state *s)
{
	return (uint8_t) (fetch8(s) + s->x);	/* wraps inside the zero page */
}

static uint16_t
am_zpy(cpu6502_state *s)
{
	return (uint8_t) (fetch8(s) + s->y);
}

static uint16_t
am_abs(cpu6502_state *s)
{
	return fetch16(s);
}

static uint16_t
am_absx(cpu6502_state *s)
{
	return (uint16_t) (fetch16(s) + s->x);
}

static uint16_t
am_absy(cpu6502_state *s)
{
	return (uint16_t) (fetch16(s) + s->y);
}

static uint16_t
am_indx(cpu6502_state *s)
{
	return rd16_zp(s, (uint8_t) (fetch8(s) + s->x));
}

static uint16_t
am_indy(cpu6502_state *s)
{
	return (uint16_t) (rd16_zp(s, fetch8(s)) + s->y);
}

/* ------------------------------------------------------------------- flags */

static void
set_flag(cpu6502_state *s, uint8_t flag, int on)
{
	if (on) {
		s->p |= flag;
	} else {
		s->p = (uint8_t) (s->p & ~flag);
	}
}

/** N and Z from a result, which is most of what the instruction set does. */
static void
set_nz(cpu6502_state *s, uint8_t val)
{
	set_flag(s, FLAG_Z, val == 0);
	set_flag(s, FLAG_N, (val & 0x80u) != 0);
}

/* -------------------------------------------------------------- operations */

static void
op_adc(cpu6502_state *s, uint8_t m)
{
	const unsigned a = s->a;
	const unsigned c = (s->p & FLAG_C) ? 1u : 0u;
	const unsigned bin = a + m + c;

	if (s->p & FLAG_D) {
		/* Decimal mode. The result is the BCD sum; the flags are the part
		   nobody remembers. On an NMOS part Z comes from the BINARY sum,
		   while N and V come from the high nibble as it stands after the
		   low nibble has been corrected and before the high one is - which
		   is why the two corrections are separated below rather than done
		   at the end. The plain BCD results are what the unit test pins
		   down; these flag corners are documented NMOS behaviour and are
		   not the reason anyone runs a 6502 here. */
		unsigned lo = (a & 0x0fu) + (m & 0x0fu) + c;
		unsigned hi = (a >> 4) + (m >> 4);

		if (lo > 9u) {
			lo += 6u;
			hi += 1u;
		}

		set_flag(s, FLAG_Z, (bin & 0xffu) == 0u);
		set_flag(s, FLAG_N, (hi & 0x08u) != 0u);
		set_flag(s, FLAG_V, (((a ^ m) & 0x80u) == 0u) &&
		                    (((a ^ (hi << 4)) & 0x80u) != 0u));

		if (hi > 9u) {
			hi += 6u;
		}
		set_flag(s, FLAG_C, hi > 15u);
		s->a = (uint8_t) ((hi << 4) | (lo & 0x0fu));
		return;
	}

	set_flag(s, FLAG_C, bin > 0xffu);
	/* Overflow is "the operands agreed on sign and the result disagrees". */
	set_flag(s, FLAG_V, (((a ^ m) & 0x80u) == 0u) &&
	                    (((a ^ bin) & 0x80u) != 0u));
	s->a = (uint8_t) bin;
	set_nz(s, s->a);
}

static void
op_sbc(cpu6502_state *s, uint8_t m)
{
	const unsigned a = s->a;
	const unsigned borrow = (s->p & FLAG_C) ? 0u : 1u;
	const unsigned bin = (a - m - borrow) & 0x1ffu;

	/* Unlike ADC, SBC sets exactly the same flags in decimal mode as in
	   binary mode; only the result differs. So the flags are done once. */
	set_flag(s, FLAG_C, (a >= m + borrow));
	set_flag(s, FLAG_V, (((a ^ m) & 0x80u) != 0u) &&
	                    (((a ^ bin) & 0x80u) != 0u));
	set_nz(s, (uint8_t) bin);

	if (s->p & FLAG_D) {
		unsigned lo = (a & 0x0fu) - (m & 0x0fu) - borrow;
		unsigned hi = (a >> 4) - (m >> 4);

		if (lo & 0x10u) {
			lo -= 6u;
			hi -= 1u;
		}
		if (hi & 0x10u) {
			hi -= 6u;
		}
		s->a = (uint8_t) ((hi << 4) | (lo & 0x0fu));
		return;
	}

	s->a = (uint8_t) bin;
}

/** CMP, CPX and CPY: a subtraction whose result is thrown away. */
static void
op_cmp(cpu6502_state *s, uint8_t reg, uint8_t m)
{
	const unsigned diff = (unsigned) reg - (unsigned) m;

	set_flag(s, FLAG_C, reg >= m);
	set_nz(s, (uint8_t) diff);
}

static void
op_bit(cpu6502_state *s, uint8_t m)
{
	set_flag(s, FLAG_Z, (s->a & m) == 0u);
	set_flag(s, FLAG_N, (m & 0x80u) != 0u);
	set_flag(s, FLAG_V, (m & 0x40u) != 0u);
}

static uint8_t
op_asl(cpu6502_state *s, uint8_t v)
{
	set_flag(s, FLAG_C, (v & 0x80u) != 0u);
	v = (uint8_t) (v << 1);
	set_nz(s, v);
	return v;
}

static uint8_t
op_lsr(cpu6502_state *s, uint8_t v)
{
	set_flag(s, FLAG_C, (v & 0x01u) != 0u);
	v = (uint8_t) (v >> 1);
	set_nz(s, v);
	return v;
}

static uint8_t
op_rol(cpu6502_state *s, uint8_t v)
{
	const uint8_t carry_in = (s->p & FLAG_C) ? 1u : 0u;

	set_flag(s, FLAG_C, (v & 0x80u) != 0u);
	v = (uint8_t) ((v << 1) | carry_in);
	set_nz(s, v);
	return v;
}

static uint8_t
op_ror(cpu6502_state *s, uint8_t v)
{
	const uint8_t carry_in = (s->p & FLAG_C) ? 0x80u : 0u;

	set_flag(s, FLAG_C, (v & 0x01u) != 0u);
	v = (uint8_t) ((v >> 1) | carry_in);
	set_nz(s, v);
	return v;
}

static uint8_t
op_inc(cpu6502_state *s, uint8_t v)
{
	v = (uint8_t) (v + 1);
	set_nz(s, v);
	return v;
}

static uint8_t
op_dec(cpu6502_state *s, uint8_t v)
{
	v = (uint8_t) (v - 1);
	set_nz(s, v);
	return v;
}

/** Read-modify-write on memory, which the shifts, INC and DEC all share. */
static void
op_rmw(cpu6502_state *s, uint16_t addr, uint8_t (*fn)(cpu6502_state *, uint8_t))
{
	const uint8_t v = rd8(s, addr);

	wr8(s, addr, fn(s, v));
}

static void
op_branch(cpu6502_state *s, int take)
{
	/* The offset is always consumed, taken or not, or the program counter
	   would be left on it. */
	const uint8_t off = fetch8(s);

	if (take) {
		/* Signed, and the addition wraps at 16 bits as the hardware does. */
		s->pc = (uint16_t) (s->pc + (int8_t) off);
	}
}

/* --------------------------------------------------------------- interface */

void
cpu6502_init(cpu6502_state *s, uint8_t *ram, uint32_t ram_size)
{
	cpu6502_reset(s, 0);
	s->ram = ram;
	s->ram_size = ram_size;
}

void
cpu6502_reset(cpu6502_state *s, uint16_t entry)
{
	uint8_t *ram = s->ram;
	const uint32_t ram_size = s->ram_size;

	s->a = 0;
	s->x = 0;
	s->y = 0;
	s->sp = 0xfd;		/* where a real part's reset sequence leaves it */
	s->p = FLAG_U | FLAG_I;
	s->pc = entry;

	s->halted = 0;
	s->halt_reason = 0;
	s->exit_code = 0;
	s->faulted = 0;
	s->fault_cause = 0;
	s->fault_addr = 0;
	s->cycles = 0;

	s->ram = ram;
	s->ram_size = ram_size;
}

int
cpu6502_step(cpu6502_state *s)
{
	uint8_t op;
	uint16_t addr;

	if (s->halted || s->faulted) {
		return 0;
	}

	op = fetch8(s);
	if (s->faulted) {
		return 0;	/* the program counter left its memory */
	}

	switch (op) {
	/* ------------------------------------------------------------- ADC */
	case 0x69:	op_adc(s, rd8(s, am_imm(s)));	break;
	case 0x65:	op_adc(s, rd8(s, am_zp(s)));	break;
	case 0x75:	op_adc(s, rd8(s, am_zpx(s)));	break;
	case 0x6d:	op_adc(s, rd8(s, am_abs(s)));	break;
	case 0x7d:	op_adc(s, rd8(s, am_absx(s)));	break;
	case 0x79:	op_adc(s, rd8(s, am_absy(s)));	break;
	case 0x61:	op_adc(s, rd8(s, am_indx(s)));	break;
	case 0x71:	op_adc(s, rd8(s, am_indy(s)));	break;

	/* ------------------------------------------------------------- AND */
	case 0x29:	s->a &= rd8(s, am_imm(s));	set_nz(s, s->a); break;
	case 0x25:	s->a &= rd8(s, am_zp(s));	set_nz(s, s->a); break;
	case 0x35:	s->a &= rd8(s, am_zpx(s));	set_nz(s, s->a); break;
	case 0x2d:	s->a &= rd8(s, am_abs(s));	set_nz(s, s->a); break;
	case 0x3d:	s->a &= rd8(s, am_absx(s));	set_nz(s, s->a); break;
	case 0x39:	s->a &= rd8(s, am_absy(s));	set_nz(s, s->a); break;
	case 0x21:	s->a &= rd8(s, am_indx(s));	set_nz(s, s->a); break;
	case 0x31:	s->a &= rd8(s, am_indy(s));	set_nz(s, s->a); break;

	/* ------------------------------------------------------------- ASL */
	case 0x0a:	s->a = op_asl(s, s->a);			break;
	case 0x06:	op_rmw(s, am_zp(s), op_asl);		break;
	case 0x16:	op_rmw(s, am_zpx(s), op_asl);		break;
	case 0x0e:	op_rmw(s, am_abs(s), op_asl);		break;
	case 0x1e:	op_rmw(s, am_absx(s), op_asl);		break;

	/* -------------------------------------------------------- branches */
	case 0x90:	op_branch(s, (s->p & FLAG_C) == 0);	break;	/* BCC */
	case 0xb0:	op_branch(s, (s->p & FLAG_C) != 0);	break;	/* BCS */
	case 0xf0:	op_branch(s, (s->p & FLAG_Z) != 0);	break;	/* BEQ */
	case 0x30:	op_branch(s, (s->p & FLAG_N) != 0);	break;	/* BMI */
	case 0xd0:	op_branch(s, (s->p & FLAG_Z) == 0);	break;	/* BNE */
	case 0x10:	op_branch(s, (s->p & FLAG_N) == 0);	break;	/* BPL */
	case 0x50:	op_branch(s, (s->p & FLAG_V) == 0);	break;	/* BVC */
	case 0x70:	op_branch(s, (s->p & FLAG_V) != 0);	break;	/* BVS */

	/* ------------------------------------------------------------- BIT */
	case 0x24:	op_bit(s, rd8(s, am_zp(s)));		break;
	case 0x2c:	op_bit(s, rd8(s, am_abs(s)));		break;

	/* ------------------------------------------------------------- BRK */
	case 0x00:
		/* Halts, rather than pushing a frame and vectoring through
		   $fffe: there is no operating system here to vector to, and
		   stopping with a result in the accumulator is what a program on
		   a co-processor card wants at its end. See cpu_6502.h.

		   The second byte of BRK is still consumed, because on real
		   hardware it is - BRK is a two-byte instruction whose operand
		   nothing reads - so a program single-stepped past a halt
		   resumes in the right place. */
		(void) fetch8(s);
		s->halted = 1;
		s->halt_reason = CPU6502_HALT_BRK;
		s->exit_code = s->a;
		s->cycles++;
		return 1;

	/* ------------------------------------------------------ flag clears */
	case 0x18:	set_flag(s, FLAG_C, 0);			break;	/* CLC */
	case 0xd8:	set_flag(s, FLAG_D, 0);			break;	/* CLD */
	case 0x58:	set_flag(s, FLAG_I, 0);			break;	/* CLI */
	case 0xb8:	set_flag(s, FLAG_V, 0);			break;	/* CLV */
	case 0x38:	set_flag(s, FLAG_C, 1);			break;	/* SEC */
	case 0xf8:	set_flag(s, FLAG_D, 1);			break;	/* SED */
	case 0x78:	set_flag(s, FLAG_I, 1);			break;	/* SEI */

	/* ------------------------------------------------- CMP / CPX / CPY */
	case 0xc9:	op_cmp(s, s->a, rd8(s, am_imm(s)));	break;
	case 0xc5:	op_cmp(s, s->a, rd8(s, am_zp(s)));	break;
	case 0xd5:	op_cmp(s, s->a, rd8(s, am_zpx(s)));	break;
	case 0xcd:	op_cmp(s, s->a, rd8(s, am_abs(s)));	break;
	case 0xdd:	op_cmp(s, s->a, rd8(s, am_absx(s)));	break;
	case 0xd9:	op_cmp(s, s->a, rd8(s, am_absy(s)));	break;
	case 0xc1:	op_cmp(s, s->a, rd8(s, am_indx(s)));	break;
	case 0xd1:	op_cmp(s, s->a, rd8(s, am_indy(s)));	break;
	case 0xe0:	op_cmp(s, s->x, rd8(s, am_imm(s)));	break;
	case 0xe4:	op_cmp(s, s->x, rd8(s, am_zp(s)));	break;
	case 0xec:	op_cmp(s, s->x, rd8(s, am_abs(s)));	break;
	case 0xc0:	op_cmp(s, s->y, rd8(s, am_imm(s)));	break;
	case 0xc4:	op_cmp(s, s->y, rd8(s, am_zp(s)));	break;
	case 0xcc:	op_cmp(s, s->y, rd8(s, am_abs(s)));	break;

	/* --------------------------------------------------- DEC / DEX / DEY */
	case 0xc6:	op_rmw(s, am_zp(s), op_dec);		break;
	case 0xd6:	op_rmw(s, am_zpx(s), op_dec);		break;
	case 0xce:	op_rmw(s, am_abs(s), op_dec);		break;
	case 0xde:	op_rmw(s, am_absx(s), op_dec);		break;
	case 0xca:	s->x = (uint8_t) (s->x - 1);	set_nz(s, s->x); break;
	case 0x88:	s->y = (uint8_t) (s->y - 1);	set_nz(s, s->y); break;

	/* ------------------------------------------------------------- EOR */
	case 0x49:	s->a ^= rd8(s, am_imm(s));	set_nz(s, s->a); break;
	case 0x45:	s->a ^= rd8(s, am_zp(s));	set_nz(s, s->a); break;
	case 0x55:	s->a ^= rd8(s, am_zpx(s));	set_nz(s, s->a); break;
	case 0x4d:	s->a ^= rd8(s, am_abs(s));	set_nz(s, s->a); break;
	case 0x5d:	s->a ^= rd8(s, am_absx(s));	set_nz(s, s->a); break;
	case 0x59:	s->a ^= rd8(s, am_absy(s));	set_nz(s, s->a); break;
	case 0x41:	s->a ^= rd8(s, am_indx(s));	set_nz(s, s->a); break;
	case 0x51:	s->a ^= rd8(s, am_indy(s));	set_nz(s, s->a); break;

	/* --------------------------------------------------- INC / INX / INY */
	case 0xe6:	op_rmw(s, am_zp(s), op_inc);		break;
	case 0xf6:	op_rmw(s, am_zpx(s), op_inc);		break;
	case 0xee:	op_rmw(s, am_abs(s), op_inc);		break;
	case 0xfe:	op_rmw(s, am_absx(s), op_inc);		break;
	case 0xe8:	s->x = (uint8_t) (s->x + 1);	set_nz(s, s->x); break;
	case 0xc8:	s->y = (uint8_t) (s->y + 1);	set_nz(s, s->y); break;

	/* ------------------------------------------------------- JMP / JSR */
	case 0x4c:	s->pc = am_abs(s);			break;
	case 0x6c:	s->pc = rd16_jmp_bug(s, am_abs(s));	break;

	case 0x20: {
		/* The return address pushed is the last byte of the JSR itself,
		   not the instruction after it, which is why RTS adds one. */
		const uint16_t target = am_abs(s);
		const uint16_t ret = (uint16_t) (s->pc - 1u);

		push8(s, (uint8_t) (ret >> 8));
		push8(s, (uint8_t) ret);
		s->pc = target;
		break;
	}

	/* --------------------------------------------------- LDA / LDX / LDY */
	case 0xa9:	s->a = rd8(s, am_imm(s));	set_nz(s, s->a); break;
	case 0xa5:	s->a = rd8(s, am_zp(s));	set_nz(s, s->a); break;
	case 0xb5:	s->a = rd8(s, am_zpx(s));	set_nz(s, s->a); break;
	case 0xad:	s->a = rd8(s, am_abs(s));	set_nz(s, s->a); break;
	case 0xbd:	s->a = rd8(s, am_absx(s));	set_nz(s, s->a); break;
	case 0xb9:	s->a = rd8(s, am_absy(s));	set_nz(s, s->a); break;
	case 0xa1:	s->a = rd8(s, am_indx(s));	set_nz(s, s->a); break;
	case 0xb1:	s->a = rd8(s, am_indy(s));	set_nz(s, s->a); break;
	case 0xa2:	s->x = rd8(s, am_imm(s));	set_nz(s, s->x); break;
	case 0xa6:	s->x = rd8(s, am_zp(s));	set_nz(s, s->x); break;
	case 0xb6:	s->x = rd8(s, am_zpy(s));	set_nz(s, s->x); break;
	case 0xae:	s->x = rd8(s, am_abs(s));	set_nz(s, s->x); break;
	case 0xbe:	s->x = rd8(s, am_absy(s));	set_nz(s, s->x); break;
	case 0xa0:	s->y = rd8(s, am_imm(s));	set_nz(s, s->y); break;
	case 0xa4:	s->y = rd8(s, am_zp(s));	set_nz(s, s->y); break;
	case 0xb4:	s->y = rd8(s, am_zpx(s));	set_nz(s, s->y); break;
	case 0xac:	s->y = rd8(s, am_abs(s));	set_nz(s, s->y); break;
	case 0xbc:	s->y = rd8(s, am_absx(s));	set_nz(s, s->y); break;

	/* ------------------------------------------------------------- LSR */
	case 0x4a:	s->a = op_lsr(s, s->a);			break;
	case 0x46:	op_rmw(s, am_zp(s), op_lsr);		break;
	case 0x56:	op_rmw(s, am_zpx(s), op_lsr);		break;
	case 0x4e:	op_rmw(s, am_abs(s), op_lsr);		break;
	case 0x5e:	op_rmw(s, am_absx(s), op_lsr);		break;

	/* ------------------------------------------------------------- NOP */
	case 0xea:						break;

	/* ------------------------------------------------------------- ORA */
	case 0x09:	s->a |= rd8(s, am_imm(s));	set_nz(s, s->a); break;
	case 0x05:	s->a |= rd8(s, am_zp(s));	set_nz(s, s->a); break;
	case 0x15:	s->a |= rd8(s, am_zpx(s));	set_nz(s, s->a); break;
	case 0x0d:	s->a |= rd8(s, am_abs(s));	set_nz(s, s->a); break;
	case 0x1d:	s->a |= rd8(s, am_absx(s));	set_nz(s, s->a); break;
	case 0x19:	s->a |= rd8(s, am_absy(s));	set_nz(s, s->a); break;
	case 0x01:	s->a |= rd8(s, am_indx(s));	set_nz(s, s->a); break;
	case 0x11:	s->a |= rd8(s, am_indy(s));	set_nz(s, s->a); break;

	/* ------------------------------------------------------------ stack */
	case 0x48:	push8(s, s->a);				break;	/* PHA */
	case 0x08:
		/* PHP pushes the break flag set, which is the only place that bit
		   is ever visible: it does not exist as a register bit. */
		push8(s, (uint8_t) (s->p | FLAG_B | FLAG_U));
		break;
	case 0x68:	s->a = pop8(s);		set_nz(s, s->a); break;	/* PLA */
	case 0x28:
		/* The unused bit reads as one whatever was pushed, and the break
		   bit is meaningless in the register, so both are forced. */
		s->p = (uint8_t) (pop8(s) | FLAG_U | FLAG_B);
		break;

	/* ------------------------------------------------------- ROL / ROR */
	case 0x2a:	s->a = op_rol(s, s->a);			break;
	case 0x26:	op_rmw(s, am_zp(s), op_rol);		break;
	case 0x36:	op_rmw(s, am_zpx(s), op_rol);		break;
	case 0x2e:	op_rmw(s, am_abs(s), op_rol);		break;
	case 0x3e:	op_rmw(s, am_absx(s), op_rol);		break;
	case 0x6a:	s->a = op_ror(s, s->a);			break;
	case 0x66:	op_rmw(s, am_zp(s), op_ror);		break;
	case 0x76:	op_rmw(s, am_zpx(s), op_ror);		break;
	case 0x6e:	op_rmw(s, am_abs(s), op_ror);		break;
	case 0x7e:	op_rmw(s, am_absx(s), op_ror);		break;

	/* ------------------------------------------------------- RTI / RTS */
	case 0x40:
		/* Nothing here raises an interrupt, so RTI is only useful to a
		   program that pushed a frame itself. It is implemented because
		   leaving a documented opcode out would be a worse surprise. */
		s->p = (uint8_t) (pop8(s) | FLAG_U | FLAG_B);
		addr = pop8(s);
		s->pc = (uint16_t) (addr | (pop8(s) << 8));
		break;

	case 0x60:
		addr = pop8(s);
		s->pc = (uint16_t) (((addr | (pop8(s) << 8)) + 1u) & 0xffffu);
		break;

	/* ------------------------------------------------------------- SBC */
	case 0xe9:	op_sbc(s, rd8(s, am_imm(s)));	break;
	case 0xe5:	op_sbc(s, rd8(s, am_zp(s)));	break;
	case 0xf5:	op_sbc(s, rd8(s, am_zpx(s)));	break;
	case 0xed:	op_sbc(s, rd8(s, am_abs(s)));	break;
	case 0xfd:	op_sbc(s, rd8(s, am_absx(s)));	break;
	case 0xf9:	op_sbc(s, rd8(s, am_absy(s)));	break;
	case 0xe1:	op_sbc(s, rd8(s, am_indx(s)));	break;
	case 0xf1:	op_sbc(s, rd8(s, am_indy(s)));	break;

	/* --------------------------------------------------- STA / STX / STY */
	case 0x85:	wr8(s, am_zp(s), s->a);			break;
	case 0x95:	wr8(s, am_zpx(s), s->a);		break;
	case 0x8d:	wr8(s, am_abs(s), s->a);		break;
	case 0x9d:	wr8(s, am_absx(s), s->a);		break;
	case 0x99:	wr8(s, am_absy(s), s->a);		break;
	case 0x81:	wr8(s, am_indx(s), s->a);		break;
	case 0x91:	wr8(s, am_indy(s), s->a);		break;
	case 0x86:	wr8(s, am_zp(s), s->x);			break;
	case 0x96:	wr8(s, am_zpy(s), s->x);		break;
	case 0x8e:	wr8(s, am_abs(s), s->x);		break;
	case 0x84:	wr8(s, am_zp(s), s->y);			break;
	case 0x94:	wr8(s, am_zpx(s), s->y);		break;
	case 0x8c:	wr8(s, am_abs(s), s->y);		break;

	/* -------------------------------------------------------- transfers */
	case 0xaa:	s->x = s->a;	set_nz(s, s->x);	break;	/* TAX */
	case 0xa8:	s->y = s->a;	set_nz(s, s->y);	break;	/* TAY */
	case 0xba:	s->x = s->sp;	set_nz(s, s->x);	break;	/* TSX */
	case 0x8a:	s->a = s->x;	set_nz(s, s->a);	break;	/* TXA */
	case 0x98:	s->a = s->y;	set_nz(s, s->a);	break;	/* TYA */
	/* TXS is the one transfer that sets no flags. */
	case 0x9a:	s->sp = s->x;				break;

	default:
		/* An undocumented opcode. A real NMOS part does something for
		   most of these, and emulating that would be modelling a defect
		   nobody should rely on here; a program that lands on one has
		   gone wrong, so say so rather than carrying on plausibly. */
		fault(s, CPU6502_FAULT_ILLEGAL, op);
		return 0;
	}

	if (s->faulted) {
		return 0;
	}

	s->cycles++;
	return 1;
}

int
cpu6502_run(cpu6502_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = cpu6502_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}

const char *
cpu6502_fault_name(uint32_t cause)
{
	switch (cause) {
	case CPU6502_FAULT_ILLEGAL:	return "undocumented opcode";
	case CPU6502_FAULT_ACCESS:	return "access outside the core's memory";
	default:			return "unknown";
	}
}
