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
 * netcapcmd.c - see netcapcmd.h.
 *
 * Built to the same shape as debugcmd.c on purpose: one client at a time, a
 * non-blocking listener polled from the emulator thread, and an outbound ring
 * so that a slow reader stalls its own stream rather than the machine.
 *
 * ★ A slow reader must never slow the guest down.
 *
 * That is the whole reason for the ring. Somebody piping a live capture into
 * Wireshark on a busy machine can easily read slower than the guest transmits,
 * and the emulator thread cannot wait for them. When the ring fills, frames
 * are dropped from the STREAM - not from the capture file, and not from the
 * ring in netcapture.c - and the drop is counted and reported, because a
 * capture with a silent hole in it is worse than one that admits to it.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "socket-compat.h"

#ifndef _WIN32
#include <sys/un.h>
#endif

#include "machine_lock.h"
#include "netcapcmd.h"
#include "netcapture.h"
#include "net_dissect.h"
#include "rpcemu.h"

#ifdef _WIN32
#define NETCAPCMD_DEFAULT_TCP_PORT 15592
#endif

#define NC_IN_BUF_SZ	512
#define NC_OUT_RING_SZ	(1024u * 1024u)		/* MUST be a power of two */
#define NC_RESP_SZ	(8u * 1024u)		/* one JSON line */
#define NC_TAIL_MAX	512u			/* frames one `tail` may return */

typedef enum {
	NC_MODE_LINE = 0,	/* request in, response out */
	NC_MODE_FOLLOW,		/* a JSON line per frame, from now on */
	NC_MODE_PCAP		/* raw pcap, binary, from now on */
} NetCapCmdMode;

typedef struct {
	int	initialised;
	int	listen_fd;
	int	client_fd;
	int	is_tcp;
	char	sock_path[512];

	char	in_buf[NC_IN_BUF_SZ];
	size_t	in_len;
	int	in_overflow;

	uint8_t	*out_ring;		/* NC_OUT_RING_SZ, allocated on first use */
	size_t	out_head;
	size_t	out_tail;

	NetCapCmdMode mode;
	uint64_t last_serial;		/* newest frame handed to this client */
	uint64_t stream_dropped;	/* frames the client was too slow for */
	int	warned_dropped;
} NetCapCmdState;

static NetCapCmdState nc = {
	.listen_fd = -1,
	.client_fd = -1,
};

/* ---- outbound ring ----------------------------------------------------- */

static size_t
nc_ring_used(void)
{
	return (nc.out_head - nc.out_tail) & (NC_OUT_RING_SZ - 1);
}

static size_t
nc_ring_free(void)
{
	return (NC_OUT_RING_SZ - 1) - nc_ring_used();
}

/**
 * Queue bytes for the client.
 *
 * All or nothing: a half-written pcap record would desynchronise the stream
 * for good, and a half-written JSON line cannot be parsed. The caller is told
 * so it can count the loss rather than pretend it did not happen.
 *
 * @return non-zero if it was queued
 */
static int
nc_send_bytes(const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t i;

	if (nc.client_fd < 0 || nc.out_ring == NULL || len > nc_ring_free()) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		nc.out_ring[nc.out_head] = p[i];
		nc.out_head = (nc.out_head + 1) & (NC_OUT_RING_SZ - 1);
	}
	return 1;
}

/** Queue a line, appending the newline. */
static int
nc_send(const char *s)
{
	return nc_send_bytes(s, strlen(s)) && nc_send_bytes("\n", 1);
}

static void
nc_sendf(const char *fmt, ...)
{
	char buf[NC_RESP_SZ];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	(void) nc_send(buf);
}

/* ---- JSON helpers ------------------------------------------------------ */

/** Escape a string for JSON. Control characters go out as \uXXXX. */
static void
nc_json_escape(const char *in, char *out, size_t out_len)
{
	size_t o = 0;

	for (; *in != '\0' && o + 7 < out_len; in++) {
		const unsigned char c = (unsigned char) *in;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char) c;
		} else if (c < 0x20) {
			o += (size_t) snprintf(out + o, out_len - o, "\\u%04x", c);
		} else {
			out[o++] = (char) c;
		}
	}
	out[o] = '\0';
}

/* ---- sockets ----------------------------------------------------------- */

static void
nc_set_nonblock(int fd)
{
	socket_set_nonblocking(fd);
}

#ifndef _WIN32
static int
nc_parent_dir_exists(const char *path)
{
	char dir[512];
	char *slash;
	struct stat st;

	if (strlen(path) >= sizeof(dir)) {
		return 0;
	}
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash == NULL || slash == dir) {
		return 1;
	}
	*slash = '\0';
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
nc_listen_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		rpclog("NetCapCmd: socket path too long: %s\n", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("NetCapCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* The length was checked above; snprintf rather than strncpy so the
	   terminator is not something to reason about. */
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("NetCapCmd: bind(%s) failed: %s\n", path, strerror(errno));
		closesocket(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("NetCapCmd: listen() failed: %s\n", strerror(errno));
		closesocket(fd);
		return -1;
	}
	nc_set_nonblock(fd);
	nc.is_tcp = 0;
	snprintf(nc.sock_path, sizeof(nc.sock_path), "%s", path);
	rpclog("NetCapCmd: listening on %s\n", path);
	return fd;
}
#endif /* !_WIN32 */

static int
nc_listen_tcp(int port)
{
	struct sockaddr_in addr;
	int fd;
	int on = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		rpclog("NetCapCmd: socket() failed: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t) port);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		rpclog("NetCapCmd: bind(127.0.0.1:%d) failed: %s\n", port,
		    strerror(errno));
		closesocket(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		rpclog("NetCapCmd: listen() failed: %s\n", strerror(errno));
		closesocket(fd);
		return -1;
	}
	nc_set_nonblock(fd);
	nc.is_tcp = 1;
	nc.sock_path[0] = '\0';
	rpclog("NetCapCmd: listening on TCP 127.0.0.1:%d\n", port);
	return fd;
}

static void
nc_drop_client(void)
{
	if (nc.client_fd >= 0) {
		closesocket(nc.client_fd);
		nc.client_fd = -1;
	}
	nc.in_len = 0;
	nc.in_overflow = 0;
	nc.out_head = nc.out_tail = 0;
	nc.mode = NC_MODE_LINE;
	nc.last_serial = 0;
	nc.stream_dropped = 0;
	nc.warned_dropped = 0;
}

/* ---- frames as JSON ---------------------------------------------------- */

static void
nc_frame_json(const NetcapFrame *fr, char *out, size_t out_len)
{
	NetDissectSummary sum;
	char proto[32], src[128], dst[128], info[400];
	char hex[NETCAP_SNAPLEN * 2 + 1];
	uint32_t i;

	netdis_summary(fr->data, fr->captured, &sum);
	nc_json_escape(sum.protocol, proto, sizeof(proto));
	nc_json_escape(sum.source, src, sizeof(src));
	nc_json_escape(sum.dest, dst, sizeof(dst));
	nc_json_escape(sum.info, info, sizeof(info));

	for (i = 0; i < fr->captured; i++) {
		static const char digits[] = "0123456789abcdef";

		hex[i * 2] = digits[fr->data[i] >> 4];
		hex[i * 2 + 1] = digits[fr->data[i] & 0x0f];
	}
	hex[fr->captured * 2] = '\0';

	snprintf(out, out_len,
	    "{\"serial\":%llu,\"sec\":%u,\"usec\":%u,\"dir\":\"%s\","
	    "\"len\":%u,\"cap\":%u,\"proto\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
	    "\"info\":\"%s\",\"data\":\"%s\"}",
	    (unsigned long long) fr->serial, fr->sec, fr->usec,
	    (fr->direction == NETCAP_TX) ? "tx" : "rx",
	    fr->length, fr->captured, proto, src, dst, info, hex);
}

/* ---- commands ---------------------------------------------------------- */

static void
nc_cmd_status(void)
{
	NetcapStats st;
	char path[600];

	netcap_get_stats(&st);
	nc_json_escape(st.file_path, path, sizeof(path));
	nc_sendf("{\"ok\":true,\"frames\":%llu,\"bytes\":%llu,\"tx\":%llu,"
	    "\"rx\":%llu,\"file\":\"%s\",\"file_active\":%s,\"file_bytes\":%llu,"
	    "\"file_stopped_full\":%s,\"ring\":%s,\"ring_dropped\":%llu,"
	    "\"stream_dropped\":%llu}",
	    (unsigned long long) st.frames, (unsigned long long) st.bytes,
	    (unsigned long long) st.frames_tx, (unsigned long long) st.frames_rx,
	    path, st.file_active ? "true" : "false",
	    (unsigned long long) st.file_bytes,
	    st.file_stopped_full ? "true" : "false",
	    st.ring_active ? "true" : "false",
	    (unsigned long long) st.dropped_ring,
	    (unsigned long long) nc.stream_dropped);
}

static void
nc_cmd_tail(const char *arg)
{
	unsigned want = 50;
	NetcapFrame *frames;
	unsigned count = 0;
	unsigned i;

	if (arg != NULL && *arg != '\0') {
		const long n = strtol(arg, NULL, 10);

		if (n > 0) {
			want = (unsigned) n;
		}
	}
	if (want > NC_TAIL_MAX) {
		want = NC_TAIL_MAX;
	}
	if (!netcap_ring_enabled()) {
		nc_send("{\"ok\":false,\"error\":\"frames are not being kept; "
		        "use 'ring on' first\"}");
		return;
	}

	frames = malloc(sizeof(*frames) * want);
	if (frames == NULL) {
		nc_send("{\"ok\":false,\"error\":\"out of memory\"}");
		return;
	}
	/*
	 * Asks for everything and keeps the last `want`. The ring hands frames
	 * back oldest first, and "tail" means the newest ones, so the window has
	 * to be taken from the end.
	 */
	{
		NetcapFrame *all = malloc(sizeof(*all) * NETCAP_RING_FRAMES);
		unsigned total = 0;

		if (all == NULL) {
			free(frames);
			nc_send("{\"ok\":false,\"error\":\"out of memory\"}");
			return;
		}
		netcap_copy_since(0, all, NETCAP_RING_FRAMES, &total);
		count = (total < want) ? total : want;
		memcpy(frames, all + (total - count), sizeof(*frames) * count);
		free(all);
	}

	for (i = 0; i < count; i++) {
		char line[NC_RESP_SZ];

		nc_frame_json(&frames[i], line, sizeof(line));
		if (!nc_send(line)) {
			break;		/* the client is not keeping up; stop cleanly */
		}
	}
	free(frames);
	nc_send("end");
}

static void
nc_cmd_help(void)
{
	nc_send("{\"ok\":true,\"commands\":["
	    "\"ping\",\"help\",\"status\",\"start <path> [maxbytes]\",\"stop\","
	    "\"clear\",\"ring on|off\",\"tail [n]\",\"follow\",\"pcap\",\"quit\"]}");
}

/** Begin a stream, in whichever format. Frames from now on, not the backlog. */
static void
nc_begin_stream(NetCapCmdMode mode)
{
	NetcapStats st;

	netcap_ring_enable(1);
	netcap_get_stats(&st);
	nc.last_serial = st.frames;	/* from here, not the whole history */
	nc.stream_dropped = 0;
	nc.warned_dropped = 0;

	if (mode == NC_MODE_PCAP) {
		uint8_t header[NETCAP_PCAP_HEADER_LEN];

		netcap_pcap_file_header(header);
		(void) nc_send_bytes(header, sizeof(header));
	} else {
		nc_send("{\"ok\":true,\"following\":true}");
	}
	nc.mode = mode;
}

static void
nc_handle_line(char *line)
{
	char *arg;

	while (*line == ' ' || *line == '\t') {
		line++;
	}
	arg = strpbrk(line, " \t");
	if (arg != NULL) {
		*arg++ = '\0';
		while (*arg == ' ' || *arg == '\t') {
			arg++;
		}
	}

	if (strcmp(line, "ping") == 0) {
		nc_send("{\"ok\":true,\"pong\":true}");
	} else if (strcmp(line, "help") == 0 || line[0] == '\0') {
		nc_cmd_help();
	} else if (strcmp(line, "status") == 0) {
		nc_cmd_status();
	} else if (strcmp(line, "start") == 0) {
		char path[512];
		uint64_t max = 0;
		char *space;

		if (arg == NULL || *arg == '\0') {
			nc_send("{\"ok\":false,\"error\":\"start needs a file path\"}");
			return;
		}
		snprintf(path, sizeof(path), "%s", arg);
		space = strpbrk(path, " \t");
		if (space != NULL) {
			*space++ = '\0';
			max = strtoull(space, NULL, 10);
		}
		if (netcap_file_start(path, max)) {
			nc_sendf("{\"ok\":true,\"capturing\":\"%s\"}", path);
		} else {
			nc_send("{\"ok\":false,\"error\":\"could not open that file\"}");
		}
	} else if (strcmp(line, "stop") == 0) {
		netcap_file_stop();
		nc_send("{\"ok\":true}");
	} else if (strcmp(line, "clear") == 0) {
		netcap_clear();
		nc_send("{\"ok\":true}");
	} else if (strcmp(line, "ring") == 0) {
		if (arg != NULL && strcmp(arg, "off") == 0) {
			netcap_ring_enable(0);
		} else {
			netcap_ring_enable(1);
		}
		nc_sendf("{\"ok\":true,\"ring\":%s}",
		    netcap_ring_enabled() ? "true" : "false");
	} else if (strcmp(line, "tail") == 0) {
		nc_cmd_tail(arg);
	} else if (strcmp(line, "follow") == 0) {
		nc_begin_stream(NC_MODE_FOLLOW);
	} else if (strcmp(line, "pcap") == 0) {
		nc_begin_stream(NC_MODE_PCAP);
	} else if (strcmp(line, "quit") == 0) {
		nc_drop_client();
	} else {
		nc_sendf("{\"ok\":false,\"error\":\"unknown command\"}");
	}
}

/* ---- streaming --------------------------------------------------------- */

/**
 * Hand the client whatever has happened since it last saw a frame.
 *
 * Called every poll while following. Bounded per call so a burst cannot turn
 * one poll into an unbounded amount of work on the emulator thread.
 */
static void
nc_pump_stream(void)
{
	enum { BATCH = 64 };
	NetcapFrame *batch;
	unsigned count = 0;
	unsigned i;
	uint64_t oldest;

	if (nc.mode == NC_MODE_LINE || nc.client_fd < 0) {
		return;
	}
	batch = malloc(sizeof(*batch) * BATCH);
	if (batch == NULL) {
		return;
	}
	oldest = netcap_copy_since(nc.last_serial, batch, BATCH, &count);

	/*
	 * The client has fallen so far behind that frames it never saw have been
	 * overwritten in netcapture's own ring. Said once rather than per frame:
	 * the point is that there is a hole, not how big it is.
	 */
	if (oldest > nc.last_serial + 1 && nc.last_serial != 0 &&
	    !nc.warned_dropped && nc.mode == NC_MODE_FOLLOW)
	{
		nc_sendf("{\"ok\":true,\"missed\":%llu}",
		    (unsigned long long) (oldest - nc.last_serial - 1));
		nc.warned_dropped = 1;
	}

	for (i = 0; i < count; i++) {
		const NetcapFrame *fr = &batch[i];
		int queued;

		if (nc.mode == NC_MODE_PCAP) {
			uint8_t record[NETCAP_PCAP_RECORD_LEN];

			netcap_pcap_record_header(record, fr->sec, fr->usec, fr->captured,
			    fr->length);
			/* Both halves or neither: a record header with no payload behind
			   it makes every later frame in the stream unreadable. */
			queued = (nc_ring_free() >= sizeof(record) + fr->captured) &&
			    nc_send_bytes(record, sizeof(record)) &&
			    nc_send_bytes(fr->data, fr->captured);
		} else {
			char json[NC_RESP_SZ];

			nc_frame_json(fr, json, sizeof(json));
			queued = nc_send(json);
		}

		if (!queued) {
			/* The outbound ring is full: this reader is slower than the
			   guest. Stop here and try again next poll rather than
			   spinning, and count what was lost. */
			nc.stream_dropped++;
			break;
		}
		nc.last_serial = fr->serial;
	}
	free(batch);
}

/* ---- client I/O -------------------------------------------------------- */

static void
nc_read_client(void)
{
	char buf[512];
	int n;

	n = (int) recv(nc.client_fd, buf, sizeof(buf), 0);
	if (n == 0) {
		nc_drop_client();
		return;
	}
	if (n < 0) {
		if (sock_errno() != SOCK_EWOULDBLOCK && sock_errno() != SOCK_EINTR) {
			nc_drop_client();
		}
		return;
	}

	/*
	 * A client that has asked for a stream is not expected to say anything
	 * else, but anything it does send is read and thrown away rather than
	 * left to fill the socket buffer and stall its own reads.
	 */
	if (nc.mode != NC_MODE_LINE) {
		return;
	}

	{
		int i;

		for (i = 0; i < n; i++) {
			const char c = buf[i];

			if (c == '\n' || c == '\r') {
				if (nc.in_overflow) {
					nc_send("{\"ok\":false,\"error\":\"line too long\"}");
					nc.in_overflow = 0;
				} else if (nc.in_len > 0) {
					nc.in_buf[nc.in_len] = '\0';
					nc_handle_line(nc.in_buf);
				}
				nc.in_len = 0;
				if (nc.client_fd < 0) {
					return;
				}
				continue;
			}
			if (nc.in_len + 1 < sizeof(nc.in_buf)) {
				nc.in_buf[nc.in_len++] = c;
			} else {
				nc.in_overflow = 1;
			}
		}
	}
}

static void
nc_write_client(void)
{
	while (nc_ring_used() > 0) {
		const size_t contiguous =
		    (nc.out_head > nc.out_tail) ? nc.out_head - nc.out_tail
		                                : NC_OUT_RING_SZ - nc.out_tail;
		int n = (int) send(nc.client_fd, (const char *) nc.out_ring + nc.out_tail,
		    (int) contiguous, MSG_NOSIGNAL);

		if (n <= 0) {
			if (n < 0 && sock_errno() != SOCK_EWOULDBLOCK &&
			    sock_errno() != SOCK_EINTR)
			{
				nc_drop_client();
			}
			return;
		}
		nc.out_tail = (nc.out_tail + (size_t) n) & (NC_OUT_RING_SZ - 1);
	}
}

/* ---- lifecycle --------------------------------------------------------- */

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
nc_listen_tcp_from(int first)
{
	const int attempts = 16;
	int i;

	for (i = 0; i < attempts; i++) {
		const int port = first + i;
		int fd;

		if (port < 1 || port > 65535) {
			break;
		}
		fd = nc_listen_tcp(port);
		if (fd >= 0) {
			if (i > 0) {
				rpclog("NetCapCmd: TCP port %d was in use, listening on %d "
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
nc_record_tcp_endpoint(int fd)
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
	machine_lock_set_netcap_endpoint(endpoint);
}

void
netcapcmd_init(void)
{
	if (nc.initialised) {
		netcapcmd_close();
	}
	memset(&nc, 0, sizeof(nc));
	nc.listen_fd = -1;
	nc.client_fd = -1;
	nc.initialised = 1;

	if (!config.netcap_enabled) {
		return;
	}

#ifdef _WIN32
	{
		int port = NETCAPCMD_DEFAULT_TCP_PORT;

		if (config.netcap_socket[0] != '\0' && config.netcap_socket[0] != '/') {
			const int p = atoi(config.netcap_socket);

			if (p > 0 && p < 65536) {
				port = p;
			}
		}
		nc.listen_fd = nc_listen_tcp_from(port);
		nc_record_tcp_endpoint(nc.listen_fd);
	}
#else
	if (config.netcap_socket[0] == '\0' || config.netcap_socket[0] == '/') {
		char path[512];

		if (config.netcap_socket[0] == '/') {
			snprintf(path, sizeof(path), "%s", config.netcap_socket);
			if (!nc_parent_dir_exists(path)) {
				rpclog("NetCapCmd: '%s' is not there any more, using the "
				       "default socket instead\n", path);
				snprintf(path, sizeof(path), "%srpcemu-netcap.sock",
				    rpcemu_get_machine_datadir());
			}
		} else {
			/*
			 * ★ The machine's own directory, as hostcmd.c and debugcmd.c
			 * already do.
			 *
			 * This was the data directory, which is one path for every
			 * machine in it - and nc_listen_unix() unlinks before it binds,
			 * so the second machine to start did not fail, it took the
			 * first machine's socket away. The first was then unreachable
			 * with its socket file gone from under it, rpcemu-netcap always
			 * reached whichever machine had started last, and stopping that
			 * one removed the path entirely.
			 */
			snprintf(path, sizeof(path), "%srpcemu-netcap.sock",
			    rpcemu_get_machine_datadir());
		}
		nc.listen_fd = nc_listen_unix(path);
		if (nc.listen_fd >= 0) {
			/* Recorded so a tool can find it. A machine may be configured
			   with any path, so nothing outside can work one out. */
			machine_lock_set_netcap_endpoint(path);
		}
	} else {
		const int p = atoi(config.netcap_socket);

		nc.listen_fd = nc_listen_tcp((p > 0 && p < 65536) ? p : 15592);
		nc_record_tcp_endpoint(nc.listen_fd);
	}
#endif
}

void
netcapcmd_reset(void)
{
	/* A machine reset does not disconnect anybody: watching a boot is one of
	   the things this is for. */
}

void
netcapcmd_close(void)
{
	if (!nc.initialised) {
		return;
	}
	nc_drop_client();
	if (nc.listen_fd >= 0) {
		closesocket(nc.listen_fd);
		nc.listen_fd = -1;
	}
#ifndef _WIN32
	if (!nc.is_tcp && nc.sock_path[0] != '\0') {
		unlink(nc.sock_path);
	}
#endif
	free(nc.out_ring);
	nc.out_ring = NULL;
	nc.initialised = 0;
}

void
netcapcmd_poll(void)
{
	struct pollfd pfd;

	if (nc.listen_fd < 0) {
		return;
	}

	if (nc.client_fd < 0) {
		/*
		 * Throttled exactly as DebugCmd's is, and for the same reason: this
		 * runs tens of thousands of times a second, and asking the kernel
		 * whether anybody has connected costs a syscall every time. Somebody
		 * about to connect can wait a few milliseconds.
		 */
		static unsigned since_listen_check;

		if ((++since_listen_check & 0x3fu) != 0) {
			return;
		}
		pfd.fd = nc.listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
			const int c = accept(nc.listen_fd, NULL, NULL);

			if (c >= 0) {
				if (nc.out_ring == NULL) {
					nc.out_ring = malloc(NC_OUT_RING_SZ);
				}
				if (nc.out_ring == NULL) {
					closesocket(c);
					return;
				}
				nc_set_nonblock(c);
				nc.client_fd = c;
				nc.in_len = 0;
				nc.in_overflow = 0;
				nc.out_head = nc.out_tail = 0;
				nc.mode = NC_MODE_LINE;
				nc.last_serial = 0;
				nc.stream_dropped = 0;
				nc.warned_dropped = 0;
			}
		}
		return;
	}

	if (nc.mode != NC_MODE_LINE) {
		nc_pump_stream();
	}

	pfd.fd = nc.client_fd;
	pfd.events = POLLIN;
	if (nc_ring_used() > 0) {
		pfd.events |= POLLOUT;
	}
	pfd.revents = 0;
	if (poll(&pfd, 1, 0) <= 0) {
		return;
	}
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		nc_drop_client();
		return;
	}
	if (pfd.revents & POLLIN) {
		nc_read_client();
		if (nc.client_fd < 0) {
			return;
		}
	}
	if (pfd.revents & POLLOUT) {
		nc_write_client();
	}
}
