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
#include "savestate.h"

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

	/* Pointer */
	uint32_t	ptr_ctrl;
	uint32_t	ptr_x;
	uint32_t	ptr_y;
	uint32_t	ptr_width;	/**< Bytes per shape row */
	uint32_t	ptr_height;
	uint32_t	ptr_phys;
	uint32_t	ptr_pal_index;
	uint32_t	ptr_palette[4];

	uint32_t	palette[256];

	/* Rectangle copy and fill */
	uint32_t	render_x, render_y, render_w, render_h;
	uint32_t	render_src_x, render_src_y;
	uint32_t	render_pat_index;
	uint32_t	render_pattern[GFXCARD_PATTERN_WORDS];
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
		       GFXCARD_CAP_HW_POINTER | GFXCARD_CAP_RENDER |
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

	case GFXCARD_REG_PTR_CTRL:    return gfx.ptr_ctrl;
	case GFXCARD_REG_PTR_X:       return gfx.ptr_x;
	case GFXCARD_REG_PTR_Y:       return gfx.ptr_y;
	case GFXCARD_REG_PTR_WIDTH:   return gfx.ptr_width;
	case GFXCARD_REG_PTR_HEIGHT:  return gfx.ptr_height;
	case GFXCARD_REG_PTR_PHYS:    return gfx.ptr_phys;
	case GFXCARD_REG_PTR_PAL_IDX: return gfx.ptr_pal_index;
	case GFXCARD_REG_PTR_PAL:     return gfx.ptr_palette[gfx.ptr_pal_index & 3];

	case GFXCARD_REG_RENDER_X:     return gfx.render_x;
	case GFXCARD_REG_RENDER_Y:     return gfx.render_y;
	case GFXCARD_REG_RENDER_W:     return gfx.render_w;
	case GFXCARD_REG_RENDER_H:     return gfx.render_h;
	case GFXCARD_REG_RENDER_SRC_X: return gfx.render_src_x;
	case GFXCARD_REG_RENDER_SRC_Y: return gfx.render_src_y;
	case GFXCARD_REG_RENDER_PAT_IDX: return gfx.render_pat_index;
	case GFXCARD_REG_RENDER_PAT:   return gfx.render_pattern[gfx.render_pat_index];

	case GFXCARD_REG_OPTIONS:
		return config.gfxcard_boot_display ? GFXCARD_OPT_BOOT_DISPLAY : 0;

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

/**
 * Is the geometry of a rectangle operation one the framestore can actually hold?
 *
 * The numbers come from the guest, so nothing is taken on trust: every rectangle
 * has to sit inside the mode being displayed, and the mode itself has to be one
 * that scans out. Computed in 64 bits so that a width chosen to wrap cannot pass.
 *
 * @param x     Left edge, pixels from the left of the display
 * @param y     Top edge, pixels down from the top of the display
 * @param w     Width in pixels
 * @param h     Height in pixels
 * @return non-zero if the rectangle is wholly inside the displayed area
 */
static int
gfxcard_render_fits(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	return (uint64_t) x + w <= (uint64_t) gfx.width &&
	       (uint64_t) y + h <= (uint64_t) gfx.height;
}

/** Where a row of the displayed area starts in the framestore. */
static uint8_t *
gfxcard_render_row(uint32_t row)
{
	return gfxcard_fb + gfx.start + (size_t) row * gfx.stride;
}

/**
 * Copy a rectangle within the framestore, and fill one through the pattern.
 *
 * This is what RISC OS asks a card to do when it scrolls a window or clears one,
 * and what it otherwise does itself a word at a time over the bus. Doing it here
 * is worth having at 1920x1080, where a full-screen scroll is eight megabytes the
 * ARM no longer has to move.
 *
 * @param op GFXCARD_RENDER_COPY or GFXCARD_RENDER_FILL
 */
static void
gfxcard_render(uint32_t op)
{
	const uint32_t bytes_per_pixel = gfx.bpp / 8u;
	GfxCardFrame frame;
	uint32_t row;

	/* Only while the card is displaying something it could draw into: the mode
	   registers are what the addresses below are built from. gfxcard_frame()
	   applies every check that makes them safe. */
	if (!gfxcard_frame(&frame)) {
		return;
	}
	if (gfx.render_w == 0 || gfx.render_h == 0) {
		return;		/* nothing to do, and not an error */
	}
	if (!gfxcard_render_fits(gfx.render_x, gfx.render_y, gfx.render_w, gfx.render_h)) {
		return;
	}

	if (op == GFXCARD_RENDER_COPY) {
		const size_t bytes = (size_t) gfx.render_w * bytes_per_pixel;

		if (!gfxcard_render_fits(gfx.render_src_x, gfx.render_src_y,
		                         gfx.render_w, gfx.render_h))
		{
			return;
		}

		/* The two rectangles may overlap - scrolling a window is exactly
		   that - so the rows are walked away from the overlap, and each row
		   moved with memmove rather than memcpy. */
		if (gfx.render_src_y < gfx.render_y) {
			for (row = gfx.render_h; row-- > 0; ) {
				memmove(gfxcard_render_row(gfx.render_y + row) +
				            (size_t) gfx.render_x * bytes_per_pixel,
				        gfxcard_render_row(gfx.render_src_y + row) +
				            (size_t) gfx.render_src_x * bytes_per_pixel,
				        bytes);
			}
		} else {
			for (row = 0; row < gfx.render_h; row++) {
				memmove(gfxcard_render_row(gfx.render_y + row) +
				            (size_t) gfx.render_x * bytes_per_pixel,
				        gfxcard_render_row(gfx.render_src_y + row) +
				            (size_t) gfx.render_src_x * bytes_per_pixel,
				        bytes);
			}
		}
		return;
	}

	if (op != GFXCARD_RENDER_FILL) {
		return;
	}

	/* (destination OR ora) EOR eor, a pair per pattern row. Done a byte at a
	   time against the pattern's own byte, which keeps the horizontal phase
	   right at 8bpp (four pixels to the word) without the ends of the rectangle
	   needing a case of their own. */
	for (row = 0; row < gfx.render_h; row++) {
		const uint32_t pat = (gfx.render_y + row) & (GFXCARD_PATTERN_ROWS - 1u);
		const uint32_t ora = gfx.render_pattern[pat * 2u];
		const uint32_t eor = gfx.render_pattern[pat * 2u + 1u];
		uint8_t *line = gfxcard_render_row(gfx.render_y + row);
		size_t i;

		for (i = 0; i < (size_t) gfx.render_w * bytes_per_pixel; i++) {
			const size_t offset = (size_t) gfx.render_x * bytes_per_pixel + i;
			const unsigned shift = 8u * (unsigned) (offset & 3u);
			const uint8_t o = (uint8_t) (ora >> shift);
			const uint8_t e = (uint8_t) (eor >> shift);

			line[offset] = (uint8_t) ((line[offset] | o) ^ e);
		}
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

	case GFXCARD_REG_PTR_CTRL:   gfx.ptr_ctrl = val & GFXCARD_PTR_CTRL_SHOW; break;
	case GFXCARD_REG_PTR_X:      gfx.ptr_x = val; break;
	case GFXCARD_REG_PTR_Y:      gfx.ptr_y = val; break;
	case GFXCARD_REG_PTR_PHYS:   gfx.ptr_phys = val; break;

	/* Clamped rather than trusted: these come from the guest and the video code
	   walks the shape with them. */
	case GFXCARD_REG_PTR_WIDTH:
		gfx.ptr_width = (val <= GFXCARD_PTR_MAX_WIDTH) ? val : GFXCARD_PTR_MAX_WIDTH;
		break;

	case GFXCARD_REG_PTR_HEIGHT:
		gfx.ptr_height = (val <= GFXCARD_PTR_MAX_HEIGHT) ? val : GFXCARD_PTR_MAX_HEIGHT;
		break;

	case GFXCARD_REG_PTR_PAL_IDX:
		gfx.ptr_pal_index = val & 3;
		break;

	case GFXCARD_REG_PTR_PAL:
		/* Entry 0 is transparent and cannot be written, so a driver setting the
		   three pointer colours writes the index once and then three words. */
		if ((gfx.ptr_pal_index & 3) != 0) {
			gfx.ptr_palette[gfx.ptr_pal_index & 3] = val;
		}
		gfx.ptr_pal_index = (gfx.ptr_pal_index + 1) & 3;
		break;

	case GFXCARD_REG_RENDER_X:     gfx.render_x = val; break;
	case GFXCARD_REG_RENDER_Y:     gfx.render_y = val; break;
	case GFXCARD_REG_RENDER_W:     gfx.render_w = val; break;
	case GFXCARD_REG_RENDER_H:     gfx.render_h = val; break;
	case GFXCARD_REG_RENDER_SRC_X: gfx.render_src_x = val; break;
	case GFXCARD_REG_RENDER_SRC_Y: gfx.render_src_y = val; break;

	case GFXCARD_REG_RENDER_PAT_IDX:
		gfx.render_pat_index = val & (GFXCARD_PATTERN_WORDS - 1u);
		break;

	case GFXCARD_REG_RENDER_PAT:
		/* The index steps on after each word, so the driver writes it once
		   and then the pattern, exactly as it does for the palette. */
		gfx.render_pattern[gfx.render_pat_index] = val;
		gfx.render_pat_index = (gfx.render_pat_index + 1u) &
		                       (GFXCARD_PATTERN_WORDS - 1u);
		break;

	case GFXCARD_REG_RENDER_OP:
		gfxcard_render(val);
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

	gfx.ptr_ctrl = 0;
	gfx.ptr_x = 0;
	gfx.ptr_y = 0;
	gfx.ptr_width = 0;
	gfx.ptr_height = 0;
	gfx.ptr_phys = 0;
	gfx.ptr_pal_index = 1;

	gfx.render_x = 0;
	gfx.render_y = 0;
	gfx.render_w = 0;
	gfx.render_h = 0;
	gfx.render_src_x = 0;
	gfx.render_src_y = 0;
	gfx.render_pat_index = 0;
	memset(gfx.render_pattern, 0, sizeof(gfx.render_pattern));

	/* A visible default, so a pointer appears even before its colours are set:
	   white with a black edge, as RISC OS's own pointer is. */
	gfx.ptr_palette[0] = 0;
	gfx.ptr_palette[1] = 0xffffff00u;	/* &BBGGRRSS: white */
	gfx.ptr_palette[2] = 0x00000000u;	/* black */
	gfx.ptr_palette[3] = 0x8080ff00u;	/* light blue, as the OS uses */
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

	/* The pointer, only if there is a shape to draw and it was placed on the
	   display. Taken in the same snapshot as the geometry so it cannot change
	   halfway through a frame. */
	frame->ptr_visible = (gfx.ptr_ctrl & GFXCARD_PTR_CTRL_SHOW) != 0 &&
	                     gfx.ptr_width != 0 && gfx.ptr_height != 0 &&
	                     gfx.ptr_phys != 0;
	frame->ptr_x = gfx.ptr_x;
	frame->ptr_y = gfx.ptr_y;
	frame->ptr_width = gfx.ptr_width;
	frame->ptr_height = gfx.ptr_height;
	frame->ptr_phys = gfx.ptr_phys;
	frame->ptr_palette = gfx.ptr_palette;

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

/* ------------------------------------------------------------------------
 * Suspend and resume
 * ------------------------------------------------------------------------ */

/* What belongs in a snapshot is the guest-visible card: its registers, both its
   palettes, and the framestore. What does not is everything the machine builds
   again for itself on reset - the slot the backplane hands out, the address that
   follows from the slot, and the ROM, which is read from disc rather than
   written by the guest.

   The framestore is run-length encoded the way VRAM is. 15MB of largely
   untouched memory costs very little that way, and a desktop that is mostly one
   colour costs little more. */

void
gfxcard_savestate(FILE *f)
{
	const int present = (gfx.podule != NULL && gfxcard_fb != NULL);
	unsigned i;

	savestate_write_u32(f, present ? 1u : 0u);
	if (!present) {
		return;
	}

	/* Recorded so that a restore can say when the card has come back somewhere
	   else. The guest's driver holds the EASI address it was given at boot, so
	   a card in a different slot leaves it talking to the wrong window. */
	savestate_write_i32(f, gfx.slot);
	savestate_write_u32(f, gfx.easi_phys);

	savestate_write_u32(f, gfx.ctrl);
	savestate_write_u32(f, gfx.status);
	savestate_write_u32(f, gfx.width);
	savestate_write_u32(f, gfx.height);
	savestate_write_u32(f, gfx.bpp);
	savestate_write_u32(f, gfx.stride);
	savestate_write_u32(f, gfx.start);
	savestate_write_u32(f, gfx.pal_index);
	savestate_write_u32(f, gfx.frames);
	savestate_write_u32(f, gfx.edid_index);

	savestate_write_u32(f, gfx.ptr_ctrl);
	savestate_write_u32(f, gfx.ptr_x);
	savestate_write_u32(f, gfx.ptr_y);
	savestate_write_u32(f, gfx.ptr_width);
	savestate_write_u32(f, gfx.ptr_height);
	savestate_write_u32(f, gfx.ptr_phys);
	savestate_write_u32(f, gfx.ptr_pal_index);
	for (i = 0; i < 4; i++) {
		savestate_write_u32(f, gfx.ptr_palette[i]);
	}

	for (i = 0; i < 256; i++) {
		savestate_write_u32(f, gfx.palette[i]);
	}

	/* The operands of a rectangle operation. Scratch between one being set up
	   and it being performed, but the guest sets them a register at a time, so a
	   snapshot can be taken part way through - and coming back with half of a
	   rectangle would draw the wrong one. */
	savestate_write_u32(f, gfx.render_x);
	savestate_write_u32(f, gfx.render_y);
	savestate_write_u32(f, gfx.render_w);
	savestate_write_u32(f, gfx.render_h);
	savestate_write_u32(f, gfx.render_src_x);
	savestate_write_u32(f, gfx.render_src_y);
	savestate_write_u32(f, gfx.render_pat_index);
	for (i = 0; i < GFXCARD_PATTERN_WORDS; i++) {
		savestate_write_u32(f, gfx.render_pattern[i]);
	}

	savestate_write_rle(f, gfxcard_fb, GFXCARD_FB_SIZE);
}

void
gfxcard_loadstate(FILE *f)
{
	uint32_t present, easi_phys;
	int32_t slot;
	unsigned i;

	present = savestate_read_u32(f);
	if (savestate_error || present == 0) {
		/* The snapshot has no card. resetrpc() has already put any card this
		   machine does have back to its reset state, which is where a machine
		   that has never had one sits as well. */
		return;
	}

	/* The snapshot has a card and this machine has not, so there is nowhere to
	   put its state. check_header() normally reports this first and more
	   helpfully; this catches the case where the card is configured but could
	   not be fitted, for want of a slot or of its driver ROM. */
	if (gfx.podule == NULL || gfxcard_fb == NULL) {
		rpclog("gfxcard: the snapshot has a graphics card but this machine "
		       "has none fitted, so its state cannot be restored\n");
		savestate_error = 1;
		return;
	}

	slot = savestate_read_i32(f);
	easi_phys = savestate_read_u32(f);
	if (slot != gfx.slot || easi_phys != gfx.easi_phys) {
		/* Not fatal - the card works, and the driver will find it again after
		   a reset - but worth saying, because it explains a display that stays
		   dark on resume. */
		rpclog("gfxcard: the snapshot has the card in slot %d at 0x%08x, this "
		       "machine has it in slot %d at 0x%08x; the guest's driver still "
		       "holds the old address\n",
		       (int) slot, (unsigned) easi_phys, gfx.slot,
		       (unsigned) gfx.easi_phys);
	}

	/* A snapshot is no more trustworthy than the guest that wrote it, and it
	   may have been edited since, so the values that gfxcard_reg_write() clamps
	   on the way in are clamped again here. The rest (geometry, stride, start)
	   is checked by gfxcard_frame() on every frame, so it needs nothing. */
	gfx.ctrl	= savestate_read_u32(f) & (GFXCARD_CTRL_ENABLE |
			                           GFXCARD_CTRL_BLANK |
			                           GFXCARD_CTRL_VSYNC_IRQ);
	gfx.status	= savestate_read_u32(f) & GFXCARD_STATUS_VSYNC;
	gfx.width	= savestate_read_u32(f);
	gfx.height	= savestate_read_u32(f);
	gfx.bpp		= savestate_read_u32(f);
	gfx.stride	= savestate_read_u32(f);
	gfx.start	= savestate_read_u32(f);
	gfx.pal_index	= savestate_read_u32(f) & 0xff;
	gfx.frames	= savestate_read_u32(f);
	gfx.edid_index	= savestate_read_u32(f);
	if (gfx.edid_index > EDID_BLOCK_SIZE) {
		gfx.edid_index = EDID_BLOCK_SIZE;
	}

	gfx.ptr_ctrl	= savestate_read_u32(f) & GFXCARD_PTR_CTRL_SHOW;
	gfx.ptr_x	= savestate_read_u32(f);
	gfx.ptr_y	= savestate_read_u32(f);
	gfx.ptr_width	= savestate_read_u32(f);
	gfx.ptr_height	= savestate_read_u32(f);
	gfx.ptr_phys	= savestate_read_u32(f);
	gfx.ptr_pal_index = savestate_read_u32(f) & 3;
	if (gfx.ptr_width > GFXCARD_PTR_MAX_WIDTH) {
		gfx.ptr_width = GFXCARD_PTR_MAX_WIDTH;
	}
	if (gfx.ptr_height > GFXCARD_PTR_MAX_HEIGHT) {
		gfx.ptr_height = GFXCARD_PTR_MAX_HEIGHT;
	}
	for (i = 0; i < 4; i++) {
		gfx.ptr_palette[i] = savestate_read_u32(f);
	}

	for (i = 0; i < 256; i++) {
		gfx.palette[i] = savestate_read_u32(f);
	}

	gfx.render_x	= savestate_read_u32(f);
	gfx.render_y	= savestate_read_u32(f);
	gfx.render_w	= savestate_read_u32(f);
	gfx.render_h	= savestate_read_u32(f);
	gfx.render_src_x = savestate_read_u32(f);
	gfx.render_src_y = savestate_read_u32(f);
	gfx.render_pat_index = savestate_read_u32(f) & (GFXCARD_PATTERN_WORDS - 1u);
	for (i = 0; i < GFXCARD_PATTERN_WORDS; i++) {
		gfx.render_pattern[i] = savestate_read_u32(f);
	}
	/* The geometry is not clamped: gfxcard_render() checks every rectangle
	   against the mode before it touches the framestore. */

	savestate_read_rle(f, gfxcard_fb, GFXCARD_FB_SIZE);

	/* The interrupt line is not one of the registers: it lives on the expansion
	   card, and nothing else in the snapshot carries it. Put it back to what the
	   restored control and status words say it should be. */
	if ((gfx.ctrl & GFXCARD_CTRL_VSYNC_IRQ) != 0 &&
	    (gfx.status & GFXCARD_STATUS_VSYNC) != 0)
	{
		podule_irq_raise(gfx.podule);
	} else {
		podule_irq_lower(gfx.podule);
	}
}
