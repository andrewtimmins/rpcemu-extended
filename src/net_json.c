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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "socket-compat.h"

#include "guest_subnet.h"
#include "net_json.h"
#include "net_switch.h"
#include "network.h"
#include "network-nat.h"
#include "rpcemu.h"

/* An Ethernet frame, plus the header this splits off it. */
#define NET_JSON_MAX_FRAME	1522
#define NET_JSON_HEADER_LEN	14

/*
 * A line is the payload base64-encoded, which is four characters per three
 * bytes, plus the JSON around it. Twice the frame plus a little is comfortable.
 */
#define NET_JSON_MAX_LINE	4096

/* How many lines one poll will take. A busy wire must not be able to keep this
   loop running instead of the emulator. */
#define NET_JSON_POLL_MAX	64

/* Longer when the server has never answered - usually one that is not running
   yet - and shorter after a working one went away, which is more often a
   restart than a decision. */
#define NET_JSON_RETRY_COLD_SECONDS	10
#define NET_JSON_RETRY_WARM_SECONDS	5

/* How long to let a handshake run before giving up on it, well under the TCP
   timeout an unanswering address would otherwise take. */
#define NET_JSON_CONNECT_SECONDS	5

/*
 * ★ Two servers at once, not one.
 *
 * A machine can be on a server of its own AND on the Community Network, so
 * everything about a connection lives in a JsonLink and there is one per
 * server. Frames from the guest go to every link that is up; frames from any
 * link go to the guest. The links know nothing about each other and retry
 * independently, so one server being down is not the other's problem.
 *
 * The cost is duplication, and it is worth stating plainly rather than
 * discovering: both servers are hubs that copy every frame to every client, so
 * a machine that is on both networks hears a peer that is also on both of them
 * twice. IP tolerates that. Broadcast discovery - ShareFS and Access - shows
 * the same machine from both directions and doubles its traffic. Being on one
 * network is the quiet arrangement; being on both is for reaching two sets of
 * people at once and paying for it.
 */
typedef struct {
	const char *name;	/* for the log, so the two can be told apart */
	const char *host;	/* into config, or the constant; never owned */
	int port;

	int fd;

	/* Retrying, so a machine started before its server, or left running
	   across a server restart, joins the network by itself. */
	int want_connection;		/* configured for this server at startup */
	int ever_connected;		/* which of the two intervals applies */
	time_t next_attempt;
	int failure_logged;		/* so it is said once, not every attempt */
	int connecting;			/* socket open, handshake still running */
	time_t connect_deadline;	/* when to give that handshake up */

	/* Partial line left over from the last read: TCP gives no message
	   boundaries, so a frame can arrive in pieces or several can arrive
	   together. */
	char in[NET_JSON_MAX_LINE * 2];
	size_t in_len;
} JsonLink;

/* fd is set here, not left to zero-initialisation: net_json_close() can be
   called before anything has been opened, and closing fd 0 would take standard
   input with it. */
static JsonLink json_links[NET_JSON_LINK_COUNT] = {
	{ "JSON server", NULL, 0, -1, 0, 0, 0, 0, 0, 0, { 0 }, 0 },
	{ "Community Network", COMMUNITY_NET_HOST, JSON_NET_DEFAULT_PORT,
	  -1, 0, 0, 0, 0, 0, 0, { 0 }, 0 },
};

static const char base64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * base64, as the server's Python produces and consumes.
 *
 * @return length written, not NUL-terminated
 */
static size_t
base64_encode(const uint8_t *in, size_t in_len, char *out)
{
	size_t i, o = 0;

	for (i = 0; i + 2 < in_len; i += 3) {
		const uint32_t v = ((uint32_t) in[i] << 16) |
		                   ((uint32_t) in[i + 1] << 8) | in[i + 2];

		out[o++] = base64_alphabet[(v >> 18) & 0x3f];
		out[o++] = base64_alphabet[(v >> 12) & 0x3f];
		out[o++] = base64_alphabet[(v >> 6) & 0x3f];
		out[o++] = base64_alphabet[v & 0x3f];
	}

	if (i < in_len) {
		const size_t left = in_len - i;
		const uint32_t v = ((uint32_t) in[i] << 16) |
		                   (left > 1 ? ((uint32_t) in[i + 1] << 8) : 0);

		out[o++] = base64_alphabet[(v >> 18) & 0x3f];
		out[o++] = base64_alphabet[(v >> 12) & 0x3f];
		out[o++] = (left > 1) ? base64_alphabet[(v >> 6) & 0x3f] : '=';
		out[o++] = '=';
	}

	return o;
}

/** One base64 character's value, or -1 if it is not one. */
static int
base64_value(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/**
 * @return bytes written, or -1 if the input is not valid base64 or will not fit
 */
static int
base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
	uint32_t acc = 0;
	int bits = 0;
	size_t o = 0;
	size_t i;

	for (i = 0; i < in_len; i++) {
		int v;

		if (in[i] == '=') {
			break;
		}
		v = base64_value(in[i]);
		if (v < 0) {
			return -1;
		}
		acc = (acc << 6) | (uint32_t) v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= out_size) {
				return -1;
			}
			out[o++] = (uint8_t) ((acc >> bits) & 0xff);
		}
	}

	return (int) o;
}

int
net_json_encode(const uint8_t *frame, int frame_len, char *out, size_t out_size)
{
	const int payload_len = frame_len - NET_JSON_HEADER_LEN;
	size_t n;
	int used;

	if (frame == NULL || out == NULL || frame_len < NET_JSON_HEADER_LEN ||
	    frame_len > NET_JSON_MAX_FRAME) {
		return -1;
	}

	/* Worst case: the JSON scaffolding, six three-digit numbers twice over,
	   and four base64 characters per three payload bytes. */
	if (out_size < 160 + ((size_t) payload_len + 2) / 3 * 4 + 2) {
		return -1;
	}

	used = snprintf(out, out_size,
	    "{\"frame_type\": %u, \"src\": [%u, %u, %u, %u, %u, %u], "
	    "\"dst\": [%u, %u, %u, %u, %u, %u], \"data\": \"",
	    (unsigned) ((frame[12] << 8) | frame[13]),
	    frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
	    frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);

	if (used < 0 || (size_t) used >= out_size) {
		return -1;
	}

	n = base64_encode(frame + NET_JSON_HEADER_LEN, (size_t) payload_len,
	    out + used);
	used += (int) n;

	if ((size_t) used + 3 >= out_size) {
		return -1;
	}
	out[used++] = '"';
	out[used++] = '}';
	out[used++] = '\n';

	return used;
}

/**
 * The value of one key in a flat JSON object, without a general parser.
 *
 * The schema is fixed and four keys wide, so this looks for "key" and returns
 * what follows the colon. Enough for what the server sends and no more; a
 * parser would be a great deal of code for four keys that never change.
 *
 * @return pointer to the first character of the value, or NULL
 */
static const char *
json_value_of(const char *line, const char *key)
{
	char quoted[32];
	const char *p;

	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	p = strstr(line, quoted);
	if (p == NULL) {
		return NULL;
	}
	p = strchr(p + strlen(quoted), ':');
	if (p == NULL) {
		return NULL;
	}
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

/**
 * Six integers from a JSON array into a MAC address.
 *
 * @return 0 on success, -1 if it is not six values in range
 */
static int
json_mac_of(const char *line, const char *key, uint8_t *mac)
{
	const char *p = json_value_of(line, key);
	int i;

	if (p == NULL || *p != '[') {
		return -1;
	}
	p++;
	for (i = 0; i < 6; i++) {
		char *end = NULL;
		long v;

		while (*p == ' ' || *p == ',') {
			p++;
		}
		v = strtol(p, &end, 10);
		if (end == p || v < 0 || v > 255) {
			return -1;
		}
		mac[i] = (uint8_t) v;
		p = end;
	}
	return 0;
}

int
net_json_decode(const char *line, uint8_t *out, size_t out_size)
{
	uint8_t dst[6], src[6];
	const char *p;
	const char *end;
	long frame_type;
	char *type_end = NULL;
	int payload;

	if (line == NULL || out == NULL || out_size < NET_JSON_HEADER_LEN) {
		return -1;
	}
	if (json_mac_of(line, "dst", dst) != 0 ||
	    json_mac_of(line, "src", src) != 0) {
		return -1;
	}

	p = json_value_of(line, "frame_type");
	if (p == NULL) {
		return -1;
	}
	frame_type = strtol(p, &type_end, 10);
	if (type_end == p || frame_type < 0 || frame_type > 0xffff) {
		return -1;
	}

	p = json_value_of(line, "data");
	if (p == NULL || *p != '"') {
		return -1;
	}
	p++;
	end = strchr(p, '"');
	if (end == NULL) {
		return -1;
	}

	memcpy(out, dst, 6);
	memcpy(out + 6, src, 6);
	out[12] = (uint8_t) ((frame_type >> 8) & 0xff);
	out[13] = (uint8_t) (frame_type & 0xff);

	payload = base64_decode(p, (size_t) (end - p), out + NET_JSON_HEADER_LEN,
	    out_size - NET_JSON_HEADER_LEN);
	if (payload < 0) {
		return -1;
	}

	return NET_JSON_HEADER_LEN + payload;
}

int
net_json_link_wanted(int link, const char **host, int *port)
{
	if (link == NET_JSON_LINK_COMMUNITY) {
		if (!config.community_net_enabled) {
			return 0;
		}
		if (host != NULL) {
			*host = COMMUNITY_NET_HOST;
		}
		if (port != NULL) {
			*port = JSON_NET_DEFAULT_PORT;
		}
		return 1;
	}

	if (link != NET_JSON_LINK_OWN) {
		return 0;
	}

	/* A server with no host name is not a server. The setting can be on with
	   the field never filled in, which is not an error worth refusing to start
	   over - it simply means this half is not configured. */
	if (!config.json_net_enabled || config.json_net_host[0] == '\0') {
		return 0;
	}
	if (host != NULL) {
		*host = config.json_net_host;
	}
	if (port != NULL) {
		*port = (config.json_net_port > 0 && config.json_net_port <= 65535)
		    ? config.json_net_port : JSON_NET_DEFAULT_PORT;
	}
	return 1;
}

/** Usable, as opposed to open: a handshake still running is neither. */
static int
link_is_connected(const JsonLink *link)
{
	return link->fd >= 0 && !link->connecting;
}

int
net_json_is_connected(void)
{
	int i;

	/* Any of them: one server answering is enough for the guest to be on a
	   network, and net_json_tx() sends to whichever are up. */
	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		if (link_is_connected(&json_links[i])) {
			return 1;
		}
	}
	return 0;
}

int
net_json_is_community_connected(void)
{
	return link_is_connected(&json_links[NET_JSON_LINK_COMMUNITY]);
}

int
net_json_wants_connection(void)
{
	int i;

	/*
	 * Wanting a connection, not having one. This is what takes the machine off
	 * the loopback wire, and it has to stay true while a server is being
	 * retried, or a machine would drift back onto the local wire every time its
	 * server blinked.
	 */
	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		if (json_links[i].want_connection) {
			return 1;
		}
	}
	return 0;
}

/** Arrange for the next attempt, at whichever interval now applies. */
static void
net_json_schedule_retry(JsonLink *link)
{
	link->next_attempt = time(NULL) +
	    (link->ever_connected ? NET_JSON_RETRY_WARM_SECONDS
	                          : NET_JSON_RETRY_COLD_SECONDS);
}

static void net_json_drop(JsonLink *link);

/** The socket is up: settle it and say so. */
static void
net_json_connected(JsonLink *link)
{
	/* One frame per line, each written on its own: Nagle would hold each back
	   waiting for company and add its delay to every frame on the wire. */
	socket_set_nodelay(link->fd);
	link->in_len = 0;
	link->connecting = 0;

	if (link->ever_connected) {
		rpclog("net_json: reconnected to the %s at %s:%d\n", link->name,
		    link->host, link->port);
	} else {
		rpclog("net_json: on the %s at %s:%d; frames go there rather than to "
		       "the loopback wire\n", link->name, link->host, link->port);
	}
	link->ever_connected = 1;
	link->failure_logged = 0;
}

/**
 * Has the handshake finished?
 *
 * The socket becomes writable either way, so the outcome is read from
 * SO_ERROR rather than from writability alone.
 *
 * @return 1 if the connection is now up
 */
static int
net_json_finish_connect(JsonLink *link)
{
	struct timeval tv;
	fd_set wfds;
	int err = 0;
	socklen_t len = sizeof(err);

	FD_ZERO(&wfds);
	FD_SET(link->fd, &wfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select(link->fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
		/* Still running. Give it up once it has had long enough, rather than
		   waiting out the TCP timeout. */
		if (time(NULL) >= link->connect_deadline) {
			if (!link->failure_logged) {
				rpclog("net_json: %s (%s:%d) accepted no connection within %d "
				       "seconds, retrying every %d\n", link->name, link->host,
				    link->port, NET_JSON_CONNECT_SECONDS,
				    link->ever_connected ? NET_JSON_RETRY_WARM_SECONDS
				                         : NET_JSON_RETRY_COLD_SECONDS);
				link->failure_logged = 1;
			}
			net_json_drop(link);
		}
		return 0;
	}

	if (getsockopt(link->fd, SOL_SOCKET, SO_ERROR, (char *) &err, &len) != 0 ||
	    err != 0) {
		if (!link->failure_logged) {
			rpclog("net_json: cannot reach the %s at %s:%d, retrying every %d "
			       "seconds\n", link->name, link->host, link->port,
			    link->ever_connected ? NET_JSON_RETRY_WARM_SECONDS
			                         : NET_JSON_RETRY_COLD_SECONDS);
			link->failure_logged = 1;
		}
		net_json_drop(link);
		return 0;
	}

	net_json_connected(link);
	return 1;
}

/**
 * One attempt at this link's server.
 *
 * @return 0 if the connection is up
 */
static int
net_json_try_connect(JsonLink *link)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL, *rp;
	char port[16];

	snprintf(port, sizeof(port), "%d", link->port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(link->host, port, &hints, &res) != 0) {
		if (!link->failure_logged) {
			rpclog("net_json: cannot resolve %s:%s for the %s, retrying every "
			       "%d seconds\n", link->host, port, link->name,
			    NET_JSON_RETRY_COLD_SECONDS);
			link->failure_logged = 1;
		}
		return -1;
	}

	/*
	 * Non-blocking before connect(), not after.
	 *
	 * This runs on the emulator thread, from resetrpc(), before the guest has
	 * executed an instruction. A blocking connect() to a host that takes the
	 * SYN and never finishes the handshake waits out the TCP timeout - minutes
	 * - and the machine cannot boot until it returns. A refused connection
	 * comes back at once, so a server that is simply not running looked fine
	 * and a wrong address hung the machine until it was killed.
	 */
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		link->fd = (int) socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (link->fd < 0) {
			continue;
		}
		socket_set_nonblocking(link->fd);

		if (connect(link->fd, rp->ai_addr, (int) rp->ai_addrlen) == 0) {
			link->connecting = 0;
			break;
		}
		/* Under way rather than failed: the handshake finishes in its own time
		   and net_json_poll() picks it up. */
		if (sock_errno() == SOCK_EINPROGRESS ||
		    sock_errno() == SOCK_EWOULDBLOCK) {
			link->connecting = 1;
			break;
		}
		closesocket(link->fd);
		link->fd = -1;
	}
	freeaddrinfo(res);

	if (link->fd < 0) {
		if (!link->failure_logged) {
			rpclog("net_json: cannot reach the %s at %s:%s, retrying every %d "
			       "seconds\n", link->name, link->host, port,
			    link->ever_connected ? NET_JSON_RETRY_WARM_SECONDS
			                         : NET_JSON_RETRY_COLD_SECONDS);
			link->failure_logged = 1;
		}
		return -1;
	}

	if (link->connecting) {
		/* The socket exists but is not up yet, so the caller must not treat it
		   as connected. net_json_poll() finishes it or gives it up. */
		link->connect_deadline = time(NULL) + NET_JSON_CONNECT_SECONDS;
		return -1;
	}

	net_json_connected(link);
	return 0;
}

int
net_json_init(void)
{
	int wanted = 0;
	int i;

	net_json_close();

	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		JsonLink *link = &json_links[i];
		const char *host = NULL;
		int port = 0;

		link->want_connection = 0;
		link->ever_connected = 0;
		link->failure_logged = 0;
		link->next_attempt = 0;

		if (!net_json_link_wanted(i, &host, &port)) {
			continue;
		}
		link->host = host;
		link->port = port;

		/*
		 * From here this machine belongs to that server's wire whether or not
		 * it answers, so the caller must not put it on the loopback wire
		 * instead: they are alternatives, and a machine on both receives every
		 * frame twice. A first attempt that fails is therefore still success as
		 * far as the caller is concerned - net_json_poll() keeps trying.
		 */
		link->want_connection = 1;
		wanted++;

		if (net_json_try_connect(link) != 0) {
			net_json_schedule_retry(link);
		}
	}

	if (wanted == 0) {
		return -1;
	}

	rpclog("net_json: this machine is on %s; the direct link to machines "
	       "started on this computer is off, and they are unreachable unless "
	       "they are on the same server. NAT is unaffected.\n",
	    wanted > 1 ? "both a JSON server of its own and the Community Network"
	               : (json_links[NET_JSON_LINK_COMMUNITY].want_connection
	                      ? "the Community Network"
	                      : "a JSON server"));

	if (json_links[NET_JSON_LINK_COMMUNITY].want_connection) {
		/*
		 * Said every time the machine starts, not only when the option is
		 * ticked: this is an open network of strangers with no encryption and
		 * no authentication, and the log is the record that the machine was put
		 * on it. See docs/community-network.md.
		 */
		rpclog("net_json: the Community Network is a public, unencrypted "
		       "network shared with people you do not know. Anything the guest "
		       "shares on it - discs, printers, ShareFS - is reachable by all "
		       "of them.\n");
	}
	return 0;
}

/** Give up this link's connection, but keep wanting one. */
static void
net_json_drop(JsonLink *link)
{
	if (link->fd >= 0) {
		closesocket(link->fd);
		link->fd = -1;
	}
	link->in_len = 0;
	net_json_schedule_retry(link);
}

void
net_json_close(void)
{
	int i;

	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		JsonLink *link = &json_links[i];

		if (link->fd >= 0) {
			closesocket(link->fd);
			link->fd = -1;
		}
		link->in_len = 0;
		link->want_connection = 0;
	}
}

/** Write one encoded line to one link, dropping the link if it will not go. */
static void
net_json_send_line(JsonLink *link, const char *line, int len)
{
	int sent = 0;

	/*
	 * A short write is possible on a stream socket, and half a line would
	 * corrupt the frame after it as well as this one. Loop, and give up on a
	 * real error rather than spinning.
	 */
	while (sent < len) {
		const int n = (int) send(link->fd, line + sent, (size_t) (len - sent),
		    MSG_NOSIGNAL);

		if (n > 0) {
			sent += n;
			continue;
		}
		if (n < 0 && (sock_errno() == SOCK_EWOULDBLOCK ||
		              sock_errno() == SOCK_EAGAIN ||
		              sock_errno() == SOCK_EINTR)) {
			continue;
		}
		rpclog("net_json: the %s has gone; retrying every %d seconds\n",
		    link->name, NET_JSON_RETRY_WARM_SECONDS);
		net_json_drop(link);
		return;
	}
}

void
net_json_tx(const uint8_t *frame, int frame_len)
{
	char line[NET_JSON_MAX_LINE];
	int len = -1;
	int i;

	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		JsonLink *link = &json_links[i];

		/* Not while a handshake is still running: the socket exists but
		   nothing sent down it would arrive. */
		if (!link_is_connected(link)) {
			continue;
		}

		/* Encoded once, on the first link that wants it, and reused: the line
		   is the same for every server. */
		if (len < 0) {
			len = net_json_encode(frame, frame_len, line, sizeof(line));
			if (len < 0) {
				return;
			}
		}
		net_json_send_line(link, line, len);
	}
}

/**
 * Read and deliver what one link has to offer.
 *
 * @return how many frames reached the guest
 */
static int
net_json_poll_link(JsonLink *link)
{
	uint8_t frame[NET_JSON_MAX_FRAME];
	int delivered = 0;
	int lines = 0;

	if (link->fd < 0) {
		/* Waiting for a server to answer. Nothing is logged per attempt: the
		   one failure message has already been written. */
		if (link->want_connection && time(NULL) >= link->next_attempt) {
			if (net_json_try_connect(link) != 0) {
				net_json_schedule_retry(link);
			}
		}
		return 0;
	}

	if (link->connecting && !net_json_finish_connect(link)) {
		return 0;
	}

	for (;;) {
		char *nl;
		int n;

		/* Take what is there, then deal in whole lines. */
		if (link->in_len < sizeof(link->in) - 1) {
			n = (int) recv(link->fd, link->in + link->in_len,
			    sizeof(link->in) - link->in_len - 1, 0);
			if (n > 0) {
				link->in_len += (size_t) n;
			} else if (n == 0) {
				rpclog("net_json: the %s closed the connection; retrying every "
				       "%d seconds\n", link->name, NET_JSON_RETRY_WARM_SECONDS);
				net_json_drop(link);
				return delivered;
			}
		}
		link->in[link->in_len] = '\0';

		nl = memchr(link->in, '\n', link->in_len);
		if (nl == NULL) {
			/*
			 * No whole line. If the buffer is full there never will be
			 * one, so throw it away rather than wedging: a line that long
			 * is not a frame this can carry.
			 */
			if (link->in_len >= sizeof(link->in) - 1) {
				rpclog("net_json: over-long line discarded from the %s\n",
				    link->name);
				link->in_len = 0;
			}
			return delivered;
		}

		*nl = '\0';
		{
			const int len = net_json_decode(link->in, frame, sizeof(frame));

			/* Same filter the loopback wire uses: our address, or a
			   broadcast or multicast. The wires are alternatives to the
			   local one, so there is one right answer to "is this for us"
			   and it lives in one place. */
			/* Before the filter: a machine using our address is
			   talking to somebody else, so its frames would be
			   dropped below without anything noticing. */
			guest_subnet_check_duplicate(frame, len, network_hwaddr);

			if (len >= NET_JSON_HEADER_LEN &&
			    net_switch_frame_is_for_us(frame, len, network_hwaddr)) {
				if (network_nat_inject_packet(frame, len)) {
					delivered++;
				}
			}
		}

		/* Shuffle the rest down. */
		{
			const size_t used = (size_t) (nl - link->in) + 1;

			memmove(link->in, link->in + used, link->in_len - used);
			link->in_len -= used;
		}

		if (++lines >= NET_JSON_POLL_MAX) {
			return delivered;
		}
	}
}

int
net_json_poll(void)
{
	int delivered = 0;
	int i;

	/*
	 * Every link, every poll, and each with its own line budget: one busy
	 * server must not be able to starve the other, and neither must be able to
	 * keep this loop running instead of the emulator.
	 */
	for (i = 0; i < NET_JSON_LINK_COUNT; i++) {
		delivered += net_json_poll_link(&json_links[i]);
	}
	return delivered;
}
