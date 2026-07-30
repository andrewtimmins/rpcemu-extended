/* MMU access permission tests.
 *
 * ADFFS has reported since 2013 that "RPCEmu doesn't emulate memory page access
 * correctly", and specifically that "JIT will not work under RPCEmu as it does
 * not generate Page Faults when User mode writes to pages with Supervisor only
 * write permissions". This test builds real page tables in emulated RAM and
 * checks the access permission rules of ARM DDI 0100B Table 7-7 are actually
 * enforced.
 *
 * The interesting cases are not the cold ones. A cold access walks the page
 * tables and the permission check is reached, so it behaves. The question is
 * whether an access that hits one of the emulator's three translation caches
 * still gets checked, which is what DDI 0100B section 7.6 requires:
 *
 *   "The TLB caches virtual to physical address translations and access
 *    permissions for each translation. If the TLB contains a translated entry
 *    for the virtual address, the access control logic determines whether
 *    access is permitted."
 *
 * So a page read legally in User mode, or written legally in Supervisor mode,
 * must still refuse a later illegal User write. Each such pair is tested with
 * the priming access first and the illegal access second.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "cp15.h"

/* Physical layout inside the emulated RAM at 0x10000000. The translation table
   base has to be 16K aligned; the section it maps can be anywhere. */
#define PHYS_RAM_BASE	0x10000000u
#define TT_BASE		(PHYS_RAM_BASE + 0x00100000u)	/* 1MB in, 16K aligned */
#define TARGET_PHYS	(PHYS_RAM_BASE + 0x00200000u)	/* section we map to */

/* Virtual address of the mapped section, and an address inside it. */
#define TEST_VADDR	0x00100000u
#define TEST_ADDR	(TEST_VADDR + 0x40u)

/* CP15 register numbers, positioned where cp15_write's RN macro reads them. */
#define CP15_OPCODE(crn)	((crn) << 16)

/* Control register: MMU enabled, S and R clear so Table 7-7 rows use AP alone. */
#define CTRL_MMU_ON	0x00000001u

/* Access permissions, from DDI 0100B Table 7-7 (S=0, R=0). */
#define AP_NONE		0	/* Supervisor: no access,  User: no access */
#define AP_SVC_RW	1	/* Supervisor: read/write, User: no access */
#define AP_SVC_RW_USR_RO 2	/* Supervisor: read/write, User: read only */
#define AP_RW		3	/* Supervisor: read/write, User: read/write */

static int failures;
static int checks;

/* Build a first-level section descriptor: DDI 0100B section 7.6.5. */
static uint32_t
section_descriptor(uint32_t phys_base, uint32_t ap, uint32_t domain)
{
	return (phys_base & 0xfff00000u) | (ap << 10) | (domain << 5) | 2u;
}

/*
 * Write a first-level descriptor straight into emulated RAM.
 *
 * mem_phys_write32 rather than the virtual path, because at this point there is
 * no valid mapping to write it through - this is the table the mapping comes
 * from.
 */
static void
install_section(uint32_t vaddr, uint32_t phys_base, uint32_t ap, uint32_t domain)
{
	const uint32_t fld_addr = TT_BASE | ((vaddr >> 18) & ~3u);

	mem_phys_write32(fld_addr, section_descriptor(phys_base, ap, domain));
}

/* Flush every translation cache: the single-entry caches, the TLB cache and the
   host-pointer maps. Writing CP15 register 8 on a StrongARM does all three. */
static void
flush_all(void)
{
	cp15_write(CP15_OPCODE(8), 0);
}

/* Perform one access and report whether it aborted. */
static int
access_faults(uint32_t addr, int is_write, int privileged)
{
	const int prev_memmode = memmode;
	int faulted;

	memmode = privileged;
	arm.event &= ~0x40u;

	if (is_write) {
		writememfl(addr, 0xdeadbeef);
	} else {
		(void) readmemfl(addr);
	}

	faulted = (arm.event & 0x40) != 0;
	arm.event &= ~0x40u;
	memmode = prev_memmode;

	return faulted;
}

static void
check(const char *what, int got, int want)
{
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL: %s: %s, expected %s\n", what,
		       got ? "faulted" : "was allowed",
		       want ? "a fault" : "to be allowed");
	} else {
		printf("ok:   %s: %s\n", what, got ? "faulted" : "allowed");
	}
}

/*
 * The cold cases. These prove the permission check itself is right, and that
 * the test rig maps memory the way it thinks it does.
 */
static void
test_cold_permissions(void)
{
	printf("\n-- cold accesses, no cached translation --\n");

	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW_USR_RO, 0);

	flush_all();
	check("AP=2 supervisor write", access_faults(TEST_ADDR, 1, 1), 0);
	flush_all();
	check("AP=2 supervisor read", access_faults(TEST_ADDR, 0, 1), 0);
	flush_all();
	check("AP=2 user read", access_faults(TEST_ADDR, 0, 0), 0);
	flush_all();
	check("AP=2 user write", access_faults(TEST_ADDR, 1, 0), 1);

	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW, 0);
	flush_all();
	check("AP=1 user read", access_faults(TEST_ADDR, 0, 0), 1);
	flush_all();
	check("AP=1 user write", access_faults(TEST_ADDR, 1, 0), 1);
	flush_all();
	check("AP=1 supervisor write", access_faults(TEST_ADDR, 1, 1), 0);

	install_section(TEST_VADDR, TARGET_PHYS, AP_RW, 0);
	flush_all();
	check("AP=3 user write", access_faults(TEST_ADDR, 1, 0), 0);

	install_section(TEST_VADDR, TARGET_PHYS, AP_NONE, 0);
	flush_all();
	check("AP=0 S=0 R=0 supervisor read", access_faults(TEST_ADDR, 0, 1), 1);
}

/*
 * The cases the bug report is about: an illegal access that follows a legal one
 * to the same page, so a cached translation exists.
 */
static void
test_cached_translation_still_checked(void)
{
	printf("\n-- illegal access after a legal one to the same page --\n");

	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW_USR_RO, 0);

	/* User read is legal on AP=2 and caches the translation. The user write
	   that follows must still fault. This is the case ADFFS relies on. */
	flush_all();
	(void) access_faults(TEST_ADDR, 0, 0);
	check("AP=2 user write after user read",
	      access_faults(TEST_ADDR, 1, 0), 1);

	/* Supervisor write is legal and caches the translation, including in the
	   write map. The user write that follows must still fault. */
	flush_all();
	(void) access_faults(TEST_ADDR, 1, 1);
	check("AP=2 user write after supervisor write",
	      access_faults(TEST_ADDR, 1, 0), 1);

	/* Supervisor read first, then the illegal user write. */
	flush_all();
	(void) access_faults(TEST_ADDR, 0, 1);
	check("AP=2 user write after supervisor read",
	      access_faults(TEST_ADDR, 1, 0), 1);

	/* AP=1 gives User no access at all. A supervisor read must not make the
	   page readable from User mode. */
	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW, 0);
	flush_all();
	(void) access_faults(TEST_ADDR, 0, 1);
	check("AP=1 user read after supervisor read",
	      access_faults(TEST_ADDR, 0, 0), 1);
}


/*
 * LDRT/STRT: a privileged instruction that must use User permissions.
 *
 * These matter out of proportion to how often they appear. mem_user_write32()
 * and friends set memmode directly and put it back, so they never go through
 * updatemode(). Any fix built on "invalidate the caches when the processor
 * changes mode" is therefore blind to them: the mode never changes as far as
 * that hook is concerned, and a translation cached by a legitimate Supervisor
 * write is still sitting there when the STRT arrives.
 *
 * A correct fix has to make the permission decision per access, not per mode
 * change, which is what the hardware does.
 */
static void
test_user_mode_accessors(void)
{
	int faulted;

	printf("\n-- LDRT/STRT (privileged instruction, User permissions) --\n");

	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW_USR_RO, 0);

	/* Cold STRT in a privileged mode: AP=2 denies User writes, so it faults. */
	flush_all();
	memmode = 1;
	arm.event &= ~0x40u;
	mem_user_write32(TEST_ADDR, 0x12345678);
	faulted = (arm.event & 0x40) != 0;
	arm.event &= ~0x40u;
	check("AP=2 STRT, no cached translation", faulted, 1);

	/* Now with a translation cached by a legal Supervisor write first. */
	flush_all();
	memmode = 1;
	writememfl(TEST_ADDR, 0xcafebabe);
	arm.event &= ~0x40u;
	mem_user_write32(TEST_ADDR, 0x12345678);
	faulted = (arm.event & 0x40) != 0;
	arm.event &= ~0x40u;
	check("AP=2 STRT after supervisor write", faulted, 1);

	/* LDRT on a page User may not read at all. */
	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW, 0);
	flush_all();
	memmode = 1;
	(void) readmemfl(TEST_ADDR);
	arm.event &= ~0x40u;
	(void) mem_user_read32(TEST_ADDR);
	faulted = (arm.event & 0x40) != 0;
	arm.event &= ~0x40u;
	check("AP=1 LDRT after supervisor read", faulted, 1);

	memmode = 1;
}

/*
 * Instruction fetch. getpccache() translates with rw=0 and prefetch=1 and
 * returns NULL when the fetch is refused, which is how a Prefetch Abort is
 * raised. A page the current mode may not read must not be executable from it.
 */
static void
test_instruction_fetch(void)
{
	printf("\n-- instruction fetch --\n");

	install_section(TEST_VADDR, TARGET_PHYS, AP_SVC_RW, 0);

	flush_all();
	memmode = 1;
	checks++;
	if (getpccache(TEST_VADDR) == NULL) {
		failures++;
		printf("FAIL: AP=1 supervisor fetch: refused, expected to be allowed\n");
	} else {
		printf("ok:   AP=1 supervisor fetch: allowed\n");
	}

	flush_all();
	memmode = 0;
	arm.event &= ~0x40u;
	checks++;
	if (getpccache(TEST_VADDR) != NULL) {
		failures++;
		printf("FAIL: AP=1 user fetch: allowed, expected to be refused\n");
	} else {
		printf("ok:   AP=1 user fetch: refused\n");
	}

	/* And after a legal supervisor fetch has cached the translation. */
	flush_all();
	memmode = 1;
	(void) getpccache(TEST_VADDR);
	memmode = 0;
	arm.event &= ~0x40u;
	checks++;
	if (getpccache(TEST_VADDR) != NULL) {
		failures++;
		printf("FAIL: AP=1 user fetch after supervisor fetch: allowed, "
		       "expected to be refused\n");
	} else {
		printf("ok:   AP=1 user fetch after supervisor fetch: refused\n");
	}
	memmode = 1;
}

int
main(void)
{
	mem_init();
	mem_reset(16, 2);	/* megabytes, not bytes - see mem_reset() */
	cp15_init();
	cp15_reset(CPUModel_SA110);

	/* Domain 0 as a Client, so the per-page permissions are the thing being
	   tested rather than being waved through by Manager access. */
	cp15_write(CP15_OPCODE(3), 1);
	cp15_write(CP15_OPCODE(2), TT_BASE);

	/* Clear the whole first-level table, so anything not deliberately mapped
	   is a translation fault rather than whatever RAM happened to hold. */
	for (uint32_t i = 0; i < 4096; i++) {
		mem_phys_write32(TT_BASE + i * 4, 0);
	}

	cp15_write(CP15_OPCODE(1), CTRL_MMU_ON);
	memmode = 1;

	if (!mmu) {
		printf("FAIL: MMU did not turn on, test cannot run\n");
		return 1;
	}

	test_cold_permissions();
	test_cached_translation_still_checked();
	test_user_mode_accessors();
	test_instruction_fetch();

	printf("\n%d checks, %d failures\n", checks, failures);

	return failures != 0;
}
