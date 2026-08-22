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

/* One place for what a hooked access means, used by memory and ports alike:
   @addr already carries CPU_MEM_PORT when it is a port. */
static uint8_t
hooked_read(cpu_z80_state *s, uint32_t addr)
{
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
		fault(s, CPU_Z80_FAULT_ACCESS, addr & 0xffffu);
		return 0;
	}
}

static void
hooked_write(cpu_z80_state *s, uint32_t addr, uint8_t val)
{
	switch (s->mem.write(s->mem.ctx, addr, val)) {
	case CPU_MEM_OK:
		return;
	case CPU_MEM_STALL:
		s->stalled = 1;
		s->stall_addr = addr;
		s->stall_is_write = 1;
		return;
	default:
		fault(s, CPU_Z80_FAULT_ACCESS, addr & 0xffffu);
		return;
	}
}

static uint8_t
rd8(cpu_z80_state *s, uint16_t addr)
{
	if (s->mem.read != NULL) {
		return hooked_read(s, addr);
	}
	if (s->ram == NULL || addr >= s->ram_size) {
		fault(s, CPU_Z80_FAULT_ACCESS, addr);
		return 0;
	}
	return s->ram[addr];
}

static void
wr8(cpu_z80_state *s, uint16_t addr, uint8_t val)
{
	if (s->mem.write != NULL) {
		hooked_write(s, addr, val);
		return;
	}
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


/*
 * ★ T-STATES, NOT INSTRUCTIONS.
 *
 * A Spectrum frame is 69888 T-states and its border effects are timed in them,
 * so a program emulating one needs the processor's own clock rather than a count
 * of instructions.
 *
 * ★ HOW THIS TABLE WAS ARRIVED AT, because a table of 256 numbers written from
 * memory is a table with mistakes in it. The Z80's timing has a documented
 * structure - four T-states for the opcode fetch, three for every further
 * instruction byte, three for every byte of data read or written, plus a
 * documented extra for certain classes - so the table was DERIVED from the
 * accesses this core actually makes for each opcode, and then checked against
 * forty published values (NOP 4, LD BC,nn 10, EX (SP),HL 19, CALL 17, OUT 11
 * and so on). All forty agreed.
 *
 * The value here is the cost when a conditional instruction is NOT taken;
 * cycles_taken below is what a taken one adds.
 *
 * ★ THE CONDITIONALS ARE NOT DERIVED, they are the documented figures written in
 * directly - JR cc 7/12, DJNZ 8/13, RET cc 5/11, CALL cc 10/17, JP cc 10 either
 * way. Deriving them from a probe run gave a table that was right for half of
 * them and wrong for the other half, because a reset leaves the flags all ones,
 * so the Z-set forms were measured taken and the NZ forms not. The first sample
 * check agreed with it, because it only tested one polarity of each. Both are
 * tested now.
 */
static const uint8_t cycles_main[256] = {
	 4, 10,  7,  6,  4,  4,  7,  4,  4, 11,  7,  6,  4,  4,  7,  4,	/* 0x */
	 8, 10,  7,  6,  4,  4,  7,  4, 12, 11,  7,  6,  4,  4,  7,  4,	/* 1x */
	 7, 10, 16,  6,  4,  4,  7,  4,  7, 11, 16,  6,  4,  4,  7,  4,	/* 2x */
	 7, 10, 13,  6, 11, 11, 10,  4,  7, 11, 13,  6,  4,  4,  7,  4,	/* 3x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* 4x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* 5x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* 6x */
	 7,  7,  7,  7,  7,  7,  4,  7,  4,  4,  4,  4,  4,  4,  7,  4,	/* 7x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* 8x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* 9x */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* ax */
	 4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,	/* bx */
	 5, 10, 10, 10, 10, 11,  7, 11,  5, 10, 10,  7, 10, 17,  7, 11,	/* cx */
	 5, 10, 10, 11, 10, 11,  7, 11,  5,  4, 10, 11, 10,  7,  7, 11,	/* dx */
	 5, 10, 10, 19, 10, 11,  7, 11,  5,  4, 10,  4, 10,  4,  7, 11,	/* ex */
	 5, 10, 10,  4, 10, 11,  7, 11,  5,  6, 10,  4, 10,  7,  7, 11,	/* fx */
};


/*
 * ★ WHERE AN 8080 IS TIMED DIFFERENTLY FROM A Z80.
 *
 * Zero means "as the Z80 table above", which is most of the map: the two parts
 * agree on every immediate load, every ALU operation, the jumps, CALL, RET, the
 * stack and the restarts. What differs is a short and reviewable list - MOV
 * between registers is five rather than four, INX and DCX five rather than six,
 * INR and DCR of a register five rather than four, DAD ten rather than eleven,
 * XTHL eighteen rather than nineteen, IN and OUT ten rather than eleven, HLT
 * seven rather than four, and a conditional CALL not taken eleven rather than
 * ten.
 *
 * Generated from that list rather than typed out, and checked against
 * thirty-eight published 8080 figures; each entry was also checked to differ
 * from the Z80's value, because a "delta" that matches is not a delta and being
 * in the list would be a mistake.
 */
static const uint8_t cycles_8080[256] = {
	 0,  0,  0,  5,  5,  5,  0,  0,  0, 10,  0,  5,  5,  5,  0,  0,	/* 0x */
	 0,  0,  0,  5,  5,  5,  0,  0,  0, 10,  0,  5,  5,  5,  0,  0,	/* 1x */
	 0,  0,  0,  5,  5,  5,  0,  0,  0, 10,  0,  5,  5,  5,  0,  0,	/* 2x */
	 0,  0,  0,  5, 10, 10,  0,  0,  0, 10,  0,  5,  5,  5,  0,  0,	/* 3x */
	 5,  5,  5,  5,  5,  5,  0,  5,  5,  5,  5,  5,  5,  5,  0,  5,	/* 4x */
	 5,  5,  5,  5,  5,  5,  0,  5,  5,  5,  5,  5,  5,  5,  0,  5,	/* 5x */
	 5,  5,  5,  5,  5,  5,  0,  5,  5,  5,  5,  5,  5,  5,  0,  5,	/* 6x */
	 0,  0,  0,  0,  0,  0,  7,  0,  5,  5,  5,  5,  5,  5,  0,  5,	/* 7x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 8x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 9x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* ax */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* bx */
	 0,  0,  0,  0, 11,  0,  0,  0,  0,  0,  0,  0, 11,  0,  0,  0,	/* cx */
	 0,  0,  0, 10, 11,  0,  0,  0,  0,  0,  0, 10, 11,  0,  0,  0,	/* dx */
	 0,  0,  0, 18, 11,  0,  0,  0,  0,  5,  0,  0, 11,  0,  0,  0,	/* ex */
	 0,  0,  0,  0, 11,  0,  0,  0,  0,  5,  0,  0, 11,  0,  0,  0,	/* fx */
};

/* What a taken conditional adds: JR cc becomes 12, DJNZ 13, RET cc 11,
   CALL cc 17. */
static const uint8_t cycles_taken[256] = {
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 0x */
	 5,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 1x */
	 5,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,	/* 2x */
	 5,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,	/* 3x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 4x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 5x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 6x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 7x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 8x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* 9x */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* ax */
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,	/* bx */
	 6,  0,  0,  0,  7,  0,  0,  0,  6,  0,  0,  0,  7,  0,  0,  0,	/* cx */
	 6,  0,  0,  0,  7,  0,  0,  0,  6,  0,  0,  0,  7,  0,  0,  0,	/* dx */
	 6,  0,  0,  0,  7,  0,  0,  0,  6,  0,  0,  0,  7,  0,  0,  0,	/* ex */
	 6,  0,  0,  0,  7,  0,  0,  0,  6,  0,  0,  0,  7,  0,  0,  0,	/* fx */
};

/*
 * The prefixed pages.
 *
 *   DD and FD add four for the prefix itself, and eight more when a
 *   displacement byte is fetched and added - so LD A,(IX+d) is LD A,(HL)'s
 *   seven plus twelve, which is the documented nineteen. LD (IX+d),n is the one
 *   documented exception to that: nineteen rather than twenty-two, because it
 *   overlaps the displacement fetch with the operand.
 *
 *   CB is eight for a register operation, twelve for BIT b,(HL) and fifteen for
 *   any other (HL) form. Under DD or FD it is twenty for BIT and twenty-three
 *   otherwise, whatever the operation.
 */
#define Z80_PREFIX_COST		4
#define Z80_DISPLACEMENT_COST	8
#define Z80_LD_IXD_N_COST	5	/* the exception noted above */
#define Z80_CB_REG		8
#define Z80_CB_BIT_HL		12
#define Z80_CB_HL		15
#define Z80_DDCB_BIT		20
#define Z80_DDCB_OTHER		23

/* ------------------------------------------------------------ input/output */

/*
 * The memory hook takes precedence over io_in/io_out, because a machine being
 * emulated describes its ports in the same region table as its memory and
 * having two ways in would mean two answers to which one wins. The older pair
 * stays for a card that only wants a mailbox on port 0.
 */
static uint8_t
io_read(cpu_z80_state *s, uint8_t port)
{
	if (s->mem.read != NULL) {
		return hooked_read(s, (uint32_t) port | CPU_MEM_PORT);
	}
	if (s->io_in != NULL) {
		return s->io_in(s->io_ctx, port);
	}
	return s->ports[port];
}

static void
io_write(cpu_z80_state *s, uint8_t port, uint8_t val)
{
	if (s->mem.write != NULL) {
		hooked_write(s, (uint32_t) port | CPU_MEM_PORT, val);
		return;
	}
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

/*
 * ★ WHERE THE 8080 AND THE Z80 PART COMPANY, in one place.
 *
 * @res is the byte the flags describe and @arithmetic says whether the
 * parity/overflow bit currently holds overflow. On an 8080 that bit is always
 * parity - the Z80 redefined it for arithmetic and kept parity for logic - so
 * every add, subtract, increment and decrement reports something different on
 * the two parts, and software that tests it after arithmetic behaves differently
 * on each.
 *
 * The undocumented bits differ too: an 8080 reads bit 1 as one and bits 3 and 5
 * as zero, which anything doing PUSH PSW can see.
 */
static void
store_flags(cpu_z80_state *s, uint8_t f, uint8_t res, int arithmetic)
{
	if (s->i8080) {
		if (arithmetic) {
			f &= (uint8_t) ~FLAG_PV;
			if (parity_even(res)) {
				f |= FLAG_PV;
			}
		}
		f &= (uint8_t) ~FLAG_XY;
		f |= 0x02u;
	}
	s->f = f;
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
	store_flags(s, f, r8, 1);
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
	/* CP's flags describe the subtraction it threw away, so the parity an 8080
	   reports is of that result rather than of the accumulator. */
	store_flags(s, f, r8, 1);
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
	/* Not arithmetic: the Z80 reports parity here too, so only the 8080's
	   fixed bits differ. */
	store_flags(s, f, res, 0);
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
	store_flags(s, f, r, 1);
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
	store_flags(s, f, r, 1);
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
	/* ★ An 8080 has no subtract flag, so its DAA always adds. Consulting the
	   Z80's N bit here would make an 8080 program's decimal arithmetic depend
	   on a flag its own instructions never set. */
	const int subtract = !s->i8080 && (s->f & FLAG_N) != 0;
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
	store_flags(s, f, res, 0);	/* DAA reports parity on both parts */
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

static void ed_page_charge(cpu_z80_state *s, uint8_t op);

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
			/* Twenty-one rather than sixteen: an iteration with more
			   to do costs five more than the last one. Charged here
			   because ed_page returns before its own tail for this
			   family, which is why setting a flag for the tail to
			   read did nothing. */
			s->last_cycles = 21;
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
			/* Twenty-one rather than sixteen: an iteration with more
			   to do costs five more than the last one. Charged here
			   because ed_page returns before its own tail for this
			   family, which is why setting a flag for the tail to
			   read did nothing. */
			s->last_cycles = 21;
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
			/* Twenty-one rather than sixteen: an iteration with more
			   to do costs five more than the last one. Charged here
			   because ed_page returns before its own tail for this
			   family, which is why setting a flag for the tail to
			   read did nothing. */
			s->last_cycles = 21;
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
			/* Twenty-one rather than sixteen: an iteration with more
			   to do costs five more than the last one. Charged here
			   because ed_page returns before its own tail for this
			   family, which is why setting a flag for the tail to
			   read did nothing. */
			s->last_cycles = 21;
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

	ed_page_charge(s, op);
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

/*
 * The ED page's cost, worked out from the opcode before it executes so that a
 * path which returns early still leaves a figure behind. Eight T-states for the
 * two fetches, and then per class as documented: an input or output is twelve,
 * sixteen-bit arithmetic fifteen, a sixteen-bit load through an address twenty,
 * a block instruction sixteen, and the nibble rotates eighteen.
 */
static void
ed_page_charge(cpu_z80_state *s, uint8_t op)
{
	const unsigned x = op >> 6;
	const unsigned y = (op >> 3) & 7u;
	const unsigned z = op & 7u;

	if (x == 2) {
		s->last_cycles = 16;
	} else if (x == 1) {
		switch (z) {
		case 0:				/* IN r,(C)  */
		case 1:	s->last_cycles = 12; break;	/* OUT (C),r */
		case 2:	s->last_cycles = 15; break;	/* SBC/ADC HL,rr */
		case 3:	s->last_cycles = 20; break;	/* LD (nn),rr, LD rr,(nn) */
		case 4:	s->last_cycles = 8;  break;	/* NEG */
		case 5:	s->last_cycles = 14; break;	/* RETN, RETI */
		case 6:	s->last_cycles = 8;  break;	/* IM n */
		default:
			/* RRD and RLD, against LD I,A and its relatives. */
			s->last_cycles = (y >= 4) ? 18 : 9;
			break;
		}
	} else {
		s->last_cycles = 8;
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

	/* Charged before the operation runs, from the rule in the note above the
	   tables: eight for a register form, more for an (HL) one, and more again
	   under an index prefix. Before, because this function returns early for an
	   undefined encoding and a cost still has to have been recorded. */

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
		s->last_cycles = (x == 1) ? Z80_DDCB_BIT : Z80_DDCB_OTHER;
	} else if (z == 6) {
		s->last_cycles = (x == 1) ? Z80_CB_BIT_HL : Z80_CB_HL;
	} else {
		s->last_cycles = Z80_CB_REG;
	}

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
					s->branch_taken = 1;
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
					s->branch_taken = 1;
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
				s->branch_taken = 1;
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
					s->branch_taken = 1;
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
					s->branch_taken = 1;
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

	/*
	 * What the instruction cost. The prefix and any displacement are charged
	 * here rather than inside the decode, because they are properties of how
	 * the instruction was reached rather than of what it did.
	 */
	/*
	 * CB is a page of its own and has already charged itself a figure that
	 * includes the prefix and the displacement, so nothing here applies to it:
	 * adding the prefix cost again made RLC (IX+d) thirty-five rather than
	 * twenty-three.
	 */
	if (op != 0xcbu) {
		/* The 8080 table holds only the opcodes it times differently; zero
		   there means the Z80's figure. */
		const uint8_t base = (s->i8080 && cycles_8080[op] != 0)
		    ? cycles_8080[op] : cycles_main[op];

		s->last_cycles = base;
		if (s->branch_taken) {
			s->last_cycles = (uint8_t) (base + cycles_taken[op]);
		}
		if (p->prefix != 0) {
			s->last_cycles = (uint8_t) (s->last_cycles +
			                            Z80_PREFIX_COST);
			if (p->disp_fetched) {
				s->last_cycles = (uint8_t) (s->last_cycles +
				    ((op == 0x36) ? Z80_LD_IXD_N_COST
				                  : Z80_DISPLACEMENT_COST));
			}
		}
	}

	return s->faulted ? 0 : 1;
}

/* --------------------------------------------------------------- interface */

void
cpu_z80_init(cpu_z80_state *s, uint8_t *ram, uint32_t ram_size)
{
	s->mem.ctx = NULL;
	s->mem.read = NULL;
	s->mem.write = NULL;
	s->mem.can_stall = 0;
	s->i8080 = 0;			/* a Z80 unless told otherwise */

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

	/* A reset clears a waiting access, or a card reset while one was pending
	   would leave the core with nothing able to answer it. */
	s->last_cycles = 0;
	s->branch_taken = 0;
	s->stalled = 0;
	s->stall_addr = 0;
	s->stall_is_write = 0;
}

/* Put the core back where it was before the abandoned instruction. See the note
   in cpu_mem.h for why an instruction is retried whole rather than resumed. */
static int
cpu_z80_abandon(cpu_z80_state *s)
{
	s->a = s->saved.a;   s->f = s->saved.f;
	s->b = s->saved.b;   s->c = s->saved.c;
	s->d = s->saved.d;   s->e = s->saved.e;
	s->h = s->saved.h;   s->l = s->saved.l;
	s->a2 = s->saved.a2; s->f2 = s->saved.f2;
	s->b2 = s->saved.b2; s->c2 = s->saved.c2;
	s->d2 = s->saved.d2; s->e2 = s->saved.e2;
	s->h2 = s->saved.h2; s->l2 = s->saved.l2;
	s->ix = s->saved.ix; s->iy = s->saved.iy;
	s->sp = s->saved.sp; s->pc = s->saved.pc;
	s->i = s->saved.i;   s->r = s->saved.r;
	s->iff1 = s->saved.iff1; s->iff2 = s->saved.iff2;
	s->im = s->saved.im;
	return 0;
}

static void
cpu_z80_save(cpu_z80_state *s)
{
	s->saved.a = s->a;   s->saved.f = s->f;
	s->saved.b = s->b;   s->saved.c = s->c;
	s->saved.d = s->d;   s->saved.e = s->e;
	s->saved.h = s->h;   s->saved.l = s->l;
	s->saved.a2 = s->a2; s->saved.f2 = s->f2;
	s->saved.b2 = s->b2; s->saved.c2 = s->c2;
	s->saved.d2 = s->d2; s->saved.e2 = s->e2;
	s->saved.h2 = s->h2; s->saved.l2 = s->l2;
	s->saved.ix = s->ix; s->saved.iy = s->iy;
	s->saved.sp = s->sp; s->saved.pc = s->pc;
	s->saved.i = s->i;   s->saved.r = s->r;
	s->saved.iff1 = s->iff1; s->saved.iff2 = s->iff2;
	s->saved.im = s->im;
}

void
cpu_z80_set_8080(cpu_z80_state *s, int i8080)
{
	s->i8080 = (i8080 != 0);
}

void
cpu_z80_set_mem_hook(cpu_z80_state *s, const cpu_mem_hook *hook)
{
	if (hook == NULL) {
		s->mem.ctx = NULL;
		s->mem.read = NULL;
		s->mem.write = NULL;
		s->mem.can_stall = 0;
	} else {
		s->mem = *hook;
	}
	s->stalled = 0;
}

int
cpu_z80_interrupt(cpu_z80_state *s, uint8_t vector)
{
	if (s->halted || s->faulted || !s->iff1) {
		return 0;
	}

	/* Both latches clear as the interrupt is taken, so a handler that has not
	   yet executed EI cannot be interrupted again. */
	s->iff1 = 0;
	s->iff2 = 0;

	push16(s, s->pc);

	if (s->im == 2) {
		const uint16_t at = (uint16_t) ((s->i << 8) | vector);

		s->pc = (uint16_t) (rd8(s, at) | (rd8(s, (uint16_t) (at + 1)) << 8));
	} else {
		s->pc = 0x0038;
	}
	return 1;
}

void
cpu_z80_nmi(cpu_z80_state *s)
{
	if (s->halted || s->faulted) {
		return;
	}
	/* IFF2 keeps IFF1's value so that RETN can put it back, which is the whole
	   reason the Z80 has two of them. */
	s->iff2 = s->iff1;
	s->iff1 = 0;
	push16(s, s->pc);
	s->pc = 0x0066;
}

int
cpu_z80_step(cpu_z80_state *s)
{
	pfx p;
	uint8_t next;

	if (s->halted || s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return 0;	/* waiting for the guest to answer */
	}
	s->branch_taken = 0;
	if (s->mem.can_stall) {
		cpu_z80_save(s);
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

	/* A stall during the prefix or opcode fetch: bail out before executing
	   anything, rather than letting the zero the failed fetch returned be
	   decoded as an instruction. */
	if (s->stalled) {
		return cpu_z80_abandon(s);
	}

	/*
	 * ★ THE Z80'S OWN INSTRUCTIONS DO NOT EXIST ON AN 8080.
	 *
	 * The prefixes (CB, DD, ED, FD), the relative jumps and DJNZ, the
	 * alternate register set, and EXX. A real 8080 does something with these
	 * encodings - mostly an undocumented NOP or an alias - and reproducing
	 * that would be modelling a defect nobody should rely on. Faulting says
	 * "this is not an 8080 program", which is the useful answer.
	 */
	if (s->i8080) {
		/* ★ The index prefixes are consumed by the loop above, so by here
		   `next` is the opcode they modified rather than the prefix itself.
		   Asking about `next` alone let DD and FD through, which the test
		   caught: it is p.prefix that records them. */
		if (p.prefix != 0) {
			fault(s, CPU_Z80_FAULT_ILLEGAL, p.prefix);
			return 0;
		}
		switch (next) {
		case 0x08:	/* EX AF,AF'   */
		case 0x10:	/* DJNZ        */
		case 0x18:	/* JR          */
		case 0x20: case 0x28: case 0x30: case 0x38:	/* JR cc */
		case 0xcb:	/* the bit page   */
		case 0xd9:	/* EXX            */
		case 0xdd: case 0xfd:	/* the index prefixes */
		case 0xed:	/* the extended page  */
			fault(s, CPU_Z80_FAULT_ILLEGAL, next);
			return 0;
		default:
			break;
		}
	}

	if (next == 0xedu) {
		s->pc = (uint16_t) (s->pc + 1);
		ed_page(s, &p);
		if (s->faulted) {
			return 0;
		}
		if (s->stalled) {
			return cpu_z80_abandon(s);
		}
		{
			const int n = s->last_cycles;

			s->cycles += (uint64_t) n;
			return n;
		}
	}

	if (!main_page(s, &p)) {
		return 0;
	}
	/* An access anywhere in the instruction may have stalled; the opcode
	   handlers do not check, so this is the one place that does. */
	if (s->stalled) {
		return cpu_z80_abandon(s);
	}

	{
		const int n = s->last_cycles;

		s->cycles += (uint64_t) n;
		return n;
	}
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
