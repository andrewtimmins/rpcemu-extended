/*
 * The wire between machines running on this host.
 *
 * WHY THIS EXISTS. Two emulated machines could not see each other at all, each
 * sitting behind its own SLiRP, and this is what connects them. The failure it
 * has to catch is a quiet one: a frame that is sent and simply never arrives
 * looks exactly like a guest that has not finished booting, and the thing a
 * user notices is that ShareFS does not find the other machine - several
 * layers away from the cause.
 *
 * The addressing decision is checked here too. A hub floods every frame to
 * every port and the receiving end decides what to keep, so a mistake in that
 * decision either drops traffic that was meant for this machine or hands it
 * everything on the wire. Both are silent.
 *
 * Two real sockets on the loopback, because the point is that a frame put in
 * one end comes out of the other; a test of the decision function alone would
 * not have noticed the ports being wrong.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "socket-compat.h"

#include "net_slot.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* Also for NET_SWITCH_PORT_BASE: the test binds those ports by hand to stand in
   for other machines, so it takes the number from the unit rather than keeping a
   copy of it. It used to keep a copy, with a comment saying the two had to
   agree - which they do, right up until somebody changes one of them. */
#include "net_switch.h"

/* network_hwaddr and network_nat_inject_packet belong to the emulator; the test
   supplies its own so the switch can be exercised without one. */
unsigned char network_hwaddr[6] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x06 };

/* The switch offers frames to the Access relay on their way past; the relay is
   the emulator's, not this unit's, so it is stubbed out. Returning 0 is "not
   taken", which is what a machine without the relay would say. */
int
broadcast_relay_tx(const uint8_t *pkt, int pkt_len)
{
	(void) pkt;
	(void) pkt_len;
	return 0;
}

static unsigned char injected[1522];
static int injected_len;
static int injected_count;

int
network_nat_inject_packet(const uint8_t *pkt, int pkt_len)
{
	if (pkt_len > 0 && pkt_len <= (int) sizeof(injected)) {
		memcpy(injected, pkt, (size_t) pkt_len);
		injected_len = pkt_len;
		injected_count++;
		return 1;
	}
	return 0;
}

static void
build_frame(unsigned char *frame, const unsigned char *dest)
{
	memset(frame, 0, 64);
	memcpy(frame, dest, 6);
	memcpy(frame + 6, "\x06\x02\x03\x04\x05\x07", 6);
	frame[12] = 0x08;	/* IPv4 */
	frame[13] = 0x00;
}

static void
test_addressing(void)
{
	const unsigned char us[6] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x06 };
	const unsigned char them[6] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x07 };
	const unsigned char broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	const unsigned char multicast[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
	unsigned char frame[64];

	printf("what a machine keeps off the wire\n");

	build_frame(frame, us);
	check("a frame addressed to this machine is kept",
	    net_switch_frame_is_for_us(frame, 64, us) == 1);

	build_frame(frame, them);
	check("a frame for another machine is dropped",
	    net_switch_frame_is_for_us(frame, 64, us) == 0);

	/* ARP and Access+ discovery are broadcasts; dropping them would leave the
	   machines unable to find each other while unicast worked, which is a
	   confusing way to be broken. */
	build_frame(frame, broadcast);
	check("a broadcast is kept",
	    net_switch_frame_is_for_us(frame, 64, us) == 1);

	build_frame(frame, multicast);
	check("a multicast is kept",
	    net_switch_frame_is_for_us(frame, 64, us) == 1);

	build_frame(frame, us);
	check("a runt shorter than an Ethernet header is dropped",
	    net_switch_frame_is_for_us(frame, 13, us) == 0);
	check("a null frame is dropped",
	    net_switch_frame_is_for_us(NULL, 64, us) == 0);
}

/*
 * A frame put in one end comes out of the other.
 *
 * The test claims a slot the way an emulator does, then binds the ports of two
 * OTHER slots by hand to stand in for two more machines, and checks that a
 * transmitted frame reaches both of them - a hub, not a point-to-point link -
 * and that it is not also sent back to the sender.
 */
static void
test_frame_reaches_the_others(void)
{
	int peer[2] = { -1, -1 };
	int our_slot;
	int i;
	int peer_slots[2];
	unsigned char frame[64];
	int got[2] = { 0, 0 };

	printf("\na frame reaches the other machines\n");

	our_slot = net_slot_acquire();
	check("this instance claimed a slot", our_slot >= 0);

	/* Two slots that are not ours. */
	peer_slots[0] = (our_slot + 1) % NET_SLOT_MAX;
	peer_slots[1] = (our_slot + 2) % NET_SLOT_MAX;

	for (i = 0; i < 2; i++) {
		struct sockaddr_in addr;

		peer[i] = (int) socket(AF_INET, SOCK_DGRAM, 0);
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((unsigned short)
		    (NET_SWITCH_PORT_BASE + peer_slots[i]));
		if (bind(peer[i], (struct sockaddr *) &addr, sizeof(addr)) != 0) {
			/* Another emulator on this machine holds it. Say so rather
			   than reporting a failure that is not one. */
			printf("  (slot %d's port is in use - skipping)\n", peer_slots[i]);
			closesocket(peer[i]);
			peer[i] = -1;
			continue;
		}
		socket_set_nonblocking(peer[i]);
	}

	check("the switch joined", net_switch_init() == 0);

	build_frame(frame, (const unsigned char *) "\xff\xff\xff\xff\xff\xff");
	net_switch_tx(frame, 64);

	/* Loopback delivery is immediate, but give it a moment rather than
	   assuming. */
	usleep(50000);

	for (i = 0; i < 2; i++) {
		unsigned char buf[1522];

		if (peer[i] < 0) {
			got[i] = -1;
			continue;
		}
		got[i] = ((int) recv(peer[i], (char *) buf, sizeof(buf), 0) == 64 &&
		    memcmp(buf, frame, 64) == 0) ? 1 : 0;
	}

	if (got[0] >= 0) {
		check("the first machine received it, unchanged", got[0] == 1);
	}
	if (got[1] >= 0) {
		check("the second machine received it too", got[1] == 1);
	}

	/*
	 * Nothing comes back to the sender. A card does not receive its own
	 * transmissions, and a guest that saw its own broadcasts would answer
	 * them - an ARP storm of one machine talking to itself.
	 */
	injected_count = 0;
	net_switch_poll();
	check("the sender does not receive its own frame", injected_count == 0);

	for (i = 0; i < 2; i++) {
		if (peer[i] >= 0) {
			closesocket(peer[i]);
		}
	}
	net_switch_close();
	net_slot_release();
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

	test_addressing();
	test_frame_reaches_the_others();

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
