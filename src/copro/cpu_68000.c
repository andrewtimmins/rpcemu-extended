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
 * cpu_68000.c - memory, flags, the dispatch table and the instruction loop.
 *
 * The addressing modes are in cpu_68000_ea.c, the privilege and exception model
 * in cpu_68000_exc.c, and the instructions themselves in cpu_68000_ops.c. What
 * each of those is for, and why this core is four files where every other is one,
 * is in docs/copro-68000.md.
 */

#include <string.h>

#include "cpu_68000_priv.h"

/* ------------------------------------------------------------------ memory */

/*
 * ★ THE ADDRESS IS MASKED TO 24 BITS ON EVERY ACCESS, not just checked. A 68000
 * has 24 address lines, so &1000000 IS &000000 to it and a program that walks off
 * the top of memory wraps rather than faulting. Bounds-checking without masking
 * first would report a fault where the hardware quietly wraps, which is a
 * different processor.
 */
static uint8_t *
resolve(cpu68000_state *s, uint32_t addr)
{
	if (s->ram == NULL || addr >= s->ram_size) {
		return NULL;
	}
	return &s->ram[addr];
}

uint8_t
m68k_read8(cpu68000_state *s, uint32_t addr)
{
	addr &= M68K_ADDR_MASK;

	if (s->mem.read != NULL) {
		uint8_t val = 0;

		switch (s->mem.read(s->mem.ctx, addr, &val)) {
		case CPU_MEM_OK:
			return val;
		case CPU_MEM_STALL:
			/* Abandon the instruction; cpu68000_step puts the registers
			   back and it is retried once the guest has answered. See
			   cpu_mem.h, and the note on transfer_done in cpu_68000.h
			   for the instructions that resume instead. */
			s->stalled = 1;
			s->stall_addr = addr;
			s->stall_is_write = 0;
			return 0;
		default:
			m68k_exception_address(s, CPU68000_VEC_BUS_ERROR, addr, 0, 0);
			return 0;
		}
	}

	{
		const uint8_t *p = resolve(s, addr);

		if (p == NULL) {
			m68k_exception_address(s, CPU68000_VEC_BUS_ERROR, addr, 0, 0);
			return 0;
		}
		return *p;
	}
}

void
m68k_write8(cpu68000_state *s, uint32_t addr, uint8_t val)
{
	addr &= M68K_ADDR_MASK;

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
			m68k_exception_address(s, CPU68000_VEC_BUS_ERROR, addr, 1, 0);
			return;
		}
	}

	{
		uint8_t *p = resolve(s, addr);

		if (p == NULL) {
			m68k_exception_address(s, CPU68000_VEC_BUS_ERROR, addr, 1, 0);
			return;
		}
		*p = val;
	}
}

/*
 * ★ A WORD OR LONG AT AN ODD ADDRESS IS AN ADDRESS ERROR, not a slow access.
 * The part cannot do it: it has no way to put a 16-bit quantity on the bus
 * straddling two words. Code written for a 68020, which removed the restriction,
 * is exactly the code that appears to work and would not on real hardware, so
 * this is worth reproducing rather than tolerating.
 */
static int
odd(cpu68000_state *s, uint32_t addr, int is_write)
{
	if ((addr & 1u) == 0) {
		return 0;
	}
	m68k_exception_address(s, CPU68000_VEC_ADDRESS_ERROR, addr, is_write, 0);
	return 1;
}

uint16_t
m68k_read16(cpu68000_state *s, uint32_t addr)
{
	addr &= M68K_ADDR_MASK;
	if (odd(s, addr, 0)) {
		return 0;
	}
	/* Big-endian: the high byte is at the lower address. */
	return (uint16_t) ((m68k_read8(s, addr) << 8) |
	                   m68k_read8(s, (addr + 1) & M68K_ADDR_MASK));
}

uint32_t
m68k_read32(cpu68000_state *s, uint32_t addr)
{
	addr &= M68K_ADDR_MASK;
	if (odd(s, addr, 0)) {
		return 0;
	}
	{
		const uint32_t hi = m68k_read16(s, addr);

		return (hi << 16) | m68k_read16(s, (addr + 2) & M68K_ADDR_MASK);
	}
}

void
m68k_write16(cpu68000_state *s, uint32_t addr, uint16_t val)
{
	addr &= M68K_ADDR_MASK;
	if (odd(s, addr, 1)) {
		return;
	}
	m68k_write8(s, addr, (uint8_t) (val >> 8));
	m68k_write8(s, (addr + 1) & M68K_ADDR_MASK, (uint8_t) val);
}

void
m68k_write32(cpu68000_state *s, uint32_t addr, uint32_t val)
{
	addr &= M68K_ADDR_MASK;
	if (odd(s, addr, 1)) {
		return;
	}
	m68k_write16(s, addr, (uint16_t) (val >> 16));
	m68k_write16(s, (addr + 2) & M68K_ADDR_MASK, (uint16_t) val);
}

uint32_t
m68k_read(cpu68000_state *s, uint32_t addr, m68k_size size)
{
	switch (size) {
	case M68K_BYTE: return m68k_read8(s, addr);
	case M68K_WORD: return m68k_read16(s, addr);
	default:        return m68k_read32(s, addr);
	}
}

void
m68k_write(cpu68000_state *s, uint32_t addr, m68k_size size, uint32_t val)
{
	switch (size) {
	case M68K_BYTE: m68k_write8(s, addr, (uint8_t) val); break;
	case M68K_WORD: m68k_write16(s, addr, (uint16_t) val); break;
	default:        m68k_write32(s, addr, val); break;
	}
}

/*
 * ★ AN INSTRUCTION FETCH FROM AN ODD ADDRESS IS ALSO AN ADDRESS ERROR, and the
 * frame records that it was an instruction fetch rather than a data access. Every
 * instruction is a whole number of words, so this only happens after something
 * has already gone wrong - a corrupted return address, a jump to a computed
 * address that was not even. Which is exactly when a program needs to be told.
 */
uint16_t
m68k_fetch16(cpu68000_state *s)
{
	const uint32_t at = s->pc & M68K_ADDR_MASK;

	if ((at & 1u) != 0) {
		m68k_exception_address(s, CPU68000_VEC_ADDRESS_ERROR, at, 0, 1);
		return 0;
	}
	s->pc = (at + 2) & M68K_ADDR_MASK;
	return (uint16_t) ((m68k_read8(s, at) << 8) |
	                   m68k_read8(s, (at + 1) & M68K_ADDR_MASK));
}

uint32_t
m68k_fetch32(cpu68000_state *s)
{
	const uint32_t hi = m68k_fetch16(s);

	return (hi << 16) | m68k_fetch16(s);
}

uint32_t
m68k_mask(m68k_size size)
{
	switch (size) {
	case M68K_BYTE: return 0xffu;
	case M68K_WORD: return 0xffffu;
	default:        return 0xffffffffu;
	}
}

uint32_t
m68k_extend(uint32_t val, m68k_size size)
{
	switch (size) {
	case M68K_BYTE: return (uint32_t) (int32_t) (int8_t) val;
	case M68K_WORD: return (uint32_t) (int32_t) (int16_t) val;
	default:        return val;
	}
}

/* ------------------------------------------------------------------- flags */

void
m68k_set_flag(cpu68000_state *s, uint16_t bit, int on)
{
	if (on) {
		s->sr |= bit;
	} else {
		s->sr = (uint16_t) (s->sr & ~bit);
	}
}

int
m68k_flag(const cpu68000_state *s, uint16_t bit)
{
	return (s->sr & bit) != 0;
}

void
m68k_set_nz(cpu68000_state *s, uint32_t val, m68k_size size)
{
	const uint32_t m = m68k_mask(size);
	const uint32_t v = val & m;

	m68k_set_flag(s, CPU68000_FLAG_N, (v & (m ^ (m >> 1))) != 0);
	m68k_set_flag(s, CPU68000_FLAG_Z, v == 0);
}

/*
 * The sixteen conditions, by the encoding's condition field. Bcc, Scc and DBcc
 * all use it, so it is one function rather than three copies.
 *
 * ★ CARRY MEANS BORROW AFTER A SUBTRACTION here as on the Motorola 8-bits, so
 * "carry clear" is "higher or same" and not the 6502's reading of it.
 */
int
m68k_cond(const cpu68000_state *s, unsigned cond)
{
	const int c = m68k_flag(s, CPU68000_FLAG_C);
	const int v = m68k_flag(s, CPU68000_FLAG_V);
	const int z = m68k_flag(s, CPU68000_FLAG_Z);
	const int n = m68k_flag(s, CPU68000_FLAG_N);

	switch (cond & 0xfu) {
	case 0x0: return 1;			/* T  */
	case 0x1: return 0;			/* F  */
	case 0x2: return !c && !z;		/* HI */
	case 0x3: return c || z;		/* LS */
	case 0x4: return !c;			/* CC, HS */
	case 0x5: return c;			/* CS, LO */
	case 0x6: return !z;			/* NE */
	case 0x7: return z;			/* EQ */
	case 0x8: return !v;			/* VC */
	case 0x9: return v;			/* VS */
	case 0xa: return !n;			/* PL */
	case 0xb: return n;			/* MI */
	case 0xc: return n == v;		/* GE */
	case 0xd: return n != v;		/* LT */
	case 0xe: return !z && n == v;		/* GT */
	default:  return z || n != v;		/* LE */
	}
}

/* ---------------------------------------------------- the dispatch table */

m68k_handler m68k_handlers[0x10000];
uint8_t m68k_base_cycles[0x10000];

/*
 * Claim every encoding matching (op & mask) == match.
 *
 * ★ IT REFUSES TO CLAIM AN ENCODING TWICE, by asserting nothing is there. The
 * groups overlap in the 68000's map - &4xxx holds twenty unrelated instructions
 * and several of them differ only in bits the others do not look at - so an
 * install whose mask is one bit too loose would silently steal encodings from a
 * group installed later. Failing loudly at build time is the whole reason the
 * table is generated rather than typed.
 */
static int m68k_install_clash;

void
m68k_install(uint16_t mask, uint16_t match, m68k_handler fn, uint8_t cycles)
{
	uint32_t op;

	for (op = 0; op < 0x10000u; op++) {
		if ((op & mask) != match) {
			continue;
		}
		if (m68k_handlers[op] != NULL) {
			m68k_install_clash++;
			continue;
		}
		m68k_handlers[op] = fn;
		m68k_base_cycles[op] = cycles;
	}
}

/** Non-zero if any encoding was claimed twice. For the tests to assert on. */
int m68k_table_clashes(void);

int
m68k_table_clashes(void)
{
	return m68k_install_clash;
}

void
m68k_build_tables(void)
{
	static int built;

	if (built) {
		return;
	}
	built = 1;

	memset(m68k_handlers, 0, sizeof(m68k_handlers));
	memset(m68k_base_cycles, 0, sizeof(m68k_base_cycles));

	/* Order matters only in that a clash is reported rather than resolved;
	   see m68k_install. Each group claims what is genuinely its own. */
	m68k_install_moves();
	m68k_install_alu();
	m68k_install_shifts();
	m68k_install_bits();
	m68k_install_branches();
	m68k_install_misc();
}

/* ------------------------------------------------------------ step and run */

static int
abandon(cpu68000_state *s)
{
	memcpy(s->d, s->saved.d, sizeof(s->d));
	memcpy(s->a, s->saved.a, sizeof(s->a));
	s->usp = s->saved.usp;
	s->ssp = s->saved.ssp;
	s->pc = s->saved.pc;
	s->sr = s->saved.sr;
	/* transfer_done is deliberately NOT restored: it is how a resumed MOVEM
	   knows what it already moved. See cpu_68000.h. */
	return 0;
}

int
cpu68000_step(cpu68000_state *s)
{
	uint16_t op;
	m68k_handler fn;

	if (s->halted || s->faulted || s->stopped) {
		return 0;
	}
	if (s->stalled) {
		return 0;		/* waiting for the guest to answer */
	}

	s->extra_cycles = 0;

	/* 72 bytes, so it is worth not doing when nothing can stall. */
	if (s->mem.can_stall) {
		memcpy(s->saved.d, s->d, sizeof(s->d));
		memcpy(s->saved.a, s->a, sizeof(s->a));
		s->saved.usp = s->usp;
		s->saved.ssp = s->ssp;
		s->saved.pc = s->pc;
		s->saved.sr = s->sr;
	}

	op = m68k_fetch16(s);
	if (s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return abandon(s);
	}

	fn = m68k_handlers[op];
	if (fn == NULL) {
		/*
		 * ★ &Axxx AND &Fxxx HAVE THEIR OWN VECTORS, and that is not a
		 * detail: the Macintosh's entire operating system is reached
		 * through line-A traps, so an emulator on this card needs them
		 * delivered as exceptions rather than reported as bad opcodes.
		 */
		if ((op & 0xf000u) == 0xa000u) {
			m68k_exception(s, CPU68000_VEC_LINE_A);
		} else if ((op & 0xf000u) == 0xf000u) {
			m68k_exception(s, CPU68000_VEC_LINE_F);
		} else {
			m68k_exception(s, CPU68000_VEC_ILLEGAL);
		}
		if (s->faulted) {
			return 0;
		}
		if (s->stalled) {
			return abandon(s);
		}
		/* m68k_exception has charged what taking it cost; adding a
		   figure here as well would count it twice. */
		s->cycles += (uint64_t) s->extra_cycles;
		return s->extra_cycles;
	}

	fn(s, op);

	if (s->faulted) {
		return 0;
	}
	if (s->stalled) {
		return abandon(s);
	}

	/* An instruction that completed has nothing left part-done. */
	s->transfer_done = 0;

	{
		const int n = m68k_base_cycles[op] + s->extra_cycles;

		s->cycles += (uint64_t) n;
		return n;
	}
}

int
cpu68000_run(cpu68000_state *s, int cycles)
{
	int used = 0;

	while (used < cycles) {
		const int n = cpu68000_step(s);

		if (n == 0) {
			break;
		}
		used += n;
	}
	return used;
}

/* ------------------------------------------------------------- entry points */

void
cpu68000_init(cpu68000_state *s, uint8_t *ram, uint32_t ram_size)
{
	m68k_build_tables();
	memset(s, 0, sizeof(*s));
	s->ram = ram;
	s->ram_size = ram_size;
	cpu68000_reset(s, 0);
}

void
cpu68000_set_mem_hook(cpu68000_state *s, const cpu_mem_hook *hook)
{
	if (hook == NULL) {
		memset(&s->mem, 0, sizeof(s->mem));
		return;
	}
	s->mem = *hook;
}

void
cpu68000_reset(cpu68000_state *s, uint32_t entry)
{
	memset(s->d, 0, sizeof(s->d));
	memset(s->a, 0, sizeof(s->a));
	s->usp = 0;
	s->ssp = 0;
	s->pc = entry & M68K_ADDR_MASK;
	/* Supervisor, tracing off, and the interrupt mask at 7, which is the state
	   a real part comes up in. */
	s->sr = (uint16_t) (CPU68000_SR_S | CPU68000_SR_IMASK);

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
	s->stopped = 0;
	s->extra_cycles = 0;
	s->transfer_done = 0;
}

const char *
cpu68000_fault_name(uint32_t cause)
{
	switch (cause) {
	case CPU68000_FAULT_ILLEGAL:
		return "not an instruction on this part";
	case CPU68000_FAULT_ACCESS:
		return "access outside the core's memory";
	case CPU68000_FAULT_NO_HANDLER:
		return "exception taken with no handler installed";
	case CPU68000_FAULT_DOUBLE:
		return "a fault while taking an exception";
	default:
		return "unknown";
	}
}
