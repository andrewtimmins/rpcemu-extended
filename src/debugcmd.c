/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

  DebugCmd - expose the host-side debugger/inspector over a local socket so an
  external tool (e.g. the MCP server) can inspect and control the emulated CPU.

  Both this socket service (debugcmd_poll) and the debugger core it drives run
  on the emulator thread, so the shared state needs no locking. It is serviced
  from execrpcemu()/rpcemu_idle() while running and from MainEmuLoop() while the
  debugger has the CPU paused (so a paused CPU can still be resumed/inspected).

  Wire protocol: newline-delimited. The client sends one request line
  "<verb> [args]\n"; the server replies with exactly one JSON object line. See
  docs/debugcmd.md for the verb reference.

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

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket-compat.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/un.h>
#endif

#include "debugcmd.h"
#include "debugexpr.h"
#include "debugsym.h"
#include "arm.h"
#include "arm_disasm.h"
#include "cp15.h"
#include "hostclipboard.h"
#include "mem.h"
#include "machine_lock.h"
#include "rpcemu.h"
#include "savestate.h"

#ifdef _WIN32
/* Default control-socket port on Windows, where AF_UNIX is unavailable and the
   config default (a filesystem path) cannot be honoured. */
#define DEBUGCMD_DEFAULT_TCP_PORT 15591
#endif

#define DC_IN_BUF_SZ	512		/* one request line */
#define DC_OUT_RING_SZ	(256u * 1024u)	/* MUST be a power of two */
#define DC_RESP_SZ	(64u * 1024u)	/* max size of a single JSON response */
/*
 * How much escaped text one of those responses may carry.
 *
 * Short of the response size by more than any of the wrappers below, because a
 * maximum-length string plus its {"ok":...,"text":"..."} did not fit: snprintf
 * truncated it mid-string and the client was handed JSON with no closing quote.
 * Losing the tail of an over-long message is the intended behaviour; losing the
 * syntax is not.
 */
#define DC_RESP_SZ_TEXT	(DC_RESP_SZ - 128u)
#define DC_MEM_MAX	4096u		/* cap bytes per mem read */
#define DC_DIS_MAX	256u		/* cap instructions per disassemble */
#define SYM_ESC_SZ	160u		/* JSON-escaped symbol name */

typedef struct {
	int	initialised;
	int	listen_fd;		/* -1 = disabled/failed */
	int	client_fd;		/* -1 = no client */
	int	is_tcp;
	char	sock_path[512];

	char	in_buf[DC_IN_BUF_SZ];
	size_t	in_len;
	int	in_overflow;

	uint8_t	out_ring[DC_OUT_RING_SZ];
	size_t	out_head;
	size_t	out_tail;
} DebugCmdState;

static DebugCmdState dc = {
	.listen_fd = -1,
	.client_fd = -1,
};

/* ---- outbound ring --------------------------------------------------- */

static size_t
dc_ring_used(void)
{
	return (dc.out_head - dc.out_tail) & (DC_OUT_RING_SZ - 1);
}

static size_t
dc_ring_free(void)
{
	return (DC_OUT_RING_SZ - 1) - dc_ring_used();
}

/* Queue a NUL-terminated response line (a '\n' is appended). Dropped whole if
   it doesn't fit, so the client never sees a truncated JSON object. */
static void
dc_send(const char *s)
{
	size_t len = strlen(s);

	if (dc.client_fd < 0) {
		return;
	}
	if (dc_ring_free() < len + 1) {
		rpclog("DebugCmd: output ring full, dropping response\n");
		return;
	}
	for (size_t i = 0; i < len; i++) {
		dc.out_ring[dc.out_head] = (uint8_t) s[i];
		dc.out_head = (dc.out_head + 1) & (DC_OUT_RING_SZ - 1);
	}
	dc.out_ring[dc.out_head] = (uint8_t) '\n';
	dc.out_head = (dc.out_head + 1) & (DC_OUT_RING_SZ - 1);
}

/* The precision is not decoration: it bounds the message so the result
   provably fits, which is what lets a caller pass a message it built itself
   (a rejected breakpoint condition, say) without having to size it here. */
#define DC_ERROR_MAX_MSG 400

static void
dc_error(const char *msg)
{
	char buf[DC_ERROR_MAX_MSG + 64];

	snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%.*s\"}",
	    DC_ERROR_MAX_MSG, msg);
	dc_send(buf);
}

/* ---- safe, side-effect-free memory access ---------------------------- */

/* Translate a virtual address to physical without leaving a data-abort event
   pending on the CPU (translateaddress2 sets arm.event & 0x40 on a miss, which
   would otherwise inject a spurious abort into execution). Lives in mem.c
   because breakpoint conditions need the same guarantee. */
#define dc_translate(vaddr, phys) mem_debug_translate((vaddr), (phys))

/* Read one byte at a virtual (default) or physical address; *mapped=0 if the
   virtual page is unmapped. Never triggers watchpoints or aborts. */
static uint8_t
dc_read8(uint32_t addr, int physical, int *mapped)
{
	uint32_t phys = addr;

	if (!physical) {
		if (!dc_translate(addr, &phys)) {
			*mapped = 0;
			return 0;
		}
	}
	*mapped = 1;
	return (uint8_t) mem_phys_read8_debug(phys);
}

static int dc_write8(uint32_t addr, int physical, uint8_t val);

/**
 * reg set <n|pc|cpsr> <hexvalue>
 *
 * Registers were readable and not writable, so a theory about what a register
 * should have held could only be tested by finding the code that sets it.
 *
 * "pc" and "15" are both accepted and they do NOT mean the same thing, for the
 * same reason `regs` reports both: R15 reads eight ahead of the instruction
 * being executed, so the raw register and the address everything else calls
 * the PC are eight apart.
 *
 *   reg set pc 5000    execution resumes at 5000
 *   reg set 15 5000    R15 becomes 5000, so execution resumes at 4ff8
 *
 * "pc" used to write R15 raw, which meant that setting the PC to an address
 * and then reading it back gave an address eight lower, and a machine told to
 * resume at the top of a function started two instructions earlier - on the
 * tail of whatever preceded it. It agreed with nothing else in the protocol:
 * `regs` reports "pc" as (R15 - 8) and "regs[15]" as the raw register, the
 * disassembly and the breakpoints mean the former, and `status.halt_pc` is the
 * former too. It is the same fault as issue #258, where the memory view read
 * physical addresses while every other address in the window was virtual: one
 * name, two meanings, and nothing to say which you had.
 *
 * The bits R15 carries besides the address are kept. In a 26-bit mode it holds
 * the flags and the mode there, and writing a bare address would silently
 * clear them; arm.r15_mask is exactly the address part, so everything outside
 * it survives.
 */
static void
dc_cmd_reg(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	char *a2 = strtok(NULL, " \t");
	char *a3 = strtok(NULL, " \t");
	uint32_t val;
	int n;

	if (!a1 || strcmp(a1, "set") != 0 || !a2 || !a3) {
		dc_error("usage: reg set <0-15|pc|cpsr> <hexvalue>");
		return;
	}

	/* Only while stopped: writing a register underneath a running core races the
	   emulator thread, and the value would be overwritten immediately anyway. */
	if (!debugger_is_paused()) {
		dc_error("the machine must be paused to write a register");
		return;
	}

	val = (uint32_t) strtoul(a3, NULL, 16);

	/* The write itself is debugger_set_register(), shared with the inspector:
	   the PC rule is exactly the sort of thing that goes wrong once it is
	   written down in two places. */
	if (strcmp(a2, "cpsr") == 0) {
		debugger_set_register(DEBUG_REG_CPSR, val);
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"reg\":\"cpsr\",\"value\":\"%08x\"}",
		    (unsigned) val);
		return;
	}
	if (strcmp(a2, "pc") == 0) {
		debugger_set_register(DEBUG_REG_PC, val);
		snprintf(r, DC_RESP_SZ,
		    "{\"ok\":true,\"reg\":\"pc\",\"value\":\"%08x\",\"r15\":\"%08x\"}",
		    (unsigned) (val & arm.r15_mask), (unsigned) arm.reg[15]);
		return;
	}
	n = atoi(a2);
	if (!debugger_set_register(n, val)) {
		dc_error("register must be 0-15, pc or cpsr");
		return;
	}
	snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"reg\":%d,\"value\":\"%08x\"}",
	    n, (unsigned) val);
}

/**
 * trace config [key=value ...]
 *
 * The GUI could already halt on a data abort, an undefined instruction or a SWI;
 * nothing driving the machine over this socket could turn any of it on, which made
 * the most useful stopping conditions unreachable from a script or an agent. With no
 * arguments it reports the current configuration.
 */
static void
dc_cmd_trace_config(char *r, char *args)
{
	DebugTraceConfig cfg;
	char *tok;

	debugger_get_trace_config(&cfg);

	for (tok = strtok(args, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
		char *eq = strchr(tok, '=');
		uint32_t v;

		if (eq == NULL) {
			dc_error("usage: trace config [data_abort=0|1] [prefetch_abort=0|1] "
			         "[undefined=0|1] [log_exceptions=0|1] [swi_log=0|1] "
			         "[swi_halt=0|1] [swi_min=<hex>] [swi_max=<hex>] "
			         "[step_skip_irq=0|1] [step_skip_os=0|1] "
			         "[step_skip_swi=0|1]");
			return;
		}
		*eq = '\0';
		v = (uint32_t) strtoul(eq + 1, NULL, 0);

		if (strcmp(tok, "data_abort") == 0)          { cfg.trap_data_abort = v ? 1 : 0; }
		else if (strcmp(tok, "prefetch_abort") == 0) { cfg.trap_prefetch_abort = v ? 1 : 0; }
		else if (strcmp(tok, "undefined") == 0)      { cfg.trap_undefined = v ? 1 : 0; }
		else if (strcmp(tok, "log_exceptions") == 0) { cfg.log_exceptions = v ? 1 : 0; }
		else if (strcmp(tok, "swi_log") == 0)        { cfg.swi_trace_enabled = v ? 1 : 0; }
		else if (strcmp(tok, "swi_halt") == 0)       { cfg.swi_trace_halt = v ? 1 : 0; }
		else if (strcmp(tok, "swi_min") == 0)        { cfg.swi_filter_min = (uint32_t) strtoul(eq + 1, NULL, 16); }
		else if (strcmp(tok, "swi_max") == 0)        { cfg.swi_filter_max = (uint32_t) strtoul(eq + 1, NULL, 16); }
		else if (strcmp(tok, "step_skip_irq") == 0)  { cfg.step_skip_irq = v ? 1 : 0; }
		else if (strcmp(tok, "step_skip_os") == 0)   { cfg.step_skip_os = v ? 1 : 0; }
		else if (strcmp(tok, "step_skip_swi") == 0)  { cfg.step_skip_swi = v ? 1 : 0; }
		else {
			dc_error("unknown trace config key");
			return;
		}
		debugger_set_trace_config(&cfg);
	}

	debugger_get_trace_config(&cfg);
	snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"data_abort\":%u,\"prefetch_abort\":%u,\"undefined\":%u,"
	    "\"log_exceptions\":%u,\"swi_log\":%u,\"swi_halt\":%u,"
	    "\"swi_min\":\"%08x\",\"swi_max\":\"%08x\","
	    "\"step_skip_irq\":%u,\"step_skip_os\":%u,\"step_skip_swi\":%u}",
	    cfg.trap_data_abort, cfg.trap_prefetch_abort, cfg.trap_undefined,
	    cfg.log_exceptions, cfg.swi_trace_enabled, cfg.swi_trace_halt,
	    (unsigned) cfg.swi_filter_min, (unsigned) cfg.swi_filter_max,
	    cfg.step_skip_irq, cfg.step_skip_os, cfg.step_skip_swi);
}

/**
 * swi load <path> | swi clear
 *
 * The names a module's own SWIs disassemble under. The built-in table is the
 * OS's; anything else is site knowledge, so it comes from a file. Same shape as
 * "sym", deliberately, because it is the same idea applied to SWI numbers.
 */
static void
dc_cmd_swi(char *r, char *args)
{
	char *sub = strtok(args, " \t");

	if (!sub) {
		dc_error("usage: swi load <path> | swi clear");
		return;
	}

	if (strcmp(sub, "clear") == 0) {
		arm_disasm_clear_swi_names();
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"count\":0}");
		return;
	}

	if (strcmp(sub, "load") == 0) {
		char *path = strtok(NULL, "");	/* rest of the line: paths have spaces */
		unsigned count = 0;
		const char *error;

		if (path != NULL) {
			while (*path == ' ' || *path == '\t') {
				path++;
			}
		}
		if (path == NULL || *path == '\0') {
			dc_error("usage: swi load <path>");
			return;
		}

		error = arm_disasm_load_swi_names(path, &count);
		if (error != NULL) {
			char buf[DC_ERROR_MAX_MSG];

			snprintf(buf, sizeof(buf), "cannot load SWI names: %s", error);
			dc_error(buf);
			return;
		}
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"count\":%u}", count);
		return;
	}

	dc_error("usage: swi load <path> | swi clear");
}

/**
 * help
 *
 * The socket had no way to ask what it accepted, so the only reference was the
 * source. Kept to one line per verb deliberately: it is a reminder, not a manual.
 */
static void
dc_cmd_help(char *r)
{
	snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"commands\":["
	    "\"ping\","
	    "\"status\","
	    "\"regs\","
	    "\"fpregs\","
	    "\"bankregs\","
	    "\"reg set <0-15|pc|cpsr> <hex>\","
	    "\"mem <hexaddr> <len> [phys]\","
	    "\"mem write <hexaddr> <hexbytes> [phys]\","
	    "\"dis <hexaddr> [count]\","
	    "\"bp add <hexaddr> [once] [count <n>] [if <expr>]\","
	    "\"bp del|enable|disable <hexaddr> | bp clear\","
	    "\"wp add|del <hexaddr> <size> <r|w|rw> [log] | wp clear\","
	    "\"trace [max]\","
	    "\"trace config [key=value ...]\","
	    "\"pause\",\"resume\",\"continue\",\"reset\","
	    "\"step [count] | step into [count] | step over | step out\","
	    "\"runto <hexaddr>\",\"bt [depth]\","
	    "\"sym load <path> | sym clear | sym lookup <hexaddr> | sym find <name>\","
	    "\"swi load <path> | swi clear\","
	    "\"state save|load <path>\","
	    "\"clipboard get|set [text]\""
	    "]}");
}

/**
 * mem write <hexaddr> <hexbytes> [phys]
 *
 * Bytes are given as hex pairs, so "mem write 8000 e1a00000" writes four bytes in
 * the order written. Reports how many landed rather than failing the whole request
 * on the first refusal, so a run that crosses out of RAM says where it stopped.
 */
static void
dc_mem_write(char *r, const char *a_addr, const char *a_bytes, const char *a_phys)
{
	uint32_t addr;
	size_t len, i;
	uint32_t written = 0;
	int physical;

	if (!a_addr || !a_bytes) {
		dc_error("usage: mem write <hexaddr> <hexbytes> [phys]");
		return;
	}
	len = strlen(a_bytes);
	if (len == 0 || (len & 1) != 0) {
		dc_error("hex bytes must be an even number of hex digits");
		return;
	}
	addr = (uint32_t) strtoul(a_addr, NULL, 16);
	physical = (a_phys && (a_phys[0] == 'p' || a_phys[0] == 'P'));

	for (i = 0; i < len; i += 2) {
		char pair[3];
		char *end = NULL;
		unsigned long b;

		pair[0] = a_bytes[i];
		pair[1] = a_bytes[i + 1];
		pair[2] = '\0';
		b = strtoul(pair, &end, 16);
		if (end != pair + 2) {
			dc_error("hex bytes contain a non-hex digit");
			return;
		}
		if (!dc_write8(addr + (uint32_t) (i / 2), physical, (uint8_t) b)) {
			break;
		}
		written++;
	}

	snprintf(r, DC_RESP_SZ,
	    "{\"ok\":%s,\"addr\":\"%08x\",\"written\":%u,\"requested\":%u%s}",
	    written == len / 2 ? "true" : "false", (unsigned) addr,
	    (unsigned) written, (unsigned) (len / 2),
	    written == len / 2 ? ""
	        : ",\"error\":\"stopped at an address that is unmapped, ROM or I/O\"");
}

/**
 * Write a byte for the debugger, translating unless asked for a physical address.
 *
 * @param addr     Virtual address, or physical if physical is non-zero
 * @param physical Treat addr as already physical
 * @return 1 if written; 0 if unmapped, or if the target refuses writes (ROM, I/O)
 */
static int
dc_write8(uint32_t addr, int physical, uint8_t val)
{
	uint32_t phys = addr;

	if (!physical) {
		if (!dc_translate(addr, &phys)) {
			return 0;
		}
	}
	return mem_phys_write8_debug(phys, val);
}

static uint32_t
dc_read32(uint32_t addr, int physical, int *mapped)
{
	uint8_t b0 = dc_read8(addr, physical, mapped);
	uint8_t b1 = dc_read8(addr + 1, physical, mapped);
	uint8_t b2 = dc_read8(addr + 2, physical, mapped);
	uint8_t b3 = dc_read8(addr + 3, physical, mapped);

	return (uint32_t) b0 | ((uint32_t) b1 << 8) | ((uint32_t) b2 << 16)
	       | ((uint32_t) b3 << 24);
}

/* ---- JSON helpers ---------------------------------------------------- */

/* Append the JSON-escaped form of src to dst (bounded). */
static void
dc_json_str(char *dst, size_t dstsz, const char *src)
{
	size_t n = strlen(dst);

	for (; *src && n + 2 < dstsz; src++) {
		unsigned char c = (unsigned char) *src;

		if (c == '"' || c == '\\') {
			dst[n++] = '\\';
			dst[n++] = (char) c;
		} else if (c < 0x20) {
			n += (size_t) snprintf(dst + n, dstsz - n, "\\u%04x", c);
		} else {
			dst[n++] = (char) c;
		}
	}
	dst[n] = '\0';
}

/* ---- request handlers ------------------------------------------------ */

static void
dc_cmd_regs(char *r)
{
	size_t n;
	int i;

	/* The SPSR, for the five modes that bank one. User and System do not, and
	   reporting whatever the last exception left in the array would be worse
	   than reporting nothing - so it comes back null for those. Asked for in
	   discussion #223: walking an exception return means knowing the mode it is
	   about to restore, and the CPSR does not say. */
	{
		char spsr[24];

		switch (arm.mode & 0xf) {
		case FIQ:
		case IRQ:
		case SUPERVISOR:
		case ABORT:
		case UNDEFINED:
			snprintf(spsr, sizeof(spsr), "\"%08x\"",
			    (unsigned) arm.spsr[arm.mode & 0xf]);
			break;
		default:
			snprintf(spsr, sizeof(spsr), "null");
			break;
		}

	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"pc\":\"%08x\",\"cpsr\":\"%08x\","
	    "\"spsr\":%s,"
	    "\"mode\":%u,\"flags\":\"%c%c%c%c\",\"regs\":[",
	    debugger_is_paused() ? "true" : "false",
	    (unsigned) PC, (unsigned) arm.reg[cpsr], spsr, (unsigned) arm.mode,
	    (arm.reg[cpsr] & NFLAG) ? 'N' : '-',
	    (arm.reg[cpsr] & ZFLAG) ? 'Z' : '-',
	    (arm.reg[cpsr] & CFLAG) ? 'C' : '-',
	    (arm.reg[cpsr] & VFLAG) ? 'V' : '-');
	for (i = 0; i < 16; i++) {
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%s\"%08x\"",
		    i ? "," : "", (unsigned) arm.reg[i]);
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
	}
}

/**
 * bankregs
 *
 * Every mode's banked registers at once, User first.
 *
 * Only the registers that are banked: R0-R7 are shared by every mode and R15
 * is the PC, so repeating them six times would be noise. R8-R12 appear for
 * FIQ, which has its own, and for User, which is where every other mode's copy
 * of them lives.
 *
 * `current` marks the mode the machine is in, and it matters more than it
 * looks: that mode's registers are the live ones and its bank array is stale,
 * so a client that assumes otherwise reads the wrong values for exactly the
 * mode being debugged. debugger_get_banked_registers() sorts that out; this
 * only formats it.
 */
static void
dc_cmd_bankregs(char *r)
{
	DebugBankedRegs banks[DEBUG_BANK_COUNT];
	const uint32_t count = debugger_get_banked_registers(banks, DEBUG_BANK_COUNT);
	size_t n;
	uint32_t i;

	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"banks\":[",
	    debugger_is_paused() ? "true" : "false");

	for (i = 0; i < count; i++) {
		const DebugBankedRegs *b = &banks[i];

		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"mode\":\"%s\",\"mode_value\":\"%02x\",\"current\":%s,"
		    "\"r13\":\"%08x\",\"r14\":\"%08x\",\"spsr\":",
		    i ? "," : "", b->name, (unsigned) b->mode,
		    b->is_current ? "true" : "false",
		    (unsigned) b->r13, (unsigned) b->r14);

		if (b->has_spsr) {
			n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "\"%08x\"",
			    (unsigned) b->spsr);
		} else {
			n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "null");
		}

		if (b->banks_r8_r12) {
			uint32_t c;

			n += (size_t) snprintf(r + n, DC_RESP_SZ - n, ",\"r8_r12\":[");
			for (c = 0; c < 5; c++) {
				n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%s\"%08x\"",
				    c ? "," : "", (unsigned) b->r8_r12[c]);
			}
			n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "]");
		}
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "}");
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

/**
 * An FPA register's value as a JSON string.
 *
 * A string rather than a JSON number because a register can hold an infinity
 * or a NaN, and JSON has no way to write either: emitting a bare `nan` gives
 * every client a parse error on the whole response instead of one odd
 * register. Seventeen significant digits is what a double round-trips in.
 */
static void
dc_fp_value(char *out, size_t sz, double v)
{
	if (isnan(v)) {
		snprintf(out, sz, "nan");
	} else if (isinf(v)) {
		snprintf(out, sz, v < 0.0 ? "-inf" : "inf");
	} else {
		snprintf(out, sz, "%.17g", v);
	}
}

/**
 * fpregs
 *
 * The FPA10's eight registers, its status and its control word.
 *
 * Each register comes back both ways round: `raw` is the three words the chip
 * holds, in the 80-bit extended format, and `value` is the same number written
 * out. The raw words are the authority - see FPADebugReg in rpcemu.h for what
 * a double cannot carry - and they are also what LDFE/STFE and LFM/SFM move,
 * so they can be compared directly against a register spill in memory.
 *
 * `sysid` is the top byte of FPSR, which is how the FPEmulator support code
 * decides what it is talking to: 0x81 is an FPA10, and it is what this
 * emulator reports. It is in the response on its own because "is the machine
 * using the hardware path" is the first question to ask when a floating point
 * value looks wrong, and picking a byte out of FPSR by hand to answer it is a
 * step nobody should have to take.
 *
 * Read-only. There is deliberately no "fpreg set": the support code keeps its
 * own copy of the register file around a trap, so a write here would be
 * overwritten or, worse, would not be.
 */
static void
dc_cmd_fpregs(char *r)
{
	FPADebugState st;
	size_t n;
	int i;

	fpa_get_state(&st);

	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"fpsr\":\"%08x\",\"fpcr\":\"%08x\","
	    "\"sysid\":\"%02x\",\"fpregs\":[",
	    debugger_is_paused() ? "true" : "false",
	    (unsigned) st.fpsr, (unsigned) st.fpcr,
	    (unsigned) (st.fpsr >> 24));

	for (i = 0; i < 8; i++) {
		char value[40];

		dc_fp_value(value, sizeof(value), st.reg[i].value);
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"raw\":[\"%08x\",\"%08x\",\"%08x\"],\"value\":\"%s\"}",
		    i ? "," : "",
		    (unsigned) st.reg[i].w[0], (unsigned) st.reg[i].w[1],
		    (unsigned) st.reg[i].w[2], value);
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

static void
dc_cmd_status(char *r)
{
	DebuggerStatus st;
	size_t n;
	uint32_t i;
	uint32_t pc_offset = 0;
	const char *pc_sym;
	char pc_esc[SYM_ESC_SZ];

	debugger_get_status(&st);

	/* Where the machine stopped, said in the program's own terms */
	pc_sym = debugsym_lookup(st.halt_pc, &pc_offset);
	pc_esc[0] = '\0';
	if (pc_sym != NULL) {
		dc_json_str(pc_esc, sizeof(pc_esc), pc_sym);
	}
	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"pause_requested\":%s,\"reason\":%u,"
	    "\"halt_pc\":\"%08x\",\"halt_opcode\":\"%08x\","
	    "\"halt_from_pc\":\"%08x\",\"last_pc\":\"%08x\","
	    "\"hit_address\":\"%08x\",\"hit_value\":\"%08x\",\"hit_size\":%u,"
	    "\"hit_is_write\":%u,\"step_active\":%u,\"trace_pending\":%u,"
	    "\"pc_symbol\":%s%s%s,\"pc_offset\":%u,"
	    "\"symbols_loaded\":%u,"
	    "\"breakpoints\":[",
	    st.paused ? "true" : "false", st.pause_requested ? "true" : "false",
	    (unsigned) st.reason, (unsigned) st.halt_pc, (unsigned) st.halt_opcode,
	    (unsigned) st.halt_from_pc, (unsigned) st.last_pc, (unsigned) st.hit_address, (unsigned) st.hit_value,
	    (unsigned) st.hit_size, (unsigned) st.hit_is_write,
	    (unsigned) st.step_active, (unsigned) debugger_trace_pending(),
	    pc_sym ? "\"" : "null", pc_sym ? pc_esc : "", pc_sym ? "\"" : "",
	    (unsigned) pc_offset, (unsigned) debugsym_count());
	/* Objects rather than bare addresses: a breakpoint now carries a
	   condition, an ignore count and its hit counts, and those are exactly
	   what is needed to explain a breakpoint that is not firing. */
	for (i = 0; i < st.breakpoint_count; i++) {
		const DebugBreakpointInfo *bp = &st.breakpoints[i];
		char cond[DEBUGGER_MAX_CONDITION * 2 + 4];

		cond[0] = '\0';
		if (bp->has_condition) {
			dc_json_str(cond, sizeof(cond), bp->condition);
		}

		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"address\":\"%08x\",\"enabled\":%s,\"one_shot\":%s,"
		    "\"condition\":%s%s%s,\"ignore_count\":%u,\"hit_count\":%u,"
		    "\"eval_errors\":%u}",
		    i ? "," : "", (unsigned) bp->address,
		    bp->enabled ? "true" : "false",
		    bp->one_shot ? "true" : "false",
		    bp->has_condition ? "\"" : "null",
		    bp->has_condition ? cond : "",
		    bp->has_condition ? "\"" : "",
		    (unsigned) bp->ignore_count, (unsigned) bp->hit_count,
		    (unsigned) bp->eval_errors);
	}
	n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "],\"watchpoints\":[");
	for (i = 0; i < st.watchpoint_count; i++) {
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"address\":\"%08x\",\"size\":%u,\"on_read\":%u,"
		    "\"on_write\":%u,\"log_only\":%u}",
		    i ? "," : "", (unsigned) st.watchpoints[i].address,
		    (unsigned) st.watchpoints[i].size, st.watchpoints[i].on_read,
		    st.watchpoints[i].on_write, st.watchpoints[i].log_only);
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

static void
dc_cmd_mem(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	char *a2 = strtok(NULL, " \t");
	char *a3 = strtok(NULL, " \t");
	uint32_t addr, len, i;
	int physical;
	size_t n;

	/* "mem write <hexaddr> <hexbytes> [phys]" pokes; anything else reads. A
	   debugger that can only look is half a debugger: testing a theory about a
	   word in RAM meant rebuilding or driving the guest into writing it. */
	if (a1 && strcmp(a1, "write") == 0) {
		dc_mem_write(r, a2, a3, strtok(NULL, " \t"));
		return;
	}
	if (!a1 || !a2) {
		dc_error("usage: mem <hexaddr> <len> [phys] | mem write <hexaddr> <hexbytes> [phys]");
		return;
	}
	addr = (uint32_t) strtoul(a1, NULL, 16);
	len = (uint32_t) strtoul(a2, NULL, 0);
	physical = (a3 && (a3[0] == 'p' || a3[0] == 'P'));
	if (len > DC_MEM_MAX) {
		len = DC_MEM_MAX;
	}
	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"addr\":\"%08x\",\"physical\":%s,\"len\":%u,\"data\":\"",
	    (unsigned) addr, physical ? "true" : "false", (unsigned) len);
	for (i = 0; i < len && n + 3 < DC_RESP_SZ; i++) {
		int mapped;
		uint8_t b = dc_read8(addr + i, physical, &mapped);

		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%02x", b);
	}
	snprintf(r + n, DC_RESP_SZ - n, "\"}");
}

static void
dc_cmd_dis(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	char *a2 = strtok(NULL, " \t");
	char *a3 = strtok(NULL, " \t");
	uint32_t addr, count, i;
	int physical;
	size_t n;

	if (!a1) {
		dc_error("usage: dis <hexaddr> [count] [phys]");
		return;
	}
	addr = (uint32_t) strtoul(a1, NULL, 16);
	count = a2 ? (uint32_t) strtoul(a2, NULL, 0) : 16;
	physical = (a3 && (a3[0] == 'p' || a3[0] == 'P'));
	if (count == 0 || count > DC_DIS_MAX) {
		count = (count == 0) ? 1 : DC_DIS_MAX;
	}
	n = (size_t) snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"lines\":[");
	for (i = 0; i < count; i++) {
		uint32_t a = addr + i * 4;
		int mapped;
		uint32_t opcode = dc_read32(a, physical, &mapped);
		char dis[128];
		char line[192];

		if (!mapped) {
			snprintf(line, sizeof(line), "%08x: <unmapped>", (unsigned) a);
		} else {
			const char *sym;
			uint32_t sym_offset = 0;

			/* Branch targets are annotated by the disassembler; the
			   label here is for the instruction's own address, which
			   is what makes a listing navigable rather than a wall
			   of hex. */
			arm_disasm_sym(opcode, a, dis, sizeof(dis),
			    debugsym_count() ? debugsym_disasm_lookup : NULL, NULL);
			sym = debugsym_lookup(a, &sym_offset);

			if (sym != NULL && sym_offset == 0) {
				snprintf(line, sizeof(line), "%08x <%s>: %08x  %s",
				    (unsigned) a, sym, (unsigned) opcode, dis);
			} else {
				snprintf(line, sizeof(line), "%08x: %08x  %s",
				    (unsigned) a, (unsigned) opcode, dis);
			}
		}
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%s\"", i ? "," : "");
		dc_json_str(r, DC_RESP_SZ, line);
		n = strlen(r);
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "\"");
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

#define DC_BP_USAGE \
	"usage: bp add <hexaddr> [once] [count <n>] [if <expr>] | " \
	"bp del|enable|disable <hexaddr> | bp clear"

/**
 * bp add <hexaddr> [once] [count <n>] [if <expr>]
 * bp del|enable|disable <hexaddr>
 * bp clear
 *
 * "if" is last and swallows the rest of the line, so a condition can contain
 * spaces without needing quoting - which the newline-delimited protocol has no
 * way to express anyway.
 *
 * A malformed condition is refused here rather than accepted and found to be
 * unusable later, when the breakpoint is reached and quietly fails to fire.
 */
/**
 * step [n] | step into [n] | step over | step out
 *
 * "into" is the original behaviour and stays the default, so an existing
 * client that says `step 5` is unaffected.
 *
 * Over and out both run the machine rather than stepping it, so neither can
 * report how far it went - they answer immediately and the client polls
 * `status` to learn where it stopped, exactly as it already does for `pause`.
 */
static void
dc_cmd_step(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	char *a2 = strtok(NULL, " \t");
	uint32_t nsteps;

	if (a1 && strcmp(a1, "over") == 0) {
		if (!debugger_step_over()) {
			dc_error("the machine must be paused to step");
			return;
		}
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"mode\":\"over\"}");
		return;
	}

	if (a1 && strcmp(a1, "out") == 0) {
		if (!debugger_step_out()) {
			dc_error("no return address to step out to "
			         "(is the machine paused, and in a called function?)");
			return;
		}
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"mode\":\"out\"}");
		return;
	}

	if (a1 && strcmp(a1, "into") == 0) {
		a1 = a2;
	}

	nsteps = a1 ? (uint32_t) strtoul(a1, NULL, 0) : 1;
	if (nsteps == 0) {
		nsteps = 1;
	}
	debugger_single_step(nsteps);
	snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"stepped\":%u,\"mode\":\"into\"}",
	    (unsigned) nsteps);
}

/**
 * bt [depth]
 *
 * `truncated` is not decoration. The frame chain is a compiler convention and
 * much of RISC OS keeps no frame pointer, so a two-frame answer may be the
 * whole stack or may be the point at which the walk gave up. Those mean
 * entirely different things and the client cannot tell them apart otherwise.
 */
static void
dc_cmd_backtrace(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	DebugFrame frames[DEBUGGER_MAX_FRAMES];
	uint32_t depth = DEBUGGER_MAX_FRAMES;
	uint32_t count, i;
	int truncated = 0;
	size_t n;

	if (a1) {
		depth = (uint32_t) strtoul(a1, NULL, 0);
		if (depth == 0 || depth > DEBUGGER_MAX_FRAMES) {
			depth = DEBUGGER_MAX_FRAMES;
		}
	}

	count = debugger_backtrace(frames, depth, &truncated);

	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"truncated\":%s,\"frames\":[",
	    truncated ? "true" : "false");

	for (i = 0; i < count; i++) {
		uint32_t sym_offset = 0;
		const char *sym = debugsym_lookup(frames[i].pc, &sym_offset);
		char esc[SYM_ESC_SZ];

		esc[0] = '\0';
		if (sym != NULL) {
			dc_json_str(esc, sizeof(esc), sym);
		}

		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"level\":%u,\"pc\":\"%08x\",\"lr\":\"%08x\","
		    "\"sp\":\"%08x\",\"fp\":\"%08x\","
		    "\"symbol\":%s%s%s,\"offset\":%u}",
		    i ? "," : "", (unsigned) i, (unsigned) frames[i].pc,
		    (unsigned) frames[i].lr, (unsigned) frames[i].sp,
		    (unsigned) frames[i].fp,
		    sym ? "\"" : "null", sym ? esc : "", sym ? "\"" : "",
		    (unsigned) sym_offset);
	}

	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

/**
 * sym load <path> | sym clear | sym lookup <hexaddr> | sym find <name>
 *
 * Symbols come from a file the user supplies rather than from the running
 * guest. Reading the module chain out of RISC OS was considered and left out:
 * it depends on kernel workspace layout that differs between versions, and a
 * symbol table that is confidently wrong is worse than none - the whole value
 * of a name beside an address is that it can be trusted.
 */
/**
 * Accept either a hex address or the name of a loaded symbol.
 *
 * A bare hex number wins over a symbol of the same spelling: addresses are
 * what this protocol has always taken, and a symbol table should not be able
 * to change what an existing script means.
 *
 * @return Non-zero if `text` named somewhere
 */
static int
dc_parse_address(const char *text, uint32_t *address)
{
	char *end;
	unsigned long value;

	if (text == NULL || *text == '\0') {
		return 0;
	}

	value = strtoul(text, &end, 16);
	if (*end == '\0') {
		*address = (uint32_t) value;
		return 1;
	}

	return debugsym_resolve(text, address);
}

static void
dc_cmd_sym(char *r, char *args)
{
	char *sub = strtok(args, " \t");

	if (!sub) {
		dc_error("usage: sym load <path> | sym clear | "
		         "sym lookup <hexaddr> | sym find <name>");
		return;
	}

	if (strcmp(sub, "clear") == 0) {
		debugsym_clear();
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"count\":0}");
		return;
	}

	if (strcmp(sub, "load") == 0) {
		/* Rest of the line: a path may contain spaces */
		char *path = strtok(NULL, "");
		uint32_t count = 0;
		const char *error = NULL;

		if (path != NULL) {
			while (*path == ' ' || *path == '\t') {
				path++;
			}
		}
		if (path == NULL || *path == '\0') {
			dc_error("usage: sym load <path>");
			return;
		}
		if (!debugsym_load_file(path, &count, &error)) {
			char buf[DC_ERROR_MAX_MSG];

			snprintf(buf, sizeof(buf), "cannot load symbols: %s",
			    error ? error : "unknown error");
			dc_error(buf);
			return;
		}
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"count\":%u}",
		    (unsigned) count);
		return;
	}

	if (strcmp(sub, "lookup") == 0) {
		char *a1 = strtok(NULL, " \t");
		uint32_t address, offset = 0;
		const char *name;
		char esc[SYM_ESC_SZ];

		if (!a1) {
			dc_error("usage: sym lookup <hexaddr>");
			return;
		}
		address = (uint32_t) strtoul(a1, NULL, 16);
		name = debugsym_lookup(address, &offset);

		if (name == NULL) {
			snprintf(r, DC_RESP_SZ,
			    "{\"ok\":true,\"address\":\"%08x\",\"symbol\":null}",
			    (unsigned) address);
			return;
		}
		esc[0] = '\0';
		dc_json_str(esc, sizeof(esc), name);
		snprintf(r, DC_RESP_SZ,
		    "{\"ok\":true,\"address\":\"%08x\",\"symbol\":\"%s\","
		    "\"offset\":%u}",
		    (unsigned) address, esc, (unsigned) offset);
		return;
	}

	if (strcmp(sub, "find") == 0) {
		char *name = strtok(NULL, "");
		uint32_t address = 0;
		char esc[SYM_ESC_SZ];

		if (name != NULL) {
			while (*name == ' ' || *name == '\t') {
				name++;
			}
		}
		if (name == NULL || *name == '\0') {
			dc_error("usage: sym find <name>");
			return;
		}
		if (!debugsym_resolve(name, &address)) {
			dc_error("no symbol of that name");
			return;
		}
		esc[0] = '\0';
		dc_json_str(esc, sizeof(esc), name);
		snprintf(r, DC_RESP_SZ,
		    "{\"ok\":true,\"symbol\":\"%s\",\"address\":\"%08x\"}",
		    esc, (unsigned) address);
		return;
	}

	dc_error("usage: sym load <path> | sym clear | "
	         "sym lookup <hexaddr> | sym find <name>");
}

static void
dc_cmd_bp(char *r, char *args)
{
	char *sub = strtok(args, " \t");
	char *a1 = strtok(NULL, " \t");
	uint32_t addr;

	if (!sub) {
		dc_error(DC_BP_USAGE);
		return;
	}
	if (strcmp(sub, "clear") == 0) {
		debugger_clear_breakpoints();
		snprintf(r, DC_RESP_SZ, "{\"ok\":true}");
		return;
	}
	if (!a1) {
		dc_error(DC_BP_USAGE);
		return;
	}
	/* A symbol name is accepted wherever an address is. Setting a breakpoint
	   on "main" beats looking main up and typing its address back in, which
	   is the whole reason for loading symbols in the first place. */
	if (!dc_parse_address(a1, &addr)) {
		dc_error("not an address, and no symbol of that name");
		return;
	}

	if (strcmp(sub, "add") == 0) {
		const char *condition = NULL;
		uint32_t ignore_count = 0;
		int one_shot = 0;
		int seen_if = 0;
		const char *error = NULL;
		char *tok;
		int ok;

		while ((tok = strtok(NULL, " \t")) != NULL) {
			if (strcmp(tok, "once") == 0) {
				one_shot = 1;
			} else if (strcmp(tok, "count") == 0) {
				char *value = strtok(NULL, " \t");

				if (!value) {
					dc_error(DC_BP_USAGE);
					return;
				}
				ignore_count = (uint32_t) strtoul(value, NULL, 0);
			} else if (strcmp(tok, "if") == 0) {
				/* Rest of the line, spaces and all. Note this is
				   NULL when "if" ends the line, which is why the
				   flag is tracked separately: without it, a bare
				   trailing "if" would fall through and set an
				   unconditional breakpoint while reporting
				   success - the exact silent misunderstanding
				   conditions are supposed to prevent. */
				condition = strtok(NULL, "");
				seen_if = 1;
				break;
			} else {
				dc_error(DC_BP_USAGE);
				return;
			}
		}

		if (seen_if) {
			while (condition != NULL &&
			       (*condition == ' ' || *condition == '\t')) {
				condition++;
			}
			if (condition == NULL || *condition == '\0') {
				dc_error("bp add: 'if' needs an expression");
				return;
			}
			if (!debugexpr_check(condition, &error)) {
				char buf[256];
				char esc[192];

				esc[0] = '\0';
				dc_json_str(esc, sizeof(esc), condition);
				snprintf(buf, sizeof(buf),
				    "bad condition \\\"%s\\\": %s", esc,
				    error ? error : "cannot be parsed");
				dc_error(buf);
				return;
			}
		}

		ok = debugger_add_breakpoint_ex(addr, condition, ignore_count, one_shot);
		snprintf(r, DC_RESP_SZ, "{\"ok\":%s,\"address\":\"%08x\"%s}",
		    ok ? "true" : "false", (unsigned) addr,
		    ok ? "" : ",\"error\":\"breakpoint table full\"");
	} else if (strcmp(sub, "del") == 0) {
		debugger_remove_breakpoint(addr);
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"address\":\"%08x\"}", (unsigned) addr);
	} else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
		int enable = (sub[0] == 'e');

		if (!debugger_set_breakpoint_enabled(addr, enable)) {
			dc_error("no breakpoint at that address");
			return;
		}
		snprintf(r, DC_RESP_SZ,
		    "{\"ok\":true,\"address\":\"%08x\",\"enabled\":%s}",
		    (unsigned) addr, enable ? "true" : "false");
	} else {
		dc_error(DC_BP_USAGE);
	}
}

static void
dc_cmd_wp(char *r, char *args)
{
	char *sub = strtok(args, " \t");
	char *a1 = strtok(NULL, " \t");
	char *a2 = strtok(NULL, " \t");
	char *a3 = strtok(NULL, " \t");
	char *a4 = strtok(NULL, " \t");
	uint32_t addr, size;
	int on_read, on_write, log_only;

	if (!sub) {
		dc_error("usage: wp add|del|clear [hexaddr size r|w|rw [log]]");
		return;
	}
	if (strcmp(sub, "clear") == 0) {
		debugger_clear_watchpoints();
		snprintf(r, DC_RESP_SZ, "{\"ok\":true}");
		return;
	}
	if (!a1 || !a2 || !a3) {
		dc_error("usage: wp add|del <hexaddr> <size> <r|w|rw> [log]");
		return;
	}
	addr = (uint32_t) strtoul(a1, NULL, 16);
	size = (uint32_t) strtoul(a2, NULL, 0);
	on_read = (strchr(a3, 'r') != NULL || strchr(a3, 'R') != NULL);
	on_write = (strchr(a3, 'w') != NULL || strchr(a3, 'W') != NULL);
	log_only = (a4 && a4[0] == 'l');
	if (strcmp(sub, "add") == 0) {
		int ok = debugger_add_watchpoint(addr, size, on_read, on_write, log_only);

		snprintf(r, DC_RESP_SZ, "{\"ok\":%s%s}", ok ? "true" : "false",
		    ok ? "" : ",\"error\":\"watchpoint table full or invalid\"");
	} else if (strcmp(sub, "del") == 0) {
		debugger_remove_watchpoint(addr, size, on_read, on_write);
		snprintf(r, DC_RESP_SZ, "{\"ok\":true}");
	} else {
		dc_error("usage: wp add|del|clear ...");
	}
}

/**
 * state save <path> / state load <path>
 *
 * Snapshots over the debug socket, which is what lets something driving the
 * machine from outside - an agent, a test - put it back exactly as it was
 * rather than starting again. state_save() and state_load() have to run on the
 * emulator thread between instructions, and this socket is serviced on that
 * thread, so they can simply be called.
 */
static void
dc_cmd_state(char *r, char *args)
{
	char *op = strtok(args, " \t");
	char *path = strtok(NULL, "");

	if (op == NULL || path == NULL || path[0] == '\0') {
		dc_error("usage: state save|load <path>");
		return;
	}

	if (strcmp(op, "save") == 0) {
		if (state_save(path) == 0) {
			snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"saved\":true}");
		} else {
			dc_error("save failed");
		}
	} else if (strcmp(op, "load") == 0) {
		char err[256];

		/* Checked first so a mismatch is reported as itself. state_load()
		   resets the machine on any failure, which would otherwise leave a
		   caller wondering why its machine had rebooted. */
		if (state_check(path, err, sizeof(err)) != 0) {
			char esc[DC_RESP_SZ_TEXT];

			esc[0] = '\0';
			dc_json_str(esc, sizeof(esc), err);
			snprintf(r, DC_RESP_SZ, "{\"ok\":false,\"error\":\"%s\"}", esc);
			return;
		}
		if (state_load(path) == 0) {
			snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"loaded\":true}");
		} else {
			dc_error("load failed");
		}
	} else {
		dc_error("usage: state save|load <path>");
	}
}

/**
 * clipboard get / clipboard set <text>
 *
 * The shared clipboard as a data channel, which is a different thing from
 * typing at the emulated keyboard: this moves a whole string in one go and
 * needs no host clipboard at all, so it works on a headless machine. Typing
 * remains the way to drive something that is watching for keys.
 */
static void
dc_cmd_clipboard(char *r, char *args)
{
	char *op = strtok(args, " \t");
	char *text;

	if (op == NULL) {
		dc_error("usage: clipboard get|set <text>");
		return;
	}

	if (strcmp(op, "get") == 0) {
		char buf[DC_RESP_SZ / 2];
		const int type = clipboard_get_type();
		const int len = clipboard_get_text(buf, sizeof(buf));

		if (type == 0) {
			snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"type\":0,\"text\":null}");
		} else if (len < 0) {
			/* An image, or more text than fits: say what is there rather
			   than pretending the clipboard is empty. */
			snprintf(r, DC_RESP_SZ,
			    "{\"ok\":true,\"type\":%d,\"text\":null}", type);
		} else {
			char esc[DC_RESP_SZ_TEXT];

			esc[0] = '\0';
			dc_json_str(esc, sizeof(esc), buf);
			snprintf(r, DC_RESP_SZ,
			    "{\"ok\":true,\"type\":%d,\"text\":\"%s\"}", type, esc);
		}
		return;
	}

	if (strcmp(op, "set") == 0) {
		text = strtok(NULL, "");
		if (text == NULL) {
			text = (char *) "";
		}
		clipboard_host_changed(CLIPBOARD_TYPE_TEXT, text,
		                       (unsigned int) strlen(text));
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"set\":%u}",
		         (unsigned) strlen(text));
		return;
	}

	dc_error("usage: clipboard get|set <text>");
}

static void
dc_cmd_trace(char *r, char *args)
{
	char *a1 = strtok(args, " \t");
	uint32_t max = a1 ? (uint32_t) strtoul(a1, NULL, 0) : 128;
	DebugTraceEvent ev[128];
	uint32_t dropped = 0, got, i;
	size_t n;

	if (max == 0 || max > 128) {
		max = 128;
	}
	got = debugger_drain_trace_events(ev, max, &dropped);
	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"dropped\":%u,\"events\":[", (unsigned) dropped);
	for (i = 0; i < got; i++) {
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n,
		    "%s{\"seq\":%u,\"type\":%u,\"pc\":\"%08x\",\"opcode\":\"%08x\","
		    "\"arg0\":\"%08x\",\"arg1\":\"%08x\",\"arg2\":\"%08x\"}",
		    i ? "," : "", (unsigned) ev[i].seq, (unsigned) ev[i].type,
		    (unsigned) ev[i].pc, (unsigned) ev[i].opcode, (unsigned) ev[i].arg0,
		    (unsigned) ev[i].arg1, (unsigned) ev[i].arg2);
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

static void
dc_dispatch(char *line)
{
	static char resp[DC_RESP_SZ];
	char *verb;
	char *args;

	/* trim trailing CR/whitespace */
	{
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' '
		    || line[l - 1] == '\t')) {
			line[--l] = '\0';
		}
	}
	verb = strtok(line, " \t");
	args = strtok(NULL, "");	/* rest of line */
	if (!verb) {
		return;			/* blank line: ignore */
	}
	resp[0] = '\0';

	if (strcmp(verb, "ping") == 0) {
		snprintf(resp, DC_RESP_SZ,
		    "{\"ok\":true,\"paused\":%s,\"model\":\"%s\",\"dynarec\":%s}",
		    debugger_is_paused() ? "true" : "false",
		    models[machine.model].name_config, arm_is_dynarec() ? "true" : "false");
	} else if (strcmp(verb, "regs") == 0) {
		dc_cmd_regs(resp);
	} else if (strcmp(verb, "fpregs") == 0) {
		dc_cmd_fpregs(resp);
	} else if (strcmp(verb, "bankregs") == 0) {
		dc_cmd_bankregs(resp);
	} else if (strcmp(verb, "status") == 0) {
		dc_cmd_status(resp);
	} else if (strcmp(verb, "mem") == 0) {
		dc_cmd_mem(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "dis") == 0) {
		dc_cmd_dis(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "bp") == 0) {
		dc_cmd_bp(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "wp") == 0) {
		dc_cmd_wp(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "reg") == 0) {
		dc_cmd_reg(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "help") == 0) {
		dc_cmd_help(resp);
	} else if (strcmp(verb, "trace") == 0 && args != NULL &&
	           strncmp(args, "config", 6) == 0) {
		/* Checked before plain "trace", which would otherwise read "config" as a
		   count and drain the ring instead of configuring it. */
		dc_cmd_trace_config(resp, args + 6);
	} else if (strcmp(verb, "trace") == 0) {
		dc_cmd_trace(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "pause") == 0) {
		debugger_request_pause(DebugPauseReason_User);
		snprintf(resp, DC_RESP_SZ, "{\"ok\":true,\"paused\":%s}",
		    debugger_is_paused() ? "true" : "false");
	} else if (strcmp(verb, "resume") == 0 || strcmp(verb, "continue") == 0) {
		debugger_resume();
		snprintf(resp, DC_RESP_SZ, "{\"ok\":true,\"paused\":false}");
	} else if (strcmp(verb, "step") == 0) {
		dc_cmd_step(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "runto") == 0) {
		char *a1 = args ? strtok(args, " \t") : NULL;
		uint32_t runto_addr = 0;

		if (!a1) {
			dc_error("usage: runto <hexaddr>");
			return;
		}
		if (!dc_parse_address(a1, &runto_addr)) {
			dc_error("not an address, and no symbol of that name");
			return;
		}
		if (!debugger_run_to(runto_addr)) {
			dc_error("the machine must be paused to run to an address");
			return;
		}
		snprintf(resp, DC_RESP_SZ, "{\"ok\":true}");
	} else if (strcmp(verb, "swi") == 0) {
		dc_cmd_swi(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "sym") == 0) {
		dc_cmd_sym(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "bt") == 0 || strcmp(verb, "backtrace") == 0) {
		dc_cmd_backtrace(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "reset") == 0) {
		/* A wedged guest is otherwise the end of an unattended session: the
		   command channel into RISC OS can be left mid-command by a client
		   that goes away, and nothing short of a reset gets it back. */
		resetrpc();
		snprintf(resp, DC_RESP_SZ, "{\"ok\":true,\"reset\":true}");
	} else if (strcmp(verb, "state") == 0) {
		dc_cmd_state(resp, args ? args : (char *) "");
	} else if (strcmp(verb, "clipboard") == 0) {
		dc_cmd_clipboard(resp, args ? args : (char *) "");
	} else {
		dc_error("unknown verb");
	}

	if (resp[0] != '\0') {
		dc_send(resp);
	}
}

/* ---- socket lifecycle (mirrors hostcmd.c) ---------------------------- */

static void
dc_set_nonblock(int fd)
{
	socket_set_nonblocking(fd);
}

#ifndef _WIN32
/*
 * Does the directory that would hold @path exist?
 *
 * bind() on a Unix socket needs its parent directory to be there, and answers a
 * missing one with ENOENT - which reads as "the socket is missing" rather than
 * "the folder is". Checked separately so the caller can say which.
 */
static int
dc_parent_dir_exists(const char *path)
{
	char dir[512];
	char *slash;
	struct stat st;

	if (snprintf(dir, sizeof(dir), "%s", path) < 0 ||
	    strlen(path) >= sizeof(dir)) {
		return 0;
	}

	slash = strrchr(dir, '/');
	if (slash == NULL) {
		return 1;	/* A bare name, so the current directory. */
	}
	if (slash == dir) {
		return 1;	/* Directly in the root. */
	}
	*slash = '\0';

	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
dc_listen_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		rpclog("DebugCmd: socket path too long: %s\n", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("DebugCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* The length was checked above, so this fits with room for the
	   terminator the memset already put there. memcpy rather than strncpy
	   because the bound strncpy is given is the destination's size, which
	   GCC cannot relate to the check and so warns about truncating a path
	   that has already been refused. */
	memcpy(addr.sun_path, path, strlen(path));
	unlink(path);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("DebugCmd: bind(%s) failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("DebugCmd: listen() failed: %s\n", strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	dc_set_nonblock(fd);
	strncpy(dc.sock_path, path, sizeof(dc.sock_path) - 1);
	dc.sock_path[sizeof(dc.sock_path) - 1] = '\0';
	dc.is_tcp = 0;
	rpclog("DebugCmd: listening on AF_UNIX %s\n", path);
	return fd;
}
#endif /* !_WIN32 */

static int
dc_listen_tcp(int port)
{
	struct sockaddr_in addr;
	int fd;
	int on = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("DebugCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t) port);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("DebugCmd: bind(127.0.0.1:%d) failed: %s\n", port, strerror(errno));
		closesocket(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("DebugCmd: listen() failed: %s\n", strerror(errno));
		closesocket(fd);
		return -1;
	}
	dc_set_nonblock(fd);
	dc.is_tcp = 1;
	dc.sock_path[0] = '\0';
	rpclog("DebugCmd: listening on TCP 127.0.0.1:%d\n", port);
	return fd;
}

#ifdef _WIN32
/*
 * Bind the first free port at or above `first`.
 *
 * Every machine's configuration carries the same default port, and Windows lets
 * a second listener bind a port that already has one when SO_REUSEADDR is set,
 * so two machines did not conflict loudly: both bound it and a connection went
 * to whichever the system chose. The port settled on goes into the lock file,
 * which is how the client tools find it. Same reasoning as the VNC server's.
 */
static int
dc_listen_tcp_from(int first)
{
	const int attempts = 16;
	int i;

	for (i = 0; i < attempts; i++) {
		const int port = first + i;
		int fd;

		if (port < 1 || port > 65535) {
			break;
		}
		fd = dc_listen_tcp(port);
		if (fd >= 0) {
			if (i > 0) {
				rpclog("DebugCmd: TCP port %d was in use, listening on %d "
				       "instead\n", first, port);
			}
			return fd;
		}
	}
	return -1;
}
#endif /* _WIN32 - only the Windows path picks a port for itself */

/* Record the port actually bound, so a tool does not have to guess it. */
static void
dc_record_tcp_endpoint(int fd)
{
	char endpoint[64];
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	if (fd < 0) {
		return;
	}
	if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
		return;
	}
	snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u",
	    (unsigned) ntohs(addr.sin_port));
	machine_lock_set_debug_endpoint(endpoint);
}

void
debugcmd_init(void)
{
	if (dc.initialised) {
		debugcmd_close();
	}
	memset(&dc, 0, sizeof(dc));
	dc.listen_fd = -1;
	dc.client_fd = -1;
	dc.initialised = 1;

	if (!config.debug_enabled) {
		return;
	}
#ifdef _WIN32
	/* Windows: TCP loopback only (no useful AF_UNIX). A bare integer in
	   debug_socket selects the port; anything else uses the default port. */
	{
		int port = DEBUGCMD_DEFAULT_TCP_PORT;

		if (config.debug_socket[0] != '\0'
		    && config.debug_socket[0] != '/')
		{
			int p = atoi(config.debug_socket);

			if (p > 0 && p < 65536) {
				port = p;
			}
		}
		dc.listen_fd = dc_listen_tcp_from(port);
		dc_record_tcp_endpoint(dc.listen_fd);
	}
#else
	if (config.debug_socket[0] == '\0' || config.debug_socket[0] == '/') {
		char path[512];

		if (config.debug_socket[0] == '/') {
			strncpy(path, config.debug_socket, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';

			/*
			 * ★ Fall back to the default when the directory it names has
			 * gone, rather than failing to bind and losing the debug
			 * socket for the run.
			 *
			 * This heals configurations that already carry a stale
			 * absolute path. Until now, one run with --debug-socket wrote
			 * that path into the machine's configuration file, so moving
			 * a data folder left it pointing at a directory that no
			 * longer existed and every later run reported
			 * "bind(...) failed: No such file or directory". The write is
			 * fixed in settings.cpp, but existing files still hold the
			 * bad value, and rewriting somebody's configuration on a
			 * guess about what they meant is worse than quietly working.
			 */
			if (!dc_parent_dir_exists(path)) {
				rpclog("DebugCmd: '%s' is not there any more, using the "
				       "default socket instead\n", path);
				snprintf(path, sizeof(path), "%srpcemu-debug.sock",
				    rpcemu_get_machine_datadir());
			}
		} else {
			/* The machine's own directory - see the same change in
			   hostcmd.c. A debugger socket shared between machines
			   attaches to whichever bound it last. */
			snprintf(path, sizeof(path), "%srpcemu-debug.sock",
			    rpcemu_get_machine_datadir());
		}
		dc.listen_fd = dc_listen_unix(path);
		if (dc.listen_fd >= 0) {
			/* Say where, so rpcemu-debug can find a machine whose
			   configuration named a path of its own. */
			machine_lock_set_debug_endpoint(path);
		}
	} else {
		int port = atoi(config.debug_socket);

		if (port > 0 && port < 65536) {
			dc.listen_fd = dc_listen_tcp(port);
			dc_record_tcp_endpoint(dc.listen_fd);
		} else {
			rpclog("DebugCmd: invalid socket spec '%s', disabling\n",
			    config.debug_socket);
		}
	}
#endif
}

static void
dc_drop_client(void)
{
	if (dc.client_fd >= 0) {
		closesocket(dc.client_fd);
		dc.client_fd = -1;
	}
	dc.in_len = 0;
	dc.in_overflow = 0;
	dc.out_head = dc.out_tail = 0;
}

void
debugcmd_reset(void)
{
	if (!dc.initialised) {
		return;
	}
	dc.in_len = 0;
	dc.in_overflow = 0;
}

void
debugcmd_close(void)
{
	if (dc.client_fd >= 0) {
		closesocket(dc.client_fd);
		dc.client_fd = -1;
	}
	if (dc.listen_fd >= 0) {
		closesocket(dc.listen_fd);
		dc.listen_fd = -1;
	}
#ifndef _WIN32
	if (!dc.is_tcp && dc.sock_path[0] != '\0') {
		unlink(dc.sock_path);
		dc.sock_path[0] = '\0';
	}
#endif
	dc.initialised = 0;
}

/* ---- per-tick service ------------------------------------------------ */

static void
dc_read_client(void)
{
	uint8_t tmp[1024];
	ssize_t nread;
	ssize_t i;

	nread = recv(dc.client_fd, (char *) tmp, sizeof(tmp), 0);
	if (nread == 0) {
		dc_drop_client();
		return;
	}
	if (nread < 0) {
		if (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN) {
			return;
		}
		dc_drop_client();
		return;
	}
	for (i = 0; i < nread; i++) {
		uint8_t b = tmp[i];

		if (b == '\n') {
			if (!dc.in_overflow) {
				dc.in_buf[dc.in_len] = '\0';
				dc_dispatch(dc.in_buf);
			} else {
				dc_error("request line too long");
			}
			dc.in_len = 0;
			dc.in_overflow = 0;
			continue;
		}
		if (dc.in_len < DC_IN_BUF_SZ - 1) {
			dc.in_buf[dc.in_len++] = (char) b;
		} else {
			dc.in_overflow = 1;
			dc.in_len = 0;
		}
	}
}

static void
dc_write_client(void)
{
	while (dc_ring_used() > 0) {
		size_t used = dc_ring_used();
		size_t to_end = DC_OUT_RING_SZ - dc.out_tail;
		size_t contig = (used < to_end) ? used : to_end;
		ssize_t nwr = send(dc.client_fd, (const char *) &dc.out_ring[dc.out_tail], contig,
		    MSG_NOSIGNAL);

		if (nwr < 0) {
			if (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN) {
				return;
			}
			dc_drop_client();
			return;
		}
		if (nwr == 0) {
			return;
		}
		dc.out_tail = (dc.out_tail + (size_t) nwr) & (DC_OUT_RING_SZ - 1);
	}
}

void
debugcmd_poll(void)
{
	struct pollfd pfd;

	if (dc.listen_fd < 0) {
		return;
	}
	if (dc.client_fd < 0) {
		/*
		 * Nothing connected, which is the ordinary case. This function is called
		 * every 20000 emulated cycles - tens of thousands of times a second -
		 * and asking the kernel whether anybody has connected costs a syscall
		 * every time. That was most of the cost of a feature nobody was using.
		 *
		 * Only this branch is throttled. A client waiting to connect can wait a
		 * few milliseconds; data on an open connection cannot, so the path below
		 * is unchanged. Sixty-four calls is about 3ms of guest time at full
		 * speed, and about 75ms while the machine idles under "Reduce CPU
		 * usage", where this is called from the idle loop instead.
		 */
		static unsigned since_listen_check;

		if ((++since_listen_check & 0x3fu) != 0) {
			return;
		}

		pfd.fd = dc.listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
			int c = accept(dc.listen_fd, NULL, NULL);

			if (c >= 0) {
				dc_set_nonblock(c);
				dc.client_fd = c;
				dc.in_len = 0;
				dc.in_overflow = 0;
				dc.out_head = dc.out_tail = 0;
			}
		}
		return;
	}
	pfd.fd = dc.client_fd;
	pfd.events = POLLIN;
	if (dc_ring_used() > 0) {
		pfd.events |= POLLOUT;
	}
	pfd.revents = 0;
	if (poll(&pfd, 1, 0) <= 0) {
		return;
	}
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		dc_drop_client();
		return;
	}
	if (pfd.revents & POLLIN) {
		dc_read_client();
		if (dc.client_fd < 0) {
			return;
		}
	}
	if (pfd.revents & POLLOUT) {
		dc_write_client();
	}
}
