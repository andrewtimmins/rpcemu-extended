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
 * test_ip_reass.c - SLiRP IP fragment reassembly
 *
 * Acorn Access/ShareFS fragments bulk writes that exceed the Ethernet MTU, so
 * the guest drives SLiRP's fragment reassembly hard during a large copy. Two
 * upstream fixes are backported into src/slirp/ip_input.c for that path (see the
 * provenance note in that file); this exercises both.
 *
 * Each datagram is addressed to the BOOTP server port with a valid BOOTP
 * request opcode, so SLiRP answers a correctly reassembled datagram with a
 * reply. Counting replies therefore tests reassembly end to end without needing
 * to reach into SLiRP's internals: if reassembly hands back the wrong pointer,
 * no reply is produced.
 *
 * Worth running under AddressSanitizer as well, which is what catches the
 * released-mbuf accesses directly. Configure the build with
 * -DCMAKE_C_FLAGS=-fsanitize=address and run this test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "slirp/libslirp.h"

extern void ip_slowtimo(Slirp *);

#define ETH_HLEN_T    14
#define IP_HDR_LEN_T  20
#define IP_PROTO_UDP_T 17

#define GUEST_IP      0x0a0a0a0a  /* 10.10.10.10, the DHCP address */
#define SLIRP_HOST_IP 0x0a0a0a02  /* 10.10.10.2 */

#define BOOTP_CLIENT_PORT 68
#define BOOTP_SERVER_PORT 67
#define BOOTP_REQUEST     1

static int reply_count;
static int failures;

/* SLiRP delivers frames to the guest through these */
void
slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
	(void) opaque;
	(void) pkt;
	(void) pkt_len;
	reply_count++;
}

int
slirp_can_output(void *opaque)
{
	(void) opaque;
	return 1;
}

void
rpclog(const char *format, ...)
{
	(void) format;
}

static void
check(int cond, const char *what)
{
	printf("  %-56s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

/**
 * Build one Ethernet frame carrying a single IP fragment of a UDP datagram
 * aimed at the BOOTP server port.
 *
 * @param frame       Buffer to build into
 * @param ip_id       IP identification, distinguishing datagrams
 * @param frag_offset Offset of this fragment within the IP payload, in bytes
 * @param more_frags  Non-zero to set the More Fragments flag
 * @param frag_len    Bytes of IP payload in this fragment
 *
 * @return Length of the built frame
 */
static int
build_fragment(uint8_t *frame, uint16_t ip_id, uint32_t frag_offset,
               int more_frags, int frag_len)
{
	uint8_t *ip = frame + ETH_HLEN_T;
	uint8_t *body = ip + IP_HDR_LEN_T;
	uint16_t flags_frag;
	uint32_t sum;
	int i;

	/* Ethernet: unicast to the virtual gateway, from the guest */
	memset(frame, 0, ETH_HLEN_T);
	frame[0] = 0x52; frame[1] = 0x54;
	frame[6] = 0x06; frame[7] = 0x02;
	frame[12] = 0x08; frame[13] = 0x00;

	for (i = 0; i < frag_len; i++) {
		body[i] = (uint8_t) (i + frag_offset);
	}

	if (frag_offset == 0) {
		/* UDP header, then enough of a BOOTP request to draw a reply */
		body[0] = BOOTP_CLIENT_PORT >> 8; body[1] = BOOTP_CLIENT_PORT & 0xff;
		body[2] = BOOTP_SERVER_PORT >> 8; body[3] = BOOTP_SERVER_PORT & 0xff;
		body[4] = 0; body[5] = 8;   /* Length, not checked on this path */
		body[6] = 0; body[7] = 0;   /* No checksum */
		body[8] = BOOTP_REQUEST;
		body[9] = 1;                /* htype: Ethernet */
		body[10] = 6;               /* hlen */
		body[11] = 0;               /* hops */
	}

	ip[0] = 0x45;
	ip[1] = 0;
	ip[2] = (uint8_t) ((IP_HDR_LEN_T + frag_len) >> 8);
	ip[3] = (uint8_t) ((IP_HDR_LEN_T + frag_len) & 0xff);
	ip[4] = (uint8_t) (ip_id >> 8);
	ip[5] = (uint8_t) (ip_id & 0xff);
	flags_frag = (uint16_t) ((frag_offset / 8) & 0x1fff);
	if (more_frags) {
		flags_frag |= 0x2000;
	}
	ip[6] = (uint8_t) (flags_frag >> 8);
	ip[7] = (uint8_t) (flags_frag & 0xff);
	ip[8] = 64;
	ip[9] = IP_PROTO_UDP_T;
	ip[10] = 0; ip[11] = 0;
	ip[12] = (GUEST_IP >> 24) & 0xff; ip[13] = (GUEST_IP >> 16) & 0xff;
	ip[14] = (GUEST_IP >> 8) & 0xff;  ip[15] = GUEST_IP & 0xff;
	ip[16] = (SLIRP_HOST_IP >> 24) & 0xff; ip[17] = (SLIRP_HOST_IP >> 16) & 0xff;
	ip[18] = (SLIRP_HOST_IP >> 8) & 0xff;  ip[19] = SLIRP_HOST_IP & 0xff;

	sum = 0;
	for (i = 0; i < IP_HDR_LEN_T; i += 2) {
		sum += (uint32_t) ((ip[i] << 8) | ip[i + 1]);
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	sum = ~sum;
	ip[10] = (uint8_t) (sum >> 8);
	ip[11] = (uint8_t) (sum & 0xff);

	return ETH_HLEN_T + IP_HDR_LEN_T + frag_len;
}

int
main(void)
{
	static uint8_t frame[8192];
	struct in_addr net, mask, host, dhcp, dns;
	Slirp *slirp;
	int len, i, id;

	net.s_addr = htonl(0x0a0a0a00);
	mask.s_addr = htonl(0xffffff00);
	host.s_addr = htonl(SLIRP_HOST_IP);
	dhcp.s_addr = htonl(GUEST_IP);
	dns.s_addr = htonl(0x0a0a0a03);

	slirp = slirp_init(0, net, mask, host, NULL, "", NULL, dhcp, dns, NULL);
	if (slirp == NULL) {
		printf("slirp_init failed\n");
		return 2;
	}

	/*
	 * Leave a batch of datagrams half reassembled. This pushes the mbuf count
	 * past MBUF_THRESH, after which m_free() really releases an mbuf instead of
	 * parking it on the free list. That is the state a lengthy copy reaches,
	 * and it is what makes touching a released mbuf destructive.
	 */
	printf("incomplete datagrams, to put the mbuf pool past MBUF_THRESH\n");
	reply_count = 0;
	for (id = 0; id < 24; id++) {
		len = build_fragment(frame, (uint16_t) (0x1000 + id), 1480, 1, 1480);
		slirp_input(slirp, frame, len);
	}
	check(reply_count == 0, "no replies from incomplete datagrams");

	/*
	 * Overlapping fragments, as a retransmitting guest produces. A queued
	 * fragment is completely covered by a later one, which drives the
	 * trim-and-dequeue path.
	 */
	printf("overlapping fragments (trim and dequeue path)\n");
	reply_count = 0;
	for (id = 0; id < 40; id++) {
		uint16_t ip_id = (uint16_t) (0x2000 + id);

		len = build_fragment(frame, ip_id, 1480, 1, 8);
		slirp_input(slirp, frame, len);
		len = build_fragment(frame, ip_id, 1480, 1, 1480);
		slirp_input(slirp, frame, len);
		len = build_fragment(frame, ip_id, 2960, 1, 1480);
		slirp_input(slirp, frame, len);
		len = build_fragment(frame, ip_id, 1480, 1, 2960);
		slirp_input(slirp, frame, len);
	}
	check(reply_count == 0, "no replies, none of these datagrams complete");

	/*
	 * Complete large datagrams. Concatenating the fragments grows the first
	 * mbuf beyond its fixed size, so it is promoted to an external buffer and
	 * then reallocated, moving the buffer the fragment list points into.
	 */
	printf("large datagrams completed via external-buffer promotion\n");
	reply_count = 0;
	for (id = 0; id < 20; id++) {
		uint16_t ip_id = (uint16_t) (0x3000 + id);
		uint32_t off;

		for (off = 0; off < 4 * 1480; off += 1480) {
			len = build_fragment(frame, ip_id, off, 1, 1480);
			slirp_input(slirp, frame, len);
		}
		len = build_fragment(frame, ip_id, 4 * 1480, 0, 8);
		slirp_input(slirp, frame, len);
	}
	check(reply_count == 20, "every reassembled datagram was answered");

	/*
	 * The first fragment arrives in a frame larger than a fixed mbuf, so it is
	 * already using an external buffer before reassembly begins.
	 * network_nat_tx() allows frames up to 2048 bytes, so a guest can produce
	 * this. Completing the datagram must re-derive the fragment pointer from
	 * the external buffer, not from the mbuf's built-in one.
	 */
	printf("first fragment oversized (mbuf already external)\n");
	reply_count = 0;
	for (id = 0; id < 20; id++) {
		uint16_t ip_id = (uint16_t) (0x5000 + id);

		len = build_fragment(frame, ip_id, 0, 1, 1600);
		slirp_input(slirp, frame, len);
		len = build_fragment(frame, ip_id, 1600, 0, 8);
		slirp_input(slirp, frame, len);
	}
	check(reply_count == 20, "every reassembled datagram was answered");

	/* Churn: completions, abandoned queues and reassembly timeouts mixed */
	printf("mixed churn with reassembly timeouts\n");
	reply_count = 0;
	for (i = 0; i < 150; i++) {
		uint16_t ip_id = (uint16_t) (0x4000 + i);

		len = build_fragment(frame, ip_id, 0, 1, 1480);
		slirp_input(slirp, frame, len);
		len = build_fragment(frame, ip_id, 1480, 1, 1480);
		slirp_input(slirp, frame, len);
		if ((i % 3) != 0) {
			len = build_fragment(frame, ip_id, 2960, 0, 16);
			slirp_input(slirp, frame, len);
		}
		if ((i % 7) == 0) {
			ip_slowtimo(slirp);
		}
	}
	check(reply_count == 100, "all completed datagrams answered during churn");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
