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
 * cpu_6800.c - a Motorola 6800, 6802 and 6808 for the OPEN Bus co-processor card.
 *
 * Why one core covers three parts, why it is not a flag on the 6809, and which
 * of this processor's quirks are deliberate are all in cpu_6800.h.
 *
 * The shape follows the 6809's: the regular part of the opcode map, &80-&FF, is
 * decoded by taking the opcode apart, and the irregular part is a switch. The
 * grid here is smaller than the 6809's, because this processor has no 16-bit
 * accumulator and so column three is empty in both halves, and because the
 * second half has no comparison and no subroutine call.
 *
 * The one structural difference worth noticing before reading on: an INDEXED
 * instruction on a 6800 costs MORE than the extended form of the same thing,
 * because the offset has to be added to X and a full address does not. On the
 * 6809 it is the other way round. Neither is a mistake in the table.
 */

#include <string.h>

#include "cpu_6800.h"

#define FLAG_C	CPU6800_FLAG_C
#define FLAG_V	CPU6800_FLAG_V
#define FLAG_Z	CPU6800_FLAG_Z
#define FLAG_N	CPU6800_FLAG_N
#define FLAG_I	CPU6800_FLAG_I
#define FLAG_H	CPU6800_FLAG_H

static void
fault(cpu6800_state *s, uint32_t cause, uint32_t addr)
{
	if (!s->faulted) {
		s->faulted = 1;
		s->fault_cause = cause;
		s->fault_addr = addr;
	}
}

/*
 * The cycle counts.
 *
 * ★ DERIVED FROM THE GROUP RULES, NOT TRANSCRIBED, for the reason given in
 * cpu_6809.c: within the grid the cost follows from the addressing mode and the
 * width of the operand, and typing 256 numbers out of a datasheet would put a
 * wrong one somewhere no reader would find. Generated from those rules and then
 * cross-checked against 65 values read off the datasheet independently; all 65
 * agreed, and the table has exactly the 197 valid opcodes the part is documented
 * to have, which is a second check on the same work.
 *
 * A zero means the opcode is not an instruction.
 */
static const uint8_t cycles_6800[256] = {
	 0,  2,  0,  0,  0,  0,  2,  2,  4,  4,  2,  2,  2,  2,  2,  2,	/* 00 */
	 2,  2,  0,  0,  0,  0,  2,  2,  0,  2,  0,  2,  0,  0,  0,  0,	/* 10 */
	 4,  0,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,	/* 20 */
	 4,  4,  4,  4,  4,  4,  4,  4,  0,  5,  0, 10,  0,  0,  9, 12,	/* 30 */
	 2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2,	/* 40 */
	 2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2,	/* 50 */
	 7,  0,  0,  7,  7,  0,  7,  7,  7,  7,  7,  0,  7,  7,  4,  7,	/* 60 */
	 6,  0,  0,  6,  6,  0,  6,  6,  6,  6,  6,  0,  6,  6,  3,  6,	/* 70 */
	 2,  2,  2,  0,  2,  2,  2,  0,  2,  2,  2,  2,  3,  8,  3,  0,	/* 80 */
	 3,  3,  3,  0,  3,  3,  3,  4,  3,  3,  3,  3,  4,  0,  4,  5,	/* 90 */
	 5,  5,  5,  0,  5,  5,  5,  6,  5,  5,  5,  5,  6,  8,  6,  7,	/* a0 */
	 4,  4,  4,  0,  4,  4,  4,  5,  4,  4,  4,  4,  5,  9,  5,  6,	/* b0 */
	 2,  2,  2,  0,  2,  2,  2,  0,  2,  2,  2,  2,  0,  0,  3,  0,	/* c0 */
	 3,  3,  3,  0,  3,  3,  3,  4,  3,  3,  3,  3,  0,  0,  4,  5,	/* d0 */
	 5,  5,  5,  0,  5,  5,  5,  6,  5,  5,  5,  5,  0,  0,  6,  7,	/* e0 */
	 4,  4,  4,  0,  4,  4,  4,  5,  4,  4,  4,  4,  0,  0,  5,  6,	/* f0 */
};

/* ------------------------------------------------------------------ memory */

static uint8_t
rd8(cpu6800_state *s, uint16_t addr)
{
	if (s->mem.read != NULL) {
		uint8_t val = 0;

		switch (s->mem.read(s->mem.ctx, addr, &val)) {
		case CPU_MEM_OK:
			return val;
		case CPU_MEM_STALL:
			s->stalled = 1;
			s->stall_addr = addr;
			s->stall_is_write = 0;
			return 0;
		default:
			fault(s, CPU6800_FAULT_ACCESS, addr);
			return 0;
		}
	}

	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6800_FAULT_ACCESS, addr);
		return 0;
	}
	return s->ram[addr];
}

static void
wr8(cpu6800_state *s, uint16_t addr, uint8_t val)
{
	if (s->mem.write != NULL) {
		switch (s->mem.write(s->mem.ctx, addr, val)) {
		case CPU_MEM_OK:
			return;
		case CPU_MEM_STALL:
			s->stalled = 1;
			s->stall_addr = addr;
			s->stall_is_write = 1;
			return;
		default:
			fault(s, CPU6800_FAULT_ACCESS, addr);
			return;
		}
	}

	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6800_FAULT_ACCESS, addr);
		return;
	}
	s->ram[addr] = val;
}

/* Big-endian, as the 6809 is: the high byte of a word sits at the lower address. */
static uint16_t
rd16(cpu6800_state *s, uint16_t addr)
{
	const uint8_t hi = rd8(s, addr);
	const uint8_t lo = rd8(s, (uint16_t) (addr + 1));

	return (uint16_t) ((hi << 8) | lo);
}

static void
wr16(cpu6800_state *s, uint16_t addr, uint16_t val)
{
	wr8(s, addr, (uint8_t) (val >> 8));
	wr8(s, (uint16_t) (addr + 1), (uint8_t) val);
}

static uint8_t
fetch8(cpu6800_state *s)
{
	const uint8_t val = rd8(s, s->pc);

	s->pc = (uint16_t) (s->pc + 1);
	return val;
}

static uint16_t
fetch16(cpu6800_state *s)
{
	const uint16_t val = rd16(s, s->pc);

	s->pc = (uint16_t) (s->pc + 2);
	return val;
}

/* ------------------------------------------------------------------- stack */

/*
 * ★ THE STACK POINTER ADDRESSES THE NEXT FREE BYTE, one below the top of the
 * stack, so a push stores and THEN decrements where the 6809 decrements first.
 * The off-by-one is not an implementation detail to hide: it is visible in the
 * instruction set, since TSX loads X with SP plus one and TXS sets SP to X minus
 * one. Getting it backwards would put every pushed byte one address out and a
 * subroutine would still appear to work, because a matched push and pull agree
 * with each other whichever convention they share.
 */
static void
push8(cpu6800_state *s, uint8_t val)
{
	wr8(s, s->sp, val);
	s->sp = (uint16_t) (s->sp - 1);
}

static uint8_t
pull8(cpu6800_state *s)
{
	s->sp = (uint16_t) (s->sp + 1);
	return rd8(s, s->sp);
}

static void
push16(cpu6800_state *s, uint16_t val)
{
	push8(s, (uint8_t) val);		/* low byte first, so it ends higher */
	push8(s, (uint8_t) (val >> 8));
}

static uint16_t
pull16(cpu6800_state *s)
{
	const uint8_t hi = pull8(s);
	const uint8_t lo = pull8(s);

	return (uint16_t) ((hi << 8) | lo);
}

/* -------------------------------------------------------------------- flags */

static void
set_flag(cpu6800_state *s, uint8_t bit, int on)
{
	if (on) {
		s->cc |= bit;
	} else {
		s->cc = (uint8_t) (s->cc & ~bit);
	}
}

static int
flag(const cpu6800_state *s, uint8_t bit)
{
	return (s->cc & bit) != 0;
}

static void
set_nz8(cpu6800_state *s, uint8_t v)
{
	set_flag(s, FLAG_N, (v & 0x80u) != 0);
	set_flag(s, FLAG_Z, v == 0);
}

static void
set_nz16(cpu6800_state *s, uint16_t v)
{
	set_flag(s, FLAG_N, (v & 0x8000u) != 0);
	set_flag(s, FLAG_Z, v == 0);
}

/* ---------------------------------------------------------------------- ALU */

static uint8_t
alu_add8(cpu6800_state *s, uint8_t x, uint8_t y, unsigned carry_in)
{
	const unsigned res = (unsigned) x + y + carry_in;
	const uint8_t r8 = (uint8_t) res;

	set_nz8(s, r8);
	set_flag(s, FLAG_C, res > 0xffu);
	set_flag(s, FLAG_V, (((x ^ r8) & (y ^ r8)) & 0x80u) != 0);
	set_flag(s, FLAG_H, (((x & 0x0fu) + (y & 0x0fu) + carry_in) & 0x10u) != 0);
	return r8;
}

static uint8_t
alu_sub8(cpu6800_state *s, uint8_t x, uint8_t y, unsigned borrow_in)
{
	const unsigned res = (unsigned) x - y - borrow_in;
	const uint8_t r8 = (uint8_t) res;

	set_nz8(s, r8);
	/* A borrow, as on the 6809 and unlike the 6502. */
	set_flag(s, FLAG_C, (res & 0x100u) != 0);
	set_flag(s, FLAG_V, (((x ^ y) & (x ^ r8)) & 0x80u) != 0);
	return r8;
}

static uint8_t
alu_logic8(cpu6800_state *s, uint8_t v)
{
	set_nz8(s, v);
	set_flag(s, FLAG_V, 0);
	return v;
}

/*
 * ★ THE SHIFTS AND ROTATES DEFINE V AS N EXCLUSIVE-OR C, after the operation.
 *
 * That one rule covers all six of them and is not the same as the 6809's. For
 * ASL the two agree by arithmetic - N ends up as the old bit 6 and C as the old
 * bit 7, so their exclusive-or is the 6809's "did the sign change" - but for LSR
 * they do not agree at all: N is always clear after an LSR, so V simply follows
 * the bit shifted out, where a 6809 leaves V untouched. Software that tests V
 * after a shift behaves differently on the two parts.
 */
static void
set_shift_v(cpu6800_state *s)
{
	set_flag(s, FLAG_V, flag(s, FLAG_N) != flag(s, FLAG_C));
}

static uint8_t
op_neg(cpu6800_state *s, uint8_t v)
{
	return alu_sub8(s, 0, v, 0);
}

static uint8_t
op_com(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ~v;

	set_nz8(s, r);
	set_flag(s, FLAG_V, 0);
	set_flag(s, FLAG_C, 1);
	return r;
}

static uint8_t
op_lsr(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v >> 1);

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	set_shift_v(s);
	return r;
}

static uint8_t
op_asr(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v >> 1) | (v & 0x80u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	set_shift_v(s);
	return r;
}

static uint8_t
op_asl(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v << 1);

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x80u) != 0);
	set_shift_v(s);
	return r;
}

static uint8_t
op_rol(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v << 1) | (flag(s, FLAG_C) ? 1u : 0u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x80u) != 0);
	set_shift_v(s);
	return r;
}

static uint8_t
op_ror(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v >> 1) | (flag(s, FLAG_C) ? 0x80u : 0u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	set_shift_v(s);
	return r;
}

static uint8_t
op_inc(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v + 1);

	set_nz8(s, r);
	set_flag(s, FLAG_V, v == 0x7fu);
	return r;
}

static uint8_t
op_dec(cpu6800_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v - 1);

	set_nz8(s, r);
	set_flag(s, FLAG_V, v == 0x80u);
	return r;
}

static uint8_t
op_tst(cpu6800_state *s, uint8_t v)
{
	set_nz8(s, v);
	set_flag(s, FLAG_V, 0);
	/* ★ And the carry, which the 6809's TST leaves alone. */
	set_flag(s, FLAG_C, 0);
	return v;
}

static uint8_t
op_clr(cpu6800_state *s, uint8_t v)
{
	(void) v;
	set_flag(s, FLAG_N, 0);
	set_flag(s, FLAG_Z, 1);
	set_flag(s, FLAG_V, 0);
	set_flag(s, FLAG_C, 0);
	return 0;
}

/* DAA, on the same terms as the 6809's: the half carry is why H exists. */
static void
op_daa(cpu6800_state *s)
{
	const uint8_t lo = (uint8_t) (s->a & 0x0fu);
	const uint8_t hi = (uint8_t) (s->a >> 4);
	unsigned add = 0;

	if (flag(s, FLAG_H) || lo > 9) {
		add |= 0x06u;
	}
	if (flag(s, FLAG_C) || hi > 9 || (hi == 9 && lo > 9)) {
		add |= 0x60u;
	}

	{
		const unsigned res = (unsigned) s->a + add;

		s->a = (uint8_t) res;
		set_nz8(s, s->a);
		if (res > 0xffu) {
			set_flag(s, FLAG_C, 1);
		}
	}
}

/* ---------------------------------------------------------------- addressing */

/*
 * All four modes, and there is nothing to decode.
 *
 * ★ Direct is PAGE ZERO and nowhere else: this processor has no direct-page
 * register, so the 8-bit address is the whole address. Indexed is one form, an
 * UNSIGNED 8-bit offset from X, so it reaches 256 bytes forward and never
 * backwards. Both of those are what the 6809 was designed to get away from.
 */
static uint16_t
am_direct(cpu6800_state *s)
{
	return fetch8(s);
}

static uint16_t
am_indexed(cpu6800_state *s)
{
	const uint8_t off = fetch8(s);

	return (uint16_t) (s->x + off);
}

static uint16_t
am_extended(cpu6800_state *s)
{
	return fetch16(s);
}

/* -------------------------------------------------------------- interrupts */

/*
 * Seven bytes, in the order the hardware pushes them, and the same seven for
 * every one of IRQ, NMI and a software interrupt that vectors. There is no
 * shorter frame here: the 6809's fast interrupt, which pushes two, was an
 * addition and this part has no equivalent.
 */
static void
push_state(cpu6800_state *s)
{
	push16(s, s->pc);
	push16(s, s->x);
	push8(s, s->a);
	push8(s, s->b);
	push8(s, s->cc);
}

static void
take_interrupt(cpu6800_state *s, uint16_t vector)
{
	push_state(s);
	s->cc |= FLAG_I;
	s->pc = rd16(s, vector);
}

int
cpu6800_irq(cpu6800_state *s)
{
	if (s->halted || s->faulted || flag(s, FLAG_I)) {
		return 0;
	}
	take_interrupt(s, CPU6800_VEC_IRQ);
	return 1;
}

void
cpu6800_nmi(cpu6800_state *s)
{
	if (s->halted || s->faulted) {
		return;
	}
	take_interrupt(s, CPU6800_VEC_NMI);
}

/* ------------------------------------------------------------- entry points */

void
cpu6800_init(cpu6800_state *s, uint8_t *ram, uint32_t ram_size)
{
	memset(s, 0, sizeof(*s));
	s->ram = ram;
	s->ram_size = ram_size;
	cpu6800_reset(s, 0);
}

void
cpu6800_set_mem_hook(cpu6800_state *s, const cpu_mem_hook *hook)
{
	if (hook == NULL) {
		memset(&s->mem, 0, sizeof(s->mem));
		return;
	}
	s->mem = *hook;
}

void
cpu6800_reset(cpu6800_state *s, uint16_t entry)
{
	s->a = 0;
	s->b = 0;
	s->x = 0;
	s->sp = 0;
	/* The two unused bits read as ones on a real part, and a program that
	   pushes the condition codes and looks at them can see it. */
	s->cc = (uint8_t) (CPU6800_CC_ALWAYS | FLAG_I);
	s->pc = entry;

	s->halted = 0;
	s->halt_reason = 0;
	s->exit_code = 0;
	s->faulted = 0;
	s->fault_cause = 0;
	s->fault_addr = 0;
	s->cycles = 0;
	s->stalled = 0;
	s->stall_addr = 0;
	s->stall_is_write = 0;
}

const char *
cpu6800_fault_name(uint32_t cause)
{
	switch (cause) {
	case CPU6800_FAULT_ILLEGAL:	return "not an instruction on this part";
	case CPU6800_FAULT_ACCESS:	return "access outside the core's memory";
	case CPU6800_FAULT_WAIT:	return "WAI, which this card does not model";
	default:			return "unknown";
	}
}

/* ------------------------------------------------------------ the dispatch */

/* The read-modify-write column, by its low nibble, as on the 6809. The gaps are
   opcodes that do not exist and step() has already refused them. */
typedef uint8_t (*rmw_fn)(cpu6800_state *s, uint8_t v);

static const rmw_fn rmw_ops[16] = {
	op_neg, NULL,   NULL,   op_com,		/* 0 1 2 3 */
	op_lsr, NULL,   op_ror, op_asr,		/* 4 5 6 7 */
	op_asl, op_rol, op_dec, NULL,		/* 8 9 a b */
	op_inc, op_tst, NULL,   op_clr,		/* c d e f */
};

#define RMW_COL_TST	0x0du
#define RMW_COL_JMP	0x0eu

static void
rmw_memory(cpu6800_state *s, uint8_t col, uint16_t addr)
{
	uint8_t v;

	if (col == RMW_COL_JMP) {
		s->pc = addr;
		return;
	}

	v = rd8(s, addr);
	if (s->stalled || s->faulted) {
		return;
	}
	v = rmw_ops[col](s, v);
	if (col != RMW_COL_TST) {
		wr8(s, addr, v);
	}
}

static void
rmw_accumulator(cpu6800_state *s, uint8_t col, uint8_t *acc)
{
	const uint8_t v = rmw_ops[col](s, *acc);

	if (col != RMW_COL_TST) {
		*acc = v;
	}
}

/*
 * &80-&FF: the grid, on the same principle as the 6809's and smaller.
 *
 * Column three is empty in both halves, this processor having no 16-bit
 * accumulator to add or subtract; and the second half has no comparison and no
 * subroutine call, so its only 16-bit columns are the two that reach X.
 */
static void
exec_grid(cpu6800_state *s, uint8_t op)
{
	const int blk = (op & 0x40u) != 0;	/* 0: A, X and the stack. 1: B and X */
	const int mode = (op >> 4) & 3;		/* 0 imm, 1 direct, 2 indexed, 3 ext */
	const uint8_t col = (uint8_t) (op & 0x0fu);
	uint8_t *acc = blk ? &s->b : &s->a;
	uint16_t addr = 0;

	/* BSR when immediate, JSR otherwise; there is no direct form of either,
	   and the cycle table has already refused it. */
	if (!blk && col == 0x0d) {
		if (mode == 0) {
			const int8_t off = (int8_t) fetch8(s);

			if (s->stalled || s->faulted) {
				return;
			}
			push16(s, s->pc);
			s->pc = (uint16_t) (s->pc + off);
			return;
		}
		addr = (mode == 2) ? am_indexed(s) : am_extended(s);
		if (s->stalled || s->faulted) {
			return;
		}
		push16(s, s->pc);
		s->pc = addr;
		return;
	}

	if (mode != 0) {
		addr = (mode == 1) ? am_direct(s)
		     : (mode == 2) ? am_indexed(s) : am_extended(s);
		if (s->stalled || s->faulted) {
			return;
		}
	}

	/* The 16-bit columns. In the first half they reach the stack pointer and
	   compare the index register; in the second they load and store it. */
	if (col >= 0x0c) {
		uint16_t v;

		if (col == 0x0f) {		/* STS or STX */
			const uint16_t val = blk ? s->x : s->sp;

			wr16(s, addr, val);
			set_nz16(s, val);
			set_flag(s, FLAG_V, 0);
			return;
		}

		v = (mode == 0) ? fetch16(s) : rd16(s, addr);
		if (s->stalled || s->faulted) {
			return;
		}
		if (col == 0x0e) {		/* LDS or LDX */
			if (blk) {
				s->x = v;
			} else {
				s->sp = v;
			}
			set_nz16(s, v);
			set_flag(s, FLAG_V, 0);
			return;
		}
		/*
		 * ★ &0c is CPX, and its famous defect: it sets N, Z and V and
		 * leaves the CARRY ALONE. An unsigned 16-bit comparison therefore
		 * cannot be done with it in the obvious way, which is the single
		 * most complained-about thing about this processor and one of the
		 * things the 6809 was built to fix. Reproducing it is the point.
		 */
		{
			const uint16_t d = (uint16_t) (s->x - v);

			set_nz16(s, d);
			set_flag(s, FLAG_V,
			    (((s->x ^ v) & (s->x ^ d)) & 0x8000u) != 0);
		}
		return;
	}

	if (col == 0x07) {			/* STAA or STAB */
		wr8(s, addr, *acc);
		set_nz8(s, *acc);
		set_flag(s, FLAG_V, 0);
		return;
	}

	{
		const uint8_t v = (mode == 0) ? fetch8(s) : rd8(s, addr);
		const unsigned carry = flag(s, FLAG_C) ? 1u : 0u;

		if (s->stalled || s->faulted) {
			return;
		}
		switch (col) {
		case 0x00: *acc = alu_sub8(s, *acc, v, 0); break;	/* SUB */
		case 0x01: (void) alu_sub8(s, *acc, v, 0); break;	/* CMP */
		case 0x02: *acc = alu_sub8(s, *acc, v, carry); break;	/* SBC */
		case 0x04: *acc = alu_logic8(s, (uint8_t) (*acc & v)); break;
		case 0x05: (void) alu_logic8(s, (uint8_t) (*acc & v)); break;
		case 0x06: *acc = alu_logic8(s, v); break;		/* LDA */
		case 0x08: *acc = alu_logic8(s, (uint8_t) (*acc ^ v)); break;
		case 0x09: *acc = alu_add8(s, *acc, v, carry); break;	/* ADC */
		case 0x0a: *acc = alu_logic8(s, (uint8_t) (*acc | v)); break;
		default:   *acc = alu_add8(s, *acc, v, 0); break;	/* &b ADD */
		}
	}
}

/* The branch conditions. ★ There is no BRN: &21 is not an instruction. */
static int
branch_taken(const cpu6800_state *s, uint8_t cond)
{
	const int c = flag(s, FLAG_C);
	const int v = flag(s, FLAG_V);
	const int z = flag(s, FLAG_Z);
	const int n = flag(s, FLAG_N);

	switch (cond & 0x0fu) {
	case 0x0: return 1;			/* BRA */
	case 0x2: return !c && !z;		/* BHI */
	case 0x3: return c || z;		/* BLS */
	case 0x4: return !c;			/* BCC */
	case 0x5: return c;			/* BCS */
	case 0x6: return !z;			/* BNE */
	case 0x7: return z;			/* BEQ */
	case 0x8: return !v;			/* BVC */
	case 0x9: return v;			/* BVS */
	case 0xa: return !n;			/* BPL */
	case 0xb: return n;			/* BMI */
	case 0xc: return n == v;		/* BGE, signed */
	case 0xd: return n != v;		/* BLT */
	case 0xe: return !z && n == v;		/* BGT */
	default:  return z || n != v;		/* BLE */
	}
}

/* &00-&3F, which is inherent operations, the branches and nothing regular. */
static void
exec_low(cpu6800_state *s, uint8_t op)
{
	if (op >= 0x20u && op < 0x30u) {	/* the branches, all four cycles */
		const int8_t off = (int8_t) fetch8(s);

		if (s->stalled || s->faulted) {
			return;
		}
		if (branch_taken(s, op)) {
			s->pc = (uint16_t) (s->pc + off);
		}
		return;
	}
	if (op >= 0x40u && op < 0x60u) {	/* inherent, on A then on B */
		rmw_accumulator(s, (uint8_t) (op & 0x0fu),
		                (op < 0x50u) ? &s->a : &s->b);
		return;
	}
	if (op >= 0x60u) {			/* indexed, then extended */
		const uint16_t addr = (op < 0x70u) ? am_indexed(s)
		                                   : am_extended(s);

		if (!s->stalled && !s->faulted) {
			rmw_memory(s, (uint8_t) (op & 0x0fu), addr);
		}
		return;
	}

	switch (op) {
	case 0x01:				/* NOP */
		break;

	case 0x06:				/* TAP: A into the condition codes */
		s->cc = (uint8_t) (s->a | CPU6800_CC_ALWAYS);
		break;
	case 0x07:				/* TPA */
		s->a = (uint8_t) (s->cc | CPU6800_CC_ALWAYS);
		break;

	/*
	 * ★ INX AND DEX AFFECT Z AND NOTHING ELSE. No N, no V - so a loop that
	 * counts an index register down and branches on BPL does not work on this
	 * processor, and one that branches on BNE does. A core that set N as well
	 * would run software that a real part would not.
	 */
	case 0x08:				/* INX */
		s->x = (uint16_t) (s->x + 1);
		set_flag(s, FLAG_Z, s->x == 0);
		break;
	case 0x09:				/* DEX */
		s->x = (uint16_t) (s->x - 1);
		set_flag(s, FLAG_Z, s->x == 0);
		break;

	case 0x0a: set_flag(s, FLAG_V, 0); break;	/* CLV */
	case 0x0b: set_flag(s, FLAG_V, 1); break;	/* SEV */
	case 0x0c: set_flag(s, FLAG_C, 0); break;	/* CLC */
	case 0x0d: set_flag(s, FLAG_C, 1); break;	/* SEC */
	case 0x0e: set_flag(s, FLAG_I, 0); break;	/* CLI */
	case 0x0f: set_flag(s, FLAG_I, 1); break;	/* SEI */

	case 0x10:				/* SBA: A = A - B */
		s->a = alu_sub8(s, s->a, s->b, 0);
		break;
	case 0x11:				/* CBA: compare them */
		(void) alu_sub8(s, s->a, s->b, 0);
		break;
	case 0x1b:				/* ABA: A = A + B */
		s->a = alu_add8(s, s->a, s->b, 0);
		break;

	case 0x16:				/* TAB */
		s->b = alu_logic8(s, s->a);
		break;
	case 0x17:				/* TBA */
		s->a = alu_logic8(s, s->b);
		break;

	case 0x19:				/* DAA */
		op_daa(s);
		break;

	/* ★ TSX and TXS carry the stack pointer's off-by-one on their faces. */
	case 0x30:				/* TSX */
		s->x = (uint16_t) (s->sp + 1);
		break;
	case 0x35:				/* TXS */
		s->sp = (uint16_t) (s->x - 1);
		break;

	case 0x31:				/* INS */
		s->sp = (uint16_t) (s->sp + 1);
		break;
	case 0x34:				/* DES */
		s->sp = (uint16_t) (s->sp - 1);
		break;

	case 0x32:				/* PULA */
		s->a = pull8(s);
		break;
	case 0x33:				/* PULB */
		s->b = pull8(s);
		break;
	case 0x36:				/* PSHA */
		push8(s, s->a);
		break;
	case 0x37:				/* PSHB */
		push8(s, s->b);
		break;

	case 0x39:				/* RTS */
		s->pc = pull16(s);
		break;

	case 0x3b:				/* RTI */
		/* The frame is one size on this part, so unlike the 6809's there
		   is nothing to consult and no shorter form to get wrong. */
		s->cc = (uint8_t) (pull8(s) | CPU6800_CC_ALWAYS);
		s->b = pull8(s);
		s->a = pull8(s);
		s->x = pull16(s);
		s->pc = pull16(s);
		break;

	case 0x3e:				/* WAI */
		/* Stops until an interrupt arrives, which this card does not
		   model. See cpu_6800.h, and SYNC on the 6809. */
		fault(s, CPU6800_FAULT_WAIT, op);
		break;

	case 0x3f:				/* SWI: the program is finished */
		s->halted = 1;
		s->halt_reason = CPU6800_HALT_SWI;
		s->exit_code = s->a;
		break;

	default:
		/* Unreachable: step() has already refused any opcode the cycle
		   table has no entry for, and every entry it has is above. */
		fault(s, CPU6800_FAULT_ILLEGAL, op);
		break;
	}
}

/* ------------------------------------------------------------ step and run */

static int
abandon(cpu6800_state *s)
{
	s->a = s->saved.a;
	s->b = s->saved.b;
	s->cc = s->saved.cc;
	s->x = s->saved.x;
	s->sp = s->saved.sp;
	s->pc = s->saved.pc;
	return 0;
}

int
cpu6800_step(cpu6800_state *s)
{
	uint8_t op;

	if (s->halted || s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return 0;		/* waiting for the guest to answer */
	}

	if (s->mem.can_stall) {
		s->saved.a = s->a;
		s->saved.b = s->b;
		s->saved.cc = s->cc;
		s->saved.x = s->x;
		s->saved.sp = s->sp;
		s->saved.pc = s->pc;
	}

	op = fetch8(s);
	if (s->faulted) {
		return 0;		/* the program counter left its memory */
	}
	if (s->stalled) {
		return abandon(s);
	}

	/* The cycle table is also the list of what exists, for the reason given
	   in cpu_6809.c: one table cannot disagree with itself. */
	if (cycles_6800[op] == 0) {
		fault(s, CPU6800_FAULT_ILLEGAL, op);
		return 0;
	}

	if (op >= 0x80u) {
		exec_grid(s, op);
	} else {
		exec_low(s, op);
	}

	if (s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return abandon(s);
	}

	/*
	 * No adjustment at all, unlike the other cores here: this processor's
	 * branches cost the same taken or not, its indexed addressing has one
	 * form with one price, and its stack instructions move one register each.
	 * The table is the whole answer.
	 */
	s->cycles += cycles_6800[op];
	return cycles_6800[op];
}

int
cpu6800_run(cpu6800_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = cpu6800_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}
