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
 * cpu_rv32i.c - RV32IM.
 *
 * What this is, what it deliberately leaves out and how a program stops are all
 * in cpu_rv32i.h.
 *
 * ★ EVERY ARITHMETIC OPERATION HERE IS WRITTEN TO BE DEFINED IN C, not merely to
 * work on this compiler. That matters more than usual: the CI sanitiser job runs
 * with -fno-sanitize-recover=all, so undefined behaviour is a hard failure rather
 * than something that quietly produces the right answer - and two real defects of
 * exactly this kind have been found in this codebase's older arithmetic. So:
 * signed values are computed on unsigned types and cast at the ends, shift counts
 * are masked to five bits before use, arithmetic right shift is open-coded, and
 * the two division cases the hardware defines but C does not are special-cased.
 */

#include "cpu_rv32i.h"

#include <stddef.h>

/* Opcodes, as the 7-bit field. Named as the RISC-V manual names them. */
#define OP_LUI		0x37
#define OP_AUIPC	0x17
#define OP_JAL		0x6f
#define OP_JALR		0x67
#define OP_BRANCH	0x63
#define OP_LOAD		0x03
#define OP_STORE	0x23
#define OP_OP_IMM	0x13
#define OP_OP		0x33
#define OP_MISC_MEM	0x0f
#define OP_SYSTEM	0x73

/**
 * Sign-extend the low @bits of @val.
 *
 * Done on unsigned types throughout: the obvious `(int32_t)(val << sh) >> sh`
 * shifts a signed value left into the sign bit, which is undefined.
 */
static uint32_t
sign_extend(uint32_t val, unsigned bits)
{
	const uint32_t mask = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
	const uint32_t sign = (bits >= 32) ? 0x80000000u : (1u << (bits - 1u));

	val &= mask;
	if (val & sign) {
		val |= ~mask;
	}
	return val;
}

/**
 * Arithmetic right shift, open-coded.
 *
 * Right shift of a negative signed value is implementation-defined in C. Every
 * compiler we use does the arithmetic thing, but "every compiler we use" is not
 * the same as defined, and this costs nothing.
 */
static uint32_t
sra32(uint32_t val, unsigned sh)
{
	sh &= 31u;
	if (val & 0x80000000u) {
		/* At sh == 0 the complement is of all-ones, which is zero, so the
		   value comes back unchanged as it must. */
		return (val >> sh) | ~(0xffffffffu >> sh);
	}
	return val >> sh;
}

static void
fault(rv32i_state *s, uint32_t cause, uint32_t addr)
{
	s->faulted = 1;
	s->fault_cause = cause;
	s->fault_addr = addr;
	/* pc is left at the instruction that faulted rather than advanced: a
	   guest reading it back wants to know where the program died, and there
	   is no trap vector here for it to have gone to instead. */
}

/**
 * Is [addr, addr + len) inside the core's memory?
 *
 * Written as a subtraction so that an address near 2^32 cannot wrap the sum and
 * come out looking valid.
 */
static int
in_range(const rv32i_state *s, uint32_t addr, uint32_t len)
{
	if (s->ram == NULL || s->ram_size < len) {
		return 0;
	}
	return addr <= s->ram_size - len;
}

static uint32_t
load32(const rv32i_state *s, uint32_t addr)
{
	const uint8_t *p = s->ram + addr;

	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t
load16(const rv32i_state *s, uint32_t addr)
{
	const uint8_t *p = s->ram + addr;

	return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static void
store32(rv32i_state *s, uint32_t addr, uint32_t val)
{
	uint8_t *p = s->ram + addr;

	p[0] = (uint8_t) val;
	p[1] = (uint8_t) (val >> 8);
	p[2] = (uint8_t) (val >> 16);
	p[3] = (uint8_t) (val >> 24);
}

static void
store16(rv32i_state *s, uint32_t addr, uint32_t val)
{
	uint8_t *p = s->ram + addr;

	p[0] = (uint8_t) val;
	p[1] = (uint8_t) (val >> 8);
}

/** Write a register, honouring x0 being hardwired to zero. */
static void
set_reg(rv32i_state *s, uint32_t rd, uint32_t val)
{
	if (rd != 0) {
		s->x[rd] = val;
	}
}

void
rv32i_init(rv32i_state *s, uint8_t *ram, uint32_t ram_size)
{
	rv32i_reset(s, 0);
	s->ram = ram;
	s->ram_size = ram_size;
}

void
rv32i_reset(rv32i_state *s, uint32_t entry)
{
	uint8_t *ram = s->ram;
	const uint32_t ram_size = s->ram_size;
	unsigned i;

	for (i = 0; i < RV32I_REGS; i++) {
		s->x[i] = 0;
	}
	s->pc = entry;
	s->halted = 0;
	s->halt_reason = 0;
	s->exit_code = 0;
	s->faulted = 0;
	s->fault_cause = 0;
	s->fault_addr = 0;
	s->cycles = 0;

	/* Memory survives a reset, because on real hardware it does and because a
	   card resets its core after the guest has loaded a program into it. */
	s->ram = ram;
	s->ram_size = ram_size;
}

/**
 * Register-register and register-immediate ALU work.
 *
 * @is_imm distinguishes OP-IMM from OP; @alt is bit 30 of the instruction, which
 * is what separates ADD from SUB and SRL from SRA.
 */
static uint32_t
alu(uint32_t funct3, uint32_t a, uint32_t b, int is_imm, int alt)
{
	switch (funct3) {
	case 0:	/* ADD / ADDI, or SUB */
		/* Unsigned arithmetic, so the wrap is defined rather than an
		   overflow the sanitisers would object to. */
		return (!is_imm && alt) ? (a - b) : (a + b);

	case 1:	/* SLL */
		return a << (b & 31u);

	case 2:	/* SLT */
		return ((int32_t) a < (int32_t) b) ? 1u : 0u;

	case 3:	/* SLTU */
		return (a < b) ? 1u : 0u;

	case 4:	/* XOR */
		return a ^ b;

	case 5:	/* SRL or SRA */
		return alt ? sra32(a, b & 31u) : (a >> (b & 31u));

	case 6:	/* OR */
		return a | b;

	default: /* 7, AND */
		return a & b;
	}
}

/**
 * The M extension.
 *
 * The two cases C leaves undefined and RISC-V defines are handled first: divide
 * by zero, and the signed overflow of the most negative value divided by -1.
 */
static uint32_t
muldiv(uint32_t funct3, uint32_t a, uint32_t b)
{
	const int32_t sa = (int32_t) a;
	const int32_t sb = (int32_t) b;

	switch (funct3) {
	case 0:	/* MUL - low half, so unsigned multiply gives the same bits */
		return a * b;

	case 1:	/* MULH - both signed. Converted to unsigned before the shift
		   because right shift of a negative value is implementation
		   defined; the conversion itself is not. */
		return (uint32_t) (((uint64_t) ((int64_t) sa * (int64_t) sb)) >> 32);

	case 2:	/* MULHSU - signed times unsigned. The product of the widest
		   operands still fits in int64_t, so there is no overflow. */
		return (uint32_t) (((uint64_t) ((int64_t) sa * (int64_t) (uint64_t) b)) >> 32);

	case 3:	/* MULHU - both unsigned */
		return (uint32_t) (((uint64_t) a * (uint64_t) b) >> 32);

	case 4:	/* DIV */
		if (sb == 0) {
			return 0xffffffffu;		/* -1, per the manual */
		}
		if (sa == INT32_MIN && sb == -1) {
			return (uint32_t) INT32_MIN;	/* overflow wraps to itself */
		}
		return (uint32_t) (sa / sb);

	case 5:	/* DIVU */
		return (b == 0) ? 0xffffffffu : (a / b);

	case 6:	/* REM */
		if (sb == 0) {
			return a;			/* the dividend, per the manual */
		}
		if (sa == INT32_MIN && sb == -1) {
			return 0;
		}
		return (uint32_t) (sa % sb);

	default: /* 7, REMU */
		return (b == 0) ? a : (a % b);
	}
}

/** Should this branch be taken? */
static int
branch_taken(uint32_t funct3, uint32_t a, uint32_t b)
{
	switch (funct3) {
	case 0:	return a == b;					/* BEQ */
	case 1:	return a != b;					/* BNE */
	case 4:	return (int32_t) a < (int32_t) b;		/* BLT */
	case 5:	return (int32_t) a >= (int32_t) b;		/* BGE */
	case 6:	return a < b;					/* BLTU */
	case 7:	return a >= b;					/* BGEU */
	default: return -1;	/* 2 and 3 do not exist: illegal instruction */
	}
}

int
rv32i_step(rv32i_state *s)
{
	uint32_t insn, imm, addr, a, b, next_pc;
	uint32_t opcode, rd, rs1, rs2, funct3, funct7;
	const uint32_t pc = s->pc;
	int taken;

	if (s->halted || s->faulted) {
		return 0;
	}

	/* Fetch. A misaligned pc is its own exception in RISC-V, and since the C
	   extension is not implemented every instruction is four bytes. */
	if ((pc & 3u) != 0) {
		fault(s, RV32I_FAULT_INSN_MISALIGNED, pc);
		return 0;
	}
	if (!in_range(s, pc, 4)) {
		fault(s, RV32I_FAULT_INSN_ACCESS, pc);
		return 0;
	}
	insn = load32(s, pc);

	opcode = insn & 0x7fu;
	rd     = (insn >> 7) & 0x1fu;
	funct3 = (insn >> 12) & 0x7u;
	rs1    = (insn >> 15) & 0x1fu;
	rs2    = (insn >> 20) & 0x1fu;
	funct7 = (insn >> 25) & 0x7fu;

	next_pc = pc + 4u;

	switch (opcode) {
	case OP_LUI:
		set_reg(s, rd, insn & 0xfffff000u);
		break;

	case OP_AUIPC:
		set_reg(s, rd, pc + (insn & 0xfffff000u));
		break;

	case OP_JAL:
		imm = (((insn >> 31) & 1u) << 20) |
		      (((insn >> 21) & 0x3ffu) << 1) |
		      (((insn >> 20) & 1u) << 11) |
		      (((insn >> 12) & 0xffu) << 12);
		imm = sign_extend(imm, 21);
		/* The link register is written before the jump so that
		   `jal x1, somewhere` with rd == rs1 cannot matter. */
		set_reg(s, rd, next_pc);
		next_pc = pc + imm;
		break;

	case OP_JALR:
		if (funct3 != 0) {
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		imm = sign_extend(insn >> 20, 12);
		/* rs1 is read before rd is written, which is the whole reason for
		   the temporary: `jalr x1, 0(x1)` is an ordinary return. */
		addr = (s->x[rs1] + imm) & ~1u;	/* bit 0 is cleared, not a fault */
		set_reg(s, rd, next_pc);
		next_pc = addr;
		break;

	case OP_BRANCH:
		taken = branch_taken(funct3, s->x[rs1], s->x[rs2]);
		if (taken < 0) {
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		if (taken) {
			imm = (((insn >> 31) & 1u) << 12) |
			      (((insn >> 25) & 0x3fu) << 5) |
			      (((insn >> 8) & 0xfu) << 1) |
			      (((insn >> 7) & 1u) << 11);
			next_pc = pc + sign_extend(imm, 13);
		}
		break;

	case OP_LOAD:
		imm = sign_extend(insn >> 20, 12);
		addr = s->x[rs1] + imm;
		switch (funct3) {
		case 0:	/* LB */
		case 4:	/* LBU */
			if (!in_range(s, addr, 1)) {
				fault(s, RV32I_FAULT_LOAD_ACCESS, addr);
				return 0;
			}
			a = s->ram[addr];
			set_reg(s, rd, (funct3 == 0) ? sign_extend(a, 8) : a);
			break;

		case 1:	/* LH */
		case 5:	/* LHU */
			if ((addr & 1u) != 0) {
				fault(s, RV32I_FAULT_LOAD_MISALIGNED, addr);
				return 0;
			}
			if (!in_range(s, addr, 2)) {
				fault(s, RV32I_FAULT_LOAD_ACCESS, addr);
				return 0;
			}
			a = load16(s, addr);
			set_reg(s, rd, (funct3 == 1) ? sign_extend(a, 16) : a);
			break;

		case 2:	/* LW */
			if ((addr & 3u) != 0) {
				fault(s, RV32I_FAULT_LOAD_MISALIGNED, addr);
				return 0;
			}
			if (!in_range(s, addr, 4)) {
				fault(s, RV32I_FAULT_LOAD_ACCESS, addr);
				return 0;
			}
			set_reg(s, rd, load32(s, addr));
			break;

		default:
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		break;

	case OP_STORE:
		imm = sign_extend(((insn >> 25) << 5) | ((insn >> 7) & 0x1fu), 12);
		addr = s->x[rs1] + imm;
		b = s->x[rs2];
		switch (funct3) {
		case 0:	/* SB */
			if (!in_range(s, addr, 1)) {
				fault(s, RV32I_FAULT_STORE_ACCESS, addr);
				return 0;
			}
			s->ram[addr] = (uint8_t) b;
			break;

		case 1:	/* SH */
			if ((addr & 1u) != 0) {
				fault(s, RV32I_FAULT_STORE_MISALIGNED, addr);
				return 0;
			}
			if (!in_range(s, addr, 2)) {
				fault(s, RV32I_FAULT_STORE_ACCESS, addr);
				return 0;
			}
			store16(s, addr, b);
			break;

		case 2:	/* SW */
			if ((addr & 3u) != 0) {
				fault(s, RV32I_FAULT_STORE_MISALIGNED, addr);
				return 0;
			}
			if (!in_range(s, addr, 4)) {
				fault(s, RV32I_FAULT_STORE_ACCESS, addr);
				return 0;
			}
			store32(s, addr, b);
			break;

		default:
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		break;

	case OP_OP_IMM:
		imm = sign_extend(insn >> 20, 12);
		if (funct3 == 1 || funct3 == 5) {
			/* Shifts by an immediate: the count is five bits and the rest
			   of the field selects logical or arithmetic. Anything else in
			   the top bits is not a defined encoding. */
			if ((funct7 & ~0x20u) != 0) {
				fault(s, RV32I_FAULT_ILLEGAL, insn);
				return 0;
			}
			set_reg(s, rd, alu(funct3, s->x[rs1], rs2, 0,
			                   (funct7 & 0x20u) != 0));
		} else {
			set_reg(s, rd, alu(funct3, s->x[rs1], imm, 1, 0));
		}
		break;

	case OP_OP:
		a = s->x[rs1];
		b = s->x[rs2];
		if (funct7 == 0x01u) {
			set_reg(s, rd, muldiv(funct3, a, b));
		} else if (funct7 == 0x00u || funct7 == 0x20u) {
			/* Only ADD/SUB and SRL/SRA have a second form, so bit 30 set
			   on anything else is not a defined encoding. */
			if (funct7 == 0x20u && funct3 != 0 && funct3 != 5) {
				fault(s, RV32I_FAULT_ILLEGAL, insn);
				return 0;
			}
			set_reg(s, rd, alu(funct3, a, b, 0, funct7 == 0x20u));
		} else {
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		break;

	case OP_MISC_MEM:
		/* FENCE and FENCE.I. Every access this core makes has completed
		   before the next instruction starts and there is no instruction
		   cache to invalidate, so doing nothing is a correct
		   implementation rather than a shortcut. */
		if (funct3 != 0 && funct3 != 1) {
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		break;

	case OP_SYSTEM:
		/* ECALL and EBREAK only. There is no machine mode to trap to, so
		   both stop the core and hand a0 back as the exit code; see the
		   header. Everything else in this opcode is a CSR instruction,
		   which this core does not implement. */
		if (funct3 != 0 || rd != 0 || rs1 != 0) {
			fault(s, RV32I_FAULT_ILLEGAL, insn);
			return 0;
		}
		imm = insn >> 20;
		if (imm == 0 || imm == 1) {
			s->halted = 1;
			s->halt_reason = (imm == 0) ? RV32I_HALT_ECALL
			                            : RV32I_HALT_EBREAK;
			s->exit_code = s->x[10];	/* a0 */
			/* pc moves past the ecall, so a core restarted without a
			   reset carries on after it rather than halting again. */
			s->pc = next_pc;
			s->cycles++;
			return 1;
		}
		fault(s, RV32I_FAULT_ILLEGAL, insn);
		return 0;

	default:
		fault(s, RV32I_FAULT_ILLEGAL, insn);
		return 0;
	}

	s->pc = next_pc;
	s->cycles++;
	return 1;
}

int
rv32i_run(rv32i_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = rv32i_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}

const char *
rv32i_fault_name(uint32_t cause)
{
	switch (cause) {
	case RV32I_FAULT_INSN_MISALIGNED:	return "misaligned instruction";
	case RV32I_FAULT_INSN_ACCESS:		return "instruction access";
	case RV32I_FAULT_ILLEGAL:		return "illegal instruction";
	case RV32I_FAULT_LOAD_MISALIGNED:	return "misaligned load";
	case RV32I_FAULT_LOAD_ACCESS:		return "load access";
	case RV32I_FAULT_STORE_MISALIGNED:	return "misaligned store";
	case RV32I_FAULT_STORE_ACCESS:		return "store access";
	default:				return "unknown";
	}
}
