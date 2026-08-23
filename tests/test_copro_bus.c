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
 * The co-processor card's address decode (src/copro/copro_bus.c).
 *
 * Links nothing: no card, no bus, no emulator, no display. The decode is a
 * function of a region table and two byte arrays, which is the whole reason it
 * was written as its own unit rather than inside the card.
 *
 * The cases here are the ones a machine actually needs, named after the machine
 * that needs them: a ROM that ignores writes, a screen whose writes must be
 * seen in order and with their timing, a keyboard the guest presents through a
 * latch, a register whose read has a side effect and so has to stop the core,
 * and an address that is not there at all - which is not an edge case, because
 * the Mac ROM sizes memory by touching one deliberately.
 */

#include "copro_bus.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __func__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* Larger than a 6502's address space on purpose: banking pages a bigger card
   RAM through a window, so the tests need somewhere for the far banks to be. */
#define RAM_SIZE	(256u * 1024u)
#define CTL_SIZE	(16u * 1024u)

static uint8_t ram[RAM_SIZE];
static uint8_t ctl[CTL_SIZE];

/* Where the test puts things in the control area. Arbitrary, which is the
   point: the guest chooses, so nothing may assume offset zero. */
#define MAP_OFF		0x40u
#define LOG_OFF		0x400u
#define LATCH_OFF	0x20u

static void
put_word(uint32_t offset, uint32_t value)
{
	ctl[offset]     = (uint8_t) value;
	ctl[offset + 1] = (uint8_t) (value >> 8);
	ctl[offset + 2] = (uint8_t) (value >> 16);
	ctl[offset + 3] = (uint8_t) (value >> 24);
}

static void
put_region(unsigned index, uint32_t base, uint32_t size, uint32_t kind,
           uint32_t latch)
{
	const uint32_t at = MAP_OFF + index * 16u;

	put_word(at, base);
	put_word(at + 4, size);
	put_word(at + 8, kind);
	put_word(at + 12, latch);
}

static void
fresh(copro_bus *bus)
{
	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	copro_bus_init(bus, ram, RAM_SIZE, ctl, CTL_SIZE);
}

/* With no map at all, a core sees exactly what it saw before any of this
   existed: its RAM as a flat array. */
static void
test_no_map_is_plain_ram(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);

	CHECK(copro_bus_write(&bus, 0x1234, 0, 0, 0x5a) == COPRO_BUS_OK, "write failed");
	CHECK(copro_bus_read(&bus, 0x1234, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0x5a, "read back %02x, wanted 5a", v);
	CHECK(ram[0x1234] == 0x5a, "did not reach RAM");
	CHECK(copro_bus_log_pending(&bus) == 0, "logged with no log set");
}

/* A Spectrum's bottom 16K, or a BBC's sideways ROM: writes must not stick. */
static void
test_rom_ignores_writes(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	ram[0x0100] = 0xc9;
	put_region(0, 0x0000, 0x4000, COPRO_REGION_ROM, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_write(&bus, 0x0100, 0, 0, 0x00) == COPRO_BUS_OK, "write failed");
	CHECK(copro_bus_read(&bus, 0x0100, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0xc9, "ROM changed: %02x", v);
	CHECK(ram[0x0100] == 0xc9, "ROM changed in RAM");

	/* Outside the region it is ordinary RAM again. */
	CHECK(copro_bus_write(&bus, 0x8000, 0, 0, 0x11) == COPRO_BUS_OK, "write failed");
	CHECK(ram[0x8000] == 0x11, "RAM outside the ROM did not take a write");
}

/*
 * A screen: the write reaches memory AND is logged, with its cycle, in order.
 * The cycle is the part that matters - without it the guest knows the border
 * changed but not on which scanline.
 */
static void
test_log_records_writes_in_order(void)
{
	copro_bus bus;
	copro_log_entry e;
	uint8_t v = 0;

	fresh(&bus);
	put_region(0, 0x4000, 0x1b00, COPRO_REGION_LOG, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 8), "log rejected");

	CHECK(copro_bus_write(&bus, 0x4000, 0, 100, 0xaa) == COPRO_BUS_OK, "write failed");
	CHECK(copro_bus_write(&bus, 0x4001, 0, 214, 0xbb) == COPRO_BUS_OK, "write failed");

	CHECK(copro_bus_read(&bus, 0x4000, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0xaa, "a logged write must still reach memory, got %02x", v);

	CHECK(copro_bus_log_pending(&bus) == 2, "pending %u, wanted 2",
	      copro_bus_log_pending(&bus));

	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing to pop");
	CHECK(e.cycle == 100 && e.addr == 0x4000 && e.value == 0xaa,
	      "first entry wrong: cycle %u addr %x value %x", e.cycle, e.addr, e.value);
	CHECK((e.info & COPRO_LOG_INFO_PORT) == 0, "memory write marked as a port");

	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing to pop");
	CHECK(e.cycle == 214 && e.addr == 0x4001 && e.value == 0xbb,
	      "second entry wrong");

	CHECK(copro_bus_log_pop(&bus, &e) == 0, "popped an entry that was not there");
	CHECK(copro_bus_log_pending(&bus) == 0, "still pending after draining");
}

/* A keyboard: the guest writes the matrix into the latch page and the core
   reads it, without either side waiting for the other. */
static void
test_latch_reads_come_from_the_guest(void)
{
	copro_bus bus;
	copro_log_entry e;
	uint8_t v = 0;

	fresh(&bus);
	/* Eight addresses of port space, as a Spectrum's ULA port decodes. */
	put_region(0, 0x00fe, 0x0008, COPRO_REGION_LATCH | COPRO_REGION_PORT,
	           LATCH_OFF);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 8), "log rejected");

	ctl[LATCH_OFF] = 0xbf;		/* what the guest presents at 0xfe */
	ctl[LATCH_OFF + 2] = 0x7f;	/* and at 0x100 */

	CHECK(copro_bus_read(&bus, 0x00fe, 1, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0xbf, "latch read gave %02x, wanted bf", v);
	CHECK(copro_bus_read(&bus, 0x0100, 1, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0x7f, "latch is one byte per address: got %02x, wanted 7f", v);

	/* The same address in the MEMORY space is not this region at all. */
	CHECK(copro_bus_read(&bus, 0x00fe, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0x00, "a port region answered a memory read");

	/* A write to a latch is logged and must NOT overwrite what the guest
	   presented, or the guest's value is lost. */
	CHECK(copro_bus_write(&bus, 0x00fe, 1, 7, 0x02) == COPRO_BUS_OK, "write failed");
	CHECK(ctl[LATCH_OFF] == 0xbf, "the core overwrote the guest's latch");
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "the write was not logged");
	CHECK(e.addr == 0x00fe && e.value == 0x02 && e.cycle == 7, "wrong entry");
	CHECK((e.info & COPRO_LOG_INFO_PORT) != 0, "port write not marked as one");
}

/* A CIA or a VIA: reading it has a side effect, so only the guest can answer. */
static void
test_stall_hands_the_access_to_the_guest(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	put_region(0, 0xdc00, 0x0010, COPRO_REGION_STALL, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_read(&bus, 0xdc0d, 0, 0, &v) == COPRO_BUS_STALL, "did not stall");
	CHECK(bus.stalled == 1, "stall not recorded");
	CHECK(bus.wait_addr == 0xdc0d, "wrong address reported: %x", bus.wait_addr);
	CHECK(bus.wait_is_write == 0, "a read reported as a write");
	CHECK(copro_bus_resume(&bus, 0x81) == 0x81, "resume did not return the answer");
	CHECK(bus.stalled == 0, "still stalled after resuming");

	CHECK(copro_bus_write(&bus, 0xdc04, 0, 0, 0x40) == COPRO_BUS_STALL,
	      "a stalled write did not stall");
	CHECK(bus.wait_is_write == 1, "a write reported as a read");
	CHECK(bus.wait_value == 0x40, "the written value was not reported");
	/* Resuming a write keeps the written value, whatever is passed. */
	CHECK(copro_bus_resume(&bus, 0x00) == 0x40, "resume changed a written value");
	CHECK(ram[0xdc04] == 0x00, "a stalled write must not reach RAM by itself");
}

/* The Mac ROM sizes memory by reading an address that is not there and
   expecting the bus error. */
static void
test_unmapped_is_a_bus_error(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	put_region(0, 0x600000, 0x100000, COPRO_REGION_UNMAPPED, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_read(&bus, 0x600000, 0, 0, &v) == COPRO_BUS_BUSERROR,
	      "no bus error on an unmapped read");
	CHECK(v == 0xff, "an unmapped read gave %02x, wanted ff", v);
	CHECK(copro_bus_write(&bus, 0x6fffff, 0, 0, 0x01) == COPRO_BUS_BUSERROR,
	      "no bus error on an unmapped write");
}

/*
 * A full ring drops the newest write and says so on the next entry the guest
 * reads. Overwriting the oldest instead would silently renumber everything the
 * guest had not read, and a display rebuilt from an ordered list would be wrong
 * with nothing to indicate it.
 */
static void
test_full_log_drops_and_reports(void)
{
	copro_bus bus;
	copro_log_entry e;
	unsigned i;
	int saw_dropped = 0;

	fresh(&bus);
	put_region(0, 0x0000, 0x10000, COPRO_REGION_LOG, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 4), "log rejected");

	/* Four entries in a four-entry ring: one slot is the gap that tells full
	   from empty, so three fit and the fourth is dropped. */
	for (i = 0; i < 4; i++) {
		copro_bus_write(&bus, 0x100 + i, 0, i, (uint8_t) i);
	}
	CHECK(copro_bus_log_pending(&bus) == 3, "pending %u, wanted 3",
	      copro_bus_log_pending(&bus));

	/* Drain, then write again: the next entry carries the dropped marker. */
	while (copro_bus_log_pop(&bus, &e)) {
		CHECK(e.value < 3, "an entry from after the drop survived: %u", e.value);
	}
	copro_bus_write(&bus, 0x200, 0, 99, 0x77);
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing after the drop");
	CHECK(e.value == 0x77, "wrong entry after the drop");
	saw_dropped = (e.info & COPRO_LOG_INFO_DROPPED) != 0;
	CHECK(saw_dropped, "the guest was not told writes had been lost");

	/* And the marker is not sticky. */
	copro_bus_write(&bus, 0x201, 0, 100, 0x78);
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing on the next write");
	CHECK((e.info & COPRO_LOG_INFO_DROPPED) == 0, "dropped marker stuck");
}

/* The ring wraps, rather than running off the end of the control area. */
static void
test_log_wraps(void)
{
	copro_bus bus;
	copro_log_entry e;
	unsigned i;

	fresh(&bus);
	put_region(0, 0x0000, 0x10000, COPRO_REGION_LOG, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 4), "log rejected");

	/* Round the ring twice, draining as we go. */
	for (i = 0; i < 12; i++) {
		copro_bus_write(&bus, 0x300 + i, 0, i, (uint8_t) i);
		CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing to pop at %u", i);
		CHECK(e.value == (uint8_t) i && e.cycle == i,
		      "entry %u came back as value %u cycle %u", i, e.value, e.cycle);
	}
	CHECK(copro_bus_log_pending(&bus) == 0, "left something pending");
}

/*
 * Banking, which is the only way an 8-bit core reaches more than 64K: a window
 * at &8000 onto a larger card RAM, switched by rewriting one field.
 *
 * This is a Spectrum 128's paged RAM, or a BBC's sideways ROMs, and it is the
 * reason a region may carry an offset at all.
 */
static void
test_banking_windows_onto_card_ram(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);

	/* Two 16K banks living at 0x10000 and 0x14000 of card RAM, well above
	   anything the core can address directly. */
	ram[0x10000] = 0xa1;
	ram[0x14000] = 0xb2;

	put_region(0, 0x8000, 0x4000, COPRO_REGION_RAM | COPRO_REGION_OFFSET, 0x10000);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_read(&bus, 0x8000, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0xa1, "bank 0 read %02x, wanted a1", v);

	/* Switching bank is one word in the region table. */
	put_word(MAP_OFF + 12, 0x14000);
	CHECK(copro_bus_read(&bus, 0x8000, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0xb2, "after paging, read %02x, wanted b2", v);

	/* A write goes to the bank now in the window, and not to the address the
	   core used - which would be somebody else's memory. */
	CHECK(copro_bus_write(&bus, 0x8001, 0, 0, 0x77) == COPRO_BUS_OK, "write failed");
	CHECK(ram[0x14001] == 0x77, "the write did not reach the paged bank");
	CHECK(ram[0x8001] == 0x00, "the write also landed at the core's address");

	/* And the offset applies across the whole window, not just at its base. */
	ram[0x14000 + 0x3fff] = 0x5c;
	CHECK(copro_bus_read(&bus, 0xbfff, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0x5c, "the top of the window read %02x, wanted 5c", v);
}

/* Without the flag a region still lies at its own address, which is what a
   machine with no paging wants and what every map written before banking says. */
static void
test_without_the_offset_flag_nothing_moves(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	ram[0x8000] = 0x33;
	/* A latch offset is set, and must be ignored without the flag. */
	put_region(0, 0x8000, 0x4000, COPRO_REGION_RAM, 0x10000);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_read(&bus, 0x8000, 0, 0, &v) == COPRO_BUS_OK, "read failed");
	CHECK(v == 0x33, "read %02x: the offset was applied without the flag", v);
}

/* A logged write in a banked region is logged at the address the PROGRAM used.
   The guest describes its machine in the core's addresses; telling it where the
   byte landed in card RAM would make it undo the banking to understand it. */
static void
test_banked_writes_log_the_core_address(void)
{
	copro_bus bus;
	copro_log_entry e;

	fresh(&bus);
	put_region(0, 0x4000, 0x4000, COPRO_REGION_LOG | COPRO_REGION_OFFSET, 0x20000);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 8), "log rejected");

	CHECK(copro_bus_write(&bus, 0x4010, 0, 42, 0x9e) == COPRO_BUS_OK, "write failed");
	CHECK(ram[0x20010] == 0x9e, "the write did not reach the banked location");
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "not logged");
	CHECK(e.addr == 0x4010, "logged %x, wanted the core's 4010", e.addr);
}

/* A table or ring that does not fit is refused, and refusing leaves the
   previous one alone rather than half-applied. */
static void
test_bad_offsets_are_refused(void)
{
	copro_bus bus;

	fresh(&bus);
	put_region(0, 0, 0x100, COPRO_REGION_RAM, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "a good map was refused");

	CHECK(copro_bus_set_map(&bus, CTL_SIZE - 8, 1) == 0,
	      "a table running off the end was accepted");
	CHECK(bus.map_count == 1 && bus.map_off == MAP_OFF,
	      "a refused map changed the one in use");

	/* A count large enough to overflow the size multiply must not pass. */
	CHECK(copro_bus_set_map(&bus, 0, 0x20000000u) == 0,
	      "an overflowing count was accepted");
	CHECK(copro_bus_set_log(&bus, CTL_SIZE - 8, 4) == 0,
	      "a ring running off the end was accepted");
	CHECK(copro_bus_set_log(&bus, 0, 0x20000000u) == 0,
	      "an overflowing entry count was accepted");
}

/*
 * Later entries do not shadow earlier ones, and an entry with a size of zero
 * matches nothing. There is no code in the decode for that second case: the
 * range test cannot match a zero-size region, which a mutation proved by
 * showing that removing the explicit check changed nothing.
 */
static void
test_first_match_wins_and_zero_size_is_disabled(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	put_region(0, 0x0000, 0x0000, COPRO_REGION_UNMAPPED, 0);	/* disabled */
	put_region(1, 0x2000, 0x1000, COPRO_REGION_ROM, 0);
	put_region(2, 0x2000, 0x1000, COPRO_REGION_RAM, 0);	/* shadowed */
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 3), "map rejected");

	ram[0x2000] = 0x42;
	CHECK(copro_bus_write(&bus, 0x2000, 0, 0, 0x99) == COPRO_BUS_OK, "write failed");
	CHECK(ram[0x2000] == 0x42, "the shadowed RAM entry took the write");

	/* The disabled entry covered address 0, so it must read as plain RAM. */
	ram[0] = 0x7e;
	CHECK(copro_bus_read(&bus, 0, 0, 0, &v) == COPRO_BUS_OK,
	      "a zero-size entry was not skipped");
	CHECK(v == 0x7e, "read %02x through a disabled entry", v);
}

/* A region reaching the top of the address space must not wrap and stop
   matching, which is what base + size would do. */
static void
test_region_at_the_top_of_the_space(void)
{
	copro_bus bus;
	uint8_t v = 0;

	fresh(&bus);
	put_region(0, 0xfffffff0u, 0x10u, COPRO_REGION_UNMAPPED, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");

	CHECK(copro_bus_read(&bus, 0xffffffffu, 0, 0, &v) == COPRO_BUS_BUSERROR,
	      "the last address in the space did not match its region");
}

/*
 * A logged write is stamped with the cycle the core was at.
 *
 * The stamp is the point of the log: it is what lets a guest know where on the
 * screen the card was when the write landed. Every entry said zero, because
 * hook_write() - the only path a running core takes - passed a literal 0, and no
 * test went through that path. The tests above call copro_bus_write() directly
 * with hand-supplied cycles, so they were asserting on numbers they had just
 * provided themselves; this one goes through the hook, which is where the value
 * actually has to come from.
 */
static uint32_t fake_cycles;

static uint32_t
fake_cycle_source(void *ctx)
{
	CHECK(ctx == (void *) 0x1234, "the context was not handed back");
	return fake_cycles;
}

static void
test_the_hook_stamps_the_cycle(void)
{
	copro_bus bus;
	cpu_mem_hook hook;
	copro_log_entry e;

	fresh(&bus);
	put_region(0, 0x4000, 0x100, COPRO_REGION_LOG, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 8), "log rejected");
	copro_bus_hook(&bus, &hook);

	/* No source: zero, which is what the whole log used to report. */
	CHECK(hook.write(hook.ctx, 0x4000, 0x11) == CPU_MEM_OK, "write failed");
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing logged");
	CHECK(e.cycle == 0, "unstamped write said cycle %u", e.cycle);

	copro_bus_set_cycle_source(&bus, fake_cycle_source, (void *) 0x1234);

	fake_cycles = 4242;
	CHECK(hook.write(hook.ctx, 0x4001, 0x22) == CPU_MEM_OK, "write failed");
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing logged");
	CHECK(e.cycle == 4242, "stamped %u, wanted 4242", e.cycle);
	CHECK(e.addr == 0x4001 && e.value == 0x22, "logged %x = %x", e.addr, e.value);

	/* And it is read per write, not once: a second write at a later cycle must
	   carry the later number. */
	fake_cycles = 9001;
	CHECK(hook.write(hook.ctx, 0x4002, 0x33) == CPU_MEM_OK, "write failed");
	CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing logged");
	CHECK(e.cycle == 9001, "stamped %u, wanted 9001", e.cycle);
}

int
main(void)
{
	test_no_map_is_plain_ram();
	test_rom_ignores_writes();
	test_log_records_writes_in_order();
	test_latch_reads_come_from_the_guest();
	test_stall_hands_the_access_to_the_guest();
	test_unmapped_is_a_bus_error();
	test_full_log_drops_and_reports();
	test_log_wraps();
	test_banking_windows_onto_card_ram();
	test_without_the_offset_flag_nothing_moves();
	test_banked_writes_log_the_core_address();
	test_bad_offsets_are_refused();
	test_first_match_wins_and_zero_size_is_disabled();
	test_region_at_the_top_of_the_space();
	test_the_hook_stamps_the_cycle();

	if (failures != 0) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("copro_bus: all checks pass\n");
	return 0;
}
