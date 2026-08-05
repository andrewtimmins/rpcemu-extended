/*
 * The Risc PC second processor bus, and the stub card that proves it.
 *
 * WHY THIS CAN BE TESTED AT ALL. OPEN Bus is where a second CPU would live, and
 * a second CPU is the least testable thing imaginable - it needs a card, guest
 * software, and on a real machine it needed an ASIC. So the bus was written to
 * take its host services through a struct of function pointers, and this
 * substitutes a fake memory and a fake interrupt controller for them. Everything
 * about an OPEN Bus card EXCEPT the processor can then be checked on any
 * platform, with no emulator and no machine: the register window, bus mastering,
 * the interrupt line, reset, and the timeslice.
 *
 * That is the whole argument for building the bus before choosing a card. When
 * an x86 or another CPU eventually arrives, the plumbing underneath it is
 * already known to work, and a fault can be attributed to the processor rather
 * than to the six other things that could have been wrong.
 *
 * The rules being pinned down here are the ones that would be expensive to get
 * wrong later:
 *   - an empty slot reads as an undriven bus, not as a card saying no
 *   - a card is never silently replaced
 *   - a card removed while holding nPIRQ does not leave the host interrupted
 *   - cycles a card claims are clamped, so it cannot corrupt the ARM's budget
 *   - the interrupt line is only driven on a change
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openbus.h"
#include "openbus_stub.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* ---- a fake machine ---------------------------------------------------- */

#define FAKE_BASE	0x10000000u	/* where our pretend DRAM starts */
#define FAKE_WORDS	64

static uint32_t fake_ram[FAKE_WORDS];
static int irq_line, fiq_line;
static int irq_calls, fiq_calls;
static uint32_t last_bad_read;

static uint32_t
fake_read32(uint32_t phys)
{
	const uint32_t index = (phys - FAKE_BASE) >> 2;

	if (phys < FAKE_BASE || index >= FAKE_WORDS) {
		last_bad_read = phys;
		return 0xdeadbeefu;
	}
	return fake_ram[index];
}

static void
fake_write32(uint32_t phys, uint32_t val)
{
	const uint32_t index = (phys - FAKE_BASE) >> 2;

	if (phys < FAKE_BASE || index >= FAKE_WORDS) {
		return;
	}
	fake_ram[index] = val;
}

/* Counted as well as latched, so "only on a change" can be asserted. */
static void fake_set_irq(int state) { irq_line = state; irq_calls++; }
static void fake_set_fiq(int state) { fiq_line = state; fiq_calls++; }

static const openbus_host_ops fake_ops = {
	.read32 = fake_read32,
	.write32 = fake_write32,
	.set_irq = fake_set_irq,
	.set_fiq = fake_set_fiq,
};

static void
fresh(void)
{
	memset(fake_ram, 0, sizeof(fake_ram));
	irq_line = fiq_line = 0;
	irq_calls = fiq_calls = 0;
	last_bad_read = 0;
	openbus_close();
	openbus_init(&fake_ops);
}

static uint32_t
rd(uint32_t offset)
{
	return openbus_reg_read(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_32);
}

static void
wr(uint32_t offset, uint32_t val)
{
	openbus_reg_write(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_32, val);
}

/* ---- the window ------------------------------------------------------- */

/*
 * The TRM reserves 0x03600000-0x037FFFFF for second bus master registers, and
 * says nothing about what goes in it. Getting the bounds wrong would either
 * shadow real I/O below or leave part of the window dead, and neither would be
 * obvious from a card that mostly worked.
 */
static void
test_window(void)
{
	puts("The register window is exactly where the TRM reserves it:");

	check("0x03600000 is the first address in the window",
	    openbus_addr_in_window(0x03600000u));
	check("0x037fffff is the last",
	    openbus_addr_in_window(0x037fffffu));
	check("0x035fffff is below it",
	    !openbus_addr_in_window(0x035fffffu));
	check("0x03800000 is above it",
	    !openbus_addr_in_window(0x03800000u));
	/* 0x03400000-0x035FFFFF is the external PROG space that drives Nprog, and
	   is emphatically not ours. */
	check("the external PROG space is not ours",
	    !openbus_addr_in_window(0x03400000u));
	check("IOMD's own registers are not ours",
	    !openbus_addr_in_window(0x03200000u));
	check("the window is 2MB", OPENBUS_REG_SIZE == 0x00200000u);
}

/*
 * An empty slot. Reading all ones matters: a machine with nothing in the slot
 * has nothing driving those lines, and answering zero would look to a driver
 * like a card that is present and reporting no capabilities.
 */
static void
test_empty_slot(void)
{
	puts("An empty slot behaves like an undriven bus:");
	fresh();

	check("no card is fitted", !openbus_present());
	check("and it has no name", openbus_name() == NULL);
	check("reads answer all ones", rd(OPENBUS_STUB_REG_ID) == 0xffffffffu);
	check("writes are harmless", (wr(OPENBUS_STUB_REG_SCRATCH, 0x1234), 1));
	check("running costs nothing", openbus_run(1000) == 0);
	check("reset is harmless", (openbus_reset(), 1));
	check("removing nothing is harmless", (openbus_remove(), 1));
	check("no interrupt was ever raised", irq_calls == 0 && fiq_calls == 0);
}

static void
test_fitting(void)
{
	puts("Fitting and removing a card:")	;
	fresh();

	check("the stub fits", openbus_stub_fit() == 0);
	check("and is reported present", openbus_present());
	check("and names itself", openbus_name() != NULL &&
	    strstr(openbus_name(), "stub") != NULL);

	/* Two cards in one slot is a configuration mistake. Replacing silently
	   would hide it, and the second card would appear to work. */
	check("a second card is REFUSED, not swapped in",
	    openbus_stub_fit() != 0);
	check("and the first is still there", openbus_present());

	openbus_remove();
	check("removal takes it away", !openbus_present());
	check("and reads go back to all ones", rd(OPENBUS_STUB_REG_ID) == 0xffffffffu);

	/* Refitting after removal must work, or a machine reset that rebuilds the
	   slot would only ever succeed once. */
	check("it can be fitted again", openbus_stub_fit() == 0);

	/* Nothing can be fitted before the bus is started. */
	openbus_close();
	check("nothing fits before the bus is started", openbus_stub_fit() != 0);
}

/* ---- the card's registers --------------------------------------------- */

static void
test_registers(void)
{
	puts("The register window reaches the card:");
	fresh();
	openbus_stub_fit();

	check("the card identifies itself", rd(OPENBUS_STUB_REG_ID) == OPENBUS_STUB_ID);

	wr(OPENBUS_STUB_REG_SCRATCH, 0xa5a55a5au);
	check("a scratch register round-trips",
	    rd(OPENBUS_STUB_REG_SCRATCH) == 0xa5a55a5au);

	wr(OPENBUS_STUB_REG_SRC, FAKE_BASE);
	wr(OPENBUS_STUB_REG_DST, FAKE_BASE + 0x80);
	wr(OPENBUS_STUB_REG_LEN, 4);
	check("addresses and length round-trip",
	    rd(OPENBUS_STUB_REG_SRC) == FAKE_BASE &&
	    rd(OPENBUS_STUB_REG_DST) == FAKE_BASE + 0x80 &&
	    rd(OPENBUS_STUB_REG_LEN) == 4);

	check("an unimplemented offset reads as undriven", rd(0x400) == 0xffffffffu);
	/* Writing a read-only register must not corrupt anything nearby. */
	wr(OPENBUS_STUB_REG_ID, 0);
	check("the ID is read-only", rd(OPENBUS_STUB_REG_ID) == OPENBUS_STUB_ID);

	/* Offsets are word-based; a narrow access lands in the same register rather
	   than off the end of the map. */
	check("an unaligned offset resolves to its word",
	    openbus_reg_read(OPENBUS_REG_BASE + OPENBUS_STUB_REG_ID + 2,
	        OPENBUS_SIZE_8) == OPENBUS_STUB_ID);
}

/* ---- bus mastering ---------------------------------------------------- */

/*
 * ★ THE ONE THAT MATTERS. A card reaching host physical memory on its own is
 * the hard half of any real OPEN Bus card - it is what Gemini's mapping
 * registers exist to arrange, and what a podule cannot do at all. If this works,
 * an x86 dropped in later inherits a path that is known good.
 */
static void
test_bus_mastering(void)
{
	int i;
	int used;

	puts("The card moves data through host memory as a bus master:");
	fresh();
	openbus_stub_fit();

	for (i = 0; i < 8; i++) {
		fake_ram[i] = 0x1000 + i;
	}

	wr(OPENBUS_STUB_REG_SRC, FAKE_BASE);
	wr(OPENBUS_STUB_REG_DST, FAKE_BASE + 0x80);	/* word 32 */
	wr(OPENBUS_STUB_REG_LEN, 8);
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_COPY);

	check("the copy is reported busy",
	    (rd(OPENBUS_STUB_REG_STATUS) & OPENBUS_STUB_STATUS_BUSY) != 0);

	used = openbus_run(100);
	check("the copy consumed cycles", used > 0);
	check("and finished", (rd(OPENBUS_STUB_REG_STATUS) &
	    OPENBUS_STUB_STATUS_DONE) != 0);
	check("and is no longer busy", (rd(OPENBUS_STUB_REG_STATUS) &
	    OPENBUS_STUB_STATUS_BUSY) == 0);
	check("it copied the right number of words",
	    rd(OPENBUS_STUB_REG_COPIED) == 8);

	{
		int mismatch = 0;

		for (i = 0; i < 8; i++) {
			if (fake_ram[32 + i] != (uint32_t) (0x1000 + i)) {
				mismatch++;
			}
		}
		check("every word arrived intact in host memory", mismatch == 0);
	}
	check("nothing was read outside the fake memory", last_bad_read == 0);

	/* A copy longer than one slice must resume, not restart or stall: a real
	   card holding the bus for a whole transfer would freeze the machine. */
	fresh();
	openbus_stub_fit();
	for (i = 0; i < 16; i++) {
		fake_ram[i] = 0x2000 + i;
	}
	wr(OPENBUS_STUB_REG_SRC, FAKE_BASE);
	wr(OPENBUS_STUB_REG_DST, FAKE_BASE + 0x80);
	wr(OPENBUS_STUB_REG_LEN, 16);
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_COPY);

	openbus_run(4);
	check("a long copy is only partly done after a short slice",
	    rd(OPENBUS_STUB_REG_COPIED) == 4 && rd(OPENBUS_STUB_REG_LEN) == 12);
	check("and is still busy",
	    (rd(OPENBUS_STUB_REG_STATUS) & OPENBUS_STUB_STATUS_BUSY) != 0);

	openbus_run(100);
	check("a later slice finishes it", rd(OPENBUS_STUB_REG_COPIED) == 16);
	{
		int mismatch = 0;

		for (i = 0; i < 16; i++) {
			if (fake_ram[32 + i] != (uint32_t) (0x2000 + i)) {
				mismatch++;
			}
		}
		check("and all of it arrived", mismatch == 0);
	}
}

/* ---- interrupts ------------------------------------------------------- */

static void
test_interrupts(void)
{
	int calls;

	puts("nPIRQ and the FIQ path:");
	fresh();
	openbus_stub_fit();

	check("no interrupt to begin with", !irq_line && !fiq_line);

	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_IRQ);
	check("the card raises nPIRQ", irq_line == 1);
	check("and says so in its status",
	    (rd(OPENBUS_STUB_REG_STATUS) & OPENBUS_STUB_STATUS_IRQ) != 0);
	check("without touching FIQ", fiq_line == 0);

	/* The host's aggregation walks every podule slot and recomputes the IOMD
	   status, so a card asserting what is already asserted must not reach it. */
	calls = irq_calls;
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_IRQ);
	check("asserting it again does not disturb the host", irq_calls == calls);

	wr(OPENBUS_STUB_REG_CTRL, 0);
	check("clearing it releases the line", irq_line == 0);

	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_FIQ);
	check("FIQ can be raised on its own", fiq_line == 1 && irq_line == 0);

	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_IRQ | OPENBUS_STUB_CTRL_FIQ);
	check("both together", irq_line == 1 && fiq_line == 1);

	/* ★ A card taken out while holding nPIRQ would leave the host with an
	   interrupt that nothing remains to clear. */
	openbus_remove();
	check("removing a card that holds nPIRQ releases it", irq_line == 0);
	check("and releases FIQ", fiq_line == 0);
}

static void
test_reset(void)
{
	puts("Reset:");
	fresh();
	openbus_stub_fit();

	wr(OPENBUS_STUB_REG_SCRATCH, 0xcafebabeu);
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_IRQ);
	check("state and interrupt are set", irq_line == 1 &&
	    rd(OPENBUS_STUB_REG_SCRATCH) == 0xcafebabeu);

	openbus_reset();
	check("reset clears the card's state", rd(OPENBUS_STUB_REG_SCRATCH) == 0);
	check("and drops nPIRQ", irq_line == 0);
	check("the card still answers afterwards",
	    rd(OPENBUS_STUB_REG_ID) == OPENBUS_STUB_ID);

	/* The card's own reset bit, which a driver would use without a machine
	   reset. Self-clearing, so it cannot stick on. */
	wr(OPENBUS_STUB_REG_SCRATCH, 0x55);
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_RESET);
	check("the card's reset bit works too", rd(OPENBUS_STUB_REG_SCRATCH) == 0);
	check("and does not stick",
	    (rd(OPENBUS_STUB_REG_CTRL) & OPENBUS_STUB_CTRL_RESET) == 0);
}

/* ---- the timeslice ---------------------------------------------------- */

/*
 * Cycles a card claims are charged against the ARM's own budget, which is how
 * nWAIT stalling the host is modelled. A card that could claim more than it was
 * offered, or a negative number, would corrupt the host's timing in a way that
 * would be very hard to trace back to the card.
 */
static void
test_timeslice_is_clamped(void)
{
	puts("A card cannot corrupt the host's cycle budget:");
	fresh();

	check("running with no card returns nothing", openbus_run(500) == 0);

	openbus_stub_fit();
	check("a zero budget is refused", openbus_run(0) == 0);
	check("a negative budget is refused", openbus_run(-5) == 0);

	/* An idle card wants nothing, which is the normal case. */
	check("an idle card claims nothing", openbus_run(100) == 0);
	check("but was still given the slice", rd(OPENBUS_STUB_REG_RUNS) > 0);

	/* A copy far longer than the slice must claim no more than it was offered. */
	wr(OPENBUS_STUB_REG_SRC, FAKE_BASE);
	wr(OPENBUS_STUB_REG_DST, FAKE_BASE + 0x80);
	wr(OPENBUS_STUB_REG_LEN, 1000000);
	wr(OPENBUS_STUB_REG_CTRL, OPENBUS_STUB_CTRL_COPY);
	{
		const int used = openbus_run(10);

		check("a long operation claims no more than it was offered", used <= 10);
		check("and claims something", used > 0);
	}
}

/* Host services that are absent must not crash a card that uses them. A card
   ported from elsewhere will call things we have not wired up. */
static void
test_missing_host_services(void)
{
	static const openbus_host_ops empty_ops = { NULL, NULL, NULL, NULL };

	puts("Missing host services degrade rather than crash:");

	openbus_close();
	openbus_init(&empty_ops);
	check("a card still fits", openbus_stub_fit() == 0);
	check("a master read answers all ones",
	    openbus_master_read32(FAKE_BASE) == 0xffffffffu);
	check("a master write is harmless",
	    (openbus_master_write32(FAKE_BASE, 1), 1));
	check("raising an interrupt is harmless",
	    (openbus_master_set_irq(1), 1));

	/* And a NULL ops table means the bus never started at all. */
	openbus_close();
	openbus_init(NULL);
	check("a NULL host does not start the bus", openbus_stub_fit() != 0);
	check("and reads answer all ones", rd(0) == 0xffffffffu);
}

/*
 * The request flag that --openbus-stub sets.
 *
 * Separate from fitting because the command line is parsed long before there is a
 * bus. Worth a check because it is also what gives the linker a reference to this
 * object: nothing else in the emulator calls into openbus_stub.c, so without it
 * the card is compiled into the static library and then left there.
 */
static void
test_request_flag(void)
{
	puts("The --openbus-stub request flag:");

	check("nothing is requested by default", !openbus_stub_requested());
	openbus_stub_request();
	check("asking is remembered", openbus_stub_requested());
	/* Asking twice is not an error: it is a command-line flag. */
	openbus_stub_request();
	check("asking twice is harmless", openbus_stub_requested());
}

int
main(void)
{
	/* Before anything fits a card, so the default really is the default. */
	test_request_flag();

	test_window();
	test_empty_slot();
	test_fitting();
	test_registers();
	test_bus_mastering();
	test_interrupts();
	test_reset();
	test_timeslice_is_clamped();
	test_missing_host_services();

	openbus_close();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall OPEN Bus checks passed");
	return EXIT_SUCCESS;
}
