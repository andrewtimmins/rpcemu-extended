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

/* The address decode for the co-processor card. See copro_bus.h for what this
   is for and, more importantly, for what it deliberately is not. */

#include "copro_bus.h"

#include <string.h>

/* The tables are read out of the control area a field at a time rather than by
   casting a pointer to it: the guest wrote those bytes, the offset it gave is
   arbitrary, and a struct pointer into an unaligned offset is undefined even
   where it happens to work. */

static uint32_t
ctl_word(const copro_bus *bus, uint32_t offset)
{
	if (bus->ctl == NULL || offset + 4 > bus->ctl_size) {
		return 0;
	}
	return (uint32_t) bus->ctl[offset]
	     | ((uint32_t) bus->ctl[offset + 1] << 8)
	     | ((uint32_t) bus->ctl[offset + 2] << 16)
	     | ((uint32_t) bus->ctl[offset + 3] << 24);
}

static void
ctl_set_word(copro_bus *bus, uint32_t offset, uint32_t value)
{
	if (bus->ctl == NULL || offset + 4 > bus->ctl_size) {
		return;
	}
	bus->ctl[offset]     = (uint8_t) value;
	bus->ctl[offset + 1] = (uint8_t) (value >> 8);
	bus->ctl[offset + 2] = (uint8_t) (value >> 16);
	bus->ctl[offset + 3] = (uint8_t) (value >> 24);
}

void
copro_bus_init(copro_bus *bus, uint8_t *ram, uint32_t ram_size,
               uint8_t *ctl, uint32_t ctl_size)
{
	memset(bus, 0, sizeof(*bus));
	bus->ram = ram;
	bus->ram_size = ram_size;
	bus->ctl = ctl;
	bus->ctl_size = ctl_size;
}

int
copro_bus_set_map(copro_bus *bus, uint32_t offset, uint32_t count)
{
	if (count == 0) {
		bus->map_count = 0;
		return 1;
	}
	if (bus->ctl == NULL) {
		return 0;
	}
	/* Checked in a way that cannot overflow: a count the guest chose could
	   otherwise wrap the multiply and pass. */
	if (count > bus->ctl_size / sizeof(copro_region)) {
		return 0;
	}
	if (offset > bus->ctl_size - count * sizeof(copro_region)) {
		return 0;
	}

	bus->map_off = offset;
	bus->map_count = count;
	return 1;
}

int
copro_bus_set_log(copro_bus *bus, uint32_t offset, uint32_t entries)
{
	if (entries == 0) {
		bus->log_entries = 0;
		bus->log_head = 0;
		bus->log_tail = 0;
		return 1;
	}
	if (bus->ctl == NULL) {
		return 0;
	}
	if (entries > bus->ctl_size / sizeof(copro_log_entry)) {
		return 0;
	}
	if (offset > bus->ctl_size - entries * sizeof(copro_log_entry)) {
		return 0;
	}

	bus->log_off = offset;
	bus->log_entries = entries;
	bus->log_head = 0;
	bus->log_tail = 0;
	bus->log_dropped = 0;
	return 1;
}

const copro_region *
copro_bus_region(const copro_bus *bus, uint32_t addr, int is_port)
{
	uint32_t i;

	if (bus->map_count == 0 || bus->ctl == NULL) {
		return NULL;
	}

	for (i = 0; i < bus->map_count; i++) {
		const uint32_t at = bus->map_off + i * (uint32_t) sizeof(copro_region);
		const uint32_t size = ctl_word(bus, at + 4);
		const uint32_t kind = ctl_word(bus, at + 8);
		uint32_t base;

		/* A size of zero disables an entry, and no separate check is
		   needed for it: the range test below cannot match anything when
		   size is zero. A check here would be dead code, which is worth
		   saying because it was written that way first and a mutation test
		   showed nothing could tell the difference. */
		if (((kind & COPRO_REGION_PORT) != 0) != (is_port != 0)) {
			continue;	/* wrong space */
		}
		base = ctl_word(bus, at);
		/* Written as a subtraction so a region reaching the top of the
		   address space cannot wrap base + size and stop matching. */
		if (addr >= base && addr - base < size) {
			/* The caller only reads it; returning a pointer into the
			   control area is what avoids copying an entry per access. */
			return (const copro_region *) (const void *) (bus->ctl + at);
		}
	}
	return NULL;
}

/*
 * Read a region's fields through ctl_word rather than through the returned
 * pointer, for the alignment reason at the top of this file. The pointer is
 * still what copro_bus_region hands back, because a caller outside this file
 * (a test, or the card reporting a map) wants to see the entry.
 */
static void
region_fields(const copro_bus *bus, const copro_region *region,
              uint32_t *kind, uint32_t *latch, uint32_t *base)
{
	const uint32_t at = (uint32_t) ((const uint8_t *) region - bus->ctl);

	*base  = ctl_word(bus, at);
	*kind  = ctl_word(bus, at + 8);
	*latch = ctl_word(bus, at + 12);
}

uint32_t
copro_bus_log_pending(const copro_bus *bus)
{
	if (bus->log_entries == 0) {
		return 0;
	}
	if (bus->log_head >= bus->log_tail) {
		return bus->log_head - bus->log_tail;
	}
	return bus->log_entries - (bus->log_tail - bus->log_head);
}

/*
 * A full ring DROPS the new write and counts it, rather than overwriting the
 * oldest. Losing the newest is a gap the guest is told about on the next entry
 * it reads; overwriting the oldest silently renumbers everything it has not
 * read yet, and a guest reconstructing a display from an ordered list of writes
 * would have no way to know.
 */
static void
log_push(copro_bus *bus, uint32_t cycle, uint32_t addr, uint32_t value,
         uint32_t info)
{
	uint32_t next;
	uint32_t at;

	if (bus->log_entries == 0) {
		return;
	}

	next = bus->log_head + 1;
	if (next >= bus->log_entries) {
		next = 0;
	}
	if (next == bus->log_tail) {
		bus->log_dropped++;
		return;
	}

	if (bus->log_dropped != 0) {
		info |= COPRO_LOG_INFO_DROPPED;
	}

	at = bus->log_off + bus->log_head * (uint32_t) sizeof(copro_log_entry);
	ctl_set_word(bus, at,      cycle);
	ctl_set_word(bus, at + 4,  addr);
	ctl_set_word(bus, at + 8,  value);
	ctl_set_word(bus, at + 12, info);

	bus->log_head = next;
	bus->log_dropped = 0;
}

int
copro_bus_log_pop(copro_bus *bus, copro_log_entry *out)
{
	uint32_t at;

	if (bus->log_entries == 0 || bus->log_tail == bus->log_head) {
		return 0;
	}

	at = bus->log_off + bus->log_tail * (uint32_t) sizeof(copro_log_entry);
	out->cycle = ctl_word(bus, at);
	out->addr  = ctl_word(bus, at + 4);
	out->value = ctl_word(bus, at + 8);
	out->info  = ctl_word(bus, at + 12);

	bus->log_tail++;
	if (bus->log_tail >= bus->log_entries) {
		bus->log_tail = 0;
	}
	return 1;
}

/* Plain card RAM, wrapping as the aperture does: an address a core can form is
   always inside its own space, so this only matters for a map that describes a
   region beyond the RAM the card was given. */
static uint8_t
ram_read(const copro_bus *bus, uint32_t addr)
{
	if (bus->ram == NULL || bus->ram_size == 0) {
		return 0xff;
	}
	return bus->ram[addr % bus->ram_size];
}

static void
ram_write(copro_bus *bus, uint32_t addr, uint8_t value)
{
	if (bus->ram == NULL || bus->ram_size == 0) {
		return;
	}
	bus->ram[addr % bus->ram_size] = value;
}

/*
 * Has the guest already answered this access? Consumed if so, which is what
 * stops a retried instruction stalling on the same address for ever.
 */
static int
take_answer(copro_bus *bus, uint32_t addr, int is_write, uint8_t *value)
{
	if (!bus->answer_valid || bus->answer_addr != addr
	    || bus->answer_is_write != (uint32_t) (is_write != 0)) {
		return 0;
	}
	bus->answer_valid = 0;
	if (value != NULL) {
		*value = (uint8_t) bus->answer_value;
	}
	return 1;
}

/*
 * Where a region's data actually lives in card RAM.
 *
 * Without COPRO_REGION_OFFSET that is the address itself, which is what a
 * machine with no paging wants and what everything did before banking. With it,
 * the region is a window: the offset the entry carries is where its base maps
 * to, so the same addresses reach a different part of a larger card RAM when the
 * guest changes it. See copro_bus.h.
 */
static uint32_t
ram_address(uint32_t kind, uint32_t base, uint32_t offset, uint32_t addr)
{
	if ((kind & COPRO_REGION_OFFSET) != 0) {
		return offset + (addr - base);
	}
	return addr;
}

copro_bus_result
copro_bus_read(copro_bus *bus, uint32_t addr, int is_port, uint32_t cycle,
               uint8_t *value)
{
	const copro_region *region;
	uint32_t kind, latch, base;

	(void) cycle;

	if (take_answer(bus, addr, 0, value)) {
		return COPRO_BUS_OK;
	}
	region = copro_bus_region(bus, addr, is_port);

	if (region == NULL) {
		/* No map, or nothing describing this address. In the memory space
		   that is plain RAM, which is what the cores did before any of this
		   existed. In the port space it is an undriven bus. */
		*value = is_port ? 0xff : ram_read(bus, addr);
		return COPRO_BUS_OK;
	}

	region_fields(bus, region, &kind, &latch, &base);

	switch (COPRO_REGION_KIND(kind)) {
	case COPRO_REGION_RAM:
	case COPRO_REGION_ROM:
	case COPRO_REGION_LOG:
		*value = ram_read(bus, ram_address(kind, base, latch, addr));
		return COPRO_BUS_OK;

	case COPRO_REGION_LATCH: {
		/* The latch page holds one byte per address in the region, so a
		   machine with sixteen ports needs sixteen bytes and the guest
		   updates whichever it likes whenever it likes. */
		const uint32_t off = latch + (addr - base);

		*value = (bus->ctl != NULL && off < bus->ctl_size)
		    ? bus->ctl[off] : 0xff;
		return COPRO_BUS_OK;
	}

	case COPRO_REGION_STALL:
		/* Reported, not answered. The core stops, the guest reads what was
		   wanted and calls copro_bus_resume(). */
		bus->stalled = 1;
		bus->wait_addr = addr;
		bus->wait_is_write = 0;
		bus->wait_is_port = is_port ? 1 : 0;
		bus->wait_value = 0;
		*value = 0xff;
		return COPRO_BUS_STALL;

	default:
		*value = 0xff;
		return COPRO_BUS_BUSERROR;
	}
}

copro_bus_result
copro_bus_write(copro_bus *bus, uint32_t addr, int is_port, uint32_t cycle,
                uint8_t value)
{
	const copro_region *region;
	const uint32_t info = is_port ? COPRO_LOG_INFO_PORT : 0u;
	uint32_t kind, latch, base;

	/* The guest has already dealt with this write; the retry must let it
	   through rather than stall again. */
	if (take_answer(bus, addr, 1, NULL)) {
		return COPRO_BUS_OK;
	}
	region = copro_bus_region(bus, addr, is_port);

	if (region == NULL) {
		if (!is_port) {
			ram_write(bus, addr, value);
		}
		return COPRO_BUS_OK;
	}

	region_fields(bus, region, &kind, &latch, &base);

	switch (COPRO_REGION_KIND(kind)) {
	case COPRO_REGION_RAM:
		ram_write(bus, ram_address(kind, base, latch, addr), value);
		return COPRO_BUS_OK;

	case COPRO_REGION_ROM:
		/* Dropped, but logged: a program writing to ROM is either paging
		   something or is a bug, and both are worth being able to see. */
		log_push(bus, cycle, addr, value, info);
		return COPRO_BUS_OK;

	case COPRO_REGION_LOG:
		ram_write(bus, ram_address(kind, base, latch, addr), value);
		/* Logged at the address the PROGRAM wrote, not where it landed in
		   card RAM: the guest describes its machine in the core's
		   addresses and would have to undo the banking to make sense of
		   anything else. */
		log_push(bus, cycle, addr, value, info);
		return COPRO_BUS_OK;

	case COPRO_REGION_LATCH:
		/* Not stored: the latch is what the GUEST presents to the core, so
		   letting the core overwrite it would lose the guest's value. The
		   write is logged, which is how the guest learns of it. */
		log_push(bus, cycle, addr, value, info);
		return COPRO_BUS_OK;

	case COPRO_REGION_STALL:
		bus->stalled = 1;
		bus->wait_addr = addr;
		bus->wait_is_write = 1;
		bus->wait_is_port = is_port ? 1 : 0;
		bus->wait_value = value;
		return COPRO_BUS_STALL;

	default:
		return COPRO_BUS_BUSERROR;
	}
}

uint8_t
copro_bus_resume(copro_bus *bus, uint8_t value)
{
	if (!bus->stalled) {
		return value;
	}
	bus->stalled = 0;
	if (!bus->wait_is_write) {
		bus->wait_value = value;
	}

	/* Left for the retried access to find. */
	bus->answer_valid = 1;
	bus->answer_addr = bus->wait_addr;
	bus->answer_value = bus->wait_value;
	bus->answer_is_write = bus->wait_is_write;

	return (uint8_t) bus->wait_value;
}

int
copro_bus_can_stall(const copro_bus *bus)
{
	uint32_t i;

	for (i = 0; i < bus->map_count; i++) {
		const uint32_t at = bus->map_off + i * (uint32_t) sizeof(copro_region);

		if (ctl_word(bus, at + 4) == 0) {
			continue;
		}
		if (COPRO_REGION_KIND(ctl_word(bus, at + 8)) == COPRO_REGION_STALL) {
			return 1;
		}
	}
	return 0;
}

/*
 * The address a core hands over carries CPU_MEM_PORT for a port access, which is
 * the one thing translated here: the decode above deals in a space and an
 * address, and a core deals in one number.
 */
static cpu_mem_result
hook_read(void *ctx, uint32_t addr, uint8_t *value)
{
	copro_bus *bus = (copro_bus *) ctx;
	const int is_port = (addr & CPU_MEM_PORT) != 0;

	switch (copro_bus_read(bus, addr & ~CPU_MEM_PORT, is_port, 0, value)) {
	case COPRO_BUS_OK:	return CPU_MEM_OK;
	case COPRO_BUS_STALL:	return CPU_MEM_STALL;
	default:		return CPU_MEM_BUSERROR;
	}
}

static cpu_mem_result
hook_write(void *ctx, uint32_t addr, uint8_t value)
{
	copro_bus *bus = (copro_bus *) ctx;
	const int is_port = (addr & CPU_MEM_PORT) != 0;

	switch (copro_bus_write(bus, addr & ~CPU_MEM_PORT, is_port, 0, value)) {
	case COPRO_BUS_OK:	return CPU_MEM_OK;
	case COPRO_BUS_STALL:	return CPU_MEM_STALL;
	default:		return CPU_MEM_BUSERROR;
	}
}

void
copro_bus_hook(copro_bus *bus, cpu_mem_hook *hook)
{
	hook->ctx = bus;
	hook->read = hook_read;
	hook->write = hook_write;
	hook->can_stall = copro_bus_can_stall(bus);
}
