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
 * openbus.c - the Risc PC second processor bus.
 *
 * The reasoning, and what this deliberately leaves to a card, are in openbus.h.
 * No emulator headers here on purpose: every host service arrives through
 * openbus_host_ops, so this file builds standalone and the bus rules can be
 * tested without a machine.
 */

#include "openbus.h"

#include <stddef.h>
#include <string.h>

static struct {
	const openbus_host_ops *ops;
	openbus_master master;
	int fitted;
	int started;

	/* Latched so a card asking for the state it already has costs nothing, and
	   so a reset can put the lines back without guessing. */
	int irq_asserted;
	int fiq_asserted;
} bus;

void
openbus_init(const openbus_host_ops *ops)
{
	memset(&bus, 0, sizeof(bus));
	bus.ops = ops;
	bus.started = ops != NULL;
}

void
openbus_close(void)
{
	openbus_remove();
	memset(&bus, 0, sizeof(bus));
}

int
openbus_fit(const openbus_master *master)
{
	if (!bus.started || master == NULL || master->name == NULL) {
		return -1;
	}
	/* Never replace silently: two cards in one slot is a configuration
	   mistake, and dropping one without a word would hide it. */
	if (bus.fitted) {
		return -1;
	}

	bus.master = *master;
	bus.fitted = 1;
	return 0;
}

void
openbus_remove(void)
{
	if (!bus.fitted) {
		return;
	}

	/* Lines down before the card goes. A card removed while holding nPIRQ would
	   otherwise leave the host with an interrupt nothing can ever clear. */
	openbus_master_set_irq(0);
	openbus_master_set_fiq(0);

	if (bus.master.close != NULL) {
		bus.master.close(bus.master.p);
	}
	memset(&bus.master, 0, sizeof(bus.master));
	bus.fitted = 0;
}

int
openbus_present(void)
{
	return bus.fitted;
}

const char *
openbus_name(void)
{
	return bus.fitted ? bus.master.name : NULL;
}

void
openbus_reset(void)
{
	/* nRESET drops the interrupt lines whether or not the card thinks to. */
	openbus_master_set_irq(0);
	openbus_master_set_fiq(0);

	if (bus.fitted && bus.master.reset != NULL) {
		bus.master.reset(bus.master.p);
	}
}

int
openbus_addr_in_window(uint32_t addr)
{
	return addr >= OPENBUS_REG_BASE && addr < OPENBUS_REG_END;
}

uint32_t
openbus_reg_read(uint32_t addr, openbus_size size)
{
	/* An empty slot is an undriven bus, which reads as all ones. Answering zero
	   would look like a card that is present and saying no. */
	if (!bus.fitted || bus.master.reg_read == NULL ||
	    !openbus_addr_in_window(addr)) {
		return 0xffffffffu;
	}
	return bus.master.reg_read(bus.master.p, addr - OPENBUS_REG_BASE, size);
}

void
openbus_reg_write(uint32_t addr, openbus_size size, uint32_t val)
{
	if (!bus.fitted || bus.master.reg_write == NULL ||
	    !openbus_addr_in_window(addr)) {
		return;
	}
	bus.master.reg_write(bus.master.p, addr - OPENBUS_REG_BASE, size, val);
}

int
openbus_run(int cycles)
{
	int used;

	if (!bus.fitted || bus.master.run == NULL || cycles <= 0) {
		return 0;
	}

	used = bus.master.run(bus.master.p, cycles);

	/*
	 * Clamped rather than trusted. A card returning more than it was offered
	 * would drive the host's cycle budget negative, and a card returning a
	 * negative would hand the host time it never had - either way the ARM's
	 * timing goes wrong in a manner that would be hard to trace back here.
	 */
	if (used < 0) {
		return 0;
	}
	if (used > cycles) {
		return cycles;
	}
	return used;
}

uint32_t
openbus_master_read32(uint32_t phys_addr)
{
	if (!bus.started || bus.ops->read32 == NULL) {
		return 0xffffffffu;
	}
	return bus.ops->read32(phys_addr);
}

void
openbus_master_write32(uint32_t phys_addr, uint32_t val)
{
	if (!bus.started || bus.ops->write32 == NULL) {
		return;
	}
	bus.ops->write32(phys_addr, val);
}

void
openbus_master_set_irq(int state)
{
	state = state ? 1 : 0;

	/* Only on a change. The host's aggregation walks every podule slot and
	   recalculates the IOMD interrupt status, so a card polling this every
	   instruction would otherwise be surprisingly expensive. */
	if (state == bus.irq_asserted) {
		return;
	}
	bus.irq_asserted = state;

	if (bus.started && bus.ops->set_irq != NULL) {
		bus.ops->set_irq(state);
	}
}

void
openbus_master_set_fiq(int state)
{
	state = state ? 1 : 0;

	if (state == bus.fiq_asserted) {
		return;
	}
	bus.fiq_asserted = state;

	if (bus.started && bus.ops->set_fiq != NULL) {
		bus.ops->set_fiq(state);
	}
}
