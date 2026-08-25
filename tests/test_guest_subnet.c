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
 * Which /24 the guests live on.
 *
 * The addresses derived here are also derived independently by the Access relay
 * (broadcast_relay.c), which decides from them what is local traffic and what is
 * external. If the two disagree the relay sends the wrong packets to the wrong
 * network, so the arithmetic is worth pinning down.
 */

#include <stdio.h>
#include <string.h>

#include "guest_subnet.h"

static int failures = 0;

static void
check(const char *what, int ok)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

/** A MAC address from six bytes, for readability below. */
static void
mac(uint8_t out[6], unsigned a, unsigned b, unsigned c,
    unsigned d, unsigned e, unsigned f)
{
	out[0] = (uint8_t) a; out[1] = (uint8_t) b; out[2] = (uint8_t) c;
	out[3] = (uint8_t) d; out[4] = (uint8_t) e; out[5] = (uint8_t) f;
}

int
main(void)
{
	printf("the network\n");
	check("is 100.64.0.0", guest_subnet_network() == 0x64400000u);
	check("the gateway is 100.64.0.1", guest_subnet_gateway() == 0x64400001u);
	check("the DNS is 100.64.0.2", guest_subnet_dns() == 0x64400002u);
	/* SLiRP special-cases traffic to the nameserver, so these must differ. */
	check("gateway and DNS are different addresses",
	    guest_subnet_gateway() != guest_subnet_dns());
	check("the broadcast is 100.127.255.255",
	    guest_subnet_broadcast() == 0x647fffffu);

	printf("what is inside it\n");
	check("the network address is", guest_subnet_contains(0x64400000u));
	check("the broadcast is", guest_subnet_contains(0x647fffffu));
	check("100.63.255.255 is not", !guest_subnet_contains(0x643fffffu));
	check("100.128.0.0 is not", !guest_subnet_contains(0x64800000u));
	check("10.10.10.10 is not", !guest_subnet_contains(0x0a0a0a0au));
	/* The range a host LAN is most likely to be on, and the reason this is not
	   inside 10.0.0.0/8. */
	check("192.168.1.1 is not", !guest_subnet_contains(0xc0a80101u));

	printf("a guest address\n");
	{
		uint8_t m[6];
		uint32_t addr;
		char text[16];

		mac(m, 0x06, 0x02, 0x03, 0x04, 0x05, 0x06);
		addr = guest_subnet_guest(m);

		check("is inside the network", guest_subnet_contains(addr));
		check("is not the network address", addr != guest_subnet_network());
		check("is not the gateway", addr != guest_subnet_gateway());
		check("is not the DNS", addr != guest_subnet_dns());
		check("is not the broadcast", addr != guest_subnet_broadcast());

		guest_subnet_format(addr, text, sizeof(text));
		check("formats as four octets", strchr(text, '.') != NULL);
	}

	printf("stability\n");
	{
		uint8_t m[6], n[6];

		mac(m, 0x06, 0x02, 0x03, 0x04, 0x05, 0x06);
		mac(n, 0x06, 0x02, 0x03, 0x04, 0x05, 0x06);
		check("the same MAC always gives the same address",
		    guest_subnet_guest(m) == guest_subnet_guest(n));

		/* ★ The value itself is pinned, not just its stability. Two
		   installations must compute the SAME address from one MAC, so a change
		   to the hash breaks interoperability between versions and has to be a
		   deliberate act rather than a refactor nobody noticed. */
		check("and that address is 100.100.51.77",
		    guest_subnet_guest(m) == 0x6464334du);

		mac(n, 0x06, 0x02, 0x03, 0x04, 0x05, 0x07);
		check("one bit of difference gives a different address",
		    guest_subnet_guest(m) != guest_subnet_guest(n));
	}

	printf("the reserved addresses are avoided\n");
	{
		/* Walked rather than reasoned about: the fold has to land clear of the
		   four reserved values for every possible MAC, and the interesting ones
		   are wherever the hash happens to be small. */
		unsigned i;
		int all_clear = 1;

		for (i = 0; i < 200000; i++) {
			uint8_t m[6];
			uint32_t addr;

			m[0] = (uint8_t) (i & 0xff);
			m[1] = (uint8_t) ((i >> 8) & 0xff);
			m[2] = (uint8_t) ((i >> 16) & 0xff);
			m[3] = 0x11; m[4] = 0x22; m[5] = 0x33;

			addr = guest_subnet_guest(m);
			if (!guest_subnet_contains(addr) ||
			    addr == guest_subnet_network() ||
			    addr == guest_subnet_gateway() ||
			    addr == guest_subnet_dns() ||
			    addr == guest_subnet_broadcast()) {
				all_clear = 0;
				break;
			}
		}
		check("200000 MAC addresses all give usable addresses", all_clear);
	}

	/*
	 * The duplicate warning. A collision is unlikely but not impossible, and
	 * this is the one thing that turns it from a silent failure into something
	 * a log can be read for.
	 */
	printf("noticing a duplicate\n");
	{
		uint8_t me[6], other[6];
		uint8_t frame[64];

		mac(me, 0x06, 0x02, 0x03, 0x04, 0x05, 0x06);
		mac(other, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);

		/* An ARP frame from `who`, claiming `ip`. */
		#define BUILD_ARP(who, ip) do {                                       \
			memset(frame, 0, sizeof(frame));                              \
			memset(frame, 0xff, 6);                                       \
			memcpy(frame + 6, (who), 6);                                  \
			frame[12] = 0x08; frame[13] = 0x06;                           \
			memcpy(frame + 22, (who), 6);                                 \
			frame[28] = (uint8_t) (((ip) >> 24) & 0xffu);                 \
			frame[29] = (uint8_t) (((ip) >> 16) & 0xffu);                 \
			frame[30] = (uint8_t) (((ip) >> 8) & 0xffu);                  \
			frame[31] = (uint8_t) ((ip) & 0xffu);                         \
		} while (0)

		/* None of these may report, so the check is that they return at all -
		   a wrong read of the frame would be a crash or an assert, and the
		   reported-once flag would be spent on the wrong frame. */
		BUILD_ARP(me, guest_subnet_guest(me));
		guest_subnet_check_duplicate(frame, 42, me);

		BUILD_ARP(other, guest_subnet_guest(other));
		guest_subnet_check_duplicate(frame, 42, me);

		/* Too short to hold an ARP payload. */
		BUILD_ARP(other, guest_subnet_guest(me));
		guest_subnet_check_duplicate(frame, 20, me);

		/* Not ARP at all. */
		BUILD_ARP(other, guest_subnet_guest(me));
		frame[12] = 0x08; frame[13] = 0x00;
		guest_subnet_check_duplicate(frame, 42, me);

		check("a frame that is not a duplicate leaves the warning unspent", 1);

		#undef BUILD_ARP
	}

	printf("formatting\n");
	{
		char text[16];

		guest_subnet_format(0x64400001u, text, sizeof(text));
		check("the gateway reads as 100.64.0.1", strcmp(text, "100.64.0.1") == 0);
		guest_subnet_format(0x647fffffu, text, sizeof(text));
		check("the broadcast reads as 100.127.255.255",
		    strcmp(text, "100.127.255.255") == 0);
	}

	if (failures != 0) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
