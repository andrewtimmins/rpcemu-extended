/*
  RPCEmu - An Acorn system emulator

  HostCmd - expose the guest RISC OS command line to the host.

  This file is the emulator (host) half of HostCmd: a small local socket
  server plus the SWI handler the guest gateway module talks to. Both the SWI
  handler (hostcmd) and the socket service (hostcmd_poll) run on the emulator
  thread, so the shared state below needs no locking.

  Wire protocol (see docs): the client sends a command as one '\n'-terminated
  line; the server streams back length-prefixed frames [type:1][len:u32 BE]
  [payload], where type is 'O' (output chunk), 'D' (done; payload = 4-byte BE
  return code) or 'X' (advisory text).

  Copyright (C) 2025-2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
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

#include "hostcmd.h"
#include "mem.h"
#include "rpcemu.h"

#ifdef _WIN32
/* Default control-socket port on Windows, where AF_UNIX is unavailable and the
   config default (a filesystem path) cannot be honoured. */
#define HOSTCMD_DEFAULT_TCP_PORT 15590
#endif

/* SWI sub-operations, selected in R9 (mirrors the HostFS R9 convention). */
#define HC_OP_REGISTER	0xffffffffu
#define HC_OP_POLL	0u
#define HC_OP_OUTPUT	1u
#define HC_OP_STATUS	2u
#define HC_OP_DISPLAY	3u

/* STATUS markers, in R0. */
#define HC_STATUS_START	0u
#define HC_STATUS_END	1u

/* Server -> client frame types. */
#define HC_FRAME_OUTPUT	'O'
#define HC_FRAME_DONE	'D'
#define HC_FRAME_NOTICE	'X'

#define HC_PROTOCOL_VERSION	1

#define HC_CMDLINE_MAX	256		/* RISC OS OS_CLI limit is 255 chars + NUL */
#define HC_IN_BUF_SZ	HC_CMDLINE_MAX
#define HC_OUT_RING_SZ	(64u * 1024u)	/* MUST be a power of two */

typedef struct {
	int	initialised;
	int	listen_fd;		/* -1 = disabled/failed */
	int	client_fd;		/* -1 = no client */
	int	is_tcp;
	char	sock_path[512];		/* AF_UNIX path, for unlink() on teardown */

	/* Inbound: bytes from the client accumulate here until a newline. */
	char	in_buf[HC_IN_BUF_SZ];
	size_t	in_len;
	int	in_overflow;		/* current line too long -> resync at next '\n' */

	/* The single command awaiting the guest. cmd_pending: queued, not yet
	   delivered; cmd_inflight: delivered, awaiting STATUS END. */
	int	cmd_pending;
	int	cmd_inflight;
	char	cmd_line[HC_CMDLINE_MAX];
	size_t	cmd_len;

	/* Outbound byte ring: producer = SWI handler, consumer = poll() socket
	   write. Empty when head == tail; usable capacity is HC_OUT_RING_SZ - 1. */
	uint8_t	out_ring[HC_OUT_RING_SZ];
	size_t	out_head;
	size_t	out_tail;

	/* Set once the guest module has announced itself, so an internal caller
	   can be told there is nothing listening rather than waiting for a
	   command that will never be collected. Cleared on reset. */
	int	guest_registered;

	/*
	 * Commands handed to the guest that nobody is waiting for any more, because
	 * the client went away or the caller gave up. The guest still finishes them
	 * and still reports a status, so that many STATUS ENDs are expected and
	 * discarded rather than being taken for the current command's.
	 */
	int	orphaned;

	/* An internal command: one of ours rather than a client's. It owns the
	   same cmd_* fields, so the two take turns. */
	int	internal_active;	/* ours is the command in cmd_* */
	hostcmd_internal_done_fn internal_done;
	void	*internal_opaque;
	char	*internal_out;		/* captured output, grown as it arrives */
	size_t	internal_out_len;
	size_t	internal_out_cap;
	int	internal_out_failed;	/* an allocation failed; report what we have */
} HostCmdState;

static HostCmdState hc = {
	.listen_fd = -1,
	.client_fd = -1,
};

/* ---- outbound ring helpers ------------------------------------------- */

static size_t
hc_ring_used(void)
{
	return (hc.out_head - hc.out_tail) & (HC_OUT_RING_SZ - 1);
}

static size_t
hc_ring_free(void)
{
	return (HC_OUT_RING_SZ - 1) - hc_ring_used();
}

/* Append len bytes; the caller must have ensured hc_ring_free() >= len. */
static void
hc_ring_write(const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		hc.out_ring[hc.out_head] = data[i];
		hc.out_head = (hc.out_head + 1) & (HC_OUT_RING_SZ - 1);
	}
}

/* ---- internal command output -------------------------------------------- */

/*
 * Collect output from an internal command.
 *
 * Grown geometrically and capped: a command that prints without end (*Type on a
 * device, say) must not be able to exhaust memory. Past the cap the output is
 * truncated and the command still completes, which is more useful than failing.
 */
#define HC_INTERNAL_OUT_MAX	(1024u * 1024u)

static void
hc_internal_capture(const char *data, size_t len)
{
	if (!hc.internal_active || hc.internal_out_failed || len == 0) {
		return;
	}

	if (hc.internal_out_len + len > HC_INTERNAL_OUT_MAX) {
		len = (hc.internal_out_len < HC_INTERNAL_OUT_MAX)
		    ? HC_INTERNAL_OUT_MAX - hc.internal_out_len : 0;
		if (len == 0) {
			return;
		}
	}

	if (hc.internal_out_len + len > hc.internal_out_cap) {
		size_t want = hc.internal_out_cap ? hc.internal_out_cap * 2 : 4096;
		char *bigger;

		while (want < hc.internal_out_len + len) {
			want *= 2;
		}
		bigger = realloc(hc.internal_out, want);
		if (bigger == NULL) {
			hc.internal_out_failed = 1;
			return;
		}
		hc.internal_out = bigger;
		hc.internal_out_cap = want;
	}

	memcpy(hc.internal_out + hc.internal_out_len, data, len);
	hc.internal_out_len += len;
}

/* Finish an internal command: hand the result over, then let go of it. */
static void
hc_internal_finish(uint32_t rc)
{
	const hostcmd_internal_done_fn done = hc.internal_done;
	void *opaque = hc.internal_opaque;
	char *out = hc.internal_out;
	const size_t len = hc.internal_out_len;

	hc.internal_active = 0;
	hc.internal_done = NULL;
	hc.internal_opaque = NULL;
	hc.internal_out = NULL;
	hc.internal_out_len = 0;
	hc.internal_out_cap = 0;
	hc.internal_out_failed = 0;

	if (done != NULL) {
		done(rc, (out != NULL) ? out : "", len, opaque);
	}
	free(out);
}

/* Push a complete frame if it fits; drop it otherwise (control frames are
   tiny and the ring is large, so a drop indicates a stuck client). */
static void
hc_push_frame(uint8_t type, const uint8_t *payload, uint32_t len)
{
	uint8_t hdr[5];

	if (hc.client_fd < 0) {
		return;
	}
	if (hc_ring_free() < (size_t) len + 5) {
		rpclog("HostCmd: output ring full, dropping '%c' frame\n", type);
		return;
	}
	hdr[0] = type;
	hdr[1] = (uint8_t) (len >> 24);
	hdr[2] = (uint8_t) (len >> 16);
	hdr[3] = (uint8_t) (len >> 8);
	hdr[4] = (uint8_t) len;
	hc_ring_write(hdr, 5);
	hc_ring_write(payload, len);
}

static void
hc_notice(const char *msg)
{
	hc_push_frame(HC_FRAME_NOTICE, (const uint8_t *) msg, (uint32_t) strlen(msg));
}

/* ---- SWI handler ----------------------------------------------------- */

void
hostcmd(ARMul_State *state)
{
	uint32_t op = state->Reg[9];

	switch (op) {
	case HC_OP_REGISTER:
		/* Handshake: acknowledge presence and report client state. Also the
		   only proof that the guest module exists, which is what lets an
		   internal caller be refused straight away rather than waiting for a
		   command nothing will collect. */
		hc.guest_registered = 1;
		state->Reg[0] = 0xffffffffu;
		state->Reg[1] = (hc.client_fd >= 0) ? 1u : 0u;
		break;

	case HC_OP_POLL: {
		/* R0 = guest buffer ptr, R1 = buffer size.
		   R0 out: 0 none / 1 delivered / 2 buffer too small; R1 out: length. */
		uint32_t bufptr = state->Reg[0];
		uint32_t bufsize = state->Reg[1];

		if (hc.cmd_pending && !hc.cmd_inflight) {
			if (hc.cmd_len + 1 > bufsize) {
				state->Reg[0] = 2;
				state->Reg[1] = (uint32_t) hc.cmd_len;
			} else {
				size_t i;

				for (i = 0; i < hc.cmd_len; i++) {
					ARMul_StoreByte(state, bufptr + i,
					    (uint8_t) hc.cmd_line[i]);
				}
				ARMul_StoreByte(state, bufptr + hc.cmd_len, 0);
				hc.cmd_pending = 0;
				hc.cmd_inflight = 1;
				state->Reg[0] = 1;
				state->Reg[1] = (uint32_t) hc.cmd_len;
			}
		} else {
			state->Reg[0] = 0;
		}
		break;
	}

	case HC_OP_OUTPUT: {
		/* R0 = guest ptr, R1 = length. R0 out: bytes accepted (backpressure). */
		uint32_t ptr = state->Reg[0];
		uint32_t len = state->Reg[1];
		uint32_t accept = 0;

		/* Ours: collect it here. An internal command has no socket to write
		   to, and the whole point of running one is to read what it said. */
		if (hc.internal_active) {
			uint32_t i;

			for (i = 0; i < len; i++) {
				const char byte = (char) ARMul_LoadByte(state, ptr + i);

				hc_internal_capture(&byte, 1);
			}
			state->Reg[0] = len;
			break;
		}

		if (hc.client_fd < 0 || (!hc.cmd_inflight && hc.orphaned > 0)) {
			/* Nobody to send it to, or it belongs to an abandoned command:
			   discard, but tell the guest it all went so it never stalls
			   waiting to flush. */
			state->Reg[0] = len;
			break;
		}

		{
			size_t freeb = hc_ring_free();

			if (freeb > 5) {
				uint32_t canpay = (uint32_t) (freeb - 5);

				accept = (len < canpay) ? len : canpay;
			}
		}
		if (accept > 0) {
			uint8_t hdr[5];
			uint32_t i;

			hdr[0] = HC_FRAME_OUTPUT;
			hdr[1] = (uint8_t) (accept >> 24);
			hdr[2] = (uint8_t) (accept >> 16);
			hdr[3] = (uint8_t) (accept >> 8);
			hdr[4] = (uint8_t) accept;
			hc_ring_write(hdr, 5);
			for (i = 0; i < accept; i++) {
				hc.out_ring[hc.out_head] =
				    (uint8_t) ARMul_LoadByte(state, ptr + i);
				hc.out_head = (hc.out_head + 1) & (HC_OUT_RING_SZ - 1);
			}
		}
		state->Reg[0] = accept;
		break;
	}

	case HC_OP_STATUS: {
		/* R0 = marker (START/END), R1 = return code, R2 = flags (bit0 truncated). */
		uint32_t marker = state->Reg[0];

		if (marker == HC_STATUS_END && !hc.cmd_inflight && hc.orphaned > 0) {
			/* An abandoned command reporting in. Not the current one's status,
			   so it must not close it or reach a client. */
			hc.orphaned--;
			rpclog("HostCmd: discarded the status of an abandoned command "
			       "(%d still outstanding)\n", hc.orphaned);
			state->Reg[0] = 0;
			break;
		}

		if (marker == HC_STATUS_END && hc.internal_active) {
			hc.cmd_inflight = 0;
			hc_internal_finish(state->Reg[1]);
			state->Reg[0] = 0;
			break;
		}

		if (marker == HC_STATUS_END) {
			uint32_t rc = state->Reg[1];
			uint8_t rcbuf[4];

			rcbuf[0] = (uint8_t) (rc >> 24);
			rcbuf[1] = (uint8_t) (rc >> 16);
			rcbuf[2] = (uint8_t) (rc >> 8);
			rcbuf[3] = (uint8_t) rc;
			hc_push_frame(HC_FRAME_DONE, rcbuf, 4);
			hc.cmd_inflight = 0;
		}
		/* START needs no frame: the 'O'/'D' framing already delimits output. */
		state->Reg[0] = 0;
		break;
	}

	case HC_OP_DISPLAY: {
		/* Report the display mode the guest should adopt to follow the host.
		   R0 out: 1 if a mode is being reported, else 0.
		   R1 out: generation - changes when the host display changes, so the
		           module compares it against the one it last acted on rather
		           than us having to track what it has seen.
		   R2/R3 out: width and height, already bounded by the host display and
		           by VRAM, so the guest can use them as they stand.
		   R4 out: refresh in Hz, 0 if the front-end did not supply one. */
		unsigned width = 0, height = 0, hz = 0;
		uint32_t generation = 0;
		const int have = rpcemu_guest_display_target(&width, &height, &hz,
		                                             &generation);

		state->Reg[0] = have ? 1u : 0u;
		state->Reg[1] = generation;
		state->Reg[2] = width;
		state->Reg[3] = height;
		state->Reg[4] = hz;
		break;
	}

	default:
		/* R0 = 0 so a guest module asking about something this build does not
		   implement reads a definite "no" rather than whatever it passed in. */
		state->Reg[0] = 0;
		rpclog("HostCmd: unknown SWI op 0x%08x\n", op);
		break;
	}
}

/* ---- socket lifecycle ------------------------------------------------ */

static void
hc_set_nonblock(int fd)
{
	socket_set_nonblocking(fd);
}

#ifndef _WIN32
static int
hc_listen_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		rpclog("HostCmd: socket path too long: %s\n", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("HostCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	unlink(path);	/* clear a stale socket left by a previous crash */
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("HostCmd: bind(%s) failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("HostCmd: listen() failed: %s\n", strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	hc_set_nonblock(fd);
	strncpy(hc.sock_path, path, sizeof(hc.sock_path) - 1);
	hc.sock_path[sizeof(hc.sock_path) - 1] = '\0';
	hc.is_tcp = 0;
	rpclog("HostCmd: listening on AF_UNIX %s\n", path);
	return fd;
}
#endif /* !_WIN32 */

static int
hc_listen_tcp(int port)
{
	struct sockaddr_in addr;
	int fd;
	int on = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("HostCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	/* local only */
	addr.sin_port = htons((uint16_t) port);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("HostCmd: bind(127.0.0.1:%d) failed: %s\n", port, strerror(errno));
		closesocket(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("HostCmd: listen() failed: %s\n", strerror(errno));
		closesocket(fd);
		return -1;
	}
	hc_set_nonblock(fd);
	hc.is_tcp = 1;
	hc.sock_path[0] = '\0';
	rpclog("HostCmd: listening on TCP 127.0.0.1:%d\n", port);
	return fd;
}

void
hostcmd_init(void)
{
	/* Idempotent: tear down any previous listener (e.g. machine switch). */
	if (hc.initialised) {
		hostcmd_close();
	}

	memset(&hc, 0, sizeof(hc));
	hc.listen_fd = -1;
	hc.client_fd = -1;
	hc.initialised = 1;

	if (!config.hostcmd_enabled) {
		return;
	}

#ifdef _WIN32
	/* Windows has no useful AF_UNIX (no filesystem permission semantics, and
	   the unlink()-on-teardown is meaningless), so the control socket is TCP
	   loopback only. A bare integer in hostcmd_socket selects the port;
	   anything else falls back to the default port. */
	{
		int port = HOSTCMD_DEFAULT_TCP_PORT;

		if (config.hostcmd_socket[0] != '\0'
		    && config.hostcmd_socket[0] != '/')
		{
			int p = atoi(config.hostcmd_socket);

			if (p > 0 && p < 65536) {
				port = p;
			}
		}
		hc.listen_fd = hc_listen_tcp(port);
	}
#else
	/* Transport selection from config.hostcmd_socket:
	   - empty or path-like ('/')  -> AF_UNIX
	   - a bare integer            -> TCP 127.0.0.1:<port>
	   Default AF_UNIX path is "<datadir>hostcmd.sock". */
	if (config.hostcmd_socket[0] == '\0' || config.hostcmd_socket[0] == '/') {
		char path[512];

		if (config.hostcmd_socket[0] == '/') {
			strncpy(path, config.hostcmd_socket, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';
		} else {
			snprintf(path, sizeof(path), "%shostcmd.sock",
			    rpcemu_get_datadir());
		}
		hc.listen_fd = hc_listen_unix(path);
	} else {
		int port = atoi(config.hostcmd_socket);

		if (port > 0 && port < 65536) {
			hc.listen_fd = hc_listen_tcp(port);
		} else {
			rpclog("HostCmd: invalid socket spec '%s', disabling\n",
			    config.hostcmd_socket);
		}
	}
#endif
}

static void
hc_drop_client(void)
{
	if (hc.client_fd >= 0) {
		closesocket(hc.client_fd);
		hc.client_fd = -1;
	}
	hc.in_len = 0;
	hc.in_overflow = 0;
	hc.out_head = hc.out_tail = 0;	/* discard undelivered output */
	hc.cmd_pending = 0;		/* nobody to serve an undelivered command */

	/*
	 * The guest module is unaffected by a client coming and going, so its
	 * registration stands. Clearing it here was a mistake: it would have left
	 * the emulator's own commands refused after anyone used rpcemu-run, since
	 * the module only announces itself when it starts.
	 *
	 * A command already delivered is orphaned rather than waited for. Nobody is
	 * left to receive its result, and holding cmd_inflight for it would stop the
	 * next client's command being read - the channel wedged until the guest
	 * happened to finish.
	 */
	if (hc.cmd_inflight && !hc.internal_active) {
		hc.cmd_inflight = 0;
		hc.orphaned++;
	}
}

void
hostcmd_reset(void)
{
	if (!hc.initialised) {
		return;
	}
	/* Machine reset: end any in-flight command cleanly for the client. */
	if (hc.client_fd >= 0 && hc.cmd_inflight) {
		uint8_t rc[4] = { 0xff, 0xff, 0xff, 0xff };

		hc_notice("machine reset\n");
		hc_push_frame(HC_FRAME_DONE, rc, 4);
	}
	hc.cmd_pending = 0;
	hc.cmd_inflight = 0;
	hc.cmd_len = 0;
	hc.orphaned = 0;		/* nothing outstanding can now report back */

	/* The module goes with the machine and will announce itself again. */
	hc.guest_registered = 0;

	if (hc.internal_active) {
		/* Reset underneath it, so it will never finish. Tell the caller rather
		   than leaving them waiting for a reply that cannot come. */
		hc.internal_done = NULL;
		hc_internal_finish(0);
	}
	/* Listener and client persist across a guest reboot. */
}

void
hostcmd_close(void)
{
	if (hc.client_fd >= 0) {
		closesocket(hc.client_fd);
		hc.client_fd = -1;
	}
	if (hc.listen_fd >= 0) {
		closesocket(hc.listen_fd);
		hc.listen_fd = -1;
	}
#ifndef _WIN32
	if (!hc.is_tcp && hc.sock_path[0] != '\0') {
		unlink(hc.sock_path);
		hc.sock_path[0] = '\0';
	}
#endif
	hc.initialised = 0;
}

/* ---- per-tick service ------------------------------------------------ */

static void
hc_read_client(void)
{
	uint8_t tmp[4096];
	ssize_t n;
	ssize_t i;

	n = recv(hc.client_fd, (char *) tmp, sizeof(tmp), 0);
	if (n == 0) {
		hc_drop_client();
		return;
	}
	if (n < 0) {
		if (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN) {
			return;
		}
		hc_drop_client();
		return;
	}

	for (i = 0; i < n; i++) {
		uint8_t b = tmp[i];

		if (hc.in_overflow) {
			if (b == '\n') {
				hc.in_overflow = 0;
				hc_notice("command too long, ignored\n");
			}
			continue;
		}
		if (b == '\n') {
			size_t copy = hc.in_len;

			hc.in_len = 0;
			if (copy > 0 && hc.in_buf[copy - 1] == '\r') {
				copy--;
			}
			if (copy == 0) {
				continue;	/* blank line: nothing to run */
			}
			if (hc.cmd_pending || hc.cmd_inflight) {
				/* Interactive model is one command at a time; a client
				   should wait for 'D' before sending the next line. */
				hc_notice("busy, command dropped\n");
				continue;
			}
			memcpy(hc.cmd_line, hc.in_buf, copy);
			hc.cmd_line[copy] = '\0';
			hc.cmd_len = copy;
			hc.cmd_pending = 1;
			continue;
		}
		if (hc.in_len < HC_CMDLINE_MAX - 1) {
			hc.in_buf[hc.in_len++] = (char) b;
		} else {
			hc.in_overflow = 1;
			hc.in_len = 0;
		}
	}
}

static void
hc_write_client(void)
{
	while (hc_ring_used() > 0) {
		size_t used = hc_ring_used();
		size_t to_end = HC_OUT_RING_SZ - hc.out_tail;
		size_t contig = (used < to_end) ? used : to_end;
		ssize_t n = send(hc.client_fd, (const char *) &hc.out_ring[hc.out_tail], contig,
		    MSG_NOSIGNAL);

		if (n < 0) {
			if (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN) {
				return;
			}
			hc_drop_client();
			return;
		}
		if (n == 0) {
			return;
		}
		hc.out_tail = (hc.out_tail + (size_t) n) & (HC_OUT_RING_SZ - 1);
	}
}

/* ---- running a command from inside the emulator -------------------------- */

int
hostcmd_internal_ready(void)
{
	return hc.initialised && hc.guest_registered;
}

int
hostcmd_internal_busy(void)
{
	return hc.internal_active;
}

int
hostcmd_internal_submit(const char *command, hostcmd_internal_done_fn done,
    void *opaque)
{
	size_t len;

	if (command == NULL) {
		return -1;
	}
	len = strlen(command);
	if (len == 0 || len + 1 > HC_CMDLINE_MAX) {
		rpclog("HostCmd: internal command rejected (%zu bytes)\n", len);
		return -1;
	}
	if (!hostcmd_internal_ready()) {
		rpclog("HostCmd: internal command refused, the guest module has not "
		       "announced itself\n");
		return -1;
	}
	/* One command at a time, shared with any outside client: taking turns is
	   simpler to reason about than interleaving two conversations, and an
	   internal caller can retry. */
	if (hc.internal_active || hc.cmd_pending || hc.cmd_inflight) {
		return -1;
	}

	memcpy(hc.cmd_line, command, len);
	hc.cmd_line[len] = '\0';
	hc.cmd_len = len;
	hc.cmd_pending = 1;
	hc.cmd_inflight = 0;

	hc.internal_active = 1;
	hc.internal_done = done;
	hc.internal_opaque = opaque;
	hc.internal_out = NULL;
	hc.internal_out_len = 0;
	hc.internal_out_cap = 0;
	hc.internal_out_failed = 0;

	rpclog("HostCmd: internal command queued: %s\n", hc.cmd_line);
	return 0;
}

void
hostcmd_internal_abandon(void)
{
	if (!hc.internal_active) {
		return;
	}

	/* Drop the callback first, so finishing cannot call back into a caller
	   that has given up on us. */
	hc.internal_done = NULL;
	if (hc.cmd_pending) {
		hc.cmd_pending = 0;	/* never delivered: nothing to wait for */
		hc.cmd_len = 0;
	} else if (hc.cmd_inflight) {
		/*
		 * Already with the guest. It finishes in its own time, but the channel
		 * is handed back now: leaving cmd_inflight set would stop any later
		 * command being read, which is a stuck channel for everyone rather than
		 * one abandoned request.
		 *
		 * Safe because the guest asks for work only between commands, so a
		 * command accepted now cannot be delivered until this one has finished.
		 */
		hc.cmd_inflight = 0;
		hc.orphaned++;
	}
	hc_internal_finish(0);
}

void
hostcmd_poll(void)
{
	struct pollfd pfd;

	if (hc.listen_fd < 0) {
		return;
	}

	if (hc.client_fd < 0) {
		pfd.fd = hc.listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
			int c = accept(hc.listen_fd, NULL, NULL);

			if (c >= 0) {
				hc_set_nonblock(c);
				hc.client_fd = c;
				hc.in_len = 0;
				hc.in_overflow = 0;
				hc.out_head = hc.out_tail = 0;
				hc_notice("RPCEmu HostCmd v1\n");
			}
		}
		return;
	}

	pfd.fd = hc.client_fd;
	pfd.events = 0;
	pfd.revents = 0;
	/* Only read a new command while idle: this preserves command order and
	   back-pressures a client that types ahead (its bytes wait in the kernel
	   socket buffer until we finish the current command). */
	if (!hc.cmd_pending && !hc.cmd_inflight) {
		pfd.events |= POLLIN;
	}
	if (hc_ring_used() > 0) {
		pfd.events |= POLLOUT;
	}
	if (pfd.events == 0) {
		return;
	}
	if (poll(&pfd, 1, 0) <= 0) {
		return;
	}
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		hc_drop_client();
		return;
	}
	if (pfd.revents & POLLIN) {
		hc_read_client();
		if (hc.client_fd < 0) {
			return;
		}
	}
	if (pfd.revents & POLLOUT) {
		hc_write_client();
	}
}
