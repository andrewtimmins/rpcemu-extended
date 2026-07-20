/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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
  Null platform networking backend.

  The "platform" (network_plt_*) hooks implement Ethernet bridging and IP
  tunnelling, which on Linux go through /dev/net/tun (network-tun.c). Those
  transports are out of scope on platforms without a TUN/TAP device (e.g.
  Windows), so this stub is built instead. NAT/SLiRP networking is handled
  separately (network-nat.c) and remains available.

  If the user selects bridging or tunnelling on such a platform, init fails
  gracefully and the network is left disabled.
*/

#include <stdint.h>

#include "rpcemu.h"
#include "network.h"
#include "savestate.h"

void
network_plt_reset(void)
{
}

int
network_plt_init(void)
{
	rpclog("Network: Ethernet bridging / IP tunnelling is not supported on "
	       "this platform; use NAT instead\n");
	return 0; /* failure: leaves networking disabled */
}

uint32_t
network_plt_tx(uint32_t errbuf, uint32_t mbufs, uint32_t dest, uint32_t src,
    uint32_t frametype)
{
	NOT_USED(errbuf);
	NOT_USED(mbufs);
	NOT_USED(dest);
	NOT_USED(src);
	NOT_USED(frametype);

	return 1; /* non-zero: error */
}

uint32_t
network_plt_rx(uint32_t errbuf, uint32_t mbuf, uint32_t rxhdr,
    uint32_t *dataavail)
{
	NOT_USED(errbuf);
	NOT_USED(mbuf);
	NOT_USED(rxhdr);

	*dataavail = 0;
	return 1; /* non-zero: error */
}

void
network_plt_setirqstatus(uint32_t address)
{
	NOT_USED(address);
}

void
network_plt_savestate(FILE *f)
{
	savestate_write_u32(f, 0);
}

void
network_plt_loadstate(FILE *f)
{
	(void) savestate_read_u32(f);
}
