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
 * guest_subnet - the addresses the guests live on.
 *
 * ONE NETWORK, EVERYWHERE: 100.64.0.0/10, and a guest's address within it is a
 * function of its MAC address. Nothing is configurable and nothing depends on
 * the order machines were started in.
 *
 * ★ WHY A MACHINE'S ADDRESS COMES FROM ITS MAC.
 *
 * The address used to be 10.10.10.10 + a slot claimed when the machine started.
 * The slot is per-computer, so it tells apart machines started by ONE RPCEmu and
 * nothing else. Two machines meeting over a JSON server (net_json.h) were started
 * by different RPCEmus, each handing its first machine 10.10.10.10, so they
 * arrived on one wire with distinct MAC addresses and the same IP - which cannot
 * work, and says nothing about why.
 *
 * Making the network configurable does not fix that. Machines can only reach each
 * other on ONE subnet, so two installations that must talk have to agree on a
 * network anyway, and once they share it they are back to both numbering from
 * .10. Deriving from the MAC removes the agreement instead of moving it: every
 * machine already has a MAC of its own, generated once and kept in its
 * configuration, so every machine everywhere computes to a different address with
 * nothing to configure and nothing to negotiate.
 *
 * ★ WHY 100.64.0.0/10.
 *
 * It has to be one network for every installation, so it has to be somewhere a
 * host's real LAN is unlikely to be. 10.0.0.0/8 is the obvious large private
 * range and the wrong choice: home and office LANs live there, and SLiRP treats
 * anything inside its own network as an alias (tcp_subr.c), so a guest could no
 * longer reach a real 10.x machine on the host's LAN.
 *
 * 100.64.0.0/10 is the shared address space of RFC 6598, set aside for carrier
 * NAT. It is routable-looking but not routed, and vanishingly rare on the LANs
 * this runs on. Its 22 host bits are also what makes the next part work.
 *
 * ★ IT IS A HASH, NOT AN ENCODING.
 *
 * 46 usable MAC bits do not fit in 22 address bits, so two machines CAN compute
 * to one address. The odds are small - about one in a million for two machines,
 * and around 0.1% for a hundred on one wire - but they are not zero, which is why
 * a duplicate has to be detected and reported rather than assumed away.
 */

#ifndef GUEST_SUBNET_H
#define GUEST_SUBNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 100.64.0.0/10, as a host-order 32-bit value, and its mask. */
#define GUEST_NET_ADDR		0x64400000u	/**< 100.64.0.0 */
#define GUEST_NET_MASK		0xffc00000u	/**< /10 */
#define GUEST_NET_BROADCAST	0x647fffffu	/**< 100.127.255.255 */

/*
 * The two addresses SLiRP answers for. They MUST differ: SLiRP special-cases
 * traffic to the nameserver, resolving it through the host's own resolver
 * rather than treating it as ordinary gateway traffic (tcp_subr.c, socket.c,
 * ip_icmp.c), so collapsing them onto one address would break DNS.
 */
#define GUEST_NET_GATEWAY	0x64400001u	/**< 100.64.0.1 */
#define GUEST_NET_DNS		0x64400002u	/**< 100.64.0.2 */

/**
 * The network address, as a host-order 32-bit value.
 *
 * Fixed, and a function only so that the callers read the same way they did
 * when it was configurable.
 */
uint32_t guest_subnet_network(void);

/** The addresses SLiRP answers for, and the broadcast. */
uint32_t guest_subnet_gateway(void);
uint32_t guest_subnet_dns(void);
uint32_t guest_subnet_broadcast(void);

/**
 * Is this address inside the guests' network?
 *
 * @param addr Host-order address
 * @return 1 if it is
 */
int guest_subnet_contains(uint32_t addr);

/**
 * The address for a machine with this MAC address.
 *
 * Stable: the same MAC always gives the same address, on any computer and in
 * any order, so a machine keeps its address across restarts and two
 * installations never have to agree on anything.
 *
 * The reserved addresses are avoided - the network and broadcast of the /10,
 * the gateway and the DNS - so the result is always usable as a host address.
 *
 * @param hwaddr The machine's six-byte MAC address
 * @return Host-order address inside 100.64.0.0/10
 */
uint32_t guest_subnet_guest(const uint8_t hwaddr[6]);

/**
 * Render a host-order address as "a.b.c.d".
 *
 * @param addr Host-order address
 * @param out  Buffer
 * @param len  Size of out, at least 16
 */
void guest_subnet_format(uint32_t addr, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GUEST_SUBNET_H */
