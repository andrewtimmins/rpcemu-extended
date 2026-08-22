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
 * cpu_68000_ops.c - the instructions, and how they claim their encodings.
 *
 * ★ THE ADDRESSING MODES AN INSTRUCTION PERMITS ARE PART OF ITS ENCODING, and
 * expressing that properly is what most of this file's structure is for.
 *
 * A 68000 instruction does not accept all twelve modes. CLR cannot clear an
 * address register or a program-counter-relative operand; MOVEM cannot name a
 * register directly, because those encodings are EXT; JMP takes only the modes
 * that can name a control destination. Motorola names these sets - data,
 * memory, alterable, control, and their intersections - and the encodings
 * outside a set are not instructions.
 *
 * Trying to say that with a bitmask is what invites the classic mistake: a mask
 * one bit too loose silently claims an encoding belonging to something else, and
 * whichever group is installed second quietly loses. So instead each instruction
 * declares its mode set and install_ea() walks the modes, which means the sets
 * are written down in the same words the datasheet uses and m68k_install() can
 * assert that nothing is ever claimed twice.
 */

#include "cpu_68000_priv.h"

/* Fields common to nearly every encoding. */
#define OP_REG(op)	((unsigned) ((op) >> 9) & 7u)
#define OP_MODE(op)	((unsigned) ((op) >> 3) & 7u)
#define OP_RM(op)	((unsigned) (op) & 7u)
#define OP_SIZE_FIELD(op) ((unsigned) ((op) >> 6) & 3u)

/* The size field in the ALU and single-operand groups: 00 byte, 01 word, 10 long. */
static m68k_size
size_from_field(unsigned field)
{
	switch (field) {
	case 0:  return M68K_BYTE;
	case 1:  return M68K_WORD;
	default: return M68K_LONG;
	}
}

/* ------------------------------------------------------ the mode sets */

typedef enum {
	EA_ALL,			/**< every mode */
	EA_DATA,		/**< data: everything but An */
	EA_DATA_ALT,		/**< data and alterable: no An, no PC-rel, no imm */
	EA_MEM_ALT,		/**< memory and alterable: no Dn, no An, no PC-rel, no imm */
	EA_CONTROL,		/**< control: (An), displacement, index, absolute, PC-rel */
	EA_CONTROL_ALT		/**< control and alterable: the above without PC-rel */
} ea_class;

/*
 * Is this mode in that set? Written as the datasheet describes the sets rather
 * than as bit arithmetic, because the sets are the specification.
 */
static int
mode_allowed(ea_class cls, unsigned mode, unsigned reg)
{
	const int is_data_reg = (mode == 0);
	const int is_addr_reg = (mode == 1);
	const int is_imm = (mode == 7 && reg == 4);
	const int is_pc_rel = (mode == 7 && (reg == 2 || reg == 3));
	const int is_absolute = (mode == 7 && (reg == 0 || reg == 1));
	/* Mode 7 registers 5, 6 and 7 are not modes at all. */
	const int exists = (mode != 7) || (reg <= 4);

	if (!exists) {
		return 0;
	}

	switch (cls) {
	case EA_ALL:
		return 1;
	case EA_DATA:
		return !is_addr_reg;
	case EA_DATA_ALT:
		return !is_addr_reg && !is_pc_rel && !is_imm;
	case EA_MEM_ALT:
		return !is_data_reg && !is_addr_reg && !is_pc_rel && !is_imm;
	case EA_CONTROL:
		/* No register direct, no postincrement or predecrement, and no
		   immediate: a control mode names a place, not a datum. */
		return !is_data_reg && !is_addr_reg && !is_imm &&
		       mode != 3 && mode != 4;
	default: /* EA_CONTROL_ALT */
		return !is_data_reg && !is_addr_reg && !is_imm && !is_pc_rel &&
		       mode != 3 && mode != 4 && (is_absolute || mode == 2 ||
		       mode == 5 || mode == 6);
	}
}

/*
 * Install a handler over one instruction's permitted modes.
 *
 * ★ THE BASE COST DIFFERS BETWEEN A REGISTER OPERAND AND A MEMORY ONE on the
 * long forms, which is why there are two figures. ADD.L <ea>,Dn is documented as
 * 8 cycles when the source is a register or immediate and 6 plus the addressing
 * when it is memory - the part overlaps some of the work with the fetch it is
 * doing anyway. Passing one number for both would be wrong for one of them.
 */
static void
install_ea(uint16_t base, ea_class cls, m68k_handler fn,
           uint8_t cycles_reg, uint8_t cycles_mem)
{
	unsigned mode, reg;

	for (mode = 0; mode < 8; mode++) {
		for (reg = 0; reg < 8; reg++) {
			const uint16_t op = (uint16_t) (base | (mode << 3) | reg);
			const int memory = m68k_ea_is_memory(mode, reg);

			if (!mode_allowed(cls, mode, reg)) {
				continue;
			}
			m68k_install(0xffffu, op, fn,
			             memory ? cycles_mem : cycles_reg);
		}
	}
}

/* ----------------------------------------------------------------- MOVE */

/*
 * MOVE and MOVEA share the group 00: the size is in bits 13-12 and the
 * destination is a mode field of its own, which makes this the only instruction
 * on the part with two effective addresses. When the destination mode is 001 it
 * is an address register and the instruction is MOVEA, which sign-extends and
 * sets no flags.
 */
static void
op_move(cpu68000_state *s, uint16_t op)
{
	static const m68k_size sizes[4] = { M68K_LONG, M68K_BYTE, M68K_LONG,
	                                    M68K_WORD };
	const m68k_size size = sizes[(op >> 12) & 3u];
	const unsigned dst_mode = (unsigned) ((op >> 6) & 7u);
	const unsigned dst_reg = OP_REG(op);
	m68k_ea src, dst;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), size, &src);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &src, size);
	if (M68K_ABORTED(s)) {
		return;
	}

	if (dst_mode == 1) {
		/* MOVEA: the whole register is written, sign-extended, and no
		   condition code changes. */
		s->a[dst_reg] = m68k_extend(val, size);
		return;
	}

	m68k_ea_calc(s, dst_mode, dst_reg, size, &dst);
	if (M68K_ABORTED(s)) {
		return;
	}
	m68k_set_nz(s, val, size);
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
	m68k_ea_write(s, &dst, size, val);
}

/* MOVEQ: eight bits of signed data straight into a data register. */
static void
op_moveq(cpu68000_state *s, uint16_t op)
{
	const uint32_t val = m68k_extend((uint32_t) (op & 0xffu), M68K_BYTE);

	s->d[OP_REG(op)] = val;
	m68k_set_nz(s, val, M68K_LONG);
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
}

static void
op_lea(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_LONG, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	/* The address, not what is at it, and no flags: LEA exists to compute. */
	s->a[OP_REG(op)] = ea.addr & M68K_ADDR_MASK;
}

static void
op_pea(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_LONG, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] - 4) & M68K_ADDR_MASK;
	m68k_write32(s, s->a[7], ea.addr & M68K_ADDR_MASK);
}

/*
 * ★ MOVEM, AND THE ONE INSTRUCTION THAT RESUMES RATHER THAN RETRYING.
 *
 * It moves up to sixteen registers in up to sixteen accesses, so a stalling
 * access part way through cannot abandon the whole instruction and start again:
 * the transfers already done would be done twice, and on a region the guest is
 * watching that means eight hardware registers written twice with nothing
 * reporting it. So it counts its transfers in transfer_done, which a stall
 * deliberately does not roll back, and picks up where it stopped.
 *
 * The register list is a bitmask in an extension word. For every mode but
 * predecrement, bit 0 is D0 and bit 15 is A7; for predecrement the order is
 * reversed, because the registers have to come off the stack the way they went
 * on.
 */
static void
op_movem(cpu68000_state *s, uint16_t op)
{
	const int to_memory = ((op & 0x0400u) == 0);
	const m68k_size size = ((op & 0x0040u) != 0) ? M68K_LONG : M68K_WORD;
	const unsigned mode = OP_MODE(op);
	const unsigned reg = OP_RM(op);
	uint16_t list;
	unsigned i;
	unsigned done = 0;
	uint32_t addr;

	/* The extension word is only fetched on the first attempt: a resumed
	   instruction has already consumed it and its program counter was not
	   rolled back past it. */
	list = m68k_fetch16(s);
	if (M68K_ABORTED(s)) {
		return;
	}

	if (mode == 4) {
		/* Predecrement, so the address walks down and the list is read
		   from the other end. */
		addr = s->a[reg];
		for (i = 0; i < 16; i++) {
			const unsigned bit = 15u - i;
			uint32_t val;

			if ((list & (1u << i)) == 0) {
				continue;
			}
			if (done++ < s->transfer_done) {
				addr -= (uint32_t) size;
				continue;
			}
			val = (bit < 8) ? s->d[bit] : s->a[bit - 8];
			addr = (addr - (uint32_t) size) & M68K_ADDR_MASK;
			m68k_write(s, addr, size, val);
			if (M68K_ABORTED(s)) {
				/* Keep what has been moved, and where. */
				s->transfer_done = done - 1;
				s->a[reg] = addr;
				return;
			}
			s->extra_cycles += (size == M68K_LONG) ? 8 : 4;
		}
		s->a[reg] = addr & M68K_ADDR_MASK;
		return;
	}

	{
		m68k_ea ea;

		m68k_ea_calc(s, mode, reg, size, &ea);
		if (M68K_ABORTED(s)) {
			return;
		}
		addr = ea.addr;
	}

	for (i = 0; i < 16; i++) {
		if ((list & (1u << i)) == 0) {
			continue;
		}
		if (done++ < s->transfer_done) {
			addr += (uint32_t) size;
			continue;
		}
		if (to_memory) {
			const uint32_t val = (i < 8) ? s->d[i] : s->a[i - 8];

			m68k_write(s, addr, size, val);
		} else {
			const uint32_t val = m68k_read(s, addr, size);

			if (!M68K_ABORTED(s)) {
				/* ★ Always the whole register, even for a word
				   transfer: MOVEM.W sign-extends into all 32
				   bits, which is unlike every other word move
				   into a data register. */
				if (i < 8) {
					s->d[i] = m68k_extend(val, size);
				} else {
					s->a[i - 8] = m68k_extend(val, size);
				}
			}
		}
		if (M68K_ABORTED(s)) {
			s->transfer_done = done - 1;
			return;
		}
		addr = (addr + (uint32_t) size) & M68K_ADDR_MASK;
		s->extra_cycles += (size == M68K_LONG) ? 8 : 4;
	}

	/* Postincrement leaves the register past what it read. */
	if (mode == 3) {
		s->a[reg] = addr & M68K_ADDR_MASK;
	}
}

void
m68k_install_moves(void)
{
	unsigned size_field, dst_mode, dst_reg;

	/*
	 * MOVE, over both of its mode fields. The destination is walked here
	 * rather than by install_ea, because this is the only instruction with
	 * two of them and the destination's permitted set is its own.
	 */
	for (size_field = 1; size_field <= 3; size_field++) {
		const uint16_t base = (uint16_t) (size_field << 12);
		const m68k_size size = (size_field == 1) ? M68K_BYTE
		    : (size_field == 3) ? M68K_WORD : M68K_LONG;

		for (dst_mode = 0; dst_mode < 8; dst_mode++) {
			for (dst_reg = 0; dst_reg < 8; dst_reg++) {
				const uint16_t d = (uint16_t)
				    (base | (dst_reg << 9) | (dst_mode << 6));

				/* MOVEA takes word and long only; a byte into an
				   address register is not an instruction. */
				if (dst_mode == 1) {
					if (size == M68K_BYTE) {
						continue;
					}
				} else if (!mode_allowed(EA_DATA_ALT, dst_mode,
				                        dst_reg)) {
					continue;
				}
				/* A byte source may not be an address register. */
				install_ea(d,
				    (size == M68K_BYTE) ? EA_DATA : EA_ALL,
				    op_move, 4, 4);
			}
		}
	}

	m68k_install(0xf100u, 0x7000u, op_moveq, 4);

	for (dst_reg = 0; dst_reg < 8; dst_reg++) {
		install_ea((uint16_t) (0x41c0u | (dst_reg << 9)), EA_CONTROL,
		           op_lea, 4, 4);
	}
	install_ea(0x4840u, EA_CONTROL, op_pea, 12, 12);

	/* MOVEM, in its four combinations of direction and size. Register direct
	   is excluded by EA_CONTROL, which is what leaves those encodings free
	   for EXT and SWAP. */
	install_ea(0x4880u, EA_CONTROL_ALT, op_movem, 8, 8);
	install_ea(0x48c0u, EA_CONTROL_ALT, op_movem, 8, 8);
	install_ea(0x4c80u, EA_CONTROL, op_movem, 12, 12);
	install_ea(0x4cc0u, EA_CONTROL, op_movem, 12, 12);
}

/* ------------------------------------------------------------------- ALU */

/*
 * The arithmetic, and which flags each kind touches. The X flag is the one with
 * no equivalent on the other cores here: it is a second carry that ordinary
 * moves leave alone, so a multi-precision addition can put an unrelated
 * instruction between its halves without losing the carry.
 */
static uint32_t
alu_add(cpu68000_state *s, uint32_t a, uint32_t b, m68k_size size,
        unsigned carry_in)
{
	const uint32_t m = m68k_mask(size);
	const uint32_t sign = (m ^ (m >> 1));
	const uint32_t res = (a + b + carry_in) & m;
	const int carry = ((uint64_t) (a & m) + (b & m) + carry_in) > m;

	m68k_set_nz(s, res, size);
	m68k_set_flag(s, CPU68000_FLAG_C, carry);
	m68k_set_flag(s, CPU68000_FLAG_X, carry);
	/* Overflow is a sign question: the operands agreed and the answer does
	   not. */
	m68k_set_flag(s, CPU68000_FLAG_V,
	    ((~(a ^ b) & (a ^ res)) & sign) != 0);
	return res;
}

static uint32_t
alu_sub(cpu68000_state *s, uint32_t a, uint32_t b, m68k_size size,
        unsigned borrow_in, int keep_x)
{
	const uint32_t m = m68k_mask(size);
	const uint32_t sign = (m ^ (m >> 1));
	const uint32_t res = (a - b - borrow_in) & m;
	const int borrow = ((uint64_t) (b & m) + borrow_in) > (a & m);

	m68k_set_nz(s, res, size);
	m68k_set_flag(s, CPU68000_FLAG_C, borrow);
	if (!keep_x) {
		m68k_set_flag(s, CPU68000_FLAG_X, borrow);
	}
	m68k_set_flag(s, CPU68000_FLAG_V,
	    (((a ^ b) & (a ^ res)) & sign) != 0);
	return res;
}

static uint32_t
alu_logic(cpu68000_state *s, uint32_t res, m68k_size size)
{
	m68k_set_nz(s, res, size);
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
	return res;
}

/* The kind of operation, shared by the immediate and register-operand forms. */
typedef enum { ALU_OR, ALU_AND, ALU_SUB, ALU_ADD, ALU_EOR, ALU_CMP } alu_kind;

static uint32_t
alu_apply(cpu68000_state *s, alu_kind kind, uint32_t dst, uint32_t src,
          m68k_size size)
{
	switch (kind) {
	case ALU_OR:  return alu_logic(s, dst | src, size);
	case ALU_AND: return alu_logic(s, dst & src, size);
	case ALU_EOR: return alu_logic(s, dst ^ src, size);
	case ALU_SUB: return alu_sub(s, dst, src, size, 0, 0);
	case ALU_ADD: return alu_add(s, dst, src, size, 0);
	default:      return alu_sub(s, dst, src, size, 0, 1); /* CMP keeps X */
	}
}

/* ORI, ANDI, SUBI, ADDI, EORI and CMPI: an immediate against an operand. */
static void
op_alu_imm(cpu68000_state *s, uint16_t op)
{
	static const alu_kind kinds[8] = { ALU_OR, ALU_AND, ALU_SUB, ALU_ADD,
	                                   ALU_CMP, ALU_EOR, ALU_CMP, ALU_CMP };
	const alu_kind kind = kinds[OP_REG(op)];
	const m68k_size size = size_from_field(OP_SIZE_FIELD(op));
	m68k_ea imm, ea;
	uint32_t src, dst, res;

	m68k_ea_calc(s, 7, 4, size, &imm);		/* the immediate */
	if (M68K_ABORTED(s)) {
		return;
	}
	src = m68k_ea_read(s, &imm, size);

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), size, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	dst = m68k_ea_read(s, &ea, size);
	if (M68K_ABORTED(s)) {
		return;
	}
	res = alu_apply(s, kind, dst, src, size);
	if (kind != ALU_CMP) {
		m68k_ea_write(s, &ea, size, res);
	}
}

/*
 * ADD, SUB, AND, OR, EOR and CMP with a data register as one operand. The
 * three-bit opmode field says the size and which way round the operands go:
 * 0-2 are <ea> into Dn, 4-6 are Dn into <ea>, and 3 and 7 are the address
 * register forms.
 */
static void
op_alu_reg(cpu68000_state *s, uint16_t op)
{
	const unsigned opmode = OP_SIZE_FIELD(op) | (((op >> 8) & 1u) << 2);
	const unsigned group = (unsigned) (op >> 12) & 0xfu;
	const unsigned reg = OP_REG(op);
	alu_kind kind;
	m68k_size size;
	m68k_ea ea;

	switch (group) {
	case 0x8: kind = ALU_OR; break;
	case 0x9: kind = ALU_SUB; break;
	case 0xb: kind = ((opmode & 4u) != 0) ? ALU_EOR : ALU_CMP; break;
	case 0xc: kind = ALU_AND; break;
	default:  kind = ALU_ADD; break;		/* 0xd */
	}

	/* Opmode 3 and 7 are ADDA, SUBA, CMPA - a whole address register, and
	   for ADDA and SUBA no flags at all. */
	if ((opmode & 3u) == 3u) {
		const m68k_size asize = ((opmode & 4u) != 0) ? M68K_LONG
		                                             : M68K_WORD;
		uint32_t val;

		m68k_ea_calc(s, OP_MODE(op), OP_RM(op), asize, &ea);
		if (M68K_ABORTED(s)) {
			return;
		}
		val = m68k_extend(m68k_ea_read(s, &ea, asize), asize);
		if (M68K_ABORTED(s)) {
			return;
		}
		if (kind == ALU_CMP) {
			(void) alu_sub(s, s->a[reg], val, M68K_LONG, 0, 1);
		} else if (kind == ALU_SUB) {
			s->a[reg] = (s->a[reg] - val) & M68K_ADDR_MASK;
		} else {
			s->a[reg] = (s->a[reg] + val) & M68K_ADDR_MASK;
		}
		return;
	}

	size = size_from_field(opmode & 3u);
	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), size, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}

	/* EOR only exists in the Dn-into-<ea> direction, and CMP only in the
	   other; the encodings that would be the reverse are used by other
	   instructions and are not installed here. */
	if ((opmode & 4u) != 0 && kind != ALU_CMP) {
		const uint32_t dst = m68k_ea_read(s, &ea, size);

		if (M68K_ABORTED(s)) {
			return;
		}
		m68k_ea_write(s, &ea, size,
		    alu_apply(s, kind, dst, s->d[reg] & m68k_mask(size), size));
	} else {
		const uint32_t src = m68k_ea_read(s, &ea, size);
		uint32_t res;

		if (M68K_ABORTED(s)) {
			return;
		}
		res = alu_apply(s, kind, s->d[reg] & m68k_mask(size), src, size);
		if (kind != ALU_CMP) {
			const uint32_t m = m68k_mask(size);

			s->d[reg] = (s->d[reg] & ~m) | (res & m);
		}
	}
}

/* ADDQ and SUBQ: a small constant, and 0 in the field means 8. */
static void
op_addq(cpu68000_state *s, uint16_t op)
{
	const int is_sub = ((op & 0x0100u) != 0);
	const uint32_t imm = (OP_REG(op) == 0) ? 8u : OP_REG(op);
	const m68k_size size = size_from_field(OP_SIZE_FIELD(op));
	m68k_ea ea;
	uint32_t dst;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), size, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}

	/* ★ On an address register the whole thing moves and NO flag changes,
	   whatever the size field says. A loop that adjusts a pointer with ADDQ
	   and then branches on the previous comparison depends on that. */
	if (ea.kind == M68K_EA_ADDR_REG) {
		s->a[ea.reg] = (s->a[ea.reg] + (is_sub ? -imm : imm)) &
		    M68K_ADDR_MASK;
		return;
	}

	dst = m68k_ea_read(s, &ea, size);
	if (M68K_ABORTED(s)) {
		return;
	}
	m68k_ea_write(s, &ea, size,
	    is_sub ? alu_sub(s, dst, imm, size, 0, 0)
	           : alu_add(s, dst, imm, size, 0));
}

/* CLR, NEG, NOT and TST: one operand, and NEGX which takes the extend bit in. */
static void
op_single(cpu68000_state *s, uint16_t op)
{
	const m68k_size size = size_from_field(OP_SIZE_FIELD(op));
	const unsigned which = (unsigned) (op >> 9) & 7u;
	m68k_ea ea;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), size, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}

	/*
	 * ★ CLR READS BEFORE IT WRITES on a 68000, which matters for a hardware
	 * register whose read has a side effect: clearing it reads it first. That
	 * is a documented quirk of the part rather than an implementation detail,
	 * and it is why this is not simply a write of zero.
	 */
	val = m68k_ea_read(s, &ea, size);
	if (M68K_ABORTED(s)) {
		return;
	}

	switch (which) {
	case 0:		/* NEGX */
		m68k_ea_write(s, &ea, size,
		    alu_sub(s, 0, val, size,
		            m68k_flag(s, CPU68000_FLAG_X) ? 1u : 0u, 0));
		return;
	case 1:		/* CLR */
		m68k_set_flag(s, CPU68000_FLAG_N, 0);
		m68k_set_flag(s, CPU68000_FLAG_Z, 1);
		m68k_set_flag(s, CPU68000_FLAG_V, 0);
		m68k_set_flag(s, CPU68000_FLAG_C, 0);
		m68k_ea_write(s, &ea, size, 0);
		return;
	case 2:		/* NEG */
		m68k_ea_write(s, &ea, size, alu_sub(s, 0, val, size, 0, 0));
		return;
	case 3:		/* NOT */
		m68k_ea_write(s, &ea, size,
		    alu_logic(s, ~val & m68k_mask(size), size));
		return;
	default:	/* TST: flags only */
		(void) alu_logic(s, val, size);
		return;
	}
}

/* MULU and MULS: two words in, a long out. */
static void
op_mul(cpu68000_state *s, uint16_t op)
{
	const int signed_op = ((op & 0x0100u) != 0);
	const unsigned reg = OP_REG(op);
	m68k_ea ea;
	uint32_t src, res;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	src = m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}

	if (signed_op) {
		res = (uint32_t) ((int32_t) (int16_t) src *
		                  (int32_t) (int16_t) s->d[reg]);
	} else {
		res = (uint32_t) ((uint16_t) src * (uint16_t) s->d[reg]);
	}
	s->d[reg] = res;
	m68k_set_nz(s, res, M68K_LONG);
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
}

/*
 * DIVU and DIVS: a long by a word, giving a quotient in the low half and a
 * remainder in the high.
 *
 * ★ TWO THINGS THAT ARE NOT ORDINARY ARITHMETIC. Dividing by zero takes an
 * exception rather than producing a value, and a quotient too large for sixteen
 * bits sets the overflow flag and LEAVES THE REGISTER ALONE - it does not write a
 * truncated answer. Code that divides and then checks V depends on the register
 * still holding what it did.
 */
static void
op_div(cpu68000_state *s, uint16_t op)
{
	const int signed_op = ((op & 0x0100u) != 0);
	const unsigned reg = OP_REG(op);
	m68k_ea ea;
	uint32_t src;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	src = m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}

	if ((src & 0xffffu) == 0) {
		m68k_exception(s, CPU68000_VEC_DIV_ZERO);
		return;
	}

	if (signed_op) {
		const int32_t dividend = (int32_t) s->d[reg];
		const int32_t divisor = (int32_t) (int16_t) src;
		const int32_t q = dividend / divisor;
		const int32_t r = dividend % divisor;

		if (q > 32767 || q < -32768) {
			m68k_set_flag(s, CPU68000_FLAG_V, 1);
			return;
		}
		s->d[reg] = ((uint32_t) r << 16) | ((uint32_t) q & 0xffffu);
		m68k_set_nz(s, (uint32_t) q & 0xffffu, M68K_WORD);
	} else {
		const uint32_t dividend = s->d[reg];
		const uint32_t divisor = src & 0xffffu;
		const uint32_t q = dividend / divisor;
		const uint32_t r = dividend % divisor;

		if (q > 0xffffu) {
			m68k_set_flag(s, CPU68000_FLAG_V, 1);
			return;
		}
		s->d[reg] = (r << 16) | (q & 0xffffu);
		m68k_set_nz(s, q, M68K_WORD);
	}
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
}

void
m68k_install_alu(void)
{
	unsigned which, size_field, reg, opmode;

	/* The immediate forms. Which operation is in bits 11-9, and CMPI cannot
	   write so its destination set is wider. */
	for (which = 0; which < 8; which++) {
		if (which == 4 || which == 6 || which == 7) {
			continue;	/* the bit instructions and unused rows */
		}
		for (size_field = 0; size_field < 3; size_field++) {
			const uint16_t base = (uint16_t)
			    (0x0000u | (which << 9) | (size_field << 6));

			install_ea(base,
			    (which == 6) ? EA_DATA : EA_DATA_ALT,
			    op_alu_imm, 8, 12);
		}
	}
	/* CMPI is row 6 and reads rather than writes. */
	for (size_field = 0; size_field < 3; size_field++) {
		install_ea((uint16_t) (0x0c00u | (size_field << 6)), EA_DATA,
		           op_alu_imm, 8, 8);
	}

	/* ADDQ and SUBQ. An address register is allowed, unlike most of these. */
	for (which = 0; which < 2; which++) {
		for (size_field = 0; size_field < 3; size_field++) {
			for (reg = 0; reg < 8; reg++) {
				install_ea((uint16_t) (0x5000u | (which << 8) |
				    (reg << 9) | (size_field << 6)),
				    EA_ALL, op_addq, 4, 8);
			}
		}
	}

	/* The register-operand forms of the six operations. */
	for (reg = 0; reg < 8; reg++) {
		for (opmode = 0; opmode < 8; opmode++) {
			const uint16_t bits = (uint16_t)
			    ((reg << 9) | (opmode << 6));
			const int to_ea = ((opmode & 4u) != 0);
			const int is_addr = ((opmode & 3u) == 3u);
			const uint8_t creg = (opmode == 2 || opmode == 6) ? 8 : 4;
			const uint8_t cmem = (opmode == 2 || opmode == 6) ? 6 : 4;

			/* ADD and SUB: both directions, and the address forms. */
			install_ea((uint16_t) (0xd000u | bits),
			    is_addr ? EA_ALL : (to_ea ? EA_MEM_ALT : EA_ALL),
			    op_alu_reg, creg, cmem);
			install_ea((uint16_t) (0x9000u | bits),
			    is_addr ? EA_ALL : (to_ea ? EA_MEM_ALT : EA_ALL),
			    op_alu_reg, creg, cmem);

			if (is_addr) {
				/* CMPA exists; there is no ANDA or ORA. */
				install_ea((uint16_t) (0xb000u | bits), EA_ALL,
				    op_alu_reg, 6, 6);
				continue;
			}

			/* AND and OR: the &x1C0 rows are DIV and MUL, and the
			   &x100 rows with a register operand are ABCD, SBCD, EXG
			   and the X forms, which are not installed here. */
			install_ea((uint16_t) (0xc000u | bits),
			    to_ea ? EA_MEM_ALT : EA_DATA, op_alu_reg, creg, cmem);
			install_ea((uint16_t) (0x8000u | bits),
			    to_ea ? EA_MEM_ALT : EA_DATA, op_alu_reg, creg, cmem);

			/* CMP reads, EOR writes, and they share group &B. */
			if (to_ea) {
				install_ea((uint16_t) (0xb000u | bits),
				    EA_DATA_ALT, op_alu_reg, 4, 8);
			} else {
				install_ea((uint16_t) (0xb000u | bits), EA_ALL,
				    op_alu_reg, 4, 4);
			}
		}

		/* MULU, MULS, DIVU and DIVS sit in the &1C0 rows of AND and OR. */
		install_ea((uint16_t) (0xc0c0u | (reg << 9)), EA_DATA, op_mul,
		           70, 70);
		install_ea((uint16_t) (0xc1c0u | (reg << 9)), EA_DATA, op_mul,
		           70, 70);
		install_ea((uint16_t) (0x80c0u | (reg << 9)), EA_DATA, op_div,
		           140, 140);
		install_ea((uint16_t) (0x81c0u | (reg << 9)), EA_DATA, op_div,
		           158, 158);
	}

	/* CLR, NEG, NOT, TST and NEGX, whose row is in bits 11-9. The &x0C0
	   encodings of each row are the status register moves and are installed
	   with the rest of the system instructions. */
	for (which = 0; which < 8; which++) {
		if (which > 3 && which != 5) {
			continue;
		}
		for (size_field = 0; size_field < 3; size_field++) {
			install_ea((uint16_t) (0x4000u | (which << 9) |
			    (size_field << 6)),
			    (which == 5) ? EA_DATA : EA_DATA_ALT,
			    op_single, 4, 8);
		}
	}
}

/* ---------------------------------------------------------------- shifts */

/*
 * The eight shifts and rotates, which differ in what comes in at the vacated
 * end and in what they do to the flags.
 *
 * ★ ASL'S OVERFLOW FLAG IS NOT LIKE ANYTHING ELSE HERE. It is set if the sign
 * bit changed AT ANY POINT during the shift, not merely between the value going
 * in and the answer coming out - so shifting &C0 left twice sets it even though
 * the sign is negative at both ends. That means the shift cannot be done in one
 * step and the flag worked out afterwards, which is why this loops.
 */
typedef enum { SH_AS, SH_LS, SH_ROX, SH_RO } shift_kind;

static uint32_t
do_shift(cpu68000_state *s, shift_kind kind, int left, uint32_t val,
         unsigned count, m68k_size size)
{
	const uint32_t m = m68k_mask(size);
	const uint32_t sign = m ^ (m >> 1);
	int carry = 0;
	int overflow = 0;
	unsigned i;

	val &= m;

	if (count == 0) {
		/*
		 * A count of zero leaves the value alone and still reports:
		 * carry is cleared, except for the rotate-through-extend forms,
		 * which report the extend bit they would have rotated in.
		 */
		m68k_set_nz(s, val, size);
		m68k_set_flag(s, CPU68000_FLAG_V, 0);
		m68k_set_flag(s, CPU68000_FLAG_C,
		    (kind == SH_ROX) ? m68k_flag(s, CPU68000_FLAG_X) : 0);
		return val;
	}

	for (i = 0; i < count; i++) {
		const uint32_t before = val;

		if (left) {
			carry = ((val & sign) != 0);
			val = (val << 1) & m;
			switch (kind) {
			case SH_RO:  val |= carry ? 1u : 0u; break;
			case SH_ROX: val |= m68k_flag(s, CPU68000_FLAG_X) ? 1u : 0u;
			             break;
			default:     break;
			}
		} else {
			carry = ((val & 1u) != 0);
			val >>= 1;
			switch (kind) {
			case SH_AS:  val |= (before & sign); break;
			case SH_RO:  val |= carry ? sign : 0u; break;
			case SH_ROX: val |= m68k_flag(s, CPU68000_FLAG_X) ? sign : 0u;
			             break;
			default:     break;
			}
		}
		if (kind == SH_ROX) {
			m68k_set_flag(s, CPU68000_FLAG_X, carry);
		}
		if (kind == SH_AS && left && ((before ^ val) & sign) != 0) {
			overflow = 1;
		}
	}

	m68k_set_nz(s, val, size);
	m68k_set_flag(s, CPU68000_FLAG_C, carry);
	m68k_set_flag(s, CPU68000_FLAG_V, overflow);
	/* A rotate that does not go through the extend bit leaves it alone; a
	   shift copies its carry into it. */
	if (kind != SH_RO && kind != SH_ROX) {
		m68k_set_flag(s, CPU68000_FLAG_X, carry);
	}
	return val;
}

static void
op_shift_reg(cpu68000_state *s, uint16_t op)
{
	const shift_kind kind = (shift_kind) ((op >> 3) & 3u);
	const int left = ((op & 0x0100u) != 0);
	const m68k_size size = size_from_field(OP_SIZE_FIELD(op));
	const unsigned reg = OP_RM(op);
	unsigned count;
	uint32_t res;

	if ((op & 0x0020u) != 0) {
		/* The count is in a register, and only its low six bits: a count
		   of 64 is possible and shifts everything out. */
		count = s->d[OP_REG(op)] & 63u;
	} else {
		count = (OP_REG(op) == 0) ? 8u : OP_REG(op);
	}

	res = do_shift(s, kind, left, s->d[reg], count, size);
	{
		const uint32_t m = m68k_mask(size);

		s->d[reg] = (s->d[reg] & ~m) | (res & m);
	}
	/* Two cycles per bit moved, on top of the base. */
	s->extra_cycles += (int) (2u * count);
}

/* The memory forms shift by one, in place. */
static void
op_shift_mem(cpu68000_state *s, uint16_t op)
{
	const shift_kind kind = (shift_kind) ((op >> 9) & 3u);
	const int left = ((op & 0x0100u) != 0);
	m68k_ea ea;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}
	m68k_ea_write(s, &ea, M68K_WORD,
	    do_shift(s, kind, left, val, 1, M68K_WORD));
}

void
m68k_install_shifts(void)
{
	unsigned count, dir, size_field, ir, type, reg;

	for (count = 0; count < 8; count++) {
		for (dir = 0; dir < 2; dir++) {
			for (size_field = 0; size_field < 3; size_field++) {
				for (ir = 0; ir < 2; ir++) {
					for (type = 0; type < 4; type++) {
						for (reg = 0; reg < 8; reg++) {
							const uint16_t op = (uint16_t)
							    (0xe000u | (count << 9) |
							     (dir << 8) |
							     (size_field << 6) |
							     (ir << 5) | (type << 3) |
							     reg);
							m68k_install(0xffffu, op,
							    op_shift_reg,
							    (size_field == 2) ? 8 : 6);
						}
					}
				}
			}
		}
	}

	for (type = 0; type < 4; type++) {
		for (dir = 0; dir < 2; dir++) {
			install_ea((uint16_t) (0xe0c0u | (type << 9) | (dir << 8)),
			    EA_MEM_ALT, op_shift_mem, 8, 8);
		}
	}
}

/* ------------------------------------------------------------------- bits */

/*
 * BTST, BCHG, BCLR and BSET.
 *
 * ★ THE OPERAND'S SIZE DECIDES HOW THE BIT NUMBER IS READ. On a data register
 * the operand is a long and the number is taken modulo 32; anywhere else it is a
 * byte in memory and the number is modulo 8. So the same instruction addresses a
 * different bit depending on where its operand lives, which is not something any
 * other core here does.
 *
 * Z reports the bit's value BEFORE the operation, complemented - so a BSET on a
 * bit that was already set leaves Z clear.
 */
static void
op_bit(cpu68000_state *s, uint16_t op)
{
	const unsigned which = (unsigned) (op >> 6) & 3u;
	const int is_static = ((op & 0x0f00u) == 0x0800u);
	const unsigned mode = OP_MODE(op);
	m68k_size size;
	m68k_ea ea;
	unsigned bit;
	uint32_t val;

	if (is_static) {
		bit = m68k_fetch16(s) & 0xffu;
	} else {
		bit = s->d[OP_REG(op)];
	}
	if (M68K_ABORTED(s)) {
		return;
	}

	size = (mode == 0) ? M68K_LONG : M68K_BYTE;
	bit &= (size == M68K_LONG) ? 31u : 7u;

	m68k_ea_calc(s, mode, OP_RM(op), size, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &ea, size);
	if (M68K_ABORTED(s)) {
		return;
	}

	m68k_set_flag(s, CPU68000_FLAG_Z, (val & (1u << bit)) == 0);

	switch (which) {
	case 0:
		return;					/* BTST writes nothing */
	case 1:
		val ^= (1u << bit);			/* BCHG */
		break;
	case 2:
		val &= ~(1u << bit);			/* BCLR */
		break;
	default:
		val |= (1u << bit);			/* BSET */
		break;
	}
	m68k_ea_write(s, &ea, size, val);
}

void
m68k_install_bits(void)
{
	unsigned which, reg;

	for (which = 0; which < 4; which++) {
		/* The static form's bit number is in an extension word. BTST may
		   read a program-counter-relative operand; the others must be
		   able to write. */
		install_ea((uint16_t) (0x0800u | (which << 6)),
		    (which == 0) ? EA_DATA : EA_DATA_ALT, op_bit, 10, 12);

		for (reg = 0; reg < 8; reg++) {
			install_ea((uint16_t) (0x0100u | (reg << 9) | (which << 6)),
			    (which == 0) ? EA_DATA : EA_DATA_ALT, op_bit, 6, 8);
		}
	}
}

/* --------------------------------------------------------------- branches */

/*
 * Bcc, BRA and BSR share one encoding: the condition field selects, and 0 and 1
 * are the unconditional branch and the subroutine call.
 *
 * A displacement of zero in the opcode means a 16-bit one follows. &FF means a
 * 32-bit one on later parts and is not an instruction here.
 */
static void
op_branch(cpu68000_state *s, uint16_t op)
{
	const unsigned cond = (unsigned) (op >> 8) & 0xfu;
	const uint32_t base = s->pc;
	int32_t disp = (int32_t) (int8_t) (op & 0xffu);

	if ((op & 0xffu) == 0) {
		disp = (int32_t) (int16_t) m68k_fetch16(s);
		if (M68K_ABORTED(s)) {
			return;
		}
	}

	if (cond == 1) {				/* BSR */
		s->a[7] = (s->a[7] - 4) & M68K_ADDR_MASK;
		m68k_write32(s, s->a[7], s->pc);
		if (M68K_ABORTED(s)) {
			return;
		}
		s->pc = (base + (uint32_t) disp) & M68K_ADDR_MASK;
		return;
	}

	if (m68k_cond(s, cond)) {
		s->pc = (base + (uint32_t) disp) & M68K_ADDR_MASK;
		s->extra_cycles += 2;
	} else if ((op & 0xffu) == 0) {
		s->extra_cycles += 4;	/* a word displacement not taken costs more */
	}
}

/*
 * DBcc: the loop instruction, and the condition is the one to STOP on.
 *
 * ★ It decrements first and tests the counter as a WORD, so a loop set up with
 * a count of zero runs 65536 times rather than none. That is the hardware's
 * behaviour and code relies on it.
 */
static void
op_dbcc(cpu68000_state *s, uint16_t op)
{
	const unsigned cond = (unsigned) (op >> 8) & 0xfu;
	const unsigned reg = OP_RM(op);
	const uint32_t base = s->pc;
	const int32_t disp = (int32_t) (int16_t) m68k_fetch16(s);

	if (M68K_ABORTED(s)) {
		return;
	}
	if (m68k_cond(s, cond)) {
		s->extra_cycles += 2;
		return;				/* the condition ends the loop */
	}
	{
		const uint16_t next = (uint16_t) ((s->d[reg] & 0xffffu) - 1u);

		s->d[reg] = (s->d[reg] & 0xffff0000u) | next;
		if (next != 0xffffu) {
			s->pc = (base + (uint32_t) disp) & M68K_ADDR_MASK;
		} else {
			s->extra_cycles += 4;
		}
	}
}

/* Scc: the condition as a value, and it writes &FF rather than 1 when true. */
static void
op_scc(cpu68000_state *s, uint16_t op)
{
	const unsigned cond = (unsigned) (op >> 8) & 0xfu;
	const int set = m68k_cond(s, cond);
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_BYTE, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	m68k_ea_write(s, &ea, M68K_BYTE, set ? 0xffu : 0x00u);
	if (set && ea.kind == M68K_EA_DATA_REG) {
		s->extra_cycles += 2;
	}
}

void
m68k_install_branches(void)
{
	unsigned cond, disp, reg;

	for (cond = 0; cond < 16; cond++) {
		for (disp = 0; disp < 256; disp++) {
			if (disp == 0xff) {
				continue;	/* a long displacement is 68020 */
			}
			m68k_install(0xffffu,
			    (uint16_t) (0x6000u | (cond << 8) | disp),
			    op_branch, (cond <= 1) ? 10 : 8);
		}
	}

	for (cond = 0; cond < 16; cond++) {
		for (reg = 0; reg < 8; reg++) {
			m68k_install(0xffffu,
			    (uint16_t) (0x50c8u | (cond << 8) | reg),
			    op_dbcc, 10);
		}
		/* Scc shares the row; DBcc has already claimed mode 001. */
		install_ea((uint16_t) (0x50c0u | (cond << 8)), EA_DATA_ALT,
		    op_scc, 4, 8);
	}
}

/* ------------------------------------------------------- system and misc */

/*
 * ★ THE PRIVILEGED INSTRUCTIONS, and this is the whole reason the supervisor bit
 * exists. Writing the status register, touching the user stack pointer, RESET,
 * STOP and RTE are all refused in user mode with a privilege violation - which is
 * how an operating system keeps a program from simply granting itself supervisor
 * mode and taking over.
 */
static int
require_supervisor(cpu68000_state *s)
{
	if (m68k_flag(s, CPU68000_SR_S)) {
		return 1;
	}
	m68k_exception(s, CPU68000_VEC_PRIVILEGE);
	return 0;
}

static void
op_nop(cpu68000_state *s, uint16_t op)
{
	(void) s;
	(void) op;
}

/* RESET drives a line no card here has, so it costs its cycles and does nothing. */
static void
op_reset(cpu68000_state *s, uint16_t op)
{
	(void) op;
	(void) require_supervisor(s);
}

/*
 * ★ STOP IS HOW A PROGRAM ON THIS CARD FINISHES, and it needs no special case.
 *
 * It loads the status register and stops. If nothing ever interrupts the core it
 * is finished, and the card reports it halted with D0 as the exit code - the
 * bargain SWI makes on the 6809 and BRK on the 6502. If the guest offers an
 * interrupt, cpu68000_interrupt clears the stopped flag and the core carries on,
 * which is what the instruction actually means.
 *
 * SYNC and CWAI fault on the 6809, and WAI on the 6800, only because those cards
 * have no interrupt model to wait on. This one has, so this one does not have to
 * lie about it.
 */
static void
op_stop(cpu68000_state *s, uint16_t op)
{
	uint16_t sr;

	(void) op;
	sr = m68k_fetch16(s);
	if (M68K_ABORTED(s)) {
		return;
	}
	if (!require_supervisor(s)) {
		return;
	}
	m68k_set_sr(s, sr);
	s->stopped = 1;
	s->halted = 1;
	s->halt_reason = CPU68000_HALT_STOP;
	s->exit_code = s->d[0];
}

static void
op_rte(cpu68000_state *s, uint16_t op)
{
	uint16_t sr;
	uint32_t pc;

	(void) op;
	if (!require_supervisor(s)) {
		return;
	}
	sr = m68k_read16(s, s->a[7]);
	pc = m68k_read32(s, (s->a[7] + 2) & M68K_ADDR_MASK);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] + 6) & M68K_ADDR_MASK;
	/* The stack pointer is adjusted before the mode changes, because the
	   adjustment belongs to the supervisor stack it was made on. */
	m68k_set_sr(s, sr);
	s->pc = pc & M68K_ADDR_MASK;
}

static void
op_rts(cpu68000_state *s, uint16_t op)
{
	const uint32_t pc = m68k_read32(s, s->a[7]);

	(void) op;
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] + 4) & M68K_ADDR_MASK;
	s->pc = pc & M68K_ADDR_MASK;
}

/* RTR takes the condition codes back too, but not the whole status register:
   a user-mode routine may restore its flags without gaining privilege. */
static void
op_rtr(cpu68000_state *s, uint16_t op)
{
	const uint16_t ccr = m68k_read16(s, s->a[7]);
	const uint32_t pc = m68k_read32(s, (s->a[7] + 2) & M68K_ADDR_MASK);

	(void) op;
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] + 6) & M68K_ADDR_MASK;
	s->sr = (uint16_t) ((s->sr & 0xff00u) | (ccr & 0x00ffu));
	s->pc = pc & M68K_ADDR_MASK;
}

static void
op_trap(cpu68000_state *s, uint16_t op)
{
	m68k_exception(s, CPU68000_VEC_TRAP + (unsigned) (op & 0xfu));
}

static void
op_trapv(cpu68000_state *s, uint16_t op)
{
	(void) op;
	if (m68k_flag(s, CPU68000_FLAG_V)) {
		m68k_exception(s, CPU68000_VEC_TRAPV);
	}
}

static void
op_illegal(cpu68000_state *s, uint16_t op)
{
	(void) op;
	m68k_exception(s, CPU68000_VEC_ILLEGAL);
}

static void
op_jmp(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_LONG, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->pc = ea.addr & M68K_ADDR_MASK;
}

static void
op_jsr(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_LONG, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] - 4) & M68K_ADDR_MASK;
	m68k_write32(s, s->a[7], s->pc);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->pc = ea.addr & M68K_ADDR_MASK;
}

/* LINK and UNLK: a stack frame in two instructions, which is why C compilers
   for this part are so much happier than they were for the 6800. */
static void
op_link(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_RM(op);
	const int32_t disp = (int32_t) (int16_t) m68k_fetch16(s);

	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[7] = (s->a[7] - 4) & M68K_ADDR_MASK;
	m68k_write32(s, s->a[7], s->a[reg]);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[reg] = s->a[7];
	s->a[7] = (s->a[7] + (uint32_t) disp) & M68K_ADDR_MASK;
}

static void
op_unlk(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_RM(op);
	uint32_t val;

	s->a[7] = s->a[reg];
	val = m68k_read32(s, s->a[7]);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->a[reg] = val;
	s->a[7] = (s->a[7] + 4) & M68K_ADDR_MASK;
}

static void
op_swap(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_RM(op);
	const uint32_t val = (s->d[reg] >> 16) | (s->d[reg] << 16);

	s->d[reg] = val;
	m68k_set_nz(s, val, M68K_LONG);
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
}

/* EXT: sign-extend a byte into a word, or a word into a long. */
static void
op_ext(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_RM(op);
	const int to_long = ((op & 0x0040u) != 0);
	uint32_t val;

	if (to_long) {
		val = m68k_extend(s->d[reg] & 0xffffu, M68K_WORD);
		s->d[reg] = val;
		m68k_set_nz(s, val, M68K_LONG);
	} else {
		val = m68k_extend(s->d[reg] & 0xffu, M68K_BYTE) & 0xffffu;
		s->d[reg] = (s->d[reg] & 0xffff0000u) | val;
		m68k_set_nz(s, val, M68K_WORD);
	}
	m68k_set_flag(s, CPU68000_FLAG_V, 0);
	m68k_set_flag(s, CPU68000_FLAG_C, 0);
}

/*
 * TAS: test and set, the part's one indivisible read-modify-write.
 *
 * It exists for locks between two processors sharing memory, which on this card
 * is a situation the guest can genuinely create. The stall protocol needs nothing
 * special for it: the stall lands on the read, which is the case cpu_mem.h
 * already covers.
 */
static void
op_tas(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_BYTE, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &ea, M68K_BYTE);
	if (M68K_ABORTED(s)) {
		return;
	}
	(void) alu_logic(s, val, M68K_BYTE);
	m68k_ea_write(s, &ea, M68K_BYTE, val | 0x80u);
}

/* MOVE from SR, MOVE to SR, MOVE to CCR: the status register as data. */
static void
op_move_from_sr(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	m68k_ea_write(s, &ea, M68K_WORD, s->sr);
}

static void
op_move_to_sr(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}
	if (!require_supervisor(s)) {
		return;
	}
	m68k_set_sr(s, (uint16_t) val);
}

static void
op_move_to_ccr(cpu68000_state *s, uint16_t op)
{
	m68k_ea ea;
	uint32_t val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}
	/* Only the condition codes, so this one is not privileged. */
	s->sr = (uint16_t) ((s->sr & 0xff00u) | (val & 0x00ffu));
}

/*
 * MOVE USP: reach the stack pointer that is not currently A7.
 *
 * Privileged, and it has to be: a user-mode program that could write its own
 * supervisor stack pointer could redirect the next exception's frame anywhere it
 * liked.
 */
static void
op_move_usp(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_RM(op);

	if (!require_supervisor(s)) {
		return;
	}
	if ((op & 0x0008u) != 0) {
		s->a[reg] = s->usp;
	} else {
		s->usp = s->a[reg];
	}
}

/* CHK: bounds-check a register, and take an exception if it is outside. */
static void
op_chk(cpu68000_state *s, uint16_t op)
{
	const unsigned reg = OP_REG(op);
	m68k_ea ea;
	int32_t bound, val;

	m68k_ea_calc(s, OP_MODE(op), OP_RM(op), M68K_WORD, &ea);
	if (M68K_ABORTED(s)) {
		return;
	}
	bound = (int32_t) (int16_t) m68k_ea_read(s, &ea, M68K_WORD);
	if (M68K_ABORTED(s)) {
		return;
	}
	val = (int32_t) (int16_t) s->d[reg];

	m68k_set_flag(s, CPU68000_FLAG_N, val < 0);
	if (val < 0 || val > bound) {
		m68k_exception(s, CPU68000_VEC_CHK);
	}
}

/* EXG: swap two registers whole, either pair of files. */
static void
op_exg(cpu68000_state *s, uint16_t op)
{
	const unsigned rx = OP_REG(op);
	const unsigned ry = OP_RM(op);
	const unsigned mode = (unsigned) (op >> 3) & 0x1fu;
	uint32_t tmp;

	if (mode == 0x08) {			/* two data registers */
		tmp = s->d[rx];
		s->d[rx] = s->d[ry];
		s->d[ry] = tmp;
	} else if (mode == 0x09) {		/* two address registers */
		tmp = s->a[rx];
		s->a[rx] = s->a[ry];
		s->a[ry] = tmp;
	} else {				/* one of each */
		tmp = s->d[rx];
		s->d[rx] = s->a[ry];
		s->a[ry] = tmp;
	}
}

/* ANDI, ORI and EORI to the status register or the condition codes. */
static void
op_logic_sr(cpu68000_state *s, uint16_t op)
{
	const unsigned which = OP_REG(op);
	const int whole_sr = ((op & 0x0040u) != 0);
	const uint16_t imm = m68k_fetch16(s);
	uint16_t val;

	if (M68K_ABORTED(s)) {
		return;
	}
	if (whole_sr && !require_supervisor(s)) {
		return;
	}

	val = whole_sr ? s->sr : (uint16_t) (s->sr & 0x00ffu);
	switch (which) {
	case 0: val = (uint16_t) (val | imm); break;		/* ORI  */
	case 1: val = (uint16_t) (val & imm); break;		/* ANDI */
	default: val = (uint16_t) (val ^ imm); break;		/* EORI */
	}

	if (whole_sr) {
		m68k_set_sr(s, val);
	} else {
		s->sr = (uint16_t) ((s->sr & 0xff00u) | (val & 0x00ffu));
	}
}

void
m68k_install_misc(void)
{
	unsigned reg, n, which;

	/* The status register moves sit in the &x0C0 rows the single-operand
	   group left free. MOVE from CCR does not exist on a 68000. */
	install_ea(0x40c0u, EA_DATA_ALT, op_move_from_sr, 6, 8);
	install_ea(0x44c0u, EA_DATA, op_move_to_ccr, 12, 12);
	install_ea(0x46c0u, EA_DATA, op_move_to_sr, 12, 12);

	/* ANDI, ORI and EORI to SR and CCR: the immediate group's &x3C and &x7C
	   encodings, which are mode 7 register 4. */
	for (which = 0; which < 3; which++) {
		const uint16_t row = (uint16_t)
		    (0x0000u | ((which == 2 ? 5u : which) << 9));

		m68k_install(0xffffu, (uint16_t) (row | 0x003cu), op_logic_sr, 20);
		m68k_install(0xffffu, (uint16_t) (row | 0x007cu), op_logic_sr, 20);
	}

	install_ea(0x4ac0u, EA_DATA_ALT, op_tas, 4, 14);
	m68k_install(0xffffu, 0x4afcu, op_illegal, 34);

	for (reg = 0; reg < 8; reg++) {
		m68k_install(0xffffu, (uint16_t) (0x4840u | reg), op_swap, 4);
		m68k_install(0xffffu, (uint16_t) (0x4880u | reg), op_ext, 4);
		m68k_install(0xffffu, (uint16_t) (0x48c0u | reg), op_ext, 4);
		m68k_install(0xffffu, (uint16_t) (0x4e50u | reg), op_link, 16);
		m68k_install(0xffffu, (uint16_t) (0x4e58u | reg), op_unlk, 12);
		m68k_install(0xffffu, (uint16_t) (0x4e60u | reg), op_move_usp, 4);
		m68k_install(0xffffu, (uint16_t) (0x4e68u | reg), op_move_usp, 4);
		install_ea((uint16_t) (0x4180u | (reg << 9)), EA_DATA, op_chk,
		    10, 10);
	}

	for (n = 0; n < 16; n++) {
		m68k_install(0xffffu, (uint16_t) (0x4e40u | n), op_trap, 34);
	}

	m68k_install(0xffffu, 0x4e70u, op_reset, 132);
	m68k_install(0xffffu, 0x4e71u, op_nop, 4);
	m68k_install(0xffffu, 0x4e72u, op_stop, 4);
	m68k_install(0xffffu, 0x4e73u, op_rte, 20);
	m68k_install(0xffffu, 0x4e75u, op_rts, 16);
	m68k_install(0xffffu, 0x4e76u, op_trapv, 4);
	m68k_install(0xffffu, 0x4e77u, op_rtr, 20);

	install_ea(0x4e80u, EA_CONTROL, op_jsr, 16, 16);
	install_ea(0x4ec0u, EA_CONTROL, op_jmp, 8, 8);

	/*
	 * EXG's three forms, in the rows of AND that the register-operand group
	 * does not claim.
	 */
	for (reg = 0; reg < 8; reg++) {
		for (n = 0; n < 8; n++) {
			m68k_install(0xffffu,
			    (uint16_t) (0xc140u | (reg << 9) | n), op_exg, 6);
			m68k_install(0xffffu,
			    (uint16_t) (0xc148u | (reg << 9) | n), op_exg, 6);
			m68k_install(0xffffu,
			    (uint16_t) (0xc188u | (reg << 9) | n), op_exg, 6);
		}
	}
}
