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

/* -------------------------------------------------------------------------
 * Video memory the mode list is vetted against
 * ------------------------------------------------------------------------- */

/* RISC OS decides which screen modes exist by measuring each one against the
   video memory it believes the machine has, and that figure comes from the
   fitted VRAM. A GraphicsV driver with a framestore of its own is invisible to
   it: with 2MB of VRAM the graphics card is held to 2MB of screen however much
   it carries, so the deeper modes it exists to provide are never offered.

   The figure is produced by the kernel's GetBandwidthAndSize macro, which reads
   VideoSizeFlags (the VRAM size) out of low memory and rounds it down to a page.
   Its own source carries the note "Sort out GetBandwidthAndSize", and the sorting
   out is already sitting in the workspace next door: TotalScreenSize, which the
   kernel maintains as the screen memory of whichever display driver is current -
   the screen dynamic area under VIDC20, and whatever GraphicsV_FramestoreAddress
   reported under a card. The kernel's own OS_CheckModeValid uses it for exactly
   this purpose.

   So this patch redirects the load. It leaves VIDC20 behaving as before (there
   TotalScreenSize is the VRAM), and lets the card offer what it can actually
   hold. Applied only when the card is fitted, so a machine without one is
   untouched.

     mov  r5, #0                 ->  mov  r5, #<workspace base>
     ldr  r5, [r5, #VideoSize..] ->  ldr  r5, [r5, #TotalScreenSize]
     mov  r4, #100*1024*1024         (bandwidth, left alone: ScreenModes
     mov  r5, r5, lsr #12             ignores it)
     mov  r5, r5, lsl #12

   Both replacements are single words in the slots already there, so nothing
   moves. The offsets are read out of the ROM rather than assumed, and the patch
   is skipped unless every part of the pattern is found. */

#define GBS_MOV_R5_0		0xe3a05000u	/* mov r5, #0 (ZeroPage) */
#define GBS_LDR_R5_MASK		0xfffff000u	/* ldr r5, [r5, #imm12] */
#define GBS_LDR_R5_MATCH	0xe5955000u
#define GBS_MOV_R4_100MB	0xe3a04519u	/* mov r4, #100*1024*1024 */
#define GBS_LSR_R5_12		0xe1a05625u	/* mov r5, r5, lsr #12 */
#define GBS_LSL_R5_12		0xe1a05605u	/* mov r5, r5, lsl #12 */

/* The anchor that tells us where TotalScreenSize lives: OS_CheckModeValid's own
   memory check, which is unmistakable.

     ldr   r9, [ip, #GraphicsVFeatures]
     tst   r9, #GVDisplayFeature_VariableFramestore
     mvnne r9, #0
     ldreq r9, [ip, #TotalScreenSize]
     cmp   r9, fp, lsl sl                     */
#define CMV_TST_R9_20		0xe3190020u
#define CMV_MVNNE_R9_0		0x13e09000u
#define CMV_LDREQ_R9_MASK	0xfffff000u
#define CMV_LDREQ_R9_MATCH	0x059c9000u
#define CMV_CMP_R9_FP_LSL_SL	0xe1590a1bu
#define CMV_MOV_IP_MASK		0xfffff000u	/* mov ip, #imm (the VDU workspace) */
#define CMV_MOV_IP_MATCH	0xe3a0c000u

static uint32_t
rotr32(uint32_t v, unsigned n)
{
	n &= 31;

	return n ? ((v >> n) | (v << (32 - n))) : v;
}

static uint32_t
rotl32(uint32_t v, unsigned n)
{
	n &= 31;

	return n ? ((v << n) | (v >> (32 - n))) : v;
}

/** Decode an ARM data-processing immediate: an 8-bit value rotated right. */
static uint32_t
arm_immediate(uint32_t instr)
{
	return rotr32(instr & 0xffu, ((instr >> 8) & 0xfu) * 2);
}

/**
 * Encode "mov Rd, #value", or 0 if the value is not one an ARM immediate can
 * express.
 */
static uint32_t
arm_encode_mov(unsigned rd, uint32_t value)
{
	unsigned rot;

	for (rot = 0; rot < 16; rot++) {
		const uint32_t imm = rotl32(value, rot * 2);

		if (imm <= 0xffu) {
			return 0xe3a00000u | (rd << 12) | (rot << 8) | imm;
		}
	}

	return 0;
}

/**
 * Find where the kernel keeps TotalScreenSize, by reading it out of the one
 * place that already uses it.
 *
 * @param words     Size of the loaded ROM image in words
 * @param base      Filled in with the VDU workspace address
 * @param offset    Filled in with TotalScreenSize's offset within it
 * @return non-zero if both were found
 */
static int
rom_find_total_screen_size(size_t words, uint32_t *base, uint32_t *offset)
{
	size_t i;

	for (i = 0; i + 3 < words; i++) {
		size_t back;

		if (rom[i] != CMV_TST_R9_20 || rom[i + 1] != CMV_MVNNE_R9_0) {
			continue;
		}
		if ((rom[i + 2] & CMV_LDREQ_R9_MASK) != CMV_LDREQ_R9_MATCH) {
			continue;
		}
		if (rom[i + 3] != CMV_CMP_R9_FP_LSL_SL) {
			continue;
		}

		/* The workspace pointer is set up earlier in the same routine. */
		for (back = 1; back <= 128 && back <= i; back++) {
			const uint32_t instr = rom[i - back];

			if ((instr & CMV_MOV_IP_MASK) != CMV_MOV_IP_MATCH) {
				continue;
			}
			*base = arm_immediate(instr);
			*offset = rom[i + 2] & 0xfffu;

			/* Sanity: a page-aligned low-memory workspace, and an offset
			   the replacement instructions can encode. */
			if (*base == 0 || (*base & 0xfffu) != 0 || *base > 0x10000u) {
				return 0;
			}
			return 1;
		}
		return 0;
	}
	return 0;
}

/**
 * Point the mode list's video memory limit at the current display driver's
 * screen memory rather than at the fitted VRAM.
 *
 * @param rom_bytes Number of bytes actually loaded into the ROM image
 */
static void
rom_patch_video_memory_limit(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	uint32_t ws_base = 0, tss_offset = 0;
	uint32_t mov_ws;
	unsigned patched = 0;
	size_t i;

	if (!config.gfxcard_enabled) {
		return;
	}

	if (!rom_find_total_screen_size(words, &ws_base, &tss_offset)) {
		rpclog("rom_patch: could not locate TotalScreenSize - the graphics card "
		       "will be limited to modes the fitted VRAM could hold\n");
		return;
	}

	mov_ws = arm_encode_mov(5, ws_base);
	if (mov_ws == 0) {
		rpclog("rom_patch: VDU workspace at 0x%x is not an encodable immediate - "
		       "leaving the mode list's memory limit alone\n", (unsigned) ws_base);
		return;
	}

	for (i = 0; i + 4 < words; i++) {
		if (rom[i] != GBS_MOV_R5_0) {
			continue;
		}
		if ((rom[i + 1] & GBS_LDR_R5_MASK) != GBS_LDR_R5_MATCH) {
			continue;
		}
		if (rom[i + 2] != GBS_MOV_R4_100MB ||
		    rom[i + 3] != GBS_LSR_R5_12 || rom[i + 4] != GBS_LSL_R5_12)
		{
			continue;
		}

		rom[i] = mov_ws;			/* mov r5, #<workspace> */
		rom[i + 1] = GBS_LDR_R5_MATCH | tss_offset;	/* ldr r5, [r5, #TSS] */
		patched++;
	}

	if (patched == 0) {
		rpclog("rom_patch: video memory limit pattern not found - the graphics "
		       "card will be limited to modes the fitted VRAM could hold\n");
		return;
	}

	rpclog("rom_patch: applied: mode list vetted against the current display "
	       "driver's memory (TotalScreenSize at 0x%x + 0x%x), %u site%s\n",
	       (unsigned) ws_base, (unsigned) tss_offset, patched,
	       patched == 1 ? "" : "s");
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
/* Where the block was found and what it was built from, so the same block can
   be rebuilt without re-scanning when the configured screen size changes. The
   offset is into the loaded ROM image, which stays mapped for the life of the
   machine. */
static uint8_t edid_base_block[EDID_BLOCK_SIZE];
static size_t edid_rom_offset;
static int edid_base_known;

/*
 * The largest mode this monitor should claim it can show.
 *
 * Deliberately NOT the configured screen size. That is one desktop; this is the
 * monitor's capability, and RISC OS will only accept a mode the monitor declares
 * - so bounding the advertisement by the configured size makes every larger size
 * in the emulator's own chooser unselectable. The chooser is bounded by display
 * memory, so this is too, and the two then agree.
 *
 * The host display's FULL geometry is the ceiling, not its work area: full
 * screen exists, and the work area is a fact about where a window may sit, not
 * about what the screen can show. With no host display to ask - a headless run -
 * there is nothing to bound by beyond the framestore.
 */
static void
rom_patch_edid_ceiling(unsigned native_x, unsigned native_y,
                       unsigned *max_x, unsigned *max_y)
{
	unsigned host_x = 0, host_y = 0;
	unsigned fitted_x = 0, fitted_y = 0;

	if (!rpcemu_get_host_display(&host_x, &host_y)) {
		host_x = EDID_NATIVE_MAX_X;
		host_y = EDID_NATIVE_MAX_Y;
	}
	if (host_x > EDID_NATIVE_MAX_X) {
		host_x = EDID_NATIVE_MAX_X;
	}
	if (host_y > EDID_NATIVE_MAX_Y) {
		host_y = EDID_NATIVE_MAX_Y;
	}

	/* Budgeted at the shallowest depth, exactly as the chooser is: a mode that
	   fits at 8bpp is one the user may legitimately pick, and the depth at that
	   size is the guest's business afterwards. */
	if (!display_mode_fit(host_x, host_y, DISPLAY_BPP_SHALLOWEST,
	                      rpcemu_display_memory(), &fitted_x, &fitted_y)) {
		fitted_x = native_x;
		fitted_y = native_y;
	}

	*max_x = fitted_x > native_x ? fitted_x : native_x;
	*max_y = fitted_y > native_y ? fitted_y : native_y;
}

static void
rom_patch_monitor_edid(size_t rom_bytes)
{
	const size_t words = rom_bytes / 4;
	uint8_t *rb = (uint8_t *) rom;
	size_t found = (size_t) -1;
	size_t hits = 0;
	unsigned native_x, native_y;
	unsigned max_x = 0, max_y = 0;
	unsigned bound_x = 0, bound_y = 0;
	uint8_t base[EDID_BLOCK_SIZE];
	uint8_t block[EDID_BLOCK_SIZE];
	size_t byte;

	edid_base_known = 0;

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

	/* Bound by whatever the screen-size setting says the monitor should offer:
	   the display's work area, the whole display, or the fixed size asked for.
	   See rpcemu_edid_bound(). Falls back to a sensible high default when there
	   is no display to ask about at all. Clamp to keep it sane. */
	if (!rpcemu_edid_bound(&bound_x, &bound_y)) {
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
	   framestore, which is the point of it. A machine with no VRAM fitted takes
	   screen memory from DRAM instead, so there is no figure to reason about and
	   the limit is skipped.

	   Budgeted at the shallowest depth, and it has to be: rpcemu_edid_bound()
	   hands a size the user named straight through, and re-fitting it at 32bpp
	   here would advertise a smaller preferred timing than the one they chose.
	   RISC OS validates against the monitor definition in force, so the mode
	   they asked for would then be refused and the setting would appear to do
	   nothing - issue #207. The size still cannot exceed what the framestore
	   holds at any depth; what it no longer does is demand room for a depth
	   nobody asked for. The fallback is unaffected, since the default size has
	   already been fitted at DISPLAY_BPP_SAFE before it arrives here. */
	if (!display_mode_fit(bound_x, bound_y, DISPLAY_BPP_SHALLOWEST,
	                             rpcemu_display_memory(), &native_x, &native_y))
	{
		rpclog("rom_patch: no standard mode fits %zu KB of display memory "
		       "within %ux%u - leaving the monitor EDID alone\n",
		       rpcemu_display_memory() / 1024, bound_x, bound_y);
		return;
	}

	memcpy(base, &rb[found], EDID_BLOCK_SIZE);
	rom_patch_edid_ceiling(native_x, native_y, &max_x, &max_y);
	edid_build_from_base(block, base, native_x, native_y,
	                     max_x, max_y, EDID_NATIVE_HZ);
	memcpy(&rb[found], block, EDID_BLOCK_SIZE);

	/* Kept so a later screen-size change can rebuild without re-scanning. */
	memcpy(edid_base_block, base, EDID_BLOCK_SIZE);
	edid_rom_offset = found;
	edid_base_known = 1;

	/* Publish it: the graphics card serves this same block over DDC, because
	   RISC OS re-reads the EDID from the display driver when the driver
	   changes and would otherwise end up with a different monitor. */
	edid_publish(block);

	rpclog("rom_patch: applied: monitor EDID replaced, native %ux%u@%u, "
	       "advertising up to %ux%u (block at 0x%06x)\n",
	       native_x, native_y, EDID_NATIVE_HZ, max_x, max_y,
	       (unsigned) found);
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

/*
 * Rebuild the monitor EDID for the screen size now configured, and republish it.
 *
 * Called when the configured size changes while the machine is running, so the
 * block the graphics card serves over DDC describes the monitor the machine is
 * being asked for rather than the one it booted with.
 *
 * What this does NOT do is make RISC OS re-read it. The kernel builds its mode
 * table from the EDID when the display driver starts and does not poll, so the
 * new block is picked up on the next driver change or the next boot. That is why
 * the size chooser still tells the user to restart when the size it wants is one
 * the block in force does not declare: this keeps the two copies honest, it does
 * not make the change live.
 */
void
rom_patch_refresh_monitor_edid(void)
{
	uint8_t *rb = (uint8_t *) rom;
	unsigned native_x, native_y;
	unsigned max_x = 0, max_y = 0;
	unsigned bound_x = 0, bound_y = 0;
	uint8_t block[EDID_BLOCK_SIZE];

	if (!edid_base_known) {
		return;		/* No EDID in this ROM (RISC OS 3/4), nothing to do. */
	}

	if (!rpcemu_edid_bound(&bound_x, &bound_y)) {
		bound_x = EDID_NATIVE_DEFAULT_X;
		bound_y = EDID_NATIVE_DEFAULT_Y;
	}
	if (bound_x > EDID_NATIVE_MAX_X) {
		bound_x = EDID_NATIVE_MAX_X;
	}
	if (bound_y > EDID_NATIVE_MAX_Y) {
		bound_y = EDID_NATIVE_MAX_Y;
	}

	if (!display_mode_fit(bound_x, bound_y, DISPLAY_BPP_SHALLOWEST,
	                      rpcemu_display_memory(), &native_x, &native_y)) {
		return;
	}

	rom_patch_edid_ceiling(native_x, native_y, &max_x, &max_y);
	edid_build_from_base(block, edid_base_block, native_x, native_y,
	                     max_x, max_y, EDID_NATIVE_HZ);
	memcpy(&rb[edid_rom_offset], block, EDID_BLOCK_SIZE);
	edid_publish(block);

	rpclog("rom_patch: monitor EDID rebuilt, native %ux%u@%u, "
	       "advertising up to %ux%u (takes effect on the next display driver "
	       "start or restart)\n",
	       native_x, native_y, EDID_NATIVE_HZ, max_x, max_y);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

void
rom_patch_apply(size_t rom_bytes)
{
	/* Fixed-offset VRAM cap (RISC OS 3/4/6). */
	rom_patch_vram_cap();

	/* Free the desktop from the emulated-away VIDC20 pixel-clock limit so the
	   larger monitor-definition modes become selectable. */
	rom_patch_display_clock(rom_bytes);

	/* Advertise a capable monitor to MonitorType Auto via a populated EDID. */
	rom_patch_monitor_edid(rom_bytes);
	rom_patch_video_memory_limit(rom_bytes);

	/* Let the desktop "Shutdown" quit the application cleanly. */
	rom_patch_software_poweroff(rom_bytes);

	/* Bypass the failing NCOS power-on self-test. */
	rom_patch_ncos_post();
}
