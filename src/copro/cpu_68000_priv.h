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

#ifndef CPU_68000_PRIV_H
#define CPU_68000_PRIV_H

/*
 * What the four 68000 sources share. Not part of the core's interface: nothing
 * outside src/copro/cpu_68000*.c includes this, and cpu_68000.h is what the card
 * and the tests see.
 *
 * The split exists because this core is around four times the size of the ones
 * that fit comfortably in one file. See docs/copro-68000.md.
 */

#include "cpu_68000.h"

/* ------------------------------------------------------------------ memory */

/*
 * ★ EVERY ACCESS GOES THROUGH THESE FOUR, and the address is masked to 24 bits
 * on the way in, because that is what the part's address lines can carry. A
 * program that adds one to &FFFFFF reaches zero on real hardware and must here.
 */
#define M68K_ADDR_MASK	0x00ffffffu

uint8_t m68k_read8(cpu68000_state *s, uint32_t addr);
uint16_t m68k_read16(cpu68000_state *s, uint32_t addr);
uint32_t m68k_read32(cpu68000_state *s, uint32_t addr);
void m68k_write8(cpu68000_state *s, uint32_t addr, uint8_t val);
void m68k_write16(cpu68000_state *s, uint32_t addr, uint16_t val);
void m68k_write32(cpu68000_state *s, uint32_t addr, uint32_t val);

uint32_t m68k_read(cpu68000_state *s, uint32_t addr, m68k_size size);
void m68k_write(cpu68000_state *s, uint32_t addr, m68k_size size, uint32_t val);

uint16_t m68k_fetch16(cpu68000_state *s);
uint32_t m68k_fetch32(cpu68000_state *s);

/** Sign-extend a value of @size to 32 bits. */
uint32_t m68k_extend(uint32_t val, m68k_size size);
/** The mask for a value of @size: &ff, &ffff or &ffffffff. */
uint32_t m68k_mask(m68k_size size);

/* Has anything gone wrong that means the instruction must not continue? */
#define M68K_ABORTED(s)	((s)->stalled || (s)->faulted)

/* ------------------------------------------------------- effective addresses */

/*
 * A resolved operand. The 68000's twelve addressing modes all end up as either a
 * register or an address, plus immediate data which is neither, so one struct
 * covers reading and writing without the instruction implementations caring which
 * mode they were given.
 */
typedef struct {
	enum {
		M68K_EA_DATA_REG,	/**< Dn */
		M68K_EA_ADDR_REG,	/**< An */
		M68K_EA_MEMORY,		/**< an address was computed */
		M68K_EA_IMMEDIATE	/**< the value is inline in the instruction */
	} kind;
	unsigned reg;			/**< which register, for the two register kinds */
	uint32_t addr;			/**< the address, or the immediate value */
} m68k_ea;

/*
 * Resolve mode/reg into an operand, fetching any extension words.
 *
 * ★ THIS CHARGES THE INSTRUCTION FOR THE ADDRESSING, into extra_cycles, because
 * on a 68000 the cost of reaching an operand is not a property of the opcode: the
 * same MOVE costs four cycles or eighteen depending on how its operands are
 * named. That is the 6809's postbyte problem with more modes and two sizes.
 *
 * Predecrement and postincrement modify the register here, which is why an
 * abandoned instruction has to put the registers back.
 */
void m68k_ea_calc(cpu68000_state *s, unsigned mode, unsigned reg,
                  m68k_size size, m68k_ea *ea);

uint32_t m68k_ea_read(cpu68000_state *s, const m68k_ea *ea, m68k_size size);
void m68k_ea_write(cpu68000_state *s, const m68k_ea *ea, m68k_size size,
                   uint32_t val);

/** Does this mode name memory? Used to refuse the modes an instruction forbids. */
int m68k_ea_is_memory(unsigned mode, unsigned reg);
/** Can this mode be written to? Program-counter relative and immediate cannot. */
int m68k_ea_is_writable(unsigned mode, unsigned reg);

/* ------------------------------------------------------------------- flags */

void m68k_set_nz(cpu68000_state *s, uint32_t val, m68k_size size);
void m68k_set_flag(cpu68000_state *s, uint16_t bit, int on);
int m68k_flag(const cpu68000_state *s, uint16_t bit);

/** The condition codes, by the encoding's four-bit condition field. */
int m68k_cond(const cpu68000_state *s, unsigned cond);

/* --------------------------------------------------------------- exceptions */

/*
 * Take an exception through @vector.
 *
 * ★ A VECTOR THAT HAS NEVER BEEN WRITTEN FAULTS RATHER THAN JUMPING. Real
 * hardware reads it and goes there, so a zero vector kills the program somewhere
 * unrelated with nothing to say why; this reports CPU68000_FAULT_NO_HANDLER
 * instead. It is the only deliberate divergence in the exception model, and a
 * guest that wants the hardware behaviour installs a vector of its own.
 */
void m68k_exception(cpu68000_state *s, unsigned vector);

/** An exception carrying the group 0 frame: bus error and address error. */
void m68k_exception_address(cpu68000_state *s, unsigned vector, uint32_t addr,
                            int is_write, int is_instruction);

/** Enter or leave supervisor mode, swapping which stack pointer a7 is. */
void m68k_set_supervisor(cpu68000_state *s, int supervisor);
/** Write the status register, honouring the swap above. */
void m68k_set_sr(cpu68000_state *s, uint16_t sr);

/* ---------------------------------------------------- the dispatch table */

/*
 * ★ ONE TABLE, BUILT ONCE, FROM THE RULES.
 *
 * A handler and a cycle cost for each of the 65,536 encodings. A NULL handler
 * means the encoding is not an instruction, so "does this exist?" and "what does
 * it cost?" come from the same place and cannot disagree - which is what went
 * wrong when the 65C02's overrides were typed by hand.
 *
 * The base cost here excludes the addressing, which m68k_ea_calc charges as it
 * resolves the operand.
 */
typedef void (*m68k_handler)(cpu68000_state *s, uint16_t op);

extern m68k_handler m68k_handlers[0x10000];
extern uint8_t m68k_base_cycles[0x10000];

/** Build the tables. Idempotent, and called by cpu68000_init. */
void m68k_build_tables(void);

/*
 * The instruction implementations register themselves through this, from
 * cpu_68000_ops.c. @mask and @match select the encodings a handler claims: an
 * encoding is claimed when (op & mask) == match.
 */
void m68k_install(uint16_t mask, uint16_t match, m68k_handler fn, uint8_t cycles);

/** Every group's install function, called in order by m68k_build_tables. */
void m68k_install_moves(void);
void m68k_install_alu(void);
void m68k_install_shifts(void);
void m68k_install_bits(void);
void m68k_install_branches(void);
void m68k_install_misc(void);

#endif /* CPU_68000_PRIV_H */
