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

/* Main header file */

#ifndef _rpc_h
#define _rpc_h

#include <stdio.h>
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
#define URL_MANUAL  "https://github.com/andrewtimmins/rpcemu-extended/tree/main/docs"
#define URL_WEBSITE "https://github.com/andrewtimmins/rpcemu-extended"

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
	NetworkType_EthernetBridging,
	NetworkType_IPTunnelling,
} NetworkType;

#define DEBUGGER_MAX_BREAKPOINTS 64
#define DEBUGGER_MAX_WATCHPOINTS 32

typedef enum {
	DebugPauseReason_None = 0,
	DebugPauseReason_User = 1,
	DebugPauseReason_Breakpoint = 2,
	DebugPauseReason_Watchpoint = 3,
	DebugPauseReason_Step = 4,
	DebugPauseReason_Exception = 5,
	DebugPauseReason_Swi = 6
} DebugPauseReason;

typedef struct {
	uint32_t address;
	uint32_t size;
	uint8_t on_read;
	uint8_t on_write;
	uint8_t log_only;	/**< Emit a trace event instead of halting */
	uint8_t reserved1;
} DebugWatchpointInfo;

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
	TraceException_DataAbort = 2
} TraceExceptionKind;

/** A single entry in the debug trace ring. Pure POD, copied between threads. */
typedef struct DebugTraceEvent {
	uint32_t seq;		/**< Monotonic sequence number; gaps imply drops */
	uint32_t type;		/**< TraceEventType */
	uint32_t pc;		/**< Faulting / calling PC */
	uint32_t opcode;	/**< Instruction word (0 if not available) */
	uint32_t arg0;		/**< exc: TraceExceptionKind | swi: number | wp: address */
	uint32_t arg1;		/**< exc: abort address | swi: R0 | wp: value */
	uint32_t arg2;		/**< swi: cpsr flags | wp: (size << 1) | is_write */
} DebugTraceEvent;

/** Runtime configuration of debug tracing/trapping, set from the GUI */
typedef struct DebugTraceConfig {
	uint8_t trap_undefined;		/**< Halt on undefined instruction */
	uint8_t trap_prefetch_abort;	/**< Halt on prefetch abort */
	uint8_t trap_data_abort;	/**< Halt on data abort */
	uint8_t log_exceptions;		/**< Also emit exception events to the ring */
	uint8_t swi_trace_enabled;	/**< Emit SWI events to the ring */
	uint8_t swi_trace_halt;		/**< Halt on a matching SWI */
	uint8_t reserved0;
	uint8_t reserved1;
	uint32_t swi_filter_min;	/**< Inclusive SWI-number range (0..0xffffffff = all) */
	uint32_t swi_filter_max;
} DebugTraceConfig;

typedef struct {
	int paused;
	int pause_requested;
	DebugPauseReason reason;
	uint32_t halt_pc;
	uint32_t halt_opcode;
	uint32_t last_pc;
	uint32_t last_opcode;
	uint32_t hit_address;
	uint32_t hit_value;
	uint8_t hit_size;
	uint8_t hit_is_write;
	uint8_t step_active;
	uint8_t reserved;
	uint32_t breakpoint_count;
	uint32_t breakpoints[DEBUGGER_MAX_BREAKPOINTS];
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

/** The user's configuration of the emulator */
typedef struct {
	char name[256];		/**< User-defined name for this configuration */
	char hd4_path[512];	/**< Path to the hard disk image file (optional override) */
	char rom_dir[256];	/**< ROM directory name within roms/ folder */
	unsigned mem_size;	/**< Amount of RAM in megabytes */
	unsigned vram_size;	/**< Amount of VRAM in megabytes */
	char *username;
	char *ipaddress;
	char *macaddress;
	char *bridgename;
	int refresh;		/**< Video refresh rate */
	int soundenabled;
	int cdromenabled;
	int cdromtype;
	char isoname[512];
	int mousehackon;
	int mousetwobutton;	/**< Swap the behaviour of the right and middle
	                             buttons, for mice with two buttons */
	NetworkType network_type;
	int cpu_idle;		/**< Attempt to reduce CPU usage */
	int show_fullscreen_message;	/**< Show explanation of how to leave fullscreen, on entering fullscreen */
	int integer_scaling;	/**< Use integer scaling (2x, 3x) for sharp pixels instead of smooth scaling */
	char *network_capture;		///< Path to capture network traffic file, or NULL to disable
	int vnc_enabled;	/**< Enable the built-in VNC server */
	int vnc_port;		/**< Port for the VNC server (default 5900) */
	char vnc_password[64];	/**< Password for VNC authentication (empty = no auth) */
	int hostcmd_enabled;	/**< Enable the HostCmd control socket (host drives the RISC OS CLI) */
	char hostcmd_socket[512];	/**< Socket spec: empty = <datadir>hostcmd.sock (AF_UNIX); a path = AF_UNIX; a bare port = TCP 127.0.0.1:port */
	int debug_enabled;	/**< Enable the DebugCmd control socket (host inspects/controls the emulated CPU) */
	char debug_socket[512];	/**< Socket spec: empty = <datadir>rpcemu-debug.sock (AF_UNIX); a path = AF_UNIX; a bare port = TCP 127.0.0.1:port */
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

/* Activity counters for status bar indicators (implemented in wx host) */
extern void hostfs_activity_increment(void);
extern void network_activity_increment(void);
extern void ide_activity_increment(void);
extern void fdc_activity_increment(void);

/* These functions can optionally be overridden by a platform. If not
   needed to be overridden, there is a generic version in rpc-machdep.c */
extern const char *rpcemu_get_datadir(void);
extern const char *rpcemu_get_resourcedir(void);
extern void rpcemu_set_datadir(const char *path);
extern void rpcemu_set_resourcedir(const char *path);
extern const char *rpcemu_get_machine_datadir(void);
extern void rpcemu_set_machine_datadir(const char *machine_name);
extern const char *rpcemu_get_log_path(void);

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
extern void debugger_clear_breakpoints(void);
extern int debugger_add_breakpoint(uint32_t address);
extern int debugger_remove_breakpoint(uint32_t address);
extern int debugger_has_breakpoint(uint32_t address);
extern void debugger_clear_watchpoints(void);
extern int debugger_add_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write, int log_only);
extern int debugger_remove_watchpoint(uint32_t address, uint32_t size, int on_read, int on_write);
extern int debugger_instruction_hook(uint32_t pc, uint32_t opcode);
extern void debugger_memory_access(uint32_t address, uint32_t size, int is_write, uint32_t value);
extern void debugger_after_instruction(uint32_t pc, uint32_t opcode);

/* Debug tracing: exception trapping, SWI tracing, logging watchpoints */
extern int debugger_swi_trace_active;	/**< Fast gate read from opSWI() */
extern void debugger_set_trace_config(const DebugTraceConfig *cfg);
extern void debugger_get_trace_config(DebugTraceConfig *cfg);
extern uint32_t debugger_trace_pending(void);
extern uint32_t debugger_drain_trace_events(DebugTraceEvent *out, uint32_t max, uint32_t *dropped);
extern void debugger_exception_hook(uint32_t mmode, uint32_t address, uint32_t pc);
extern int debugger_swi_hook(uint32_t swinum, uint32_t opcode);

/* host GUI bridge */
extern void rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize, int yl, int yh, int double_size, int host_xsize, int host_ysize);
extern void rpcemu_move_host_mouse(uint16_t x, uint16_t y);
extern void rpcemu_idle_process_events(void);
extern void rpcemu_send_nat_rule_to_gui(PortForwardRule rule);
extern uint64_t rpcemu_nsec_timer_ticks(void);

extern int drawscre;
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

/* settings.cpp */
extern void config_deep_copy(Config *dest, const Config *src);
extern void config_sync_machine_edit_to_copy(Config *dest, const Config *src);
extern void config_apply_machine_edit(Config *cfg, const char *name, const char *rom_dir,
                                      unsigned mem_size, unsigned vram_size, int refresh,
                                      NetworkType network_type, const char *bridgename,
                                      const char *ipaddress);
extern void config_load(Config *config);
extern void config_load_from_path(Config *config, const char *path);
extern void config_save(Config *config);
extern void config_save_to_path(Config *config, const char *path);
extern void config_set_path(const char *path);
extern const char *config_get_path(void);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
#endif /* _rpc_h */
