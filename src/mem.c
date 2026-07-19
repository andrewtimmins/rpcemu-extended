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

/* Memory handling */
#include <assert.h>

#include "rpcemu.h"
#include "vidc20.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "cp15.h"
#include "superio.h"
#include "podules.h"
#include "savestate.h"
#include "fdc.h"

/* References -
   Acorn Risc PC - Technical Reference Manual
*/

uint32_t *ram00 = NULL; /**< Word pointer to SIMM 0 Bank 0 of physical RAM */
uint32_t *ram01 = NULL; /**< Word pointer to SIMM 0 Bank 1 of physical RAM */
uint32_t *ram1  = NULL; /**< Word pointer to SIMM 1 of physical RAM */
uint32_t *sdram0 = NULL; /**< Word pointer to Kinetic on-card SDRAM bank 0 (128MB) */
uint32_t *sdram1 = NULL; /**< Word pointer to Kinetic on-card SDRAM bank 1 (128MB) */
uint32_t *rom   = NULL; /**< Word pointer to ROM */
uint32_t *vram  = NULL; /**< Word pointer to Video RAM */

int mmu = 0;     /**< Bool of whether the MMU is enabled */
int memmode = 0; /**< Bool of whether ARM is in a privileged mode */

uint32_t mem_rammask; /**< Mask used for SIMM Bank 0/1 to handle the repeating address space */
uint32_t mem_vrammask; /**< Mask used for VRAM to handle the repeating address space */

static uint8_t *ramb00 = NULL; /**< Byte pointer to SIMM 0 Bank 0 of physical RAM */
static uint8_t *ramb01 = NULL; /**< Byte pointer to SIMM 0 Bank 1 of physical RAM */
static uint8_t *ramb1  = NULL; /**< Byte pointer to SIMM 1 of physical RAM */
static uint8_t *sdramb0 = NULL; /**< Byte pointer to Kinetic SDRAM bank 0 */
static uint8_t *sdramb1 = NULL; /**< Byte pointer to Kinetic SDRAM bank 1 */

#define SDRAM_BANK_SIZE  (128 * 1024 * 1024) /**< Kinetic SDRAM bank size */
#define SDRAM_BANK_MASK  0x7ffffff            /**< 128MB address mask within an SDRAM bank */
uint8_t *romb = NULL;          /**< Byte pointer to ROM */
static uint8_t *vramb  = NULL; /**< Byte pointer to Video RAM */

static uint32_t readmemcache = 0,readmemcache2 = 0;
static uint32_t writememcache = 0,writememcache2 = 0;
static uint32_t writemembcache = 0,writemembcache2 = 0;

static uint32_t phys_space_mask; /**< Mask used to convert to physical memory address space */

/**
 * Place an 8-bit I/O write value into the correct lane for a word-oriented handler.
 */
static uint32_t
mem_io_byte_to_lane(uint32_t addr, uint32_t val)
{
#ifdef _RPCEMU_BIG_ENDIAN
	return val << ((3u - (addr & 3u)) * 8);
#else
	return val << ((addr & 3u) * 8);
#endif
}

/**
 * Expand an 8-bit I/O read into a 32-bit word with all byte lanes aliased.
 */
static uint32_t
mem_io_byte_to_word(uint8_t val)
{
	return (uint32_t) val * 0x01010101u;
}

void clearmemcache(void)
{
	readmemcache = 0xffffffff;
	writememcache = 0xffffffff;
	writemembcache = 0xffffffff;
}

static int vraddrlpos, vwaddrlpos;

/**
 * Initialise memory (called only once on program startup)
 */
void mem_init(void)
{
	rom  = malloc(ROMSIZE);

	/* Allocate the VRAM at the largest supported size (16MB - the most that
	   fits the Risc PC's 0x02000000 VRAM aperture). The active size is
	   selected per-machine and only narrows mem_vrammask in mem_reset(). */
	vram = malloc(16 * 1024 * 1024);
	if (rom == NULL || vram == NULL) {
		fatal("Unable to allocate memory for ROM/VRAM");
	}
	romb  = (uint8_t *) rom;
	vramb = (uint8_t *) vram;
}

/**
 * Initialise/reset RAM (called on startup and emulated machine reset)
 *
 * @param ramsize Amount of RAM in megabytes
 * @param vram_size Amount of VRAM in megabytes
 */
void
mem_reset(uint32_t ramsize, uint32_t vram_size)
{
	int have_sdram;

	assert(ramsize >= 4); /* At least 4MB */
	assert(ramsize <= 512); /* At most 512MB (Kinetic); 256MB on other models */
	assert(((ramsize - 1) & ramsize) == 0); /* Must be a power of 2 */

	/* Only the Kinetic StrongARM card carries the extra on-card SDRAM; clamp
	   any other model back to the 256MB the motherboard IOMD can address. */
	if (machine.model != Model_Kinetic && ramsize > 256) {
		ramsize = 256;
	}

	/* Any RAM above the motherboard's 256MB lives in the two 128MB Kinetic
	   SDRAM banks (physical 0x20000000 and 0x30000000). */
	have_sdram = ramsize > 256;

	/* Convert ramsize from bytes to megabytes */
	ramsize *= (1024 * 1024);

	if (have_sdram) {
		/* The motherboard still provides its full 256MB; the surplus is SDRAM */
		ramsize = 256 * 1024 * 1024;

		sdram0 = realloc(sdram0, SDRAM_BANK_SIZE);
		sdram1 = realloc(sdram1, SDRAM_BANK_SIZE);
		if (sdram0 == NULL || sdram1 == NULL) {
			fatal("Unable to allocate memory for the Kinetic SDRAM banks");
		}
		sdramb0 = (uint8_t *) sdram0;
		sdramb1 = (uint8_t *) sdram1;
		memset(sdram0, 0, SDRAM_BANK_SIZE);
		memset(sdram1, 0, SDRAM_BANK_SIZE);
	} else {
		free(sdram0);
		free(sdram1);
		sdram0 = NULL;
		sdram1 = NULL;
		sdramb0 = NULL;
		sdramb1 = NULL;
	}

	if (ramsize == (256 * 1024 * 1024)) {
		ramsize = 128 * 1024 * 1024; /* 128MB for first SIMM */

		/* Allocate additional 128MB */
		ram1 = realloc(ram1, 128 * 1024 * 1024);
		if (ram1 == NULL) {
			fatal("Unable to allocate memory for the second 128MB SIMM");
		}
		ramb1 = (uint8_t *) ram1;
		memset(ram1, 0, 128 * 1024 * 1024);
	} else {
		free(ram1);
		ram1 = NULL;
		ramb1 = NULL;
	}

	/* Calculate mem_rammask */
	mem_rammask = (ramsize / 2) - 1;

	/* Calculate mem_vramask */
	if (vram_size != 0) {
		mem_vrammask = (vram_size * 1024 * 1024) - 1;
	} else {
		mem_vrammask = 0;
	}

	ram00 = realloc(ram00, ramsize / 2);
	ram01 = realloc(ram01, ramsize / 2);
	if (ram00 == NULL || ram01 == NULL) {
		fatal("Unable to allocate memory for RAM");
	}
	ramb00 = (uint8_t *) ram00;
	ramb01 = (uint8_t *) ram01;
	memset(ram00, 0, ramsize / 2);
	memset(ram01, 0, ramsize / 2);

	vraddrlpos = vwaddrlpos = 0;

	if (machine.model == Model_Phoebe || machine.model == Model_Kinetic) {
		/* 30 address bits are decoded (IOMD2 on Phoebe; the Kinetic card
		   places its SDRAM at 0x20000000/0x30000000). This gives a physical
		   memory map of 1G that repeats in the 4G address space. */
		phys_space_mask = 0x3fffffff;
	} else {
		/* 29 address bits are connected to IOMD. This results in a
		   physical memory map of 512M that repeats in the 4G address space */
		phys_space_mask = 0x1fffffff;
	}
}

static inline void
vradd(uint32_t a, const void *v, uint32_t f, uint32_t p)
{
	NOT_USED(f);

	if (vraddrls[vraddrlpos] != 0xffffffff) {
		vraddrl[vraddrls[vraddrlpos]] = 0xffffffff;
	}
	vraddrls[vraddrlpos] = a >> 12;
	vraddrl[a >> 12] = (uintptr_t) v; /* | f; */
	vraddrphys[vraddrlpos] = p;
	vraddrlpos = (vraddrlpos + 1) & 0x3ff;
}

static inline void
vwadd(uint32_t a, const void *v, uint32_t f, uint32_t p)
{
	NOT_USED(f);

	/* Invalidate all code blocks on this page, so that any blocks on this
	   page are forced to be recompiled */
	cacheclearpage(a >> 12);
	if (vwaddrls[vwaddrlpos] != 0xffffffff) {
		vwaddrl[vwaddrls[vwaddrlpos]] = 0xffffffff;
	}
	vwaddrls[vwaddrlpos] = a >> 12;
	vwaddrl[a >> 12] = (uintptr_t) v; /* | f; */
	vwaddrphys[vwaddrlpos] = p;
	vwaddrlpos = (vwaddrlpos + 1) & 0x3ff;
}

/**
 * Read a 32-bit word from a physical address.
 *
 * @param addr Physical address
 * @return 32-bit word read from given physical address
 */
uint32_t
mem_phys_read32(uint32_t addr)
{
	addr &= phys_space_mask;

	switch (addr & (phys_space_mask & 0xff000000)) { /* Select in 16MB chunks */
	case 0x00000000: /* ROM */
		return rom[(addr & 0x7fffff) >> 2];

	case 0x02000000: /* VRAM */
		if (mem_vrammask == 0)
			return 0xffffffff;
		return vram[(addr & mem_vrammask) >> 2];

	case 0x03000000: /* IO */
		if ((addr & 0xc00000) == 0) {
			/* 03000000 - 033fffff */
			uint32_t bank = (addr >> 16) & 7;

			switch (bank) {
			case 0:
				/* IOMD Registers */
				return iomd_read(addr);
			case 1:
			case 2:
				if ((addr == 0x3310000) && (machine.iomd_type == IOMDType_IOMD))
					return iomd_mouse_buttons_read();
				if (addr >= 0x3010000 && addr < 0x3012000) {
					/* SuperIO */
					if ((addr & 0xffc) == 0x7c0) {
						return readidew();
					}
					{
						const uint8_t byte = superio_read(addr);
						return mem_io_byte_to_word(byte);
					}
				}
				break;
			case 4:
				/* Podule space 0, 1, 2, 3 */
				return podules_read16((addr >> 14) & 3, PODULE_IO_TYPE_IOC, addr & 0x3fff);
			case 7:
				/* Podule space 4, 5, 6, 7 */
				return podules_read16(((addr >> 14) & 3) + 4, PODULE_IO_TYPE_IOC, addr & 0x3fff);
			}
		}
		if ((machine.model == Model_Phoebe) && (addr & 0xcffffc) == 0x8007c0) {
			return readidew();
		}
		break;

	case 0x08000000: /* EASI space */
	case 0x09000000:
	case 0x0a000000:
	case 0x0b000000:
	case 0x0c000000:
	case 0x0d000000:
	case 0x0e000000:
	case 0x0f000000:
		return podules_read32((addr >> 24) & 7, PODULE_IO_TYPE_EASI, addr & 0xffffff);

	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
		return ram00[(addr & mem_rammask) >> 2];

	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
		return ram01[(addr & mem_rammask) >> 2];

	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ram1 != NULL) {
			return ram1[(addr & 0x7ffffff) >> 2];
		}
		break;

	case 0x20000000: /* Kinetic SDRAM bank 0 */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000: /* 128MB bank aliases on undecoded A27 */
	case 0x29000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdram0 != NULL) {
			return sdram0[(addr & SDRAM_BANK_MASK) >> 2];
		}
		break;

	case 0x30000000: /* Kinetic SDRAM bank 1 */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000: /* 128MB bank aliases on undecoded A27 */
	case 0x39000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdram1 != NULL) {
			return sdram1[(addr & SDRAM_BANK_MASK) >> 2];
		}
		break;
	}
	return 0;
}

/**
 * Read a byte from a physical address.
 *
 * @param addr Physical address
 * @return Byte read from given physical address
 */
static uint32_t
mem_phys_read8(uint32_t addr)
{
	addr &= phys_space_mask;

	switch (addr & (phys_space_mask & 0xff000000)) { /* Select in 16MB chunks */
	case 0x00000000: /* ROM */
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return romb[addr & 0x7fffff];

	case 0x02000000: /* VRAM */
		if (mem_vrammask == 0)
			return 0xff;
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return vramb[addr & mem_vrammask];

	case 0x03000000: /* IO */
		if ((addr & 0xc00000) == 0) {
			/* 03000000 - 033fffff */
			uint32_t bank = (addr >> 16) & 7;

			switch (bank) {
			case 0:
				/* IOMD Registers */
				return iomd_read(addr);
			case 1:
			case 2:
				if ((addr == 0x3310000) && (machine.iomd_type == IOMDType_IOMD))
					return iomd_mouse_buttons_read();
				if (addr >= 0x3012000 && addr <= 0x302a000)
					return fdc_dma_read(addr);
				if ((addr & 0xfff400) == 0x02b000) {
					/* Network podule */
					return 0xffffffff;
				}
				if (addr >= 0x3010000 && addr < 0x3012000) {
					/* SuperIO */
					return superio_read(addr);
				}
				break;
			case 4:
				/* Podule space 0, 1, 2, 3 */
				return podules_read8((addr >> 14) & 3, PODULE_IO_TYPE_IOC, addr & 0x3fff);
			case 7:
				/* Podule space 4, 5, 6, 7 */
				return podules_read8(((addr >> 14) & 3) + 4, PODULE_IO_TYPE_IOC, addr & 0x3fff);
			}
		}
		if ((machine.model == Model_Phoebe) && (addr & 0xcff000) == 0x800000) {
			return readide((addr >> 2) & 0x3ff);
		}
		break;

	case 0x08000000: /* EASI space */
	case 0x09000000:
	case 0x0a000000:
	case 0x0b000000:
	case 0x0c000000:
	case 0x0d000000:
	case 0x0e000000:
	case 0x0f000000:
		return podules_read8((addr >> 24) & 7, PODULE_IO_TYPE_EASI, addr & 0xffffff);

	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return ramb00[addr & mem_rammask];

	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return ramb01[addr & mem_rammask];

	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return ramb1[addr & 0x7ffffff];
		}
		break;

	case 0x20000000: /* Kinetic SDRAM bank 0 */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000: /* 128MB bank aliases on undecoded A27 */
	case 0x29000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb0 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return sdramb0[addr & SDRAM_BANK_MASK];
		}
		break;

	case 0x30000000: /* Kinetic SDRAM bank 1 */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000: /* 128MB bank aliases on undecoded A27 */
	case 0x39000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return sdramb1[addr & SDRAM_BANK_MASK];
		}
		break;
	}
	return 0xff;
}

/**
 * Read a byte from a physical address for debugging.
 *
 * This function is safe to call from the debugger - it only reads
 * RAM, ROM, and VRAM directly without triggering I/O side effects
 * or data aborts. IO space returns 0.
 *
 * @param addr Physical address
 * @return Byte read from given physical address, or 0 for unmapped/IO
 */
uint32_t
mem_phys_read8_debug(uint32_t addr)
{
	addr &= phys_space_mask;

	switch (addr & (phys_space_mask & 0xff000000)) {
	case 0x00000000: /* ROM */
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return romb[addr & 0x7fffff];

	case 0x02000000: /* VRAM */
		if (mem_vrammask == 0)
			return 0;
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return vramb[addr & mem_vrammask];

	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return ramb00[addr & mem_rammask];

	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		return ramb01[addr & mem_rammask];

	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return ramb1[addr & 0x7ffffff];
		}
		break;

	case 0x20000000: /* Kinetic SDRAM bank 0 */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000: /* 128MB bank aliases on undecoded A27 */
	case 0x29000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb0 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return sdramb0[addr & SDRAM_BANK_MASK];
		}
		break;

	case 0x30000000: /* Kinetic SDRAM bank 1 */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000: /* 128MB bank aliases on undecoded A27 */
	case 0x39000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			return sdramb1[addr & SDRAM_BANK_MASK];
		}
		break;

	default:
		/* IO space or unmapped - return 0 without side effects */
		break;
	}
	return 0;
}

/**
 * Write a 32-bit word to a physical address.
 *
 * @param addr Physical address
 * @param val  32-bit word to write
 */
static void
mem_phys_write32(uint32_t addr, uint32_t val)
{
	addr &= phys_space_mask;

	switch (addr & (phys_space_mask & 0xff000000)) { /* Select in 16MB chunks */
	case 0x02000000: /* VRAM */
		if (mem_vrammask == 0)
			return;
		vram[(addr & mem_vrammask) >> 2] = val;
		dirtybuffer[(addr & mem_vrammask) >> 12] = 1;
		break;

	case 0x03000000: /* IO */
		if ((addr & 0xc00000) == 0) {
			uint32_t bank = (addr >> 16) & 7;

			switch (bank) {
			case 0:
				iomd_write(addr, val);
				return;
			case 1:
			case 2:
				if (addr >= 0x3010000 && addr < 0x3012000) {
					/* SuperIO */
					if ((addr & 0xffc) == 0x7c0) {
						writeidew(val);
						return;
					}
					superio_write(addr, val);
					return;
				}
				if ((addr & 0xfff0000) == 0x33a0000) {
					/* Econet? */
					return;
				}
				break;
			case 4:
				/* Podule space 0, 1, 2, 3 */
				podules_write16((addr >> 14) & 3, PODULE_IO_TYPE_IOC, addr & 0x3fff, val >> 16);
				break;
			case 7:
				/* Podule space 4, 5, 6, 7 */
				podules_write16(((addr >> 14) & 3) + 4, PODULE_IO_TYPE_IOC, addr & 0x3fff, val >> 16);
				break;
			}
		}
		if ((addr & 0xc00000) == 0x400000) {
			/* VIDC20 */
			writevidc20(val);
			return;
		}
		if ((machine.model == Model_Phoebe) && (addr & 0xcffffc) == 0x8007c0) {
			writeidew(val);
			return;
		}
		break;

	case 0x08000000: /* EASI space */
	case 0x09000000:
	case 0x0a000000:
	case 0x0b000000:
	case 0x0c000000:
	case 0x0d000000:
	case 0x0e000000:
	case 0x0f000000:
		podules_write32((addr >> 24) & 7, PODULE_IO_TYPE_EASI, addr & 0xffffff, val);
		return;

	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
		ram00[(addr & mem_rammask) >> 2] = val;
		/* In 0MB VRAM modes allow up to 4MB of writes to DRAM video data to update the dirty buffer */
		if ((mem_vrammask == 0) && ((addr & 0xffc00000) == (iomd.vidstart & 0xffc00000))) {
			dirtybuffer[(addr & mem_rammask) >> 12] = 1;
		}
		return;

	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
		ram01[(addr & mem_rammask) >> 2] = val;
		return;

	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ram1 != NULL) {
			ram1[(addr & 0x7ffffff) >> 2] = val;
		}
		return;

	case 0x20000000: /* Kinetic SDRAM bank 0 */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000: /* 128MB bank aliases on undecoded A27 */
	case 0x29000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdram0 != NULL) {
			sdram0[(addr & SDRAM_BANK_MASK) >> 2] = val;
		}
		return;

	case 0x30000000: /* Kinetic SDRAM bank 1 */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000: /* 128MB bank aliases on undecoded A27 */
	case 0x39000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdram1 != NULL) {
			sdram1[(addr & SDRAM_BANK_MASK) >> 2] = val;
		}
		return;
	}
}

/**
 * Write a byte to a physical address.
 *
 * @param addr Physical address
 * @param val  Byte to write
 */
static void
mem_phys_write8(uint32_t addr, uint8_t val)
{
	addr &= phys_space_mask;

	switch (addr & (phys_space_mask & 0xff000000)) { /* Select in 16MB chunks */
	case 0x02000000: /* VRAM */
		if (mem_vrammask == 0)
			return;
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		vramb[addr & mem_vrammask] = val;
		dirtybuffer[(addr & mem_vrammask) >> 12] = 1;
		return;

	case 0x03000000: /* IO */
		if ((addr & 0xc00000) == 0) {
			uint32_t bank = (addr >> 16) & 7;

			switch (bank) {
			case 0:
				iomd_write(addr, val);
				return;
			case 1:
			case 2:
				if (addr == 0x3310000)
					return;
				if ((addr & 0xfc0000) == 0x240000)
					return;
				if (addr >= 0x3012000 && addr <= 0x302a000) {
					fdc_dma_write(addr, val);
					return;
				}
				if (addr >= 0x3010000 && addr < 0x3012000) {
					/* SuperIO */
					superio_write_byte(addr, val);
					return;
				}
				if ((addr & 0xfff0000) == 0x33a0000) {
					/* Econet? */
					return;
				}
				break;
			case 4:
				/* Podule space 0, 1, 2, 3 */
				podules_write8((addr >> 14) & 3, PODULE_IO_TYPE_IOC, addr & 0x3fff, val);
				break;
			case 7:
				/* Podule space 4, 5, 6, 7 */
				podules_write8(((addr >> 14) & 3) + 4, PODULE_IO_TYPE_IOC, addr & 0x3fff, val);
				break;
			}
		}
		if ((machine.model == Model_Phoebe) && (addr & 0xcff000) == 0x800000) {
			writeide((addr >> 2) & 0x3ff, val);
			return;
		}
		break;

	case 0x08000000: /* EASI space */
	case 0x09000000:
	case 0x0a000000:
	case 0x0b000000:
	case 0x0c000000:
	case 0x0d000000:
	case 0x0e000000:
	case 0x0f000000:
		podules_write8((addr >> 24) & 7, PODULE_IO_TYPE_EASI, addr & 0xffffff, val);
		return;

	case 0x10000000: /* SIMM 0 bank 0 */
	case 0x11000000:
	case 0x12000000:
	case 0x13000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		ramb00[addr & mem_rammask] = val;
		/* In 0MB VRAM modes allow up to 4MB of writes to DRAM video data to update the dirty buffer */
		if ((mem_vrammask == 0) && ((addr & 0xffc00000) == (iomd.vidstart & 0xffc00000))) {
			dirtybuffer[(addr & mem_rammask) >> 12] = 1;
		}
		return;

	case 0x14000000: /* SIMM 0 bank 1 */
	case 0x15000000:
	case 0x16000000:
	case 0x17000000:
#ifdef _RPCEMU_BIG_ENDIAN
		addr ^= 3;
#endif
		ramb01[addr & mem_rammask] = val;
		return;

	case 0x18000000: /* SIMM 1 bank 0 */
	case 0x19000000:
	case 0x1a000000:
	case 0x1b000000:
	case 0x1c000000: /* SIMM 1 bank 1 */
	case 0x1d000000:
	case 0x1e000000:
	case 0x1f000000:
		if (ramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			ramb1[addr & 0x7ffffff] = val;
		}
		return;

	case 0x20000000: /* Kinetic SDRAM bank 0 */
	case 0x21000000:
	case 0x22000000:
	case 0x23000000:
	case 0x24000000:
	case 0x25000000:
	case 0x26000000:
	case 0x27000000:
	case 0x28000000: /* 128MB bank aliases on undecoded A27 */
	case 0x29000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb0 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			sdramb0[addr & SDRAM_BANK_MASK] = val;
		}
		return;

	case 0x30000000: /* Kinetic SDRAM bank 1 */
	case 0x31000000:
	case 0x32000000:
	case 0x33000000:
	case 0x34000000:
	case 0x35000000:
	case 0x36000000:
	case 0x37000000:
	case 0x38000000: /* 128MB bank aliases on undecoded A27 */
	case 0x39000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
	case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
		if (sdramb1 != NULL) {
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			sdramb1[addr & SDRAM_BANK_MASK] = val;
		}
		return;
	}
}

uint32_t
readmemfl(uint32_t addr)
{
	const uint32_t virt_addr = addr;
	uint32_t phys_addr = addr;
	uint32_t value = 0;

	if (mmu) {
		if ((addr >> 12) == readmemcache) {
			phys_addr = readmemcache2 + (addr & 0xfff);
		} else {
			readmemcache = addr >> 12;
			phys_addr = translateaddress(addr, 0, 0);
			if (arm.event & 0x40) {
				vraddrl[addr >> 12] = readmemcache = 0xffffffff;
				value = 0;
				goto out;
			}
			readmemcache2 = phys_addr & 0xfffff000;
		}
		switch (readmemcache2 & (phys_space_mask & 0xff000000)) {
		case 0x00000000: /* ROM */
			vradd(addr, &rom[((readmemcache2 & 0x7ff000) - (uintptr_t) (addr & ~0xfffu)) >> 2], 2, readmemcache2);
			value = *(const uint32_t *) ((vraddrl[addr >> 12] & ~3) + (addr & ~3u));
			goto out;

		case 0x02000000: /* VRAM */
			if (mem_vrammask != 0) {
				vradd(addr, &vram[((readmemcache2 & mem_vrammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
				value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
				goto out;
			}
			break;

		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			vradd(addr, &ram00[((readmemcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
			value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
			goto out;

		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			vradd(addr, &ram01[((readmemcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
			value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
			goto out;

		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			if (ram1 != NULL) {
				vradd(addr, &ram1[((readmemcache2 & 0x7ffffff) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
				value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
				goto out;
			}
			break;

		case 0x20000000: /* Kinetic SDRAM bank 0 */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000: /* 128MB bank aliases on undecoded A27 */
		case 0x29000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram0 != NULL) {
				vradd(addr, &sdram0[((readmemcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
				value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
				goto out;
			}
			break;

		case 0x30000000: /* Kinetic SDRAM bank 1 */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000: /* 128MB bank aliases on undecoded A27 */
		case 0x39000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram1 != NULL) {
				vradd(addr, &sdram1[((readmemcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
				value = *(const uint32_t *) (vraddrl[addr >> 12] + (addr & ~3u));
				goto out;
			}
			break;
		}
	} else {
		switch (addr & (phys_space_mask & 0xff000000)) {
		case 0x00000000: /* ROM */
			//vradd(addr, &rom[((addr & 0x7ff000) - (uintptr_t) (addr & ~0xfffu)) >> 2], 2, addr);
			break;
		case 0x02000000: /* VRAM */
			if (mem_vrammask != 0) {
				vradd(addr, &vram[((addr & mem_vrammask & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			}
			break;
		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			vradd(addr, &ram00[((addr & mem_rammask & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			break;
		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			vradd(addr, &ram01[((addr & mem_rammask & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			break;
		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			if (ram1 != NULL) {
				vradd(addr, &ram1[((addr & 0x7ffffff & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			}
			break;

		case 0x20000000: /* Kinetic SDRAM bank 0 */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000: /* 128MB bank aliases on undecoded A27 */
		case 0x29000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram0 != NULL) {
				vradd(addr, &sdram0[((addr & SDRAM_BANK_MASK & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			}
			break;

		case 0x30000000: /* Kinetic SDRAM bank 1 */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000: /* 128MB bank aliases on undecoded A27 */
		case 0x39000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram1 != NULL) {
				vradd(addr, &sdram1[((addr & SDRAM_BANK_MASK & ~0xfffu) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, addr);
			}
			break;
		}
	}

	value = mem_phys_read32(phys_addr);


out:
	debugger_memory_access(virt_addr, 4, 0, value);
	return value;
}


uint32_t
readmemfb(uint32_t addr)
{
	const uint32_t virt_addr = addr;
	uint32_t phys_addr = addr;
	uint32_t value = 0;

	if (mmu) {
		if ((addr >> 12) == readmemcache) {
			phys_addr = readmemcache2 + (addr & 0xfff);
		} else {
			readmemcache = addr >> 12;
			phys_addr = translateaddress(addr, 0, 0);
			if (arm.event & 0x40) {
				readmemcache = 0xffffffff;
				value = 0;
				goto out;
			}
			readmemcache2 = phys_addr & 0xfffff000;
		}
		switch (readmemcache2 & (phys_space_mask & 0xff000000)) {
		case 0x00000000: /* ROM */
			vradd(addr, &rom[((readmemcache2 & 0x7ff000) - (uintptr_t) (addr & ~0xfffu)) >> 2], 2, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			value = *(const uint8_t *) ((vraddrl[addr >> 12] & ~3) + addr);
			goto out;

		case 0x02000000: /* VRAM */
			if (mem_vrammask != 0) {
				vradd(addr, &vram[((readmemcache2 & mem_vrammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
				addr ^= 3;
#endif
				value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
				goto out;
			}
			break;

		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			vradd(addr, &ram00[((readmemcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
			goto out;

		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			vradd(addr, &ram01[((readmemcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
			addr ^= 3;
#endif
			value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
			goto out;

		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			if (ram1 != NULL) {
				vradd(addr, &ram1[((readmemcache2 & 0x7ffffff) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
				addr ^= 3;
#endif
				value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
				goto out;
			}
			break;

		case 0x20000000: /* Kinetic SDRAM bank 0 */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000: /* 128MB bank aliases on undecoded A27 */
		case 0x29000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram0 != NULL) {
				vradd(addr, &sdram0[((readmemcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
				addr ^= 3;
#endif
				value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
				goto out;
			}
			break;

		case 0x30000000: /* Kinetic SDRAM bank 1 */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000: /* 128MB bank aliases on undecoded A27 */
		case 0x39000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram1 != NULL) {
				vradd(addr, &sdram1[((readmemcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, readmemcache2);
#ifdef _RPCEMU_BIG_ENDIAN
				addr ^= 3;
#endif
				value = *(const uint8_t *) (vraddrl[addr >> 12] + addr);
				goto out;
			}
			break;
		}
	}

	value = mem_phys_read8(phys_addr);

out:
	debugger_memory_access(virt_addr, 1, 0, value);
	return value;
}

void
writememfl(uint32_t addr, uint32_t val)
{
	const uint32_t virt_addr = addr;
	uint32_t phys_addr = addr;

	if (mmu) {
		if ((addr >> 12) == writememcache) {
			phys_addr = writememcache2 + (addr & 0xfff);
		} else {
			writememcache = addr >> 12;
			phys_addr = translateaddress(addr, 1, 0);
			if (arm.event & 0x40) {
				writememcache = 0xffffffff;
				return;
			}
			writememcache2 = phys_addr & 0xfffff000;
		}
		switch (writememcache2 & (phys_space_mask & 0xff000000)) {
		case 0x02000000: /* VRAM */
			if (mem_vrammask != 0) {
				vwadd(addr, &vram[((writememcache2 & mem_vrammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			}
			break;

		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			vwadd(addr, &ram00[((writememcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			break;

		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			vwadd(addr, &ram01[((writememcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			break;

		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			if (ram1 != NULL) {
				vwadd(addr, &ram1[((writememcache2 & 0x7ffffff) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			}
			break;

		case 0x20000000: /* Kinetic SDRAM bank 0 */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000: /* 128MB bank aliases on undecoded A27 */
		case 0x29000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram0 != NULL) {
				vwadd(addr, &sdram0[((writememcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			}
			break;

		case 0x30000000: /* Kinetic SDRAM bank 1 */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000: /* 128MB bank aliases on undecoded A27 */
		case 0x39000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram1 != NULL) {
				vwadd(addr, &sdram1[((writememcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writememcache2);
			}
			break;
		}
	}
	mem_phys_write32(phys_addr, val);
	debugger_memory_access(virt_addr, 4, 1, val);
}

void
writememfb(uint32_t addr, uint8_t val)
{
	const uint32_t virt_addr = addr;
	uint32_t phys_addr = addr;

	if (mmu) {
		if ((addr >> 12) == writemembcache) {
			phys_addr = writemembcache2 + (addr & 0xfff);
		} else {
			writemembcache = addr >> 12;
			phys_addr = translateaddress(addr, 1, 0);
			if (arm.event & 0x40) {
				writemembcache = 0xffffffff;
				return;
			}
			writemembcache2 = phys_addr & 0xfffff000;
		}
		switch (writemembcache2 & (phys_space_mask & 0xff000000)) {
		case 0x02000000: /* VRAM */
			if (mem_vrammask != 0) {
				vwadd(addr, &vram[((writemembcache2 & mem_vrammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			}
			break;

		case 0x10000000: /* SIMM 0 bank 0 */
		case 0x11000000:
		case 0x12000000:
		case 0x13000000:
			vwadd(addr, &ram00[((writemembcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			break;

		case 0x14000000: /* SIMM 0 bank 1 */
		case 0x15000000:
		case 0x16000000:
		case 0x17000000:
			vwadd(addr, &ram01[((writemembcache2 & mem_rammask) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			break;

		case 0x18000000: /* SIMM 1 bank 0 */
		case 0x19000000:
		case 0x1a000000:
		case 0x1b000000:
		case 0x1c000000: /* SIMM 1 bank 1 */
		case 0x1d000000:
		case 0x1e000000:
		case 0x1f000000:
			if (ram1 != NULL) {
				vwadd(addr, &ram1[((writemembcache2 & 0x7ffffff) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			}
			break;

		case 0x20000000: /* Kinetic SDRAM bank 0 */
		case 0x21000000:
		case 0x22000000:
		case 0x23000000:
		case 0x24000000:
		case 0x25000000:
		case 0x26000000:
		case 0x27000000:
		case 0x28000000: /* 128MB bank aliases on undecoded A27 */
		case 0x29000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x2f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram0 != NULL) {
				vwadd(addr, &sdram0[((writemembcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			}
			break;

		case 0x30000000: /* Kinetic SDRAM bank 1 */
		case 0x31000000:
		case 0x32000000:
		case 0x33000000:
		case 0x34000000:
		case 0x35000000:
		case 0x36000000:
		case 0x37000000:
		case 0x38000000: /* 128MB bank aliases on undecoded A27 */
		case 0x39000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3a000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3b000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3c000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3d000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3e000000: /* 128MB bank aliases on undecoded A27 */
		case 0x3f000000: /* 128MB bank aliases on undecoded A27 */
			if (sdram1 != NULL) {
				vwadd(addr, &sdram1[((writemembcache2 & SDRAM_BANK_MASK) - (uintptr_t) (addr & ~0xfffu)) >> 2], 0, writemembcache2);
			}
			break;
		}
	}
	mem_phys_write8(phys_addr, val);
	debugger_memory_access(virt_addr, 1, 1, val);
}

/**
 * Write the RAM and VRAM contents to a suspend snapshot.
 *
 * The ROM is not stored; the resume path reloads it from disk and the
 * snapshot header carries a checksum to ensure it is the same image.
 *
 * Up to five RAM regions can be present: SIMM 0 banks 0 and 1 (always),
 * the second 128MB SIMM (256MB configs), and the two 128MB Kinetic on-card
 * SDRAM banks (Kinetic configs above 256MB). Each region is stored with a
 * size word so it can be validated on resume, then RLE-compressed (guest
 * RAM is mostly zero). The 16MB VRAM buffer is stored at its active size
 * (mem_vrammask + 1).
 */
void
mem_savestate(FILE *f)
{
	uint32_t bank_size = mem_rammask + 1;
	uint32_t ram1_size = (ram1 != NULL) ? (128 * 1024 * 1024) : 0;
	uint32_t sdram_size = (sdram0 != NULL) ? SDRAM_BANK_SIZE : 0;
	uint32_t vram_size = (mem_vrammask != 0) ? (mem_vrammask + 1) : 0;

	savestate_write_u32(f, bank_size);
	savestate_write_rle(f, ram00, bank_size);
	savestate_write_rle(f, ram01, bank_size);

	savestate_write_u32(f, ram1_size);
	if (ram1_size != 0) {
		savestate_write_rle(f, ram1, ram1_size);
	}

	savestate_write_u32(f, sdram_size);
	if (sdram_size != 0) {
		savestate_write_rle(f, sdram0, sdram_size);
		savestate_write_rle(f, sdram1, sdram_size);
	}

	savestate_write_u32(f, vram_size);
	if (vram_size != 0) {
		savestate_write_rle(f, vram, vram_size);
	}
}

/**
 * Restore the RAM and VRAM contents from a suspend snapshot.
 *
 * The arrays have already been allocated at their configured sizes by
 * mem_reset(); the snapshot header guarantees the machine model and RAM/VRAM
 * configuration match, so the bank presence and sizes must agree.
 */
void
mem_loadstate(FILE *f)
{
	uint32_t bank_size = savestate_read_u32(f);
	uint32_t ram1_size, sdram_size, vram_size;

	if (bank_size != mem_rammask + 1) {
		rpclog("mem_loadstate: RAM bank size mismatch\n");
		savestate_error = 1;
		return;
	}
	savestate_read_rle(f, ram00, bank_size);
	savestate_read_rle(f, ram01, bank_size);

	ram1_size = savestate_read_u32(f);
	if (ram1_size != ((ram1 != NULL) ? (128 * 1024 * 1024) : 0)) {
		rpclog("mem_loadstate: SIMM 1 size mismatch\n");
		savestate_error = 1;
		return;
	}
	if (ram1_size != 0) {
		savestate_read_rle(f, ram1, ram1_size);
	}

	sdram_size = savestate_read_u32(f);
	if (sdram_size != ((sdram0 != NULL) ? SDRAM_BANK_SIZE : 0)) {
		rpclog("mem_loadstate: Kinetic SDRAM size mismatch\n");
		savestate_error = 1;
		return;
	}
	if (sdram_size != 0) {
		savestate_read_rle(f, sdram0, sdram_size);
		savestate_read_rle(f, sdram1, sdram_size);
	}

	vram_size = savestate_read_u32(f);
	if (vram_size != ((mem_vrammask != 0) ? (mem_vrammask + 1) : 0)) {
		rpclog("mem_loadstate: VRAM size mismatch\n");
		savestate_error = 1;
		return;
	}
	if (vram_size != 0) {
		savestate_read_rle(f, vram, vram_size);
	}

	clearmemcache();
}
