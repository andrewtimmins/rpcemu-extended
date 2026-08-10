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

#include "socket-compat.h"

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

static int json_fd = -1;

/* Partial line left over from the last read: TCP gives no message boundaries,
   so a frame can arrive in pieces or several can arrive together. */
static char json_in[NET_JSON_MAX_LINE * 2];
static size_t json_in_len;

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
net_json_is_connected(void)
{
	return json_fd >= 0;
}

int
net_json_init(void)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL, *rp;
	char port[16];

	net_json_close();

	if (!config.json_net_enabled || config.json_net_host[0] == '\0') {
		return -1;
	}

	snprintf(port, sizeof(port), "%d",
	    config.json_net_port > 0 ? config.json_net_port : 33445);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(config.json_net_host, port, &hints, &res) != 0) {
		rpclog("net_json: cannot resolve %s:%s, this machine is not on that "
		       "network\n", config.json_net_host, port);
		return -1;
	}

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		json_fd = (int) socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (json_fd < 0) {
			continue;
		}
		if (connect(json_fd, rp->ai_addr, (int) rp->ai_addrlen) == 0) {
			break;
		}
		closesocket(json_fd);
		json_fd = -1;
	}
	freeaddrinfo(res);

	if (json_fd < 0) {
		rpclog("net_json: cannot reach %s:%s, this machine is not on that "
		       "network\n", config.json_net_host, port);
		return -1;
	}

	socket_set_nonblocking(json_fd);
	json_in_len = 0;

	rpclog("net_json: on %s:%s; frames go there rather than to the loopback "
	       "wire\n", config.json_net_host, port);
	return 0;
}

void
net_json_close(void)
{
	if (json_fd >= 0) {
		closesocket(json_fd);
		json_fd = -1;
	}
	json_in_len = 0;
}

void
net_json_tx(const uint8_t *frame, int frame_len)
{
	char line[NET_JSON_MAX_LINE];
	int len;
	int sent = 0;

	if (json_fd < 0) {
		return;
	}

	len = net_json_encode(frame, frame_len, line, sizeof(line));
	if (len < 0) {
		return;
	}

	/*
	 * A short write is possible on a stream socket, and half a line would
	 * corrupt the frame after it as well as this one. Loop, and give up on a
	 * real error rather than spinning.
	 */
	while (sent < len) {
		const int n = (int) send(json_fd, line + sent, (size_t) (len - sent),
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
		rpclog("net_json: the server has gone; this machine is off that "
		       "network\n");
		net_json_close();
		return;
	}
}

int
net_json_poll(void)
{
	uint8_t frame[NET_JSON_MAX_FRAME];
	int delivered = 0;
	int lines = 0;

	if (json_fd < 0) {
		return 0;
	}

	for (;;) {
		char *nl;
		int n;

		/* Take what is there, then deal in whole lines. */
		if (json_in_len < sizeof(json_in) - 1) {
			n = (int) recv(json_fd, json_in + json_in_len,
			    sizeof(json_in) - json_in_len - 1, 0);
			if (n > 0) {
				json_in_len += (size_t) n;
			} else if (n == 0) {
				rpclog("net_json: the server closed the connection\n");
				net_json_close();
				return delivered;
			}
		}
		json_in[json_in_len] = '\0';

		nl = memchr(json_in, '\n', json_in_len);
		if (nl == NULL) {
			/*
			 * No whole line. If the buffer is full there never will be
			 * one, so throw it away rather than wedging: a line that long
			 * is not a frame this can carry.
			 */
			if (json_in_len >= sizeof(json_in) - 1) {
				rpclog("net_json: over-long line discarded\n");
				json_in_len = 0;
			}
			return delivered;
		}

		*nl = '\0';
		{
			const int len = net_json_decode(json_in, frame, sizeof(frame));

			/* Same filter the loopback wire uses: our address, or a
			   broadcast or multicast. The two wires are alternatives,
			   so there is one right answer to "is this for us" and it
			   lives in one place. */
			if (len >= NET_JSON_HEADER_LEN &&
			    net_switch_frame_is_for_us(frame, len, network_hwaddr)) {
				if (network_nat_inject_packet(frame, len)) {
					delivered++;
				}
			}
		}

		/* Shuffle the rest down. */
		{
			const size_t used = (size_t) (nl - json_in) + 1;

			memmove(json_in, json_in + used, json_in_len - used);
			json_in_len -= used;
		}

		if (++lines >= NET_JSON_POLL_MAX) {
			return delivered;
		}
	}
}
