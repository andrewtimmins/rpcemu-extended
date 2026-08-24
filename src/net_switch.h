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
 * net_switch - the wire between machines running on this host.
 *
 * Each emulator runs its own SLiRP, which is a complete TCP/IP stack living
 * inside that one process. Two machines were therefore two networks with no
 * connection between them: they could each reach the outside world and neither
 * could see the other, which defeats ShareFS between them and is one of the main
 * reasons for running two RISC OS machines at once.
 *
 * ★ A hub with no owner, rather than a switch somebody has to run.
 *
 * The obvious design is a switch process that everything connects to, and the
 * awkward questions follow immediately: who starts it, what happens to machines
 * started by hand, what happens when it dies with machines still running, and
 * who restarts it. Putting it in the Manager answers the first and makes the
 * rest worse - a machine started from a shell would not be on the network, and
 * closing the Manager would take the LAN down under machines still using it.
 *
 * There is no need for any of it. Every instance already claims a slot that is
 * its alone (net_slot.h). Give each slot a port, have every instance bind its
 * own and send each frame to all the others, and the result is a hub: no owner,
 * nothing to start, nothing to reconnect to, and a machine started any way at
 * all joins by existing. An instance that dies takes nothing with it but
 * itself.
 *
 * Flooding every frame to every port is what a hub does, and at NET_SLOT_MAX
 * ports it is nine small sends for a frame that has already been copied out of
 * guest memory. Receivers drop what is not addressed to them, exactly as a real
 * card on a hub does.
 *
 * Loopback UDP, because it is the only transport available on all three
 * platforms - a UNIX datagram socket would not exist on Windows. Bound and sent
 * to 127.0.0.1 only, so this LAN cannot leave the machine it is on.
 */

#ifndef NET_SWITCH_H
#define NET_SWITCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One UDP port per slot, on loopback.
 *
 * Below 32768 deliberately, which is the lowest ephemeral range of the three
 * platforms (Linux starts there; Windows and macOS start at 49152). These used
 * to be at 49200, inside all of them, and the consequence was not theoretical:
 * Windows hands out ports from that range and, on a Hyper-V host, also reserves
 * blocks of it at boot, so a fixed port there is sometimes simply unavailable.
 * A CI runner refused three of these at once whilst the TCP claim ports next
 * door were fine, which is what a per-protocol reservation looks like. A user
 * would have seen it as a machine that quietly did not join the others.
 *
 * Declared here rather than in net_switch.c because the test binds these ports
 * by hand to stand in for other machines. It used to carry its own copy of the
 * number with a comment saying it had to agree - which is a thing that agrees
 * until somebody changes one of them.
 */
#define NET_SWITCH_PORT_BASE 19200

/**
 * Join the hub.
 *
 * Binds this instance's port, chosen from the slot claimed by net_slot_acquire().
 * Failing is not fatal: the machine keeps its own network and simply cannot see
 * the others, which is what happened before this existed.
 *
 * @return 0 on success, -1 if this instance could not join
 */
extern int net_switch_init(void);

/**
 * Leave the hub and release the port.
 */
extern void net_switch_close(void);

/**
 * Offer a frame the guest has transmitted to the other machines.
 *
 * Sent to every other slot whether it is occupied or not - an unbound port
 * costs one datagram that nothing receives, which is cheaper than tracking who
 * is present and getting it wrong.
 *
 * @param frame     Complete Ethernet frame
 * @param frame_len Length in bytes
 */
extern void net_switch_tx(const uint8_t *frame, int frame_len);

/**
 * Deliver frames from the other machines into this guest.
 *
 * Frames not addressed to this machine are dropped here rather than given to
 * the guest, the same way a card ignores what is not for it. Call from the
 * network poll.
 *
 * @return the number of frames delivered
 */
extern int net_switch_poll(void);

/**
 * Whether this frame is for us: our own address, or a broadcast/multicast.
 *
 * Exposed for the tests; the poll uses it.
 *
 * @param frame     Complete Ethernet frame
 * @param frame_len Length in bytes
 * @param our_mac   This machine's six-byte address
 * @return          1 if the frame should be delivered, 0 if it should be dropped
 */
extern int net_switch_frame_is_for_us(const uint8_t *frame, int frame_len,
                                      const unsigned char *our_mac);

#ifdef __cplusplus
}
#endif

#endif /* NET_SWITCH_H */
