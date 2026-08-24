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

#include "socket-compat.h"

#ifdef _WIN32
#define slot_closesocket(s) closesocket(s)
typedef SOCKET slot_socket_t;
#define SLOT_INVALID_SOCKET INVALID_SOCKET
#define SLOT_SOCKET_ERROR   SOCKET_ERROR
#else
#include <unistd.h>
#define slot_closesocket(s) close(s)
typedef int slot_socket_t;
#define SLOT_INVALID_SOCKET (-1)
#define SLOT_SOCKET_ERROR   (-1)
#endif

#include "net_slot.h"
#include "rpcemu.h"

/*
 * One TCP port per slot, on loopback, and twenty below the switch's own range so
 * the two cannot be confused when reading a netstat.
 *
 * Below 32768 deliberately, which is the lowest ephemeral range of the three
 * platforms (Linux starts there; Windows and macOS start at 49152). These used
 * to be at 49180, inside all of them, where the operating system is entitled to
 * hand the same port to something else - and on Windows to have reserved it at
 * boot before anything asks. See NET_SWITCH_PORT_BASE, which was one block up
 * and is where it was caught.
 */
#define NET_SLOT_PORT_BASE 19180

static slot_socket_t slot_socket = SLOT_INVALID_SOCKET;
static int slot = -1;

/**
 * Try to take one specific slot.
 *
 * Deliberately without SO_REUSEADDR: the whole point is that the second binder
 * fails. The relay learned this the hard way, where reuse let every instance bind
 * successfully and relay the same traffic with no error to notice.
 *
 * @return 1 if the slot is now ours.
 */
static int
claim_slot(int candidate)
{
	struct sockaddr_in addr;

	slot_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (slot_socket == SLOT_INVALID_SOCKET) {
		return 0;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short) (NET_SLOT_PORT_BASE + candidate));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(slot_socket, (struct sockaddr *) &addr, sizeof(addr))
	    == SLOT_SOCKET_ERROR) {
		slot_closesocket(slot_socket);
		slot_socket = SLOT_INVALID_SOCKET;
		return 0;
	}
	return 1;
}

int
net_slot_acquire(void)
{
	int candidate;

	if (slot >= 0) {
		return slot;
	}

	for (candidate = 0; candidate < NET_SLOT_MAX; candidate++) {
		if (claim_slot(candidate)) {
			slot = candidate;
			if (slot > 0) {
				rpclog("net_slot: slot %d, so this machine is addressed "
				       "distinctly from the %d already running\n",
				       slot, slot);
			}
			return slot;
		}
	}

	/*
	 * Every slot taken, or sockets unavailable entirely. Slot 0's addresses are the
	 * ones used before slots existed, so falling back to it leaves this machine no
	 * worse off than it always was: it will collide with the machine that holds
	 * slot 0, which is exactly the old behaviour, and it still starts.
	 */
	rpclog("net_slot: no free slot for this machine (%d already running), so it "
	       "shares slot 0's addresses and will collide with that machine on the "
	       "network\n", NET_SLOT_MAX);
	slot = 0;
	return slot;
}

int
net_slot_get(void)
{
	return slot;
}

void
net_slot_release(void)
{
	if (slot_socket != SLOT_INVALID_SOCKET) {
		slot_closesocket(slot_socket);
		slot_socket = SLOT_INVALID_SOCKET;
	}
	slot = -1;
}
