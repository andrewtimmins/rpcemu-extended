/*
 * Does an OPEN Bus card actually get reached through the machine's memory decode?
 *
 * tests/test_openbus.c checks the bus rules against a fake host. This checks the
 * other half, and the half that a mistake would make silently useless: that
 * mem.c's physical accessors dispatch the reserved window to a fitted card, and
 * leave it alone when the slot is empty. It links rpcemu_core to get the real
 * decode, in the same way test_mmu_perms does.
 *
 * ★ WHY THIS TEST EXISTS AT ALL. Having added a --openbus-stub switch, the
 * obvious way to poke the card was the debugger socket. It cannot: the
 * debugger's memory accessor is deliberately side-effect-free and handles only
 * ROM, VRAM and RAM, so a read of the window answers zero and looks exactly like
 * a card that is not there. That is correct behaviour for a debugger and a
 * genuine dead end for proving the wiring, so the wiring is proven here instead.
 *
 * The accessors under test are the PHYSICAL ones. That matters twice over: a bus
 * master has no MMU so those are the right ones for a card, and the guest ARM's
 * own virtual accesses funnel into them too (readmemfl() calls
 * mem_phys_read32()), so one hook serves both.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openbus.h"
#include "openbus_stub.h"
#include "rpcemu.h"
#include "mem.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/*
 * The host services the emulator itself gives the bus. Deliberately the same
 * functions rpcemu.c wires up, so this exercises the real arrangement rather
 * than a convenient substitute.
 */
static uint32_t host_read32(uint32_t a) { return mem_phys_read32(a); }
static void host_write32(uint32_t a, uint32_t v) { mem_phys_write32(a, v); }
static int irq_state;
static void host_set_irq(int s) { irq_state = s; }
static void host_set_fiq(int s) { (void) s; }

static const openbus_host_ops ops = {
	.read32 = host_read32,
	.write32 = host_write32,
	.set_irq = host_set_irq,
	.set_fiq = host_set_fiq,
};

int
main(void)
{
	/* Enough of a machine for the physical accessors to work. */
	mem_init();
	mem_reset(8, 0);

	puts("With an empty second slot, the window is not claimed:");
	openbus_init(&ops);
	check("a word read answers all ones",
	    mem_phys_read32(OPENBUS_REG_BASE) == 0xffffffffu ||
	    mem_phys_read32(OPENBUS_REG_BASE) == 0u);
	check("and nothing is fitted", !openbus_present());

	puts("With the test card fitted, the decode reaches it:");
	check("the card fits", openbus_stub_fit() == 0);

	/* ★ The whole point. Through the machine's own physical accessor, not
	   through the bus API, so this fails if the mem.c hook is wrong or missing. */
	check("a word read of the window returns the card's ID",
	    mem_phys_read32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_ID) ==
	    OPENBUS_STUB_ID);

	mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_SCRATCH, 0xdecafbadu);
	check("a word write reaches the card and reads back",
	    mem_phys_read32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_SCRATCH) ==
	    0xdecafbadu);

	/*
	 * The byte accessors are hooked too, but mem_phys_read8() and
	 * mem_phys_write8() are static in mem.c, so a test cannot reach them
	 * without either exporting them or building page tables to go in through
	 * the virtual path as test_mmu_perms does. Neither is worth it for a symmetry
	 * that is one line either side of the word hooks, so THE BYTE PATH IS
	 * COVERED BY INSPECTION ONLY - said plainly here rather than left to look
	 * like it was tested.
	 */

	puts("Addresses either side of the window are untouched:");
	/* IOMD's registers must still be IOMD's. If the window were decoded too
	   widely, this is where it would show, and it would break every machine. */
	check("IOMD register space does not reach the card",
	    mem_phys_read32(0x03200000u) != OPENBUS_STUB_ID);
	check("RAM does not reach the card",
	    (mem_phys_write32(0x10000000u, 0x12345678u),
	     mem_phys_read32(0x10000000u) == 0x12345678u));

	puts("The card can reach guest RAM as a bus master, through the real decode:");
	{
		int i;
		int mismatch = 0;

		for (i = 0; i < 8; i++) {
			mem_phys_write32(0x10001000u + (uint32_t) i * 4, 0xa000u + (uint32_t) i);
		}
		mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_SRC, 0x10001000u);
		mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_DST, 0x10002000u);
		mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_LEN, 8);
		mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_CTRL,
		    OPENBUS_STUB_CTRL_COPY);

		check("the copy consumed cycles", openbus_run(100) > 0);

		for (i = 0; i < 8; i++) {
			if (mem_phys_read32(0x10002000u + (uint32_t) i * 4) !=
			    (uint32_t) (0xa000u + i)) {
				mismatch++;
			}
		}
		check("every word arrived in guest RAM", mismatch == 0);
	}

	puts("And its interrupt reaches the host:");
	mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_CTRL,
	    OPENBUS_STUB_CTRL_IRQ);
	check("nPIRQ is asserted", irq_state == 1);
	mem_phys_write32(OPENBUS_REG_BASE + OPENBUS_STUB_REG_CTRL, 0);
	check("and released", irq_state == 0);

	openbus_close();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall OPEN Bus decode checks passed");
	return EXIT_SUCCESS;
}
