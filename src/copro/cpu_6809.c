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
 * cpu_6809.c - a Motorola 6809, for the OPEN Bus co-processor card.
 *
 * What is implemented, what is not, and how a program stops are all in
 * cpu_6809.h. This file is the how.
 *
 * ★ WHY THIS IS DECODED AS A GRID AND NOT AS 256 CASES.
 *
 * The other cores here use one switch with a case per opcode, which is the right
 * shape for a 6502 or a Z80: their maps are historical accretion and a case per
 * opcode is the only honest way to write them down. A 6809's map is not like
 * that. Motorola laid it out as a table on purpose, and the position of an
 * opcode in that table tells you what it does:
 *
 *   &80-&BF   operate on A, or on X          &C0-&FF   operate on B, or on D/U
 *   bits 5-4  the addressing mode: immediate, direct, indexed, extended
 *   bits 3-0  which operation
 *
 * so SUBA immediate, SUBA direct, SUBA indexed and SUBA extended are one
 * operation reached four ways, and SUBA and SUBB are the same operation on a
 * different register. Writing that out as 256 cases would be writing the same
 * sixteen operations sixteen times, and the mistakes it invites are exactly the
 * ones a reviewer cannot see: one cell of the grid quietly holding the operation
 * from the cell next to it.
 *
 * So the regular part of the map - &40-&FF, which is three quarters of it - is
 * decoded by taking the grid apart, and only the irregular quarter (&00-&3F, the
 * branches, the stack instructions and the odds and ends) is a switch. The
 * datasheet's own table is the specification, and the code is shaped like it.
 *
 * ★ AND WHY THE POSTBYTE IS THE REAL WORK. See am_indexed() below. A 6809's
 * "indexed" addressing is a language of its own with fourteen forms and an
 * indirect version of most of them, and it is where the cycle counts stop being
 * a table lookup: the same opcode costs a different number of cycles depending
 * on the postbyte that follows it.
 */

#include <string.h>

#include "cpu_6809.h"

#define FLAG_C	CPU6809_FLAG_C
#define FLAG_V	CPU6809_FLAG_V
#define FLAG_Z	CPU6809_FLAG_Z
#define FLAG_N	CPU6809_FLAG_N
#define FLAG_I	CPU6809_FLAG_I
#define FLAG_H	CPU6809_FLAG_H
#define FLAG_F	CPU6809_FLAG_F
#define FLAG_E	CPU6809_FLAG_E

static void
fault(cpu6809_state *s, uint32_t cause, uint32_t addr)
{
	/* The first fault is the one that gets reported: a fault handler here
	   would be the next thing to fault, and the cause a program needs is the
	   original. */
	if (!s->faulted) {
		s->faulted = 1;
		s->fault_cause = cause;
		s->fault_addr = addr;
	}
}

/*
 * The cycle counts, one table per page.
 *
 * ★ DERIVED FROM THE GROUP RULES, NOT TRANSCRIBED. Every entry in the regular
 * part of these tables follows from two things: which column of the grid the
 * opcode is in, which says how wide its operand is, and which addressing mode
 * it uses, which says what reaching that operand costs. Typing 768 numbers out
 * of a datasheet would put a wrong one somewhere and no reader would ever find
 * it. These were generated from those rules and then cross-checked against 83
 * values read off the datasheet independently; all 83 agreed.
 *
 * A zero means the opcode does not exist. An indexed opcode's entry is the cost
 * BEFORE its postbyte, which adds its own; see indexed_cycles below.
 */
static const uint8_t cycles_page1[256] = {
	 6,  0,  0,  6,  6,  0,  6,  6,  6,  6,  6,  0,  6,  6,  3,  6,	/* 00 */
	 0,  0,  2,  2,  0,  0,  5,  9,  0,  2,  3,  0,  3,  2,  8,  6,	/* 10 */
	 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,	/* 20 */
	 4,  4,  4,  4,  5,  5,  5,  5,  0,  5,  3,  6, 20, 11,  0, 19,	/* 30 */
	 2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2,	/* 40 */
	 2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2,	/* 50 */
	 6,  0,  0,  6,  6,  0,  6,  6,  6,  6,  6,  0,  6,  6,  3,  6,	/* 60 */
	 7,  0,  0,  7,  7,  0,  7,  7,  7,  7,  7,  0,  7,  7,  4,  7,	/* 70 */
	 2,  2,  2,  4,  2,  2,  2,  0,  2,  2,  2,  2,  4,  7,  3,  0,	/* 80 */
	 4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  6,  7,  5,  5,	/* 90 */
	 4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  6,  7,  5,  5,	/* a0 */
	 5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  5,  7,  8,  6,  6,	/* b0 */
	 2,  2,  2,  4,  2,  2,  2,  0,  2,  2,  2,  2,  3,  0,  3,  0,	/* c0 */
	 4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,	/* d0 */
	 4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,	/* e0 */
	 5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,	/* f0 */
};

/* Page two, reached through the &10 prefix. These counts include the prefix
   byte, which is why the prefix itself costs nothing in the table above. */
static const uint8_t cycles_page2[256] = {
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 00 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 10 */
	 0,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,	/* 20 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 20,	/* 30 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 40 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 50 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 60 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 70 */
	 0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,  0,  5,  0,  4,  0,	/* 80 */
	 0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  0,  0,  7,  0,  6,  6,	/* 90 */
	 0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  0,  0,  7,  0,  6,  6,	/* a0 */
	 0,  0,  0,  8,  0,  0,  0,  0,  0,  0,  0,  0,  8,  0,  7,  7,	/* b0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,	/* c0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  6,  6,	/* d0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  6,  6,	/* e0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  7,  7,	/* f0 */
};

/* Page three, through the &11 prefix: the two comparisons the other pages have
   no room for, and SWI3. */
static const uint8_t cycles_page3[256] = {
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 00 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 10 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 20 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 20,	/* 30 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 40 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 50 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 60 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 70 */
	 0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,	/* 80 */
	 0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  0,  0,  7,  0,  0,  0,	/* 90 */
	 0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  0,  0,  7,  0,  0,  0,	/* a0 */
	 0,  0,  0,  8,  0,  0,  0,  0,  0,  0,  0,  0,  8,  0,  0,  0,	/* b0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* c0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* d0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* e0 */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* f0 */
};

/* ------------------------------------------------------------------ memory */

static uint8_t
rd8(cpu6809_state *s, uint16_t addr)
{
	if (s->mem.read != NULL) {
		uint8_t val = 0;

		switch (s->mem.read(s->mem.ctx, addr, &val)) {
		case CPU_MEM_OK:
			return val;
		case CPU_MEM_STALL:
			/* Abandon the instruction; cpu6809_step puts the registers
			   back and it is retried once the guest has answered. See
			   cpu_mem.h. */
			s->stalled = 1;
			s->stall_addr = addr;
			s->stall_is_write = 0;
			return 0;
		default:
			fault(s, CPU6809_FAULT_ACCESS, addr);
			return 0;
		}
	}

	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6809_FAULT_ACCESS, addr);
		return 0;
	}
	return s->ram[addr];
}

static void
wr8(cpu6809_state *s, uint16_t addr, uint8_t val)
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
			fault(s, CPU6809_FAULT_ACCESS, addr);
			return;
		}
	}

	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU6809_FAULT_ACCESS, addr);
		return;
	}
	s->ram[addr] = val;
}

/* The 6809 is big-endian, unlike every other 8-bit core here: the high byte of a
   16-bit quantity is at the lower address, in memory and on the stack alike. */
static uint16_t
rd16(cpu6809_state *s, uint16_t addr)
{
	const uint8_t hi = rd8(s, addr);
	const uint8_t lo = rd8(s, (uint16_t) (addr + 1));

	return (uint16_t) ((hi << 8) | lo);
}

static void
wr16(cpu6809_state *s, uint16_t addr, uint16_t val)
{
	wr8(s, addr, (uint8_t) (val >> 8));
	wr8(s, (uint16_t) (addr + 1), (uint8_t) val);
}

static uint8_t
fetch8(cpu6809_state *s)
{
	const uint8_t val = rd8(s, s->pc);

	s->pc = (uint16_t) (s->pc + 1);
	return val;
}

static uint16_t
fetch16(cpu6809_state *s)
{
	const uint16_t val = rd16(s, s->pc);

	s->pc = (uint16_t) (s->pc + 2);
	return val;
}

/* ------------------------------------------------------------------ stacks */

/*
 * Both stacks work the same way and grow down, so which one an instruction uses
 * is an argument rather than two copies of the code. S is the hardware stack,
 * the one an interrupt and a subroutine call use; U is the program's own, and
 * nothing but the program touches it.
 */
static void
push8(cpu6809_state *s, uint16_t *sp, uint8_t val)
{
	*sp = (uint16_t) (*sp - 1);
	wr8(s, *sp, val);
}

static uint8_t
pull8(cpu6809_state *s, uint16_t *sp)
{
	const uint8_t val = rd8(s, *sp);

	*sp = (uint16_t) (*sp + 1);
	return val;
}

static void
push16(cpu6809_state *s, uint16_t *sp, uint16_t val)
{
	push8(s, sp, (uint8_t) val);		/* low byte first: it ends up higher */
	push8(s, sp, (uint8_t) (val >> 8));
}

static uint16_t
pull16(cpu6809_state *s, uint16_t *sp)
{
	const uint8_t hi = pull8(s, sp);
	const uint8_t lo = pull8(s, sp);

	return (uint16_t) ((hi << 8) | lo);
}

/* -------------------------------------------------------------------- flags */

static void
set_flag(cpu6809_state *s, uint8_t bit, int on)
{
	if (on) {
		s->cc |= bit;
	} else {
		s->cc = (uint8_t) (s->cc & ~bit);
	}
}

static void
set_nz8(cpu6809_state *s, uint8_t v)
{
	set_flag(s, FLAG_N, (v & 0x80u) != 0);
	set_flag(s, FLAG_Z, v == 0);
}

static void
set_nz16(cpu6809_state *s, uint16_t v)
{
	set_flag(s, FLAG_N, (v & 0x8000u) != 0);
	set_flag(s, FLAG_Z, v == 0);
}

static uint16_t
reg_d(const cpu6809_state *s)
{
	return (uint16_t) ((s->a << 8) | s->b);
}

static void
set_d(cpu6809_state *s, uint16_t v)
{
	s->a = (uint8_t) (v >> 8);
	s->b = (uint8_t) v;
}

/* ---------------------------------------------------------------------- ALU */

static uint8_t
alu_add8(cpu6809_state *s, uint8_t x, uint8_t y, unsigned carry_in)
{
	const unsigned res = (unsigned) x + y + carry_in;
	const uint8_t r8 = (uint8_t) res;

	set_nz8(s, r8);
	set_flag(s, FLAG_C, res > 0xffu);
	/* Overflow is a sign question, not a magnitude one: it happened if the
	   operands agreed about their sign and the result disagrees with them. */
	set_flag(s, FLAG_V, (((x ^ r8) & (y ^ r8)) & 0x80u) != 0);
	/* The half carry exists for DAA and is only defined for an 8-bit add. */
	set_flag(s, FLAG_H, (((x & 0x0fu) + (y & 0x0fu) + carry_in) & 0x10u) != 0);
	return r8;
}

static uint8_t
alu_sub8(cpu6809_state *s, uint8_t x, uint8_t y, unsigned borrow_in)
{
	const unsigned res = (unsigned) x - y - borrow_in;
	const uint8_t r8 = (uint8_t) res;

	set_nz8(s, r8);
	/* C after a subtract is a BORROW on a 6809, set when the subtraction
	   needed one. That is the 6502's convention inverted, and getting it the
	   wrong way round would break every multi-byte subtraction. */
	set_flag(s, FLAG_C, (res & 0x100u) != 0);
	set_flag(s, FLAG_V, (((x ^ y) & (x ^ r8)) & 0x80u) != 0);
	return r8;
}

static uint16_t
alu_add16(cpu6809_state *s, uint16_t x, uint16_t y)
{
	const uint32_t res = (uint32_t) x + y;
	const uint16_t r16 = (uint16_t) res;

	set_nz16(s, r16);
	set_flag(s, FLAG_C, res > 0xffffu);
	set_flag(s, FLAG_V, (((x ^ r16) & (y ^ r16)) & 0x8000u) != 0);
	return r16;
}

static uint16_t
alu_sub16(cpu6809_state *s, uint16_t x, uint16_t y)
{
	const uint32_t res = (uint32_t) x - y;
	const uint16_t r16 = (uint16_t) res;

	set_nz16(s, r16);
	set_flag(s, FLAG_C, (res & 0x10000u) != 0);
	set_flag(s, FLAG_V, (((x ^ y) & (x ^ r16)) & 0x8000u) != 0);
	return r16;
}

static uint8_t
alu_logic8(cpu6809_state *s, uint8_t v)
{
	set_nz8(s, v);
	set_flag(s, FLAG_V, 0);
	return v;
}

/*
 * The read-modify-write operations, shared by their inherent, direct, indexed
 * and extended forms. Each one is one function because the grid reaches the same
 * operation four ways, and a second copy is a second place to be wrong.
 */
static uint8_t
op_neg(cpu6809_state *s, uint8_t v)
{
	/* Negation IS a subtraction from zero, so it borrows the flag rules
	   rather than restating them: C ends up set for every value but zero and
	   V for &80 alone, which is what the datasheet says in words. */
	return alu_sub8(s, 0, v, 0);
}

static uint8_t
op_com(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ~v;

	set_nz8(s, r);
	set_flag(s, FLAG_V, 0);
	set_flag(s, FLAG_C, 1);		/* always, which is a 6809 peculiarity */
	return r;
}

static uint8_t
op_lsr(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v >> 1);

	set_nz8(s, r);			/* N is always clear after this */
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	return r;
}

static uint8_t
op_asr(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v >> 1) | (v & 0x80u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	return r;
}

static uint8_t
op_asl(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v << 1);

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x80u) != 0);
	/* The two top bits of the value going in: if they differed, the shift
	   moved the sign. */
	set_flag(s, FLAG_V, (((v >> 7) ^ (v >> 6)) & 1u) != 0);
	return r;
}

static uint8_t
op_rol(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v << 1) | ((s->cc & FLAG_C) ? 1u : 0u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x80u) != 0);
	set_flag(s, FLAG_V, (((v >> 7) ^ (v >> 6)) & 1u) != 0);
	return r;
}

static uint8_t
op_ror(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) ((v >> 1) | ((s->cc & FLAG_C) ? 0x80u : 0u));

	set_nz8(s, r);
	set_flag(s, FLAG_C, (v & 0x01u) != 0);
	return r;
}

static uint8_t
op_inc(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v + 1);

	set_nz8(s, r);
	set_flag(s, FLAG_V, v == 0x7fu);	/* the one value that overflows */
	return r;				/* C is untouched, deliberately */
}

static uint8_t
op_dec(cpu6809_state *s, uint8_t v)
{
	const uint8_t r = (uint8_t) (v - 1);

	set_nz8(s, r);
	set_flag(s, FLAG_V, v == 0x80u);
	return r;
}

static uint8_t
op_tst(cpu6809_state *s, uint8_t v)
{
	set_nz8(s, v);
	set_flag(s, FLAG_V, 0);
	return v;				/* nothing is stored back */
}

static uint8_t
op_clr(cpu6809_state *s, uint8_t v)
{
	(void) v;
	set_flag(s, FLAG_N, 0);
	set_flag(s, FLAG_Z, 1);
	set_flag(s, FLAG_V, 0);
	set_flag(s, FLAG_C, 0);
	return 0;
}

/*
 * DAA, which fixes up A after an addition of two packed BCD values.
 *
 * The half carry is why H exists: an addition that carried out of the low
 * nibble needs six added to it whether or not that nibble now looks legal.
 */
static void
op_daa(cpu6809_state *s)
{
	const uint8_t lo = (uint8_t) (s->a & 0x0fu);
	const uint8_t hi = (uint8_t) (s->a >> 4);
	unsigned add = 0;

	if ((s->cc & FLAG_H) != 0 || lo > 9) {
		add |= 0x06u;
	}
	if ((s->cc & FLAG_C) != 0 || hi > 9 ||
	    (hi == 9 && lo > 9)) {
		add |= 0x60u;
	}

	{
		const unsigned res = (unsigned) s->a + add;

		s->a = (uint8_t) res;
		set_nz8(s, s->a);
		/* The carry is sticky: an addition that already carried stays
		   carried, because the fix-up cannot take it back. */
		if (res > 0xffu) {
			set_flag(s, FLAG_C, 1);
		}
		set_flag(s, FLAG_V, 0);
	}
}

/* ------------------------------------------------------- indexed addressing */

/*
 * ★ THE POSTBYTE, which is where a 6809 keeps its complexity.
 *
 * One byte after the opcode says which of four registers to index from, which of
 * fourteen ways to form the offset, and whether the address so formed is the
 * operand's address or a pointer to it. Bit 7 splits it in two:
 *
 *   0rr nnnnn   a five-bit signed offset from R, and nothing else; the common
 *               case, which is why it gets a whole half of the encoding
 *   1rr immmm   mmmm chooses the form, i says take the address it forms and
 *               read the real address from there
 *
 * Three of the sixteen forms do not exist, and two more exist only in their
 * direct version: there is no indirect ",R+" or ",-R", because incrementing by
 * one and then reading a 16-bit pointer would be reading half of one pointer and
 * half of the next. Those fault rather than being quietly treated as something
 * else, for the reason the 8080 faults on a Z80 opcode: a core that invents a
 * meaning makes software look portable when it is not.
 */

/* What each form costs, before the indirect surcharge. An unlisted form does
   not exist. Indirection adds three to any of them. */
static const uint8_t indexed_cycles[16] = {
	2,	/* 0  ,R+     */
	3,	/* 1  ,R++    */
	2,	/* 2  ,-R     */
	3,	/* 3  ,--R    */
	0,	/* 4  ,R      */
	1,	/* 5  B,R     */
	1,	/* 6  A,R     */
	0,	/* 7  -       */
	1,	/* 8  n,R  8  */
	4,	/* 9  n,R  16 */
	0,	/* a  -       */
	4,	/* b  D,R     */
	1,	/* c  n,PCR 8 */
	5,	/* d  n,PCR16 */
	0,	/* e  -       */
	2,	/* f  [n]     */
};

static uint16_t *
index_reg(cpu6809_state *s, uint8_t which)
{
	switch (which & 3u) {
	case 0:	return &s->x;
	case 1:	return &s->y;
	case 2:	return &s->u;
	default: return &s->s;
	}
}

static uint16_t
am_indexed(cpu6809_state *s)
{
	const uint8_t pb = fetch8(s);
	uint16_t *reg = index_reg(s, (uint8_t) (pb >> 5));
	const int indirect = (pb & 0x10u) != 0;
	const uint8_t form = (uint8_t) (pb & 0x0fu);
	uint16_t addr;

	if (s->stalled || s->faulted) {
		return 0;
	}

	if ((pb & 0x80u) == 0) {
		/* Five-bit signed offset, sign-extended from bit 4. There is no
		   indirect form of this one and no room in the encoding for it. */
		int offset = pb & 0x1fu;

		if ((offset & 0x10) != 0) {
			offset -= 0x20;
		}
		s->extra_cycles = 1;
		return (uint16_t) (*reg + offset);
	}

	if (indexed_cycles[form] == 0 && form != 0x04) {
		fault(s, CPU6809_FAULT_POSTBYTE, pb);
		return 0;
	}
	s->extra_cycles = indexed_cycles[form] + (indirect ? 3 : 0);

	switch (form) {
	case 0x00:	/* ,R+  */
		if (indirect) {
			fault(s, CPU6809_FAULT_POSTBYTE, pb);
			return 0;
		}
		addr = *reg;
		*reg = (uint16_t) (*reg + 1);
		break;
	case 0x01:	/* ,R++ */
		addr = *reg;
		*reg = (uint16_t) (*reg + 2);
		break;
	case 0x02:	/* ,-R  */
		if (indirect) {
			fault(s, CPU6809_FAULT_POSTBYTE, pb);
			return 0;
		}
		*reg = (uint16_t) (*reg - 1);
		addr = *reg;
		break;
	case 0x03:	/* ,--R */
		*reg = (uint16_t) (*reg - 2);
		addr = *reg;
		break;
	case 0x04:	/* ,R   */
		addr = *reg;
		break;
	case 0x05:	/* B,R, and the offset is signed */
		addr = (uint16_t) (*reg + (int8_t) s->b);
		break;
	case 0x06:	/* A,R */
		addr = (uint16_t) (*reg + (int8_t) s->a);
		break;
	case 0x08: {	/* n,R with an 8-bit signed offset */
		const int8_t off = (int8_t) fetch8(s);

		addr = (uint16_t) (*reg + off);
		break;
	}
	case 0x09: {	/* n,R with a 16-bit offset */
		const int16_t off = (int16_t) fetch16(s);

		addr = (uint16_t) (*reg + off);
		break;
	}
	case 0x0b:	/* D,R, and D is unsigned here */
		addr = (uint16_t) (*reg + reg_d(s));
		break;
	case 0x0c: {	/* n,PCR, 8-bit */
		/*
		 * Relative to the program counter AFTER the offset byte, which is
		 * where it is by now: an assembler works out the offset on that
		 * basis and the two have to agree.
		 *
		 * ★ THE FETCH IS ON ITS OWN LINE ON PURPOSE. Written as
		 * "s->pc + (int8_t) fetch8(s)" this reads the program counter and
		 * advances it in one expression, and C does not say which happens
		 * first - so the offset was applied to the program counter as it
		 * was BEFORE the operand, one byte short, and only on the two
		 * forms that index from the program counter at all. It built
		 * without a warning and the test caught it.
		 */
		const int8_t off = (int8_t) fetch8(s);

		addr = (uint16_t) (s->pc + off);
		break;
	}
	case 0x0d: {	/* n,PCR, 16-bit */
		const int16_t off = (int16_t) fetch16(s);

		addr = (uint16_t) (s->pc + off);
		break;
	}
	default:	/* &0f: [n], and the register field means nothing */
		if (!indirect) {
			fault(s, CPU6809_FAULT_POSTBYTE, pb);
			return 0;
		}
		addr = fetch16(s);
		break;
	}

	if (indirect) {
		if (s->stalled || s->faulted) {
			return 0;
		}
		addr = rd16(s, addr);
	}
	return addr;
}

/* The three addressing modes that need no decision. */
static uint16_t
am_direct(cpu6809_state *s)
{
	return (uint16_t) ((s->dp << 8) | fetch8(s));
}

static uint16_t
am_extended(cpu6809_state *s)
{
	return fetch16(s);
}

/* -------------------------------------------------------------- interrupts */

/*
 * Push the state an interrupt needs to come back from, and record in E how much
 * of it there was. RTI reads E to know what to take back, so the flag is not
 * decoration: FIRQ pushes two bytes and everything else pushes twelve.
 */
static void
push_state(cpu6809_state *s, int entire)
{
	set_flag(s, FLAG_E, entire);
	push16(s, &s->s, s->pc);
	if (entire) {
		push16(s, &s->s, s->u);
		push16(s, &s->s, s->y);
		push16(s, &s->s, s->x);
		push8(s, &s->s, s->dp);
		push8(s, &s->s, s->b);
		push8(s, &s->s, s->a);
	}
	push8(s, &s->s, s->cc);
}

static void
take_interrupt(cpu6809_state *s, uint16_t vector, int entire, uint8_t mask)
{
	push_state(s, entire);
	s->cc |= mask;
	s->pc = rd16(s, vector);
}

int
cpu6809_irq(cpu6809_state *s)
{
	if (s->halted || s->faulted || (s->cc & FLAG_I) != 0) {
		return 0;
	}
	take_interrupt(s, CPU6809_VEC_IRQ, 1, FLAG_I);
	return 1;
}

int
cpu6809_firq(cpu6809_state *s)
{
	if (s->halted || s->faulted || (s->cc & FLAG_F) != 0) {
		return 0;
	}
	/* Both masks, not just its own: a fast interrupt handler is not meant to
	   be interrupted by the slow one either. */
	take_interrupt(s, CPU6809_VEC_FIRQ, 0, (uint8_t) (FLAG_F | FLAG_I));
	return 1;
}

void
cpu6809_nmi(cpu6809_state *s)
{
	if (s->halted || s->faulted) {
		return;
	}
	take_interrupt(s, CPU6809_VEC_NMI, 1, (uint8_t) (FLAG_F | FLAG_I));
}

/* ------------------------------------------------------------- entry points */

void
cpu6809_init(cpu6809_state *s, uint8_t *ram, uint32_t ram_size)
{
	memset(s, 0, sizeof(*s));
	s->ram = ram;
	s->ram_size = ram_size;
	cpu6809_reset(s, 0);
}

void
cpu6809_set_mem_hook(cpu6809_state *s, const cpu_mem_hook *hook)
{
	if (hook == NULL) {
		memset(&s->mem, 0, sizeof(s->mem));
		return;
	}
	s->mem = *hook;
}

void
cpu6809_reset(cpu6809_state *s, uint16_t entry)
{
	s->a = 0;
	s->b = 0;
	s->x = 0;
	s->y = 0;
	s->u = 0;
	s->s = 0;
	s->dp = 0;			/* direct page zero, as the hardware does */
	s->cc = (uint8_t) (FLAG_I | FLAG_F);	/* both interrupts masked */
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
	s->extra_cycles = 0;
	s->branch_taken = 0;
}

const char *
cpu6809_fault_name(uint32_t cause)
{
	switch (cause) {
	case CPU6809_FAULT_ILLEGAL:	return "undocumented opcode";
	case CPU6809_FAULT_ACCESS:	return "access outside the core's memory";
	case CPU6809_FAULT_POSTBYTE:	return "indexed postbyte with no meaning";
	case CPU6809_FAULT_WAIT:	return "SYNC or CWAI, which this card does not model";
	default:			return "unknown";
	}
}

/* ------------------------------------------------------------ the dispatch */

/*
 * The read-modify-write operations, by their column in the grid. The gaps are
 * opcodes that do not exist and are never reached: the cycle table has a zero
 * for each of them and step() has already refused the opcode by the time these
 * are indexed. JMP is not here because it writes to the program counter rather
 * than to its operand, and TST is here but must not store.
 */
typedef uint8_t (*rmw_fn)(cpu6809_state *s, uint8_t v);

static const rmw_fn rmw_ops[16] = {
	op_neg, NULL,   NULL,   op_com,		/* 0 1 2 3 */
	op_lsr, NULL,   op_ror, op_asr,		/* 4 5 6 7 */
	op_asl, op_rol, op_dec, NULL,		/* 8 9 a b */
	op_inc, op_tst, NULL,   op_clr,		/* c d e f */
};

/* TST is the one column that reads its operand and puts nothing back. */
#define RMW_COL_TST	0x0du
#define RMW_COL_JMP	0x0eu

static void
rmw_memory(cpu6809_state *s, uint8_t col, uint16_t addr)
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
		/* Guarded because the read above may have stalled on the way to a
		   region the guest answers for, and a store that went ahead
		   anyway would be the one thing a retry cannot undo. See the note
		   on what a stall assumes in cpu_mem.h. */
		wr8(s, addr, v);
	}
}

static void
rmw_accumulator(cpu6809_state *s, uint8_t col, uint8_t *acc)
{
	const uint8_t v = rmw_ops[col](s, *acc);

	if (col != RMW_COL_TST) {
		*acc = v;
	}
}

/* ---- the 16-bit half of the grid, which is where the pages diverge ------- */

typedef enum { WR_D, WR_X, WR_Y, WR_U, WR_S } wide_reg;
typedef enum { W_NONE, W_SUB, W_ADD, W_CMP, W_LD, W_ST } wide_kind;

static uint16_t
get_wide(const cpu6809_state *s, wide_reg r)
{
	switch (r) {
	case WR_X: return s->x;
	case WR_Y: return s->y;
	case WR_U: return s->u;
	case WR_S: return s->s;
	default:   return reg_d(s);
	}
}

static void
set_wide(cpu6809_state *s, wide_reg r, uint16_t v)
{
	switch (r) {
	case WR_X: s->x = v; break;
	case WR_Y: s->y = v; break;
	case WR_U: s->u = v; break;
	case WR_S: s->s = v; break;
	default:   set_d(s, v); break;
	}
}

/*
 * Which 16-bit operation a column is, and on which register.
 *
 * ★ This function is the whole reason pages two and three exist. The 6809 has
 * more 16-bit registers than the grid has columns for, so Motorola put the
 * leftovers behind two prefixes and reused the same columns: column &C is CMPX
 * on page one, CMPY on page two and CMPS on page three. Written out as cases
 * that is legible; inferred from arithmetic on the opcode it would not be.
 */
static wide_kind
wide_op(int page, int blk, uint8_t col, wide_reg *reg)
{
	switch (col) {
	case 0x03:
		if (page == 1) {
			*reg = WR_D;
			return blk ? W_ADD : W_SUB;	/* ADDD or SUBD */
		}
		*reg = (page == 2) ? WR_D : WR_U;	/* CMPD or CMPU */
		return W_CMP;
	case 0x0c:
		if (page == 1) {
			if (blk) {
				*reg = WR_D;
				return W_LD;		/* LDD */
			}
			*reg = WR_X;
			return W_CMP;			/* CMPX */
		}
		*reg = (page == 2) ? WR_Y : WR_S;	/* CMPY or CMPS */
		return W_CMP;
	case 0x0d:
		*reg = WR_D;
		return W_ST;				/* STD; page one, block one */
	case 0x0e:
		*reg = (page == 1) ? (blk ? WR_U : WR_X) : (blk ? WR_S : WR_Y);
		return W_LD;				/* LDX LDU LDY LDS */
	case 0x0f:
		*reg = (page == 1) ? (blk ? WR_U : WR_X) : (blk ? WR_S : WR_Y);
		return W_ST;				/* STX STU STY STS */
	default:
		*reg = WR_D;
		return W_NONE;
	}
}

/*
 * &80-&FF: the regular grid. Block, mode and column between them say everything
 * about what the opcode does, so this is one function rather than 128 cases.
 */
static void
exec_grid(cpu6809_state *s, int page, uint8_t op)
{
	const int blk = (op & 0x40u) != 0;	/* 0: A and X   1: B and D or U */
	const int mode = (op >> 4) & 3;		/* 0 imm, 1 direct, 2 indexed, 3 ext */
	const uint8_t col = (uint8_t) (op & 0x0fu);
	uint8_t *acc = blk ? &s->b : &s->a;
	wide_reg wreg;
	const wide_kind wide = wide_op(page, blk, col, &wreg);
	uint16_t addr = 0;

	/* BSR and JSR sit in the middle of the grid and are neither: they take an
	   address and go there, having pushed where they were. */
	if (page == 1 && !blk && col == 0x0d) {
		if (mode == 0) {			/* BSR, an 8-bit relative */
			const int8_t off = (int8_t) fetch8(s);

			if (s->stalled || s->faulted) {
				return;
			}
			push16(s, &s->s, s->pc);
			s->pc = (uint16_t) (s->pc + off);
			return;
		}
		addr = (mode == 1) ? am_direct(s)
		     : (mode == 2) ? am_indexed(s) : am_extended(s);
		if (s->stalled || s->faulted) {
			return;
		}
		push16(s, &s->s, s->pc);
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

	if (wide != W_NONE) {
		uint16_t v;

		if (wide == W_ST) {
			const uint16_t val = get_wide(s, wreg);

			wr16(s, addr, val);
			set_nz16(s, val);
			set_flag(s, FLAG_V, 0);
			return;
		}

		v = (mode == 0) ? fetch16(s) : rd16(s, addr);
		if (s->stalled || s->faulted) {
			return;
		}
		switch (wide) {
		case W_SUB:
			set_wide(s, wreg, alu_sub16(s, get_wide(s, wreg), v));
			break;
		case W_ADD:
			set_wide(s, wreg, alu_add16(s, get_wide(s, wreg), v));
			break;
		case W_CMP:
			(void) alu_sub16(s, get_wide(s, wreg), v);
			break;
		default:	/* W_LD */
			set_wide(s, wreg, v);
			set_nz16(s, v);
			set_flag(s, FLAG_V, 0);
			break;
		}
		return;
	}

	if (col == 0x07) {			/* STA or STB */
		wr8(s, addr, *acc);
		set_nz8(s, *acc);
		set_flag(s, FLAG_V, 0);
		return;
	}

	{
		const uint8_t v = (mode == 0) ? fetch8(s) : rd8(s, addr);
		const unsigned carry = (s->cc & FLAG_C) ? 1u : 0u;

		if (s->stalled || s->faulted) {
			return;
		}
		switch (col) {
		case 0x00: *acc = alu_sub8(s, *acc, v, 0); break;	/* SUB */
		case 0x01: (void) alu_sub8(s, *acc, v, 0); break;	/* CMP */
		case 0x02: *acc = alu_sub8(s, *acc, v, carry); break;	/* SBC */
		case 0x04: *acc = alu_logic8(s, (uint8_t) (*acc & v)); break;
		case 0x05: (void) alu_logic8(s, (uint8_t) (*acc & v)); break;
		case 0x06: *acc = alu_logic8(s, v); break;		/* LD */
		case 0x08: *acc = alu_logic8(s, (uint8_t) (*acc ^ v)); break;
		case 0x09: *acc = alu_add8(s, *acc, v, carry); break;	/* ADC */
		case 0x0a: *acc = alu_logic8(s, (uint8_t) (*acc | v)); break;
		default:   *acc = alu_add8(s, *acc, v, 0); break;	/* &b ADD */
		}
	}
}

/* ---- the stack instructions, whose cost is their register list ---------- */

/*
 * PSHS and its three relations. The mask has one bit per register and they are
 * pushed from the top of the mask down, so that a PUL taking them off from the
 * bottom up gets each one back where it belongs.
 *
 * ★ The "other" stack is what the U bit means: PSHS pushes U and PSHU pushes S.
 * A register cannot be pushed onto itself, and the encoding does not let it.
 */
static void
op_push(cpu6809_state *s, uint16_t *sp, uint16_t *other, uint8_t mask)
{
	int bytes = 0;

	if (mask & 0x80u) { push16(s, sp, s->pc);  bytes += 2; }
	if (mask & 0x40u) { push16(s, sp, *other); bytes += 2; }
	if (mask & 0x20u) { push16(s, sp, s->y);   bytes += 2; }
	if (mask & 0x10u) { push16(s, sp, s->x);   bytes += 2; }
	if (mask & 0x08u) { push8(s, sp, s->dp);   bytes += 1; }
	if (mask & 0x04u) { push8(s, sp, s->b);    bytes += 1; }
	if (mask & 0x02u) { push8(s, sp, s->a);    bytes += 1; }
	if (mask & 0x01u) { push8(s, sp, s->cc);   bytes += 1; }
	s->extra_cycles += bytes;
}

static void
op_pull(cpu6809_state *s, uint16_t *sp, uint16_t *other, uint8_t mask)
{
	int bytes = 0;

	if (mask & 0x01u) { s->cc = pull8(s, sp);  bytes += 1; }
	if (mask & 0x02u) { s->a  = pull8(s, sp);  bytes += 1; }
	if (mask & 0x04u) { s->b  = pull8(s, sp);  bytes += 1; }
	if (mask & 0x08u) { s->dp = pull8(s, sp);  bytes += 1; }
	if (mask & 0x10u) { s->x  = pull16(s, sp); bytes += 2; }
	if (mask & 0x20u) { s->y  = pull16(s, sp); bytes += 2; }
	if (mask & 0x40u) { *other = pull16(s, sp); bytes += 2; }
	if (mask & 0x80u) { s->pc = pull16(s, sp); bytes += 2; }
	s->extra_cycles += bytes;
}

/* ---- EXG and TFR, which name their registers in a postbyte -------------- */

/*
 * ★ MIXING WIDTHS FAULTS RATHER THAN GUESSING. A postbyte can ask for A to be
 * transferred into X, and a real 6809 does something with that - which half it
 * lands in, and what the other half becomes, is not documented and differs
 * between accounts. Reproducing one account of it would make software that
 * relies on it look portable. No assembler emits it and no program needs it.
 */
#define TFR_IS_16(code)	((code) <= 5u)

static int
tfr_read(cpu6809_state *s, uint8_t code, uint16_t *out)
{
	switch (code) {
	case 0x0: *out = reg_d(s); return 1;
	case 0x1: *out = s->x; return 1;
	case 0x2: *out = s->y; return 1;
	case 0x3: *out = s->u; return 1;
	case 0x4: *out = s->s; return 1;
	case 0x5: *out = s->pc; return 1;
	case 0x8: *out = s->a; return 1;
	case 0x9: *out = s->b; return 1;
	case 0xa: *out = s->cc; return 1;
	case 0xb: *out = s->dp; return 1;
	default:  return 0;			/* the codes 6, 7 and c-f do not exist */
	}
}

static void
tfr_write(cpu6809_state *s, uint8_t code, uint16_t val)
{
	switch (code) {
	case 0x0: set_d(s, val); break;
	case 0x1: s->x = val; break;
	case 0x2: s->y = val; break;
	case 0x3: s->u = val; break;
	case 0x4: s->s = val; break;
	case 0x5: s->pc = val; break;
	case 0x8: s->a = (uint8_t) val; break;
	case 0x9: s->b = (uint8_t) val; break;
	case 0xa: s->cc = (uint8_t) val; break;
	default:  s->dp = (uint8_t) val; break;
	}
}

static void
op_tfr_exg(cpu6809_state *s, int exchange)
{
	const uint8_t pb = fetch8(s);
	const uint8_t src = (uint8_t) (pb >> 4);
	const uint8_t dst = (uint8_t) (pb & 0x0fu);
	uint16_t a, b;

	if (s->stalled || s->faulted) {
		return;
	}
	if (!tfr_read(s, src, &a) || !tfr_read(s, dst, &b) ||
	    TFR_IS_16(src) != TFR_IS_16(dst)) {
		fault(s, CPU6809_FAULT_POSTBYTE, pb);
		return;
	}
	tfr_write(s, dst, a);
	if (exchange) {
		tfr_write(s, src, b);
	}
}

/* ---- the branch conditions, shared by the short and long forms ---------- */

static int
branch_taken(const cpu6809_state *s, uint8_t cond)
{
	const int c = (s->cc & FLAG_C) != 0;
	const int v = (s->cc & FLAG_V) != 0;
	const int z = (s->cc & FLAG_Z) != 0;
	const int n = (s->cc & FLAG_N) != 0;

	switch (cond & 0x0fu) {
	case 0x0: return 1;			/* BRA */
	case 0x1: return 0;			/* BRN, which exists to be patched */
	case 0x2: return !c && !z;		/* BHI */
	case 0x3: return c || z;		/* BLS */
	case 0x4: return !c;			/* BCC, BHS */
	case 0x5: return c;			/* BCS, BLO */
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

/* ---- &00-&3F, the part of the map that is not a grid -------------------- */

static void
exec_low(cpu6809_state *s, uint8_t op)
{
	if (op < 0x10u) {			/* the RMW column, direct */
		const uint16_t addr = am_direct(s);

		if (!s->stalled && !s->faulted) {
			rmw_memory(s, (uint8_t) (op & 0x0fu), addr);
		}
		return;
	}
	if (op >= 0x20u && op < 0x30u) {	/* the short branches */
		const int8_t off = (int8_t) fetch8(s);

		if (s->stalled || s->faulted) {
			return;
		}
		if (branch_taken(s, op)) {
			s->pc = (uint16_t) (s->pc + off);
		}
		/* Deliberately not recording it: a short branch costs three
		   cycles whether it is taken or not. */
		return;
	}
	if (op >= 0x40u && op < 0x60u) {	/* inherent, on A then on B */
		rmw_accumulator(s, (uint8_t) (op & 0x0fu),
		                (op < 0x50u) ? &s->a : &s->b);
		return;
	}
	if (op >= 0x60u) {			/* the RMW column, indexed then ext */
		const uint16_t addr = (op < 0x70u) ? am_indexed(s)
		                                   : am_extended(s);

		if (!s->stalled && !s->faulted) {
			rmw_memory(s, (uint8_t) (op & 0x0fu), addr);
		}
		return;
	}

	switch (op) {
	case 0x12:				/* NOP */
		break;

	case 0x13:				/* SYNC */
	case 0x3c:				/* CWAI */
		/* Both stop the processor until an interrupt arrives. This card
		   runs a core for a bounded slice and has nothing to model a
		   halted-but-not-stopped processor with, so saying so is better
		   than pretending: see cpu_6809.h, and WAI on the 65C02, which is
		   left out for the same reason. CWAI's own operand is consumed
		   first so the fault address points at the instruction and not
		   into the middle of it. */
		if (op == 0x3c) {
			(void) fetch8(s);
		}
		fault(s, CPU6809_FAULT_WAIT, op);
		break;

	case 0x16: {				/* LBRA */
		const int16_t off = (int16_t) fetch16(s);

		if (!s->stalled && !s->faulted) {
			s->pc = (uint16_t) (s->pc + off);
		}
		break;
	}
	case 0x17: {				/* LBSR */
		const int16_t off = (int16_t) fetch16(s);

		if (!s->stalled && !s->faulted) {
			push16(s, &s->s, s->pc);
			s->pc = (uint16_t) (s->pc + off);
		}
		break;
	}

	case 0x19:				/* DAA */
		op_daa(s);
		break;

	case 0x1a:				/* ORCC */
		s->cc |= fetch8(s);
		break;
	case 0x1c:				/* ANDCC */
		s->cc = (uint8_t) (s->cc & fetch8(s));
		break;

	case 0x1d:				/* SEX: B's sign fills A */
		s->a = (uint8_t) ((s->b & 0x80u) ? 0xffu : 0x00u);
		set_nz16(s, reg_d(s));
		break;

	case 0x1e:				/* EXG */
		op_tfr_exg(s, 1);
		break;
	case 0x1f:				/* TFR */
		op_tfr_exg(s, 0);
		break;

	case 0x30:				/* LEAX */
	case 0x31: {				/* LEAY */
		const uint16_t addr = am_indexed(s);

		if (s->stalled || s->faulted) {
			break;
		}
		if (op == 0x30) {
			s->x = addr;
		} else {
			s->y = addr;
		}
		/* ★ LEAX and LEAY set Z and the other two do not, which looks
		   arbitrary and is not: these two are how a program walks a list,
		   so "did that come out zero" is worth having, and S and U are
		   stack pointers whose adjustment must not disturb a flag a
		   caller is about to test. */
		set_flag(s, FLAG_Z, addr == 0);
		break;
	}
	case 0x32:				/* LEAS */
	case 0x33: {				/* LEAU */
		const uint16_t addr = am_indexed(s);

		if (s->stalled || s->faulted) {
			break;
		}
		if (op == 0x32) {
			s->s = addr;
		} else {
			s->u = addr;
		}
		break;
	}

	case 0x34:				/* PSHS */
		op_push(s, &s->s, &s->u, fetch8(s));
		break;
	case 0x35:				/* PULS */
		op_pull(s, &s->s, &s->u, fetch8(s));
		break;
	case 0x36:				/* PSHU */
		op_push(s, &s->u, &s->s, fetch8(s));
		break;
	case 0x37:				/* PULU */
		op_pull(s, &s->u, &s->s, fetch8(s));
		break;

	case 0x39:				/* RTS */
		s->pc = pull16(s, &s->s);
		break;

	case 0x3a:				/* ABX: B is unsigned here */
		s->x = (uint16_t) (s->x + s->b);
		break;			/* and it touches no flag at all */

	case 0x3b:				/* RTI */
		s->cc = pull8(s, &s->s);
		if ((s->cc & FLAG_E) != 0) {
			s->a = pull8(s, &s->s);
			s->b = pull8(s, &s->s);
			s->dp = pull8(s, &s->s);
			s->x = pull16(s, &s->s);
			s->y = pull16(s, &s->s);
			s->u = pull16(s, &s->s);
			/* Nine more cycles for the nine more bytes. The E flag
			   just pulled is what decides it, so the table cannot. */
			s->extra_cycles += 9;
		}
		s->pc = pull16(s, &s->s);
		break;

	case 0x3d: {				/* MUL */
		const uint16_t d = (uint16_t) (s->a * s->b);

		set_d(s, d);
		set_flag(s, FLAG_Z, d == 0);
		/* Bit 7 of the low half, which is what a program rounds on. */
		set_flag(s, FLAG_C, (d & 0x80u) != 0);
		break;
	}

	case 0x3f:				/* SWI: the program is finished */
		s->halted = 1;
		s->halt_reason = CPU6809_HALT_SWI;
		s->exit_code = s->a;
		break;

	default:
		/* Unreachable: step() has already refused any opcode the cycle
		   table has no entry for, and every entry it has is above. */
		fault(s, CPU6809_FAULT_ILLEGAL, op);
		break;
	}
}

/* ---- pages two and three ------------------------------------------------ */

static void
exec_prefixed(cpu6809_state *s, int page, uint8_t op)
{
	if (op >= 0x20u && op < 0x30u) {	/* the long branches */
		const int16_t off = (int16_t) fetch16(s);

		if (s->stalled || s->faulted) {
			return;
		}
		if (branch_taken(s, op)) {
			s->pc = (uint16_t) (s->pc + off);
			/* Unlike a short branch, a long one taken costs a cycle
			   more than one that is not. */
			s->branch_taken = 1;
		}
		return;
	}
	if (op == 0x3fu) {			/* SWI2 and SWI3 */
		/* Unlike SWI, these vector. They are the two a guest operating
		   system claims, and a program calling one expects to come back
		   from it; neither sets a mask, for the same reason. */
		take_interrupt(s, (page == 2) ? CPU6809_VEC_SWI2
		                              : CPU6809_VEC_SWI3, 1, 0);
		return;
	}
	exec_grid(s, page, op);
}

/* ------------------------------------------------------------ step and run */

static int
abandon(cpu6809_state *s)
{
	s->a = s->saved.a;
	s->b = s->saved.b;
	s->dp = s->saved.dp;
	s->cc = s->saved.cc;
	s->x = s->saved.x;
	s->y = s->saved.y;
	s->u = s->saved.u;
	s->s = s->saved.s;
	s->pc = s->saved.pc;
	return 0;
}

int
cpu6809_step(cpu6809_state *s)
{
	const uint8_t *cycles = cycles_page1;
	int page = 1;
	uint8_t op;

	if (s->halted || s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return 0;		/* waiting for the guest to answer */
	}

	/* Cleared per instruction: whatever the operand's decode or the register
	   list charged is worked out while the instruction runs and read once at
	   the end. */
	s->extra_cycles = 0;
	s->branch_taken = 0;

	/* Only worth saving when something behind the hook can stall: with a
	   plain array, or a map with no stalling region in it, an instruction is
	   never abandoned and this would be eleven bytes copied per instruction
	   for nothing. */
	if (s->mem.can_stall) {
		s->saved.a = s->a;
		s->saved.b = s->b;
		s->saved.dp = s->dp;
		s->saved.cc = s->cc;
		s->saved.x = s->x;
		s->saved.y = s->y;
		s->saved.u = s->u;
		s->saved.s = s->s;
		s->saved.pc = s->pc;
	}

	op = fetch8(s);
	if (s->faulted) {
		return 0;		/* the program counter left its memory */
	}
	if (s->stalled) {
		return abandon(s);
	}

	if (op == 0x10u || op == 0x11u) {
		page = (op == 0x10u) ? 2 : 3;
		cycles = (page == 2) ? cycles_page2 : cycles_page3;
		op = fetch8(s);
		if (s->faulted) {
			return 0;
		}
		if (s->stalled) {
			return abandon(s);
		}
	}

	/*
	 * ★ THE CYCLE TABLE IS ALSO THE LIST OF WHAT EXISTS. A zero entry means
	 * the opcode is not an instruction, on this page, and it is refused here
	 * rather than in each of the four decoders below. One table cannot
	 * disagree with itself, where a separate list of valid opcodes would
	 * eventually disagree with the timing and let something execute that a
	 * real part faults on.
	 */
	if (cycles[op] == 0) {
		/* The prefix is kept in the reported value so a log can tell
		   &1083 from &83. */
		fault(s, CPU6809_FAULT_ILLEGAL,
		      (page == 1) ? op : (uint32_t) (((page == 2 ? 0x10u : 0x11u)
		                                      << 8) | op));
		return 0;
	}

	if (page == 1) {
		if (op >= 0x80u) {
			exec_grid(s, 1, op);
		} else {
			exec_low(s, op);
		}
	} else {
		exec_prefixed(s, page, op);
	}

	if (s->faulted) {
		return 0;
	}
	/* An access anywhere in the instruction may have stalled: the handlers
	   above do not check on the way out, so this is the one place that does.
	   The instruction is abandoned whole and retried, never half-counted. */
	if (s->stalled) {
		return abandon(s);
	}

	{
		const int n = cycles[op] + s->extra_cycles +
		    (s->branch_taken ? 1 : 0);

		s->cycles += (uint64_t) n;
		return n;
	}
}

int
cpu6809_run(cpu6809_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = cpu6809_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}
