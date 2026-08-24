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

#include <string.h>
#include <time.h>

#include "socket-compat.h"

#include "broadcast_relay.h"
#include "net_slot.h"
#include "net_switch.h"
#include "network.h"
#include "network-nat.h"
#include "rpcemu.h"

/* An Ethernet frame; the guest's card will not produce anything larger. */
#define NET_SWITCH_MAX_FRAME 1522

/* How many frames one poll will take from the socket. A guest that is being
   flooded must not be able to keep this loop running instead of the emulator. */
#define NET_SWITCH_POLL_MAX 64

static int switch_fd = -1;
static int switch_slot = -1;

/*
 * What has crossed the wire, reported now and then.
 *
 * Here because "the other machines cannot see the share" and "the frames are not
 * arriving" are different faults with the same symptom, and until this there was
 * no way to tell them apart: a machine could be receiving every frame and
 * discarding them for its own reasons, or receiving none at all, and the log said
 * nothing either way.
 *
 * Reported at most every ten seconds, and only when something has moved, so an
 * idle machine writes nothing.
 */
/* Which slots hold an emulator, and when that was last established. */
static uint32_t switch_peers;
static time_t switch_peers_checked;

static unsigned long switch_tx_frames;
static unsigned long switch_rx_frames;      /* seen on the wire */
static unsigned long switch_rx_for_us;      /* and addressed to this machine */
static unsigned long switch_reported_tx;
static unsigned long switch_reported_rx;
static time_t switch_last_report;

#define NET_SWITCH_REPORT_SECONDS 10

static void
net_switch_report(void)
{
	const time_t now = time(NULL);

	if (switch_last_report == 0) {
		switch_last_report = now;
		return;
	}
	if (now - switch_last_report < NET_SWITCH_REPORT_SECONDS) {
		return;
	}
	if (switch_tx_frames == switch_reported_tx &&
	    switch_rx_frames == switch_reported_rx) {
		switch_last_report = now;
		return;
	}

	rpclog("net_switch: slot %d - sent %lu frames, received %lu (%lu for this "
	       "machine)\n", switch_slot, switch_tx_frames, switch_rx_frames,
	    switch_rx_for_us);

	switch_reported_tx = switch_tx_frames;
	switch_reported_rx = switch_rx_frames;
	switch_last_report = now;
}

int
net_switch_frame_is_for_us(const uint8_t *frame, int frame_len,
    const unsigned char *our_mac)
{
	if (frame == NULL || our_mac == NULL || frame_len < 14) {
		return 0;
	}

	/*
	 * The low bit of the first address byte is the group bit: set for
	 * broadcast and for every multicast, which covers ARP's ff:ff:ff:ff:ff:ff
	 * and the Access+ discovery traffic ShareFS depends on.
	 */
	if ((frame[0] & 0x01) != 0) {
		return 1;
	}

	return memcmp(frame, our_mac, 6) == 0;
}

int
net_switch_init(void)
{
	struct sockaddr_in addr;
	int slot = net_slot_acquire();

	net_switch_close();

	if (slot < 0 || slot >= NET_SLOT_MAX) {
		return -1;
	}

	switch_fd = (int) socket(AF_INET, SOCK_DGRAM, 0);
	if (switch_fd < 0) {
		rpclog("net_switch: socket() failed, this machine will not see the "
		       "others\n");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((unsigned short) (NET_SWITCH_PORT_BASE + slot));

	if (bind(switch_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		/*
		 * Somebody else holds this slot's port. That should not happen -
		 * the slot was claimed by binding a port of its own - so say so
		 * rather than carrying on half-connected.
		 */
		rpclog("net_switch: cannot bind 127.0.0.1:%d, this machine will not "
		       "see the others\n", NET_SWITCH_PORT_BASE + slot);
		closesocket(switch_fd);
		switch_fd = -1;
		return -1;
	}

	socket_set_nonblocking(switch_fd);
	switch_slot = slot;

	rpclog("net_switch: on 127.0.0.1:%d as slot %d; machines on this host "
	       "can see each other\n", NET_SWITCH_PORT_BASE + slot, slot);
	return 0;
}

void
net_switch_close(void)
{
	if (switch_fd >= 0) {
		closesocket(switch_fd);
		switch_fd = -1;
	}
	switch_slot = -1;
}

/*
 * Which switch ports have a receiver, refreshed at most once a second.
 *
 * A machine appearing is not something an individual frame has to notice, and
 * the probe costs a socket, a bind and a close per empty slot.
 */
static void
switch_refresh_peers(void)
{
	const time_t now = time(NULL);
	int slot;

	if (now == switch_peers_checked) {
		return;
	}
	switch_peers_checked = now;
	switch_peers = 0;

	for (slot = 0; slot < NET_SLOT_MAX; slot++) {
		struct sockaddr_in addr;
		int probe;

		if (slot == switch_slot) {
			continue; /* never sent to, so its state does not matter */
		}

		probe = (int) socket(AF_INET, SOCK_DGRAM, 0);
		if (probe < 0) {
			/* Cannot tell, so assume a receiver rather than go silent. */
			switch_peers |= 1u << slot;
			continue;
		}

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((unsigned short) (NET_SWITCH_PORT_BASE + slot));

		/* Deliberately without SO_REUSEADDR: the bind failing is the signal. */
		if (bind(probe, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
			switch_peers |= 1u << slot;
		}
		closesocket(probe);
	}
}

void
net_switch_tx(const uint8_t *frame, int frame_len)
{
	int slot;

	if (switch_fd < 0 || frame == NULL || frame_len <= 0 ||
	    frame_len > NET_SWITCH_MAX_FRAME) {
		return;
	}

	/*
	 * Only to ports something is actually listening on.
	 *
	 * This used to send to all ten slots unconditionally, so a single machine
	 * with no siblings still paid nine sendto() calls for every frame it sent
	 * and every frame the relay brought in from the network - on the path whose
	 * latency decides ShareFS throughput, because Access acknowledges each
	 * block before the next one is sent.
	 *
	 * The test is a bind of the very port the frame would be sent to, so it
	 * cannot disagree with reality: a port that can be bound has nobody
	 * receiving on it. Anything indirect - asking which slots are claimed, or
	 * keeping a table of who has been heard from - can be wrong in the
	 * direction that makes a running machine silently unreachable.
	 */
	switch_refresh_peers();

	for (slot = 0; slot < NET_SLOT_MAX; slot++) {
		struct sockaddr_in addr;

		/* Not to ourselves: a card does not receive what it transmits, and
		   the guest would see every frame it sent come back at it. */
		if (slot == switch_slot) {
			continue;
		}

		if ((switch_peers & (1u << slot)) == 0) {
			continue;
		}

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((unsigned short) (NET_SWITCH_PORT_BASE + slot));

		/*
		 * Errors are ignored on purpose. Sending to a port nothing is bound
		 * to is the ordinary case - most slots are empty - and on some hosts
		 * that is reported back on a later call as ECONNREFUSED. There is
		 * nothing to do about it and nothing worth logging every frame.
		 */
		(void) sendto(switch_fd, (const char *) frame, (size_t) frame_len, 0,
		    (struct sockaddr *) &addr, sizeof(addr));
	}

	switch_tx_frames++;
	net_switch_report();
}

int
net_switch_poll(void)
{
	uint8_t frame[NET_SWITCH_MAX_FRAME];
	int delivered = 0;
	int i;

	if (switch_fd < 0) {
		return 0;
	}

	for (i = 0; i < NET_SWITCH_POLL_MAX; i++) {
		const int len = (int) recv(switch_fd, (char *) frame, sizeof(frame), 0);

		if (len <= 0) {
			break;
		}
		/*
		 * ★ Offered to the Access relay before it is filtered.
		 *
		 * The relay bridges Access+ to the real network and only one instance
		 * can hold its ports, so on every other machine those broadcasts had
		 * nowhere to go: ShareFS between two emulated machines worked over
		 * this wire, and neither of them could see a real RISC OS machine
		 * unless it happened to be the one that owned the relay.
		 *
		 * The instance that does own it now relays what arrives from the
		 * others as well, so the whole wire reaches the real network through
		 * whichever machine holds the ports. Offered before the address
		 * filter because the relay's business is other machines' traffic, not
		 * ours.
		 *
		 * It cannot loop: what arrives here is never sent back to the wire.
		 */
		(void) broadcast_relay_tx(frame, len);

		switch_rx_frames++;

		if (!net_switch_frame_is_for_us(frame, len, network_hwaddr)) {
			continue;
		}
		switch_rx_for_us++;
		if (network_nat_inject_packet(frame, len)) {
			delivered++;
		}
	}

	/*
	 * Also from here, not only from the transmit path: a machine that is only
	 * listening - which is exactly the machine whose silence needs explaining -
	 * would otherwise never say a word.
	 */
	net_switch_report();

	return delivered;
}
