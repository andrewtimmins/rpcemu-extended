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
 * cpu_68000_exc.c - privilege, the vector table and the three exception groups.
 *
 * This is the part of the 68000 that the 8-bit cores on this card have no
 * equivalent of, and the part that makes running a real machine's ROM possible:
 * an Atari ST or Mac Plus ROM installs a vector table and runs its operating
 * system in supervisor mode within a few instructions of reset.
 *
 * The three groups, which differ in what they push and what they cost:
 *
 *   group 0  bus error, address error       a 14-byte frame, and 50 cycles
 *   group 1  trace, interrupt, illegal,     the ordinary 6-byte frame
 *            privilege violation
 *   group 2  TRAP, TRAPV, CHK, div-by-zero  the same frame, taken by an
 *                                           instruction rather than to it
 */

#include "cpu_68000_priv.h"

/*
 * ★ WHICH STACK POINTER A7 IS depends on the supervisor bit, so changing that bit
 * means swapping the register. Keeping the inactive one in its own field and
 * exchanging on transition is the whole mechanism: a7 is always the active stack,
 * so every instruction that touches (A7) or -(A7) needs to know nothing about
 * privilege at all.
 */
void
m68k_set_supervisor(cpu68000_state *s, int supervisor)
{
	const int was = m68k_flag(s, CPU68000_SR_S);

	if ((supervisor != 0) == (was != 0)) {
		return;
	}
	if (was) {
		s->ssp = s->a[7];
		s->a[7] = s->usp;
	} else {
		s->usp = s->a[7];
		s->a[7] = s->ssp;
	}
	m68k_set_flag(s, CPU68000_SR_S, supervisor);
}

void
m68k_set_sr(cpu68000_state *s, uint16_t sr)
{
	sr = (uint16_t) (sr & CPU68000_SR_VALID);
	m68k_set_supervisor(s, (sr & CPU68000_SR_S) != 0);
	/* The supervisor bit is already applied by the swap above; take the rest
	   wholesale so the interrupt mask and trace bit follow. */
	s->sr = (uint16_t) ((sr & ~CPU68000_SR_S) | (s->sr & CPU68000_SR_S));
}

/* Pushes go on the supervisor stack, which by here is a7. */
static void
push16(cpu68000_state *s, uint16_t val)
{
	s->a[7] = (s->a[7] - 2) & M68K_ADDR_MASK;
	m68k_write16(s, s->a[7], val);
}

static void
push32(cpu68000_state *s, uint32_t val)
{
	s->a[7] = (s->a[7] - 4) & M68K_ADDR_MASK;
	m68k_write32(s, s->a[7], val);
}

/* What each vector costs to take. Anything not listed takes the group 1 cost. */
static int
exception_cycles(unsigned vector)
{
	switch (vector) {
	case CPU68000_VEC_BUS_ERROR:
	case CPU68000_VEC_ADDRESS_ERROR:	return 50;
	case CPU68000_VEC_DIV_ZERO:		return 38;
	case CPU68000_VEC_CHK:			return 40;
	default:
		if (vector >= CPU68000_VEC_AUTOVECTOR &&
		    vector < CPU68000_VEC_TRAP) {
			return 44;		/* an interrupt */
		}
		return 34;
	}
}

/*
 * ★ THE ONE DELIBERATE DIVERGENCE IN THIS MODEL.
 *
 * Real hardware reads the vector and goes there, so a vector that has never been
 * written sends the processor to address 0, where it executes whatever happens to
 * be at the bottom of memory and dies somewhere with no relation to the cause.
 * That is faithful and it is useless.
 *
 * So a zero vector is reported as a fault naming the exception instead. A guest
 * that wants the hardware behaviour writes a vector pointing at its own code,
 * which is what a real machine's ROM does in its first few instructions anyway.
 */
static int
vector_target(cpu68000_state *s, unsigned vector, uint32_t *out)
{
	const uint32_t at = (uint32_t) vector * 4u;
	const uint32_t target = m68k_read32(s, at);

	if (M68K_ABORTED(s)) {
		return 0;
	}
	if (target == 0) {
		s->faulted = 1;
		s->fault_cause = CPU68000_FAULT_NO_HANDLER;
		s->fault_addr = vector;
		return 0;
	}
	*out = target;
	return 1;
}

/*
 * ★ A FAULT WHILE TAKING AN EXCEPTION IS NOT ANOTHER EXCEPTION. A real 68000
 * that takes a bus error while pushing a bus error's frame stops dead - it has
 * nowhere to record the second one and no way to proceed. Reporting that as a
 * double fault says what happened; recursing would either loop or overflow the
 * stack into whatever is below it.
 */
static int in_exception;

static void
enter(cpu68000_state *s, unsigned vector, uint16_t sr_before)
{
	uint32_t target;

	if (!vector_target(s, vector, &target)) {
		return;
	}

	/* Every exception runs in supervisor mode with tracing off. The frame
	   records the mode the interrupted code was in, through its copy of SR. */
	m68k_set_supervisor(s, 1);
	m68k_set_flag(s, CPU68000_SR_T, 0);

	push32(s, s->pc);
	push16(s, sr_before);
	if (M68K_ABORTED(s)) {
		return;
	}
	s->pc = target & M68K_ADDR_MASK;
	s->extra_cycles += exception_cycles(vector);
}

void
m68k_exception(cpu68000_state *s, unsigned vector)
{
	const uint16_t sr_before = s->sr;

	if (in_exception) {
		s->faulted = 1;
		s->fault_cause = CPU68000_FAULT_DOUBLE;
		s->fault_addr = vector;
		return;
	}
	in_exception = 1;
	enter(s, vector, sr_before);
	in_exception = 0;
}

/*
 * The group 0 frame, which is the awkward one: fourteen bytes rather than six,
 * carrying enough for a handler to work out what the failed access was.
 *
 * In memory, lowest address first, which is the reverse of the order it is
 * pushed in:
 *
 *   +0  the special status word
 *   +2  the address the access was to
 *   +6  the instruction being executed
 *   +8  the status register as it was
 *   +10 the program counter
 */
void
m68k_exception_address(cpu68000_state *s, unsigned vector, uint32_t addr,
                       int is_write, int is_instruction)
{
	const uint16_t sr_before = s->sr;
	uint32_t target;
	uint16_t ssw;

	if (in_exception) {
		s->faulted = 1;
		s->fault_cause = CPU68000_FAULT_DOUBLE;
		s->fault_addr = addr;
		return;
	}
	in_exception = 1;

	if (!vector_target(s, vector, &target)) {
		in_exception = 0;
		return;
	}

	/*
	 * The special status word. Bit 4 says the access was a read, bit 3 that
	 * it was not an instruction fetch, and the low three bits are the
	 * function code the bus would have carried - which encodes the privilege
	 * level and whether it was program or data space.
	 *
	 * The exact bit sense here is stated differently in different references;
	 * the vector suite is what settles it, and this is the reading being
	 * checked.
	 */
	ssw = (uint16_t) ((is_write ? 0u : 0x10u) |
	                  (is_instruction ? 0u : 0x08u));
	ssw |= (uint16_t) (m68k_flag(s, CPU68000_SR_S) ? 4u : 0u);
	ssw |= (uint16_t) (is_instruction ? 2u : 1u);

	m68k_set_supervisor(s, 1);
	m68k_set_flag(s, CPU68000_SR_T, 0);

	push32(s, s->pc);
	push16(s, sr_before);
	push16(s, 0);		/* the instruction register; see below */
	push32(s, addr);
	push16(s, ssw);

	if (!M68K_ABORTED(s)) {
		s->pc = target & M68K_ADDR_MASK;
		s->extra_cycles += exception_cycles(vector);
	}
	in_exception = 0;
}

/*
 * ★ THE INSTRUCTION REGISTER IN THE FRAME IS PUSHED AS ZERO, and that is a known
 * gap rather than an oversight. A real 68000 pushes whatever was left in its
 * internal instruction register, which for a faulted access is not reliably the
 * instruction that faulted - the part was already prefetching the next one. Even
 * Motorola's documentation says a handler cannot depend on it.
 *
 * Reproducing the prefetch state properly means modelling the prefetch queue,
 * which nothing else in this core needs. If the vector suite disagrees here, this
 * comment is the first place to look.
 */

int
cpu68000_interrupt(cpu68000_state *s, unsigned level, uint8_t vector,
                   int autovec)
{
	const uint16_t sr_before = s->sr;
	const unsigned mask = (unsigned) ((s->sr & CPU68000_SR_IMASK) >> 8);
	unsigned vec;

	if (s->faulted) {
		return 0;
	}
	/*
	 * ★ A HALT CAUSED BY STOP IS RESUMABLE, and it is the only kind of halt
	 * this core has. That is the whole bargain STOP makes: the card reports a
	 * stopped core as halted so a guest that never interrupts sees a finished
	 * program, and an interrupt arriving means the program was waiting rather
	 * than finished. Refusing to interrupt a halted core, which is right for
	 * every other processor here, would make the second half of that
	 * unreachable.
	 */
	if (s->halted && !s->stopped) {
		return 0;
	}
	if (level == 0 || level > 7) {
		return 0;
	}

	/*
	 * ★ LEVEL 7 IS TAKEN WHATEVER THE MASK SAYS. It is edge-triggered on real
	 * hardware rather than level-sensitive, which makes it the part's
	 * non-maskable interrupt in everything but name - and it is what the
	 * card's IRQ_NMI bit maps onto.
	 */
	if (level != 7 && level <= mask) {
		return 0;
	}

	/*
	 * An interrupt is the other half of what STOP means: the instruction
	 * stops the processor until one arrives, so arriving starts it again.
	 */
	s->stopped = 0;
	s->halted = 0;
	s->halt_reason = 0;

	vec = autovec ? (CPU68000_VEC_AUTOVECTOR + level) : vector;

	if (in_exception) {
		return 0;
	}
	in_exception = 1;
	{
		uint32_t target;

		if (!vector_target(s, vec, &target)) {
			in_exception = 0;
			return 0;
		}
		m68k_set_supervisor(s, 1);
		m68k_set_flag(s, CPU68000_SR_T, 0);
		push32(s, s->pc);
		push16(s, sr_before);
		/* The mask rises to the level being serviced, so a handler is not
		   interrupted by its own level or anything below it. */
		s->sr = (uint16_t) ((s->sr & ~CPU68000_SR_IMASK) |
		                    ((level & 7u) << 8));
		if (!M68K_ABORTED(s)) {
			s->pc = target & M68K_ADDR_MASK;
			s->cycles += 44;
		}
	}
	in_exception = 0;
	return 1;
}
