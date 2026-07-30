/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

/* System coprocessor + MMU emulation*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "cp15.h"
#include "mem.h"
#include "savestate.h"

int dcache = 0; /* Data cache on StrongARM, unified cache pre-StrongARM */

#define TLBCACHESIZE 256

uint32_t tlbcache[0x100000] = {0};
/*
 * What the cached translation in tlbcache[] is allowed to be used for.
 *
 * DDI 0100B section 7.6: "The TLB caches virtual to physical address
 * translations and access permissions for each translation. If the TLB contains
 * a translated entry for the virtual address, the access control logic
 * determines whether access is permitted." So the permissions have to be cached
 * with the entry and checked on every access, not decided once when the entry is
 * filled. Before this existed, a cached entry was reused without any check at
 * all, which is the fault ADFFS has reported since 2013.
 *
 * The four outcomes are precomputed when the entry is filled, so checking one is
 * an array read and a bit test rather than a walk of the domain and AP rules.
 */
uint8_t tlbperm[0x100000] = {0};
static uint32_t tlbcache2[TLBCACHESIZE];
/*
 * The host-pointer fast maps, one pair per privilege level.
 *
 * These are read straight from the mem.h inline accessors and from code the
 * recompiler emits, and the test there is a fixed "are the low bits clear" with
 * no idea of the current privilege level. So there is a set for User and a set
 * for privileged modes, and a page only appears in the set for a privilege level
 * that is actually allowed the access. vraddrl/vwaddrl point at the pair for the
 * current mode and are swapped by mem_set_privilege(), so every existing use
 * site is unchanged and nothing extra happens per access.
 *
 * Indexed [privileged][page].
 */
static uintptr_t vraddrl_mode[2][0x100000];
static uintptr_t vwaddrl_mode[2][0x100000];

/* The current privilege level's maps. */
uintptr_t *vraddrl = vraddrl_mode[0];
uintptr_t *vwaddrl = vwaddrl_mode[0];

/* Ring buffers of installed entries, per privilege level, for eviction and for
   the physical-address invalidation the graphics and podule code needs. */
uint32_t vraddrls[2][1024] = {{0}}, vraddrphys[2][1024] = {{0}};
uint32_t vwaddrls[2][1024] = {{0}}, vwaddrphys[2][1024] = {{0}};

void
mem_set_privilege(int privileged)
{
	memmode = privileged;
	vraddrl = vraddrl_mode[privileged];
	vwaddrl = vwaddrl_mode[privileged];
}

/* Install into, or clear from, a specific privilege level's maps. */
uintptr_t *
mem_read_map(int privileged)
{
	return vraddrl_mode[privileged];
}

uintptr_t *
mem_write_map(int privileged)
{
	return vwaddrl_mode[privileged];
}
static int tlbcachepos = 0;
int tlbs = 0, flushes = 0;

static struct cp15 {
	uint32_t ctrl;				/**< Control register */
	uint32_t translation_table;		/**< Translation Table Base register */
	uint32_t domain_access_control;		/**< Domain Access Control register */
	uint32_t fault_status;			/**< Fault Status register */
	uint32_t fault_address;			/**< Fault Address register */

	CPUModel cpu_model;			/**< CPU model emulated */
} cp15;

static int icache = 0;

/* The bits of the processor's internal coprocessor (MMU) control register */
#define CP15_CTRL_MMU			(1 << 0)
#define CP15_CTRL_ALIGNMENT_FAULT	(1 << 1)
#define CP15_CTRL_CACHE			(1 << 2)  /* Data cache only on SA */
#define CP15_CTRL_WRITE_BUFFER		(1 << 3)
#define CP15_CTRL_PROG32		(1 << 4)  /* Always enabled in SA */
#define CP15_CTRL_DATA32		(1 << 5)  /* Always enabled in SA */
#define CP15_CTRL_ABORT_TIMING		(1 << 6)  /* Always enabled in 710 & SA */
#define CP15_CTRL_BIG_ENDIAN		(1 << 7)
#define CP15_CTRL_SYSTEM		(1 << 8)
#define CP15_CTRL_ROM			(1 << 9)  /* 710 & SA */
#define CP15_CTRL_ICACHE		(1 << 12) /* SA only */

/* Fault Status codes stored in FSR[0:3] when a fault occurs */
#define CP15_FAULT_TRANSLATION_SECTION	0x5
#define CP15_FAULT_TRANSLATION_PAGE	0x7
#define CP15_FAULT_DOMAIN_SECTION	0x9
#define CP15_FAULT_DOMAIN_PAGE		0xb
#define CP15_FAULT_PERMISSION_SECTION	0xd
#define CP15_FAULT_PERMISSION_PAGE	0xf


static void
cp15_tlb_flush(void)
{
	int c;

	for (c = 0; c < TLBCACHESIZE; c++) {
		if (tlbcache2[c] != 0xffffffff) {
			tlbcache[tlbcache2[c]] = 0xffffffff;
			tlbperm[tlbcache2[c]] = 0;
			tlbcache2[c] = 0xffffffff;
		}
	}
}

static void
cp15_vaddr_reset(void)
{
	int c;

	int m;

	for (m = 0; m < 2; m++) {
		for (c = 0; c < 1024; c++) {
			if (vraddrls[m][c] != 0xFFFFFFFF) {
				vraddrl_mode[m][vraddrls[m][c]] = 0xFFFFFFFF;
				vraddrls[m][c] = 0xFFFFFFFF;
				vraddrphys[m][c] = 0xFFFFFFFF;
			}
			if (vwaddrls[m][c] != 0xFFFFFFFF) {
				vwaddrl_mode[m][vwaddrls[m][c]] = 0xFFFFFFFF;
				vwaddrls[m][c] = 0xFFFFFFFF;
				vwaddrphys[m][c] = 0xFFFFFFFF;
			}
		}
	}
}

/**
 * Invalidate Write-TLB entries corresponding to the given region of physical
 * addresses.
 *
 * @param addr Physical address
 */
void
cp15_tlb_invalidate_physical(uint32_t addr)
{
	int c;

	int m;

	for (m = 0; m < 2; m++) {
		for (c = 0; c < 1024; c++) {
			/* Skip unused ring slots: cp15_reset() invalidates vwaddrls[]
			   (to 0xffffffff) but leaves vwaddrphys[] stale, so a stale
			   phys entry can still match here after a reset - indexing
			   vwaddrl_mode[] with the invalid 0xffffffff page marker would
			   be a wild out-of-bounds write. */
			if (vwaddrls[m][c] != 0xffffffff &&
			    (vwaddrphys[m][c] & 0x1f000000) == addr) {
				vwaddrl_mode[m][vwaddrls[m][c]] = 0xffffffff;
				vwaddrls[m][c] = 0xffffffff;
				vwaddrphys[m][c] = 0xffffffff;
			}
		}
	}
}

/**
 * Called on program startup and emulated machine reset to
 * prepare the cp15 module
 *
 * @param cpu_model Model of CPU (and associated mmu/cp15) being emulated
 */
void
cp15_reset(CPUModel cpu_model)
{
	cp15.cpu_model = cpu_model;
	switch (cpu_model) {
	case CPUModel_ARM610:
		cp15.ctrl = 0;
		break;
	case CPUModel_ARM710:
	case CPUModel_ARM7500:
	case CPUModel_ARM7500FE:
		cp15.ctrl = CP15_CTRL_ABORT_TIMING;
		break;
	case CPUModel_SA110:
	case CPUModel_ARM810:
		cp15.ctrl = CP15_CTRL_ABORT_TIMING | CP15_CTRL_DATA32 | CP15_CTRL_PROG32;
		break;
	}
	dcache = 0;
	icache = 0;
	mmu = 0;
	prog32 = (cp15.ctrl & CP15_CTRL_PROG32) != 0;

        memset(tlbcache, 0xff, 0x100000 * sizeof(uint32_t));
        memset(tlbcache2, 0xff, TLBCACHESIZE * sizeof(uint32_t));
        tlbcachepos=0;
	/* Both privilege levels, and by the array rather than through the pointer:
	   sizeof(vraddrl) is the size of a pointer now, not of the map. An entry
	   left as zero would read as "accessible" and dereference host address 0
	   plus the guest address. */
	memset(vraddrl_mode, 0xff, sizeof(vraddrl_mode));
	memset(vwaddrl_mode, 0xff, sizeof(vwaddrl_mode));
	memset(vraddrls, 0xff, sizeof(vraddrls));
	memset(vwaddrls, 0xff, sizeof(vwaddrls));
}

/**
 * Called on program startup to prepare the cp15 module
 */
void
cp15_init(void)
{
}

static uint32_t *tlbram;
static uint32_t tlbrammask;

static void
cp15_tlb_flush_all(void)
{
	clearmemcache();
	cp15_tlb_flush();
	cp15_vaddr_reset();
	flushes++;
}

static int cp15_check_permissions(uint32_t ap, int is_write);

/* Build the TLB_PERM_* mask for an entry, from its Domain and AP bits. */
static uint8_t
cp15_perm_mask(uint32_t domain_access, uint32_t ap)
{
	uint8_t mask = 0;
	int privileged, is_write;

	/* A Manager for the Domain is not guarded by the page's permissions at
	   all (DDI 0100B section 7.9), so everything is allowed. */
	if (domain_access == 3) {
		return TLB_PERM_PRIV_R | TLB_PERM_PRIV_W |
		       TLB_PERM_USER_R | TLB_PERM_USER_W;
	}

	for (privileged = 0; privileged < 2; privileged++) {
		for (is_write = 0; is_write < 2; is_write++) {
			const int prev = memmode;
			int allowed;

			/* cp15_check_permissions() reads memmode, which is the
			   thing being varied here. */
			memmode = privileged;
			allowed = !cp15_check_permissions(ap, is_write);
			memmode = prev;

			if (allowed) {
				mask |= (uint8_t) (1u << ((privileged << 1) | is_write));
			}
		}
	}

	return mask;
}

static void
cp15_tlb_add_entry(uint32_t vaddr, uint32_t paddr, uint8_t perm)
{
	if (tlbcache2[tlbcachepos] != 0xffffffff) {
		tlbcache[tlbcache2[tlbcachepos]] = 0xffffffff;
		tlbperm[tlbcache2[tlbcachepos]] = 0;
	}
	tlbcache2[tlbcachepos] = vaddr >> 12;
	tlbcache[vaddr >> 12] = paddr & 0xfffff000;
	tlbperm[vaddr >> 12] = perm;

	tlbcachepos = (tlbcachepos + 1) & (TLBCACHESIZE - 1);
}

/**
 * Perform a MCR to Co-processor 15.
 *
 * @param opcode Opcode of instruction being emulated
 * @param val    Value from ARM register
 */
void
cp15_write(uint32_t opcode, uint32_t val)
{
	const uint32_t crn = RN;
	const uint32_t crm = RM;
	const uint32_t opc2 = (opcode >> 5) & 7;

	switch (crn) {
	case 1: /* Control */
		if (!icache && (val & CP15_CTRL_ICACHE)) {
			resetcodeblocks();
		}

		/* Are any of the MMU, ROM or System bits changing? */
		if (((cp15.ctrl ^ val) & (CP15_CTRL_MMU | CP15_CTRL_ROM | CP15_CTRL_SYSTEM)) != 0) {
			cp15_tlb_flush_all();
			resetcodeblocks();
		}

		cp15.ctrl = val;
		dcache = val & CP15_CTRL_CACHE;
		icache = val & CP15_CTRL_ICACHE;
		mmu = val & CP15_CTRL_MMU;
		prog32 = val & CP15_CTRL_PROG32;

		if (!prog32 && (arm.mode & 0x10)) {
			updatemode(arm.mode & 0xf);
		}
		return;

	case 2: /* Translation Table Base */
		cp15.translation_table = val & ~0x3fffu;
		/* Decode 30 address bits so the Kinetic SDRAM banks (0x20000000 and
		   0x30000000) are distinguished; for other models the page tables
		   never live above 0x1fffffff, so the wider mask is harmless. */
		switch (cp15.translation_table & 0x3f000000) {
		case 0x02000000: /* VRAM */
			tlbram = vram;
			tlbrammask = mem_vrammask >> 2;
			break;
		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			tlbram = ram00;
			tlbrammask = mem_rammask >> 2;
			break;
		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			tlbram = ram01;
			tlbrammask = mem_rammask >> 2;
			break;
		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			tlbram = ram1;
			tlbrammask = 0x7ffffff >> 2;
			break;
		case 0x20000000: /* Kinetic SDRAM bank 0 (128MB, aliases to 0x2f) */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000:
		case 0x29000000:
		case 0x2a000000:
		case 0x2b000000:
		case 0x2c000000:
		case 0x2d000000:
		case 0x2e000000:
		case 0x2f000000:
			tlbram = sdram0;
			tlbrammask = 0x7ffffff >> 2;
			break;
		case 0x30000000: /* Kinetic SDRAM bank 1 (128MB, aliases to 0x3f) */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000:
		case 0x39000000:
		case 0x3a000000:
		case 0x3b000000:
		case 0x3c000000:
		case 0x3d000000:
		case 0x3e000000:
		case 0x3f000000:
			tlbram = sdram1;
			tlbrammask = 0x7ffffff >> 2;
			break;
		}
		cp15_tlb_flush_all();
		resetcodeblocks();
		return;

	case 3: /* Domain Access Control */
		if (val != cp15.domain_access_control) {
			cp15.domain_access_control = val;
			cp15_tlb_flush_all();
			resetcodeblocks();
		}
		return;

	case 5:
	case 6:
		switch (cp15.cpu_model) {
		/* ARMv3 Architecture */
		case CPUModel_ARM610:
		case CPUModel_ARM710:
		case CPUModel_ARM7500:
		case CPUModel_ARM7500FE:
			switch (crn) {
			case 5: /* TLB Flush */
				cp15_tlb_flush_all();
				break;

			case 6: /* TLB Purge */
				cp15_tlb_flush_all();
				break;
			}
			resetcodeblocks();
			return;

		/* ARMv4 Architecture */
		case CPUModel_SA110:
		case CPUModel_ARM810:
			switch (crn) {
			case 5: /* Fault Status Register */
				cp15.fault_status = val;
				return;

			case 6: /* Fault Address Register */
				cp15.fault_address = val;
				return;
			}
			break;

		default:
			fprintf(stderr, "cp15_write(): unknown CPU model %d\n",
				cp15.cpu_model);
			fatal("cp15_write(): unknown CPU model %d", cp15.cpu_model);
		}
		break;

	case 7: /* Flush Cache */
		if ((crm & 1) && (opc2 == 0)) {
			resetcodeblocks();
		}
		pccache = 0xffffffff;
		return;

	case 8: /* TLB Operations (ARMv4) */
		if (cp15.cpu_model == CPUModel_SA110 || cp15.cpu_model == CPUModel_ARM810) {
			if (opc2 == 0) {
				/* TLB Flush */
				cp15_tlb_flush_all();
			} else {
				/* TLB Purge */
				cp15_tlb_flush_all();
			}
			if (crm & 1) {
				resetcodeblocks();
			}
			return;
		}
		break;

	case 15:
		if (cp15.cpu_model == CPUModel_SA110) {
			/* Test, Clock and Idle control */
			if (opc2 == 2 && crm == 1) {
				/* Enable clock switching - no need to implement */
				return;
			}
		}
		break;
	}

	UNIMPLEMENTED("CP15 Write", "Register %u, opcode %08x", crn, opcode);
}

/**
 * Perform a MRC from Co-processor 15.
 *
 * @param opcode Opcode of instruction being emulated
 * @return Value to ARM register
 */
uint32_t
cp15_read(uint32_t opcode)
{
	const uint32_t crn = RN;

	switch (crn) {
	case 0: /* ID */
		switch (cp15.cpu_model) {
		case CPUModel_ARM7500:   return 0x41027100;
		case CPUModel_ARM7500FE: return 0x41077100;
		case CPUModel_ARM610:    return 0x41560610;
		case CPUModel_ARM710:    return 0x41007100;
		case CPUModel_ARM810:    return 0x41018100;
		case CPUModel_SA110:     return 0x4401a102;
		}
		break;
	case 1: /* Control */
		return cp15.ctrl;
	case 2: /* Translation Table Base */
		return cp15.translation_table;
	case 3: /* Domain Access Control */
		return cp15.domain_access_control;
	case 5: /* Fault Status */
		return cp15.fault_status;
	case 6: /* Fault Address */
		return cp15.fault_address;
	default:
		UNIMPLEMENTED("CP15 Read", "Unknown register %u, opcode %08x", crn, opcode);
	}
	fatal("Bad read CP15 %08x %08x\n", opcode, PC);
}

/**
 * @param ap       Access Permissions (from Descriptor)
 * @param is_write Non-zero if this is for write access
 * @return Non-zero if the access should be faulted
 */
static int
cp15_check_permissions(uint32_t ap, int is_write)
{
	switch (ap) {
	case 0:
		switch (cp15.ctrl & 0x300) {
		case 0x000: /* No access */
		case 0x300: /* Unpredictable */
			return 1;

		case 0x100: /* Supervisor read-only */
			return !memmode || is_write;

		case 0x200: /* Read-only */
			return is_write;
		}
		break;

	case 1: /* Supervisor read/write */
		return !memmode;

	case 2: /* Supervisor read/write, User read-only*/
		return !memmode && is_write;
	}
	/* Any access permitted */
	return 0;
}

/**
 * Return the value which encodes the access permitted for a Domain.
 *
 * @param domain Domain number
 * @return Access permitted by Domain
 */
static uint32_t
cp15_domain_access(uint32_t domain)
{
	uint32_t shift = (domain << 1); /* Shift needed to extract value for this Domain */

	return (cp15.domain_access_control >> shift) & 3;
}

/**
 * Translate a virtual address to a physical address.
 *
 * The access permissions are checked and an Abort may be generated.
 *
 * @param addr     Virtual address
 * @param rw       Bool of whether this is for write access
 * @param prefetch Bool of whether this is for instruction fetch
 * @return Translated physical address (if no Fault occurred)
 */
uint32_t
translateaddress2(uint32_t addr, int rw, int prefetch)
{
	uint32_t fld_addr, fld;
	uint32_t sld_addr, sld;
	uint32_t domain, fault_code;
	uint32_t domain_access;
	uint32_t temp;
	uint32_t access_permissions;
	uint32_t phys_addr;

	tlbs++;

	/* Fetch first-level descriptor */
	fld_addr = cp15.translation_table | ((addr >> 18) & ~3u);
	fld = tlbram[(fld_addr >> 2) & tlbrammask];
	domain = (fld >> 5) & 0xf;

	switch (fld & 3) {
	case 0: /* Fault (Section Translation) */
		fault_code = CP15_FAULT_TRANSLATION_SECTION;
		goto do_fault;

	case 1: /* Page */
		/* Fetch second-level descriptor */
		sld_addr = (fld & 0xfffffc00) | ((addr >> 10) & 0x3fc);
		sld = mem_phys_read32(sld_addr);

		/* Check second-level descriptor */
		switch (sld & 3) {
		case 1: /* Large page (64 KB) */
			temp = (addr & 0xc000) >> 13;
			phys_addr = (sld & 0xffff0000) | (addr & 0xffff);
			break;
		case 2: /* Small page (4 KB) */
			temp = (addr & 0xc00) >> 9;
			phys_addr = (sld & 0xfffff000) | (addr & 0xfff);
			break;
		default: /* 0 (Fault) or 3 (Reserved) */
			fault_code = CP15_FAULT_TRANSLATION_PAGE;
			goto do_fault;
		}

		/* Check Domain */
		domain_access = cp15_domain_access(domain);
		if (domain_access == 0 || domain_access == 2) {
			fault_code = CP15_FAULT_DOMAIN_PAGE;
			goto do_fault;
		}
		if (domain_access == 1) {
			/* Client Domain - check permissions */
			access_permissions = (sld >> (temp + 4)) & 3;
			if (cp15_check_permissions(access_permissions, rw)) {
				fault_code = CP15_FAULT_PERMISSION_PAGE;
				goto do_fault;
			}
		} else {
			access_permissions = 3;	/* Manager: unguarded */
		}
		cp15_tlb_add_entry(addr, phys_addr,
		    cp15_perm_mask(domain_access, access_permissions));
		return phys_addr;

	case 2: /* Section (1 MB) */
		/* Check Domain */
		domain_access = cp15_domain_access(domain);
		if (domain_access == 0 || domain_access == 2) {
			fault_code = CP15_FAULT_DOMAIN_SECTION;
			goto do_fault;
		}
		if (domain_access == 1) {
			/* Client Domain - check permissions */
			access_permissions = (fld >> 10) & 3;
			if (cp15_check_permissions(access_permissions, rw)) {
				fault_code = CP15_FAULT_PERMISSION_SECTION;
				goto do_fault;
			}
		} else {
			access_permissions = 3;	/* Manager: unguarded */
		}
		phys_addr = (fld & 0xfff00000) | (addr & 0xfffff);
		cp15_tlb_add_entry(addr, phys_addr,
		    cp15_perm_mask(domain_access, access_permissions));
		return phys_addr;

	default:
		fatal("Bad descriptor type %u %08x Address %08x\n", fld & 3, fld, addr);
	}
	exit(-1);

do_fault:
	arm.event |= 0x40;
	if (!prefetch) {
		cp15.fault_address = addr;
		cp15.fault_status = (domain << 4) | fault_code;
	}
	return 0;
}

void
cp15_get_fault(uint32_t *fault_address, uint32_t *fault_status)
{
	if (fault_address != NULL) {
		*fault_address = cp15.fault_address;
	}
	if (fault_status != NULL) {
		*fault_status = cp15.fault_status;
	}
}

/*uint32_t translateaddress(uint32_t addr, int rw)
{
        if (!(addr&0xFC000000) && !(tlbcache[(addr>>12)&0x3FFF]&0xFFF))
        {
//                rpclog("Cached %08X\n",tlbcache[addr>>12]);
                return tlbcache[addr>>12]|(addr&0xFFF);
        }
        return translateaddress2(addr,rw);
}*/


const uint32_t *
getpccache(uint32_t addr)
{
	uint32_t phys_addr;

	addr &= ~0xfffu;
	if (mmu) {
		phys_addr = translateaddress(addr, 0, 1);
		if (arm.event & 0x40) {
			arm.event &= ~0x40u;
			return NULL;
		}
	} else {
		phys_addr = addr;
	}

	/* Invalidate write pointer for this page - so we can handle code
	   modification. Both privilege levels, or a page being executed stays
	   directly writable in whichever one was missed, and the code cache would
	   be corrupted rather than a fault raised. */
	mem_write_map(0)[addr >> 12] = 0xffffffff;
	mem_write_map(1)[addr >> 12] = 0xffffffff;

	/* Decode 30 address bits so the Kinetic SDRAM banks (0x20000000 and
	   0x30000000) are reachable for instruction fetch; for other models the
	   PC never lives above 0x1fffffff, so the wider mask is harmless. */
	switch (phys_addr & 0x3f000000) {
	case 0x00000000: /* ROM */
		return &rom[((uintptr_t) (phys_addr & 0x7ff000) - (uintptr_t) addr) >> 2];
	case 0x02000000: /* VRAM */
		return &vram[((uintptr_t) (phys_addr & mem_vrammask) - (uintptr_t) addr) >> 2];
	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
		return &ram00[((uintptr_t) (phys_addr & mem_rammask) - (uintptr_t) addr) >> 2];
	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
		return &ram01[((uintptr_t) (phys_addr & mem_rammask) - (uintptr_t) addr) >> 2];
	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ram1 != NULL) {
			return &ram1[((uintptr_t) (phys_addr & 0x7ffffff) - (uintptr_t) addr) >> 2];
		}
		break;
	case 0x20000000: /* Kinetic SDRAM bank 0 (128MB, aliases to 0x2f) */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000:
	case 0x29000000:
	case 0x2a000000:
	case 0x2b000000:
	case 0x2c000000:
	case 0x2d000000:
	case 0x2e000000:
	case 0x2f000000:
		if (sdram0 != NULL) {
			return &sdram0[((uintptr_t) (phys_addr & 0x7ffffff) - (uintptr_t) addr) >> 2];
		}
		break;
	case 0x30000000: /* Kinetic SDRAM bank 1 (128MB, aliases to 0x3f) */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000:
	case 0x39000000:
	case 0x3a000000:
	case 0x3b000000:
	case 0x3c000000:
	case 0x3d000000:
	case 0x3e000000:
	case 0x3f000000:
		if (sdram1 != NULL) {
			return &sdram1[((uintptr_t) (phys_addr & 0x7ffffff) - (uintptr_t) addr) >> 2];
		}
		break;
	}
	/*
	 * The address translated, but no memory is mapped at the physical address
	 * it translated to.
	 *
	 * This used to be fatal, which turns a guest crash into ours: a wild branch
	 * in the guest took the whole emulator down. On a real Risc PC a fetch from
	 * unmapped physical space gives an external abort and RISC OS reports it, so
	 * a Prefetch Abort is both the accurate response and the useful one - it
	 * leaves the guest to say what happened, which is worth far more than a dead
	 * process. The caller turns NULL into that abort.
	 *
	 * Still logged, because it usually does mean something is wrong, just not
	 * necessarily in the emulator.
	 */
	{
		static int reported;

		if (reported < 8) {
			reported++;
			rpclog("Prefetch abort: nothing mapped for PC %08x (physical %08x)%s\n",
			       addr, phys_addr,
			       reported == 8 ? " - further occurrences will not be logged" : "");
		}
	}

	return NULL;
}

/**
 * Write the MMU/system coprocessor state to a suspend snapshot.
 */
void
cp15_savestate(FILE *f)
{
	savestate_write_u32(f, cp15.ctrl);
	savestate_write_u32(f, cp15.translation_table);
	savestate_write_u32(f, cp15.domain_access_control);
	savestate_write_u32(f, cp15.fault_status);
	savestate_write_u32(f, cp15.fault_address);
}

/**
 * Restore the MMU/system coprocessor state from a suspend snapshot.
 *
 * The cached control flags, the host pointer to the page-table RAM
 * (tlbram) and all cached translation state are re-derived; the TLB caches
 * are flushed and rebuilt lazily from the page tables in the restored RAM.
 */
void
cp15_loadstate(FILE *f)
{
	uint32_t ttb_bank;

	cp15.ctrl = savestate_read_u32(f);
	cp15.translation_table = savestate_read_u32(f);
	cp15.domain_access_control = savestate_read_u32(f);
	cp15.fault_status = savestate_read_u32(f);
	cp15.fault_address = savestate_read_u32(f);

	dcache = cp15.ctrl & CP15_CTRL_CACHE;
	icache = cp15.ctrl & CP15_CTRL_ICACHE;
	mmu = cp15.ctrl & CP15_CTRL_MMU;
	prog32 = cp15.ctrl & CP15_CTRL_PROG32;

	/* Re-derive the page-table RAM pointer from the Translation Table Base,
	   mirroring the 30-bit bank decode in cp15_write() case 2 (including the
	   Kinetic SDRAM banks at 0x20000000/0x30000000). */
	ttb_bank = cp15.translation_table & 0x3f000000;
	if (ttb_bank == 0x02000000) {
		tlbram = vram;
		tlbrammask = mem_vrammask >> 2;
	} else if (ttb_bank >= 0x10000000 && ttb_bank <= 0x13000000) {
		tlbram = ram00;
		tlbrammask = mem_rammask >> 2;
	} else if (ttb_bank >= 0x14000000 && ttb_bank <= 0x17000000) {
		tlbram = ram01;
		tlbrammask = mem_rammask >> 2;
	} else if (ttb_bank >= 0x18000000 && ttb_bank <= 0x1f000000) {
		tlbram = ram1;
		tlbrammask = 0x7ffffff >> 2;
	} else if (ttb_bank >= 0x20000000 && ttb_bank <= 0x2f000000) {
		tlbram = sdram0;
		tlbrammask = 0x7ffffff >> 2;
	} else if (ttb_bank >= 0x30000000 && ttb_bank <= 0x3f000000) {
		tlbram = sdram1;
		tlbrammask = 0x7ffffff >> 2;
	}

	cp15_tlb_flush_all();
}
