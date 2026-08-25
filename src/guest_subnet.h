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
 * guest_subnet - which /24 the guests on this installation live on.
 *
 * Every emulator used the same hardcoded 10.10.10.0/24, with the guest's own
 * address 10.10.10.10 + net_slot. That is enough to tell apart machines started
 * by ONE RPCEmu on one computer, which is all it ever had to do. It is not
 * enough for a JSON server (net_json.h), where the machines meeting each other
 * were started by different RPCEmus on different computers: each hands its first
 * machine 10.10.10.10, so two machines arrive on one wire with distinct MAC
 * addresses and the same IP. They cannot talk, and nothing says why.
 *
 * So the subnet is a per-installation setting, in rpcemu.cfg rather than in a
 * machine's own configuration: it describes the network every machine here sits
 * on, so it cannot sensibly differ between them.
 *
 * Only the middle two octets are settable, inside 10.0.0.0/8. That is a private
 * range in its own right, so nothing here can collide with a real network the
 * host is on; the mask stays /24 because the relay and SLiRP both assume it.
 */

#ifndef GUEST_SUBNET_H
#define GUEST_SUBNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What every installation used before this was settable, and still what one
 * that has not chosen gets. Nothing is generated behind the user's back: a
 * settings file with no subnet in it means 10.10.10.0/24, on an upgrade and on
 * a fresh install alike, so nobody's guests move under them.
 *
 * A random subnet is offered as a button in the Manager's settings, for the
 * JSON case where two installations need to differ. Choosing it is an act, and
 * what it picks is written to the settings file like any other choice.
 */
#define GUEST_SUBNET_DEFAULT_B	10
#define GUEST_SUBNET_DEFAULT_C	10

/**
 * The network address of the /24, as a host-order 32-bit value.
 *
 * @param b Second octet
 * @param c Third octet
 * @return 10.b.c.0
 */
uint32_t guest_subnet_network(unsigned b, unsigned c);

/**
 * Are these octets usable?
 *
 * Anything 0-255 is, which is the whole of 10.0.0.0/8 - a private range, so
 * there is nothing to protect the user from. The check exists so a value read
 * from a settings file somebody edited by hand cannot put the guests somewhere
 * that is not a network at all.
 *
 * @return 1 if usable
 */
int guest_subnet_valid(unsigned b, unsigned c);

/**
 * Parse "10.b.c.0/24", or the shorter "b.c" the settings file uses.
 *
 * @param text Text to parse
 * @param b    Filled in with the second octet
 * @param c    Filled in with the third octet
 * @return 1 if parsed
 */
int guest_subnet_parse(const char *text, unsigned *b, unsigned *c);

/**
 * Render the octets as the settings file stores them: "b.c".
 *
 * @param b   Second octet
 * @param c   Third octet
 * @param out Buffer
 * @param len Size of out, at least 8
 */
void guest_subnet_format(unsigned b, unsigned c, char *out, size_t len);

/*
 * The addresses within the chosen /24. All of them were constants before, and
 * the relay has its own copies (broadcast_relay.c) that have to agree with
 * these or it stops telling local traffic from external.
 */
uint32_t guest_subnet_gateway(uint32_t network);	/**< .2, the SLiRP host */
uint32_t guest_subnet_dns(uint32_t network);		/**< .3 */
uint32_t guest_subnet_broadcast(uint32_t network);	/**< .255 */

/**
 * The address handed to a guest, one per emulator on this computer.
 *
 * .10 upwards, so slot 0 keeps the .10 every single-machine installation has
 * always had.
 *
 * @param network Network address from guest_subnet_network()
 * @param slot    net_slot_acquire()
 */
uint32_t guest_subnet_guest(uint32_t network, int slot);

#ifdef __cplusplus
}
#endif

#endif /* GUEST_SUBNET_H */
