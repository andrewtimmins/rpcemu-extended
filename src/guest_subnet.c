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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"

#include "guest_subnet.h"

uint32_t
guest_subnet_network(void)
{
	return GUEST_NET_ADDR;
}

uint32_t
guest_subnet_gateway(void)
{
	return GUEST_NET_GATEWAY;
}

uint32_t
guest_subnet_dns(void)
{
	return GUEST_NET_DNS;
}

uint32_t
guest_subnet_broadcast(void)
{
	return GUEST_NET_BROADCAST;
}

int
guest_subnet_contains(uint32_t addr)
{
	return (addr & GUEST_NET_MASK) == GUEST_NET_ADDR;
}

/*
 * FNV-1a over the six MAC bytes.
 *
 * Chosen for being fixed by its own definition rather than for its statistics:
 * two installations have to compute the SAME address from the same MAC, so the
 * function cannot be allowed to vary with the compiler, the platform, or a
 * library version. Anything from <stdlib.h> could; this cannot.
 */
static uint32_t
mac_hash(const uint8_t hwaddr[6])
{
	uint32_t h = 2166136261u;	/* FNV offset basis */
	unsigned i;

	for (i = 0; i < 6; i++) {
		h ^= (uint32_t) hwaddr[i];
		h *= 16777619u;		/* FNV prime */
	}
	return h;
}

uint32_t
guest_subnet_guest(const uint8_t hwaddr[6])
{
	/* 22 bits of host space, less the four addresses that are already spoken
	   for, so the hash is folded into what is actually assignable and then
	   stepped past them. */
	const uint32_t host_bits = ~GUEST_NET_MASK;		/* 0x003fffff */
	uint32_t host;

	assert(hwaddr != NULL);

	host = mac_hash(hwaddr) & host_bits;

	/*
	 * .0 is the network, .1 the gateway, .2 the DNS and the top address the
	 * broadcast. Moved rather than re-hashed: a second hash on collision would
	 * make the address depend on which addresses happened to be reserved, and
	 * this way the mapping stays a plain function of the MAC.
	 */
	if (host < 3) {
		host += 3;
	} else if (host == host_bits) {
		host = host_bits - 1;
	}

	return GUEST_NET_ADDR | host;
}

void
guest_subnet_format(uint32_t addr, char *out, size_t len)
{
	assert(out != NULL);
	assert(len >= 16);

	snprintf(out, len, "%u.%u.%u.%u",
	    (addr >> 24) & 0xffu, (addr >> 16) & 0xffu,
	    (addr >> 8) & 0xffu, addr & 0xffu);
}

/*
 * Somebody else answering to our address.
 *
 * The address is a hash of the MAC, so two machines CAN land on one address.
 * It is unlikely - about one in a million for two machines - but the failure it
 * causes is the silent one this whole scheme exists to remove, so it is worth
 * the few lines to say so out loud.
 *
 * ARP specifically, because it is the one frame that states an address on behalf
 * of its sender. Every frame is offered rather than only those addressed to this
 * machine: a duplicate is by definition talking to somebody else, and its frames
 * would be filtered out before anything else could notice.
 *
 * Reported once. A machine that has collided will keep ARPing, and a line per
 * ARP would bury the rest of the log.
 */
static int duplicate_reported = 0;

void
guest_subnet_check_duplicate(const uint8_t *frame, int frame_len,
    const uint8_t hwaddr[6])
{
	uint32_t sender_ip;

	if (duplicate_reported || frame == NULL || hwaddr == NULL ||
	    frame_len < 42) {			/* 14 Ethernet + 28 ARP */
		return;
	}

	if (frame[12] != 0x08 || frame[13] != 0x06) {	/* not ARP */
		return;
	}

	/* Sender hardware address at 22, sender IP at 28. Ours is not a duplicate
	   of itself, and a frame the server reflected would say so. */
	if (memcmp(frame + 22, hwaddr, 6) == 0) {
		return;
	}

	sender_ip = ((uint32_t) frame[28] << 24) | ((uint32_t) frame[29] << 16) |
	            ((uint32_t) frame[30] << 8) | (uint32_t) frame[31];

	if (sender_ip != guest_subnet_guest(hwaddr)) {
		return;
	}

	{
		char addr_text[16];

		guest_subnet_format(sender_ip, addr_text, sizeof(addr_text));
		rpclog("Networking: another machine on this network is using %s, this "
		       "machine's address (it is %02x:%02x:%02x:%02x:%02x:%02x). "
		       "Neither will work properly until one of them is given a "
		       "different MAC address.\n",
		    addr_text, frame[22], frame[23], frame[24], frame[25],
		    frame[26], frame[27]);
		duplicate_reported = 1;
	}
}
