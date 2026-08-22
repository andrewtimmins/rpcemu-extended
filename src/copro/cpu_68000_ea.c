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
 * cpu_68000_ea.c - the twelve addressing modes, and what reaching an operand
 * costs.
 *
 * ★ WHY THIS IS ITS OWN FILE. On a 6502 or a Z80 the addressing mode is part of
 * the opcode and its cost is in the opcode's table entry. On a 68000 the mode is
 * a six-bit field that nearly every instruction carries, so the same MOVE costs
 * four cycles or twenty depending only on how its operands are named. That makes
 * the cost of reaching an operand a thing in its own right rather than a property
 * of the instruction, which is the same shape as the 6809's postbyte and about
 * four times the size.
 *
 * The mode field is three bits of mode and three of register, and mode 7 uses the
 * register field to select between five more modes:
 *
 *   0 Dn            4 -(An)          7/0 (xxx).W
 *   1 An            5 (d16,An)       7/1 (xxx).L
 *   2 (An)          6 (d8,An,Xn)     7/2 (d16,PC)
 *   3 (An)+                          7/3 (d8,PC,Xn)
 *                                    7/4 #imm
 */

#include "cpu_68000_priv.h"

/*
 * Effective address calculation times, in clock periods: byte or word first,
 * then long. Indexed by the mode field, with mode 7's five sub-modes appended.
 *
 * These are the documented figures and they are not derivable from anything, so
 * unlike the 6809's and 6800's tables they are transcribed - but there are
 * twenty-four numbers rather than seven hundred, and the vector suite checks them
 * against a different implementation. See docs/copro-68000.md.
 */
#define EA_ABS_W	8	/* index into the table for mode 7's sub-modes */
#define EA_ABS_L	9
#define EA_PC_DISP	10
#define EA_PC_INDEX	11
#define EA_IMMEDIATE	12

static const uint8_t ea_cycles[13][2] = {
	{  0,  0 },	/* 0   Dn            */
	{  0,  0 },	/* 1   An            */
	{  4,  8 },	/* 2   (An)          */
	{  4,  8 },	/* 3   (An)+         */
	{  6, 10 },	/* 4   -(An)         */
	{  8, 12 },	/* 5   (d16,An)      */
	{ 10, 14 },	/* 6   (d8,An,Xn)    */
	{  0,  0 },	/* 7   unused: the sub-modes are below */
	{  8, 12 },	/* 7/0 (xxx).W       */
	{ 12, 16 },	/* 7/1 (xxx).L       */
	{  8, 12 },	/* 7/2 (d16,PC)      */
	{ 10, 14 },	/* 7/3 (d8,PC,Xn)    */
	{  4,  8 },	/* 7/4 #imm          */
};

/*
 * ★ PREDECREMENT COSTS LESS AS A DESTINATION THAN AS A SOURCE, which looks like
 * an error in the table and is not.
 *
 * As a source, -(An) costs 6: the register is decremented and then read. As the
 * destination of a MOVE it costs 4, because the part overlaps the decrement with
 * the write it was going to do anyway. Every other mode costs the same either
 * way.
 *
 * The arithmetic is checkable against the documented MOVE times:
 *   MOVE.W Dn,-(An)    is  8  = 4 + 0 + 4, and would be 10 with the source cost
 *   MOVE.W -(An),-(An) is 14  = 4 + 6 + 4, which needs both numbers to be right
 */
static int
ea_cost(unsigned mode, unsigned reg, m68k_size size, int is_dest)
{
	const int longer = (size == M68K_LONG) ? 1 : 0;
	unsigned row = mode;

	if (mode == 7) {
		row = EA_ABS_W + (reg & 7u);
		if (row > EA_IMMEDIATE) {
			return 0;	/* not a mode; the caller has already refused it */
		}
	}
	if (mode == 4 && is_dest) {
		return longer ? 8 : 4;
	}
	return ea_cycles[row][longer];
}

/*
 * ★ A BYTE ACCESS THROUGH A7 MOVES IT BY TWO, NOT ONE.
 *
 * The stack pointer must stay even, because a word or long pushed onto an odd
 * stack would take an address error - and the part enforces that rather than
 * leaving it to the programmer: (A7)+ and -(A7) adjust by two even for a byte.
 * A core that moved it by one would build a stack that works until the first
 * word is pushed onto it, which is a fault a long way from its cause.
 */
static uint32_t
step_size(unsigned reg, m68k_size size)
{
	if (size == M68K_BYTE && reg == 7) {
		return 2;
	}
	return (uint32_t) size;
}

/*
 * The brief extension word, used by (d8,An,Xn) and (d8,PC,Xn):
 *
 *   bit 15     0 the index is a data register, 1 an address register
 *   bits 14-12 which register
 *   bit 11     0 the index is a sign-extended word, 1 a long
 *   bits 10-8  a scale factor on the 68020 and later; must be zero here
 *   bits 7-0   a signed byte of displacement
 */
static uint32_t
brief_extension(cpu68000_state *s, uint32_t base)
{
	const uint16_t ext = m68k_fetch16(s);
	const unsigned reg = (ext >> 12) & 7u;
	uint32_t index;

	if (M68K_ABORTED(s)) {
		return 0;
	}

	index = ((ext & 0x8000u) != 0) ? s->a[reg] : s->d[reg];
	if ((ext & 0x0800u) == 0) {
		index = m68k_extend(index & 0xffffu, M68K_WORD);
	}
	return base + index + m68k_extend(ext & 0xffu, M68K_BYTE);
}

void
m68k_ea_calc(cpu68000_state *s, unsigned mode, unsigned reg, m68k_size size,
             m68k_ea *ea)
{
	ea->reg = reg;
	ea->addr = 0;
	s->extra_cycles += ea_cost(mode, reg, size, 0);

	switch (mode) {
	case 0:
		ea->kind = M68K_EA_DATA_REG;
		return;
	case 1:
		ea->kind = M68K_EA_ADDR_REG;
		return;
	case 2:
		ea->kind = M68K_EA_MEMORY;
		ea->addr = s->a[reg];
		return;
	case 3:		/* (An)+ : read from it, then advance */
		ea->kind = M68K_EA_MEMORY;
		ea->addr = s->a[reg];
		s->a[reg] = (s->a[reg] + step_size(reg, size)) & M68K_ADDR_MASK;
		return;
	case 4:		/* -(An) : back up first, then read from it */
		ea->kind = M68K_EA_MEMORY;
		s->a[reg] = (s->a[reg] - step_size(reg, size)) & M68K_ADDR_MASK;
		ea->addr = s->a[reg];
		return;
	case 5: {
		const uint16_t d = m68k_fetch16(s);

		ea->kind = M68K_EA_MEMORY;
		ea->addr = s->a[reg] + m68k_extend(d, M68K_WORD);
		return;
	}
	case 6:
		ea->kind = M68K_EA_MEMORY;
		ea->addr = brief_extension(s, s->a[reg]);
		return;
	default:
		break;
	}

	switch (reg) {
	case 0:		/* (xxx).W, sign-extended: the top of memory is reachable */
		ea->kind = M68K_EA_MEMORY;
		ea->addr = m68k_extend(m68k_fetch16(s), M68K_WORD);
		return;
	case 1:
		ea->kind = M68K_EA_MEMORY;
		ea->addr = m68k_fetch32(s);
		return;
	case 2: {
		/*
		 * ★ Relative to where the extension word IS, not to where the
		 * instruction started or to what follows. An assembler computes
		 * the displacement on that basis, so the two must agree, and the
		 * program counter has to be read before the fetch advances it.
		 */
		const uint32_t at = s->pc;
		const uint16_t d = m68k_fetch16(s);

		ea->kind = M68K_EA_MEMORY;
		ea->addr = at + m68k_extend(d, M68K_WORD);
		return;
	}
	case 3: {
		const uint32_t at = s->pc;

		ea->kind = M68K_EA_MEMORY;
		ea->addr = brief_extension(s, at);
		return;
	}
	default:	/* #imm, and the operand is in the instruction stream */
		ea->kind = M68K_EA_IMMEDIATE;
		if (size == M68K_LONG) {
			ea->addr = m68k_fetch32(s);
		} else {
			/* A byte immediate still occupies a whole word, with the
			   byte in the low half. */
			ea->addr = m68k_fetch16(s) & m68k_mask(size);
		}
		return;
	}
}

uint32_t
m68k_ea_read(cpu68000_state *s, const m68k_ea *ea, m68k_size size)
{
	switch (ea->kind) {
	case M68K_EA_DATA_REG:
		return s->d[ea->reg] & m68k_mask(size);
	case M68K_EA_ADDR_REG:
		return s->a[ea->reg] & m68k_mask(size);
	case M68K_EA_IMMEDIATE:
		return ea->addr & m68k_mask(size);
	default:
		return m68k_read(s, ea->addr, size);
	}
}

void
m68k_ea_write(cpu68000_state *s, const m68k_ea *ea, m68k_size size, uint32_t val)
{
	switch (ea->kind) {
	case M68K_EA_DATA_REG: {
		/*
		 * ★ A partial write to a data register leaves the rest of it
		 * alone. MOVE.B into D0 changes eight bits and nothing else, so
		 * the other twenty-four have to be preserved rather than cleared.
		 */
		const uint32_t m = m68k_mask(size);

		s->d[ea->reg] = (s->d[ea->reg] & ~m) | (val & m);
		return;
	}
	case M68K_EA_ADDR_REG:
		/*
		 * ★ AND A PARTIAL WRITE TO AN ADDRESS REGISTER DOES NOT. Writing
		 * a word to An sign-extends it across all 32 bits, because an
		 * address register holds an address and half an address is not
		 * useful. This is one of the two places the two register files
		 * genuinely differ in behaviour rather than in name.
		 */
		s->a[ea->reg] = m68k_extend(val, size);
		return;
	case M68K_EA_IMMEDIATE:
		/* Unreachable: the caller refuses this mode as a destination. */
		return;
	default:
		m68k_write(s, ea->addr, size, val);
		return;
	}
}

int
m68k_ea_is_memory(unsigned mode, unsigned reg)
{
	if (mode == 0 || mode == 1) {
		return 0;
	}
	if (mode == 7 && reg == 4) {
		return 0;	/* immediate data is not memory */
	}
	return 1;
}

int
m68k_ea_is_writable(unsigned mode, unsigned reg)
{
	if (mode != 7) {
		return 1;
	}
	/* Absolute addresses can be written; program-counter relative and
	   immediate cannot, so the encodings that would are not instructions. */
	return (reg == 0 || reg == 1);
}
