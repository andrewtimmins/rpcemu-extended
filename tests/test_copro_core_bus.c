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
 * A core running against the address decode, which is where the two halves have
 * to agree and where a mistake in either shows up as the other one's fault.
 *
 * The cases that matter are the ones the decode cannot be tested for on its own:
 *
 *  - a real program's writes appearing in the log, in order;
 *  - a real program reading what the guest put in a latch;
 *  - an instruction that stalls being ABANDONED WHOLE and retried, with nothing
 *    half-done in between. That is the part most likely to be wrong, because a
 *    core cannot suspend mid-instruction and the retry has to leave no trace.
 *    See the note in cpu_mem.h.
 *
 * 6502 and Z80 both, because the Z80 reaches the same decode through a separate
 * port space and that is a different path through the core.
 */

#include "copro_bus.h"
#include "cpu_6502.h"
#include "cpu_z80.h"

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

#define RAM_SIZE	(64u * 1024u)
#define CTL_SIZE	(8u * 1024u)

#define MAP_OFF		0x100u
#define LOG_OFF		0x400u
#define LATCH_OFF	0x40u

static uint8_t ram[RAM_SIZE];
static uint8_t ctl[CTL_SIZE];

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
load(uint16_t at, const uint8_t *code, size_t len)
{
	memcpy(ram + at, code, len);
}

/*
 * A 6502 writing to a screen: three stores, which must reach memory and appear
 * in the log in the order made.
 *
 *   LDA #&aa : STA &4000 : LDA #&bb : STA &4001 : LDA #&cc : STA &4002 : BRK
 */
static void
test_6502_writes_are_logged_in_order(void)
{
	static const uint8_t code[] = {
		0xa9, 0xaa, 0x8d, 0x00, 0x40,
		0xa9, 0xbb, 0x8d, 0x01, 0x40,
		0xa9, 0xcc, 0x8d, 0x02, 0x40,
		0x00
	};
	copro_bus bus;
	cpu6502_state cpu;
	cpu_mem_hook hook;
	copro_log_entry e;
	int i;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x0200, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0x4000, 0x1b00, COPRO_REGION_LOG, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 16), "log rejected");
	copro_bus_hook(&bus, &hook);
	CHECK(hook.can_stall == 0, "a map with no stalling region said it can stall");

	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_set_mem_hook(&cpu, &hook);
	cpu6502_reset(&cpu, 0x0200);

	/* CYCLES, not instructions: three LDA #n at 2, three STA abs at 4, and a
	   BRK at 7. See tests/test_cpu_cycles.c for where those come from. */
	CHECK(cpu6502_run(&cpu, 100) == 3 * (2 + 4) + 7, "ran %llu cycles",
	      (unsigned long long) cpu.cycles);
	CHECK(cpu.halted, "did not reach BRK");

	CHECK(ram[0x4000] == 0xaa && ram[0x4001] == 0xbb && ram[0x4002] == 0xcc,
	      "the writes did not reach memory");
	CHECK(copro_bus_log_pending(&bus) == 3, "logged %u writes, wanted 3",
	      copro_bus_log_pending(&bus));

	for (i = 0; i < 3; i++) {
		CHECK(copro_bus_log_pop(&bus, &e) == 1, "nothing at %d", i);
		CHECK(e.addr == (uint32_t) (0x4000 + i), "entry %d address %x", i, e.addr);
		CHECK(e.value == (uint32_t) (0xaa + i * 0x11), "entry %d value %x",
		      i, e.value);
	}
}

/* A 6502 reading a keyboard the guest presents: LDA &d000 : STA &00 : BRK */
static void
test_6502_reads_a_latch(void)
{
	static const uint8_t code[] = { 0xad, 0x00, 0xd0, 0x85, 0x00, 0x00 };
	copro_bus bus;
	cpu6502_state cpu;
	cpu_mem_hook hook;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x0200, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0xd000, 0x0100, COPRO_REGION_LATCH, LATCH_OFF);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	ctl[LATCH_OFF] = 0x5e;

	copro_bus_hook(&bus, &hook);
	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_set_mem_hook(&cpu, &hook);
	cpu6502_reset(&cpu, 0x0200);

	/* LDA abs 4, STA zp 3, BRK 7. */
	CHECK(cpu6502_run(&cpu, 100) == 4 + 3 + 7, "wrong cycle count");
	CHECK(ram[0x0000] == 0x5e, "the core saw %02x, not the latch's 5e", ram[0x0000]);
}

/*
 * ★ The one that matters: a stalling read.
 *
 *   LDA #&11 : LDX #&22 : LDA &dc0d : STA &10 : BRK
 *
 * The LDA from &dc0d stalls. The instruction must be abandoned with the
 * accumulator still &11 and the program counter back at the LDA, and X - loaded
 * before the stall and untouched by the abandoned instruction - must survive.
 * Then the guest answers &81 and the retry completes.
 */
static void
test_6502_stall_abandons_and_retries(void)
{
	static const uint8_t code[] = {
		0xa9, 0x11,		/* LDA #&11   */
		0xa2, 0x22,		/* LDX #&22   */
		0xad, 0x0d, 0xdc,	/* LDA &dc0d  */
		0x85, 0x10,		/* STA &10    */
		0x00			/* BRK        */
	};
	copro_bus bus;
	cpu6502_state cpu;
	cpu_mem_hook hook;
	int ran;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x0200, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0xdc00, 0x0010, COPRO_REGION_STALL, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	copro_bus_hook(&bus, &hook);
	CHECK(hook.can_stall == 1, "a map with a stalling region said it cannot stall");

	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_set_mem_hook(&cpu, &hook);
	cpu6502_reset(&cpu, 0x0200);

	/* Runs the two immediates, then stops on the third instruction. */
	ran = cpu6502_run(&cpu, 100);
	/* Two immediates at two cycles each. */
	CHECK(ran == 4, "ran %d cycles before the stall, wanted 4", ran);
	CHECK(bus.stalled == 1, "the bus is not holding a stalled access");
	CHECK(bus.wait_addr == 0xdc0d, "stalled on %x", bus.wait_addr);
	CHECK(bus.wait_is_write == 0, "a read reported as a write");

	/* Nothing of the abandoned instruction may have happened. */
	CHECK(cpu.a == 0x11, "the accumulator changed to %02x", cpu.a);
	CHECK(cpu.x == 0x22, "X was lost: %02x", cpu.x);
	CHECK(cpu.pc == 0x0204, "the program counter is at %04x, wanted 0204", cpu.pc);
	CHECK(cpu.cycles == 4, "the abandoned instruction was counted: %llu",
	      (unsigned long long) cpu.cycles);
	CHECK(ram[0x0010] == 0x00, "the following store happened anyway");

	/* The guest answers, and the same instruction runs again. */
	CHECK(copro_bus_resume(&bus, 0x81) == 0x81, "resume gave the wrong value");
	cpu.stalled = 0;

	ran = cpu6502_run(&cpu, 100);
	/* The retried LDA abs 4, STA zp 3, BRK 7. */
	CHECK(ran == 4 + 3 + 7, "ran %d cycles after resuming", ran);
	CHECK(cpu.a == 0x81, "the retried read gave %02x, wanted 81", cpu.a);
	CHECK(ram[0x0010] == 0x81, "the store after it did not happen");
	CHECK(cpu.halted, "did not reach BRK");
	CHECK(bus.stalled == 0, "stalled again on the retry");
}

/*
 * Two reads of the same stalling address must stall twice and get two answers.
 *
 * A program polling a status register does exactly this, and holding on to the
 * guest's answer would give it the same value for ever while the guest was never
 * asked again. The answer is consumed by the access that retries, and nothing
 * after it.
 *
 *   LDA &dc0d : STA &10 : LDA &dc0d : STA &11 : BRK
 */
static void
test_6502_second_read_stalls_again(void)
{
	static const uint8_t code[] = {
		0xad, 0x0d, 0xdc,	/* LDA &dc0d */
		0x85, 0x10,		/* STA &10   */
		0xad, 0x0d, 0xdc,	/* LDA &dc0d */
		0x85, 0x11,		/* STA &11   */
		0x00			/* BRK       */
	};
	copro_bus bus;
	cpu6502_state cpu;
	cpu_mem_hook hook;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x0200, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0xdc00, 0x0010, COPRO_REGION_STALL, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	copro_bus_hook(&bus, &hook);

	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_set_mem_hook(&cpu, &hook);
	cpu6502_reset(&cpu, 0x0200);

	CHECK(cpu6502_run(&cpu, 100) == 0, "something ran before the first stall");
	CHECK(bus.stalled == 1, "did not stall");
	copro_bus_resume(&bus, 0x81);
	cpu.stalled = 0;

	/* The retried LDA abs 4 and the STA zp 3, then it stops on the second LDA. */
	CHECK(cpu6502_run(&cpu, 100) == 4 + 3, "wrong count between the two stalls");
	CHECK(ram[0x0010] == 0x81, "the first answer did not reach memory");
	CHECK(bus.stalled == 1, "the second read of the same address did not stall");

	copro_bus_resume(&bus, 0x82);
	cpu.stalled = 0;

	CHECK(cpu6502_run(&cpu, 100) == 4 + 3 + 7, "wrong count after the second stall");
	CHECK(ram[0x0011] == 0x82, "the second answer was %02x, wanted 82", ram[0x0011]);
	CHECK(cpu.halted, "did not reach BRK");
}

/* A stalling WRITE: the core must not stall a second time on the retry, or the
   program never gets past it. */
static void
test_6502_stalled_write_completes_on_retry(void)
{
	static const uint8_t code[] = {
		0xa9, 0x3f,		/* LDA #&3f  */
		0x8d, 0x00, 0xdc,	/* STA &dc00 */
		0xa9, 0x07,		/* LDA #&07  */
		0x00			/* BRK       */
	};
	copro_bus bus;
	cpu6502_state cpu;
	cpu_mem_hook hook;
	int ran;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x0200, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0xdc00, 0x0010, COPRO_REGION_STALL, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	copro_bus_hook(&bus, &hook);

	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_set_mem_hook(&cpu, &hook);
	cpu6502_reset(&cpu, 0x0200);

	ran = cpu6502_run(&cpu, 100);
	CHECK(ran == 2, "ran %d cycles before the stalled write, wanted 2", ran);
	CHECK(bus.stalled == 1 && bus.wait_is_write == 1, "no stalled write");
	CHECK(bus.wait_value == 0x3f, "the value written was %x", bus.wait_value);

	copro_bus_resume(&bus, 0);
	cpu.stalled = 0;

	ran = cpu6502_run(&cpu, 100);
	/* The retried STA abs 4, LDA #n 2, BRK 7. */
	CHECK(ran == 4 + 2 + 7, "ran %d cycles after resuming", ran);
	CHECK(cpu.halted, "did not reach BRK");
	CHECK(cpu.a == 0x07, "did not carry on past the write");
	CHECK(bus.stalled == 0, "the retried write stalled again");
}

/*
 * The Z80 reaches the same decode through its separate port space, which is a
 * different path through the core.
 *
 *   LD A,&00 : OUT (&fe),A : IN A,(&fe) : LD (&00),A : HALT
 */
static void
test_z80_ports_reach_the_decode(void)
{
	static const uint8_t code[] = {
		0x3e, 0x02,		/* LD A,&02      */
		0xd3, 0xfe,		/* OUT (&fe),A   */
		0xdb, 0xfe,		/* IN A,(&fe)    */
		0x32, 0x00, 0x00,	/* LD (&0000),A  */
		0x76			/* HALT          */
	};
	copro_bus bus;
	cpu_z80_state cpu;
	cpu_mem_hook hook;
	copro_log_entry e;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x8000, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0x00fe, 0x0001, COPRO_REGION_LATCH | COPRO_REGION_PORT,
	           LATCH_OFF);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	CHECK(copro_bus_set_log(&bus, LOG_OFF, 8), "log rejected");
	ctl[LATCH_OFF] = 0xbf;		/* what the guest presents at port &fe */

	copro_bus_hook(&bus, &hook);
	cpu_z80_init(&cpu, ram, RAM_SIZE);
	cpu_z80_set_mem_hook(&cpu, &hook);
	cpu_z80_reset(&cpu, 0x8000);

	/* LD A,n 7, OUT (n),A 11, IN A,(n) 11, LD (nn),A 13, HALT 4. */
	CHECK(cpu_z80_run(&cpu, 100) == 7 + 11 + 11 + 13 + 4, "wrong cycle count");
	CHECK(cpu.halted, "did not reach HALT");
	CHECK(ram[0x0000] == 0xbf, "the core read %02x from the port, wanted bf",
	      ram[0x0000]);

	CHECK(copro_bus_log_pop(&bus, &e) == 1, "the OUT was not logged");
	CHECK(e.addr == 0x00fe && e.value == 0x02, "logged %x = %x", e.addr, e.value);
	CHECK((e.info & COPRO_LOG_INFO_PORT) != 0, "not marked as a port write");
	CHECK(copro_bus_log_pending(&bus) == 0, "the memory write at &0000 was logged");
}

/* The Z80's registers must survive an abandoned instruction as the 6502's do,
   including the alternate set, which is the part a partial save would miss. */
static void
test_z80_stall_abandons_and_retries(void)
{
	static const uint8_t code[] = {
		0x3e, 0x11,		/* LD A,&11     */
		0x08,			/* EX AF,AF'    */
		0x3e, 0x22,		/* LD A,&22     */
		0x06, 0x33,		/* LD B,&33     */
		0x3a, 0x00, 0xd0,	/* LD A,(&d000) */
		0x76			/* HALT         */
	};
	copro_bus bus;
	cpu_z80_state cpu;
	cpu_mem_hook hook;
	int ran;

	memset(ram, 0, sizeof(ram));
	memset(ctl, 0, sizeof(ctl));
	load(0x8000, code, sizeof(code));

	copro_bus_init(&bus, ram, RAM_SIZE, ctl, CTL_SIZE);
	put_region(0, 0xd000, 0x0010, COPRO_REGION_STALL, 0);
	CHECK(copro_bus_set_map(&bus, MAP_OFF, 1), "map rejected");
	copro_bus_hook(&bus, &hook);

	cpu_z80_init(&cpu, ram, RAM_SIZE);
	cpu_z80_set_mem_hook(&cpu, &hook);
	cpu_z80_reset(&cpu, 0x8000);

	ran = cpu_z80_run(&cpu, 100);
	/* LD A,n 7, EX AF,AF' 4, LD A,n 7, LD B,n 7. */
	CHECK(ran == 7 + 4 + 7 + 7, "ran %d cycles before the stall", ran);
	CHECK(bus.stalled == 1, "no stalled access");
	CHECK(cpu.a == 0x22, "A changed: %02x", cpu.a);
	CHECK(cpu.a2 == 0x11, "the alternate accumulator was lost: %02x", cpu.a2);
	CHECK(cpu.b == 0x33, "B was lost: %02x", cpu.b);
	CHECK(cpu.pc == 0x8007, "pc is %04x, wanted 8007", cpu.pc);

	copro_bus_resume(&bus, 0x99);
	cpu.stalled = 0;

	ran = cpu_z80_run(&cpu, 100);
	/* The retried LD A,(nn) 13 and the HALT 4. */
	CHECK(ran == 13 + 4, "ran %d cycles after resuming", ran);
	CHECK(cpu.a == 0x99, "the retried read gave %02x", cpu.a);
	CHECK(cpu.halted, "did not reach HALT");
}

/* With no hook at all a core behaves exactly as it did before any of this, which
   is what keeps the cores usable on their own. */
static void
test_no_hook_is_unchanged(void)
{
	static const uint8_t code[] = { 0xa9, 0x7c, 0x8d, 0x34, 0x12, 0x00 };
	cpu6502_state cpu;

	memset(ram, 0, sizeof(ram));
	load(0x0200, code, sizeof(code));

	cpu6502_init(&cpu, ram, RAM_SIZE);
	cpu6502_reset(&cpu, 0x0200);

	/* LDA #n 2, STA abs 4, BRK 7. */
	CHECK(cpu6502_run(&cpu, 100) == 2 + 4 + 7, "wrong cycle count");
	CHECK(ram[0x1234] == 0x7c, "a plain store did not reach RAM");
	CHECK(cpu.halted, "did not reach BRK");
}

int
main(void)
{
	test_6502_writes_are_logged_in_order();
	test_6502_reads_a_latch();
	test_6502_stall_abandons_and_retries();
	test_6502_second_read_stalls_again();
	test_6502_stalled_write_completes_on_retry();
	test_z80_ports_reach_the_decode();
	test_z80_stall_abandons_and_retries();
	test_no_hook_is_unchanged();

	if (failures != 0) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("copro core+bus: all checks pass\n");
	return 0;
}
