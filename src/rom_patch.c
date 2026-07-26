/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025 Andy Timmins

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
 * ROM patching.
 *
 * All of RPCEmu's modifications to the loaded ROM image live here, so the set
 * of patches can be reviewed in one place. Each patch is self-gating: it
 * inspects the ROM (and, where relevant, the machine model / configuration)
 * and only writes if it recognises what it is looking at, leaving unrelated
 * ROMs untouched.
 *
 * Two location strategies are used:
 *   - Fixed offset + signature: the historic VRAM-cap and NCOS tables, keyed on
 *     an exact address plus surrounding words. Precise but ROM-revision-bound;
 *     an unrecognised build is simply skipped.
 *   - Content scan: sweep the image for a value or instruction sequence that is
 *     unique to the site of interest. Survives ROMs shifting between builds,
 *     which is how the display-clock, EDID and power-off patches stay working
 *     across RISC OS 5 revisions.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rpcemu.h"
#include "mem.h"
#include "edid.h"
#include "display_mode.h"
#include "rom_patch.h"

/* -------------------------------------------------------------------------
 * Fixed-offset VRAM cap table
 * ------------------------------------------------------------------------- */

typedef struct {
	uint32_t	addr_data;	///< Address to try matching data
	uint32_t	data[4];	///< Data that must match
	uint32_t	addr_replace;	///< Address of replacement data
	uint32_t	replace;	///< Replacement data
	const char	*comment;	///< Comment that will be added to logfile
} rom_patch_t;

/* The VRAM-cap patches below overwrite the ROM's "MOVEQ R6, #<VRAM MB>"
   instruction that limits how much VRAM the OS will use. The table encodes the
   8MB form; on a machine fitted with 16MB VRAM we rewrite the immediate so the
   OS opens up the extra bank (and the higher screen modes it enables). */
#define VRAM_CAP_MOV_8MB	0x03a06008u	/* MOVEQ R6, #8  */
#define VRAM_CAP_MOV_16MB	0x03a06010u	/* MOVEQ R6, #16 */

static const rom_patch_t rom_patch[] = {
	// Patching for 8MB VRAM
	{ 0x138c0, { 0xe3a00402, 0xe2801004, 0xeb000128, 0x03a06002 }, 0x138cc, 0x03a06008, "8MB VRAM RISC OS 3.50" },
	{ 0x1411c, { 0xe3a00402, 0xe2801004, 0xeb000122, 0x03a06002 }, 0x14128, 0x03a06008, "8MB VRAM RISC OS 3.60" },
	{ 0x15874, { 0xe3a00402, 0xe2801004, 0xeb000143, 0x03a06002 }, 0x15880, 0x03a06008, "8MB VRAM RISC OS 3.70" },
	{ 0x15898, { 0xe3a00402, 0xe2801004, 0xeb000143, 0x03a06002 }, 0x158a4, 0x03a06008, "8MB VRAM RISC OS 3.71" },
	{ 0x14744, { 0xe3a00402, 0xe2801004, 0xeb000148, 0x03a06002 }, 0x14750, 0x03a06008, "8MB VRAM RISC OS 4.02" },
	{ 0x148e8, { 0xe3a00402, 0xe2801004, 0xeb0001ae, 0x03a06002 }, 0x148f4, 0x03a06008, "8MB VRAM RISC OS 4.04" },
	{ 0x14150, { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0x1415c, 0x03a06008, "8MB VRAM RISC OS 4.29" },
	{ 0x1473c, { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0x14748, 0x03a06008, "8MB VRAM RISC OS 4.33" },
	{ 0xe504,  { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0xe510,  0x03a06008, "8MB VRAM RISC OS 4.37" },
	{ 0xe248,  { 0xe3a00402, 0xe2801004, 0xeb0001ae, 0x03a06002 }, 0xe254,  0x03a06008, "8MB VRAM RISC OS 4.39" },
	{ 0xe248,  { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0xe254,  0x03a06008, "8MB VRAM RISC OS 4.39 (Adjust)" },
	{ 0x8a764, { 0xe1a00001, 0xe2801004, 0xeb00000d, 0x03a06002 }, 0x8a770, 0x03a06008, "8MB VRAM RISC OS 6.02" },
};

/* -------------------------------------------------------------------------
 * Small content-scan helpers
 * ------------------------------------------------------------------------- */

/**
 * Find a 32-bit word that occurs exactly once in the loaded ROM.
 *
 * "Exactly once" is the safety net: if a value we want to rewrite also appears
 * elsewhere we refuse to touch it rather than risk disturbing an unrelated
 * word.
 *
 * @param words Number of 32-bit words loaded
 * @param value Word to search for
 * @param out   Set to the word index of the single match
 * @return 1 if found exactly once, 0 otherwise
 */
static int
find_unique_word(size_t words, uint32_t value, size_t *out)
{
	size_t hits = 0;
	size_t match = 0;
	size_t i;

	for (i = 0; i < words; i++) {
		if (rom[i] == value) {
			match = i;
			hits++;
		}
	}

	if (hits == 1) {
		*out = match;
		return 1;
	}
	return 0;
}

/**
 * Find a contiguous run of words that occurs exactly once in the loaded ROM.
 *
 * @param words Number of 32-bit words loaded
 * @param seq   Sequence of words to match
 * @param n     Length of seq
 * @param out   Set to the word index where the single match begins
 * @return 1 if found exactly once, 0 otherwise
 */
static int
find_unique_sequence(size_t words, const uint32_t *seq, size_t n, size_t *out)
{
	size_t hits = 0;
	size_t match = 0;
	size_t i;

	if (n == 0 || words < n) {
		return 0;
	}

	for (i = 0; i + n <= words; i++) {
		size_t j;

		for (j = 0; j < n; j++) {
			if (rom[i + j] != seq[j]) {
				break;
			}
		}
		if (j == n) {
			match = i;
			hits++;
		}
	}

	if (hits == 1) {
		*out = match;
		return 1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * VRAM cap (fixed-offset table)
 * ------------------------------------------------------------------------- */

/**
 * Scan the fixed-offset table for a match with the current ROM. If found, make
 * the required change and log it, raising the cap to 16MB when that much VRAM
 * is fitted.
 */
static void
rom_patch_vram_cap(void)
{
	const rom_patch_t *p;
	size_t i;

	for (i = 0, p = rom_patch; i < sizeof(rom_patch) / sizeof(rom_patch[0]); i++, p++) {
		uint32_t addr = p->addr_data;
		const uint32_t *data = p->data;

		if (rom[addr >> 2] == data[0] &&
		    rom[(addr + 4) >> 2] == data[1] &&
		    rom[(addr + 8) >> 2] == data[2] &&
		    rom[(addr + 12) >> 2] == data[3])
		{
			uint32_t replace = p->replace;

			// The VRAM-cap patches raise the OS's native 2MB VRAM limit.
			// Match the cap to the VRAM actually fitted: leave it alone on
			// 2MB (or no-VRAM) machines so the OS never addresses VRAM it
			// hasn't got, and raise it to 16MB when 16MB is fitted.
			if (replace == VRAM_CAP_MOV_8MB) {
				if (config.vram_size < 8) {
					continue;
				}
				if (config.vram_size >= 16) {
					replace = VRAM_CAP_MOV_16MB;
				}
			}

			// Patch the data
			rom[p->addr_replace >> 2] = replace;

			// Log the patch
			rpclog("rom_patch: applied: %s%s\n", p->comment,
			       replace == VRAM_CAP_MOV_16MB ? " (raised to 16MB)" : "");
		}
	}
}

/* -------------------------------------------------------------------------
 * Display pixel-rate ceiling
 * ------------------------------------------------------------------------- */

/* RISC OS 5's display driver refuses to program any screen mode whose pixel
   rate is above the real VIDC20 ceiling of 110 MHz, and the desktop's mode
   chooser only lists modes that clear that check. RPCEmu draws the screen in
   software and has no pixel clock at all, so on the emulator this bound only
   serves to hide the large modes a monitor definition could otherwise offer.
   The driver stores the ceiling as one pixel-rate constant (in kHz); we raise
   it so the mode list reflects what the emulator can genuinely display. */
#define DISPLAY_CLOCK_CEIL_KHZ		110000u		/* 110 MHz, as stored in the ROM */
#define DISPLAY_CLOCK_CEIL_UNCAPPED	0x00ffffffu	/* ~16 GHz: no mode ever reaches it */

/**
 * Raise the display driver's pixel-rate ceiling on RISC OS 5 images.
 *
 * The 110 MHz limit lives in the driver as a single kHz constant. Rather than
 * pin it to a build-specific address (it drifts between ROM revisions), we take
 * advantage of the fact that this exact value does not otherwise appear in a
 * RISC OS 5 image: we sweep the loaded ROM and only rewrite it when it is found
 * exactly once, which keeps us from disturbing an unrelated word. ROMs that do
 * not contain the constant at all (RISC OS 3/4, NCOS) are left completely
 * untouched.
 *
 * @param rom_bytes Number of bytes actually loaded into the ROM image
 */
static void
rom_patch_display_clock(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	size_t match = 0;

	if (find_unique_word(words, DISPLAY_CLOCK_CEIL_KHZ, &match)) {
		rom[match] = DISPLAY_CLOCK_CEIL_UNCAPPED;
		rpclog("rom_patch: applied: display pixel-rate ceiling lifted (word at 0x%06x)\n",
		       (unsigned) (match * 4));
	}
}

/* -------------------------------------------------------------------------
 * Monitor EDID injection
 * ------------------------------------------------------------------------- */

/* Bounds for the advertised native mode, so an unusually large or unset host
   display can't ask RISC OS to build something silly. */
#define EDID_NATIVE_MAX_X	2560u
#define EDID_NATIVE_MAX_Y	1440u
#define EDID_NATIVE_DEFAULT_X	1920u
#define EDID_NATIVE_DEFAULT_Y	1080u
#define EDID_NATIVE_HZ		60u

/**
 * Replace the video driver's built-in (empty) monitor EDID with one that
 * advertises a real mode ladder, so a machine set to MonitorType Auto detects a
 * capable monitor instead of falling back to a minimal default. The preferred
 * (native) mode tracks the host display where the front-end has reported it.
 *
 * The block is located by content, not address: we scan the loaded ROM for the
 * single structurally-valid EDID 1.x block it contains. RISC OS 3/4 images have
 * none and are left untouched.
 *
 * @param rom_bytes Number of bytes actually loaded into the ROM image
 */
static void
rom_patch_monitor_edid(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	uint8_t *rb = (uint8_t *) rom;
	size_t found = (size_t) -1;
	size_t hits = 0;
	unsigned native_x, native_y;
	unsigned bound_x = 0, bound_y = 0;
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t byte;

	/* Word-aligned scan (the table is word-aligned in the driver). */
	for (byte = 0; byte + EDID_BLOCK_SIZE <= words * 4; byte += 4) {
		if (edid_block_is_valid(&rb[byte])) {
			found = byte;
			hits++;
		}
	}

	if (hits != 1) {
		return;		/* Not a single unambiguous block: leave well alone. */
	}

	/* Bound by the host display if the front-end has published it, else by a
	   sensible high default. Clamp to keep it sane. */
	if (!rpcemu_get_host_display(&bound_x, &bound_y)) {
		bound_x = EDID_NATIVE_DEFAULT_X;
		bound_y = EDID_NATIVE_DEFAULT_Y;
	}
	if (bound_x > EDID_NATIVE_MAX_X) {
		bound_x = EDID_NATIVE_MAX_X;
	}
	if (bound_y > EDID_NATIVE_MAX_Y) {
		bound_y = EDID_NATIVE_MAX_Y;
	}

	/* Then bound it by the display memory the machine actually has. Advertising
	   a preferred timing whose framebuffer does not fit gives the guest a mode
	   it cannot display: RISC OS answers "not suitable for displaying the
	   desktop", which is how a Kinetic (clamped to 2MB) reacts to being offered
	   2560x1440. With the graphics card fitted the budget is the card's own
	   framestore, which is the point of it. Budget 32bpp, since that is the
	   deepest the desktop may choose and the advertised mode has to work
	   whichever depth is configured. A machine with no VRAM fitted takes screen
	   memory from DRAM instead, so there is no figure to reason about and the
	   limit is skipped. */
	if (!display_mode_fit(bound_x, bound_y, 4, rpcemu_display_memory(),
	                             &native_x, &native_y))
	{
		rpclog("rom_patch: no standard mode fits %zu KB of display memory "
		       "within %ux%u - leaving the monitor EDID alone\n",
		       rpcemu_display_memory() / 1024, bound_x, bound_y);
		return;
	}

	memcpy(base, &rb[found], EDID_BLOCK_SIZE);
	edid_build_from_base(block, base, native_x, native_y, EDID_NATIVE_HZ);
	memcpy(&rb[found], block, EDID_BLOCK_SIZE);

	rpclog("rom_patch: applied: monitor EDID replaced, native %ux%u@%u (block at 0x%06x)\n",
	       native_x, native_y, EDID_NATIVE_HZ, (unsigned) found);
}

/* -------------------------------------------------------------------------
 * Software power-off
 * ------------------------------------------------------------------------- */

/* Task Manager / Switcher soft power-off.
 *
 * On a real Risc PC the PSU has no software control, so the desktop's
 * "Shutdown" ends on a dead "you may now switch off your computer" screen.
 * RPCEmu can genuinely "power off" - by closing the application - so we make
 * the Switcher believe the hardware supports it. It derives its soft-poweroff
 * flag from OS_ReadSysInfo 8: (returned flags & valid mask) & PowerDownFeature
 * (bit 3). Forcing the "& bit 3" step into an unconditional "load bit 3" leaves
 * the flag always set, so Shutdown issues OS_Reset with R0 = "&OFF"
 * (&46464F26) - the documented soft power-off request - which the emulator
 * catches in the SWI handler (see opSWI / OS_Reset) and turns into a clean
 * quit.
 *
 * The instruction site moves between ROM builds, so it is found by its exact
 * five-word sequence rather than a fixed address:
 *     MOV  r0,#8            ; OS_ReadSysInfo reason 8
 *     SWI  XOS_ReadSysInfo
 *     ...
 *     AND  r1,r1,r2         ; flags & valid
 *     AND  r1,r1,#8         ; & PowerDownFeature   <-- rewritten
 *     STRB r1,[r12,#0x34]   ; -> soft_poweroff
 * The AND #8 is only rewritten when preceded by that context, so it cannot
 * match an unrelated "AND r1,r1,#8".
 */
#define PWR_MOV_R0_8		0xe3a00008u	/* MOV  r0,#8            */
#define PWR_SWI_READSYSINFO	0xef020058u	/* SWI  XOS_ReadSysInfo  */
#define PWR_AND_R1_R1_R2	0xe0011002u	/* AND  r1,r1,r2         */
#define PWR_AND_R1_R1_8		0xe2011008u	/* AND  r1,r1,#8         */
#define PWR_STRB_R1_R12_34	0xe5cc1034u	/* STRB r1,[r12,#0x34]   */
#define PWR_MOV_R1_8		0xe3a01008u	/* MOV  r1,#8  (replacement) */

/* How far the AND/AND/STRB triplet may sit after the ReadSysInfo call. */
#define PWR_TRIPLET_WINDOW	8u

static void
rom_patch_software_poweroff(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	size_t w;

	if (words < 2) {
		return;
	}

	for (w = 0; w + 1 < words; w++) {
		size_t j;

		if (rom[w] != PWR_MOV_R0_8 || rom[w + 1] != PWR_SWI_READSYSINFO) {
			continue;
		}

		/* Found the ReadSysInfo 8 call; look for the flag-store triplet a
		   few instructions on. */
		for (j = w + 2; j + 2 < words && j <= w + 2 + PWR_TRIPLET_WINDOW; j++) {
			if (rom[j]     == PWR_AND_R1_R1_R2 &&
			    rom[j + 1] == PWR_AND_R1_R1_8 &&
			    rom[j + 2] == PWR_STRB_R1_R12_34)
			{
				rom[j + 1] = PWR_MOV_R1_8;
				rpclog("rom_patch: applied: software power-off enabled (Switcher at 0x%06x)\n",
				       (unsigned) ((j + 1) * 4));
				return;
			}
		}
	}
}

/* -------------------------------------------------------------------------
 * Kinetic 16MB VRAM (content-located prototype)
 * ------------------------------------------------------------------------- */

/*
 * PROTOTYPE - content-located Kinetic 16MB VRAM enablement.
 *
 * Background: on the Kinetic model RISC OS relocates itself into the on-card
 * SDRAM and rebuilds its memory map from the HAL physical-memory table. That
 * path re-derives the VRAM size from the HAL's VRAMWidth readback and discards
 * the kernel's own sizer result, so a 16MB VRAM fitting is not honoured and the
 * desktop is left with a tiny screen allocation. Enabling it needs three
 * co-operating edits (mirroring what a fixed-offset patcher would do, but
 * located by content so it is not welded to one ROM build):
 *
 *   1. Kernel VRAM sizer cap: the "MOVEQ R6,#8" that caps the sizer result.
 *      Located via its unique preamble
 *          MOV r0,#0x02000000 / MOV r2,#0x41 / STRB r2,[ip,#0x8c] /
 *          ADD r1,r0,#0x400000
 *      then the MOVEQ R6 a fixed few words later. Raise #8 -> #16.
 *   2. HAL VRAMWidth cap: a "MOVEQ IP,#8" that clamps the readback decode.
 *      Raise #8 -> #16.
 *   3. HAL physinfo gap-fill: the "fill end-of-VRAM..0x03000000" loop is a
 *      do-while; with 16MB VRAM the region fills the whole window so the count
 *      is zero and the loop would wrap. Made conditional (LSR->LSRS, BL->BLNE),
 *      located via the unique "RSB r3,r3,#0x1000000 / LSR r1,r3,#15" pair.
 *
 * STATUS: this is scaffolding, deliberately gated to do nothing on a normal
 * build. It only writes when the machine is Kinetic AND 16MB VRAM is actually
 * configured - and Kinetic VRAM is currently hard-clamped to 2MB elsewhere
 * (the HAL VRAM>2MB path still faults on some ROM builds during a full !Boot,
 * an abort we have not yet traced). So in practice it locates + logs but does
 * not fire until that clamp is lifted, which awaits (a) a ROM to validate
 * against and (b) the residual abort being tracked down. Keeping the patch
 * here, content-located and logging, is the foundation for that work.
 */
#define KVRAM_SIZER_MOV_R0	0xe3a00402u	/* MOV r0,#0x02000000    */
#define KVRAM_SIZER_MOV_R2	0xe3a02041u	/* MOV r2,#0x41          */
#define KVRAM_SIZER_STRB_R2	0xe5cc208cu	/* STRB r2,[ip,#0x8c]    */
#define KVRAM_SIZER_ADD_R1	0xe2801501u	/* ADD r1,r0,#0x400000   */
#define KVRAM_SIZER_MOVEQ_R6_8	0x03a06008u	/* MOVEQ r6,#8           */
#define KVRAM_SIZER_MOVEQ_R6_16	0x03a06010u	/* MOVEQ r6,#16          */
#define KVRAM_SIZER_MOVEQ_OFF	5u		/* words from preamble start to MOVEQ */

#define KVRAM_WIDTH_MOVEQ_IP_8	0x03a0c008u	/* MOVEQ ip,#8           */
#define KVRAM_WIDTH_MOVEQ_IP_16	0x03a0c010u	/* MOVEQ ip,#16          */

#define KVRAM_GAP_RSB_R3	0xe2633401u	/* RSB r3,r3,#0x1000000  */
#define KVRAM_GAP_LSR_R1	0xe1a017a3u	/* LSR r1,r3,#15         */
#define KVRAM_GAP_LSRS_R1	0xe1b017a3u	/* LSRS r1,r3,#15        */
#define KVRAM_GAP_BL_MASK	0xff000000u
#define KVRAM_GAP_BL		0xeb000000u	/* BL <offset>           */
#define KVRAM_GAP_BLNE		0x1b000000u	/* BLNE <offset>         */
#define KVRAM_GAP_BL_OFF	2u		/* words from LSR to the BL */

static void
rom_patch_kinetic_vram(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	uint32_t seq[4];
	size_t at;
	int applied = 0;

	/* Only relevant to a Kinetic machine actually fitted with >8MB VRAM. */
	if (machine.model != Model_Kinetic || config.vram_size < 16) {
		return;
	}

	/* 1. Kernel VRAM sizer cap #8 -> #16, anchored on its unique preamble. */
	seq[0] = KVRAM_SIZER_MOV_R0;
	seq[1] = KVRAM_SIZER_MOV_R2;
	seq[2] = KVRAM_SIZER_STRB_R2;
	seq[3] = KVRAM_SIZER_ADD_R1;
	if (find_unique_sequence(words, seq, 4, &at) &&
	    at + KVRAM_SIZER_MOVEQ_OFF < words &&
	    rom[at + KVRAM_SIZER_MOVEQ_OFF] == KVRAM_SIZER_MOVEQ_R6_8)
	{
		rom[at + KVRAM_SIZER_MOVEQ_OFF] = KVRAM_SIZER_MOVEQ_R6_16;
		rpclog("rom_patch: applied: Kinetic 16MB VRAM sizer cap (at 0x%06x)\n",
		       (unsigned) ((at + KVRAM_SIZER_MOVEQ_OFF) * 4));
		applied++;
	}

	/* 2. HAL VRAMWidth cap #8 -> #16 (only if unambiguous). */
	if (find_unique_word(words, KVRAM_WIDTH_MOVEQ_IP_8, &at)) {
		rom[at] = KVRAM_WIDTH_MOVEQ_IP_16;
		rpclog("rom_patch: applied: Kinetic 16MB VRAM HAL VRAMWidth cap (at 0x%06x)\n",
		       (unsigned) (at * 4));
		applied++;
	}

	/* 3. HAL physinfo gap-fill: make the do-while conditional. Anchored on
	      the unique RSB/LSR pair, with the BL a fixed couple of words on. */
	seq[0] = KVRAM_GAP_RSB_R3;
	seq[1] = KVRAM_GAP_LSR_R1;
	if (find_unique_sequence(words, seq, 2, &at) &&
	    at + KVRAM_GAP_BL_OFF < words &&
	    (rom[at + KVRAM_GAP_BL_OFF] & KVRAM_GAP_BL_MASK) == KVRAM_GAP_BL)
	{
		rom[at + 1] = KVRAM_GAP_LSRS_R1;
		rom[at + KVRAM_GAP_BL_OFF] =
		    (rom[at + KVRAM_GAP_BL_OFF] & ~KVRAM_GAP_BL_MASK) | KVRAM_GAP_BLNE;
		rpclog("rom_patch: applied: Kinetic 16MB VRAM HAL gap-fill guard (at 0x%06x)\n",
		       (unsigned) (at * 4));
		applied++;
	}

	if (applied != 3) {
		rpclog("rom_patch: Kinetic 16MB VRAM: %d/3 sites located - not fully enabled on this ROM\n",
		       applied);
	}
}

/* -------------------------------------------------------------------------
 * NCOS POST bypass
 * ------------------------------------------------------------------------- */

/**
 * Patch Netstation versions of NCOS to bypass the results of the POST that we
 * currently fail.
 */
static void
rom_patch_ncos_post(void)
{
	/* NCOS 0.10 */
	if (rom[0x2714 >> 2] == 0xe1d70000) {
		rom[0x2714 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
		rom[0x2794 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
	}

	/* NCOS 1.06/1.11 */
	if (rom[0x26f0 >> 2] == 0xe1d70000) {
		rom[0x26f0 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
		rom[0x2750 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
	}
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

void
rom_patch_apply(size_t rom_bytes)
{
	/* Fixed-offset VRAM cap (RISC OS 3/4/6). */
	rom_patch_vram_cap();

	/* Kinetic 16MB VRAM prototype (Kinetic + 16MB VRAM only). */
	rom_patch_kinetic_vram(rom_bytes);

	/* Free the desktop from the emulated-away VIDC20 pixel-clock limit so the
	   larger monitor-definition modes become selectable. */
	rom_patch_display_clock(rom_bytes);

	/* Advertise a capable monitor to MonitorType Auto via a populated EDID. */
	rom_patch_monitor_edid(rom_bytes);

	/* Let the desktop "Shutdown" quit the application cleanly. */
	rom_patch_software_poweroff(rom_bytes);

	/* Bypass the failing NCOS power-on self-test. */
	rom_patch_ncos_post();
}
