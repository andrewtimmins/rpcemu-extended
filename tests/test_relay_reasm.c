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
 * test_relay_reasm.c - Access+/ShareFS relay outgoing fragment reassembly
 *
 * broadcast_relay.c collects the fragments of a guest-originated Access+
 * datagram and sends the whole datagram from the socket bound to the right port,
 * so that SLiRP does not reassemble it and re-emit it under a masqueraded
 * source port.
 *
 * The unit under test is included directly so its reassembly state can be
 * inspected, with sendto() intercepted. Nothing is sent and no ports are bound,
 * so this is safe to run alongside a live emulator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Pull in the platform's socket declarations before sendto is redefined below,
   so the real prototype is already in place and the macro only rewrites the
   call inside the unit under test. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#endif

/* Capture what the relay sends, rather than putting it on the network */
static int test_sendto(int fd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest, size_t addrlen);
#define sendto test_sendto

#include "broadcast_relay.c"

#undef sendto

static uint8_t sent_buf[70000];
static int sent_len;
static int sent_count;
static uint16_t sent_port;
static uint32_t sent_addr;

static int
test_sendto(int fd, const void *buf, size_t len, int flags,
            const struct sockaddr *dest, size_t addrlen)
{
	const struct sockaddr_in *d = (const struct sockaddr_in *) dest;

	(void) fd;
	(void) flags;
	(void) addrlen;

	assert(len <= sizeof(sent_buf));
	memcpy(sent_buf, buf, len);
	sent_len = (int) len;
	sent_port = ntohs(d->sin_port);
	sent_addr = ntohl(d->sin_addr.s_addr);
	sent_count++;

	return (int) len;
}

/* Stubs for the rest of the emulator */
unsigned char network_hwaddr[6] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x06 };

void
rpclog(const char *format, ...)
{
	(void) format;
}

int
network_nat_inject_packet(const uint8_t *pkt, int pkt_len)
{
	(void) pkt;
	(void) pkt_len;
	return 1;
}

int
network_nat_inject_space(void)
{
	return 32;
}

#define GUEST_IP 0x0a0a0a0a  /* 10.10.10.10 */
#define PEER_IP  0xc0000213  /* 192.0.2.19, RFC 5737 TEST-NET-1 */

static int failures;

static void
check(int cond, const char *what)
{
	printf("  %-56s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

/**
 * Put the relay into a running state without creating sockets or binding the
 * Access+ ports, which a live emulator on this machine may be using.
 */
static void
relay_test_init(void)
{
	int i;

	memset(&relay, 0, sizeof(relay));
	for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
		relay.sockets[i] = 100 + i; /* Never used, sendto is intercepted */
	}
	relay.enabled = 1;
	relay.host_addr.sin_family = AF_INET;
	relay.host_addr.sin_addr.s_addr = htonl(0xc0a8000c);
	relay.bcast_addr.sin_family = AF_INET;
	relay.bcast_addr.sin_addr.s_addr = htonl(0xc0a800ff);
	reasm_reset();

	sent_count = 0;
	sent_len = 0;
}

/**
 * Build an Ethernet frame holding one IP fragment of a UDP datagram. A UDP
 * header is written only for the fragment at offset zero, as on the wire.
 *
 * @param udp_total Length the UDP header should claim, for the first fragment
 */
static int
build_frame(uint8_t *frame, uint16_t sport, uint16_t dport, uint16_t ip_id,
            uint32_t frag_offset, int more_frags, const uint8_t *payload,
            int payload_len, int udp_total)
{
	uint8_t *ip = frame + ETH_HLEN;
	uint8_t *body = ip + IP_HDR_LEN;
	int ip_payload_len;
	uint16_t flags_frag;

	/* Unicast to the peer's MAC, from the guest */
	memset(frame, 0, ETH_HLEN);
	frame[0] = 0x52; frame[1] = 0x54;
	memcpy(frame + 6, network_hwaddr, ETH_ALEN);
	frame[12] = 0x08; frame[13] = 0x00;

	if (frag_offset == 0) {
		body[0] = (uint8_t) (sport >> 8); body[1] = (uint8_t) (sport & 0xff);
		body[2] = (uint8_t) (dport >> 8); body[3] = (uint8_t) (dport & 0xff);
		body[4] = (uint8_t) (udp_total >> 8);
		body[5] = (uint8_t) (udp_total & 0xff);
		body[6] = 0; body[7] = 0;
		memcpy(body + 8, payload, (size_t) payload_len);
		ip_payload_len = 8 + payload_len;
	} else {
		memcpy(body, payload, (size_t) payload_len);
		ip_payload_len = payload_len;
	}

	ip[0] = 0x45;
	ip[1] = 0;
	ip[2] = (uint8_t) ((IP_HDR_LEN + ip_payload_len) >> 8);
	ip[3] = (uint8_t) ((IP_HDR_LEN + ip_payload_len) & 0xff);
	ip[4] = (uint8_t) (ip_id >> 8);
	ip[5] = (uint8_t) (ip_id & 0xff);
	flags_frag = (uint16_t) ((frag_offset / 8) & 0x1fff);
	if (more_frags) {
		flags_frag |= 0x2000;
	}
	ip[6] = (uint8_t) (flags_frag >> 8);
	ip[7] = (uint8_t) (flags_frag & 0xff);
	ip[8] = 64;
	ip[9] = IP_PROTO_UDP;
	ip[10] = 0; ip[11] = 0;
	ip[12] = (GUEST_IP >> 24) & 0xff; ip[13] = (GUEST_IP >> 16) & 0xff;
	ip[14] = (GUEST_IP >> 8) & 0xff;  ip[15] = GUEST_IP & 0xff;
	ip[16] = (PEER_IP >> 24) & 0xff;  ip[17] = (PEER_IP >> 16) & 0xff;
	ip[18] = (PEER_IP >> 8) & 0xff;   ip[19] = PEER_IP & 0xff;

	return ETH_HLEN + IP_HDR_LEN + ip_payload_len;
}

int
main(void)
{
	static uint8_t frame[4096];
	static uint8_t data[4000];
	int i, len;

	for (i = 0; i < (int) sizeof(data); i++) {
		data[i] = (uint8_t) (i * 7 + 3);
	}

	printf("a 3000-byte ShareFS write split over three fragments\n");
	relay_test_init();
	len = build_frame(frame, 49171, 49171, 0x1234, 0, 1, data, 1472, 8 + 3000);
	check(broadcast_relay_tx(frame, len) == 1, "first fragment taken by relay");
	check(sent_count == 0, "nothing sent yet");
	len = build_frame(frame, 0, 0, 0x1234, 1480, 1, data + 1472, 1480, 0);
	check(broadcast_relay_tx(frame, len) == 1, "second fragment taken");
	check(sent_count == 0, "still nothing sent");
	len = build_frame(frame, 0, 0, 0x1234, 2960, 0, data + 2952, 48, 0);
	check(broadcast_relay_tx(frame, len) == 1, "final fragment taken");
	check(sent_count == 1, "exactly one datagram sent");
	check(sent_len == 3000, "reassembled payload is 3000 bytes");
	check(memcmp(sent_buf, data, 3000) == 0, "reassembled payload matches");
	check(sent_port == 49171, "sent to the Access+ port");
	check(sent_addr == PEER_IP, "sent to the peer");
	check(reasm_lookup(GUEST_IP, PEER_IP, 0x1234) == NULL, "slot released");

	printf("a fragment whose first fragment was never seen\n");
	relay_test_init();
	len = build_frame(frame, 0, 0, 0x2222, 1480, 1, data, 1480, 0);
	check(broadcast_relay_tx(frame, len) == 0, "declined, left to SLiRP");
	check(sent_count == 0, "nothing sent");

	printf("a gap in the fragment sequence\n");
	relay_test_init();
	len = build_frame(frame, 49171, 49171, 0x3333, 0, 1, data, 1472, 8 + 3000);
	check(broadcast_relay_tx(frame, len) == 1, "first fragment taken");
	len = build_frame(frame, 0, 0, 0x3333, 2960, 0, data, 48, 0);
	check(broadcast_relay_tx(frame, len) == 0, "fragment after a gap declined");
	check(sent_count == 0, "no datagram sent with a hole in it");
	check(reasm_lookup(GUEST_IP, PEER_IP, 0x3333) == NULL, "slot discarded");

	printf("fragmented traffic on a port that is not Access+\n");
	relay_test_init();
	len = build_frame(frame, 5000, 6000, 0x4444, 0, 1, data, 1472, 8 + 3000);
	check(broadcast_relay_tx(frame, len) == 0, "first fragment declined");
	len = build_frame(frame, 0, 0, 0x4444, 1480, 0, data, 8, 0);
	check(broadcast_relay_tx(frame, len) == 0, "later fragment declined");
	check(sent_count == 0, "nothing sent");

	printf("a retransmitted first fragment\n");
	relay_test_init();
	len = build_frame(frame, 49171, 49171, 0x5555, 0, 1, data, 1472, 8 + 1520);
	check(broadcast_relay_tx(frame, len) == 1, "first fragment taken");
	check(broadcast_relay_tx(frame, len) == 1, "duplicate first fragment taken");
	len = build_frame(frame, 0, 0, 0x5555, 1480, 0, data + 1472, 48, 0);
	check(broadcast_relay_tx(frame, len) == 1, "final fragment taken");
	check(sent_count == 1, "one datagram sent");
	check(sent_len == 1520, "length correct after the restart");
	check(memcmp(sent_buf, data, 1520) == 0, "payload correct after the restart");

	printf("an unfragmented Access+ unicast\n");
	relay_test_init();
	len = build_frame(frame, 49171, 49171, 0x6666, 0, 0, data, 400, 8 + 400);
	check(broadcast_relay_tx(frame, len) == 1, "relayed, and owned by the relay");
	check(sent_count == 1 && sent_len == 400, "400-byte payload sent");
	check(memcmp(sent_buf, data, 400) == 0, "payload matches");

	printf("a UDP length field longer than the frame\n");
	relay_test_init();
	len = build_frame(frame, 49171, 49171, 0x7777, 0, 0, data, 100, 8 + 1400);
	check(broadcast_relay_tx(frame, len) == 1, "handled");
	check(sent_len == 100, "clamped to the bytes the frame actually holds");

	printf("more datagrams in flight than there are slots\n");
	relay_test_init();
	for (i = 0; i < REASM_SLOTS + 1; i++) {
		len = build_frame(frame, 49171, 49171, (uint16_t) (0x8000 + i), 0, 1,
		                  data, 1472, 8 + 3000);
		check(broadcast_relay_tx(frame, len) == 1, "first fragment taken");
	}
	check(reasm_lookup(GUEST_IP, PEER_IP, (uint16_t) (0x8000 + REASM_SLOTS)) != NULL,
	      "the newest datagram holds a slot");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
