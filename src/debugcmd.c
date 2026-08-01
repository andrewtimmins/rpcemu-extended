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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket-compat.h"
#ifndef _WIN32
#include <sys/un.h>
#endif

#include "debugcmd.h"
#include "arm.h"
#include "arm_disasm.h"
#include "cp15.h"
#include "hostclipboard.h"
#include "mem.h"
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
#define DC_MEM_MAX	4096u		/* cap bytes per mem read */
#define DC_DIS_MAX	256u		/* cap instructions per disassemble */

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

static void
dc_error(const char *msg)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
	dc_send(buf);
}

/* ---- safe, side-effect-free memory access ---------------------------- */

/* Translate a virtual address to physical without leaving a data-abort event
   pending on the CPU (readmemfl/translateaddress set arm.event & 0x40 on a
   miss, which would otherwise inject a spurious abort into execution). Returns
   1 and *phys on success, 0 if unmapped. */
static int
dc_translate(uint32_t vaddr, uint32_t *phys)
{
	if (!mmu) {
		*phys = vaddr;
		return 1;
	}
	{
		uint32_t saved_event = arm.event;
		uint32_t p = translateaddress2(vaddr, 0, 0);
		int fault = (arm.event & 0x40) != 0;

		arm.event = saved_event;	/* undo any injected abort */
		if (fault) {
			return 0;
		}
		*phys = p;
		return 1;
	}
}

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
 * should have held could only be tested by finding the code that sets it. R15 is
 * accepted as "pc" as well as "15" because that is what people type.
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

	if (strcmp(a2, "cpsr") == 0) {
		arm.reg[16] = val;
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"reg\":\"cpsr\",\"value\":\"%08x\"}",
		    (unsigned) val);
		return;
	}
	n = (strcmp(a2, "pc") == 0) ? 15 : atoi(a2);
	if (n < 0 || n > 15) {
		dc_error("register must be 0-15, pc or cpsr");
		return;
	}
	arm.reg[n] = val;
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
			         "[swi_halt=0|1] [swi_min=<hex>] [swi_max=<hex>]");
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
	    "\"swi_min\":\"%08x\",\"swi_max\":\"%08x\"}",
	    cfg.trap_data_abort, cfg.trap_prefetch_abort, cfg.trap_undefined,
	    cfg.log_exceptions, cfg.swi_trace_enabled, cfg.swi_trace_halt,
	    (unsigned) cfg.swi_filter_min, (unsigned) cfg.swi_filter_max);
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
	    "\"reg set <0-15|pc|cpsr> <hex>\","
	    "\"mem <hexaddr> <len> [phys]\","
	    "\"mem write <hexaddr> <hexbytes> [phys]\","
	    "\"dis <hexaddr> [count]\","
	    "\"bp add|del <hexaddr> | bp clear\","
	    "\"wp add|del <hexaddr> <size> <r|w|rw> [log] | wp clear\","
	    "\"trace [max]\","
	    "\"trace config [key=value ...]\","
	    "\"pause\",\"resume\",\"continue\",\"step [count]\",\"reset\","
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

	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"pc\":\"%08x\",\"cpsr\":\"%08x\","
	    "\"mode\":%u,\"flags\":\"%c%c%c%c\",\"regs\":[",
	    debugger_is_paused() ? "true" : "false",
	    (unsigned) PC, (unsigned) arm.reg[cpsr], (unsigned) arm.mode,
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

static void
dc_cmd_status(char *r)
{
	DebuggerStatus st;
	size_t n;
	uint32_t i;

	debugger_get_status(&st);
	n = (size_t) snprintf(r, DC_RESP_SZ,
	    "{\"ok\":true,\"paused\":%s,\"pause_requested\":%s,\"reason\":%u,"
	    "\"halt_pc\":\"%08x\",\"halt_opcode\":\"%08x\",\"last_pc\":\"%08x\","
	    "\"hit_address\":\"%08x\",\"hit_value\":\"%08x\",\"hit_size\":%u,"
	    "\"hit_is_write\":%u,\"step_active\":%u,\"trace_pending\":%u,"
	    "\"breakpoints\":[",
	    st.paused ? "true" : "false", st.pause_requested ? "true" : "false",
	    (unsigned) st.reason, (unsigned) st.halt_pc, (unsigned) st.halt_opcode,
	    (unsigned) st.last_pc, (unsigned) st.hit_address, (unsigned) st.hit_value,
	    (unsigned) st.hit_size, (unsigned) st.hit_is_write,
	    (unsigned) st.step_active, (unsigned) debugger_trace_pending());
	for (i = 0; i < st.breakpoint_count; i++) {
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%s\"%08x\"",
		    i ? "," : "", (unsigned) st.breakpoints[i]);
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
			arm_disasm(opcode, a, dis, sizeof(dis));
			snprintf(line, sizeof(line), "%08x: %08x  %s",
			    (unsigned) a, (unsigned) opcode, dis);
		}
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "%s\"", i ? "," : "");
		dc_json_str(r, DC_RESP_SZ, line);
		n = strlen(r);
		n += (size_t) snprintf(r + n, DC_RESP_SZ - n, "\"");
	}
	snprintf(r + n, DC_RESP_SZ - n, "]}");
}

static void
dc_cmd_bp(char *r, char *args)
{
	char *sub = strtok(args, " \t");
	char *a1 = strtok(NULL, " \t");
	uint32_t addr;

	if (!sub) {
		dc_error("usage: bp add|del|clear [hexaddr]");
		return;
	}
	if (strcmp(sub, "clear") == 0) {
		debugger_clear_breakpoints();
		snprintf(r, DC_RESP_SZ, "{\"ok\":true}");
		return;
	}
	if (!a1) {
		dc_error("usage: bp add|del <hexaddr>");
		return;
	}
	addr = (uint32_t) strtoul(a1, NULL, 16);
	if (strcmp(sub, "add") == 0) {
		int ok = debugger_add_breakpoint(addr);

		snprintf(r, DC_RESP_SZ, "{\"ok\":%s,\"address\":\"%08x\"%s}",
		    ok ? "true" : "false", (unsigned) addr,
		    ok ? "" : ",\"error\":\"breakpoint table full\"");
	} else if (strcmp(sub, "del") == 0) {
		debugger_remove_breakpoint(addr);
		snprintf(r, DC_RESP_SZ, "{\"ok\":true,\"address\":\"%08x\"}", (unsigned) addr);
	} else {
		dc_error("usage: bp add|del|clear [hexaddr]");
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
			char esc[DC_RESP_SZ];

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
			char esc[DC_RESP_SZ];

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
		char *a1 = args ? strtok(args, " \t") : NULL;
		uint32_t nsteps = a1 ? (uint32_t) strtoul(a1, NULL, 0) : 1;

		if (nsteps == 0) {
			nsteps = 1;
		}
		debugger_single_step(nsteps);
		snprintf(resp, DC_RESP_SZ, "{\"ok\":true,\"stepped\":%u}", (unsigned) nsteps);
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
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
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
		dc.listen_fd = dc_listen_tcp(port);
	}
#else
	if (config.debug_socket[0] == '\0' || config.debug_socket[0] == '/') {
		char path[512];

		if (config.debug_socket[0] == '/') {
			strncpy(path, config.debug_socket, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';
		} else {
			snprintf(path, sizeof(path), "%srpcemu-debug.sock",
			    rpcemu_get_datadir());
		}
		dc.listen_fd = dc_listen_unix(path);
	} else {
		int port = atoi(config.debug_socket);

		if (port > 0 && port < 65536) {
			dc.listen_fd = dc_listen_tcp(port);
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
