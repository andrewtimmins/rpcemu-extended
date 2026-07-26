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
     0x080000 - 0xffffff   framestore, 15.5MB

   The ROM has to start at offset zero, because that is where RISC OS looks for
   the identity bytes. The framestore starts far enough above it to leave room
   for the ROM to grow, and what is left still holds 2560x1440 at 32bpp (14.06MB)
   with room to spare - well beyond anything VIDC20 can reach from VRAM.

   Registers are in the card's ROM/IOC space rather than EASI, because a module
   loaded from an expansion card's ROM is entered with that address in R11. The
   driver therefore needs no SWI, and no logical mapping, to find them. */
#define GFXCARD_EASI_SIZE	0x01000000u	/* 16MB, one EASI slot */
#define GFXCARD_ROM_WINDOW	0x00040000u	/* EASI bytes the ROM answers */
#define GFXCARD_FB_OFFSET	0x00080000u	/* framestore start within EASI */
#define GFXCARD_FB_SIZE		(GFXCARD_EASI_SIZE - GFXCARD_FB_OFFSET)

/* Largest mode the card will accept, which is what its framestore can hold at
   32bpp with a little headroom. Kept here so the guest driver and the emulator
   agree without either having to derive it. */
#define GFXCARD_MAX_WIDTH	2560u
#define GFXCARD_MAX_HEIGHT	1440u

/* ------------------------------------------------------------------------
 * Register map (card ROM/IOC space, 16-bit registers)
 *
 * Sixteen bits because expansion card space is byte and half-word only on this
 * machine (mem.c reaches it through podules_read8 / podules_read16). Values
 * wider than that are split into _LO and _HI halves; none of them are written
 * often enough for that to matter, a mode change being the busiest case.
 * ------------------------------------------------------------------------ */

#define GFXCARD_REG_ID_LO	0x00	/* R  "Gx" - together with ID_HI, "RPGx" */
#define GFXCARD_REG_ID_HI	0x02	/* R  "RP" */
#define GFXCARD_REG_VERSION	0x04	/* R  interface version */
#define GFXCARD_REG_CAPS	0x06	/* R  GFXCARD_CAP_* */
#define GFXCARD_REG_FB_PHYS_LO	0x08	/* R  framestore physical base */
#define GFXCARD_REG_FB_PHYS_HI	0x0a
#define GFXCARD_REG_FB_SIZE_LO	0x0c	/* R  framestore size in bytes */
#define GFXCARD_REG_FB_SIZE_HI	0x0e
#define GFXCARD_REG_CTRL	0x10	/* RW GFXCARD_CTRL_* */
#define GFXCARD_REG_STATUS	0x12	/* RW GFXCARD_STATUS_*, write to clear */
#define GFXCARD_REG_WIDTH	0x14	/* RW active pixels across */
#define GFXCARD_REG_HEIGHT	0x16	/* RW active lines */
#define GFXCARD_REG_BPP		0x18	/* RW bits per pixel: 8, 16 or 32 */
#define GFXCARD_REG_STRIDE_LO	0x1a	/* RW bytes per line */
#define GFXCARD_REG_STRIDE_HI	0x1c
#define GFXCARD_REG_START_LO	0x1e	/* RW display start, bytes into store */
#define GFXCARD_REG_START_HI	0x20
#define GFXCARD_REG_PAL_INDEX	0x22	/* RW palette entry to write */
#define GFXCARD_REG_PAL_LO	0x24	/* RW &BBGGRRSS, low half */
#define GFXCARD_REG_PAL_HI	0x26	/* RW high half; writing commits the entry */
#define GFXCARD_REG_MAX_WIDTH	0x28	/* R  largest mode the card accepts */
#define GFXCARD_REG_MAX_HEIGHT	0x2a
#define GFXCARD_REG_LAST	0x2b

/* ID_HI/ID_LO read as "RP" and "Gx": a driver can identify the card without
   knowing anything else about it. */
#define GFXCARD_ID_HI		0x5250u		/* "RP" */
#define GFXCARD_ID_LO		0x4778u		/* "Gx" */
#define GFXCARD_VERSION		1u

/* Capabilities. Depths the card can scan out, and the extras it offers. The
   driver passes these on through GraphicsV's own feature and pixel-format
   calls, so a capability the card does not claim is one the OS will do in
   software instead. */
#define GFXCARD_CAP_8BPP	0x0001u
#define GFXCARD_CAP_16BPP	0x0002u
#define GFXCARD_CAP_32BPP	0x0004u
#define GFXCARD_CAP_HW_SCROLL	0x0100u		/* display start register works */
#define GFXCARD_CAP_VSYNC	0x0200u		/* raises a vsync interrupt */

#define GFXCARD_CTRL_ENABLE	0x0001u		/* scan out from the framestore */
#define GFXCARD_CTRL_BLANK	0x0002u		/* output blanked */
#define GFXCARD_CTRL_VSYNC_IRQ	0x0004u		/* interrupt on vsync */

#define GFXCARD_STATUS_VSYNC	0x0001u		/* vsync since last cleared */

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
