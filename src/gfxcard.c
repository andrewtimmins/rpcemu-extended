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
 * gfxcard.c - an emulated graphics expansion card
 *
 * A display whose framestore is its own memory rather than the motherboard's
 * VRAM, which is what lets it offer modes VIDC20 cannot reach. VIDC20 is limited
 * by how much VRAM is fitted: 2MB holds only 800x600 at 32bpp, and even 8MB
 * stops at 1920x1080. This card carries 15.5MB of its own, so 2560x1440 at 32bpp
 * costs the motherboard nothing.
 *
 * It is an ordinary card in an ordinary EASI slot. No fabricated address window,
 * no patched ROM, no reliance on the VRAM cap: the guest reaches it the way it
 * reaches any expansion card, and everything it needs to know is readable from
 * the card's own registers.
 *
 * The guest side is a GraphicsV driver, carried in this card's ROM the way the
 * network card carries its own driver. The register set below exists to serve
 * that interface and no other: the depths in GFXCARD_CAP_* answer GraphicsV's
 * pixel-format call, the display start register answers its hardware-scroll
 * feature bit, and so on. Anything GraphicsV does not ask for is not here.
 *
 * The approach follows the precedent set by ViewFinder, John Kortink's graphics
 * expansion card for the Acorn Risc PC, which showed that a card-hosted
 * framestore driven by its own display driver could take the machine well beyond
 * VIDC20's limits. No ViewFinder code, firmware or programming interface is used;
 * the interface here is our own, derived from the GraphicsV documentation in the
 * RISC OS sources.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "edid.h"
#include "gfxcard.h"
#include "podules.h"

/* Expansion card identity. The product type is ours; it only has to differ from
   the other cards this emulator presents. */
#define GFXCARD_PRODUCT_TYPE	0x0f00u
#define GFXCARD_ROM_MAX		0x00010000u	/* 64KB: ROM window / 4 */

/* Where the driver module is kept, alongside the other guest-side modules but in
   a directory of its own. It must not go in poduleroms/, which is scanned into
   the general-purpose expansion card: the module would then be presented twice
   and RISC OS would see duplicate copies. */
#define GFXCARD_ROM_DIR		"gfxroms/"
#define GFXCARD_DRIVER_FILE	"RPCEmuGfx,ffa"

/* Fast path for the framestore. Every pixel the VDU drivers write arrives as an
   EASI access, so mem.c maps this range straight into the host buffer instead of
   calling through the expansion card handlers for each word. */
uint8_t *gfxcard_fb = NULL;
uint32_t gfxcard_fb_phys = 0;

/* The framestore itself, kept for the life of the process (a real card's memory
   survives a reset). gfxcard_fb points at this only while a card is fitted, so
   that turning the card off in the configuration really does remove it. */
static uint8_t *gfxcard_store;

static struct {
	podule		*podule;	/**< Backplane slot, NULL if no card */
	int		slot;		/**< Slot number, -1 if none */
	uint32_t	easi_phys;	/**< Physical base of our EASI space */

	uint8_t		*rom;		/**< Card ROM, a byte per word of window */
	uint32_t	rom_size;

	/* Registers */
	uint32_t	ctrl;
	uint32_t	status;
	uint32_t	width;
	uint32_t	height;
	uint32_t	bpp;
	uint32_t	stride;
	uint32_t	start;
	uint32_t	pal_index;
	uint32_t	frames;		/**< Frames displayed, for diagnostics */
	uint32_t	edid_index;	/**< Byte the next EDID_DATA read returns */

	uint32_t	palette[256];
} gfx;

/* ------------------------------------------------------------------------
 * Card ROM
 * ------------------------------------------------------------------------ */

static uint32_t rom_chunkbase;
static uint32_t rom_filebase;

static void
gfxcard_makechunk(uint8_t type, uint32_t filebase, uint32_t size)
{
	gfx.rom[rom_chunkbase++] = type;
	gfx.rom[rom_chunkbase++] = (uint8_t) size;
	gfx.rom[rom_chunkbase++] = (uint8_t) (size >> 8);
	gfx.rom[rom_chunkbase++] = (uint8_t) (size >> 16);

	gfx.rom[rom_chunkbase++] = (uint8_t) filebase;
	gfx.rom[rom_chunkbase++] = (uint8_t) (filebase >> 8);
	gfx.rom[rom_chunkbase++] = (uint8_t) (filebase >> 16);
	gfx.rom[rom_chunkbase++] = (uint8_t) (filebase >> 24);
}

/**
 * Build the card's ROM: identity bytes, a description, and the GraphicsV driver
 * module if it can be found. Built once, since it never changes. Mirrors how the network card presents its own
 * driver, so RISC OS starts it at boot without anything being installed.
 */
static void
gfxcard_rom_init(void)
{
	static const char description[] = "RPCEmu Graphics";
	char path[512];
	FILE *f;
	size_t driver_size = 0;

	snprintf(path, sizeof(path), "%s%s%s", rpcemu_get_resourcedir(),
	         GFXCARD_ROM_DIR, GFXCARD_DRIVER_FILE);

	f = fopen(path, "rb");
	if (f != NULL) {
		long len;

		fseek(f, 0, SEEK_END);
		len = ftell(f);
		if (len > 0 && (uint32_t) len <= GFXCARD_ROM_MAX / 2) {
			driver_size = (size_t) len;
			rewind(f);
		} else {
			rpclog("gfxcard: driver '%s' has an unusable size (%ld)\n", path, len);
			fclose(f);
			f = NULL;
		}
	} else {
		rpclog("gfxcard: no driver module at '%s': the card will be present "
		       "but nothing will drive it\n", path);
	}

	rom_chunkbase = 0x10;
	rom_filebase = rom_chunkbase + (8 * 2) + 4;	/* room for two chunks */
	gfx.rom_size = rom_filebase + ((sizeof(description) + 3) & ~3u);
	if (driver_size != 0) {
		gfx.rom_size += ((uint32_t) driver_size + 3) & ~3u;
	}

	gfx.rom = calloc(gfx.rom_size, 1);
	if (gfx.rom == NULL) {
		fatal("gfxcard: out of memory for the card ROM");
	}

	gfx.rom[0] = 0;		/* Acorn conformant, extended identity */
	gfx.rom[1] = 3;		/* relocated interrupt status, chunks, byte access */
	gfx.rom[2] = 0;		/* mandatory */
	gfx.rom[3] = (uint8_t) GFXCARD_PRODUCT_TYPE;
	gfx.rom[4] = (uint8_t) (GFXCARD_PRODUCT_TYPE >> 8);
	gfx.rom[5] = 0;		/* manufacturer */
	gfx.rom[6] = 0;
	gfx.rom[7] = 0;
	gfx.rom[12] = 1;	/* interrupt status bit mask */

	memcpy(gfx.rom + rom_filebase, description, sizeof(description));
	gfxcard_makechunk(0xf5, rom_filebase, sizeof(description));	/* description */
	rom_filebase += (sizeof(description) + 3) & ~3u;

	if (f != NULL) {
		const size_t got = fread(gfx.rom + rom_filebase, 1, driver_size, f);

		fclose(f);
		if (got == driver_size) {
			gfxcard_makechunk(0x81, rom_filebase,
			                  (uint32_t) ((driver_size + 3) & ~3u));
			rpclog("gfxcard: loaded driver '%s' (%zu bytes) into the card ROM\n",
			       GFXCARD_DRIVER_FILE, driver_size);
		} else {
			rpclog("gfxcard: could not read the driver from '%s'\n", path);
		}
	}
}

/* ------------------------------------------------------------------------
 * Registers
 * ------------------------------------------------------------------------ */

/**
 * Which register, if any, an EASI address selects.
 *
 * Registers are one word apart and word access only, which is the only way the
 * driver reaches them.
 *
 * @param addr Offset within the card's EASI space
 * @return Register number, or -1 if the address is not a register
 */
static int
gfxcard_reg_number(uint32_t addr)
{
	if (addr < GFXCARD_REG_BASE || (addr & 3) != 0) {
		return -1;
	}
	addr = (addr - GFXCARD_REG_BASE) >> 2;

	return (addr < GFXCARD_REG_COUNT) ? (int) addr : -1;
}

static uint32_t
gfxcard_reg_read(int reg)
{
	switch (reg) {
	case GFXCARD_REG_ID:         return GFXCARD_ID;
	case GFXCARD_REG_VERSION:    return GFXCARD_VERSION;
	case GFXCARD_REG_CAPS:
		/* 16bpp is deliberately not claimed: the card would have to scan it
		   out and only 8 and 32 are implemented. A depth the card does not
		   claim is simply one the OS will not ask it for. */
		return GFXCARD_CAP_8BPP | GFXCARD_CAP_32BPP |
		       GFXCARD_CAP_HW_SCROLL | GFXCARD_CAP_VSYNC |
		       (edid_published() != NULL ? GFXCARD_CAP_EDID : 0);
	case GFXCARD_REG_FB_PHYS:    return gfxcard_fb_phys;
	case GFXCARD_REG_FB_SIZE:    return GFXCARD_FB_SIZE;
	case GFXCARD_REG_CTRL:       return gfx.ctrl;
	case GFXCARD_REG_STATUS:     return gfx.status;
	case GFXCARD_REG_WIDTH:      return gfx.width;
	case GFXCARD_REG_HEIGHT:     return gfx.height;
	case GFXCARD_REG_BPP:        return gfx.bpp;
	case GFXCARD_REG_STRIDE:     return gfx.stride;
	case GFXCARD_REG_START:      return gfx.start;
	case GFXCARD_REG_PAL_INDEX:  return gfx.pal_index;
	case GFXCARD_REG_PAL_ENTRY:  return gfx.palette[gfx.pal_index];
	case GFXCARD_REG_MAX_WIDTH:  return GFXCARD_MAX_WIDTH;
	case GFXCARD_REG_MAX_HEIGHT: return GFXCARD_MAX_HEIGHT;
	case GFXCARD_REG_FRAMES:     return gfx.frames;

	/* The monitor's EDID, as a byte at a time through an index that steps on.
	   This is what lets the card answer a DDC read: RISC OS re-reads the EDID
	   from the display driver whenever the driver changes, and a card that
	   cannot answer leaves the machine with a fallback monitor definition and
	   only the handful of modes that go with it. */
	case GFXCARD_REG_EDID_SIZE:
		return (edid_published() != NULL) ? EDID_BLOCK_SIZE : 0;

	case GFXCARD_REG_EDID_INDEX: return gfx.edid_index;

	case GFXCARD_REG_EDID_DATA: {
		const uint8_t *block = edid_published();
		uint32_t v = 0;

		if (block != NULL && gfx.edid_index < EDID_BLOCK_SIZE) {
			v = block[gfx.edid_index];
		}
		/* Steps on even past the end, so a driver reading a run of bytes
		   cannot be made to spin by a short block. */
		if (gfx.edid_index < EDID_BLOCK_SIZE) {
			gfx.edid_index++;
		}
		return v;
	}

	default:                     return 0;
	}
}

static void
gfxcard_reg_write(int reg, uint32_t val)
{
	switch (reg) {
	case GFXCARD_REG_CTRL:
		gfx.ctrl = val & (GFXCARD_CTRL_ENABLE | GFXCARD_CTRL_BLANK |
		                  GFXCARD_CTRL_VSYNC_IRQ);
		if ((gfx.ctrl & GFXCARD_CTRL_VSYNC_IRQ) == 0) {
			gfx.status &= ~GFXCARD_STATUS_VSYNC;
			if (gfx.podule != NULL) {
				podule_irq_lower(gfx.podule);
			}
		}
		break;

	case GFXCARD_REG_STATUS:
		/* Write-to-clear, so an interrupt is acknowledged by writing back the
		   bit that caused it. */
		gfx.status &= ~val;
		if ((gfx.status & GFXCARD_STATUS_VSYNC) == 0 && gfx.podule != NULL) {
			podule_irq_lower(gfx.podule);
		}
		break;

	case GFXCARD_REG_WIDTH:     gfx.width = val; break;
	case GFXCARD_REG_HEIGHT:    gfx.height = val; break;
	case GFXCARD_REG_BPP:       gfx.bpp = val; break;
	case GFXCARD_REG_STRIDE:    gfx.stride = val; break;
	case GFXCARD_REG_START:     gfx.start = val; break;
	case GFXCARD_REG_PAL_INDEX: gfx.pal_index = val & 0xff; break;

	case GFXCARD_REG_EDID_INDEX:
		gfx.edid_index = (val < EDID_BLOCK_SIZE) ? val : EDID_BLOCK_SIZE;
		break;

	case GFXCARD_REG_PAL_ENTRY:
		/* The index steps on after each entry, so a driver setting a run of
		   colours writes the index once and then one word per colour. */
		gfx.palette[gfx.pal_index] = val;
		gfx.pal_index = (gfx.pal_index + 1) & 0xff;
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------------
 * Expansion card access
 * ------------------------------------------------------------------------ */

/* The ROM, the registers and the framestore all live in EASI space, which is
   where the driver reaches them. The ROM is read a byte per word, as the other
   cards here are; the registers and the framestore are addressed directly. */
static uint8_t
gfxcard_readb(podule *p, PoduleIoType io_type, uint32_t addr)
{
	NOT_USED(p);

	if (io_type == PODULE_IO_TYPE_IOC) {
		/* Interrupt status, as the identity byte promised. */
		if ((addr & 0x3ffc) == 0) {
			return (uint8_t) (0xfa | ((p != NULL && p->irq) ? 1 : 0));
		}
		return 0xff;
	}

	if (io_type == PODULE_IO_TYPE_EASI) {
		if (addr < GFXCARD_ROM_WINDOW) {
			const uint32_t off = addr >> 2;

			return (off < gfx.rom_size) ? gfx.rom[off] : 0x00;
		}
		if (gfxcard_fb != NULL && addr >= GFXCARD_FB_OFFSET) {
			return gfxcard_fb[addr - GFXCARD_FB_OFFSET];
		}
	}

	return 0xff;
}

static uint16_t
gfxcard_readw(podule *p, PoduleIoType io_type, uint32_t addr)
{
	NOT_USED(p);


	if (io_type == PODULE_IO_TYPE_EASI && gfxcard_fb != NULL &&
	    addr >= GFXCARD_FB_OFFSET && addr + 1 < GFXCARD_EASI_SIZE)
	{
		const uint32_t off = addr - GFXCARD_FB_OFFSET;
		uint16_t v;

		memcpy(&v, &gfxcard_fb[off], sizeof(v));
		return v;
	}

	return 0xffff;
}

static uint32_t
gfxcard_readl(podule *p, PoduleIoType io_type, uint32_t addr)
{
	NOT_USED(p);

	if (io_type == PODULE_IO_TYPE_EASI) {
		const int reg = gfxcard_reg_number(addr);

		if (reg >= 0) {
			return gfxcard_reg_read(reg);
		}
		if (addr < GFXCARD_ROM_WINDOW) {
			const uint32_t off = addr >> 2;

			return (off < gfx.rom_size) ? gfx.rom[off] : 0x00;
		}
		if (gfxcard_fb != NULL && addr >= GFXCARD_FB_OFFSET &&
		    addr + 3 < GFXCARD_EASI_SIZE)
		{
			const uint32_t off = addr - GFXCARD_FB_OFFSET;
			uint32_t v;

			memcpy(&v, &gfxcard_fb[off], sizeof(v));
			return v;
		}
	}

	return 0xffffffffu;
}

static void
gfxcard_writeb(podule *p, PoduleIoType io_type, uint32_t addr, uint8_t val)
{
	NOT_USED(p);

	if (io_type == PODULE_IO_TYPE_EASI && gfxcard_fb != NULL &&
	    addr >= GFXCARD_FB_OFFSET && addr < GFXCARD_EASI_SIZE)
	{
		gfxcard_fb[addr - GFXCARD_FB_OFFSET] = val;
	}
}

static void
gfxcard_writew(podule *p, PoduleIoType io_type, uint32_t addr, uint16_t val)
{
	NOT_USED(p);

	if (io_type == PODULE_IO_TYPE_EASI && gfxcard_fb != NULL &&
	    addr >= GFXCARD_FB_OFFSET && addr + 1 < GFXCARD_EASI_SIZE)
	{
		memcpy(&gfxcard_fb[addr - GFXCARD_FB_OFFSET], &val, sizeof(val));
	}
}

static void
gfxcard_writel(podule *p, PoduleIoType io_type, uint32_t addr, uint32_t val)
{
	NOT_USED(p);

	if (io_type != PODULE_IO_TYPE_EASI) {
		return;
	}
	{
		const int reg = gfxcard_reg_number(addr);

		if (reg >= 0) {
			gfxcard_reg_write(reg, val);
			return;
		}
	}
	if (gfxcard_fb != NULL && addr >= GFXCARD_FB_OFFSET &&
	    addr + 3 < GFXCARD_EASI_SIZE)
	{
		memcpy(&gfxcard_fb[addr - GFXCARD_FB_OFFSET], &val, sizeof(val));
	}
}

static void
gfxcard_podule_reset(podule *p)
{
	NOT_USED(p);

	gfx.ctrl = 0;
	gfx.status = 0;
	gfx.width = 0;
	gfx.height = 0;
	gfx.bpp = 0;
	gfx.stride = 0;
	gfx.start = 0;
	gfx.pal_index = 0;
	gfx.frames = 0;
	gfx.edid_index = 0;
}

/* ------------------------------------------------------------------------
 * Emulator interface
 * ------------------------------------------------------------------------ */

const uint32_t *
gfxcard_palette(void)
{
	return gfx.palette;
}

void
gfxcard_init(void)
{
	unsigned i;

	/* Called on every machine reset, from resetrpc() after the expansion card
	   slots have been cleared. Any previous registration is gone by then, so
	   the card takes a slot again from scratch - and if the configuration has
	   turned it off since, it simply does not come back. */
	gfx.podule = NULL;
	gfx.slot = -1;
	gfxcard_fb = NULL;
	gfxcard_fb_phys = 0;

	if (!config.gfxcard_enabled) {
		return;
	}

	if (gfxcard_store == NULL) {
		gfxcard_store = calloc(GFXCARD_FB_SIZE, 1);
		if (gfxcard_store == NULL) {
			rpclog("gfxcard: could not allocate a %u MB framestore; "
			       "the card is not available\n",
			       (unsigned) (GFXCARD_FB_SIZE / (1024 * 1024)));
			return;
		}
	}

	if (gfx.rom == NULL) {
		gfxcard_rom_init();
		if (gfx.rom == NULL) {
			return;
		}
	}

	gfx.podule = addpodule(gfxcard_writel, gfxcard_writew, gfxcard_writeb,
	                       gfxcard_readl, gfxcard_readw, gfxcard_readb,
	                       NULL, gfxcard_podule_reset);
	if (gfx.podule == NULL) {
		rpclog("gfxcard: no free expansion card slot\n");
		return;
	}

	gfx.slot = podule_slot_number(gfx.podule);
	gfx.easi_phys = 0x08000000u + ((uint32_t) gfx.slot * 0x01000000u);
	gfxcard_fb = gfxcard_store;
	gfxcard_fb_phys = gfx.easi_phys + GFXCARD_FB_OFFSET;

	/* A sane greyscale ramp, so an 8bpp mode shows something recognisable
	   before the driver writes a palette. */
	for (i = 0; i < 256; i++) {
		gfx.palette[i] = ((uint32_t) i << 8) | ((uint32_t) i << 16) |
		                 ((uint32_t) i << 24);
	}

	gfxcard_podule_reset(gfx.podule);

	rpclog("gfxcard: graphics card in slot %d, framestore %u MB at physical "
	       "0x%08x, up to %ux%u\n",
	       gfx.slot, (unsigned) (GFXCARD_FB_SIZE / (1024 * 1024)),
	       (unsigned) gfxcard_fb_phys, GFXCARD_MAX_WIDTH, GFXCARD_MAX_HEIGHT);
}

void
gfxcard_reset(void)
{
	if (gfx.podule != NULL) {
		gfxcard_podule_reset(gfx.podule);
	}
}

int
gfxcard_active(void)
{
	return gfx.podule != NULL &&
	       gfxcard_fb != NULL &&
	       (gfx.ctrl & GFXCARD_CTRL_ENABLE) != 0 &&
	       gfx.width != 0 && gfx.height != 0 && gfx.stride != 0;
}

int
gfxcard_frame(GfxCardFrame *frame)
{
	uint64_t needed;

	if (!gfxcard_active()) {
		return 0;
	}
	if (gfx.bpp != 8 && gfx.bpp != 32) {
		return 0;
	}
	if (gfx.width == 0 || gfx.height == 0) {
		return 0;
	}
	if (gfx.width > GFXCARD_MAX_WIDTH || gfx.height > GFXCARD_MAX_HEIGHT) {
		return 0;
	}
	/* A stride narrower than a row would have the rows overlap, which is not a
	   display, and it would make the size check below meaningless. */
	if (gfx.stride < gfx.width * (gfx.bpp / 8u)) {
		return 0;
	}

	/* The last row has to end inside the framestore. Checked here rather than
	   trusted, because these registers come from the guest, and computed at 64
	   bits so a stride chosen to overflow the arithmetic cannot pass. */
	if (gfx.start >= GFXCARD_FB_SIZE) {
		return 0;
	}
	needed = (uint64_t) gfx.stride * (gfx.height - 1u) +
	         (uint64_t) gfx.width * (gfx.bpp / 8u);
	if (needed > (uint64_t) (GFXCARD_FB_SIZE - gfx.start)) {
		return 0;
	}

	frame->fb = gfxcard_fb + gfx.start;
	frame->available = GFXCARD_FB_SIZE - gfx.start;
	frame->width = gfx.width;
	frame->height = gfx.height;
	frame->stride = gfx.stride;
	frame->bpp = gfx.bpp;
	frame->blanked = (gfx.ctrl & GFXCARD_CTRL_BLANK) != 0;

	return 1;
}

void
gfxcard_vsync(void)
{
	if (gfx.podule == NULL || (gfx.ctrl & GFXCARD_CTRL_ENABLE) == 0) {
		return;
	}

	gfx.frames++;
	gfx.status |= GFXCARD_STATUS_VSYNC;

	if ((gfx.ctrl & GFXCARD_CTRL_VSYNC_IRQ) != 0) {
		podule_irq_raise(gfx.podule);
	}
}
