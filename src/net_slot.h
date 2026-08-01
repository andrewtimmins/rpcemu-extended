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
 * net_slot - a number that is this emulator's alone, for addressing its guest.
 *
 * Every instance ran its own NAT with the same constants, so every guest was
 * handed 10.10.10.10 by DHCP, and a guest with no MAC address configured got the
 * same hardcoded 06:02:03:04:05:06 as every other. One machine at a time, that is
 * invisible. Two machines are then duplicate addresses on what is meant to become
 * one LAN, and no amount of carrying frames between them can fix that: they cannot
 * talk to each other because they are not distinct hosts.
 *
 * A slot is claimed by binding a loopback TCP port, one port per slot. There is no
 * file, no registry and nothing to clean up, because the operating system drops the
 * binding when the process dies however it dies - the same reason the machine lock
 * and the relay ownership claim work that way. The port is bound and then simply
 * held for the life of the process; nothing ever connects to it.
 *
 * The range is deliberately small. Every running machine costs a core and its own
 * guest RAM, so a limit of ten is far past what anyone will start, and a small fixed
 * range keeps the slot derivable from the port with no negotiation.
 *
 * Slot 0 keeps exactly the addresses that were used before, so a single machine -
 * still the ordinary case - behaves identically to older versions.
 */

#ifndef NET_SLOT_H
#define NET_SLOT_H

#ifdef __cplusplus
extern "C" {
#endif

/** How many machines can be distinctly addressed at once. */
#define NET_SLOT_MAX 10

/**
 * Claim a slot for this process, or return the one already claimed.
 *
 * @return 0 to NET_SLOT_MAX-1. Falls back to 0 if no slot could be claimed, because
 *         the addresses slot 0 produces are the ones used before slots existed:
 *         being unable to claim must not stop a machine from starting.
 */
extern int net_slot_acquire(void);

/**
 * The slot claimed earlier, without claiming one.
 *
 * @return the slot, or -1 if net_slot_acquire() has not been called.
 */
extern int net_slot_get(void);

/**
 * Give it up. Safe to call when nothing was claimed.
 *
 * For tidiness on an orderly exit; the operating system would do it anyway.
 */
extern void net_slot_release(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_SLOT_H */
