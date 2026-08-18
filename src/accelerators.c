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
 * accelerators.c - work the host could do instead of the emulated ARM.
 *
 * See accelerators.h for what this is for. This pass decides and counts; it
 * does not act. `accel_swi()` always returns 0, so the machine behaves exactly
 * as it did without this file, and the counts say whether acting would be worth
 * the risk.
 *
 * The candidate under examination is OS_SpriteOp's scaled plot, which is how the
 * Wimp draws every icon, the Filer every entry and the Pinboard its backdrop. It
 * is a per-destination-pixel loop in the guest and a scaled copy here.
 *
 * ★ Reads of guest memory go through mem_debug_read(), not mem_read32().
 *
 * Two reasons, both of which matter. It translates without faulting, so a sprite
 * pointer the guest has got wrong cannot abort the machine from in here; and it
 * does not run the debugger's memory hooks, so watchpoints stay a record of what
 * the *guest* touched rather than firing on our inspection of it.
 */

#include <stdint.h>
#include <string.h>

#include "rpcemu.h"

#include "accelerators.h"
#include "arm.h"
#include "gfxcard.h"
#include "mem.h"
#include "vidc20.h"

/*
 * SWI numbers, as our own disassembler's table has them (src/arm_disasm.c).
 *
 * opSWI() hands us the number with the X bit still in it, because the mousehack
 * and HostFS cases beside it match the non-X forms only. Sprite plotting is
 * called both ways - the Wimp uses the X form - so the bit is masked off here.
 * That masking is not a detail: without it a whole desktop's worth of plots
 * would go uncounted and the answer would look like "nobody plots sprites".
 */
#define SWI_X_BIT		0x20000u
#define SWI_OS_PLOT		0x45u
#define SWI_OS_SPRITEOP		0x2eu
#define SWI_FONT_PAINT		0x40086u
#define SWI_WIMP_PLOTICON	0x400e2u
#define SWI_DRAW_FILL		0x40702u
#define SWI_DRAW_FILLFP		0x40703u

/* OS_SpriteOp reason codes, in the low byte of R0. */
#define SPRITE_PUT		0x22u	/* plot at the sprite's own size */
#define SPRITE_PUT_SCALED	0x34u	/* plot scaled */

/* R0 addressing bits: how R1 and R2 are to be read. */
#define SPRITE_ADDR_USER	0x100u	/* R1 -> a user sprite area */
#define SPRITE_ADDR_POINTER	0x200u	/* R2 -> the sprite itself */

/*
 * Sprite control block, from the PRM's sprite area and sprite header layout.
 * Width and height are both "less one", and width counts words per row rather
 * than pixels, so a 32bpp sprite's width in pixels is words per row.
 */
#define SPR_NEXT		0x00
#define SPR_NAME		0x04	/* 12 bytes, control-terminated */
#define SPR_WIDTH		0x10	/* words per row, less one */
#define SPR_HEIGHT		0x14	/* rows, less one */
#define SPR_LEFTBIT		0x18	/* wasted bits at the left of each row */
#define SPR_RIGHTBIT		0x1c
#define SPR_IMAGE		0x20	/* offset from the header to the pixels */
#define SPR_MASK		0x24	/* offset to the mask, or == image for none */
#define SPR_MODE		0x28	/* mode word */
#define SPR_HEADER_SIZE		0x2c	/* where the pixels start with no palette */

/* Sprite area header. */
#define AREA_COUNT		0x04
#define AREA_FIRST		0x08

/*
 * R5 is a plot action *and* flags. The action is the low three bits, 0 being
 * plain overwrite, and bit 3 asks for the sprite's transparency to be honoured.
 * The Wimp passes &18, which is overwrite with the mask used, so testing the
 * whole word against zero refuses the commonest plot on the desktop.
 *
 * ★ The bits above that change what the plot MEANS, and must not be ignored.
 *
 * SpriteExtend shifts R5 right by four and reads its own flags out of the
 * result (`SprOp`, at the store to `trns_flags2`), so:
 *
 *   bit 4       ignore the translation table - harmless, we require none
 *   bit 5       wide translation table
 *   bit 6       DITHER when reducing a depth
 *   bit 7       colour mapping
 *   bits 8-15   translucency, 0 being opaque
 *
 * Translucency is the dangerous one: a plot asking for it and drawn opaque here
 * would be plainly wrong and would look like a fault in the application. Bit 6
 * matters because it is the only thing that turns dithering on - SpriteExtend
 * initialises `dither_truecolour` to zero and sets it solely from that bit -
 * which is what makes reducing a depth reproducible here at all.
 */
#define PLOT_ACTION		7u
#define PLOT_USE_MASK		8u
#define PLOT_IGNORE_TTR		(1u << 4)
#define PLOT_UNSUPPORTED	0x0000ffc0u	/* bits 6-15: dither, map, translucency */
#define PLOT_WIDE_TRANS		(1u << 5)

/*
 * Sprite mode word.
 *
 * Bits 27-30 identify the format: 0 means the word is an old mode number, 15
 * means the RISC OS 5 word with the pixel type in bits 20-26, and anything else
 * is the RISC OS 3.5 word with the type in that nibble itself. Only the RISC OS
 * 5 word carries mode flags, in bits 8-15; the 3.5 word spends those bits on
 * DPI and has nowhere to put them.
 */
#define SPRMODE_TYPE_SHIFT	27
#define SPRMODE_RISCOS5		0xfu
#define SPRMODE_WIDE_MASK	(1u << 31)	/* mask is 8 bits per pixel */
#define SPRMODE_FAMILY		(3u << 12)	/* 0 = RGB */
#define SPRMODE_ORDER_RGB	(1u << 14)	/* set = &xRGB, clear = &xBGR */
#define SPRMODE_ALPHA		(1u << 15)	/* alpha channel in the pixels */

/*
 * Sprite pixel types, from the kernel's own list (Programmer/HdrSrc/hdr/Sprite).
 *
 * There are two 16bpp types and they are not adjacent, which is worth stating
 * because a check written for "16bpp" naturally reaches for one number: type 5
 * is 1:5:5:5 and type 10 is 5:6:5. Type 16 is 4:4:4:4, whose fourth channel is
 * alpha, so it belongs with the transparency work rather than here.
 */
#define SPRTYPE_8BPP		4
#define SPRTYPE_16BPP_1555	5
#define SPRTYPE_32BPP		6
#define SPRTYPE_16BPP_565	10
#define SPRTYPE_16BPP_4444	16

/*
 * The VDU driver workspace, where the kernel keeps what the current mode is and
 * where its output is going.
 *
 * Reading it is the only way to know whether a plot is going to the screen at
 * all: VDU output can be redirected into a sprite, and a plot done on the host
 * while that is in force would write the display instead of the sprite.
 *
 * ★ The offsets are the kernel's own, and the Display* copies are a trap.
 *
 * From the kernel's workspace header (RISC OS 5, Kernel/hdr/KernelWS): the
 * workspace begins at &1000, which that header derives and asserts is 64-byte
 * aligned, and the fields we need sit at the offsets below.
 *
 * The trap is that several of them have a second copy named Display<field>,
 * which describes the display while the plain field describes wherever output
 * currently goes. An earlier version of this file found its offsets by searching
 * the workspace for values it already knew - the mode width, the line length -
 * and matched DisplayXWindLimit and DisplayLineLength, which are equal to the
 * canonical fields exactly when output is NOT redirected. That is the one
 * circumstance we have to detect, so the search was blind to the thing it was
 * for. Hence: the kernel's offsets, checked for consistency at run time rather
 * than guessed at.
 */
/*
 * Mode flags, from the kernel's own header (Kernel/hdr/VduExt).
 *
 * ★ At 16bpp the SAME BIT means two different things depending on the depth.
 *
 * `ModeFlag_64k` and `ModeFlag_FullPalette` are one bit; at log2bpp 4 it says
 * the mode is 5:6:5 rather than 1:5:5:5. There is no other way to tell the two
 * apart, and picking wrong swaps green for half of blue in every pixel.
 */
#define MODEFLAG_64K		(1u << 7)
#define MODEFLAG_ORDER_RGB	(1u << 14)	/* set = &xRGB, clear = &xBGR */

#define VDU_WORKSPACE		0x1000u

#define VDWS_GWLCOL		0x050u	/* graphics window, pixels, inclusive */
#define VDWS_GWBROW		0x054u
#define VDWS_GWRCOL		0x058u
#define VDWS_GWTROW		0x05cu
#define VDWS_XWINDLIMIT		0x080u	/* pixels across, less one */
#define VDWS_YWINDLIMIT		0x084u	/* rows, less one */
#define VDWS_LINELENGTH		0x088u	/* bytes per row */
#define VDWS_NCOLOUR		0x08cu	/* colours less one */
#define VDWS_MODEFLAGS		0x094u	/* what kind of mode this is */
#define VDWS_XEIGFACTOR		0x098u	/* OS units to pixels, as a shift */
#define VDWS_YEIGFACTOR		0x09cu
#define VDWS_LOG2BPP		0x0a4u	/* 0..5 */
#define VDWS_SCREENSTART	0x0c0u	/* where VDU output goes */
#define VDWS_ORGX		0x0f0u	/* graphics origin, OS units */
#define VDWS_ORGY		0x0f4u

/* The workspace as it stands for one plot: read together, so nothing here can
   be half of one mode and half of another. */
typedef struct {
	int32_t gwl, gwb, gwr, gwt;	/* graphics window, pixels from bottom left */
	int32_t org_x, org_y;		/* graphics origin, OS units */
	uint32_t xwindlimit, ywindlimit;
	uint32_t linelength;
	uint32_t xeig, yeig;
	uint32_t log2bpp;
	uint32_t ncolour;
	uint32_t modeflags;
	AccelPixelFormat fmt;		/* how the screen stores a pixel */
	uint32_t screenstart;		/* logical address output goes to */
} VduWorkspace;

/* Where the guest's screen is, in our own terms. */
typedef struct {
	AcceleratorDest dest;
	unsigned width;
	unsigned height;
	unsigned stride;
	unsigned bpp;
	uint32_t phys_base;	/* Physical address of the framebuffer */
	uint32_t phys_size;

	/*
	 * The framebuffer as this process sees it, and how to tell the display it
	 * has changed. Both are needed together: our scan-out converts only the
	 * pages the guest has written, so a host-side write that does not mark them
	 * would simply never appear.
	 */
	uint8_t *host;		/* NULL where the host must not write it */
	int dirty_is_card;	/* Mark the card's record, or VIDC20's */
} ScreenTarget;

static AcceleratorStats stats;

/**
 * Read a word of guest memory, without faulting and without the debugger's
 * hooks. Answers 0 if the address is not mapped.
 */
static int
guest_read32(uint32_t addr, uint32_t *out)
{
	return mem_debug_read(addr, 4, out);
}

/*
 * ★★ Widening a colour channel: RISC OS REPLICATES, it does not shift.
 *
 * This is the one piece of arithmetic in the file that has to agree with RISC OS
 * bit for bit, and a plausible-looking alternative (`v << 3`) is wrong in a way
 * nobody would see until they compared a photograph drawn both ways: it can
 * never produce white, because 0x1f would become 0xf8.
 *
 * Taken from SpriteExtend's own code generator rather than from custom. In
 * `Video/Render/SprExtend/c/asmcore`, `get_expansion_mask()` builds a mask of
 * the bits each channel gains and the emitted code is
 *
 *     AND  r_temp1, r_pixel, #expansion_mask   ; the channels' top bits
 *     ORR  r_pixel, r_pixel, r_temp1, LSR #n   ; replicated into the new low bits
 *
 * with a hand-written second step for 565's green, whose six bits cannot share
 * the five-bit shift the other two channels use. The channel positions are the
 * table in `Sources/PutScaled`: 565 is r@0-4 g@5-10 b@11-15 and 1:5:5:5 is
 * r@0-4 g@5-9 b@10-14, both with red in the low bits.
 *
 * So each channel's own top bits become its new bottom bits, which keeps both
 * endpoints exact: 0 stays 0 and all-ones becomes 0xff.
 */
/*
 * ★ The top byte comes out ZERO, and that is not an oversight.
 *
 * A screen mode carries no alpha (the kernel only sets that flag for a sprite),
 * so SpriteExtend's destination format has bits[3] = 0 and it builds the output
 * word from the source's three channels and nothing else. Neither 565's spare
 * bit nor 1:5:5:5's T bit reaches it. Copying 0xff up there instead would look
 * identical on screen and differ from RISC OS in memory, which is exactly the
 * kind of difference that surfaces later as something else's bug.
 */
/*
 * Where each channel sits, and how many bits it has, per layout. The same
 * facts as SpriteExtend's own table in `Sources/PutScaled`, in the same order:
 * red, green, blue. Red is in the low bits of every layout here.
 */
typedef struct {
	uint8_t shift[3];	/* bit position of the channel's low bit */
	uint8_t bits[3];
} FormatChannels;

static int
format_channels(AccelPixelFormat fmt, FormatChannels *out)
{
	static const FormatChannels bgr32  = { { 0, 8, 16 }, { 8, 8, 8 } };
	static const FormatChannels f565   = { { 0, 5, 11 }, { 5, 6, 5 } };
	static const FormatChannels f1555  = { { 0, 5, 10 }, { 5, 5, 5 } };
	static const FormatChannels f4444  = { { 0, 4,  8 }, { 4, 4, 4 } };

	switch (fmt) {
	case ACCEL_FMT_32BPP:	*out = bgr32;  return 1;
	case ACCEL_FMT_565:	*out = f565;   return 1;
	case ACCEL_FMT_1555:	*out = f1555;  return 1;
	case ACCEL_FMT_4444:	*out = f4444;  return 1;
	default:		return 0;
	}
}

int
accel_can_convert(AccelPixelFormat from, AccelPixelFormat to)
{
	FormatChannels unused;

	if (from == ACCEL_FMT_NONE || to == ACCEL_FMT_NONE) {
		return 0;
	}
	/*
	 * ★ The same layout at both ends is always allowed, whatever it is.
	 *
	 * That is what lets a palette depth through. An 8bpp pixel is an index
	 * into a palette and means nothing on its own, so it can never be
	 * CONVERTED here - but copied index for index onto a screen of the same
	 * depth, with no translation table asked for, it needs no palette at all
	 * and RISC OS does the same thing. On a machine whose desktop is 8bpp
	 * that is nearly all of the work.
	 */
	if (from == to) {
		return 1;
	}
	if (!format_channels(from, &unused) || !format_channels(to, &unused)) {
		return 0;	/* one of them is a palette depth */
	}
	/* 4:4:4:4 is a source only: as a destination it has a fourth channel to
	   fill and nothing here knows what RISC OS would put in it. */
	return to != ACCEL_FMT_4444;
}

/*
 * ★ Every pair goes through 8 bits a channel, and that is EXACT, not an
 *   approximation.
 *
 * RISC OS widens by replicating a channel's top bits into the bits it gains,
 * and narrows by keeping the top bits and discarding the rest - a plain `UBFX`
 * in the generated code, no rounding. Composing "replicate up to 8" with "keep
 * the top n" reproduces both:
 *
 *   widening a -> b (a < b): replicating to 8 puts the channel at the top with
 *     copies of itself below, so the top b bits are the channel followed by its
 *     own top b-a bits, which is exactly replicate a -> b.
 *   narrowing a -> b (b < a): the top b bits of the replicated value are the
 *     top b bits of the channel, which is exactly the truncation.
 *
 * So one path serves all of them and there is no table of special cases to get
 * wrong. The same-layout case does NOT come through here: it is a straight copy
 * that keeps the spare top bit, which RISC OS also leaves alone.
 *
 * ★ Narrowing is only safe because DITHERING IS OFF unless asked for. R5 bit 6
 * is the only thing that turns it on, and a plot that sets it is refused.
 */
static uint32_t
widen_channel(uint32_t v, unsigned bits)
{
	/* Replicate upwards until eight bits are filled: 5 -> 10 -> truncated to
	   8 is the same as (v << 3) | (v >> 2), and 4 -> 8 is (v << 4) | v. */
	uint32_t r = v;
	unsigned have = bits;

	while (have < 8) {
		r = (r << bits) | v;
		have += bits;
	}
	return (r >> (have - 8)) & 0xffu;
}

uint32_t
accel_convert_pixel(AccelPixelFormat from, AccelPixelFormat to, uint32_t pixel)
{
	FormatChannels in, out;
	uint32_t result = 0;
	int i;

	if (!accel_can_convert(from, to)) {
		return 0;
	}
	if (from == to) {
		return pixel;	/* the bytes are already what the screen wants */
	}
	if (!format_channels(from, &in) || !format_channels(to, &out)) {
		return 0;
	}
	for (i = 0; i < 3; i++) {
		const uint32_t raw = (pixel >> in.shift[i]) & ((1u << in.bits[i]) - 1u);
		const uint32_t eight = widen_channel(raw, in.bits[i]);

		result |= (eight >> (8 - out.bits[i])) << out.shift[i];
	}
	return result;
}

uint32_t
accel_format_bytes(AccelPixelFormat fmt)
{
	switch (fmt) {
	case ACCEL_FMT_32BPP:	return 4;
	case ACCEL_FMT_565:
	case ACCEL_FMT_1555:
	case ACCEL_FMT_4444:	return 2;
	case ACCEL_FMT_8BPP:	return 1;
	default:		return 0;
	}
}

const char *
accel_format_name(AccelPixelFormat fmt)
{
	switch (fmt) {
	case ACCEL_FMT_32BPP:	return "32bpp &xBGR";
	case ACCEL_FMT_565:	return "16bpp 5:6:5";
	case ACCEL_FMT_1555:	return "16bpp 1:5:5:5";
	case ACCEL_FMT_4444:	return "16bpp 4:4:4:4";
	case ACCEL_FMT_8BPP:	return "8bpp, palette indices";
	default:		return "one this cannot write";
	}
}

/**
 * How the screen stores a pixel, from the mode's own flags.
 *
 * Follows `compute_pixelformat()` in SpriteExtend's c/asmcore, which is the
 * code that decides this for RISC OS itself. The 16bpp case is the awkward one:
 * `ModeFlag_64k` picks 5:6:5, and without it the colour count separates
 * 1:5:5:5 from 4:4:4:4.
 */
AccelPixelFormat
accel_mode_format(uint32_t log2bpp, uint32_t ncolour, uint32_t modeflags)
{
	if ((modeflags & MODEFLAG_ORDER_RGB) != 0) {
		return ACCEL_FMT_NONE;	/* &xRGB screen: not handled */
	}
	if (log2bpp == 5) {
		return ACCEL_FMT_32BPP;
	}
	if (log2bpp == 4) {
		if ((modeflags & MODEFLAG_64K) != 0) {
			return ACCEL_FMT_565;
		}
		if (ncolour < 4096) {
			return ACCEL_FMT_NONE;	/* 4:4:4:4, which carries alpha */
		}
		return ACCEL_FMT_1555;
	}
	if (log2bpp == 3) {
		/* Writable only from an 8bpp sprite, index for index; nothing here
		   can work out what index a colour ought to become. */
		return ACCEL_FMT_8BPP;
	}
	return ACCEL_FMT_NONE;		/* 4bpp and below: a pixel is not a byte,
					   so a clipped row would start mid-byte */
}

/** How the host will read this sprite's pixels, if it can read them at all. */
static AccelPixelFormat
source_format_for_type(int type)
{
	switch (type) {
	case SPRTYPE_32BPP:		return ACCEL_FMT_32BPP;
	case SPRTYPE_16BPP_4444:	return ACCEL_FMT_4444;
	case SPRTYPE_8BPP:		return ACCEL_FMT_8BPP;
	case SPRTYPE_16BPP_565:		return ACCEL_FMT_565;
	case SPRTYPE_16BPP_1555:	return ACCEL_FMT_1555;
	default:			return ACCEL_FMT_NONE;
	}
}

/**
 * Bits per pixel of a sprite type, or 0 for one whose depth is not fixed.
 *
 * The types with no entry are the ones the kernel's own header marks "no
 * support in OS" (CMYK, packed 24bpp, JPEG) and the YCbCr pair, whose row
 * length is not a width times a depth at all.
 */
static unsigned
sprite_type_bpp(int type)
{
	switch (type) {
	case 1:				return 1;
	case 2:				return 2;
	case 3:				return 4;
	case SPRTYPE_8BPP:		return 8;
	case SPRTYPE_16BPP_1555:	return 16;
	case SPRTYPE_32BPP:		return 32;
	case SPRTYPE_16BPP_565:		return 16;
	case SPRTYPE_16BPP_4444:	return 16;
	default:			return 0;
	}
}

/**
 * How many pixels a sprite row really holds.
 *
 * ★ The width field counts WORDS less one, not pixels.
 *
 * `width + 1` is the pixel count at 32bpp and at no other depth, which is why
 * this exists: a 16bpp sprite measured that way comes out half its real width -
 * and an 8bpp one a quarter - so the plot is drawn too narrow with the rest of
 * each row taken from the row below. The general form is the kernel's own, from
 * `readspritevars` in SprOp: the bits from the left-hand wastage to the
 * right-hand one inclusive, over the bits per pixel.
 *
 * Note this also corrects the COUNTING for every sprite shallower than 32bpp,
 * which was previously measured as though a word held one pixel. The share of
 * the drawn pixels a host-side plot can take was therefore reported against a
 * denominator that was too small.
 *
 * @return pixels per row, or 0 if the header does not describe a sensible one
 */
int
accel_sprite_row_pixels(uint32_t width_words_less_one, uint32_t lbit,
                        uint32_t rbit, unsigned bpp)
{
	int64_t bits;

	if (bpp == 0 || lbit > 31 || rbit > 31 || rbit < lbit ||
	    width_words_less_one > 0xffffu)
	{
		return 0;
	}
	bits = (int64_t) (rbit - lbit) + 1 + ((int64_t) width_words_less_one << 5);
	if (bits % (int64_t) bpp != 0) {
		return 0;	/* a part pixel: not a header this understands */
	}
	return (int) (bits / (int64_t) bpp);
}

/**
 * The sprite's pixel type, or -1 for an old-format sprite (a mode number),
 * which we decline: its depth and palette come from a mode table rather than
 * from the sprite.
 */
static int
sprite_pixel_type(uint32_t mode)
{
	const uint32_t format = (mode >> SPRMODE_TYPE_SHIFT) & 0xfu;

	if (format == 0) {
		return -1;
	}
	if (format == SPRMODE_RISCOS5) {
		return (int) ((mode >> 20) & 0x7fu);
	}
	return (int) format;
}

/**
 * Where the screen currently is, and what shape it is, from our own state
 * rather than from anything the guest has told us.
 *
 * The graphics card is asked first because when it is scanning out it *is* the
 * display, whatever VIDC20 still believes. Answering VIDC20's geometry in that
 * case would compare the VDU workspace against the wrong mode and resolve
 * nothing.
 */
static int
screen_target(ScreenTarget *target)
{
	GfxCardFrame frame;

	memset(target, 0, sizeof(*target));

	if (gfxcard_frame(&frame)) {
		target->dest = ACCEL_DEST_CARD;
		target->width = frame.width;
		target->height = frame.height;
		target->stride = frame.stride;
		target->bpp = frame.bpp;
		target->phys_base = gfxcard_fb_phys;
		target->phys_size = GFXCARD_FB_SIZE;
		target->host = gfxcard_fb;
		target->dirty_is_card = 1;
		return 1;
	}

	{
		VIDCStateSnapshot vidc;
		unsigned bpp;

		vidc_get_snapshot(&vidc);
		switch (vidc.bit8) {
		case 3: bpp = 8; break;
		case 4: bpp = 16; break;
		case 6: bpp = 32; break;
		default: bpp = 0; break;	/* 1, 2 and 4bpp: not candidates */
		}
		if (bpp == 0 || vidc.screen_width == 0 || vidc.screen_height == 0) {
			return 0;
		}

		target->dest = (mem_vrammask != 0) ? ACCEL_DEST_VRAM : ACCEL_DEST_RAM;
		target->width = vidc.screen_width;
		target->height = vidc.screen_height;
		target->stride = vidc.screen_width * (bpp / 8u);
		target->bpp = bpp;

		/*
		 * VIDC20 scans out of VRAM where it is fitted and out of main memory
		 * where it is not. Either way the physical window is what the guest's
		 * screen address has to land in, and the mask is the size of it.
		 */
		if (mem_vrammask != 0) {
			target->phys_base = 0x02000000u;
			target->phys_size = mem_vrammask + 1u;
			target->host = (uint8_t *) vram;
			target->dirty_is_card = 0;
		} else {
			/*
			 * Screen in main memory, on a machine with no VRAM. Counted, but
			 * not written from here: the physical address has to be resolved
			 * through the RAM banks and their aliases, and there is no
			 * evidence yet that anybody is running a desktop this way.
			 */
			target->phys_base = 0x10000000u;
			target->phys_size = mem_rammask + 1u;
			target->host = NULL;
			target->dirty_is_card = 0;
		}
		return 1;
	}
}

/**
 * Read the workspace for this plot, and check it describes the screen we think
 * we are looking at.
 *
 * Every field is cross-checked against our own side of the emulation: the
 * kernel's idea of the pixels across, the rows, the bytes per row and the depth
 * all have to agree with the mode the card or VIDC20 is actually scanning out,
 * and the screen address has to translate into that framebuffer. Any
 * disagreement means output is redirected, or the workspace is not what this
 * code believes, and either way there is nothing here to accelerate.
 *
 * @return 1 if the workspace was read and agrees with the display, 0 if it
 *         could not be read at all, and -1 if it says output is going somewhere
 *         other than the screen we are scanning out
 */
static int
vdu_workspace_read(const ScreenTarget *target, VduWorkspace *ws)
{
	uint32_t phys;

	if (!guest_read32(VDU_WORKSPACE + VDWS_XWINDLIMIT, &ws->xwindlimit) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_YWINDLIMIT, &ws->ywindlimit) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_LINELENGTH, &ws->linelength) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_NCOLOUR, &ws->ncolour) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_MODEFLAGS, &ws->modeflags) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_XEIGFACTOR, &ws->xeig) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_YEIGFACTOR, &ws->yeig) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_LOG2BPP, &ws->log2bpp) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_SCREENSTART, &ws->screenstart) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_GWLCOL, (uint32_t *) &ws->gwl) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_GWBROW, (uint32_t *) &ws->gwb) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_GWRCOL, (uint32_t *) &ws->gwr) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_GWTROW, (uint32_t *) &ws->gwt) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_ORGX, (uint32_t *) &ws->org_x) ||
	    !guest_read32(VDU_WORKSPACE + VDWS_ORGY, (uint32_t *) &ws->org_y))
	{
		return 0;
	}

	/* The mode the kernel is drawing into is the mode we are displaying. This
	   is what fails when output has been redirected into a sprite, which is
	   exactly when we must not touch the framebuffer. */
	if (ws->xwindlimit != target->width - 1u ||
	    ws->ywindlimit != target->height - 1u ||
	    ws->linelength != target->stride ||
	    (1u << ws->log2bpp) != target->bpp)
	{
		return -1;
	}

	/* Squarish pixels, and a shift we can use. Anything else is a mode this
	   has not been thought about in. */
	if (ws->xeig > 3 || ws->yeig > 3) {
		return -1;
	}

	/* The graphics window, inclusive both ends, within the screen. */
	if (ws->gwl < 0 || ws->gwb < 0 || ws->gwl > ws->gwr || ws->gwb > ws->gwt ||
	    ws->gwr >= (int32_t) target->width || ws->gwt >= (int32_t) target->height)
	{
		return -1;
	}

	/* And output really is going to the framebuffer we are scanning out. */
	if (!mem_debug_translate(ws->screenstart, &phys) ||
	    phys - target->phys_base >= target->phys_size)
	{
		return -1;
	}

	ws->fmt = accel_mode_format(ws->log2bpp, ws->ncolour, ws->modeflags);
	stats.screen_format = (uint32_t) ws->fmt;

	stats.vdu_ws_probed = 1;
	stats.vdu_off_xwindlimit = VDWS_XWINDLIMIT;
	stats.vdu_off_ywindlimit = VDWS_YWINDLIMIT;
	stats.vdu_off_linelength = VDWS_LINELENGTH;
	stats.vdu_off_screenstart = VDWS_SCREENSTART;
	return 1;
}

/**
 * Tell the display that rows [top, bottom] of the framebuffer have changed.
 *
 * Our scan-out only converts what has been written since the last frame, and it
 * learns that from the guest's own writes going through the memory system. A
 * write from here bypasses all of that, so this is not an optimisation to be
 * skipped: without it the plot is invisible until something else happens to
 * dirty the same pages, which looks like an emulator that randomly fails to draw.
 */
static void
mark_rows_dirty(const ScreenTarget *target, uint32_t fb_off, int top, int bottom)
{
	const uint32_t first = fb_off + (uint32_t) top * target->stride;
	const uint32_t last = fb_off + (uint32_t) bottom * target->stride +
	    target->stride - 1u;
	uint32_t off;

	for (off = first & ~0xfffu; off <= last; off += 0x1000u) {
		if (target->dirty_is_card) {
			gfxcard_mark_dirty(off);
		} else {
			dirtybuffer[((target->phys_base + off) & mem_vrammask) >> 12] = 1;
		}
		if (off + 0x1000u < off) {
			break;			/* wrapped: cannot happen, but say so */
		}
	}
}

int
accel_plot_rect(int32_t screen_w, int32_t screen_h,
                int32_t gwl, int32_t gwb, int32_t gwr, int32_t gwt,
                int32_t org_x, int32_t org_y, uint32_t xeig, uint32_t yeig,
                int32_t x_os, int32_t y_os, int32_t src_w, int32_t src_h,
                AccelPlotRect *out)
{
	/* The position in pixels: the origin is in OS units too, and the eig
	   factors are how many OS units make a pixel, as a shift. */
	const int32_t left = (x_os + org_x) >> xeig;
	const int32_t bottom = (y_os + org_y) >> yeig;

	/*
	 * Turned over once. `bottom` is rows up from the bottom of the screen and
	 * addresses the sprite's bottom row; the framebuffer wants the row its top
	 * row goes on.
	 */
	const int32_t top = screen_h - bottom - src_h;

	/* And the window's rows with it. Its edges are inclusive, so gwt is a row
	   that gets drawn, not one past the end. */
	const int32_t win_top = screen_h - 1 - gwt;
	const int32_t win_bottom = screen_h - 1 - gwb;

	out->origin_left = left;
	out->origin_top = top;
	out->left = (left > gwl) ? left : gwl;
	out->right = (left + src_w - 1 < gwr) ? left + src_w - 1 : gwr;
	out->top = (top > win_top) ? top : win_top;
	out->bottom = (top + src_h - 1 < win_bottom) ? top + src_h - 1 : win_bottom;

	/* The window has already been checked against the screen by the caller, so
	   clamping to it is clamping to the screen; this is belt and braces for a
	   caller that has not. */
	if (out->left < 0 || out->top < 0 ||
	    out->right >= screen_w || out->bottom >= screen_h)
	{
		return 0;
	}
	return (out->left <= out->right && out->top <= out->bottom);
}

void
accel_blit_row_plan(const AccelPlotRect *rect, int32_t row, uint32_t src_stride,
                    uint32_t src_bpp, uint32_t *src_offset, uint32_t *pixels)
{
	/*
	 * Sprite pixel data is stored top row first, the same order as screen
	 * memory, so the destination row maps straight onto the sprite row once the
	 * plot's unclipped top is subtracted. Clipping never moves where the sprite
	 * starts, it only narrows what is drawn, which is why origin_top and
	 * origin_left are carried through rather than recomputed from the clipped
	 * rectangle.
	 *
	 * The row's stride is a property of the sprite and not the product of its
	 * width and its depth: a 16bpp sprite of odd width pads to the next whole
	 * word, so the two differ by two bytes on every row and a picture built
	 * from the product leans further left with each row down.
	 */
	*src_offset = (uint32_t) (row - rect->origin_top) * src_stride +
	    (uint32_t) (rect->left - rect->origin_left) * src_bpp;
	*pixels = (uint32_t) (rect->right - rect->left + 1);
}

/**
 * One run of source pixels into the screen's own 32bpp words.
 *
 * The halfword is assembled from its two bytes rather than loaded as a
 * `uint16_t`: nothing promises a sprite's pixel data is even-aligned in this
 * process's address space, and an unaligned halfword load is undefined even
 * where the host would have permitted it. Written this way it also says out
 * loud which byte is the low one, which the rest of this file relies on.
 */
static void
convert_run(AccelPixelFormat from, AccelPixelFormat to, const void *in,
            void *out, uint32_t pixels)
{
	const uint8_t *src = (const uint8_t *) in;
	uint8_t *dst = (uint8_t *) out;
	const uint32_t src_bpp = accel_format_bytes(from);
	const uint32_t dst_bpp = accel_format_bytes(to);
	uint32_t i;

	if (from == to) {
		/* The commonest case on a 16bpp desktop, and the cheapest: the
		   sprite already holds exactly the bytes the screen wants. */
		memcpy(out, in, (size_t) pixels * dst_bpp);
		return;
	}

	/*
	 * Bytes in and out rather than typed loads: nothing promises a sprite's
	 * pixel data is aligned in this process's address space, and this also
	 * says out loud which byte is the low one, which the rest of the file
	 * relies on.
	 */
	for (i = 0; i < pixels; i++) {
		const uint8_t *p = src + (size_t) i * src_bpp;
		uint8_t *q = dst + (size_t) i * dst_bpp;
		uint32_t v = (uint32_t) p[0] | ((uint32_t) p[1] << 8);
		uint32_t r;

		if (src_bpp == 4) {
			v |= ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
		}
		r = accel_convert_pixel(from, to, v);
		q[0] = (uint8_t) r;
		q[1] = (uint8_t) (r >> 8);
		if (dst_bpp == 4) {
			q[2] = (uint8_t) (r >> 16);
			q[3] = (uint8_t) (r >> 24);
		}
	}
}

/**
 * Copy a sprite into the screen, converting its pixels if need be, clipped to
 * the graphics window.
 *
 * Sprite pixel data is stored top row first, the same order as screen memory, so
 * once the destination is in framebuffer terms the rows map straight across.
 *
 * @param src_stride Bytes per row of the source, from its own header rather
 *                   than from its width times its depth
 * @return non-zero if the plot was done in full
 */
static int
blit_sprite(const ScreenTarget *target, const VduWorkspace *ws,
            AccelPixelFormat fmt, uint32_t src_base, uint32_t src_stride,
            int src_w, int src_h, int32_t x_os, int32_t y_os,
            uint32_t *pixels_done)
{
	const uint32_t src_bpp = accel_format_bytes(fmt);
	const uint32_t dst_bpp = accel_format_bytes(ws->fmt);
	AccelPlotRect rect;
	uint32_t fb_off, phys;
	int row;

	if (target->host == NULL || src_bpp == 0 || dst_bpp == 0 ||
	    !accel_can_convert(fmt, ws->fmt))
	{
		return 0;
	}
	if (!mem_debug_translate(ws->screenstart, &phys) ||
	    phys - target->phys_base >= target->phys_size)
	{
		return 0;
	}
	fb_off = phys - target->phys_base;

	/* The last row of the picture has to end inside the framebuffer, computed
	   wide so a stride chosen to overflow the arithmetic cannot pass. */
	if ((uint64_t) fb_off + (uint64_t) target->stride * target->height >
	    (uint64_t) target->phys_size)
	{
		return 0;
	}

	/*
	 * Whole pixels are written straight into the framebuffer, so refuse one
	 * that is not aligned for them rather than making an unaligned store the
	 * guest would never have made. Every real screen start is page-aligned and
	 * every real stride a whole number of pixels, so this costs nothing and is
	 * here to stay true.
	 */
	if ((((uintptr_t) target->host + fb_off) & (dst_bpp - 1u)) != 0 ||
	    (target->stride & (dst_bpp - 1u)) != 0)
	{
		return 0;
	}

	if (!accel_plot_rect((int32_t) target->width, (int32_t) target->height,
	    ws->gwl, ws->gwb, ws->gwr, ws->gwt, ws->org_x, ws->org_y,
	    ws->xeig, ws->yeig, x_os, y_os, src_w, src_h, &rect))
	{
		/* Wholly outside the window: a plot that draws nothing, which is still
		   a plot we have handled. */
		stats.sprite.done_clipped_away++;
		*pixels_done = 0;
		return 1;
	}

	for (row = rect.top; row <= rect.bottom; row++) {
		uint32_t src_off = 0, left_to_copy = 0;
		uint32_t src_addr;
		uint8_t *out = target->host + fb_off +
		    (uint32_t) row * target->stride + (uint32_t) rect.left * dst_bpp;

		accel_blit_row_plan(&rect, row, src_stride, src_bpp, &src_off,
		    &left_to_copy);
		src_addr = src_base + src_off;

		/*
		 * A row at a time, in runs of whatever is readable before the next
		 * page: consecutive virtual pages are not consecutive physical ones, so
		 * a single long copy would read the wrong memory across a boundary.
		 *
		 * The run is rounded down to whole pixels. A 4KB page cannot split a
		 * two- or four-byte pixel that starts aligned, but the rounding is what
		 * makes that a property of the code rather than of the arithmetic
		 * happening to work out.
		 */
		while (left_to_copy > 0) {
			uint32_t avail = 0;
			const void *in = mem_debug_host_ptr(src_addr, &avail);
			uint32_t run;

			if (in == NULL || avail < src_bpp) {
				return 0;	/* unreadable source: leave it to the guest */
			}
			run = avail / src_bpp;
			if (run > left_to_copy) {
				run = left_to_copy;
			}
			convert_run(fmt, ws->fmt, in, out, run);
			out += run * dst_bpp;
			src_addr += run * src_bpp;
			left_to_copy -= run;
		}
	}

	mark_rows_dirty(target, fb_off, rect.top, rect.bottom);

	*pixels_done = (uint32_t) (rect.right - rect.left + 1) *
	    (uint32_t) (rect.bottom - rect.top + 1);
	return 1;
}

/**
 * Everything we can establish about one plot, and the verdict that follows.
 *
 * ★ Facts first, refusals afterwards.
 *
 * The first version refused as soon as it found a reason to, which meant one
 * refusal hid every other fact about the plot: a desktop at 16bpp reported
 * "screen is not 32bpp" for every plot and told us nothing about the sprites,
 * the sizes, or how much of it we could otherwise have taken. So the sprite is
 * measured whatever we then decide about it, and the verdict is recorded twice -
 * once as it stands, and once with the screen's depth left out of it, because
 * the depth is a setting the user can change and the rest are not.
 *
 * @param scaled  Non-zero for a scaled plot, where R6 and R7 are the scale
 *                factors and the translation table. An unscaled plot puts the
 *                sprite at its own size and carries neither.
 * @param handled Set to 1 if the host has done the plot in full
 */
static AcceleratorReason
classify_plot(int scaled, int *handled)
{
	const uint32_t r0 = arm.reg[0];
	const uint32_t area = arm.reg[1];
	const uint32_t id = arm.reg[2];
	const uint32_t action = arm.reg[5];
	const uint32_t factors = scaled ? arm.reg[6] : 0;
	const uint32_t trans = scaled ? arm.reg[7] : 0;
	ScreenTarget target;
	VduWorkspace ws;
	uint32_t header = 0;
	uint32_t mode = 0, image = 0, mask = 0;
	uint32_t src_stride = 0;
	AccelPixelFormat src_fmt;
	int type;
	int src_w, src_h;
	int64_t dest_w, dest_h;
	uint32_t xmul = 1, ymul = 1, xdiv = 1, ydiv = 1;
	AcceleratorSpriteTransparency transparency = ACCEL_SPRITE_OPAQUE;
	AcceleratorReason without_depth;

	/* ---- the sprite ---------------------------------------------------- */

	/*
	 * Only the pointer and named-in-a-user-area forms can be followed; the
	 * system sprite area is reached through the kernel's own workspace and is
	 * not worth the machinery for what still uses it.
	 */
	if (r0 & SPRITE_ADDR_POINTER) {
		header = id;
	} else if (r0 & SPRITE_ADDR_USER) {
		uint32_t count = 0, first = 0, sprite, i;
		char want[16];

		for (i = 0; i < 12; i++) {
			uint32_t c;

			if (!mem_debug_read(id + i, 1, &c) || c < 32) {
				break;
			}
			want[i] = (char) c;
		}
		want[i] = '\0';
		if (i == 0 || !guest_read32(area + AREA_COUNT, &count) ||
		    !guest_read32(area + AREA_FIRST, &first))
		{
			return ACCEL_NO_SPRITE;
		}

		sprite = area + first;
		for (i = 0; i < count && i < 1024; i++) {
			uint32_t next = 0;
			char have[16];
			int j, same = 1;

			if (!guest_read32(sprite + SPR_NEXT, &next)) {
				break;
			}
			for (j = 0; j < 12; j++) {
				uint32_t c = 0;

				if (!mem_debug_read(sprite + SPR_NAME + j, 1, &c)) {
					c = 0;
				}
				have[j] = (c < 32) ? '\0' : (char) c;
			}
			have[12] = '\0';
			for (j = 0; j < 13; j++) {
				char a = want[j], b = have[j];

				if (a >= 'A' && a <= 'Z') { a = (char) (a + 32); }
				if (b >= 'A' && b <= 'Z') { b = (char) (b + 32); }
				if (a != b) { same = 0; break; }
				if (a == '\0') { break; }
			}
			if (same) {
				header = sprite;
				break;
			}
			if (next == 0 || next > 0x08000000u) {
				break;
			}
			sprite += next;
		}
		if (header == 0) {
			return ACCEL_NO_SPRITE;
		}
	} else {
		return ACCEL_NO_SPRITE;
	}

	if (!guest_read32(header + SPR_MODE, &mode) ||
	    !guest_read32(header + SPR_IMAGE, &image) ||
	    !guest_read32(header + SPR_MASK, &mask))
	{
		return ACCEL_NO_SPRITE;
	}

	type = sprite_pixel_type(mode);
	src_fmt = source_format_for_type(type);

	if (type >= 0 && type < ACCEL_SPRITE_TYPES) {
		stats.sprite.type[type]++;
	} else {
		/* An old-format sprite, whose depth comes from a mode number, or a
		   RISC OS 5 type number past the end of the table. Both are worth
		   knowing about as a total rather than being dropped. */
		stats.sprite.type_other++;
	}

	{
		uint32_t w = 0, h = 0, lbit = 0, rbit = 0;
		const unsigned bpp = sprite_type_bpp(type);

		if (!guest_read32(header + SPR_WIDTH, &w) ||
		    !guest_read32(header + SPR_HEIGHT, &h) ||
		    !guest_read32(header + SPR_LEFTBIT, &lbit) ||
		    !guest_read32(header + SPR_RIGHTBIT, &rbit))
		{
			return ACCEL_NO_SPRITE;
		}
		src_h = (int) h + 1;
		src_stride = (w + 1u) * 4u;

		src_w = accel_sprite_row_pixels(w, lbit, rbit, bpp);
		if (src_w <= 0) {
			/*
			 * A depth this cannot reason about - a JPEG, a YCbCr sprite, an
			 * old-format one - or a header whose bits do not divide into
			 * pixels. The plot is refused on its type regardless; the width is
			 * filled in as words so the size and pixel counts still have a
			 * number beside them, and it is an undercount for anything
			 * shallower than 32bpp.
			 */
			if (src_fmt != ACCEL_FMT_NONE) {
				return ACCEL_NO_SIZE;
			}
			src_w = (int) w + 1;
		}
	}

	/* How the sprite carries its transparency, which is a fact about the
	   sprite whatever we decide about the plot. */
	if (mask == image || mask == 0) {
		const int riscos5 = ((mode >> SPRMODE_TYPE_SHIFT) & 0xfu) ==
		    SPRMODE_RISCOS5;

		/*
		 * 5:6:5 spends every bit on colour, so it cannot carry alpha whatever
		 * the flag says - the kernel clears that flag itself for this depth
		 * (`compute_pixelformat` in SprExtend's c/asmcore). Believing the flag
		 * here would refuse a perfectly ordinary plot for a transparency that
		 * does not exist.
		 */
		if ((action & PLOT_USE_MASK) != 0 && type != SPRTYPE_16BPP_565 &&
		    (type == SPRTYPE_16BPP_4444 ||
		     (riscos5 && (mode & SPRMODE_ALPHA) != 0)))
		{
			/* 4:4:4:4 carries alpha by virtue of its type, so it is not
			   trusted to the flag - a sprite that omitted it would
			   otherwise be blended by RISC OS and drawn opaque here. */
			transparency = ACCEL_SPRITE_ALPHA_PIXEL;
		}
	} else if ((mode & SPRMODE_WIDE_MASK) != 0) {
		transparency = ACCEL_SPRITE_ALPHA_PLANE;
	} else {
		transparency = ACCEL_SPRITE_MASK_1BPP;
	}
	stats.sprite.transparency[transparency]++;

	/* ---- how big the plot is ------------------------------------------- */

	if (factors != 0) {
		if (!guest_read32(factors + 0, &xmul) ||
		    !guest_read32(factors + 4, &ymul) ||
		    !guest_read32(factors + 8, &xdiv) ||
		    !guest_read32(factors + 12, &ydiv))
		{
			xmul = ymul = xdiv = ydiv = 0;
		}
	}
	if (xmul == 0 || ymul == 0 || xdiv == 0 || ydiv == 0 ||
	    xmul > 0x10000u || ymul > 0x10000u ||
	    xdiv > 0x10000u || ydiv > 0x10000u)
	{
		return ACCEL_NO_SCALE;
	}
	if (xmul == xdiv && ymul == ydiv) {
		stats.sprite.scaled_1to1++;
	}

	dest_w = ((int64_t) src_w * (int64_t) xmul) / (int64_t) xdiv;
	dest_h = ((int64_t) src_h * (int64_t) ymul) / (int64_t) ydiv;
	if (src_w < 1 || src_h < 1 || src_w > 16384 || src_h > 16384 ||
	    dest_w <= 0 || dest_h <= 0 || dest_w > 32768 || dest_h > 32768)
	{
		return ACCEL_NO_SIZE;
	}

	/* Destination pixels in thousands, so a session's worth of plotting is a
	   figure that means something beside the frame rate. */
	stats.sprite.pixels += (uint32_t) ((dest_w * dest_h) / 1000);

	/* ---- where it was going -------------------------------------------- */

	if (!screen_target(&target)) {
		stats.sprite.dest[ACCEL_DEST_ELSEWHERE]++;
		return ACCEL_NO_TARGET;
	}
	stats.sprite.dest[target.dest]++;
	stats.screen_width = target.width;
	stats.screen_height = target.height;
	stats.screen_bpp = target.bpp;
	switch (target.bpp) {
	case 8:  stats.depth_8bpp++; break;
	case 16: stats.depth_16bpp++; break;
	case 32: stats.depth_32bpp++; break;
	default: break;
	}

	/*
	 * Reading the workspace is also how a redirected plot is told apart from
	 * one going to the screen, so a failure here is not "we could not measure
	 * it" - it is very often "this plot is not ours".
	 */
	{
		const int state = vdu_workspace_read(&target, &ws);

		if (state == 0) {
			return ACCEL_NO_GEOMETRY;
		}
		if (state < 0) {
			return ACCEL_NO_TARGET;
		}
	}

	/* ---- the verdict --------------------------------------------------- */

	/*
	 * Everything except the screen's depth, in the order that costs least to
	 * establish. This is the figure that says whether a host blitter is worth
	 * building, because a machine can be moved to 32bpp and cannot be moved off
	 * translation tables.
	 */
	if (trans != 0) {
		without_depth = ACCEL_NO_TRANSTABLE;
	} else if (src_fmt == ACCEL_FMT_NONE) {
		without_depth = ACCEL_NO_TYPE;
		if (type >= 0 && type < ACCEL_SPRITE_TYPES) {
			stats.sprite.type_blocked[type]++;
			if (image != SPR_HEADER_SIZE) {
				/* Carries its own palette, which is what a sub-16bpp
				   source would have to be read through. Counted here
				   because the type is refused before the palette is
				   looked at, so it would otherwise be invisible. */
				stats.sprite.type_blocked_palette[type]++;
			}
		} else {
			stats.sprite.type_blocked_other++;
		}
	} else if (image != SPR_HEADER_SIZE) {
		without_depth = ACCEL_NO_PALETTE;
	} else if (((mode >> SPRMODE_TYPE_SHIFT) & 0xfu) == SPRMODE_RISCOS5 &&
	    ((mode & SPRMODE_FAMILY) != 0 || (mode & SPRMODE_ORDER_RGB) != 0))
	{
		without_depth = ACCEL_NO_ORDER;
	} else if (transparency == ACCEL_SPRITE_MASK_1BPP) {
		without_depth = ACCEL_NO_MASK;
	} else if ((action & PLOT_ACTION) != 0 ||
	    (action & (PLOT_UNSUPPORTED | PLOT_WIDE_TRANS)) != 0)
	{
		/* Not just the GCOL action: bits 6-15 ask for dithering, colour
		   mapping or translucency, any of which makes the result something
		   other than a copy of the sprite. Bit 4 (ignore the translation
		   table) is harmless here because we require there to be none. */
		without_depth = ACCEL_NO_ACTION;
	} else {
		without_depth = ACCEL_TAKEN;
	}

	/* What this plot really covers, once the redraw rectangle has had its say. */
	{
		AccelPlotRect rect;

		if (accel_plot_rect((int32_t) target.width, (int32_t) target.height,
		    ws.gwl, ws.gwb, ws.gwr, ws.gwt, ws.org_x, ws.org_y,
		    ws.xeig, ws.yeig, (int32_t) arm.reg[3], (int32_t) arm.reg[4],
		    (int32_t) dest_w, (int32_t) dest_h, &rect))
		{
			const uint32_t visible =
			    (uint32_t) (rect.right - rect.left + 1) *
			    (uint32_t) (rect.bottom - rect.top + 1) / 1000u;

			stats.sprite.pixels_visible += visible;
			if (without_depth == ACCEL_TAKEN &&
			    accel_can_convert(src_fmt, ws.fmt))
			{
				stats.sprite.pixels_visible_takeable += visible;
			}
			if (without_depth == ACCEL_NO_TYPE) {
				if (type >= 0 && type < ACCEL_SPRITE_TYPES) {
					stats.sprite.type_blocked_pixels[type] += visible;
				} else {
					stats.sprite.type_blocked_other_pixels += visible;
				}
			}
		}
	}

	stats.sprite.verdict_without_depth[without_depth]++;
	if (without_depth == ACCEL_TAKEN) {
		stats.sprite.takeable_at_32bpp++;
		stats.sprite.pixels_takeable +=
		    (uint32_t) ((dest_w * dest_h) / 1000);
	}

	/*
	 * ★ The screen's layout, not merely its depth.
	 *
	 * This used to be "is it 32bpp", which on a 16bpp desktop refused every
	 * plot for that one reason - and a 16bpp desktop is an ordinary thing to
	 * be running, not least because 1280x720 at 32bpp needs more video memory
	 * than a Risc PC has. What matters is whether the sprite's layout can be
	 * turned into the screen's exactly, which the same layout at both ends
	 * satisfies trivially.
	 */
	if (without_depth != ACCEL_TAKEN) {
		return (ws.fmt == ACCEL_FMT_NONE) ? ACCEL_NO_SCREEN : without_depth;
	}
	if (ws.fmt == ACCEL_FMT_NONE) {
		return ACCEL_NO_SCREEN;
	}
	if (!accel_can_convert(src_fmt, ws.fmt)) {
		return ACCEL_NO_CONVERSION;
	}

	/*
	 * What the blit itself does not do. All three are refusals rather than
	 * approximations: a plot drawn nearly right is worse than one drawn by the
	 * guest, because it looks like the emulator is broken and the cause is
	 * invisible.
	 *
	 * Transparency is left to the guest because the counting says almost
	 * nothing uses it here - four plots in seven thousand carried an alpha
	 * plane - so the exact blend arithmetic can wait until there is a figure
	 * that moves.
	 */
	if (transparency != ACCEL_SPRITE_OPAQUE) {
		return ACCEL_NO_ALPHA;
	}
	if (target.host == NULL) {
		return ACCEL_NO_FRAMEBUFFER;
	}
	{
		uint32_t wastage = 0;

		if (!guest_read32(header + SPR_LEFTBIT, &wastage) || wastage != 0) {
			return ACCEL_NO_WASTAGE;
		}
	}
	/* Only the 1:1 case, which the counting says is all of them: everything a
	   desktop plots comes through at the sprite's own size. */
	if (xmul != xdiv || ymul != ydiv) {
		return ACCEL_NO_SCALE;
	}

	/*
	 * Only the scaled reason is acted on, though both are counted. R5 is
	 * documented as the plot action for the scaled plot and that is the one the
	 * evidence covers; the plot-at-its-own-size reason came to 46 out of 7311
	 * on a real desktop, which is not enough to justify acting on a register
	 * meaning this code has not checked.
	 */
	if (config.accelerators_enabled && scaled) {
		uint32_t painted = 0;

		if (blit_sprite(&target, &ws, src_fmt, header + image, src_stride,
		    src_w, src_h, (int32_t) arm.reg[3], (int32_t) arm.reg[4],
		    &painted))
		{
			stats.sprite.done++;
			stats.sprite.done_pixels += painted / 1000u;
			*handled = 1;
		}
	}

	return ACCEL_TAKEN;
}

int
accel_swi(uint32_t swinum)
{
	const uint32_t swi = swinum & ~SWI_X_BIT;

	switch (swi) {
	case SWI_OS_SPRITEOP: {
		const uint32_t reason = arm.reg[0] & 0xffu;

		stats.sprite.calls++;
		if (reason < ACCEL_SPRITE_REASONS) {
			stats.sprite.reason[reason]++;
		} else {
			stats.sprite.reason_other++;
		}
		/*
		 * Two reasons plot a sprite on the screen: &34 scaled, and &22 at the
		 * sprite's own size. The first run counted only the scaled one and so
		 * missed several hundred plots a session. R0, R1 and R2 mean the same
		 * thing for both, which is what lets one classifier serve them.
		 */
		if (reason == SPRITE_PUT_SCALED || reason == SPRITE_PUT) {
			const int scaled = (reason == SPRITE_PUT_SCALED);
			int handled = 0;
			const AcceleratorReason verdict = classify_plot(scaled, &handled);

			if (scaled) {
				stats.sprite.scaled++;
			} else {
				stats.sprite.unscaled++;
			}
			stats.sprite.verdict[verdict]++;
			if (handled) {
				return 1;
			}
		}
		break;
	}

	case SWI_FONT_PAINT:	stats.other.font_paint++; break;
	case SWI_WIMP_PLOTICON:	stats.other.wimp_ploticon++; break;
	case SWI_DRAW_FILL:
	case SWI_DRAW_FILLFP:	stats.other.draw_fill++; break;
	case SWI_OS_PLOT:	stats.other.os_plot++; break;

	default:
		break;
	}

	/* Counting only: the guest does the work, exactly as before. */
	return 0;
}

void
accel_reset(void)
{
	memset(&stats, 0, sizeof(stats));
}

void
accel_get_stats(AcceleratorStats *out)
{
	if (out != NULL) {
		*out = stats;
	}
}

const char *
accel_reason_name(AcceleratorReason reason)
{
	switch (reason) {
	case ACCEL_TAKEN:		return "could be done on the host";
	case ACCEL_NO_TARGET:		return "output is not the screen";
	case ACCEL_NO_GEOMETRY:		return "VDU workspace unresolved";
	case ACCEL_NO_SCREEN:		return "screen is a layout this cannot write";
	case ACCEL_NO_CONVERSION:	return "no exact route to the screen's layout";
	case ACCEL_NO_TRANSTABLE:	return "colour translation table";
	case ACCEL_NO_SPRITE:		return "sprite not located";
	case ACCEL_NO_TYPE:		return "source is not 32bpp";
	case ACCEL_NO_PALETTE:		return "source has a palette";
	case ACCEL_NO_ORDER:		return "source is &xRGB";
	case ACCEL_NO_MASK:		return "1bpp mask";
	case ACCEL_NO_ACTION:		return "plot action";
	case ACCEL_NO_SCALE:		return "scale factors";
	case ACCEL_NO_SIZE:		return "size";
	case ACCEL_NO_FRAMEBUFFER:	return "screen not writable from the host";
	case ACCEL_NO_ALPHA:		return "transparency";
	case ACCEL_NO_WASTAGE:		return "left-hand wastage in the source";
	default:			return "unknown";
	}
}

const char *
accel_dest_name(AcceleratorDest dest)
{
	switch (dest) {
	case ACCEL_DEST_NONE:		return "unknown";
	case ACCEL_DEST_CARD:		return "graphics card";
	case ACCEL_DEST_VRAM:		return "VIDC20 (VRAM)";
	case ACCEL_DEST_RAM:		return "VIDC20 (main memory)";
	case ACCEL_DEST_ELSEWHERE:	return "elsewhere";
	default:			return "unknown";
	}
}

const char *
accel_transparency_name(AcceleratorSpriteTransparency t)
{
	switch (t) {
	case ACCEL_SPRITE_OPAQUE:	return "opaque";
	case ACCEL_SPRITE_ALPHA_PLANE:	return "8-bit alpha plane";
	case ACCEL_SPRITE_ALPHA_PIXEL:	return "alpha in the pixels";
	case ACCEL_SPRITE_MASK_1BPP:	return "1bpp mask";
	default:			return "unknown";
	}
}
