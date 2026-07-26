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

#ifndef GFXCARD_H
#define GFXCARD_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------
 * Card geometry
 * ------------------------------------------------------------------------ */

/* The card is an ordinary expansion card in an ordinary EASI slot, which gives
   it 16MB of address space (see the 0x08000000-0x0f000000 decode in mem.c).
   Nothing here needs a fabricated address window.

   Its EASI space is laid out as:

     0x000000 - 0x03ffff   expansion card ROM, read a byte per word as the other
                           cards here are, so 64KB of ROM
     0x080000 - 0x0800ff   registers, 32-bit, one word each
     0x100000 - 0xffffff   framestore, 15MB

   The ROM has to start at offset zero, because that is where RISC OS looks for
   the identity bytes. The framestore starts at a megabyte boundary because that
   is how the kernel maps a card-hosted framestore: it rounds the address down
   and the size up to megabytes (see ModeChangeSub in the RISC OS kernel), so an
   unaligned base would pull the ROM and register windows into the screen
   mapping. What is left still holds 2560x1440 at 32bpp (14.06MB) with room to
   spare - well beyond anything VIDC20 can reach from VRAM.

   The registers are in EASI space rather than the card's IOC space because EASI
   is a 32-bit bus: a register is one word, read and written as one word. The
   IOC side of an expansion card here is 16 bits wide and word-addressed, with
   write data taken from the top half of the word, so every value wider than 16
   bits would have to be split and every write shifted. The driver finds this
   window through the EASI address the Podule manager reports for the slot.
   Only the interrupt status byte remains in IOC space, where the identity bytes
   promise it. */
#define GFXCARD_EASI_SIZE	0x01000000u	/* 16MB, one EASI slot */
#define GFXCARD_ROM_WINDOW	0x00040000u	/* EASI bytes the ROM answers */
#define GFXCARD_REG_BASE	0x00080000u	/* registers start here within EASI */
#define GFXCARD_FB_OFFSET	0x00100000u	/* framestore start within EASI */
#define GFXCARD_FB_SIZE		(GFXCARD_EASI_SIZE - GFXCARD_FB_OFFSET)

/* Largest mode the card will accept, which is what its framestore can hold at
   32bpp with a little headroom. Kept here so the guest driver and the emulator
   agree without either having to derive it. */
#define GFXCARD_MAX_WIDTH	2560u
#define GFXCARD_MAX_HEIGHT	1440u

/* ------------------------------------------------------------------------
 * Register map (EASI space, 32-bit registers)
 *
 * The numbers below are register indices: register n is at EASI offset
 * GFXCARD_REG_BASE + n * 4 and is read and written as a word. Word access only;
 * a byte or half-word access to this window is not a register access.
 * ------------------------------------------------------------------------ */

#define GFXCARD_REG_ID		0	/* R  "RPGx" */
#define GFXCARD_REG_VERSION	1	/* R  interface version */
#define GFXCARD_REG_CAPS	2	/* R  GFXCARD_CAP_* */
#define GFXCARD_REG_FB_PHYS	3	/* R  framestore physical base */
#define GFXCARD_REG_FB_SIZE	4	/* R  framestore size in bytes */
#define GFXCARD_REG_CTRL	5	/* RW GFXCARD_CTRL_* */
#define GFXCARD_REG_STATUS	6	/* RW GFXCARD_STATUS_*, write to clear */
#define GFXCARD_REG_WIDTH	7	/* RW active pixels across */
#define GFXCARD_REG_HEIGHT	8	/* RW active lines */
#define GFXCARD_REG_BPP		9	/* RW bits per pixel: 8 or 32 */
#define GFXCARD_REG_STRIDE	10	/* RW bytes per line */
#define GFXCARD_REG_START	11	/* RW display start, bytes into the store */
#define GFXCARD_REG_PAL_INDEX	12	/* RW palette entry the next write lands on */
#define GFXCARD_REG_PAL_ENTRY	13	/* RW &BBGGRRSS; writing steps PAL_INDEX on */
#define GFXCARD_REG_MAX_WIDTH	14	/* R  largest mode the card accepts */
#define GFXCARD_REG_MAX_HEIGHT	15
#define GFXCARD_REG_FRAMES	16	/* R  frames displayed, for diagnostics */
#define GFXCARD_REG_EDID_SIZE	17	/* R  EDID bytes the card can serve, 0 if none */
#define GFXCARD_REG_EDID_INDEX	18	/* RW byte the next EDID_DATA read returns */
#define GFXCARD_REG_EDID_DATA	19	/* R  that byte; reading steps EDID_INDEX on */
#define GFXCARD_REG_PTR_CTRL	20	/* RW bit 0: show the pointer */
#define GFXCARD_REG_PTR_X	21	/* RW pointer position, pixels */
#define GFXCARD_REG_PTR_Y	22
#define GFXCARD_REG_PTR_WIDTH	23	/* RW shape width in BYTES (2bpp, 4 per byte) */
#define GFXCARD_REG_PTR_HEIGHT	24	/* RW shape height in rows */
#define GFXCARD_REG_PTR_PHYS	25	/* RW physical address of the shape data */
#define GFXCARD_REG_PTR_PAL_IDX	26	/* RW pointer colour to write (1-3) */
#define GFXCARD_REG_PTR_PAL	27	/* RW &BBGGRRSS; writing steps the index on */
#define GFXCARD_REG_COUNT	28

/* Where each register sits in the card's EASI space. */
#define GFXCARD_REG_ADDR(n)	(GFXCARD_REG_BASE + (n) * 4)

/* ID reads as "RPGx", so a driver can identify the card without knowing
   anything else about it. */
#define GFXCARD_ID		0x52504778u	/* "RPGx" */
#define GFXCARD_VERSION		1u

/* Capabilities. Depths the card can scan out, and the extras it offers. The
   driver passes these on through GraphicsV's own feature and pixel-format
   calls, so a capability the card does not claim is one the OS will do in
   software instead. */
#define GFXCARD_CAP_8BPP	0x0001u
#define GFXCARD_CAP_16BPP	0x0002u	/* not claimed: only 8 and 32 are scanned out */
#define GFXCARD_CAP_32BPP	0x0004u
#define GFXCARD_CAP_HW_SCROLL	0x0100u		/* display start register works */
#define GFXCARD_CAP_VSYNC	0x0200u		/* raises a vsync interrupt */
#define GFXCARD_CAP_EDID	0x0400u		/* can serve the monitor's EDID */
#define GFXCARD_CAP_HW_POINTER	0x0800u		/* draws the pointer itself */

#define GFXCARD_CTRL_ENABLE	0x0001u		/* scan out from the framestore */
#define GFXCARD_CTRL_BLANK	0x0002u		/* output blanked */
#define GFXCARD_CTRL_VSYNC_IRQ	0x0004u		/* interrupt on vsync */

#define GFXCARD_STATUS_VSYNC	0x0001u		/* vsync since last cleared */

/* The pointer, as GraphicsV describes one: a 2bpp shape, four pixels to a byte,
   colour 0 transparent and 1-3 taken from the pointer palette. The shape data
   stays where RISC OS put it and the card is given its physical address, which
   is how a real card would fetch it. */
#define GFXCARD_PTR_MAX_WIDTH	32u		/* bytes: RISC OS pads to 32 pixels */
#define GFXCARD_PTR_MAX_HEIGHT	64u
#define GFXCARD_PTR_CTRL_SHOW	0x0001u

/* ------------------------------------------------------------------------
 * Emulator interface
 * ------------------------------------------------------------------------ */

/* Framestore fast path. mem.c maps EASI accesses in this range straight into the
   host buffer rather than calling through the expansion card handlers, because
   every pixel the VDU drivers write arrives that way. NULL when there is no
   card, which is what mem.c tests. */
extern uint8_t *gfxcard_fb;
extern uint32_t gfxcard_fb_phys;

/** A consistent view of the card's display state, taken once per frame. */
typedef struct {
	const uint8_t *fb;	/**< Framestore, at the current display start */
	size_t available;	/**< Bytes from there to the end of the store */
	unsigned width;		/**< Active pixels across */
	unsigned height;	/**< Active lines */
	unsigned stride;	/**< Bytes per line */
	unsigned bpp;		/**< Bits per pixel: 8, 16 or 32 */
	int blanked;		/**< Output is blanked; show nothing */

	/* The pointer, taken with the same snapshot so it cannot change halfway
	   through a frame. */
	int ptr_visible;	/**< Draw it at all */
	unsigned ptr_x;		/**< Where the card was told to put it */
	unsigned ptr_y;
	unsigned ptr_width;	/**< Shape width in bytes (4 pixels each) */
	unsigned ptr_height;	/**< Shape height in rows */
	uint32_t ptr_phys;	/**< Physical address of the shape data */
	const uint32_t *ptr_palette;	/**< 4 entries; entry 0 is transparent */
} GfxCardFrame;

/** The card's 256-entry palette, as &BBGGRRSS words. Valid for 8bpp modes. */
extern const uint32_t *gfxcard_palette(void);

/**
 * Register the card as an expansion card, if the configuration asks for one.
 * Safe to call again on reset; the framestore is allocated once and kept.
 */
extern void gfxcard_init(void);

/** Release the card. The framestore is kept for the life of the process. */
extern void gfxcard_reset(void);

/**
 * Is the card scanning out? False whenever the guest has not enabled it, which
 * is the normal state until its driver takes over.
 */
extern int gfxcard_active(void);

/**
 * Take a consistent view of the card's display state for this frame.
 *
 * @param frame Filled in on success
 * @return non-zero if the card is active and the geometry is usable
 */
extern int gfxcard_frame(GfxCardFrame *frame);

/** Tell the card a frame has been displayed, so it can raise its vsync. */
extern void gfxcard_vsync(void);

#endif /* GFXCARD_H */
