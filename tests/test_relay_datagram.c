/*
 * The Access relay's inbound datagram path.
 *
 * Two defects live here, both of which corrupted or stalled large ShareFS
 * transfers and neither of which reported anything:
 *
 * 1. The receive buffer was 8192 bytes, with a comment saying that avoided
 *    truncation. ShareFS sends bulk file data as 8200-byte UDP payloads, and
 *    recvfrom() on a datagram socket copies what fits and discards the rest,
 *    returning the buffer size - so every data datagram silently lost its last
 *    8 bytes and the guest filled the hole with whatever its buffer held. A
 *    64MB file arrived at the right length, with no error, and around 6000
 *    corrupt 8-byte runs in it.
 *
 * 2. The fragmenting injector forged the source address as the NAT gateway
 *    while the unfragmented path used the real sender. A guest answering such
 *    a datagram addresses its reply inside the NAT network, so the relay
 *    classifies it as internal, hands it to SLiRP, and the peer never hears.
 *    Every Access datagram over 1472 bytes takes that path.
 *
 * broadcast_relay.c is included rather than linked, because both the buffer and
 * the injector are static. The alternative - reimplementing the read and the
 * fragmenting here - would only test the copy.
 *
 * See src/broadcast_relay.c and docs/network.md.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket-compat.h"

/* --- the bits of the emulator the relay leans on, stubbed ---------------- */

unsigned char network_hwaddr[6] = {0x06, 0x02, 0x03, 0x04, 0x05, 0x06};
#include "rpcemu.h"
#include "guest_subnet.h"
Config config;


/* Frames the relay handed to the guest, in the order it handed them over. */
#define MAX_CAPTURED 64
static struct {
	unsigned char data[2048];
	int len;
} captured[MAX_CAPTURED];
static int captured_count;
static int inject_space = MAX_CAPTURED;

int
network_nat_inject_packet(const uint8_t *pkt, int pkt_len)
{
	if (captured_count >= MAX_CAPTURED || pkt_len > (int) sizeof(captured[0].data)) {
		return 0;
	}
	memcpy(captured[captured_count].data, pkt, (size_t) pkt_len);
	captured[captured_count].len = pkt_len;
	captured_count++;
	return 1;
}

int
network_nat_inject_space(void)
{
	return inject_space - captured_count;
}

void
net_switch_tx(const uint8_t *frame, int frame_len)
{
	(void) frame;
	(void) frame_len;
}

void
rpclog(const char *format, ...)
{
	(void) format;
}

#include "broadcast_relay.c"

/* --- helpers ------------------------------------------------------------- */

static int failures;

static void
check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void
checkf(int cond, const char *fmt, ...)
{
	if (!cond) {
		va_list ap;
		printf("FAIL: ");
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		printf("\n");
		failures++;
	}
}

/*
 * The largest UDP payload IP can carry: 65535 total, less a 20-byte IP header
 * and an 8-byte UDP header. A receive buffer smaller than this truncates some
 * legal datagram, and truncation on a datagram socket is silent.
 */
#define MAX_LEGAL_UDP_PAYLOAD 65507

static void
test_receive_buffer_cannot_truncate(void)
{
	/* Reads the size off the buffer the relay actually receives into. */
	const size_t capacity = broadcast_relay_recv_capacity();

	checkf(capacity >= MAX_LEGAL_UDP_PAYLOAD,
	    "receive buffer is %zu bytes, which truncates any datagram above that; "
	    "the largest a peer may legally send is %d",
	    capacity, MAX_LEGAL_UDP_PAYLOAD);

	/* Named separately because this is the size that actually bit us, and a
	   regression to any value below it corrupts every ShareFS transfer. */
	checkf(capacity >= 8200,
	    "receive buffer is %zu bytes; ShareFS bulk data is 8200-byte payloads",
	    capacity);
}

/*
 * Reassemble what the injector produced and compare it with what went in.
 * Checks the whole contract at once: every byte, once, in order, from the right
 * address, with fragment offsets and the More Fragments flag consistent.
 */
static void
test_fragmented_injection_is_faithful(int payload_len)
{
	static unsigned char payload[MAX_LEGAL_UDP_PAYLOAD];
	static unsigned char rebuilt[MAX_LEGAL_UDP_PAYLOAD + 8];
	struct sockaddr_in from;
	const uint32_t peer = 0xc0a83c01; /* 192.168.60.1, a peer on the LAN */
	int i;
	int frags;
	int expect_offset = 0;
	int rebuilt_len = 0;
	int saw_last = 0;

	captured_count = 0;
	inject_space = MAX_CAPTURED;
	relay.enabled = 1;
	relay.guest_ip = guest_subnet_guest(network_hwaddr); /* learned from the guest */

	for (i = 0; i < payload_len; i++) {
		payload[i] = (unsigned char) (i * 7 + (i >> 8));
	}

	memset(&from, 0, sizeof(from));
	from.sin_family = AF_INET;
	from.sin_addr.s_addr = htonl(peer);
	from.sin_port = htons(49171);

	frags = inject_fragmented_udp(&from, 49171, payload, payload_len, 0);

	checkf(frags == captured_count,
	    "%d-byte payload: injector reported %d fragments, delivered %d",
	    payload_len, frags, captured_count);
	checkf(frags > 1, "%d-byte payload should need more than one fragment, got %d",
	    payload_len, frags);

	for (i = 0; i < captured_count; i++) {
		const unsigned char *f = captured[i].data;
		const unsigned char *ip = f + 14;
		int ihl;
		int total_len;
		int off;
		int mf;
		uint32_t src;
		uint32_t dst;

		checkf(captured[i].len >= 34, "fragment %d is only %d bytes", i,
		    captured[i].len);
		check(f[12] == 0x08 && f[13] == 0x00, "fragment is not IPv4");

		ihl = (ip[0] & 0x0f) * 4;
		total_len = (ip[2] << 8) | ip[3];
		off = (((ip[6] << 8) | ip[7]) & 0x1fff) * 8;
		mf = (((ip[6] << 8) | ip[7]) & 0x2000) != 0;
		src = ((uint32_t) ip[12] << 24) | ((uint32_t) ip[13] << 16) |
		      ((uint32_t) ip[14] << 8) | (uint32_t) ip[15];
		dst = ((uint32_t) ip[16] << 24) | ((uint32_t) ip[17] << 16) |
		      ((uint32_t) ip[18] << 8) | (uint32_t) ip[19];

		checkf(src == peer,
		    "fragment %d claims source %u.%u.%u.%u, not the peer that sent it; "
		    "a guest replying to that address never reaches the peer",
		    i, (src >> 24) & 0xff, (src >> 16) & 0xff, (src >> 8) & 0xff,
		    src & 0xff);

		/* The guest's own address, on the guests' network. A relay that
		   addressed these anywhere else would deliver nothing, and the
		   addresses move whenever guest_subnet.h does. */
		checkf(dst == relay.guest_ip,
		    "fragment %d is addressed to %u.%u.%u.%u, not to the guest",
		    i, (dst >> 24) & 0xff, (dst >> 16) & 0xff, (dst >> 8) & 0xff,
		    dst & 0xff);
		check(guest_subnet_contains(dst),
		    "the guest's address is not on the guests' network");

		checkf(off == expect_offset,
		    "fragment %d is at offset %d, expected %d", i, off, expect_offset);
		checkf(total_len == captured[i].len - 14,
		    "fragment %d IP total_len %d does not match its frame length %d",
		    i, total_len, captured[i].len - 14);

		memcpy(rebuilt + off, ip + ihl, (size_t) (total_len - ihl));
		if (off + (total_len - ihl) > rebuilt_len) {
			rebuilt_len = off + (total_len - ihl);
		}
		expect_offset = off + (total_len - ihl);

		if (!mf) {
			checkf(i == captured_count - 1,
			    "fragment %d has More Fragments clear but is not the last", i);
			saw_last = 1;
		}
	}
	check(saw_last, "no fragment had the More Fragments flag clear");

	/* The reassembled IP payload is the UDP header followed by the payload. */
	checkf(rebuilt_len == payload_len + 8,
	    "%d-byte payload reassembled to %d bytes of IP payload, expected %d",
	    payload_len, rebuilt_len, payload_len + 8);
	checkf(((rebuilt[4] << 8) | rebuilt[5]) == payload_len + 8,
	    "UDP length field says %d, expected %d",
	    (rebuilt[4] << 8) | rebuilt[5], payload_len + 8);
	checkf(memcmp(rebuilt + 8, payload, (size_t) payload_len) == 0,
	    "%d-byte payload came back changed", payload_len);
}

/*
 * A datagram needing more fragments than the guest can take must be dropped
 * whole. Delivering some of it leaves the guest unable to reassemble anything
 * and stalls the transfer, which is what broke large copies before.
 */
static void
test_partial_datagram_is_not_delivered(void)
{
	static unsigned char payload[8200];
	struct sockaddr_in from;
	int frags;

	captured_count = 0;
	inject_space = 3; /* fewer than the six an 8200-byte payload needs */
	relay.enabled = 1;
	relay.guest_ip = guest_subnet_guest(network_hwaddr);

	memset(payload, 0xa5, sizeof(payload));
	memset(&from, 0, sizeof(from));
	from.sin_family = AF_INET;
	from.sin_addr.s_addr = htonl(0xc0a83c01);
	from.sin_port = htons(49171);

	frags = inject_fragmented_udp(&from, 49171, payload, (int) sizeof(payload), 0);

	check(frags == 0, "a datagram that does not fit should inject nothing");
	checkf(captured_count == 0,
	    "%d fragments of an undeliverable datagram reached the guest",
	    captured_count);
}

/*
 * A broadcast goes to the guests' broadcast address, not to one guest.
 *
 * This is the path Access+ discovery takes inbound, and it is the one the
 * addressing change could break quietly: the guest accepts the frame only if
 * the address matches the broadcast for the netmask its DHCP lease gave it.
 */
static void
test_broadcast_is_addressed_to_the_broadcast_address(void)
{
	struct sockaddr_in from;
	unsigned char payload[64];
	int frags;

	captured_count = 0;
	inject_space = MAX_CAPTURED;
	relay.enabled = 1;
	relay.guest_ip = guest_subnet_guest(network_hwaddr);

	memset(payload, 0xa5, sizeof(payload));
	memset(&from, 0, sizeof(from));
	from.sin_family = AF_INET;
	from.sin_addr.s_addr = htonl(0xc0a80132u);	/* 192.168.1.50, a real peer */
	from.sin_port = htons(32770);

	frags = inject_fragmented_udp(&from, 32770, payload, sizeof(payload), 1);

	checkf(frags == 1, "a 64-byte broadcast should be one fragment, got %d",
	    frags);
	if (captured_count > 0) {
		const unsigned char *ip = captured[0].data + 14;
		const uint32_t dst = ((uint32_t) ip[16] << 24) |
		                     ((uint32_t) ip[17] << 16) |
		                     ((uint32_t) ip[18] << 8) | (uint32_t) ip[19];

		checkf(dst == guest_subnet_broadcast(),
		    "a broadcast went to %u.%u.%u.%u, not the guests' broadcast "
		    "address; inbound Access+ discovery would not be delivered",
		    (dst >> 24) & 0xff, (dst >> 16) & 0xff, (dst >> 8) & 0xff,
		    dst & 0xff);
		check(memcmp(captured[0].data, "\xff\xff\xff\xff\xff\xff", 6) == 0,
		    "a broadcast was not sent to the broadcast MAC address");
	}
}

int
main(void)
{
#ifdef _WIN32
	{
		WSADATA wsadata;

		if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
			printf("WSAStartup failed\n");
			return 1;
		}
	}
#endif

	test_receive_buffer_cannot_truncate();

	/* 8200 is what ShareFS actually sends; the others are the boundaries -
	   just over one fragment, and the largest datagram that is legal at all. */
	test_fragmented_injection_is_faithful(8200);
	test_fragmented_injection_is_faithful(1473);
	test_fragmented_injection_is_faithful(MAX_LEGAL_UDP_PAYLOAD);

	test_broadcast_is_addressed_to_the_broadcast_address();

	test_partial_datagram_is_not_delivered();

	if (failures != 0) {
		printf("test_relay_datagram: %d failure%s\n", failures,
		    failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_relay_datagram: all checks passed\n");
	return 0;
}
