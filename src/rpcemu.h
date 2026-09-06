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

/* Main header file */

#ifndef _rpc_h
#define _rpc_h

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* printf-style format checking for our logging helpers. On MinGW the default
   "printf" archetype is ms_printf, which rejects C99 conversions like %zu/%ll;
   __MINGW_PRINTF_FORMAT (defined by <stdio.h>) tracks the active CRT and is
   gnu_printf when built with __USE_MINGW_ANSI_STDIO, matching runtime support. */
#if defined(__MINGW_PRINTF_FORMAT)
#  define RPCEMU_FORMAT_PRINTF(fmt, args) __attribute__((format(__MINGW_PRINTF_FORMAT, fmt, args)))
#elif defined(__GNUC__)
#  define RPCEMU_FORMAT_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#  define RPCEMU_FORMAT_PRINTF(fmt, args)
#endif

#include "iomd.h"
#include "superio.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*
 * Application version — canonical value is the VERSION file at the project root.
 * CMake passes it as -DRPCEMU_VERSION="..."; VERSION is the macro used in C code.
 */
#ifndef RPCEMU_VERSION
#define RPCEMU_VERSION "unknown"
#endif
#define VERSION RPCEMU_VERSION

/* URLs used for the help menu weblinks */
/* Help > Online Manual. Points at the manual, not at the docs directory: that
   listing is reference material for people who already know what they are
   looking for, and the menu item promises a manual. */
#define URL_MANUAL  "https://github.com/andrewtimmins/rpcemu-extended/blob/main/MANUAL.md"
#define URL_WEBSITE "https://github.com/andrewtimmins/rpcemu-extended"
#define URL_ISSUES  "https://github.com/andrewtimmins/rpcemu-extended/issues"
#define URL_RELEASES "https://github.com/andrewtimmins/rpcemu-extended/releases"
#define URL_RELEASE_TAG "https://github.com/andrewtimmins/rpcemu-extended/releases/tag/"
#define URL_LATEST_RELEASE_API "https://api.github.com/repos/andrewtimmins/rpcemu-extended/releases/latest"
/* RISC OS Open, who publish the operating system this emulator runs. */
#define URL_RISCOSOPEN "https://www.riscosopen.org/"
/* Their licensing terms, and their donations page: both are acknowledged before
   anything is downloaded from them (src/gui/riscos_fetch.cpp). */
#define URL_ROOL_LICENCES "https://www.riscosopen.org/content/documents/licences"
#define URL_ROOL_DONATE "https://www.riscosopen.org/content/donations"
#define URL_APACHE_LICENCE "https://www.apache.org/licenses/LICENSE-2.0"

#if !defined(_DEBUG) && !defined(NDEBUG)
#define NDEBUG
#endif

/* If we're not using GNU C, elide __attribute__ */
#ifndef __GNUC__
# define __attribute__(x) /*NOTHING*/
#endif

#if defined __linux || defined __linux__ || defined _WIN32 || defined __APPLE__
#define RPCEMU_NETWORKING
#endif

/*This makes the RISC OS mouse pointer follow the host pointer exactly. Useful
  for Linux port, however use mouse capturing if possible - mousehack has some
  bugs*/
#define mousehack	(config.mousehackon)

/** The type of networking configured */
typedef enum {
	NetworkType_Off,
	NetworkType_NAT,
	/*
	 * Ethernet bridging and IP tunnelling were 2 and 3. They are gone: only
	 * Linux ever implemented them, the other platforms silently did nothing
	 * while the machine editor still offered the choice, and NAT with port
	 * forwarding covers what they were for. The numbers are not reused, so a
	 * snapshot or a configuration written by an older build still reads
	 * unambiguously.
	 */
} NetworkType;

#define DEBUGGER_MAX_BREAKPOINTS 64
#define DEBUGGER_MAX_WATCHPOINTS 32

/** Longest breakpoint condition expression, including the terminator. */
#define DEBUGGER_MAX_CONDITION 64

typedef enum {
	DebugPauseReason_None = 0,
	DebugPauseReason_User = 1,
	DebugPauseReason_Breakpoint = 2,
	DebugPauseReason_Watchpoint = 3,
	DebugPauseReason_Step = 4,
	DebugPauseReason_Exception = 5,
	DebugPauseReason_Swi = 6,
	/* A guest write asked for one of the nine mode values the architecture
	   reserves. Not a trap the user turns on: it is always wrong, and the
	   emulator used to answer it by ending the process. */
	DebugPauseReason_BadMode = 7,
	/* The guest executed the debugger's breakpoint SWI. Not a trap the user
	   switches on: the instruction is in their own code and putting it there
	   is the request. */
	DebugPauseReason_BreakSwi = 8
} DebugPauseReason;

/**
 * The SWI a guest can execute to stop the machine in the debugger.
 *
 * `SWI &FFFFFF`, the instruction word `0xEFFFFFFF`, chosen because it is in the
 * unallocated range and so cannot collide with anything RISC OS or a module
 * offers. Suggested in discussion #223 by somebody who had been getting the
 * same effect by trapping a SWI in his own module's handler - a breakpoint you
 * can put in the source is worth a great deal when the address moves every time
 * the code is rebuilt.
 *
 * The emulator swallows it: it never reaches RISC OS, so no "SWI not known"
 * error, and execution resumes at the instruction after it. With the debugger
 * disabled it is swallowed and nothing else happens, so code carrying one still
 * runs.
 *
 * Matched on the whole 24-bit comment field rather than on opSWI()'s masked
 * `swinum`, which folds bits out and would let other SWIs alias onto it.
 */
#define RPCEMU_SWI_BREAKPOINT	0x00ffffffu

typedef struct {
	uint32_t address;
	uint32_t size;
	uint8_t on_read;
	uint8_t on_write;
	uint8_t log_only;	/**< Emit a trace event instead of halting */
	uint8_t reserved1;
} DebugWatchpointInfo;

/**
 * A breakpoint.
 *
 * More than an address, because "stop here" is rarely the question. The
 * interesting one is "stop here when R0 is zero", or "stop here the eleventh
 * time round", and without those the only way to reach an interesting state is
 * to sit on the continue button.
 *
 * The two counters exist to make a quiet breakpoint explicable. hit_count
 * counts every time the address was reached while armed, whether or not it
 * halted, so "the condition is never true" can be told apart from "this code
 * never runs" - which look identical from outside and mean very different
 * things. eval_errors counts conditions that could not be evaluated at all,
 * which in practice means a dereference of an address that was not mapped at
 * the time; those do not halt, and without a count they would be invisible.
 */
typedef struct {
	uint32_t address;
	uint8_t enabled;	/**< Cleared to keep a breakpoint without arming it */
	uint8_t one_shot;	/**< Remove once it halts; backs step-over and run-to */
	uint8_t has_condition;	/**< condition[] holds an expression to evaluate */
	uint8_t reserved0;
	uint32_t ignore_count;	/**< Matches still to be skipped before halting */
	uint32_t hit_count;	/**< Times the address was reached while armed */
	uint32_t eval_errors;	/**< Times the condition could not be evaluated */
	char condition[DEBUGGER_MAX_CONDITION];
} DebugBreakpointInfo;

/** Deepest call stack debugger_backtrace() will walk. */
#define DEBUGGER_MAX_FRAMES 64

/**
 * One frame of a call stack.
 *
 * Recovered by walking the APCS frame-pointer chain from R11, so it depends on
 * the code having been compiled to keep one. A good deal of RISC OS is
 * assembler that does not, which is why debugger_backtrace() reports whether
 * the walk ended cleanly or gave up.
 */
typedef struct {
	uint32_t pc;	/**< Where execution is, or will resume, in this frame */
	uint32_t lr;	/**< Live link register; only meaningful for frame 0 */
	uint32_t sp;
	uint32_t fp;
} DebugFrame;

/** Categories of event captured by the debug trace ring */
typedef enum {
	TraceEvent_Exception = 0,
	TraceEvent_Swi = 1,
	TraceEvent_Watchpoint = 2
} TraceEventType;

/** Kind values carried in DebugTraceEvent.arg0 for TraceEvent_Exception */
typedef enum {
	TraceException_Undefined = 0,
	TraceException_PrefetchAbort = 1,
	TraceException_DataAbort = 2,
	/* Not an ARM exception - a reserved CPU mode. It travels with these
	   because it is the same shape of event, and arg1 carries the mode
	   that was asked for in place of a fault address. */
	TraceException_BadMode = 3
} TraceExceptionKind;

/** A single entry in the debug trace ring. Pure POD, copied between threads. */
typedef struct DebugTraceEvent {
	uint32_t seq;		/**< Monotonic sequence number; gaps imply drops */
	uint32_t type;		/**< TraceEventType */
	uint32_t pc;		/**< Faulting / calling PC */
	uint32_t opcode;	/**< Instruction word (0 if not available) */
	uint32_t arg0;		/**< exc: TraceExceptionKind | swi: number | wp: address */
	/* exc: the CP15 Fault Address and Fault Status of a DATA ABORT, which is
	   what distinguishes a translation fault from a permission fault - the
	   difference between "nothing is mapped there" and "the MMU refused the
	   access", and the only way to tell them apart from outside.
	   Both are zero for a prefetch abort and for an undefined instruction:
	   cp15.c updates the fault registers for data aborts only (see do_fault),
	   so for the others they still hold whatever an earlier data abort left
	   behind, and reporting that would be worse than reporting nothing. */
	uint32_t arg1;		/**< exc: fault address | swi: R0 | wp: value */
	uint32_t arg2;		/**< exc: fault status | swi: cpsr flags | wp: (size << 1) | is_write */
} DebugTraceEvent;

/** Runtime configuration of debug tracing/trapping, set from the GUI */
typedef struct DebugTraceConfig {
	uint8_t trap_undefined;		/**< Halt on undefined instruction */
	uint8_t trap_prefetch_abort;	/**< Halt on prefetch abort */
	uint8_t trap_data_abort;	/**< Halt on data abort */
	uint8_t log_exceptions;		/**< Also emit exception events to the ring */
	uint8_t swi_trace_enabled;	/**< Emit SWI events to the ring */
	uint8_t swi_trace_halt;		/**< Halt on a matching SWI */

	/**
	 * While stepping, do not stop inside an interrupt.
	 *
	 * "When debugging main code in games, you're not interested in seeing OS or
	 * IRQ code execute" - discussion #223. A step that lands in IRQ or FIQ mode
	 * keeps going until it is back out. Breakpoints and watchpoints still fire
	 * wherever they are set: an explicit request is not noise.
	 */
	uint8_t step_skip_irq;

	/**
	 * While stepping, do not stop in the OS.
	 *
	 * Anything at or above 0xf0000000, where the ROM lives, and the hardware
	 * vector page below 0x40. Same rule as above: stepping skips it,
	 * breakpoints do not.
	 */
	uint8_t step_skip_os;

	/**
	 * While stepping, let a SWI run to completion.
	 *
	 * The other two filters are written on where the PC is, which covers the
	 * OS's own SWIs because their handlers are in the ROM. It does not cover a
	 * SWI belonging to a module loaded into RAM - the debugger's own, a
	 * filing system, or the very module being debugged - and a step into one
	 * of those lands in its handler.
	 *
	 * This one is written on the instruction instead: a step that executes a
	 * SWI runs to the instruction after it, whatever the handler is and
	 * wherever it lives. "Unless it's an SWI you've actively trapped, these
	 * all execute to completion" - discussion #223. Trapped is the exception
	 * that still stops: a breakpoint inside the handler, a halting SWI trap,
	 * and the debugger's own breakpoint SWI all fire as they otherwise would.
	 */
	uint8_t step_skip_swi;
	uint32_t swi_filter_min;	/**< Inclusive SWI-number range (0..0xffffffff = all) */
	uint32_t swi_filter_max;
} DebugTraceConfig;

typedef struct {
	int paused;
	int pause_requested;
	DebugPauseReason reason;
	uint32_t halt_pc;
	uint32_t halt_opcode;
	/**
	 * The instruction that branched to a prefetch abort, or zero.
	 *
	 * A prefetch abort is the one exception whose "faulting instruction" does
	 * not exist: the address could not be fetched, so there is nothing there
	 * to disassemble. What somebody wants is the instruction that sent
	 * execution there, and that is the one the core executed immediately
	 * before. Set only for a prefetch abort, and only when the instruction
	 * hook was running - which trapping one guarantees. Discussion #223.
	 */
	uint32_t halt_from_pc;
	uint32_t last_pc;
	uint32_t last_opcode;
	uint32_t hit_address;
	uint32_t hit_value;
	uint8_t hit_size;
	uint8_t hit_is_write;
	uint8_t step_active;
	uint8_t reserved;
	uint32_t breakpoint_count;
	DebugBreakpointInfo breakpoints[DEBUGGER_MAX_BREAKPOINTS];
	uint32_t watchpoint_count;
	DebugWatchpointInfo watchpoints[DEBUGGER_MAX_WATCHPOINTS];
} DebuggerStatus;

/** Selection of models that the emulator can emulate,
  must be kept in sync with models[] array in rpcemu.c
  the size of model_selection gui.c must be Model_MAX */
typedef enum {
	Model_RPCARM610,
	Model_RPCARM710,
	Model_RPCSA110,
	Model_A7000,
	Model_A7000plus,
	Model_RPCARM810,
	Model_Phoebe,
	Model_Kinetic,
	Model_MAX         /**< Always last entry */
} Model;

/** The type of processor configured */
typedef enum {
	CPUModel_ARM610,
	CPUModel_ARM710,
	CPUModel_SA110,
	CPUModel_ARM7500,
	CPUModel_ARM7500FE,
	CPUModel_ARM810
} CPUModel;

/**
 * How many USB ports the machine has.
 *
 * OHCI's root hub allows up to fifteen. Four is what the interface offers -
 * more than the two the ISP1161 had, and enough that nothing has to be
 * unplugged to try something else.
 */
#define USB_PORTS 4

/**
 * What can be plugged into an emulated USB port.
 *
 * 1 was a synthesised gamepad, used to exercise the controller before there was
 * any way to plug in a real device. The number is not reused: a machine set up
 * when it existed still names it, and its port is better left empty than filled
 * with something else.
 */
typedef enum {
	UsbAttachment_None = 0,
	UsbAttachment_Host = 2	/**< A real device on the host, through libusb */
} UsbAttachment;

/*
 * The display settings.
 *
 * These began as three independent switches - integer_scaling, fit_to_window and
 * follow_host_display - which was the source of most of the confusion around
 * them. They then became two choices: where the RISC OS desktop's size came from
 * (best for this display, follow the window, or a fixed size) and how it was
 * drawn (actual size, whole multiples, fill the window).
 *
 * The automatic halves of that are gone, and what is left is a screen size and
 * how to draw it. The reason is worth recording, because on paper "the desktop
 * follows the window" is the nicer feature:
 *
 *   RISC OS will not adopt an arbitrary screen mode. It accepts only modes the
 *   monitor definition in force declares, and on a real machine that definition
 *   is a definition file the guest's own !Boot loads - not the EDID this
 *   emulator synthesises. Measured on a machine with the graphics card fitted,
 *   seven of the thirteen modes that fit its display memory were refused, and
 *   not the ones anybody would guess: 1920x1200 accepted, 1920x1080 refused.
 *
 * So a desktop that follows the window can only ever land on a coarse and
 * unpredictable set of sizes. Every intermediate window size left either a black
 * border or a stretch, and every mode change moved and resized the window while
 * the user was still dragging it. Chasing the window was producing worse
 * behaviour than not chasing it.
 *
 * A named resolution has none of those problems. It is advertised as the
 * monitor's native mode, so the machine boots straight into it; the window is
 * that size and stays that size; and nothing moves on its own.
 */

/** How the RISC OS desktop is drawn in the window. */
typedef enum {
	/** One guest pixel per host pixel. The window is the size of the desktop. */
	DisplayScaling_ActualSize = 0,

	/**
	 * Whole-number multiples only (2x, 3x): scaled up, still perfectly sharp.
	 * The window may be resized, and the desktop is centred in it.
	 */
	DisplayScaling_WholeMultiples = 1
} DisplayScaling;

/** The user's configuration of the emulator */
typedef struct {
	char name[256];		/**< User-defined name for this configuration */
	char hd4_path[512];	/**< Path to the hard disk image file (optional override) */
	/**
	 * Where this machine's HostFS drive is, or empty for the default.
	 *
	 * Empty means <machine directory>/hostfs, a relative value resolves under
	 * the machine directory, and an absolute one is taken as given - the same
	 * convention as hd4_path. Several machines can therefore be pointed at one
	 * folder, which is discussion #77. Nothing is stored for the default case,
	 * so a configuration does not go stale when its data folder moves. See
	 * hostfs_path.h.
	 */
	char hostfs_path[512];
	char rom_dir[256];	/**< ROM directory name within roms/ folder */
	unsigned mem_size;	/**< Amount of RAM in megabytes */
	unsigned vram_size;	/**< Amount of VRAM in megabytes */
	char *macaddress;
	int refresh;		/**< Video refresh rate */
	int soundenabled;
	int cdromenabled;
	int cdromtype;
	char isoname[512];
	int mousehackon;
	int mousetwobutton;	/**< Swap the behaviour of the right and middle
	                             buttons, for mice with two buttons */
	NetworkType network_type;

	/*
	 * A JSON tun/tap server, so machines here and any other emulator speaking
	 * the protocol can share one virtual network - RISC OS Pyromaniac, whose
	 * author devised it (Charles Ferguson), among them. See net_json.h. A
	 * machine using this does not use the loopback
	 * wire between local machines: both are hubs, and being on both would
	 * deliver every frame twice.
	 */
	int json_net_enabled;		/**< Join a JSON server rather than the local wire */
	char json_net_host[256];	/**< Host running the server */
	int json_net_port;		/**< Its port; 33445 is the server's own default */

	/*
	 * The Community Network: the same JSON transport, joining one shared
	 * server that everybody using this option joins, rather than a server the
	 * user runs. Its address is not a setting - see COMMUNITY_NET_HOST in
	 * net_json.h - so that everybody who ticks the box lands on the same wire.
	 *
	 * It is an open network of strangers with no encryption and no
	 * authentication, which is why turning it on asks the user to agree to
	 * something first (see the machine editor). Where both this and a server of
	 * the user's own are set, this one wins; net_json_target() decides.
	 */
	int community_net_enabled;	/**< Join the shared Community Network */
	int cpu_idle;		/**< Attempt to reduce CPU usage */
	int show_fullscreen_message;	/**< Show explanation of how to leave fullscreen, on entering fullscreen */
	int display_scaling;	/**< How the guest's screen is drawn in the window (DisplayScaling) */
	unsigned screen_size_x;	/**< RISC OS screen width, or 0 to choose one at first start */
	unsigned screen_size_y;	/**< RISC OS screen height, or 0 to choose one */
	int gfxcard_enabled;	/**< Present the graphics expansion card (its own framestore, modes beyond VRAM) */
	int usb_port[USB_PORTS];	/**< What is plugged into each emulated USB port (UsbAttachment) */
	char usb_host[USB_PORTS][16];	/**< For a UsbAttachment_Host port, the device's "vvvv:pppp" */
	int gfxcard_boot_display;	/**< Let the card take the display as the machine boots */
	int accelerators_enabled;	/**< Let the host do guest work it can do identically (see accelerators.h) */

	char *network_capture;		///< Path to capture network traffic file, or NULL to disable
	int vnc_enabled;	/**< Enable the built-in VNC server */
	int vnc_port;		/**< Port for the VNC server (default 5900) */
	char vnc_password[64];	/**< Password for VNC authentication (empty = no auth) */
	char vnc_password_readonly[64];	/**< Password for view-only VNC clients (empty = disabled) */
	int hostcmd_enabled;	/**< Enable the HostCmd control socket (host drives the RISC OS CLI) */
	/**
	 * Which host interface the Access broadcast relay should use, or empty to
	 * choose one. An application setting rather than a machine one, because the
	 * relay is claimed once per host - see app_settings.h.
	 *
	 * Matched against the interface name on Unix (en0, eth0) and against both
	 * the adapter name and its description on Windows, case-insensitively, as a
	 * substring so "Realtek" is enough. Issue #205: with a VPN connected the
	 * automatic choice took the VPN's adapter, where no Access peer can hear it.
	 */
	char relay_interface[128];

	char hostcmd_socket[512];	/**< Socket spec: empty = <machinedir>hostcmd.sock (AF_UNIX); a path = AF_UNIX; a bare port = TCP 127.0.0.1:port */
	int netcap_enabled;	/**< Enable the NetCapCmd control socket (host captures and reads network frames) */
	char netcap_socket[512];	/**< Socket spec: empty = <machinedir>rpcemu-netcap.sock (AF_UNIX); a path = AF_UNIX; a bare port = TCP 127.0.0.1:port */
	int debug_enabled;	/**< Enable the DebugCmd control socket (host inspects/controls the emulated CPU) */
	char debug_socket[512];	/**< Socket spec: empty = <machinedir>rpcemu-debug.sock (AF_UNIX); a path = AF_UNIX; a bare port = TCP 127.0.0.1:port */
	Model model;		/**< Configured machine model. Applied to machine.model on load; kept here so the configured model persists independently of the running machine.model (fixes model edits to a running machine being lost on save). */
	int clipboard_enabled;	/**< Share the host clipboard with the guest (needs the SharedClipboard module in the guest) */
	int start_fullscreen;	/**< Go full screen as soon as this machine starts */
	int suspend_on_exit;	/**< Auto-save a machine snapshot on every exit (so the next launch can Resume). Off by default: normal Quit shuts down cleanly, and only File->Suspend / Save State write a snapshot. */
	/**
	 * What is fitted to the OPEN Bus second processor slot: a core name as
	 * --openbus-card accepts it ("rv32i", "6502", "z80"), or empty for an
	 * empty slot, which is what every Risc PC had unless somebody bought a
	 * card.
	 *
	 * Kept as the NAME rather than an enumeration on purpose: the list of
	 * cores then exists in exactly one place (openbus_coproc.c), and the
	 * option parser, this configuration and the machine editor all read it
	 * from there instead of each carrying their own copy to fall out of step.
	 */
	char openbus_card[16];

	/*
	 * How much RAM the co-processor card carries, in KILOBYTES. Zero means
	 * the core's own default, which is what a machine that has never had this
	 * set will read.
	 *
	 * In KB rather than bytes because that is the unit the choice is offered
	 * in and it keeps a hand-edited value readable; the card clamps whatever
	 * arrives to what the fitted processor can actually address, so a 6502
	 * cannot be given more than its 64K however this is set.
	 */
	unsigned openbus_ram_kb;
} Config;

extern Config config;

/** Structure to hold details about a model that the emulator can emulate */
typedef struct {
	const char	*name_gui;	/**< String used in the GUI */
	const char	*name_config;	/**< String used in the Config file to select model */
	CPUModel	cpu_model;	/**< CPU used in this model */
	IOMDType	iomd_type;	/**< IOMD used in this model */
	SuperIOType	super_type;     /**< SuperIO chip used in this model */
	uint32_t        i2c_devices;    /**< Bitfield of devices on the I2C bus */
} Model_Details;

extern const Model_Details models[]; /**< array of details of models the emulator can emulate */

/** Structure to hold hardware details of the current model being emulated
 (cached values of Model_Details for speed of lookup) */
typedef struct {
	Model		model;		/**< enum value of model */
	CPUModel	cpu_model;	/**< CPU used in this model */
	IOMDType	iomd_type;	/**< IOMD used in this model */
	SuperIOType	super_type;     /**< SuperIO chip used in this model */
	uint32_t        i2c_devices;    /**< Bitfield of devices on the I2C bus */
} Machine;

extern Machine machine; /**< The details of the current model being emulated */

typedef enum {
	PORT_FORWARD_NONE = 0,		///< No valid rule stored
	PORT_FORWARD_TCP  = 1,		///< A TCP rule
	PORT_FORWARD_UDP  = 2,		///< A UDP rule
	// All other values reserved
} PortForwardType;

typedef struct {
	PortForwardType	type;		///< Which type of rule to use, or NONE for no rule
	uint16_t	emu_port;	///< Port to connect to on the emulated machine
	uint16_t	host_port;	///< Port to connect to on the host machine
} PortForwardRule;

#define MAX_PORT_FORWARDS 32

extern PortForwardRule port_forward_rules[MAX_PORT_FORWARDS]; ///< Port forward rules accross the NAT

extern void rpcemu_nat_forward_add(PortForwardRule rule);
extern void rpcemu_nat_forward_remove(PortForwardRule rule);

extern uint32_t inscount;

/**
 * Whether the host front end has queued anything for the emulator thread.
 *
 * Supplied by the front end. The idle loop stops when this is true so that the
 * main loop can act on it; see the comment on the flag in emulator_host.cpp.
 */
extern int rpcemu_host_commands_pending(void);

/* Activity counters for status bar indicators (implemented in wx host) */
extern void hostfs_activity_increment(void);
extern void network_activity_increment(void);
extern void ide_activity_increment(void);
extern void fdc_activity_increment(void);

/* These functions can optionally be overridden by a platform. If not
   needed to be overridden, there is a generic version in rpc-machdep.c */
extern const char *rpcemu_get_datadir(void);
extern const char *rpcemu_get_resourcedir(void);

/*
 * Host display geometry.
 *
 * Two sizes, because the two callers want different ones. The full geometry is
 * what the display can show and so what full-screen and scale-to-fit can use;
 * the work area is what a window is allowed to occupy, which is smaller by a
 * menu bar, a dock or a taskbar. Advertising a native mode larger than the work
 * area gives a 1:1 window that will not fit on screen.
 *
 * @param work_width  Work-area width, or 0 if the caller does not know it
 * @param work_height Work-area height, or 0 if unknown
 */
extern void rpcemu_set_host_display(unsigned width, unsigned height, unsigned hz,
                                    unsigned work_width, unsigned work_height);
extern int rpcemu_get_host_display(unsigned *width, unsigned *height);

/*
 * The mode to advertise as the monitor's native one: the configured screen size.
 *
 * This is what makes a named resolution reliable. RISC OS reads the monitor EDID
 * as it boots, and the mode it comes up in is the native one that block declares,
 * so advertising the configured size means the machine starts in it - verified
 * against a size that the same machine refuses once it is running, because by
 * then its own monitor definition file has replaced the EDID.
 *
 * Falls back to the largest standard mode that fits this display's work area and
 * the machine's display memory, for a machine that has never been given a size.
 * The work area rather than the whole display, because the window is this size:
 * a taller one would open with its title bar off the top of the screen.
 *
 * @return non-zero if a bound is known, zero to use the built-in default
 */
extern int rpcemu_edid_bound(unsigned *width, unsigned *height);

/**
 * The screen size a machine with none configured should start with.
 *
 * @return non-zero if one could be chosen
 */
extern int rpcemu_default_screen_size(unsigned *width, unsigned *height);

/*
 * Ask the guest to change to a screen mode. Quantised through display_mode_fit()
 * before it is stored, so a size that is not a mode becomes the nearest one that
 * is, and asking twice for the same mode says nothing the second time.
 */
extern void rpcemu_request_guest_size(unsigned width, unsigned height);

/*
 * The same, but "force" asks again even for the size already recorded.
 *
 * For a size the user named, where the record is of what was asked for rather
 * than of what RISC OS is showing: it may have changed mode from its own end, or
 * refused the recorded size, and in both cases asking again must reach it.
 */
extern void rpcemu_request_guest_size_ex(unsigned width, unsigned height,
                                         int force);

/*
 * What rpcemu_request_guest_size() would settle on for this size, without
 * asking for anything.
 *
 * The front end needs to know which mode a request will become so it can check
 * that the guest actually adopted it: RISC OS refuses a mode its monitor
 * definition does not declare and tells the host nothing, so the only evidence
 * is the desktop still being its old size, and that comparison needs the fitted
 * size rather than the raw one.
 *
 * @return non-zero if a mode fits, zero if nothing does
 */
extern int rpcemu_guest_size_for(unsigned width, unsigned height,
                                 unsigned *fitted_width,
                                 unsigned *fitted_height);

/* How much display memory a mode has to fit in: the fitted VRAM, or the graphics
   card's own framestore when that card is present. Both the synthesised EDID and
   the run-time mode chooser ask here, so the two cannot disagree. */
extern size_t rpcemu_display_memory(void);

/* The mode the guest should adopt to follow the host, already bounded by the
   host display and by the display memory available, plus a generation that
   changes when the host display does. Read by the guest support module through
   its SWI. */
extern int rpcemu_guest_display_target(unsigned *width, unsigned *height,
                                       unsigned *hz, uint32_t *generation);

/* Request a clean application quit from the emulator core (e.g. the guest
   asking to power off via OS_Reset "&OFF"). Routed to the front-end. */
extern void rpcemu_request_poweroff(void);

extern void rpcemu_set_datadir(const char *path);
extern void rpcemu_set_resourcedir(const char *path);
extern const char *rpcemu_get_machine_datadir(void);
extern void rpcemu_set_machine_datadir(const char *machine_name);
extern void rpcemu_machine_datadir_for(char *out, size_t size, const char *machine_name);
extern const char *rpcemu_get_log_path(void);

/**
 * Create @path and any missing parents, ignoring one that already exists.
 *
 * Exported so hostfs_init() can create a HostFS folder the configuration points
 * at. ensure_machine_dirs() only ever makes <machine dir>/hostfs, so a
 * configured folder elsewhere would not exist and the guest would have no
 * HostFS at all.
 */
extern void rpcemu_ensure_dir(const char *path);

/* rpc.c */
typedef struct {
	uint64_t	size;		/**< Size of disk */
	uint64_t	free;		/**< Free space on disk */
} disk_info;

extern void fatal(const char *format, ...)
	RPCEMU_FORMAT_PRINTF(1, 2) __attribute__((noreturn));
extern void error(const char *format, ...)
	RPCEMU_FORMAT_PRINTF(1, 2);

extern int path_disk_info(const char *path, disk_info *d);

extern void updateirqs(void);

extern void sound_thread_wakeup(void);
extern void sound_thread_start(void);
extern void sound_thread_close(void);
extern void plt_sound_set_muted(int muted);
extern int plt_sound_is_muted(void);

/* Additional logging functions (optional) */
extern void rpcemu_log_os(void);
extern void rpcemu_log_platform(void);

/* rpcemu.c */
extern void rpcemu_prestart(void);
extern void rpcemu_start(void);
extern void execrpcemu(void);
extern void rpcemu_idle(void);
extern void endrpcemu(void);
extern void resetrpc(void);
extern void rpcemu_floppy_load(int drive, const char *filename);
extern void rpcemu_floppy_eject(int drive);
extern void rpclog(const char *format, ...)
	RPCEMU_FORMAT_PRINTF(1, 2);
/*
 * Close the log file, so the next rpclog() opens it again at whatever
 * rpcemu_get_log_path() says by then.
 *
 * For moving the data folder: the log lives inside it and we are holding it open,
 * and Windows will not move a file that is open. Nothing else needs this - the log
 * is opened lazily and never otherwise closed.
 */
extern void rpclog_close(void);
extern void rpcemu_model_changed(Model model);
extern const char *rpcemu_file_get_extension(const char *filename);
extern int rpcemu_config_is_reset_required(const Config *new_config, Model new_model);
extern void rpcemu_config_apply_new_settings(Config *new_config, Model new_model);

extern void debugger_get_status(DebuggerStatus *status);
extern int debugger_is_paused(void);
extern int debugger_requires_instruction_hook(void);
extern void debugger_request_pause(DebugPauseReason reason);
extern void debugger_resume(void);
extern void debugger_single_step(uint32_t instruction_count);
extern int debugger_step_over(void);
extern int debugger_step_out(void);
extern int debugger_run_to(uint32_t address);
extern uint32_t debugger_backtrace(DebugFrame *out, uint32_t max, int *truncated);
extern void debugger_clear_breakpoints(void);
extern int debugger_add_breakpoint(uint32_t address);
extern int debugger_add_breakpoint_ex(uint32_t address, const char *condition,
                                      uint32_t ignore_count, int one_shot);
extern int debugger_remove_breakpoint(uint32_t address);
extern int debugger_has_breakpoint(uint32_t address);
extern int debugger_set_breakpoint_enabled(uint32_t address, int enabled);
extern const DebugBreakpointInfo *debugger_get_breakpoint(uint32_t address);
extern void debugger_clear_watchpoints(void);
extern int debugger_add_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write, int log_only);
extern int debugger_remove_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write);
extern int debugger_instruction_hook(uint32_t pc, uint32_t opcode);
extern void debugger_memory_access(uint32_t address, uint32_t size, int is_write, uint32_t value);
extern void debugger_after_instruction(uint32_t pc, uint32_t opcode);

/* Debug tracing: exception trapping, SWI tracing, logging watchpoints */
extern int debugger_swi_trace_active;	/**< Fast gate read from opSWI() */

/** Fast gate read once per instruction by the interpreter and the recompiler's
    dispatch. Non-zero when something (a breakpoint, a watchpoint, a step, a
    trap, or a pause) needs to see each instruction. Equivalent to calling
    debugger_requires_instruction_hook(), but without the call. */
extern int debugger_hook_active;
extern void debugger_set_trace_config(const DebugTraceConfig *cfg);
extern void debugger_get_trace_config(DebugTraceConfig *cfg);
extern uint32_t debugger_trace_pending(void);
extern uint32_t debugger_drain_trace_events(DebugTraceEvent *out, uint32_t max, uint32_t *dropped);
extern void debugger_exception_hook(uint32_t mmode, uint32_t address, uint32_t pc);
extern int debugger_swi_hook(uint32_t swinum, uint32_t opcode);
extern int debugger_bad_mode(uint32_t mode, uint32_t pc);

/** debugger_set_register(): the CPSR, rather than one of R0-R15. */
#define DEBUG_REG_CPSR	16
/**
 * debugger_set_register(): the PC as everything reports it.
 *
 * Distinct from 15, which is the raw R15 - the two are a pipeline apart. See
 * debugger_set_register().
 */
#define DEBUG_REG_PC	17

extern int debugger_set_register(int reg, uint32_t value);

/** How many register banks debugger_get_banked_registers() reports. */
#define DEBUG_BANK_COUNT 6

/**
 * One processor mode's banked registers, as a debugger shows them.
 *
 * Only the registers that are actually banked. R0-R7 are shared by every mode
 * and R15 is the PC, so repeating them per mode would be noise; R8-R12 are
 * shared by every mode except FIQ, which is why they appear here only for FIQ.
 */
typedef struct {
	uint32_t mode;		/**< Full mode value, e.g. 0x11 for FIQ32 */
	const char *name;	/**< "USR", "FIQ", "IRQ", "SVC", "ABT", "UND" */
	uint32_t r13;
	uint32_t r14;
	uint32_t spsr;
	uint8_t has_spsr;	/**< User and System bank none */
	uint8_t is_current;	/**< This is the mode the machine is in */
	uint8_t banks_r8_r12;	/**< FIQ only; r8_r12 is meaningful */
	uint8_t reserved;
	uint32_t r8_r12[5];
} DebugBankedRegs;

extern uint32_t debugger_get_banked_registers(DebugBankedRegs *out, uint32_t max);

/**
 * The guest executed RPCEMU_SWI_BREAKPOINT.
 *
 * @param pc Address of the SWI itself
 * @return 1 if the machine was halted, 0 if there was no debugger to halt into
 */
extern int debugger_break_swi(uint32_t pc);

/* host GUI bridge */
extern void rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize, int yl, int yh, int double_size, int host_xsize, int host_ysize);
extern void rpcemu_move_host_mouse(uint16_t x, uint16_t y);
extern void rpcemu_pointer_shape(const uint8_t *bits, int row_bytes,
                                 const uint32_t *palette, int width, int height,
                                 int hotspot_x, int hotspot_y, int visible,
                                 int host_side);
extern int rpcemu_vnc_active(void);

/*
 * Whether the front end can draw the guest's pointer as its own cursor.
 *
 * Zero by default, and zero is the safe answer: the emulator then composites the
 * pointer into the frame as it always did. A front end that sets this is
 * promising to draw one, and gets frames without a pointer in them - so nothing
 * may set it on a path where nobody is listening. A managed machine is exactly
 * that case: its frames go to the Manager, whose panel draws them, and its own
 * window is never shown.
 */
extern int rpcemu_host_cursor_available;
extern void rpcemu_idle_process_events(void);
extern uint64_t rpcemu_nsec_timer_ticks(void);

extern int drawscre;

/* Counts passes through the idle loop, so the interface can tell an idle machine
   from a stalled one: while RISC OS is idling almost no instructions are
   executed, and a MIPS figure near zero is the truth rather than a fault. */
extern unsigned long idle_ticks;
extern int quited;
extern char discname[2][260];

/* Performance measuring variables */
extern int updatemips;
typedef struct {
	float mips;
	float mhz;
	float tlb_sec;
	float flush_sec;
	uint32_t mips_count;
	float mips_total;
} Perf;
extern Perf perf;

/* UNIMPLEMENTED requires variable argument macros
   GCC extension or C99 */
#if defined(_DEBUG) && (defined(__GNUC__) || __STDC_VERSION__ >= 199901L)
  /**
   * UNIMPLEMENTED
   *
   * Used to report sections of code that have not been implemented yet
   *
   * @param section Section code is missing from eg. "IOMD register" or
   *                "HostFS filecore message"
   * @param format  Section specific information
   * @param ...     Section specific information variable arguments
   */
  #define UNIMPLEMENTED(section, format, args...) \
    UNIMPLEMENTEDFL(__FILE__, __LINE__, (section), (format), ## args)

  void UNIMPLEMENTEDFL(const char *file, unsigned line,
                       const char *section, const char *format, ...)
	RPCEMU_FORMAT_PRINTF(4, 5);
#else
  /* This function has no corresponding body, the compiler
     is clever enough to use it to swallow the arguments to
     debugging calls */
  void unimplemented_null(const char *section, const char *format, ...)
	RPCEMU_FORMAT_PRINTF(2, 3);

  #define UNIMPLEMENTED 1?(void)0:(void)unimplemented_null

#endif

/* Acknowledge and prevent -Wunused-parameter warnings on functions
 * where the parameter is part of more generic API */
#define NOT_USED(arg)	(void) arg

/*FPA*/
extern void resetfpa(void);
extern void fpaopcode(uint32_t opcode);

/**
 * One FPA register as a debugger sees it.
 *
 * `w` is the register exactly as the chip holds it, in the 80-bit extended
 * format (sign and exponent, integer bit and high fraction, low fraction), and
 * is the authority. `value` is the same number in a host double, for display:
 * the register carries a 64-bit mantissa and a 15-bit exponent where a double
 * has 53 and 11, so a value can lose its last few digits there, and one beyond
 * the double range reads as an infinity. Read `w` when the exact bits matter.
 */
typedef struct {
	uint32_t w[3];
	double value;
} FPADebugReg;

/** The whole of the FPA's programmer-visible state, for the debugger. */
typedef struct {
	FPADebugReg reg[8];
	uint32_t fpsr;
	uint32_t fpcr;
} FPADebugState;

extern void fpa_get_state(FPADebugState *state);

/* settings.cpp */
extern void config_deep_copy(Config *dest, const Config *src);
extern void config_sync_machine_edit_to_copy(Config *dest, const Config *src);
extern void config_apply_machine_edit(Config *cfg, const char *name, const char *rom_dir,
                                      unsigned mem_size, unsigned vram_size, int refresh,
                                      NetworkType network_type);
extern void config_load(Config *config);
extern void config_load_from_path(Config *config, const char *path);
/* Has a machine's configuration been loaded yet? The ways in to a machine are its
   own settings, so whatever opens them has to know whether there is a machine to
   read them from: before one is chosen, the app settings file is all there is.
   Headless with no --machine offers the machine list over VNC, so that case is
   real rather than theoretical. */
extern int config_machine_loaded(void);
extern void config_save(Config *config);
extern void config_save_to_path(Config *config, const char *path);
extern void config_set_path(const char *path);
extern const char *config_get_path(void);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
#endif /* _rpc_h */
