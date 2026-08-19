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
 * cpu_z80.c - Zilog Z80, the documented instruction set.
 *
 * What is here and what is not are in cpu_z80.h.
 *
 * ★ DECODED BY THE OPCODE MAP'S OWN STRUCTURE, not as 256 hand-written cases.
 * The Z80's encoding is regular once an opcode is split into the fields it is
 * really made of:
 *
 *   x = op >> 6      the quadrant           z = op & 7        operand or group
 *   y = (op >> 3) & 7    row                p = y >> 1        register pair
 *                                           q = y & 1         direction
 *
 * so `LD r[y],r[z]` is one case covering sixty-three opcodes and `alu[y] r[z]`
 * is one covering sixty-four. That is a fact about the encoding rather than a
 * trick, and it is the difference between a table this can be checked against
 * and a wall nobody will ever read. Register index 6 means (HL) throughout,
 * which is what makes the index-register prefixes tractable as well.
 *
 * ★ THE ONE RULE THAT IS EASY TO GET WRONG. Under a DD or FD prefix, HL becomes
 * IX or IY - but only in one of two ways. If the instruction refers to (HL),
 * that becomes (IX+d) and a displacement byte is fetched, and H and L used
 * elsewhere in the same instruction stay the REAL H and L: `LD H,(IX+d)` loads
 * the true H. If the instruction does not refer to (HL) at all, then H and L
 * become IXH and IXL, which are undocumented and fault here. So every register
 * access is told whether index 6 appears anywhere in this instruction.
 *
 * ★ REPEATING INSTRUCTIONS RE-EXECUTE THEMSELVES. LDIR and its relatives step
 * the program counter back over their own two bytes when they have more to do,
 * which is what the hardware does - and it matters here for a reason the
 * hardware did not care about: a card is given a cycle budget, and a block move
 * of 64K that ran to completion inside one instruction would stall the host for
 * the whole of it. One iteration is one instruction, so a long move is spread
 * across timeslices and stays interruptible.
 */

#include "cpu_z80.h"

#include <stddef.h>

#define FLAG_C	CPU_Z80_FLAG_C
#define FLAG_N	CPU_Z80_FLAG_N
#define FLAG_PV	CPU_Z80_FLAG_PV
#define FLAG_X	CPU_Z80_FLAG_X
#define FLAG_H	CPU_Z80_FLAG_H
#define FLAG_Y	CPU_Z80_FLAG_Y
#define FLAG_Z	CPU_Z80_FLAG_Z
#define FLAG_S	CPU_Z80_FLAG_S

/** The two undocumented flag bits, which are maintained together or not at all. */
#define FLAG_XY	(FLAG_X | FLAG_Y)

/**
 * Per-instruction prefix state.
 *
 * @disp is fetched at most once even when an instruction reads and writes the
 * same (IX+d), which INC (IX+d) does.
 */
typedef struct {
	int prefix;		/**< 0, 0xdd or 0xfd */
	int disp_fetched;
	int8_t disp;
} pfx;

static void
fault(cpu_z80_state *s, uint32_t cause, uint32_t addr)
{
	s->faulted = 1;
	s->fault_cause = cause;
	s->fault_addr = addr;
}

/* ------------------------------------------------------------------ memory */

static uint8_t
rd8(cpu_z80_state *s, uint16_t addr)
{
	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU_Z80_FAULT_ACCESS, addr);
		return 0;
	}
	return s->ram[addr];
}

static void
wr8(cpu_z80_state *s, uint16_t addr, uint8_t val)
{
	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU_Z80_FAULT_ACCESS, addr);
		return;
	}
	s->ram[addr] = val;
}

static uint8_t
fetch8(cpu_z80_state *s)
{
	const uint8_t val = rd8(s, s->pc);

	s->pc = (uint16_t) (s->pc + 1);
	return val;
}

static uint16_t
fetch16(cpu_z80_state *s)
{
	const uint16_t lo = fetch8(s);
	const uint16_t hi = fetch8(s);

	return (uint16_t) (lo | (hi << 8));
}

static uint16_t
rd16(cpu_z80_state *s, uint16_t addr)
{
	const uint16_t lo = rd8(s, addr);
	const uint16_t hi = rd8(s, (uint16_t) (addr + 1));

	return (uint16_t) (lo | (hi << 8));
}

static void
wr16(cpu_z80_state *s, uint16_t addr, uint16_t val)
{
	wr8(s, addr, (uint8_t) val);
	wr8(s, (uint16_t) (addr + 1), (uint8_t) (val >> 8));
}

static void
push16(cpu_z80_state *s, uint16_t val)
{
	s->sp = (uint16_t) (s->sp - 2);
	wr16(s, s->sp, val);
}

static uint16_t
pop16(cpu_z80_state *s)
{
	const uint16_t val = rd16(s, s->sp);

	s->sp = (uint16_t) (s->sp + 2);
	return val;
}

/* ------------------------------------------------------------ input/output */

static uint8_t
io_read(cpu_z80_state *s, uint8_t port)
{
	if (s->io_in != NULL) {
		return s->io_in(s->io_ctx, port);
	}
	return s->ports[port];
}

static void
io_write(cpu_z80_state *s, uint8_t port, uint8_t val)
{
	if (s->io_out != NULL) {
		s->io_out(s->io_ctx, port, val);
		return;
	}
	s->ports[port] = val;
}

/* --------------------------------------------------------------- registers */

static uint16_t
get_bc(const cpu_z80_state *s)
{
	return (uint16_t) ((s->b << 8) | s->c);
}

static uint16_t
get_de(const cpu_z80_state *s)
{
	return (uint16_t) ((s->d << 8) | s->e);
}

static uint16_t
get_hl(const cpu_z80_state *s)
{
	return (uint16_t) ((s->h << 8) | s->l);
}

static void
set_bc(cpu_z80_state *s, uint16_t v)
{
	s->b = (uint8_t) (v >> 8);
	s->c = (uint8_t) v;
}

static void
set_de(cpu_z80_state *s, uint16_t v)
{
	s->d = (uint8_t) (v >> 8);
	s->e = (uint8_t) v;
}

static void
set_hl(cpu_z80_state *s, uint16_t v)
{
	s->h = (uint8_t) (v >> 8);
	s->l = (uint8_t) v;
}

/** HL, or IX/IY when a prefix is in force. Used by the 16-bit register tables. */
static uint16_t
get_index(const cpu_z80_state *s, const pfx *p)
{
	switch (p->prefix) {
	case 0xdd:	return s->ix;
	case 0xfd:	return s->iy;
	default:	return get_hl(s);
	}
}

static void
set_index(cpu_z80_state *s, const pfx *p, uint16_t v)
{
	switch (p->prefix) {
	case 0xdd:	s->ix = v; break;
	case 0xfd:	s->iy = v; break;
	default:	set_hl(s, v); break;
	}
}

/**
 * The address that register index 6 means: (HL), or (IX+d) under a prefix.
 *
 * Fetches the displacement on the first call for this instruction, which is
 * where it appears in the instruction stream, and remembers it.
 */
static uint16_t
idx_addr(cpu_z80_state *s, pfx *p)
{
	if (p->prefix == 0) {
		return get_hl(s);
	}
	if (!p->disp_fetched) {
		p->disp = (int8_t) fetch8(s);
		p->disp_fetched = 1;
	}
	return (uint16_t) (get_index(s, p) + p->disp);
}

/**
 * Read register index @idx.
 *
 * @idx6_present says whether index 6 appears anywhere in this instruction, which
 * decides whether H and L mean the real registers or the undocumented index
 * halves. See the note at the top of this file.
 */
static uint8_t
reg_read(cpu_z80_state *s, pfx *p, unsigned idx, int idx6_present, uint8_t op)
{
	switch (idx) {
	case 0:	return s->b;
	case 1:	return s->c;
	case 2:	return s->d;
	case 3:	return s->e;
	case 4:
	case 5:
		if (p->prefix != 0 && !idx6_present) {
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);	/* IXH / IXL */
			return 0;
		}
		return (idx == 4) ? s->h : s->l;
	case 6:	return rd8(s, idx_addr(s, p));
	default: return s->a;
	}
}

static void
reg_write(cpu_z80_state *s, pfx *p, unsigned idx, int idx6_present, uint8_t op,
          uint8_t val)
{
	switch (idx) {
	case 0:	s->b = val; break;
	case 1:	s->c = val; break;
	case 2:	s->d = val; break;
	case 3:	s->e = val; break;
	case 4:
	case 5:
		if (p->prefix != 0 && !idx6_present) {
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);
			return;
		}
		if (idx == 4) {
			s->h = val;
		} else {
			s->l = val;
		}
		break;
	case 6:	wr8(s, idx_addr(s, p), val); break;
	default: s->a = val; break;
	}
}

/** Register pair table for most instructions: BC, DE, HL/IX/IY, SP. */
static uint16_t
rp_read(cpu_z80_state *s, const pfx *p, unsigned idx)
{
	switch (idx) {
	case 0:	return get_bc(s);
	case 1:	return get_de(s);
	case 2:	return get_index(s, p);
	default: return s->sp;
	}
}

static void
rp_write(cpu_z80_state *s, const pfx *p, unsigned idx, uint16_t val)
{
	switch (idx) {
	case 0:	set_bc(s, val); break;
	case 1:	set_de(s, val); break;
	case 2:	set_index(s, p, val); break;
	default: s->sp = val; break;
	}
}

/** Register pair table for PUSH and POP, where the fourth entry is AF. */
static uint16_t
rp2_read(cpu_z80_state *s, const pfx *p, unsigned idx)
{
	if (idx == 3) {
		return (uint16_t) ((s->a << 8) | s->f);
	}
	return rp_read(s, p, idx);
}

static void
rp2_write(cpu_z80_state *s, const pfx *p, unsigned idx, uint16_t val)
{
	if (idx == 3) {
		s->a = (uint8_t) (val >> 8);
		s->f = (uint8_t) val;
		return;
	}
	rp_write(s, p, idx, val);
}

/* ------------------------------------------------------------------- flags */

/** The parity flag is set when the number of set bits is even. */
static int
parity_even(uint8_t v)
{
	v ^= (uint8_t) (v >> 4);
	v ^= (uint8_t) (v >> 2);
	v ^= (uint8_t) (v >> 1);
	return (v & 1u) == 0u;
}

static void
alu_add(cpu_z80_state *s, uint8_t v, unsigned carry_in)
{
	const unsigned a = s->a;
	const unsigned res = a + v + carry_in;
	const uint8_t r8 = (uint8_t) res;
	uint8_t f = 0;

	if (r8 & 0x80u) {
		f |= FLAG_S;
	}
	if (r8 == 0) {
		f |= FLAG_Z;
	}
	f |= r8 & FLAG_XY;
	if (((a & 0x0fu) + (v & 0x0fu) + carry_in) & 0x10u) {
		f |= FLAG_H;
	}
	/* Overflow: the operands agreed on sign and the result disagrees. */
	if ((~(a ^ v) & (a ^ res) & 0x80u) != 0u) {
		f |= FLAG_PV;
	}
	if (res & 0x100u) {
		f |= FLAG_C;
	}
	s->a = r8;
	s->f = f;
}

/**
 * SUB, SBC and CP, which differ only in whether the result is kept.
 *
 * @store distinguishes them, and also decides where the two undocumented flag
 * bits come from: CP takes them from the operand rather than the result, which
 * is the one place that difference is visible.
 */
static void
alu_sub(cpu_z80_state *s, uint8_t v, unsigned borrow, int store)
{
	const unsigned a = s->a;
	const unsigned res = (a - v - borrow) & 0x1ffu;
	const uint8_t r8 = (uint8_t) res;
	uint8_t f = FLAG_N;

	if (r8 & 0x80u) {
		f |= FLAG_S;
	}
	if (r8 == 0) {
		f |= FLAG_Z;
	}
	f |= (store ? r8 : v) & FLAG_XY;
	if (((a & 0x0fu) - (v & 0x0fu) - borrow) & 0x10u) {
		f |= FLAG_H;
	}
	if (((a ^ v) & (a ^ res) & 0x80u) != 0u) {
		f |= FLAG_PV;
	}
	if (res & 0x100u) {
		f |= FLAG_C;
	}
	if (store) {
		s->a = r8;
	}
	s->f = f;
}

static void
alu_logic(cpu_z80_state *s, uint8_t res, int half_carry)
{
	uint8_t f = 0;

	if (res & 0x80u) {
		f |= FLAG_S;
	}
	if (res == 0) {
		f |= FLAG_Z;
	}
	f |= res & FLAG_XY;
	if (half_carry) {
		f |= FLAG_H;	/* AND and the bit tests set it; OR and XOR clear it */
	}
	if (parity_even(res)) {
		f |= FLAG_PV;
	}
	s->a = res;
	s->f = f;
}

static uint8_t
alu_inc(cpu_z80_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v + 1);
	uint8_t f = (uint8_t) (s->f & FLAG_C);	/* carry is untouched */

	if (r & 0x80u) {
		f |= FLAG_S;
	}
	if (r == 0) {
		f |= FLAG_Z;
	}
	f |= r & FLAG_XY;
	if ((v & 0x0fu) == 0x0fu) {
		f |= FLAG_H;
	}
	if (v == 0x7fu) {
		f |= FLAG_PV;	/* the only value that overflows on increment */
	}
	s->f = f;
	return r;
}

static uint8_t
alu_dec(cpu_z80_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v - 1);
	uint8_t f = (uint8_t) ((s->f & FLAG_C) | FLAG_N);

	if (r & 0x80u) {
		f |= FLAG_S;
	}
	if (r == 0) {
		f |= FLAG_Z;
	}
	f |= r & FLAG_XY;
	if ((v & 0x0fu) == 0u) {
		f |= FLAG_H;
	}
	if (v == 0x80u) {
		f |= FLAG_PV;
	}
	s->f = f;
	return r;
}

/** The eight ALU operations of quadrant 2, in their encoded order. */
static void
alu_op(cpu_z80_state *s, unsigned y, uint8_t v)
{
	const unsigned carry = (s->f & FLAG_C) ? 1u : 0u;

	switch (y) {
	case 0:	alu_add(s, v, 0);			break;	/* ADD */
	case 1:	alu_add(s, v, carry);			break;	/* ADC */
	case 2:	alu_sub(s, v, 0, 1);			break;	/* SUB */
	case 3:	alu_sub(s, v, carry, 1);		break;	/* SBC */
	case 4:	alu_logic(s, (uint8_t) (s->a & v), 1);	break;	/* AND */
	case 5:	alu_logic(s, (uint8_t) (s->a ^ v), 0);	break;	/* XOR */
	case 6:	alu_logic(s, (uint8_t) (s->a | v), 0);	break;	/* OR */
	default: alu_sub(s, v, 0, 0);			break;	/* CP */
	}
}

/** ADD HL,ss - which leaves sign, zero and overflow alone. */
static uint16_t
alu_add16(cpu_z80_state *s, uint16_t a, uint16_t b)
{
	const unsigned res = (unsigned) a + (unsigned) b;
	uint8_t f = (uint8_t) (s->f & (FLAG_S | FLAG_Z | FLAG_PV));

	if (((a & 0x0fffu) + (b & 0x0fffu)) & 0x1000u) {
		f |= FLAG_H;
	}
	if (res & 0x10000u) {
		f |= FLAG_C;
	}
	f |= (uint8_t) ((res >> 8) & FLAG_XY);
	s->f = f;
	return (uint16_t) res;
}

/** ADC HL,ss and SBC HL,ss, which unlike ADD HL,ss set every flag. */
static void
alu_adc16(cpu_z80_state *s, uint16_t v, int subtract)
{
	const unsigned hl = get_hl(s);
	const unsigned carry = (s->f & FLAG_C) ? 1u : 0u;
	unsigned res;
	uint8_t f;

	if (subtract) {
		res = (hl - v - carry) & 0x1ffffu;
		f = FLAG_N;
		if (((hl & 0x0fffu) - (v & 0x0fffu) - carry) & 0x1000u) {
			f |= FLAG_H;
		}
		if (((hl ^ v) & (hl ^ res) & 0x8000u) != 0u) {
			f |= FLAG_PV;
		}
	} else {
		res = hl + v + carry;
		f = 0;
		if (((hl & 0x0fffu) + (v & 0x0fffu) + carry) & 0x1000u) {
			f |= FLAG_H;
		}
		if ((~(hl ^ v) & (hl ^ res) & 0x8000u) != 0u) {
			f |= FLAG_PV;
		}
	}

	if (res & 0x10000u) {
		f |= FLAG_C;
	}
	if ((uint16_t) res == 0) {
		f |= FLAG_Z;
	}
	if (res & 0x8000u) {
		f |= FLAG_S;
	}
	f |= (uint8_t) ((res >> 8) & FLAG_XY);

	set_hl(s, (uint16_t) res);
	s->f = f;
}

/**
 * DAA, which corrects the accumulator after a packed-BCD add or subtract.
 *
 * It reads the half-carry and add/subtract flags to know what it is correcting,
 * which is the reason those flags exist at all.
 */
static void
alu_daa(cpu_z80_state *s)
{
	const uint8_t a = s->a;
	const int subtract = (s->f & FLAG_N) != 0;
	const int carry_in = (s->f & FLAG_C) != 0;
	const int half_in = (s->f & FLAG_H) != 0;
	uint8_t adjust = 0;
	uint8_t res;
	uint8_t f = (uint8_t) (subtract ? FLAG_N : 0);

	if (half_in || (a & 0x0fu) > 9u) {
		adjust |= 0x06u;
	}
	if (carry_in || a > 0x99u) {
		adjust |= 0x60u;
		f |= FLAG_C;
	}

	res = subtract ? (uint8_t) (a - adjust) : (uint8_t) (a + adjust);

	if (subtract) {
		if (half_in && (a & 0x0fu) < 6u) {
			f |= FLAG_H;
		}
	} else if ((a & 0x0fu) > 9u) {
		f |= FLAG_H;
	}

	if (res & 0x80u) {
		f |= FLAG_S;
	}
	if (res == 0) {
		f |= FLAG_Z;
	}
	if (parity_even(res)) {
		f |= FLAG_PV;
	}
	f |= res & FLAG_XY;

	s->a = res;
	s->f = f;
}

/** The eight rotate and shift operations of the CB page, in encoded order. */
static uint8_t
cb_shift(cpu_z80_state *s, unsigned y, uint8_t v, uint8_t op)
{
	uint8_t r;
	uint8_t carry_out;
	uint8_t f = 0;

	switch (y) {
	case 0:	/* RLC */
		carry_out = (uint8_t) ((v >> 7) & 1u);
		r = (uint8_t) ((v << 1) | carry_out);
		break;
	case 1:	/* RRC */
		carry_out = (uint8_t) (v & 1u);
		r = (uint8_t) ((v >> 1) | (carry_out << 7));
		break;
	case 2:	/* RL */
		carry_out = (uint8_t) ((v >> 7) & 1u);
		r = (uint8_t) ((v << 1) | ((s->f & FLAG_C) ? 1u : 0u));
		break;
	case 3:	/* RR */
		carry_out = (uint8_t) (v & 1u);
		r = (uint8_t) ((v >> 1) | ((s->f & FLAG_C) ? 0x80u : 0u));
		break;
	case 4:	/* SLA */
		carry_out = (uint8_t) ((v >> 7) & 1u);
		r = (uint8_t) (v << 1);
		break;
	case 5:	/* SRA - arithmetic, so the sign bit is kept */
		carry_out = (uint8_t) (v & 1u);
		r = (uint8_t) ((v >> 1) | (v & 0x80u));
		break;
	case 6:
		/* SLL, which shifts left and sets bit 0. Undocumented: Zilog never
		   published it and no assembler of the period emitted it, so a
		   program reaching it is far more likely to be lost than to want
		   it. */
		fault(s, CPU_Z80_FAULT_ILLEGAL, op);
		return v;
	default: /* SRL */
		carry_out = (uint8_t) (v & 1u);
		r = (uint8_t) (v >> 1);
		break;
	}

	if (r & 0x80u) {
		f |= FLAG_S;
	}
	if (r == 0) {
		f |= FLAG_Z;
	}
	f |= r & FLAG_XY;
	if (parity_even(r)) {
		f |= FLAG_PV;
	}
	if (carry_out) {
		f |= FLAG_C;
	}
	s->f = f;
	return r;
}

/** BIT b,r - a test, so nothing is written back. */
static void
cb_bit(cpu_z80_state *s, unsigned bit, uint8_t v)
{
	const int set = (v & (1u << bit)) != 0;
	uint8_t f = (uint8_t) ((s->f & FLAG_C) | FLAG_H);

	if (!set) {
		/* Zero and parity both report "the bit was clear". */
		f |= FLAG_Z | FLAG_PV;
	}
	if (bit == 7 && set) {
		f |= FLAG_S;
	}
	f |= v & FLAG_XY;
	s->f = f;
}

/** The A-register rotates, which unlike the CB page leave S, Z and P alone. */
static void
rot_a(cpu_z80_state *s, unsigned y)
{
	const uint8_t a = s->a;
	uint8_t r;
	uint8_t carry_out;

	switch (y) {
	case 0:	/* RLCA */
		carry_out = (uint8_t) ((a >> 7) & 1u);
		r = (uint8_t) ((a << 1) | carry_out);
		break;
	case 1:	/* RRCA */
		carry_out = (uint8_t) (a & 1u);
		r = (uint8_t) ((a >> 1) | (carry_out << 7));
		break;
	case 2:	/* RLA */
		carry_out = (uint8_t) ((a >> 7) & 1u);
		r = (uint8_t) ((a << 1) | ((s->f & FLAG_C) ? 1u : 0u));
		break;
	default: /* RRA */
		carry_out = (uint8_t) (a & 1u);
		r = (uint8_t) ((a >> 1) | ((s->f & FLAG_C) ? 0x80u : 0u));
		break;
	}

	s->a = r;
	s->f = (uint8_t) ((s->f & (FLAG_S | FLAG_Z | FLAG_PV)) |
	                  (carry_out ? FLAG_C : 0) | (r & FLAG_XY));
}

/** Is condition @y true? NZ, Z, NC, C, PO, PE, P, M. */
static int
condition(const cpu_z80_state *s, unsigned y)
{
	switch (y) {
	case 0:	return (s->f & FLAG_Z) == 0;
	case 1:	return (s->f & FLAG_Z) != 0;
	case 2:	return (s->f & FLAG_C) == 0;
	case 3:	return (s->f & FLAG_C) != 0;
	case 4:	return (s->f & FLAG_PV) == 0;
	case 5:	return (s->f & FLAG_PV) != 0;
	case 6:	return (s->f & FLAG_S) == 0;
	default: return (s->f & FLAG_S) != 0;
	}
}

/* ----------------------------------------------------------- the ED page */

/**
 * The block instructions: LDI/LDD/LDIR/LDDR, CPI/CPD/CPIR/CPDR,
 * INI/IND/INIR/INDR and OUTI/OUTD/OTIR/OTDR.
 *
 * @y selects the family (4 to 7) and @z the operation, exactly as encoded.
 * Bit 0 of y is the direction: even increments, odd decrements. y being 6 or 7
 * makes it repeat.
 *
 * A repeating instruction that has more to do steps the program counter back
 * over its own two bytes rather than looping here; see the note at the top of
 * this file for why that matters to a card on a cycle budget.
 */
static void
ed_block(cpu_z80_state *s, unsigned y, unsigned z, uint8_t op)
{
	const int step = (y & 1u) ? -1 : 1;
	const int repeat = (y >= 6);
	uint16_t hl = get_hl(s);

	switch (z) {
	case 0: {	/* LDI, LDD, LDIR, LDDR */
		const uint8_t v = rd8(s, hl);
		const uint16_t de = get_de(s);
		const uint16_t bc = (uint16_t) (get_bc(s) - 1u);
		uint8_t f = (uint8_t) (s->f & (FLAG_S | FLAG_Z | FLAG_C));
		const uint8_t n = (uint8_t) (v + s->a);

		wr8(s, de, v);
		set_hl(s, (uint16_t) (hl + step));
		set_de(s, (uint16_t) (de + step));
		set_bc(s, bc);

		if (bc != 0) {
			f |= FLAG_PV;	/* "there is more to move" */
		}
		/* The undocumented bits come from the byte moved plus the
		   accumulator, in an order of their own. */
		if (n & 0x08u) {
			f |= FLAG_X;
		}
		if (n & 0x02u) {
			f |= FLAG_Y;
		}
		s->f = f;

		if (repeat && bc != 0) {
			s->pc = (uint16_t) (s->pc - 2u);
		}
		break;
	}

	case 1: {	/* CPI, CPD, CPIR, CPDR */
		const uint8_t v = rd8(s, hl);
		const uint16_t bc = (uint16_t) (get_bc(s) - 1u);
		const unsigned diff = (unsigned) s->a - (unsigned) v;
		const uint8_t r8 = (uint8_t) diff;
		uint8_t f = (uint8_t) ((s->f & FLAG_C) | FLAG_N);
		uint8_t n;

		set_hl(s, (uint16_t) (hl + step));
		set_bc(s, bc);

		if (r8 & 0x80u) {
			f |= FLAG_S;
		}
		if (r8 == 0) {
			f |= FLAG_Z;
		}
		if (((s->a & 0x0fu) - (v & 0x0fu)) & 0x10u) {
			f |= FLAG_H;
		}
		if (bc != 0) {
			f |= FLAG_PV;
		}
		n = (uint8_t) (r8 - ((f & FLAG_H) ? 1u : 0u));
		if (n & 0x08u) {
			f |= FLAG_X;
		}
		if (n & 0x02u) {
			f |= FLAG_Y;
		}
		s->f = f;

		/* Stops early on a match, which is the whole point of CPIR. */
		if (repeat && bc != 0 && (f & FLAG_Z) == 0) {
			s->pc = (uint16_t) (s->pc - 2u);
		}
		break;
	}

	case 2: {	/* INI, IND, INIR, INDR */
		const uint8_t v = io_read(s, s->c);

		wr8(s, hl, v);
		set_hl(s, (uint16_t) (hl + step));
		s->b = (uint8_t) (s->b - 1u);

		/* Only the documented flags: zero says the counter reached nought,
		   and the add/subtract flag is set. The rest of what a real part
		   does here depends on the value read and is not worth pretending
		   to. */
		s->f = (uint8_t) ((s->f & FLAG_C) | FLAG_N |
		                  ((s->b == 0) ? FLAG_Z : 0) |
		                  ((s->b & 0x80u) ? FLAG_S : 0));

		if (repeat && s->b != 0) {
			s->pc = (uint16_t) (s->pc - 2u);
		}
		break;
	}

	default: {	/* OUTI, OUTD, OTIR, OTDR */
		const uint8_t v = rd8(s, hl);

		io_write(s, s->c, v);
		set_hl(s, (uint16_t) (hl + step));
		s->b = (uint8_t) (s->b - 1u);

		s->f = (uint8_t) ((s->f & FLAG_C) | FLAG_N |
		                  ((s->b == 0) ? FLAG_Z : 0) |
		                  ((s->b & 0x80u) ? FLAG_S : 0));

		if (repeat && s->b != 0) {
			s->pc = (uint16_t) (s->pc - 2u);
		}
		break;
	}
	}

	(void) op;
}

/** RRD and RLD, which rotate a BCD digit between the accumulator and (HL). */
static void
ed_nibble_rotate(cpu_z80_state *s, int left)
{
	const uint16_t hl = get_hl(s);
	const uint8_t m = rd8(s, hl);
	uint8_t a = s->a;
	uint8_t res;
	uint8_t f = (uint8_t) (s->f & FLAG_C);

	if (left) {
		/* RLD: A low <- (HL) high <- (HL) low <- A low */
		res = (uint8_t) ((m << 4) | (a & 0x0fu));
		a = (uint8_t) ((a & 0xf0u) | (m >> 4));
	} else {
		/* RRD: A low -> (HL) high, (HL) low -> A low */
		res = (uint8_t) ((a << 4) | (m >> 4));
		a = (uint8_t) ((a & 0xf0u) | (m & 0x0fu));
	}

	wr8(s, hl, res);
	s->a = a;

	if (a & 0x80u) {
		f |= FLAG_S;
	}
	if (a == 0) {
		f |= FLAG_Z;
	}
	if (parity_even(a)) {
		f |= FLAG_PV;
	}
	f |= a & FLAG_XY;
	s->f = f;
}

/** The ED page. Anything not defined here faults rather than doing nothing. */
static void
ed_page(cpu_z80_state *s, pfx *p)
{
	const uint8_t op = fetch8(s);
	const unsigned x = op >> 6;
	const unsigned y = (op >> 3) & 7u;
	const unsigned z = op & 7u;
	const unsigned q = y & 1u;
	const unsigned rp = y >> 1;

	/* An index prefix followed by ED is not a defined encoding: the ED page
	   has no (HL) references for a prefix to redirect. */
	if (p->prefix != 0) {
		fault(s, CPU_Z80_FAULT_ILLEGAL, op);
		return;
	}

	if (x == 2) {
		if (y >= 4 && z <= 3) {
			ed_block(s, y, z, op);
		} else {
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);
		}
		return;
	}

	if (x != 1) {
		fault(s, CPU_Z80_FAULT_ILLEGAL, op);
		return;
	}

	switch (z) {
	case 0:
		/* IN r[y],(C). y == 6 has no destination register - it performs
		   the input and keeps only the flags, which is documented as
		   IN F,(C) in some assemblers. */
		{
			const uint8_t v = io_read(s, s->c);
			uint8_t f = (uint8_t) (s->f & FLAG_C);

			if (v & 0x80u) {
				f |= FLAG_S;
			}
			if (v == 0) {
				f |= FLAG_Z;
			}
			if (parity_even(v)) {
				f |= FLAG_PV;
			}
			f |= v & FLAG_XY;
			s->f = f;

			if (y != 6) {
				reg_write(s, p, y, 0, op, v);
			}
		}
		break;

	case 1:
		/* OUT (C),r[y]. y == 6 would be OUT (C),0, which is
		   undocumented and differs between NMOS and CMOS parts. */
		if (y == 6) {
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);
			return;
		}
		io_write(s, s->c, reg_read(s, p, y, 0, op));
		break;

	case 2:	/* SBC HL,rp / ADC HL,rp */
		alu_adc16(s, rp_read(s, p, rp), q == 0);
		break;

	case 3:	/* LD (nn),rp / LD rp,(nn) */
		{
			const uint16_t addr = fetch16(s);

			if (q == 0) {
				wr16(s, addr, rp_read(s, p, rp));
			} else {
				rp_write(s, p, rp, rd16(s, addr));
			}
		}
		break;

	case 4:	/* NEG */
		{
			const uint8_t v = s->a;

			s->a = 0;
			alu_sub(s, v, 0, 1);
		}
		break;

	case 5:	/* RETN, and RETI at y == 1 */
		/* Nothing here raises an interrupt, so restoring the enable latch
		   has no observable effect; it is done because a program that
		   reads IFF2 back through LD A,I should see the truth. */
		s->iff1 = s->iff2;
		s->pc = pop16(s);
		break;

	case 6:	/* IM 0/1/2 */
		switch (y) {
		case 0: case 1: case 4: case 5:	s->im = 0; break;
		case 2: case 6:			s->im = 1; break;
		default:			s->im = 2; break;
		}
		break;

	default: /* z == 7 */
		switch (y) {
		case 0:	s->i = s->a; break;			/* LD I,A */
		case 1:	s->r = s->a; break;			/* LD R,A */

		case 2:	/* LD A,I */
		case 3:	/* LD A,R */
			{
				const uint8_t v = (y == 2) ? s->i : s->r;
				uint8_t f = (uint8_t) (s->f & FLAG_C);

				s->a = v;
				if (v & 0x80u) {
					f |= FLAG_S;
				}
				if (v == 0) {
					f |= FLAG_Z;
				}
				/* Parity carries the interrupt enable latch here,
				   which is the only way to read it. */
				if (s->iff2) {
					f |= FLAG_PV;
				}
				f |= v & FLAG_XY;
				s->f = f;
			}
			break;

		case 4:	ed_nibble_rotate(s, 0); break;		/* RRD */
		case 5:	ed_nibble_rotate(s, 1); break;		/* RLD */

		default:
			/* ED 6E and ED 7E are no-ops on real hardware. Faulting
			   is more use than quietly doing nothing. */
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);
			break;
		}
		break;
	}
}

/** The CB page, including the DDCB and FDCB forms that act on (IX+d). */
static void
cb_page(cpu_z80_state *s, pfx *p)
{
	uint8_t op;
	unsigned x, y, z;
	uint16_t addr = 0;
	uint8_t v;

	/* Under a prefix the displacement comes BEFORE the opcode: DD CB d op. */
	if (p->prefix != 0) {
		p->disp = (int8_t) fetch8(s);
		p->disp_fetched = 1;
	}

	op = fetch8(s);
	x = op >> 6;
	y = (op >> 3) & 7u;
	z = op & 7u;

	if (p->prefix != 0) {
		if (z != 6) {
			/* DDCB with a register operand writes the result to BOTH
			   (IX+d) and that register. Undocumented, and nothing an
			   assembler will emit. */
			fault(s, CPU_Z80_FAULT_ILLEGAL, op);
			return;
		}
		addr = (uint16_t) (get_index(s, p) + p->disp);
		v = rd8(s, addr);
	} else if (z == 6) {
		addr = get_hl(s);
		v = rd8(s, addr);
	} else {
		v = reg_read(s, p, z, 0, op);
	}

	if (s->faulted) {
		return;
	}

	switch (x) {
	case 0:	/* the rotates and shifts */
		v = cb_shift(s, y, v, op);
		break;
	case 1:	/* BIT b,r - a test only, so nothing is written back */
		cb_bit(s, y, v);
		return;
	case 2:	/* RES b,r */
		v = (uint8_t) (v & ~(1u << y));
		break;
	default: /* SET b,r */
		v = (uint8_t) (v | (1u << y));
		break;
	}

	if (s->faulted) {
		return;		/* SLL */
	}

	if (z == 6) {
		wr8(s, addr, v);
	} else {
		reg_write(s, p, z, 0, op, v);
	}
}

/* ------------------------------------------------------------------- main */

/**
 * Execute one instruction from the main page, having already consumed any
 * prefix.
 *
 * @return non-zero if an instruction executed.
 */
static int
main_page(cpu_z80_state *s, pfx *p)
{
	const uint8_t op = fetch8(s);
	const unsigned x = op >> 6;
	const unsigned y = (op >> 3) & 7u;
	const unsigned z = op & 7u;
	const unsigned q = y & 1u;
	const unsigned rp = y >> 1;

	if (s->faulted) {
		return 0;
	}

	switch (x) {
	case 0:
		switch (z) {
		case 0:
			switch (y) {
			case 0:				break;	/* NOP */
			case 1:	{				/* EX AF,AF' */
				uint8_t t = s->a;

				s->a = s->a2;
				s->a2 = t;
				t = s->f;
				s->f = s->f2;
				s->f2 = t;
				break;
			}
			case 2:	{				/* DJNZ d */
				const int8_t d = (int8_t) fetch8(s);

				s->b = (uint8_t) (s->b - 1u);
				if (s->b != 0) {
					s->pc = (uint16_t) (s->pc + d);
				}
				break;
			}
			case 3:	{				/* JR d */
				const int8_t d = (int8_t) fetch8(s);

				s->pc = (uint16_t) (s->pc + d);
				break;
			}
			default: {				/* JR cc,d */
				const int8_t d = (int8_t) fetch8(s);

				if (condition(s, y - 4u)) {
					s->pc = (uint16_t) (s->pc + d);
				}
				break;
			}
			}
			break;

		case 1:
			if (q == 0) {
				rp_write(s, p, rp, fetch16(s));	/* LD rp,nn */
			} else {
				/* ADD HL,rp - and under a prefix, ADD IX,rp,
				   where rp 2 is IX itself rather than HL. */
				set_index(s, p, alu_add16(s, get_index(s, p),
				                          rp_read(s, p, rp)));
			}
			break;

		case 2:
			/* ★ This row INTERLEAVES store and load rather than running
			   all four stores and then all four loads: q, the bottom bit
			   of y, is the direction, so it alternates as y counts up.
			   Grouping them the other way puts LD (nn),A four places from
			   where it belongs, and a test that stored to an absolute
			   address caught exactly that. */
			switch (y) {
			case 0:	wr8(s, get_bc(s), s->a);	break;	/* LD (BC),A */
			case 1:	s->a = rd8(s, get_bc(s));	break;	/* LD A,(BC) */
			case 2:	wr8(s, get_de(s), s->a);	break;	/* LD (DE),A */
			case 3:	s->a = rd8(s, get_de(s));	break;	/* LD A,(DE) */
			case 4:	wr16(s, fetch16(s), get_index(s, p)); break; /* LD (nn),HL */
			case 5:	set_index(s, p, rd16(s, fetch16(s))); break; /* LD HL,(nn) */
			case 6:	wr8(s, fetch16(s), s->a);	break;	/* LD (nn),A */
			default: s->a = rd8(s, fetch16(s));	break;	/* LD A,(nn) */
			}
			break;

		case 3:	/* INC rp / DEC rp - no flags, as 16-bit increments do not set any */
			rp_write(s, p, rp, (uint16_t) (rp_read(s, p, rp) +
			                               ((q == 0) ? 1u : 0xffffu)));
			break;

		case 4:	/* INC r */
			{
				const uint8_t v = reg_read(s, p, y, y == 6, op);

				if (s->faulted) {
					return 0;
				}
				reg_write(s, p, y, y == 6, op, alu_inc(s, v));
			}
			break;

		case 5:	/* DEC r */
			{
				const uint8_t v = reg_read(s, p, y, y == 6, op);

				if (s->faulted) {
					return 0;
				}
				reg_write(s, p, y, y == 6, op, alu_dec(s, v));
			}
			break;

		case 6:	/* LD r,n */
			if (y == 6 && p->prefix != 0) {
				/* LD (IX+d),n: the displacement comes before the
				   immediate, so the address must be resolved
				   first. */
				const uint16_t addr = idx_addr(s, p);

				wr8(s, addr, fetch8(s));
			} else {
				reg_write(s, p, y, y == 6, op, fetch8(s));
			}
			break;

		default: /* z == 7 */
			switch (y) {
			case 0: case 1: case 2: case 3:
				rot_a(s, y);
				break;
			case 4:	alu_daa(s);			break;	/* DAA */
			case 5:	/* CPL */
				s->a = (uint8_t) ~s->a;
				s->f = (uint8_t) ((s->f & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) |
				                  FLAG_H | FLAG_N | (s->a & FLAG_XY));
				break;
			case 6:	/* SCF */
				s->f = (uint8_t) ((s->f & (FLAG_S | FLAG_Z | FLAG_PV)) |
				                  FLAG_C | (s->a & FLAG_XY));
				break;
			default: /* CCF - the old carry becomes the half carry */
				s->f = (uint8_t) ((s->f & (FLAG_S | FLAG_Z | FLAG_PV)) |
				                  ((s->f & FLAG_C) ? FLAG_H : FLAG_C) |
				                  (s->a & FLAG_XY));
				break;
			}
			break;
		}
		break;

	case 1:
		if (y == 6 && z == 6) {
			/* HALT. On real hardware this executes no-ops until an
			   interrupt arrives; nothing can arrive here, so it stops
			   the core and hands back the accumulator. */
			s->halted = 1;
			s->halt_reason = CPU_Z80_HALT_HALT;
			s->exit_code = s->a;
			break;
		}
		/* LD r,r'. If either operand is (HL) then H and L elsewhere in
		   the instruction are the real registers, not the index halves. */
		{
			const int idx6 = (y == 6 || z == 6);
			const uint8_t v = reg_read(s, p, z, idx6, op);

			if (s->faulted) {
				return 0;
			}
			reg_write(s, p, y, idx6, op, v);
		}
		break;

	case 2:	/* alu[y] r[z] */
		{
			const uint8_t v = reg_read(s, p, z, z == 6, op);

			if (s->faulted) {
				return 0;
			}
			alu_op(s, y, v);
		}
		break;

	default: /* x == 3 */
		switch (z) {
		case 0:	/* RET cc */
			if (condition(s, y)) {
				s->pc = pop16(s);
			}
			break;

		case 1:
			if (q == 0) {
				rp2_write(s, p, rp, pop16(s));	/* POP rp2 */
			} else {
				switch (rp) {
				case 0:	s->pc = pop16(s);	break;	/* RET */
				case 1:	{				/* EXX */
					uint16_t t = get_bc(s);

					set_bc(s, (uint16_t) ((s->b2 << 8) | s->c2));
					s->b2 = (uint8_t) (t >> 8);
					s->c2 = (uint8_t) t;
					t = get_de(s);
					set_de(s, (uint16_t) ((s->d2 << 8) | s->e2));
					s->d2 = (uint8_t) (t >> 8);
					s->e2 = (uint8_t) t;
					t = get_hl(s);
					set_hl(s, (uint16_t) ((s->h2 << 8) | s->l2));
					s->h2 = (uint8_t) (t >> 8);
					s->l2 = (uint8_t) t;
					break;
				}
				case 2:	s->pc = get_index(s, p); break;	/* JP (HL) */
				default: s->sp = get_index(s, p); break;/* LD SP,HL */
				}
			}
			break;

		case 2:	/* JP cc,nn */
			{
				const uint16_t addr = fetch16(s);

				if (condition(s, y)) {
					s->pc = addr;
				}
			}
			break;

		case 3:
			switch (y) {
			case 0:	s->pc = fetch16(s);		break;	/* JP nn */
			case 1:	cb_page(s, p);			break;
			case 2:	io_write(s, fetch8(s), s->a);	break;	/* OUT (n),A */
			case 3:	s->a = io_read(s, fetch8(s));	break;	/* IN A,(n) */
			case 4:	{				/* EX (SP),HL */
				const uint16_t t = rd16(s, s->sp);

				wr16(s, s->sp, get_index(s, p));
				set_index(s, p, t);
				break;
			}
			case 5:	{				/* EX DE,HL */
				const uint16_t t = get_de(s);

				/* Always the real HL: this instruction has no
				   prefixed form, DD EB is not EX DE,IX. */
				set_de(s, get_hl(s));
				set_hl(s, t);
				break;
			}
			case 6:	s->iff1 = 0; s->iff2 = 0;	break;	/* DI */
			default: s->iff1 = 1; s->iff2 = 1;	break;	/* EI */
			}
			break;

		case 4:	/* CALL cc,nn */
			{
				const uint16_t addr = fetch16(s);

				if (condition(s, y)) {
					push16(s, s->pc);
					s->pc = addr;
				}
			}
			break;

		case 5:
			if (q == 0) {
				push16(s, rp2_read(s, p, rp));	/* PUSH rp2 */
			} else {
				switch (rp) {
				case 0:	{				/* CALL nn */
					const uint16_t addr = fetch16(s);

					push16(s, s->pc);
					s->pc = addr;
					break;
				}
				default:
					/* DD, ED and FD are prefixes and are
					   consumed before this function is
					   reached, so arriving here means the
					   prefix loop has a hole in it. */
					fault(s, CPU_Z80_FAULT_ILLEGAL, op);
					break;
				}
			}
			break;

		case 6:	/* alu[y] n */
			alu_op(s, y, fetch8(s));
			break;

		default: /* z == 7, RST */
			push16(s, s->pc);
			s->pc = (uint16_t) (y * 8u);
			break;
		}
		break;
	}

	return s->faulted ? 0 : 1;
}

/* --------------------------------------------------------------- interface */

void
cpu_z80_init(cpu_z80_state *s, uint8_t *ram, uint32_t ram_size)
{
	unsigned i;

	cpu_z80_reset(s, 0);
	s->ram = ram;
	s->ram_size = ram_size;
	s->io_in = NULL;
	s->io_out = NULL;
	s->io_ctx = NULL;
	for (i = 0; i < 256; i++) {
		s->ports[i] = 0;
	}
}

void
cpu_z80_reset(cpu_z80_state *s, uint16_t entry)
{
	/* Memory, the input/output hooks and the port latch survive a reset: on
	   real hardware they are outside the processor, and here a card resets
	   its core after the guest has loaded a program into it. */
	s->a = 0xff;
	s->f = 0xff;		/* AF comes up all ones on a real part */
	s->b = 0;
	s->c = 0;
	s->d = 0;
	s->e = 0;
	s->h = 0;
	s->l = 0;
	s->a2 = 0;
	s->f2 = 0;
	s->b2 = 0;
	s->c2 = 0;
	s->d2 = 0;
	s->e2 = 0;
	s->h2 = 0;
	s->l2 = 0;
	s->ix = 0;
	s->iy = 0;
	s->sp = 0xffff;
	s->pc = entry;
	s->i = 0;
	s->r = 0;
	s->iff1 = 0;
	s->iff2 = 0;
	s->im = 0;

	s->halted = 0;
	s->halt_reason = 0;
	s->exit_code = 0;
	s->faulted = 0;
	s->fault_cause = 0;
	s->fault_addr = 0;
	s->cycles = 0;
}

int
cpu_z80_step(cpu_z80_state *s)
{
	pfx p;
	uint8_t next;

	if (s->halted || s->faulted) {
		return 0;
	}

	p.prefix = 0;
	p.disp_fetched = 0;
	p.disp = 0;

	/*
	 * Consume index prefixes. A run of them is legal and only the last one
	 * counts, because each is a complete instruction as far as the fetch
	 * sequence is concerned - DD DD FD 21 is LD IY,nn. ED after a prefix is
	 * handled where it is decoded.
	 */
	for (;;) {
		next = rd8(s, s->pc);
		if (s->faulted) {
			return 0;
		}
		if (next != 0xddu && next != 0xfdu) {
			break;
		}
		p.prefix = next;
		s->pc = (uint16_t) (s->pc + 1);
	}

	if (next == 0xedu) {
		s->pc = (uint16_t) (s->pc + 1);
		ed_page(s, &p);
		if (s->faulted) {
			return 0;
		}
		s->cycles++;
		return 1;
	}

	if (!main_page(s, &p)) {
		return 0;
	}

	s->cycles++;
	return 1;
}

int
cpu_z80_run(cpu_z80_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = cpu_z80_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}

const char *
cpu_z80_fault_name(uint32_t cause)
{
	switch (cause) {
	case CPU_Z80_FAULT_ILLEGAL:	return "undocumented opcode";
	case CPU_Z80_FAULT_ACCESS:	return "access outside the core's memory";
	default:			return "unknown";
	}
}
