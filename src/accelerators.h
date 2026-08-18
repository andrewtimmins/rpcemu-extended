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
 * accelerators.h - work the host does instead of the emulated ARM.
 *
 * Some things RISC OS does are pixel-by-pixel or byte-by-byte loops that cost
 * the emulated ARM thousands of instructions and cost this process a memcpy.
 * Where the operation arrives as a SWI, and where the host can produce the
 * identical result, it can be done here and the guest's own code skipped.
 *
 * The hook is `accel_swi()`, called from `opSWI()` in arm_common.c, so it serves
 * the interpreter and the recompiler alike. That is the same place mousehack,
 * HostFS and the clipboard are intercepted, and the same rule applies: anything
 * this does not handle returns 0 and the guest runs exactly as it would have.
 *
 * ★ It counts what it could do as well as what it did.
 *
 * Every candidate operation is classified whether or not it is taken, and the
 * refusals are counted by reason, which is what the machine inspector's
 * Accelerators tab shows. That is not idle bookkeeping: it is how the decision
 * to write any of this was made, and how the next case to support gets chosen
 * instead of guessed at. Switched off, the classification still runs and the
 * counts still say what would have happened.
 *
 * Controlled by the machine's own setting (`config.accelerators_enabled`,
 * "Let the host do drawing it can do identically" on the System tab), on by
 * default.
 *
 * Named for the general case rather than for sprites. The candidates so far are
 * graphical, but the shape of the thing - a SWI the host can satisfy exactly -
 * is not.
 */

#ifndef ACCELERATORS_H
#define ACCELERATORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 * Why a candidate operation could not be done here.
 *
 * Each of these is a decision waiting on evidence: closing one is anything from
 * a line of code to a rewrite, and the counts are what says which are worth it.
 */
typedef enum {
	ACCEL_TAKEN = 0,	/**< Could be done on the host, in full */
	ACCEL_NO_TARGET,	/**< Output is redirected: a sprite, not the screen */
	ACCEL_NO_GEOMETRY,	/**< The VDU workspace could not be read at all */
	ACCEL_NO_DEPTH,		/**< Screen is not 32bpp */
	ACCEL_NO_TRANSTABLE,	/**< A colour-translation table was supplied */
	ACCEL_NO_SPRITE,	/**< System sprite area, or the sprite was not found */
	ACCEL_NO_TYPE,		/**< Source is not a 32bpp sprite */
	ACCEL_NO_PALETTE,	/**< Source carries its own palette */
	ACCEL_NO_ORDER,		/**< Source is &xRGB where we handle &xBGR */
	ACCEL_NO_MASK,		/**< A 1bpp on/off mask plane */
	ACCEL_NO_ACTION,	/**< A plot action other than plain overwrite */
	ACCEL_NO_SCALE,		/**< Scale factors outside anything sane */
	ACCEL_NO_SIZE,		/**< Source or destination size outside anything sane */
	ACCEL_NO_FRAMEBUFFER,	/**< The screen is in memory the host cannot write here */
	ACCEL_NO_ALPHA,		/**< Transparency this does not do yet */
	ACCEL_NO_WASTAGE,	/**< Source rows have wasted bits at the left */
	ACCEL_REASON_COUNT
} AcceleratorReason;

/** Where the guest's screen output is currently going. */
typedef enum {
	ACCEL_DEST_NONE = 0,	/**< Not established */
	ACCEL_DEST_CARD,	/**< The graphics card's framestore */
	ACCEL_DEST_VRAM,	/**< VIDC20, with VRAM fitted */
	ACCEL_DEST_RAM,		/**< VIDC20, screen in main memory */
	ACCEL_DEST_ELSEWHERE,	/**< Somewhere neither of those: a sprite, probably */
	ACCEL_DEST_COUNT
} AcceleratorDest;

/** How the source sprite carries its transparency. */
typedef enum {
	ACCEL_SPRITE_OPAQUE = 0,	/**< No mask plane and no alpha channel */
	ACCEL_SPRITE_ALPHA_PLANE,	/**< 8-bit alpha in a separate plane */
	ACCEL_SPRITE_ALPHA_PIXEL,	/**< Alpha in the pixel's own top byte */
	ACCEL_SPRITE_MASK_1BPP,		/**< On/off mask plane */
	ACCEL_SPRITE_TRANSPARENCY_COUNT
} AcceleratorSpriteTransparency;

#define ACCEL_SPRITE_REASONS	64	/**< OS_SpriteOp reasons we tally */
#define ACCEL_SPRITE_TYPES	32	/**< Sprite pixel types we tally */

typedef struct {
	uint32_t calls;			/**< OS_SpriteOp calls seen */
	uint32_t reason[ACCEL_SPRITE_REASONS];	/**< By reason, R0 low byte */
	uint32_t reason_other;		/**< Reasons above the table */

	/* The plots, which are the candidates. */
	uint32_t scaled;		/**< Scaled plots offered */
	uint32_t unscaled;		/**< Plots at the sprite's own size */
	uint32_t verdict[ACCEL_REASON_COUNT];	/**< What we decided, and why */

	/*
	 * The same decision with the screen's depth left out of it.
	 *
	 * Worth its own counter because the depth is a setting rather than a
	 * property of the plot: a machine whose desktop is at 16bpp refuses every
	 * plot for that one reason and tells us nothing about the rest, which is
	 * exactly what the first run did.
	 */
	uint32_t takeable_at_32bpp;
	uint32_t pixels_takeable;	/**< Their destination pixels, thousands */

	uint32_t type[ACCEL_SPRITE_TYPES];	/**< Source pixel type of each */
	uint32_t type_other;		/**< Types above the table, or old-format */
	uint32_t transparency[ACCEL_SPRITE_TRANSPARENCY_COUNT];
	uint32_t dest[ACCEL_DEST_COUNT];	/**< Where the output was going */
	uint32_t scaled_1to1;		/**< Of those, plotted at 1:1 */
	uint32_t pixels;			/**< Destination pixels they cover, thousands */

	/*
	 * The same, after clipping to the graphics window, which is the only figure
	 * that says what work is really being done.
	 *
	 * A window's redraw plots the whole of a backdrop and clips it to whatever
	 * rectangle is being repaired, so the unclipped rectangle above overstates
	 * what the guest actually grinds through, and overstated what a host-side
	 * plot would save.
	 */
	uint32_t pixels_visible;		/**< Thousands, all plots we could measure */
	uint32_t pixels_visible_takeable;	/**< Thousands, the ones we could take */

	/* What was actually done on the host, once switched on. */
	uint32_t done;			/**< Plots the host completed */
	uint32_t done_pixels;		/**< Their pixels, thousands */
	uint32_t done_clipped_away;	/**< Of those, entirely outside the window */
} AcceleratorSpriteStats;

/* Other SWIs worth knowing the rate of, to see what a redraw is made of. Not
   candidates yet: none of them is a single self-contained pixel loop the way a
   sprite plot is, and Font_Paint in particular has no host-side equivalent. */
typedef struct {
	uint32_t font_paint;		/**< Font_Paint */
	uint32_t wimp_ploticon;		/**< Wimp_PlotIcon */
	uint32_t draw_fill;		/**< Draw_Fill and Draw_FillFP */
	uint32_t os_plot;		/**< OS_Plot */
} AcceleratorOtherStats;

typedef struct {
	AcceleratorSpriteStats sprite;
	AcceleratorOtherStats other;

	/* The screen as our own side of the emulation sees it, so a refusal that
	   turns on the depth can be read without guessing what the depth was. */
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t screen_bpp;
	uint32_t depth_8bpp;		/**< Plots offered at each screen depth */
	uint32_t depth_16bpp;
	uint32_t depth_32bpp;

	/* Offsets found in the VDU driver workspace, or 0 while unresolved. Kept
	   for display because "geometry unresolved" is otherwise a dead end for
	   anyone reading the counts. */
	uint32_t vdu_ws_probed;
	uint32_t vdu_off_xwindlimit;
	uint32_t vdu_off_ywindlimit;
	uint32_t vdu_off_linelength;
	uint32_t vdu_off_screenstart;
} AcceleratorStats;

/*
 * Where a plot lands, in framebuffer terms: rows down from the top, columns from
 * the left, both ends inclusive.
 *
 * Exposed because it is the part worth testing on its own. RISC OS counts y up
 * from the bottom of the screen and a framebuffer counts rows down from the top,
 * graphics window edges are inclusive at both ends, and the position arrives in
 * OS units relative to a movable origin. Every one of those is an opportunity to
 * be off by one, or upside down, and none of them needs a running machine to
 * check.
 */
typedef struct {
	int32_t left, right;	/**< Columns to fill, inclusive */
	int32_t top, bottom;	/**< Rows to fill, inclusive */
	int32_t origin_left;	/**< Column the sprite's first pixel would be at */
	int32_t origin_top;	/**< Row the sprite's top row would be at */
} AccelPlotRect;

/**
 * Work out which pixels a plot covers.
 *
 * @return non-zero if anything is left after clipping; 0 for a plot wholly
 *         outside the graphics window, which draws nothing and is not an error
 */
extern int accel_plot_rect(int32_t screen_w, int32_t screen_h,
    int32_t gwl, int32_t gwb, int32_t gwr, int32_t gwt,
    int32_t org_x, int32_t org_y, uint32_t xeig, uint32_t yeig,
    int32_t x_os, int32_t y_os, int32_t src_w, int32_t src_h,
    AccelPlotRect *out);

/**
 * Where one destination row's pixels come from in the source.
 *
 * Separated from the copying so it can be tested: this is the arithmetic that
 * decides a clipped plot takes its pixels from part-way into the sprite rather
 * than from its first row and column, and getting it wrong shears the picture or
 * turns it over without failing in any way a test of the placement would catch.
 *
 * @param rect       The plot, as accel_plot_rect() worked it out
 * @param row        Destination row, between rect->top and rect->bottom
 * @param src_stride Bytes per row of the source
 * @param src_offset Set to the byte offset into the source pixel data
 * @param bytes      Set to how many bytes to copy
 */
extern void accel_blit_row_plan(const AccelPlotRect *rect, int32_t row,
    uint32_t src_stride, uint32_t *src_offset, uint32_t *bytes);

/**
 * How many pixels a sprite row holds, from its header fields.
 *
 * Exposed because `width + 1` is the pixel count at 32bpp and at no other
 * depth, and getting it wrong draws the picture too narrow while taking the
 * rest of each row from the row below - a failure that looks like a corrupt
 * sprite rather than like arithmetic.
 *
 * @param width_words_less_one The header's width field, verbatim
 * @param lbit  Wasted bits at the left of the first word
 * @param rbit  Last bit used in the last word
 * @param bpp   Bits per pixel of the sprite's type
 * @return pixels per row, or 0 if those fields do not describe a whole number
 */
extern int accel_sprite_row_pixels(uint32_t width_words_less_one, uint32_t lbit,
    uint32_t rbit, unsigned bpp);

/**
 * Offer a SWI to the accelerators.
 *
 * @param swinum SWI number as opSWI() has it, X bit included
 * @return non-zero if the host has done the whole thing and the guest's own
 *         handler must not run. Always 0 for now: see the note above.
 */
extern int accel_swi(uint32_t swinum);

/** Forget everything counted so far. Called on reset. */
extern void accel_reset(void);

/** Take a copy of the counts, for the machine inspector. */
extern void accel_get_stats(AcceleratorStats *stats);

/** Names for the enums above, for anything displaying the counts. */
extern const char *accel_reason_name(AcceleratorReason reason);
extern const char *accel_dest_name(AcceleratorDest dest);
extern const char *accel_transparency_name(AcceleratorSpriteTransparency t);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* ACCELERATORS_H */
