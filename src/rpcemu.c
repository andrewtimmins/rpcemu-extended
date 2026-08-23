/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

/* Main loop
   Should be platform independent */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
/* Brings in Winsock2 (before windows.h) plus the Win32 API used for Sleep()
   and the WSAStartup()/WSACleanup() bootstrap below. */
#include "socket-compat.h"
#include <timeapi.h> /* timeBeginPeriod/timeEndPeriod - see rpcemu_start() */
#endif

#include "rpcemu.h"
#include "display_mode.h"
#include "mem.h"
#include "vidc20.h"
#include "keyboard.h"
#include "sound.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "arm_disasm.h"
#include "cmos.h"
#include "serial_host.h"
#include "superio.h"
#include "i8042.h"
#include "romload.h"
#include "cp15.h"
#include "cdrom-iso.h"
#include "podulerom.h"
#include "gfxcard.h"
#include "hostclipboard.h"
#include "usb_ohci.h"
#include "usbcard.h"
#include "openbus.h"
#include "openbus_stub.h"
#include "podules.h"
#include "fdc.h"
#include "hostfs.h"
#include "hostcmd.h"
#include "debugcmd.h"
#include "debugexpr.h"
#include "disc.h"
#include "disc_adf.h"
#include "disc_hfe.h"
#include "disc_mfm_common.h"
#include "parallel.h"
#include "serial.h"
#include "printer.h"
#include "peripheral_config.h"
#include "savestate.h"

#ifdef RPCEMU_NETWORKING
#include "network.h"
#endif

char discname[2][260]={"boot.adf","notboot.adf"};

Machine machine; /**< The details of the current machine being emulated */

/* Host display geometry, published by the front-end so the synthesised monitor
   EDID can advertise a native mode matching the real screen. Zero until set.

   Both the full geometry and the work area are kept, because they answer
   different questions: the geometry is what the display can show, the work area
   is what a window may occupy. See rpcemu_edid_bound(). */
static unsigned host_display_width = 0;
static unsigned host_display_height = 0;
static unsigned host_display_work_width = 0;
static unsigned host_display_work_height = 0;
static unsigned host_display_hz = 0;

/* The screen size being asked of the guest, and the generation that identifies
   it. Separate from the host display, because the two are no longer the same
   thing: with ScreenSize_MatchWindow this is the window's client area, and with
   ScreenSize_Fixed it is whatever was configured.

   The generation counter changes whenever the request does, which is how the
   guest support module notices it should follow: it records the generation it
   last acted on and compares. A counter rather than a flag, so a change that
   arrives while the guest is busy is not lost, and so nothing has to be
   "consumed" by a reader. */
static unsigned guest_size_width = 0;
static unsigned guest_size_height = 0;
static uint32_t guest_size_generation = 0;

void
rpcemu_set_host_display(unsigned width, unsigned height, unsigned hz,
                        unsigned work_width, unsigned work_height)
{
	if (width == host_display_width && height == host_display_height &&
	    hz == host_display_hz && work_width == host_display_work_width &&
	    work_height == host_display_work_height)
	{
		return;		/* Unchanged */
	}

	host_display_width = width;
	host_display_height = height;
	host_display_hz = hz;
	host_display_work_width = work_width;
	host_display_work_height = work_height;

	rpclog("Display: host display now %ux%u@%u, work area %ux%u\n",
	       width, height, hz, work_width, work_height);
}

int
rpcemu_get_host_display(unsigned *width, unsigned *height)
{
	if (host_display_width == 0 || host_display_height == 0) {
		return 0;
	}
	*width = host_display_width;
	*height = host_display_height;
	return 1;
}

int
rpcemu_default_screen_size(unsigned *width, unsigned *height)
{
	/* The work area rather than the whole display: the window ends up this size,
	   and one taller than the work area opens with its title bar above the top of
	   the screen, where it cannot be grabbed to move the window back. */
	const unsigned max_w = host_display_work_width != 0
	    ? host_display_work_width : host_display_width;
	const unsigned max_h = host_display_work_height != 0
	    ? host_display_work_height : host_display_height;

	if (max_w == 0 || max_h == 0) {
		return 0;
	}

	return display_mode_fit(max_w, max_h, 4, rpcemu_display_memory(),
	                        width, height);
}

int
rpcemu_edid_bound(unsigned *width, unsigned *height)
{
	if (config.screen_size_x != 0 && config.screen_size_y != 0) {
		*width = config.screen_size_x;
		*height = config.screen_size_y;
		return 1;
	}

	return rpcemu_default_screen_size(width, height);
}

int
rpcemu_guest_size_for(unsigned width, unsigned height,
                      unsigned *fitted_width, unsigned *fitted_height)
{
	if (width == 0 || height == 0) {
		return 0;
	}

	return display_mode_fit(width, height, 4, rpcemu_display_memory(),
	                        fitted_width, fitted_height);
}

void
rpcemu_request_guest_size(unsigned width, unsigned height)
{
	rpcemu_request_guest_size_ex(width, height, 0);
}

void
rpcemu_request_guest_size_ex(unsigned width, unsigned height, int force)
{
	unsigned fitted_w = 0, fitted_h = 0;

	if (width == 0 || height == 0) {
		return;
	}

	/* Quantise before comparing. A window drag walks through every intermediate
	   width, and all but a few of those land on the same standard mode; bumping
	   the generation for each would have the guest reflowing its desktop over
	   and over to arrive where it already was. */
	if (!display_mode_fit(width, height, 4, rpcemu_display_memory(),
	                      &fitted_w, &fitted_h))
	{
		return;
	}

	/*
	 * The same size as last time is normally nothing to do. Not when the size
	 * was named: this records what was ASKED for, and RISC OS may have moved
	 * since - it changes mode from its own end, and a refused size leaves this
	 * naming one it would not take. Asking again for the size already recorded
	 * then did nothing at all, no generation bump, so the guest was never told
	 * and the request read as a refusal two seconds later.
	 */
	if (!force && fitted_w == guest_size_width &&
	    fitted_h == guest_size_height) {
		return;
	}

	guest_size_width = fitted_w;
	guest_size_height = fitted_h;
	guest_size_generation++;

	rpclog("Display: guest screen size requested %ux%u (from %ux%u, generation %u)\n",
	       fitted_w, fitted_h, width, height, (unsigned) guest_size_generation);
}

/**
 * The display mode the guest should adopt to match the host, and the generation
 * that identifies it.
 *
 * Bounded by the host display and by what the fitted VRAM can hold, so the guest
 * can act on the answer without knowing anything about either. Deliberately the
 * same calculation the synthesised EDID uses, budgeting 32bpp, so the mode
 * offered at boot and the mode requested later agree.
 *
 * @param[out] width      Mode width, untouched if there is nothing to report
 * @param[out] height     Mode height
 * @param[out] hz         Refresh rate, 0 if the front-end did not supply one
 * @param[out] generation Changes whenever the host display changes
 *
 * @return non-zero if a mode is available to report
 */
/**
 * How much memory a display mode has to fit in, in bytes.
 *
 * The fitted VRAM, unless the graphics card is present: the card carries its own
 * framestore and is the whole reason for it, so with the card fitted the modes
 * worth offering are the ones it can hold rather than the ones VRAM can.
 *
 * @return Budget in bytes, or 0 if there is no figure to reason about (a machine
 *         with no VRAM takes screen memory from DRAM instead)
 */
size_t
rpcemu_display_memory(void)
{
	if (config.gfxcard_enabled) {
		return GFXCARD_FB_SIZE;
	}

	return (size_t) config.vram_size * 1024 * 1024;
}

int
rpcemu_guest_display_target(unsigned *width, unsigned *height, unsigned *hz,
                            uint32_t *generation)
{
	*generation = guest_size_generation;
	*hz = host_display_hz;

	/*
	 * Nothing is reported until a size has actually been asked for. A machine
	 * that boots into the mode its EDID advertises and is left alone therefore
	 * answers "no mode" for its whole life, and the guest support module never
	 * even takes a baseline - which is what it should do, since a mode change
	 * reflows every window on the RISC OS desktop and nobody asked for that to
	 * happen behind their back.
	 */
	if (guest_size_width == 0 || guest_size_height == 0) {
		return 0;
	}

	/* Already fitted to the standard-mode list and to the display memory by
	   rpcemu_request_guest_size(), so the guest can use it as it stands. */
	*width = guest_size_width;
	*height = guest_size_height;
	return 1;
}

/** Array of details of models the emulator can emulate, must be kept in sync with
    Model enum in rpcemu.h */
const Model_Details models[] = {
	{ "Risc PC - ARM610",                "RPC610", CPUModel_ARM610,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - ARM710",                "RPC710", CPUModel_ARM710,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - StrongARM",             "RPCSA",  CPUModel_SA110,     IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "A7000",                           "A7000",  CPUModel_ARM7500,   IOMDType_ARM7500,   SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "A7000+",                          "A7000+", CPUModel_ARM7500FE, IOMDType_ARM7500FE, SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - ARM810 (experimental)", "RPC810", CPUModel_ARM810,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Phoebe (RPC2)",                   "Phoebe", CPUModel_SA110,     IOMDType_IOMD2,     SuperIOType_FDC37C672,   I2C_PCF8583 | I2C_SPD_DIMM0 },
	{ "Risc PC - Kinetic",               "Kinetic",CPUModel_SA110,     IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 }
};

Config config = {
	"",			/* name */
	"",			/* hd4_path (empty = use machine directory) */
	"",			/* hostfs_path (empty = <machine dir>/hostfs) */
	"",			/* rom_dir (empty = use 'roms' folder directly) */
	0,			/* mem_size */
	0,			/* vram_size */
	NULL,			/* username */
	NULL,			/* ipaddress */
	NULL,			/* macaddress */
	NULL,			/* bridgename */
	0,			/* refresh */
	1,			/* soundenabled */
	1,			/* cdromenabled */
	0,			/* cdromtype (0=disabled, 1=ISO, 2=ioctl) */
	"",			/* isoname */
	1,			/* mousehackon */
	0,			/* mousetwobutton */
	NetworkType_Off,	/* network_type */
	0,			/* cpu_idle */
	1,			/* show_fullscreen_message */
	DisplayScaling_ActualSize,	/* display_scaling */
	0,			/* screen_size_x (0: chosen for this display at first start) */
	0,			/* screen_size_y */
	0,			/* gfxcard_enabled (OFF: needs its guest driver) */
	{ UsbAttachment_None,	/* usb_port: nothing plugged into any USB port */
	  UsbAttachment_None,
	  UsbAttachment_None,
	  UsbAttachment_None },
	{ "", "", "", "" },	/* usb_host: no host device named for any port */
	0,			/* gfxcard_boot_display (OFF: the card is taken up on request) */
	NULL,			/* network_capture */
	0,			/* vnc_enabled */
	5900,			/* vnc_port */
	"",			/* vnc_password */
	"",			/* vnc_password_readonly */
	1,			/* hostcmd_enabled (ON by default) */
	"",			/* hostcmd_socket (empty => <datadir>hostcmd.sock) */
	1,			/* debug_enabled (ON by default) */
	"",			/* debug_socket (empty => <datadir>rpcemu-debug.sock) */
	Model_RPCARM710,	/* model (configured machine model) */
	0,			/* clipboard_enabled (OFF: it shares your host clipboard with the guest) */
	0,			/* start_fullscreen (OFF by default) */
	0,			/* suspend_on_exit (OFF by default) */
};

/* Performance measuring variables */
int updatemips = 0; /**< bool of whether to update the mips speed in the program title bar */
Perf perf = {
	0.0f, /* mips */
	0.0f, /* mhz */
	0.0f, /* tlb_sec */
	0.0f, /* flush_sec */
	0,    /* mips_count */
	0.0f  /* mips_total */
};

PortForwardRule port_forward_rules[MAX_PORT_FORWARDS]; ///< Port forward rules accross the NAT

/*
 * What the OPEN Bus subsystem is given to work with.
 *
 * A second bus master reaches memory PHYSICALLY - it has none of the ARM's MMU,
 * which is precisely why the Aleph One PC card's Gemini ASIC needed mapping
 * registers of its own. Its interrupt goes down the podule interrupt line, as the
 * Risc PC TRM specifies for nPIRQ; telling a podule apart from the second master
 * is then the card's job, via registers it has to provide itself.
 */
static uint32_t
openbus_host_read32(uint32_t phys_addr)
{
	return mem_phys_read32(phys_addr);
}

static void
openbus_host_write32(uint32_t phys_addr, uint32_t val)
{
	mem_phys_write32(phys_addr, val);
}

static void
openbus_host_set_irq(int state)
{
	podules_set_openbus_irq(state);
}

static void
openbus_host_set_fiq(int state)
{
	podules_set_openbus_fiq(state);
}

static const openbus_host_ops openbus_ops = {
	.read32 = openbus_host_read32,
	.write32 = openbus_host_write32,
	.set_irq = openbus_host_set_irq,
	.set_fiq = openbus_host_set_fiq,
};


int drawscre = 0;

unsigned long idle_ticks = 0;
int quited = 0;

static FILE *arclog; /* Log file handle */

static int cycles;

static DebugBreakpointInfo debugger_breakpoints[DEBUGGER_MAX_BREAKPOINTS];
static uint32_t debugger_breakpoint_count = 0;

static DebugWatchpointInfo debugger_watchpoints[DEBUGGER_MAX_WATCHPOINTS];
static uint32_t debugger_watchpoint_count = 0;

static int debugger_pause_requested = 0;
static DebugPauseReason debugger_pending_reason = DebugPauseReason_None;

/* The temporary breakpoint behind step-over, step-out and run-to.
   Deliberately not an entry in the breakpoint table: planting one there would
   mean either clobbering a breakpoint the user had already set at that address
   or refusing to step, and both are worse than keeping one address to one
   side. Cleared by any resume or step, so it can never fire long after the
   operation that armed it. */
static int debugger_temp_bp_active = 0;
static uint32_t debugger_temp_bp_address = 0;

static int debugger_paused = 0;
static DebugPauseReason debugger_pause_reason = DebugPauseReason_None;
static uint32_t debugger_halt_pc = 0;
static uint32_t debugger_halt_opcode = 0;
static uint32_t debugger_last_pc = 0;
static uint32_t debugger_last_opcode = 0;
static uint32_t debugger_hit_address = 0;
static uint32_t debugger_hit_value = 0;
static uint8_t debugger_hit_size = 0;
static uint8_t debugger_hit_is_write = 0;
static uint32_t debugger_step_remaining = 0;
static int debugger_step_active = 0;

/* Debug trace ring (single-writer: only touched on the emulator thread, both
   when pushing events during execution and when draining during command
   processing, so no locking is required). */
#define DEBUGGER_TRACE_RING_SIZE 4096	/* must be a power of two */
#define DEBUGGER_TRACE_RING_MASK (DEBUGGER_TRACE_RING_SIZE - 1)
static DebugTraceEvent debugger_trace_ring[DEBUGGER_TRACE_RING_SIZE];
static uint32_t debugger_trace_head = 0;	/**< next write index */
static uint32_t debugger_trace_tail = 0;	/**< next read index */
static uint32_t debugger_trace_dropped = 0;	/**< events lost to overflow */
static uint32_t debugger_trace_seq = 0;		/**< monotonic event counter */
static DebugTraceConfig debugger_trace_config;	/**< zero-initialised: all off */
int debugger_swi_trace_active = 0;		/**< fast gate read from opSWI() */

/* Fast gate read once per instruction by the interpreter and the recompiler's
   dispatch, so an idle debugger costs a predictable load rather than a call.
   Cached because the answer only changes when the debugger's state does; every
   mutator ends with debugger_refresh_hook_active(). */
int debugger_hook_active = 0;

static void debugger_refresh_hook_active(void);

static void debugger_trace_push(uint32_t type, uint32_t pc, uint32_t opcode,
	uint32_t arg0, uint32_t arg1, uint32_t arg2);

#ifdef _DEBUG
/**
 * UNIMPLEMENTEDFL
 *
 * Used to report sections of code that have not been implemented yet.
 * Do not use this function directly. Use the macro UNIMPLEMENTED() instead.
 *
 * @param file    File function is called from
 * @param line    Line function is called from
 * @param section Section code is missing from eg. "IOMD register" or
 *                "HostFS filecore message"
 * @param format  Section specific information
 * @param ...     Section specific information variable arguments
 */
void UNIMPLEMENTEDFL(const char *file, unsigned line, const char *section,
                     const char *format, ...)
{
	char buffer[1024];
	va_list arg_list;

	assert(file);
	assert(section);
	assert(format);

	va_start(arg_list, format);
	vsprintf(buffer, format, arg_list);
	va_end(arg_list);

	rpclog("UNIMPLEMENTED: %s: %s(%u): %s\n",
	       section, file, line, buffer);

	fprintf(stderr,
	        "UNIMPLEMENTED: %s: %s(%u): %s\n",
	        section, file, line, buffer);
}
#endif /* _DEBUG */

static int
debugger_breakpoint_index(uint32_t address)
{
	for (uint32_t i = 0; i < debugger_breakpoint_count; i++) {
		if (debugger_breakpoints[i].address == address) {
			return (int) i;
		}
	}
	return -1;
}

static int
debugger_watchpoint_index(uint32_t address, uint32_t size, int on_read, int on_write)
{
	for (uint32_t i = 0; i < debugger_watchpoint_count; i++) {
		const DebugWatchpointInfo *wp = &debugger_watchpoints[i];
		if (wp->address == address &&
		    wp->size == size &&
		    wp->on_read == (uint8_t) (on_read != 0) &&
		    wp->on_write == (uint8_t) (on_write != 0)) {
			return (int) i;
		}
	}
	return -1;
}

static int
debugger_watchpoint_matches(const DebugWatchpointInfo *wp, uint32_t address,
	uint32_t size, int is_write)
{
	if (wp->size == 0) {
		return 0;
	}
	if (is_write && !wp->on_write) {
		return 0;
	}
	if (!is_write && !wp->on_read) {
		return 0;
	}
	const uint64_t start = address;
	const uint64_t end = start + (uint64_t) size - 1ull;
	const uint64_t wp_start = wp->address;
	const uint64_t wp_end = wp_start + (uint64_t) wp->size - 1ull;
	return !(end < wp_start || wp_end < start);
}

static void
debugger_reset_hit_info(void)
{
	debugger_hit_address = 0;
	debugger_hit_value = 0;
	debugger_hit_size = 0;
	debugger_hit_is_write = 0;
}

static void
debugger_enter_pause(DebugPauseReason reason, uint32_t pc, uint32_t opcode)
{
	debugger_paused = 1;
	debugger_pause_reason = reason;
	debugger_halt_pc = pc;
	debugger_halt_opcode = opcode;
	debugger_pause_requested = 0;
	debugger_pending_reason = DebugPauseReason_None;
	debugger_step_remaining = 0;
	debugger_step_active = 0;
	debugger_refresh_hook_active();
}

void
debugger_get_status(DebuggerStatus *status)
{
	if (status == NULL) {
		return;
	}
	memset(status, 0, sizeof(*status));
	status->paused = debugger_paused;
	status->pause_requested = debugger_pause_requested;
	status->reason = debugger_pause_reason;
	status->halt_pc = debugger_halt_pc;
	status->halt_opcode = debugger_halt_opcode;
	status->last_pc = debugger_last_pc;
	status->last_opcode = debugger_last_opcode;
	status->hit_address = debugger_hit_address;
	status->hit_value = debugger_hit_value;
	status->hit_size = debugger_hit_size;
	status->hit_is_write = debugger_hit_is_write;
	status->step_active = debugger_step_active ? 1 : 0;
	status->breakpoint_count = debugger_breakpoint_count;
	for (uint32_t i = 0; i < debugger_breakpoint_count; i++) {
		status->breakpoints[i] = debugger_breakpoints[i];
	}
	status->watchpoint_count = debugger_watchpoint_count;
	for (uint32_t i = 0; i < debugger_watchpoint_count; i++) {
		status->watchpoints[i] = debugger_watchpoints[i];
	}
}

int
debugger_is_paused(void)
{
	return debugger_paused;
}

/**
 * Work out whether anything currently wants to see each instruction.
 *
 * Not called from the execution path - debugger_hook_active caches the answer.
 */
static int
debugger_compute_hook_active(void)
{
	if (debugger_paused) {
		return 1;
	}
	if (debugger_pause_requested) {
		return 1;
	}
	if (debugger_step_remaining > 0) {
		return 1;
	}
	if (debugger_temp_bp_active) {
		return 1;
	}
	if (debugger_breakpoint_count > 0) {
		return 1;
	}
	if (debugger_watchpoint_count > 0) {
		return 1;
	}
	/* Halting traps need the hooked path so the core stops once a trap has
	   set debugger_paused from inside exception()/opSWI(). */
	if (debugger_trace_config.trap_undefined ||
	    debugger_trace_config.trap_prefetch_abort ||
	    debugger_trace_config.trap_data_abort ||
	    debugger_trace_config.swi_trace_halt) {
		return 1;
	}
	return 0;
}

/**
 * Recompute the fast gate. Must be called after anything that could change the
 * answer - every debugger state change funnels through here.
 */
static void
debugger_refresh_hook_active(void)
{
	debugger_hook_active = debugger_compute_hook_active();
}

int
debugger_requires_instruction_hook(void)
{
	return debugger_hook_active;
}

void
debugger_request_pause(DebugPauseReason reason)
{
	if (debugger_paused) {
		debugger_pause_reason = reason;
		return;
	}
	debugger_pause_requested = 1;
	debugger_pending_reason = reason;
	debugger_refresh_hook_active();
}

void
debugger_resume(void)
{
	debugger_paused = 0;
	debugger_pause_requested = 0;
	debugger_pending_reason = DebugPauseReason_None;
	debugger_pause_reason = DebugPauseReason_None;
	debugger_step_remaining = 0;
	debugger_step_active = 0;
	/* An outstanding step-over target is abandoned here rather than left to
	   fire at some unrelated moment later on. */
	debugger_temp_bp_active = 0;
	debugger_reset_hit_info();
	debugger_refresh_hook_active();
}

void
debugger_single_step(uint32_t instruction_count)
{
	if (instruction_count == 0) {
		return;
	}
	debugger_paused = 0;
	debugger_pause_requested = 0;
	debugger_pending_reason = DebugPauseReason_None;
	debugger_pause_reason = DebugPauseReason_None;
	debugger_step_remaining = instruction_count;
	debugger_step_active = 1;
	debugger_temp_bp_active = 0;
	debugger_reset_hit_info();
	debugger_refresh_hook_active();
}

/* ---- call stack ---------------------------------------------------------- */

/**
 * Walk the call stack.
 *
 * APCS keeps a frame pointer in R11 pointing at a four-word record: the saved
 * PC at [fp], the return address at [fp,#-4], the caller's stack pointer at
 * [fp,#-8] and the caller's frame pointer at [fp,#-12]. Frame 0 is taken from
 * the live registers rather than from memory, since there is no record for the
 * function currently executing.
 *
 * Addresses are masked with arm.r15_mask, which is what makes this work on the
 * 26-bit cores: there R15 and the saved link register carry PSR bits in the
 * top and bottom of the word, and an unmasked value is not an address at all.
 *
 * Plenty of RISC OS is hand-written assembler that keeps no frame pointer, so
 * a chain that runs into such a function cannot be followed. Every link is
 * therefore checked - alignment, readability, and that frames move up the
 * stack rather than down or in circles - and a walk that gives up says so
 * through `truncated`. Reporting a short stack honestly is worth much more
 * than reporting a long invented one.
 *
 * @param out       Frames, innermost first
 * @param max       Capacity of `out`
 * @param truncated Set non-zero if the walk gave up rather than reaching the
 *                  end of the chain; may be NULL
 * @return Number of frames written
 */
uint32_t
debugger_backtrace(DebugFrame *out, uint32_t max, int *truncated)
{
	const uint32_t mask = arm.r15_mask;
	uint32_t count;
	uint32_t fp;

	if (truncated != NULL) {
		*truncated = 0;
	}
	if (out == NULL || max == 0) {
		return 0;
	}
	if (max > DEBUGGER_MAX_FRAMES) {
		max = DEBUGGER_MAX_FRAMES;
	}

	out[0].pc = PC;
	out[0].lr = arm.reg[14] & mask;
	out[0].sp = arm.reg[13];
	out[0].fp = arm.reg[11];
	count = 1;

	fp = arm.reg[11];

	while (count < max) {
		uint32_t saved_lr, saved_sp, saved_fp;

		if (fp == 0) {
			break;		/* clean end of the chain */
		}

		if ((fp & 3) != 0) {
			if (truncated != NULL) {
				*truncated = 1;
			}
			break;
		}

		if (!mem_debug_read(fp - 4, 4, &saved_lr) ||
		    !mem_debug_read(fp - 8, 4, &saved_sp) ||
		    !mem_debug_read(fp - 12, 4, &saved_fp)) {
			if (truncated != NULL) {
				*truncated = 1;
			}
			break;
		}

		saved_lr &= mask;
		if (saved_lr == 0) {
			break;		/* clean end of the chain */
		}

		out[count].pc = saved_lr;
		out[count].lr = 0;	/* only frame 0 has a live link register */
		out[count].sp = saved_sp;
		out[count].fp = saved_fp;
		count++;

		/* The stack grows down, so each caller's frame must sit above
		   the one it called. Anything else is a corrupt or absent chain
		   being followed into nonsense - and would loop forever. */
		if (saved_fp != 0 && saved_fp <= fp) {
			if (truncated != NULL) {
				*truncated = 1;
			}
			break;
		}

		fp = saved_fp;
	}

	return count;
}

/* ---- stepping over, out and to ------------------------------------------- */

/**
 * Arm the temporary breakpoint and let the machine run to it.
 */
static void
debugger_run_to_temp(uint32_t address)
{
	debugger_resume();		/* clears any previous temporary target */
	debugger_temp_bp_active = 1;
	debugger_temp_bp_address = address;
	debugger_refresh_hook_active();
}

/**
 * Step one instruction, but run subroutine calls to completion.
 *
 * Anything that is not a call is an ordinary single step. A call gets a
 * temporary breakpoint at the following instruction instead, so the whole
 * subroutine runs at full speed and execution comes back where the person
 * stepping expects it.
 *
 * Falls back to a plain step whenever the instruction cannot be read or is not
 * a recognised call. Stepping into is always safe; running away is not, so an
 * uncertain case must degrade towards the step.
 *
 * @return Non-zero if the machine was started
 */
int
debugger_step_over(void)
{
	ArmInsnInfo info;
	uint32_t pc = PC;
	uint32_t opcode;

	if (!debugger_paused) {
		return 0;
	}

	if (!mem_debug_read(pc, 4, &opcode) ||
	    !arm_decode(opcode, pc, &info) || !info.is_call) {
		debugger_single_step(1);
		return 1;
	}

	debugger_run_to_temp(pc + 4);
	return 1;
}

/**
 * Run until the current function returns.
 *
 * The return address comes from the frame chain where there is one, since R14
 * belongs to the current function and may long since have been saved and
 * reused. Where there is no frame, R14 is the only candidate left and is
 * better than refusing.
 *
 * @return Non-zero if the machine was started, zero if no return address could
 *         be found
 */
int
debugger_step_out(void)
{
	DebugFrame frames[2];
	uint32_t count;
	uint32_t target;

	if (!debugger_paused) {
		return 0;
	}

	count = debugger_backtrace(frames, 2, NULL);
	if (count >= 2 && frames[1].pc != 0) {
		target = frames[1].pc;
	} else {
		target = arm.reg[14] & arm.r15_mask;
	}

	/* Nowhere to go, or nowhere new: stepping out of the outermost frame is
	   not a thing, and a target of here would stop immediately. */
	if (target == 0 || target == PC) {
		return 0;
	}

	debugger_run_to_temp(target);
	return 1;
}

/**
 * Run until a given address is reached.
 *
 * @return Non-zero if the machine was started
 */
int
debugger_run_to(uint32_t address)
{
	if (!debugger_paused) {
		return 0;
	}
	debugger_run_to_temp(address);
	return 1;
}

void
debugger_clear_breakpoints(void)
{
	debugger_breakpoint_count = 0;
	debugger_refresh_hook_active();
}

int
debugger_add_breakpoint(uint32_t address)
{
	return debugger_add_breakpoint_ex(address, NULL, 0, 0);
}

/**
 * Add a breakpoint, or replace the settings of one already at this address.
 *
 * Replacing rather than refusing is deliberate: setting a breakpoint again with
 * a different condition is how a condition gets corrected, and having to remove
 * it first would be a papercut with no upside. The hit count is reset with it,
 * since the counts belong to the old condition.
 *
 * @param address      Address to halt at
 * @param condition    Expression that must be true to halt, or NULL for none
 * @param ignore_count Matches to skip before halting
 * @param one_shot     Remove the breakpoint once it halts
 * @return Non-zero on success, zero if the table is full
 */
int
debugger_add_breakpoint_ex(uint32_t address, const char *condition,
	uint32_t ignore_count, int one_shot)
{
	int index = debugger_breakpoint_index(address);
	DebugBreakpointInfo *bp;

	if (index < 0) {
		if (debugger_breakpoint_count >= DEBUGGER_MAX_BREAKPOINTS) {
			return 0;
		}
		index = (int) debugger_breakpoint_count++;
	}

	bp = &debugger_breakpoints[index];
	memset(bp, 0, sizeof(*bp));
	bp->address = address;
	bp->enabled = 1;
	bp->one_shot = (uint8_t) (one_shot != 0);
	bp->ignore_count = ignore_count;

	if (condition != NULL && condition[0] != '\0') {
		strncpy(bp->condition, condition, sizeof(bp->condition) - 1);
		bp->condition[sizeof(bp->condition) - 1] = '\0';
		bp->has_condition = 1;
	}

	debugger_refresh_hook_active();
	return 1;
}

int
debugger_remove_breakpoint(uint32_t address)
{
	int index = debugger_breakpoint_index(address);
	if (index < 0) {
		return 0;
	}
	if ((uint32_t) index < debugger_breakpoint_count - 1) {
		memmove(&debugger_breakpoints[index],
		        &debugger_breakpoints[index + 1],
		        (debugger_breakpoint_count - (uint32_t) index - 1) * sizeof(DebugBreakpointInfo));
	}
	debugger_breakpoint_count--;
	debugger_refresh_hook_active();
	return 1;
}

int
debugger_has_breakpoint(uint32_t address)
{
	return debugger_breakpoint_index(address) >= 0;
}

/**
 * Arm or disarm a breakpoint without forgetting it.
 *
 * @return Non-zero if a breakpoint at this address was found
 */
int
debugger_set_breakpoint_enabled(uint32_t address, int enabled)
{
	int index = debugger_breakpoint_index(address);

	if (index < 0) {
		return 0;
	}
	debugger_breakpoints[index].enabled = (uint8_t) (enabled != 0);
	debugger_refresh_hook_active();
	return 1;
}

/**
 * Look up a breakpoint's settings.
 *
 * @return The breakpoint, or NULL if there is none at this address. Only valid
 *         until the breakpoint table is next modified.
 */
const DebugBreakpointInfo *
debugger_get_breakpoint(uint32_t address)
{
	int index = debugger_breakpoint_index(address);

	return index < 0 ? NULL : &debugger_breakpoints[index];
}

void
debugger_clear_watchpoints(void)
{
	debugger_watchpoint_count = 0;
	debugger_refresh_hook_active();
}

int
debugger_add_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write, int log_only)
{
	if (size == 0) {
		return 0;
	}
	int index = debugger_watchpoint_index(address, size, on_read, on_write);
	if (index >= 0) {
		/* Already present; allow the log_only flag to be updated in place. */
		debugger_watchpoints[index].log_only = (uint8_t) (log_only != 0);
		return 1;
	}
	if (debugger_watchpoint_count >= DEBUGGER_MAX_WATCHPOINTS) {
		return 0;
	}
	DebugWatchpointInfo *wp = &debugger_watchpoints[debugger_watchpoint_count++];
	wp->address = address;
	wp->size = size;
	wp->on_read = (uint8_t) (on_read != 0);
	wp->on_write = (uint8_t) (on_write != 0);
	wp->log_only = (uint8_t) (log_only != 0);
	wp->reserved1 = 0;
	debugger_refresh_hook_active();
	return 1;
}

int
debugger_remove_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write)
{
	int index = debugger_watchpoint_index(address, size, on_read, on_write);
	if (index < 0) {
		return 0;
	}
	if ((uint32_t) index < debugger_watchpoint_count - 1) {
		memmove(&debugger_watchpoints[index],
		        &debugger_watchpoints[index + 1],
		        (debugger_watchpoint_count - (uint32_t) index - 1) * sizeof(DebugWatchpointInfo));
	}
	debugger_watchpoint_count--;
	debugger_refresh_hook_active();
	return 1;
}

/* ---- breakpoint conditions ---------------------------------------------- */

/* Expressions read the machine through these, never directly, so that a
   condition cannot reach anything with a side effect. Memory in particular
   goes via mem_debug_read(), which will not fault, will not touch I/O and will
   not trip a watchpoint - evaluating a condition has to leave the program
   being debugged exactly as it found it. */

static uint32_t
debugger_expr_read_reg(int reg, void *ctx)
{
	NOT_USED(ctx);
	return arm.reg[reg & 15];
}

static uint32_t
debugger_expr_read_cpsr(void *ctx)
{
	NOT_USED(ctx);
	return arm.reg[cpsr];
}

static int
debugger_expr_read_mem(uint32_t address, uint32_t size, uint32_t *out, void *ctx)
{
	NOT_USED(ctx);
	return mem_debug_read(address, size, out);
}

static const DebugExprEnv debugger_expr_env = {
	debugger_expr_read_reg,
	debugger_expr_read_cpsr,
	debugger_expr_read_mem,
	NULL
};

/**
 * Decide whether a breakpoint that has been reached should actually halt.
 *
 * Order matters. A disabled breakpoint is not reached at all, so it does not
 * count. The condition is tested before the ignore count, so "the twentieth
 * time R0 is zero" means what it says rather than "the twentieth time round,
 * if R0 happens to be zero then".
 *
 * @return Non-zero if the machine should stop here
 */
static int
debugger_breakpoint_should_halt(DebugBreakpointInfo *bp)
{
	if (!bp->enabled) {
		return 0;
	}

	bp->hit_count++;

	if (bp->has_condition) {
		uint32_t value = 0;

		if (!debugexpr_eval(bp->condition, &debugger_expr_env, &value, NULL)) {
			/* Almost always a dereference of an address that is not
			   mapped right now. Carrying on is the least surprising
			   thing to do, but it is counted so that a breakpoint
			   which never fires can be explained. */
			bp->eval_errors++;
			return 0;
		}
		if (value == 0) {
			return 0;
		}
	}

	if (bp->ignore_count > 0) {
		bp->ignore_count--;
		return 0;
	}

	return 1;
}

int
debugger_instruction_hook(uint32_t pc, uint32_t opcode)
{
	debugger_last_pc = pc;
	debugger_last_opcode = opcode;

	if (debugger_paused) {
		return 1;
	}

	debugger_step_active = (debugger_step_remaining > 0) ? 1 : 0;

	/* The step-over/step-out/run-to target. Checked before the breakpoint
	   table and consumed on arrival, so it cannot fire twice. */
	if (debugger_temp_bp_active && pc == debugger_temp_bp_address) {
		debugger_temp_bp_active = 0;
		debugger_pause_requested = 1;
		debugger_pending_reason = DebugPauseReason_Step;
	}

	if (debugger_breakpoint_count > 0) {
		int index = debugger_breakpoint_index(pc);

		if (index >= 0 &&
		    debugger_breakpoint_should_halt(&debugger_breakpoints[index])) {
			/* One-shot breakpoints are the machinery behind step-over,
			   step-out and run-to, and must not outlive their halt. */
			if (debugger_breakpoints[index].one_shot) {
				debugger_remove_breakpoint(pc);
			}
			debugger_pause_requested = 1;
			debugger_pending_reason = DebugPauseReason_Breakpoint;
		}
	}

	if (debugger_pause_requested) {
		DebugPauseReason reason = debugger_pending_reason;
		if (reason == DebugPauseReason_None) {
			reason = DebugPauseReason_User;
		}
		debugger_enter_pause(reason, pc, opcode);
		return 1;
	}

	return 0;
}

void
debugger_memory_access(uint32_t address, uint32_t size, int is_write, uint32_t value)
{
	if (debugger_watchpoint_count == 0) {
		return;
	}
	if (debugger_pause_requested && debugger_pending_reason == DebugPauseReason_Watchpoint) {
		return;
	}
	if (size == 0) {
		size = 1;
	}
	for (uint32_t i = 0; i < debugger_watchpoint_count; i++) {
		const DebugWatchpointInfo *wp = &debugger_watchpoints[i];
		if (debugger_watchpoint_matches(wp, address, size, is_write)) {
			if (wp->log_only) {
				/* Record the access and keep running. */
				debugger_trace_push(TraceEvent_Watchpoint, arm.reg[15], 0,
				    address, value, (size << 1) | (is_write ? 1u : 0u));
				continue;
			}
			debugger_hit_address = address;
			debugger_hit_value = value;
			debugger_hit_size = (uint8_t) ((size > 255u) ? 255u : size);
			debugger_hit_is_write = is_write ? 1 : 0;
			debugger_pause_requested = 1;
			debugger_pending_reason = DebugPauseReason_Watchpoint;
			debugger_step_remaining = 0;
			debugger_step_active = 0;
			return;
		}
	}
}

void
debugger_after_instruction(uint32_t pc, uint32_t opcode)
{
	NOT_USED(pc);
	NOT_USED(opcode);

	if (debugger_step_remaining > 0) {
		debugger_step_remaining--;
		if (debugger_step_remaining == 0) {
			debugger_pause_requested = 1;
			debugger_pending_reason = DebugPauseReason_Step;
		}
	}
	debugger_step_active = (debugger_step_remaining > 0) ? 1 : 0;
	debugger_refresh_hook_active();
}

/**
 * Push one event into the debug trace ring. Called only on the emulator thread.
 * On overflow the oldest unread event is discarded and a drop is recorded.
 */
static void
debugger_trace_push(uint32_t type, uint32_t pc, uint32_t opcode,
	uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
	uint32_t next = (debugger_trace_head + 1) & DEBUGGER_TRACE_RING_MASK;
	DebugTraceEvent *ev;

	if (next == debugger_trace_tail) {
		/* Ring full: drop the oldest entry to make room. */
		debugger_trace_tail = (debugger_trace_tail + 1) & DEBUGGER_TRACE_RING_MASK;
		debugger_trace_dropped++;
	}

	ev = &debugger_trace_ring[debugger_trace_head];
	ev->seq = ++debugger_trace_seq;
	ev->type = type;
	ev->pc = pc;
	ev->opcode = opcode;
	ev->arg0 = arg0;
	ev->arg1 = arg1;
	ev->arg2 = arg2;

	debugger_trace_head = next;
}

void
debugger_set_trace_config(const DebugTraceConfig *cfg)
{
	if (cfg == NULL) {
		return;
	}
	debugger_trace_config = *cfg;
	debugger_swi_trace_active = (cfg->swi_trace_enabled || cfg->swi_trace_halt) ? 1 : 0;
	debugger_refresh_hook_active();
}

void
debugger_get_trace_config(DebugTraceConfig *cfg)
{
	if (cfg == NULL) {
		return;
	}
	*cfg = debugger_trace_config;
}

uint32_t
debugger_trace_pending(void)
{
	return (debugger_trace_head - debugger_trace_tail) & DEBUGGER_TRACE_RING_MASK;
}

/**
 * Copy up to max pending trace events out of the ring (oldest first) and
 * report-and-clear the dropped-event counter. Returns the number copied.
 */
uint32_t
debugger_drain_trace_events(DebugTraceEvent *out, uint32_t max, uint32_t *dropped)
{
	uint32_t count = 0;

	if (dropped != NULL) {
		*dropped = debugger_trace_dropped;
	}
	debugger_trace_dropped = 0;

	if (out == NULL) {
		return 0;
	}

	while (count < max && debugger_trace_tail != debugger_trace_head) {
		out[count++] = debugger_trace_ring[debugger_trace_tail];
		debugger_trace_tail = (debugger_trace_tail + 1) & DEBUGGER_TRACE_RING_MASK;
	}

	return count;
}

/**
 * Called from the top of exception() in both the interpreter and dynarec, before
 * any CPU state is changed. Classifies the exception, optionally logs it, and
 * optionally requests a halt at the exception vector handler.
 *
 * @param mmode   Target processor mode (unused; kept for call-site clarity)
 * @param address Vector value passed to exception() (identifies the exception)
 * @param pc      Current value of R15
 */
void
debugger_exception_hook(uint32_t mmode, uint32_t address, uint32_t pc)
{
	uint32_t kind;
	int trap;

	NOT_USED(mmode);

	switch (address) {
	case 0x08: kind = TraceException_Undefined;     trap = debugger_trace_config.trap_undefined;       break;
	case 0x10: kind = TraceException_PrefetchAbort;  trap = debugger_trace_config.trap_prefetch_abort;   break;
	case 0x14: kind = TraceException_DataAbort;      trap = debugger_trace_config.trap_data_abort;       break;
	default:   return; /* SWI (0x0c), IRQ, FIQ: not exception traps */
	}

	/* Always record when trapping so the faulting PC is visible even if
	   exception logging is otherwise off. */
	if (debugger_trace_config.log_exceptions || trap) {
		debugger_trace_push(TraceEvent_Exception, pc, 0, kind, pc, 0);
	}

	/* Deferred halt: let exception() finish setting up the vector, then the
	   core stops at the handler's first instruction via the instruction hook. */
	if (trap && !debugger_paused && !debugger_pause_requested) {
		debugger_pause_requested = 1;
		debugger_pending_reason = DebugPauseReason_Exception;
		debugger_refresh_hook_active();
	}
}

/**
 * Called from opSWI() once the SWI number is known, gated by
 * debugger_swi_trace_active. Logs the SWI and optionally requests a halt.
 *
 * @return 1 if the SWI handling should stop (halt requested), 0 otherwise
 */
int
debugger_swi_hook(uint32_t swinum, uint32_t opcode)
{
	if (swinum < debugger_trace_config.swi_filter_min ||
	    swinum > debugger_trace_config.swi_filter_max) {
		return 0;
	}

	if (debugger_trace_config.swi_trace_enabled) {
		debugger_trace_push(TraceEvent_Swi, arm.reg[15], opcode,
		    swinum, arm.reg[0], arm.reg[cpsr]);
	}

	/* Deferred halt: let opSWI() run the SWI (raising the supervisor
	   exception), then stop at the SWI handler's first instruction. */
	if (debugger_trace_config.swi_trace_halt && !debugger_paused &&
	    !debugger_pause_requested) {
		debugger_pause_requested = 1;
		debugger_pending_reason = DebugPauseReason_Swi;
		debugger_refresh_hook_active();
	}

	return 0;
}

/**
 * Write a message to the RPCEmu log file rpclog.txt
 *
 * @param format printf style format of message
 * @param ...    format specific arguments
 */
void
rpclog(const char *format, ...)
{
	va_list arg_list;

	assert(format);

	if (arclog == NULL) {
		arclog = fopen(rpcemu_get_log_path(), "wt");
		if (arclog == NULL) {
			return;
		}
	}

	va_start(arg_list, format);
	vfprintf(arclog, format, arg_list);
	va_end(arg_list);

	fflush(arclog);
}

void
rpclog_close(void)
{
	if (arclog != NULL) {
		fclose(arclog);
		arclog = NULL;
	}
}

/**
 * Reinitialise all emulated subsystems based on current configuration. This
 * is equivalent to resetting the emulated hardware.
 *
 * Called from the host GUI when the user has changed configuration, or when
 * the user picks 'Reset' from the menu.
 */
void
resetrpc(void)
{
	rpclog("RPCEmu: Machine reset\n");

        mem_reset(config.mem_size, config.vram_size);
        cp15_reset(machine.cpu_model);
	arm_reset(machine.cpu_model);
	resetfpa();
        keyboard_reset();
	iomd_reset(machine.iomd_type);

        reseti2c(machine.i2c_devices);
        resetide();
        superio_reset(machine.super_type);
	i8042_reset();
	cmos_reset();
        podules_reset();
        /* The second processor slot resets with the machine (nRESET). */
        openbus_reset();
        /* Slot order. The support card goes back in slot 0, where it has always
           been: only PHCIDriver needed USB in slot 0, and OHCIDriver finds the
           controller by asking the HAL rather than by looking at a slot. */
        podulerom_reset(); // must be called after podules_reset()
        usbcard_reset();
        /* The graphics card takes the next slot after the extension-ROM card,
           and must be registered here rather than at start-up: podules_reset()
           clears every slot, so a card registered before this point would be
           overwritten by whatever claimed a slot afterwards. */
        gfxcard_init();

	/* The guest module's pollword address goes with the machine that
	   registered it. Without this the host keeps writing "the clipboard
	   changed" into whatever is at that address in the new machine, until a
	   module sets up again. */
	clipboard_reset();
        hostfs_reset();
        hostcmd_reset();
        debugcmd_reset();


#ifdef RPCEMU_NETWORKING
	network_reset();

	if (config.network_type != NetworkType_Off) {
		network_init();
	}
#endif

	/* Install plugin-ABI podules into any remaining free slots, after the
	   legacy extension-ROM and network podules have claimed theirs. */
	podules_init_headers();

	cycles = 0;

	peripheral_config_apply();

	rpclog("RPCEmu: Machine reset complete\n");
}

/**
 * Log additional information about the build and environment.
 */
void
rpcemu_log_information(void)
{
	char cwd[1024];
	time_t now;
	char buffer[22];
	struct tm* tm_info;

	/* Time and date of this run */
	time(&now);
	tm_info = localtime(&now);
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
	rpclog("localtime: %s\n", buffer);
	tm_info = gmtime(&now);
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
	rpclog("   gmtime: %s\n", buffer);

	/* Log version and build type */
	rpclog("RPCEmu " VERSION " [");
	if (arm_is_dynarec()) {
		rpclog("DYNAREC");
	} else {
		rpclog("INTERPRETER");
	}

#if defined(_DEBUG)
	rpclog(" DEBUG");
#else
	rpclog(" NO_DEBUG");
#endif
	rpclog("]\n");

	/* Log 32 or 64-bit */
	rpclog("Build: %lu-bit binary\n", (unsigned long) sizeof(void *) * 8);

	/* Log Compiler */
	/* Clang must be tested before GCC because Clang also defines __GNUC__ */
#if defined __clang__ && defined __VERSION__
	rpclog("Compiler: Clang version " __VERSION__ "\n");
#elif defined __GNUC__ && defined __VERSION__
	rpclog("Compiler: GCC version " __VERSION__ "\n");
#endif
	/* Log details of Operating System */
	rpcemu_log_os();

	/* Log details of Platform (qt) */
	rpcemu_log_platform();

	/* Log working directory */
	if (getcwd(cwd, sizeof(cwd)) != NULL) {
		rpclog("Working Directory: %s\n", cwd);
	}
}

/**
 * Start enough of the emulator system to allow
 * the GUI to initialise (e.g. load the config to init
 * the configure window)
 *
 * Called from each platform's code on program startup.
 */
void
rpcemu_prestart(void)
{
	/* On startup log additional information about the build and environment */
	rpcemu_log_information();

	config_load(&config);
}

/**
 * Set the initial state of all emulated subsystems. Load disc images, CMOS
 * and configuration.
 *
 * Called from each platform's code on program startup.
 *
 * @return Always 0
 */
void
rpcemu_start(void)
{
#ifdef _WIN32
	/* Initialise Winsock before anything opens a socket (hostcmd/debugcmd
	   control sockets, SLiRP NAT, the TCP modem, the broadcast relay). */
	{
		WSADATA wsadata;
		int err = WSAStartup(MAKEWORD(2, 2), &wsadata);

		if (err != 0) {
			fatal("WSAStartup failed: %d", err);
		}
	}

	/* Raise the system timer resolution to 1ms. Windows' default granularity is
	   ~15.6ms, which makes the Sleep(1) in rpcemu_idle() (used by "Reduce CPU
	   Usage" when RISC OS calls Portable_Idle) sleep for up to ~15ms - long
	   enough that mouse and screen updates only get serviced every ~15ms, so the
	   pointer becomes very laggy (issue #26). Paired with timeEndPeriod() in
	   endrpcemu(). */
	timeBeginPeriod(1);
#endif

	hostfs_init();
	hostcmd_init();
	debugcmd_init();
	parallel_bus_init();
	serial_bus_init();
	printer_init();
	mem_init();
	cp15_init();
	arm_init();
	loadroms();
        cmos_init();
        fdc_init();
        adf_init();
        hfe_init();
        mfm_init();
        fdc_image_load("boot.adf", 0);
        fdc_image_load("notboot.adf", 1);
        initvideo();

        sound_init();

        initcodeblocks();
        iso_init();
        if (config.cdromtype == 2) /* ISO */
                iso_open(config.isoname);
        usbcard_init();
        initpodulerom();
        podule_build_list();

	/*
	 * The second processor bus is started ONCE, here, and not in resetrpc().
	 *
	 * A card is physically still in its slot after a machine reset, so a reset
	 * must reset the card and not remove it - which is what openbus_reset() in
	 * resetrpc() does. Starting the bus there instead wiped the slot on every
	 * reset, and would have quietly unfitted any card the moment the guest
	 * rebooted.
	 *
	 * No card is fitted unless one is asked for: every machine that shipped had
	 * an empty second slot unless somebody bought a PC card.
	 */
	openbus_init(&openbus_ops);
	if (openbus_stub_requested()) {
		if (openbus_stub_fit() == 0) {
			rpclog("OPEN Bus: fitted '%s' to the second processor slot\n",
			       openbus_name());
		} else {
			rpclog("OPEN Bus: could not fit the test card\n");
		}
	}

	/* Other components are initialised in the same way as the hardware
	   being reset */
	resetrpc();
}


/**
 * Execute a chunk of ARM instructions. This is the main entry point for the
 * emulation of the virtual hardware.
 *
 * Called repeatedly from within each platform's main loop.
 */
void
execrpcemu(void)
{
	cycles += 20000;

	while (cycles > 0) {
		if (debugger_is_paused()) {
			cycles = 0;
			break;
		}

		cycles -= arm_exec();

		if (debugger_is_paused()) {
			cycles = 0;
			break;
		}

		/*
		 * The OPEN Bus second processor's share, charged against the same budget
		 * the ARM is spending. That is not an approximation: a second bus master
		 * drives nWAIT to stall the host ARM while it holds the bus, so time it
		 * uses really is time the ARM does not get. Costs one pointer test when
		 * no card is fitted, which is every machine today. See openbus.h.
		 */
		if (openbus_present()) {
			cycles -= openbus_run(cycles > 0 ? cycles : 0);
		}

		if (kcallback) {
			kcallback--;
			if (kcallback <= 0) {
				kcallback = 0;
				keyboard_callback_rpcemu();
			}
		}
		if (mcallback) {
			mcallback -= 10;
			if (mcallback <= 0) {
				mcallback = 0;
				mouse_ps2_callback();
			}
		}
		if (fdccallback) {
			fdccallback -= 100;
			if (fdccallback <= 0) {
				fdccallback = 0;
				fdc_callback();
			}
		}
		if (idecallback) {
			idecallback -= 10;
			if (idecallback <= 0) {
				idecallback = 0;
				callbackide();
			}
		}
		if (motoron) {
			disc_poll();
		}
	}

	if (drawscre > 0) {
		drawscr();
		/* A frame has gone out, so the card can raise its vsync. Its driver uses
		   this to tell the OS a frame boundary has passed, which is when RISC OS
		   wants pointer and palette changes to take effect. */
		gfxcard_vsync();
		drawscre--;
		if (drawscre > 5) {
			drawscre = 0;
		}
	}

	printer_poll();
	serial_modem_poll();
	serial_host_poll();
	hostcmd_poll();
	debugcmd_poll();
}

/**
 * Attempt to reduce CPU usage by checking for pending interrupts, running
 * any callbacks, and then sleeping for a short period of time.
 *
 * Called when RISC OS calls "Portable_Idle" SWI.
 */
void
rpcemu_idle(void)
{
	/*
	 * Loop while no interrupts are pending and the host has nothing waiting.
	 *
	 * The second test is what keeps the machine responsive with this option on.
	 * Host input, and the flyback interrupt the video thread raises, are queued
	 * for the emulator thread's main loop, and staying in here holds that up:
	 * without this the queue was taken once or twice a second, so a mouse move
	 * could wait most of a second (issue #36).
	 */
	while (!arm.event && !rpcemu_host_commands_pending()) {
		idle_ticks++;
		/* Run down any callback timers */
		if (kcallback) {
			kcallback--;
			if (kcallback <= 0) {
				kcallback = 0;
				keyboard_callback_rpcemu();
			}
		}
		if (mcallback) {
			mcallback -= 10;
			if (mcallback <= 0) {
				mcallback = 0;
				mouse_ps2_callback();
			}
		}
		if (fdccallback) {
			fdccallback -= 100;
			if (fdccallback <= 0) {
				fdccallback = 0;
				fdc_callback();
			}
		}
		if (idecallback) {
			idecallback -= 10;
			if (idecallback <= 0) {
				idecallback = 0;
				callbackide();
			}
		}
		if (motoron) {
			/* Not much point putting a counter here */
			iomd.irqa.status |= IOMD_IRQA_FLOPPY_INDEX;
			updateirqs();
		}
		serial_modem_poll();
		serial_host_poll();
		hostcmd_poll();
		debugcmd_poll();
		/* Sleep if no interrupts pending */
		if (!arm.event) {
#ifdef _WIN32
			Sleep(1);
#else
			struct timespec tm;

			tm.tv_sec = 0;
			tm.tv_nsec = 1000000;
			nanosleep(&tm, NULL);
#endif
		}
		/* Run other periodic actions */
		if (!arm.event) {
			/* Service the host timers first. It is the video timer that
			   asks for a frame, so doing this before the draw below means
			   a frame requested during this pass goes out on this pass;
			   the other way round it always waited for the next one. */
			rpcemu_idle_process_events();

			if (drawscre > 0) {
				drawscr();
				/* Exactly as execrpcemu() does after a frame: the card
				   raises its vsync, which is when RISC OS applies pointer
				   and palette changes. Leaving it out here meant those
				   changes never took effect while the machine was idling,
				   which is precisely when the pointer is being moved
				   about. */
				gfxcard_vsync();
				drawscre--;
				if (drawscre > 5) {
					drawscre = 0;
				}
			}
			printer_poll();
			serial_modem_poll();
			serial_host_poll();
			hostcmd_poll();
			debugcmd_poll();
		}
	}

	/*
	 * Leaving because the host has something queued: give up the rest of the
	 * cycle budget so execrpcemu() returns and the main loop drains it.
	 *
	 * Without this the guest would return from the SWI, find nothing to do, call
	 * Portable_Idle again and come straight back here - where the condition above
	 * now fails, so it would spin without sleeping until the budget ran out. The
	 * budget is only a scheduling quantum; emulated time is taken from the host
	 * clock, so dropping it does not disturb the machine's timekeeping.
	 */
	if (rpcemu_host_commands_pending()) {
		cycles = 0;
	}
}

/**
 * Finalise the subsystems, save floppy disc images, CMOS and configuration.
 *
 * Called from each platform's code on program closing.
 */
void
endrpcemu(void)
{
        hostcmd_close();
        debugcmd_close();
        /* Before anything else: a device passed through from the host is only
           borrowed, and this is what hands it back. */
        usb_ohci_shutdown();
        sound_thread_close();
        closevideo();
        iomd_end();
        fdc_image_save(discname[0], 0);
        fdc_image_save(discname[1], 1);
        free(vram);
        free(ram00);
        free(ram01);
        free(rom);
        savecmos();
	peripheral_config_shutdown();
        config_save(&config);

#ifdef RPCEMU_NETWORKING
	network_reset();
#endif

#ifdef _WIN32
	timeEndPeriod(1); /* release the 1ms timer resolution set in rpcemu_start() */
	WSACleanup();
#endif
}

/**
 * Called whenever the user's chosen model is changed
 *
 * Caches details of the model in the machine struct
 *
 * @param model New model being selected
 */
void
rpcemu_model_changed(Model model)
{
	/* Cache details from the models[] array into the machine struct for speed of lookup */
	machine.model       = model;
	machine.cpu_model   = models[model].cpu_model;
	machine.iomd_type   = models[model].iomd_type;
	machine.super_type  = models[model].super_type;
	machine.i2c_devices = models[model].i2c_devices;
}

/**
 * Load an .adf disc image into the specified drive. Save the previous disc
 * image before loading new.
 *
 * @param drive    RPC Drive number, 0 or 1
 * @param filename Full filepath of new .adf to load
 */
void
rpcemu_floppy_load(int drive, const char *filename)
{
	assert(drive == 0 || drive == 1);
	assert(filename);
	assert(*filename);

	fdc_image_save(discname[drive], drive);

	if (strlen(filename) > sizeof(discname[drive]) - 1) {
		// New disc image path too long
		error("Disc image disc path \'%s\' too long", filename);
	} else {
		strcpy(discname[drive], filename);
		fdc_image_load(discname[drive], drive);
	}
}

/**
 * Eject (unload) the disc from the specified drive
 *
 * @param drive    RPC Drive number, 0 or 1
 */
void
rpcemu_floppy_eject(int drive)
{
	assert(drive == 0 || drive == 1);

	fdc_image_save(discname[drive], drive);
	discname[drive][0] = '\0';
}

/**
 * Find a filename's extension (bit after the .)
 *
 * @param filename string to check
 * @returns pointer to first char in extension, or pointer to
 *          null terminator (empty string) if no extension found
 */
const char *
rpcemu_file_get_extension(const char *filename)
{
	const char *position;

	assert(filename);

	position = strrchr(filename, '.');
	if (position == NULL) {
		/* No extension, return empty string */
		return &filename[strlen(filename)];
	} else {
		/* Found extension */
		return position + 1;
	}
}

/**
 * Test whether the changes in configuration would require an emulated
 * machine reset
 * 
 * Called from GUI thread, is thread safe due to only reading the emulator
 * state
 * 
 * @thread GUI
 * @param new_config New configuration values
 * @param new_model New configuration values
 * @returns Bool of whether emulated machine reset required
 */
int
rpcemu_config_is_reset_required(const Config *new_config, Model new_model)
{
	int needs_reset = 0;
	assert(new_config);

	if(machine.model != new_model) {
		needs_reset = 1;
	}

	if(config.mem_size != new_config->mem_size) {
		needs_reset = 1;
	}

	/* vram size has changed on a machine without fixed vram size */
	if (config.vram_size != new_config->vram_size
	   && (machine.model != Model_A7000 &&
	       machine.model != Model_A7000plus &&
	       machine.model != Model_Phoebe))
	{
		needs_reset = 1;
	}

	if (config.network_type != new_config->network_type) {
		needs_reset = 1;
	}

	// TODO Various network, MAC/IP/bridgename changes will also cause reset

	return needs_reset;
}

/**
 * Apply a new configuration and reset the emulator is required
 * 
 * @thread emulator
 * @param new_config the new configuration
 * @param new_model the new configuration
 */
void
rpcemu_config_apply_new_settings(Config *new_config, Model new_model)
{
	int needs_reset = 0;
	int sound_changed = 0;

	/* Sound state changed? */
	if((config.soundenabled && !new_config->soundenabled)
	   || (new_config->soundenabled && !config.soundenabled))
	{
		sound_changed = 1;
	}

	/* Changed machine we're emulating? */
	if(new_model != machine.model) {
		rpcemu_model_changed(new_model);
		needs_reset = 1;
	}

	/* If an A7000 or an A7000+ it does not have vram */
	if (machine.model == Model_A7000 || machine.model == Model_A7000plus) {
		new_config->vram_size = 0;
	}

	/* If Phoebe, override some settings */
	if (machine.model == Model_Phoebe) {
		new_config->mem_size = 256;
		new_config->vram_size = 4;
	}

	/* Only the Kinetic has the extra on-card SDRAM; every other model is
	   limited to the 256MB the IOMD can address on the motherboard */
	if (machine.model != Model_Kinetic && new_config->mem_size > 256) {
		new_config->mem_size = 256;
	}

	/* 512MB of RAM and more than 2MB of VRAM together overrun the RISC OS memory
	   map, so on a Kinetic the VRAM is fixed at 2MB. That is an OS limit rather
	   than a ROM defect, and it is permanent: the graphics card answers the
	   ceiling it leaves, carrying its own 15MB framestore and not being bound by
	   the map at all (see src/gfxcard.c, docs/gfxcard.md and docs/kinetic.md). */
	if (machine.model == Model_Kinetic) {
		new_config->vram_size = 2;
	}

	if (new_config->mem_size != config.mem_size) {
		needs_reset = 1;
	}

	if (new_config->vram_size != config.vram_size) {
		needs_reset = 1;
	}

	/* Copy new settings over */
	memcpy(&config, new_config, sizeof(Config));

	// Save the settings to the rpc.cfg file
	config_save(&config);

	if(sound_changed) {
		if(config.soundenabled) {
			sound_restart();
		} else {
			sound_pause();
		}
	}

	/* Reset the machine after the config variables have been set to their
	   new values */
	if(needs_reset) {
		resetrpc();
	}
}

/**
 * Add a forwarding rule to the NAT
 *
 * @param type      TCP or UDP
 * @param emu_port  port number on emulated machine
 * @param host_port port number on host machine
 */
void
rpcemu_nat_forward_add(PortForwardRule rule)
{
	int i;

	rpclog("Config: Adding NAT forwarding rule %d %u %u\n", rule.type, rule.emu_port, rule.host_port);

	// Detect duplicate rules
	for (i = 0; i < MAX_PORT_FORWARDS; i++) {
		if (port_forward_rules[i].type == rule.type
		    && port_forward_rules[i].emu_port == rule.emu_port)
		{
			rpclog("Config: Discarding duplicate NAT forwarding rule for type %d emu_port %u\n",
			    rule.type, rule.emu_port);
			return;
		}
		if (port_forward_rules[i].type == rule.type
		    && port_forward_rules[i].host_port == rule.host_port)
		{
			rpclog("Config: Discarding duplicate NAT forwarding rule for type %d host_port %u\n",
			    rule.type, rule.host_port);
			return;
		}
	}

	// Find an empty slot and fill it in
	for (i = 0; i < MAX_PORT_FORWARDS; i++) {
		if (port_forward_rules[i].type == PORT_FORWARD_NONE) {
			port_forward_rules[i] = rule;
			return;
		}
	}

	// No slot found for rule
	rpclog("Config: Ran out of space for NAT port forward rules\n");
}

/**
 * Remove a forwarding rule in the NAT
 *
 * @param type      TCP or UDP
 * @param emu_port  port number on emulated machine
 * @param host_port port number on host machine
 */
void
rpcemu_nat_forward_remove(PortForwardRule rule)
{
	int i;

	for (i = 0; i < MAX_PORT_FORWARDS; i++) {
		if (port_forward_rules[i].type == rule.type
		    && port_forward_rules[i].emu_port == rule.emu_port
		    && port_forward_rules[i].host_port == rule.host_port)
		{
			port_forward_rules[i].type      = PORT_FORWARD_NONE;
			port_forward_rules[i].emu_port  = 0;
			port_forward_rules[i].host_port = 0;

			return;
		}
	}

	// rule not found, should be impossible
	assert(0);
}

/**
 * Write the execution-loop state to a suspend snapshot.
 *
 * 'cycles' is the residual cycle budget of the current execrpcemu() chunk;
 * restoring it means the first chunk after resume runs exactly as many
 * cycles as the interrupted run would have, keeping device/timer callbacks
 * aligned to the same instruction boundaries.
 */
void
rpcemu_savestate(FILE *f)
{
	savestate_write_i32(f, cycles);
	savestate_write_u32(f, inscount);
}

/**
 * Restore the execution-loop state from a suspend snapshot.
 */
void
rpcemu_loadstate(FILE *f)
{
	cycles = savestate_read_i32(f);
	inscount = savestate_read_u32(f);
}
