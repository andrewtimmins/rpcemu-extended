/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

#ifndef MEM_H
#define MEM_H

#include <stdint.h>

#include "rpcemu.h"

extern uint32_t mem_phys_read32(uint32_t addr);

/* Write guest memory by physical address. Exported for the OHCI controller,
   which bus-masters its descriptor lists out of main memory the way the real
   thing does - a device reaching into RAM without the CPU's involvement. */
extern void mem_phys_write32(uint32_t addr, uint32_t val);
extern uint32_t mem_phys_read8_debug(uint32_t addr);
extern int mem_phys_write8_debug(uint32_t addr, uint8_t val);

/* Side-effect-free virtual reads for the debugger: no I/O, no aborts, no
   watchpoint hits, and no data-abort event left pending on the CPU. */
extern int mem_debug_translate(uint32_t vaddr, uint32_t *phys);
extern int mem_debug_read(uint32_t vaddr, uint32_t size, uint32_t *out);

extern uint32_t readmemfl(uint32_t addr);
extern uint32_t readmemfb(uint32_t addr);
extern void writememfb(uint32_t addr, uint8_t val);
extern void writememfl(uint32_t addr, uint32_t val);

extern void clearmemcache(void);
extern void mem_init(void);
extern void mem_reset(uint32_t ramsize, uint32_t vram_size);

extern uintptr_t *vraddrl;
/*
 * Ring buffers of installed entries, per privilege level, for eviction and for
 * the physical-address invalidation the graphics and podule code needs.
 *
 * The size is how many guest pages can have a host pointer at once, so it is
 * also how much of the guest's working set gets the inlined fast path rather
 * than a call into readmemfl()/writememfl(). At 1024 entries - 4MB of pages -
 * an idle RISC OS desktop was installing around 480,000 entries a second and
 * evicting about 900,000, recycling the whole map some 500 times a second:
 * anything not touched again within about 2ms had already gone. 16384 entries
 * takes that to about 24,000 installs a second, with the same number of
 * evictions rather than double.
 *
 * Nothing about correctness depends on the size. An entry lives until the ring
 * wraps onto it, the guest flushes its TLB (cp15_vaddr_reset() clears both maps
 * outright), or the screen's write entries are invalidated for the next frame -
 * so a longer-lived entry cannot go stale behind a remapping. The costs that do
 * scale with it are the two linear scans over the ring, in cp15_vaddr_reset()
 * and cp15_tlb_invalidate_physical(). Measured, the guest flushes its TLB 0
 * times a second while idling and while running a benchmark, and the physical
 * invalidation is once a frame, so at this size those come to about 2 million
 * trivial comparisons a second.
 */
#define VADDR_RING_SIZE 16384

extern uint32_t vraddrls[2][VADDR_RING_SIZE],vraddrphys[2][VADDR_RING_SIZE];

extern uintptr_t *vwaddrl;
extern uint32_t vwaddrls[2][VADDR_RING_SIZE],vwaddrphys[2][VADDR_RING_SIZE];

//uint8_t pagedirty[0x1000];
#define HASH(l) (((l)>>2)&0x7FFF)

#define ROMSIZE (8*1024*1024)

extern uint32_t *ram00, *ram01, *ram1, *rom, *vram;
extern uint32_t *sdram0, *sdram1; /**< Kinetic on-card SDRAM banks (128MB each) */
extern uint8_t *romb;

extern uint32_t translateaddress2(uint32_t addr, int rw, int prefetch);

extern uint32_t tlbcache[0x100000];
extern uint8_t tlbperm[0x100000];

/** Set the privilege level and select the matching fast maps. */
extern void mem_set_privilege(int privileged);
/** The fast maps for a given privilege level, for filling and invalidating. */
extern uintptr_t *mem_read_map(int privileged);
extern uintptr_t *mem_write_map(int privileged);

/* Bits in tlbperm[], indexed by (privileged << 1) | is_write. */
#define TLB_PERM_USER_R		(1u << 0)
#define TLB_PERM_USER_W		(1u << 1)
#define TLB_PERM_PRIV_R		(1u << 2)
#define TLB_PERM_PRIV_W		(1u << 3)

extern int mmu,memmode;

/**
 * Translate a virtual address, using the cached translation where there is one.
 *
 * A cached entry is only usable if the current privilege level is allowed this
 * kind of access to it, which is what the hardware TLB does (DDI 0100B 7.6:
 * "If the TLB contains a translated entry for the virtual address, the access
 * control logic determines whether access is permitted"). This used to hand back
 * the cached address without looking, so any access to a page that had been
 * touched once was permitted regardless of privilege - the fault ADFFS reports.
 *
 * When the cached permissions refuse the access, fall through to the full walk
 * rather than faulting here, so the fault is raised in one place with the right
 * fault code and the Fault Address and Status registers set.
 *
 * @param addr     Virtual address
 * @param rw       Bool of whether this is for write access
 * @param prefetch Bool of whether this is for instruction fetch
 */
static inline uint32_t
translateaddress(uint32_t addr, int rw, int prefetch)
{
	const uint32_t page = addr >> 12;

	if (!(tlbcache[page] & 0xFFF)) {
		/* Bit (privileged << 1) | is_write, matching cp15.c. */
		const unsigned bit = (unsigned) ((memmode ? 2 : 0) | (rw ? 1 : 0));

		if (tlbperm[page] & (1u << bit)) {
			return tlbcache[page] | (addr & 0xFFF);
		}
	}

	return translateaddress2(addr, rw, prefetch);
}

extern void cacheclearpage(uint32_t a);

extern uint32_t mem_rammask;
extern uint32_t mem_vrammask;

/* Resolve a physical RAM address for the paths that fetch straight out of RAM
   rather than through the MMU: sound DMA and the VIDC cursor. See mem.c. */
extern const uint32_t *mem_dma_region(uint32_t phys_addr, uint32_t *mask);

/**
 * Read a 32-bit word from a virtual address.
 *
 * Performs direct access if possible.
 *
 * @param addr Virtual address
 * @return 32-bit word read from given virtual address
 */
static inline uint32_t
mem_read32(uint32_t addr)
{
	if (vraddrl[addr >> 12] & 1) {
		return readmemfl(addr);
	} else {
		uint32_t value = *((const uint32_t *) (addr + vraddrl[addr >> 12]));
		debugger_memory_access(addr, 4, 0, value);
		return value;
	}
}

/**
 * Read a byte from a virtual address.
 *
 * Performs direct access if possible.
 *
 * @param addr Virtual address
 * @return Byte read from given virtual address
 */
static inline uint32_t
mem_read8(uint32_t addr)
{
	if (vraddrl[addr >> 12] & 1) {
		return readmemfb(addr);
	} else {
#ifdef _RPCEMU_BIG_ENDIAN
		uint32_t value = *((const uint8_t *) ((addr ^ 3) + vraddrl[addr >> 12]));
#else
		uint32_t value = *((const uint8_t *) (addr + vraddrl[addr >> 12]));
#endif
		debugger_memory_access(addr, 1, 0, value);
		return value;
	}
}

/**
 * Write a 32-bit word to a virtual address.
 *
 * Performs direct access if possible.
 *
 * @param addr Virtual address
 * @param val  32-bit word to write
 */
static inline void
mem_write32(uint32_t addr, uint32_t val)
{
	if (vwaddrl[addr >> 12] & 3) {
		writememfl(addr, val);
	} else {
		*((uint32_t *) (addr + vwaddrl[addr >> 12])) = val;
		debugger_memory_access(addr, 4, 1, val);
	}
}

/**
 * Write a byte to a virtual address.
 *
 * Performs direct access if possible.
 *
 * @param addr Virtual address
 * @param val  Byte to write
 */
static inline void
mem_write8(uint32_t addr, uint8_t val)
{
	if (vwaddrl[addr >> 12] & 3) {
		writememfb(addr, val);
	} else {
#ifdef _RPCEMU_BIG_ENDIAN
		*((uint8_t *) ((addr ^ 3) + vwaddrl[addr >> 12])) = val;
#else
		*((uint8_t *) (addr + vwaddrl[addr >> 12])) = val;
#endif
		debugger_memory_access(addr, 1, 1, val);
	}
}

/**
 * Read a 32-bit word from a virtual address with User mode privileges.
 *
 * @param addr Virtual address
 * @return 32-bit word read from given virtual address
 */
static inline uint32_t
mem_user_read32(uint32_t addr)
{
	const int prev_memmode = memmode;
	uint32_t data;

	/* mem_set_privilege() rather than assigning memmode, so the fast maps
	   follow: they are what the inline accessor below actually reads. */
	mem_set_privilege(0);
	data = mem_read32(addr);
	mem_set_privilege(prev_memmode);

	return data;
}

/**
 * Read a byte from a virtual address with User mode privileges.
 *
 * @param addr Virtual address
 * @return Byte read from given virtual address
 */
static inline uint32_t
mem_user_read8(uint32_t addr)
{
	const int prev_memmode = memmode;
	uint32_t data;

	/* mem_set_privilege() rather than assigning memmode, so the fast maps
	   follow: they are what the inline accessor below actually reads. */
	mem_set_privilege(0);
	data = mem_read8(addr);
	mem_set_privilege(prev_memmode);

	return data;
}

/**
 * Write a 32-bit word to a virtual address with User mode privileges.
 *
 * @param addr Virtual address
 * @param val  32-bit word to write
 */
static inline void
mem_user_write32(uint32_t addr, uint32_t val)
{
	const int prev_memmode = memmode;

	/* mem_set_privilege() rather than assigning memmode, so the fast maps
	   follow: they are what the inline accessor below actually reads. */
	mem_set_privilege(0);
	mem_write32(addr, val);
	mem_set_privilege(prev_memmode);
}

/**
 * Write a byte to a virtual address with User mode privileges.
 *
 * @param addr Virtual address
 * @param val  Byte to write
 */
static inline void
mem_user_write8(uint32_t addr, uint8_t val)
{
	const int prev_memmode = memmode;

	/* mem_set_privilege() rather than assigning memmode, so the fast maps
	   follow: they are what the inline accessor below actually reads. */
	mem_set_privilege(0);
	mem_write8(addr, val);
	mem_set_privilege(prev_memmode);
}

#endif /* MEM_H */
