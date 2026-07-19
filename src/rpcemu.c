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
#endif

#include "rpcemu.h"
#include "mem.h"
#include "vidc20.h"
#include "keyboard.h"
#include "sound.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "cmos.h"
#include "superio.h"
#include "i8042.h"
#include "romload.h"
#include "cp15.h"
#include "cdrom-iso.h"
#include "podulerom.h"
#include "podules.h"
#include "fdc.h"
#include "hostfs.h"
#include "hostcmd.h"
#include "debugcmd.h"
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

/** Array of details of models the emulator can emulate, must be kept in sync with
    Model enum in rpcemu.h */
const Model_Details models[] = {
	{ "Risc PC - ARM610",                "RPC610", CPUModel_ARM610,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - ARM710",                "RPC710", CPUModel_ARM710,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - StrongARM",             "RPCSA",  CPUModel_SA110,     IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "A7000",                           "A7000",  CPUModel_ARM7500,   IOMDType_ARM7500,   SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "A7000+ (experimental)",           "A7000+", CPUModel_ARM7500FE, IOMDType_ARM7500FE, SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Risc PC - ARM810 (experimental)", "RPC810", CPUModel_ARM810,    IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 },
	{ "Phoebe (RPC2)",                   "Phoebe", CPUModel_SA110,     IOMDType_IOMD2,     SuperIOType_FDC37C672,   I2C_PCF8583 | I2C_SPD_DIMM0 },
	{ "Risc PC - Kinetic (512MB)",       "Kinetic",CPUModel_SA110,     IOMDType_IOMD,      SuperIOType_FDC37C665GT, I2C_PCF8583 }
};

Config config = {
	"",			/* name */
	"",			/* hd4_path (empty = use machine directory) */
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
	0,			/* integer_scaling */
	NULL,			/* network_capture */
	0,			/* vnc_enabled */
	5900,			/* vnc_port */
	"",			/* vnc_password */
	1,			/* hostcmd_enabled (ON by default) */
	"",			/* hostcmd_socket (empty => <datadir>hostcmd.sock) */
	1,			/* debug_enabled (ON by default) */
	"",			/* debug_socket (empty => <datadir>rpcemu-debug.sock) */
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

int drawscre = 0;
int quited = 0;

static FILE *arclog; /* Log file handle */

static int cycles;

static uint32_t debugger_breakpoints[DEBUGGER_MAX_BREAKPOINTS];
static uint32_t debugger_breakpoint_count = 0;

static DebugWatchpointInfo debugger_watchpoints[DEBUGGER_MAX_WATCHPOINTS];
static uint32_t debugger_watchpoint_count = 0;

static int debugger_pause_requested = 0;
static DebugPauseReason debugger_pending_reason = DebugPauseReason_None;

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
		if (debugger_breakpoints[i] == address) {
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

int
debugger_requires_instruction_hook(void)
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

void
debugger_request_pause(DebugPauseReason reason)
{
	if (debugger_paused) {
		debugger_pause_reason = reason;
		return;
	}
	debugger_pause_requested = 1;
	debugger_pending_reason = reason;
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
	debugger_reset_hit_info();
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
	debugger_reset_hit_info();
}

void
debugger_clear_breakpoints(void)
{
	debugger_breakpoint_count = 0;
}

int
debugger_add_breakpoint(uint32_t address)
{
	if (debugger_breakpoint_index(address) >= 0) {
		return 1;
	}
	if (debugger_breakpoint_count >= DEBUGGER_MAX_BREAKPOINTS) {
		return 0;
	}
	debugger_breakpoints[debugger_breakpoint_count++] = address;
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
		        (debugger_breakpoint_count - (uint32_t) index - 1) * sizeof(uint32_t));
	}
	debugger_breakpoint_count--;
	return 1;
}

int
debugger_has_breakpoint(uint32_t address)
{
	return debugger_breakpoint_index(address) >= 0;
}

void
debugger_clear_watchpoints(void)
{
	debugger_watchpoint_count = 0;
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

	if (debugger_breakpoint_count > 0 && debugger_has_breakpoint(pc)) {
		debugger_pause_requested = 1;
		debugger_pending_reason = DebugPauseReason_Breakpoint;
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
        podulerom_reset(); // must be called after podules_reset()
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
        initpodulerom();
        podule_build_list();

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
		drawscre--;
		if (drawscre > 5) {
			drawscre = 0;
		}
	}

	printer_poll();
	serial_modem_poll();
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
	/* Loop while no interrupts pending */
	while (!arm.event) {
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
			if (drawscre > 0) {
				drawscr();
				drawscre--;
				if (drawscre > 5) {
					drawscre = 0;
				}
			}
			printer_poll();
			serial_modem_poll();
			hostcmd_poll();
	debugcmd_poll();
			rpcemu_idle_process_events();
		}
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

	/* The Kinetic only works reliably with the 2MB of VRAM the real hardware
	   shipped with; larger VRAM sizes fault during boot with the 512MB SDRAM
	   memory map, so clamp it here regardless of what the config requested. */
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
