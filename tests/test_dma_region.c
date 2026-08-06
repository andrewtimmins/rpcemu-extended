/* Physical-address decode for the DMA fetch paths.
 *
 * Sound DMA and the VIDC cursor read RAM by physical address, without going
 * through the MMU. Both used to decode the address themselves with a pair of bit
 * tests and then apply mem_rammask to whatever they found. Two things were wrong
 * with that, and both were silent:
 *
 *   - SIMM 1 is not shaped like SIMM 0. SIMM 0 is two banks of mem_rammask + 1
 *     bytes; SIMM 1 is a single 128MB block at 0x18000000-0x1fffffff, fitted
 *     only on a 256MB machine. Masking it with mem_rammask (0x3ffffff at 256MB)
 *     folded its upper 64MB onto its lower 64MB, so a sound buffer or pointer
 *     shape above physical 0x1c000000 read the wrong data.
 *
 *   - The Kinetic's on-card SDRAM banks at 0x20000000 and 0x30000000 were not
 *     recognised at all. Neither 0x08000000 nor 0x04000000 is set in those
 *     addresses, so they fell through to "SIMM 0 bank 0" and read from
 *     completely unrelated memory.
 *
 * mem_dma_region() is now the single decode both callers share. These checks are
 * about which region and which mask, so they write a marker word through the
 * ordinary physical write path and require the decode to find that exact word.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "mem.h"

static int failures;
static int checks;

static void
check_word(const char *what, uint32_t phys_addr, uint32_t expected)
{
	uint32_t mask = 0;
	const uint32_t *region;

	checks++;

	region = mem_dma_region(phys_addr, &mask);
	if (region == NULL) {
		printf("FAIL: %s: no region for 0x%08x\n", what, phys_addr);
		failures++;
		return;
	}

	if (region[(phys_addr & mask) >> 2] != expected) {
		printf("FAIL: %s: 0x%08x (mask 0x%08x) read 0x%08x, expected 0x%08x\n",
		       what, phys_addr, mask, region[(phys_addr & mask) >> 2], expected);
		failures++;
		return;
	}
}

static void
check_absent(const char *what, uint32_t phys_addr)
{
	uint32_t mask = 0;

	checks++;

	if (mem_dma_region(phys_addr, &mask) != NULL) {
		printf("FAIL: %s: 0x%08x resolved to a region, expected none\n",
		       what, phys_addr);
		failures++;
	}
}

/* Distinct marker per address, so a decode landing in the wrong place is caught
   rather than reading a plausible zero. */
static uint32_t
marker(uint32_t phys_addr)
{
	return 0xd0000000u | (phys_addr >> 4);
}

static void
put(uint32_t phys_addr)
{
	mem_phys_write32(phys_addr, marker(phys_addr));
}

static void
test_256mb(void)
{
	/* A 256MB machine: SIMM 0 is two 64MB banks, SIMM 1 is a 128MB block. */
	mem_reset(256, 2);

	put(0x10000040); /* SIMM 0 bank 0 */
	put(0x13ffff00); /* SIMM 0 bank 0, near the top */
	put(0x14000040); /* SIMM 0 bank 1 */
	put(0x18000040); /* SIMM 1, lower half */
	put(0x1c000040); /* SIMM 1, upper half - the one that used to alias */
	put(0x1fffff00); /* SIMM 1, near the top */

	check_word("SIMM 0 bank 0",        0x10000040, marker(0x10000040));
	check_word("SIMM 0 bank 0 high",   0x13ffff00, marker(0x13ffff00));
	check_word("SIMM 0 bank 1",        0x14000040, marker(0x14000040));
	check_word("SIMM 1 lower half",    0x18000040, marker(0x18000040));
	check_word("SIMM 1 upper half",    0x1c000040, marker(0x1c000040));
	check_word("SIMM 1 top",           0x1fffff00, marker(0x1fffff00));

	/* The specific defect: with mem_rammask applied to SIMM 1, these two
	   addresses resolved to the same word. */
	{
		uint32_t m0 = 0, m1 = 0;
		const uint32_t *r0 = mem_dma_region(0x18000040, &m0);
		const uint32_t *r1 = mem_dma_region(0x1c000040, &m1);

		checks++;
		if (r0 == NULL || r1 == NULL ||
		    &r0[(0x18000040 & m0) >> 2] == &r1[(0x1c000040 & m1) >> 2])
		{
			printf("FAIL: SIMM 1 halves alias to one address\n");
			failures++;
		}
	}

	/* Not RAM, so nothing should be fetched from them. */
	check_absent("ROM",             0x00000040);
	check_absent("VRAM",            0x02000040);
	check_absent("IO",              0x03000040);
	check_absent("SDRAM on RPCSA",  0x20000040);
}

static void
test_128mb(void)
{
	/* Below 256MB no second SIMM is fitted, so its window has no RAM. */
	mem_reset(128, 2);

	put(0x10000040);
	put(0x14000040);

	check_word("128MB SIMM 0 bank 0", 0x10000040, marker(0x10000040));
	check_word("128MB SIMM 0 bank 1", 0x14000040, marker(0x14000040));
	check_absent("128MB SIMM 1 unfitted", 0x18000040);
	check_absent("128MB SIMM 1 unfitted, upper", 0x1c000040);
}

static void
test_small(void)
{
	/* 8MB: each SIMM 0 bank is 4MB and mirrors through its 64MB window, so
	   the mask must be the bank size, not the window. */
	mem_reset(8, 2);

	put(0x10000040);
	check_word("8MB SIMM 0 bank 0", 0x10000040, marker(0x10000040));
	check_word("8MB bank 0 mirror", 0x10400040, marker(0x10000040));
}

static void
test_kinetic_sdram(void)
{
	/* A Kinetic's surplus RAM is on-card SDRAM at physical 0x20000000 and
	   0x30000000, nowhere near the SIMM windows. Neither 0x08000000 nor
	   0x04000000 is set in those addresses, so the old bit-test decode read
	   them as SIMM 0 bank 0 and fetched from unrelated memory. */
	machine.model = Model_Kinetic;
	mem_reset(512, 2);

	put(0x20000040);
	put(0x2fffff00);
	put(0x30000040);
	put(0x3fffff00);

	check_word("SDRAM bank 0",       0x20000040, marker(0x20000040));
	check_word("SDRAM bank 0 top",   0x2fffff00, marker(0x2fffff00));
	check_word("SDRAM bank 1",       0x30000040, marker(0x30000040));
	check_word("SDRAM bank 1 top",   0x3fffff00, marker(0x3fffff00));

	/* The motherboard SIMMs are still there underneath. */
	put(0x10000040);
	check_word("Kinetic SIMM 0",     0x10000040, marker(0x10000040));

	/* The specific defect: SDRAM must not resolve into SIMM 0. */
	{
		uint32_t ms = 0, mr = 0;
		const uint32_t *rs = mem_dma_region(0x20000040, &ms);
		const uint32_t *rr = mem_dma_region(0x10000040, &mr);

		checks++;
		if (rs == NULL || rr == NULL || rs == rr) {
			printf("FAIL: SDRAM bank 0 resolves to the same region as SIMM 0\n");
			failures++;
		}
	}

	machine.model = Model_RPCSA110;
}

int
main(void)
{
	mem_init();

	machine.model = Model_RPCSA110;

	test_256mb();
	test_128mb();
	test_small();
	test_kinetic_sdram();

	printf("%s: %d checks, %d failures\n",
	       failures ? "FAILED" : "PASSED", checks, failures);

	return failures ? 1 : 0;
}
