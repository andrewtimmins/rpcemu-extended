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

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

int
main(void)
{
	printf("the default, which an upgrade must keep\n");
	{
		const uint32_t net = guest_subnet_network(GUEST_SUBNET_DEFAULT_B,
		                                          GUEST_SUBNET_DEFAULT_C);

		check("is 10.10.10.0", net == 0x0a0a0a00u);
		check("gateway is 10.10.10.2", guest_subnet_gateway(net) == 0x0a0a0a02u);
		check("dns is 10.10.10.3", guest_subnet_dns(net) == 0x0a0a0a03u);
		check("broadcast is 10.10.10.255",
		    guest_subnet_broadcast(net) == 0x0a0a0affu);
		/* Slot 0 is the address every single-machine installation has always
		   had, so an upgrade does not move the guest. */
		check("slot 0 is 10.10.10.10", guest_subnet_guest(net, 0) == 0x0a0a0a0au);
		check("slot 1 is 10.10.10.11", guest_subnet_guest(net, 1) == 0x0a0a0a0bu);
		check("slot 9 is 10.10.10.19", guest_subnet_guest(net, 9) == 0x0a0a0a13u);
	}

	printf("\na subnet of one's own\n");
	{
		const uint32_t net = guest_subnet_network(200, 45);

		check("is 10.200.45.0", net == 0x0ac82d00u);
		check("gateway follows it", guest_subnet_gateway(net) == 0x0ac82d02u);
		check("dns follows it", guest_subnet_dns(net) == 0x0ac82d03u);
		check("broadcast follows it",
		    guest_subnet_broadcast(net) == 0x0ac82dffu);
		check("slot 0 follows it", guest_subnet_guest(net, 0) == 0x0ac82d0au);
	}

	printf("\nthe edges of 10.0.0.0/8\n");
	{
		check("10.0.0.0 is usable", guest_subnet_network(0, 0) == 0x0a000000u);
		check("10.255.255.0 is usable",
		    guest_subnet_network(255, 255) == 0x0affff00u);
		check("both octets are accepted anywhere in range",
		    guest_subnet_valid(0, 0) && guest_subnet_valid(255, 255) &&
		    guest_subnet_valid(128, 64));
		check("above 255 is not", !guest_subnet_valid(256, 0) &&
		    !guest_subnet_valid(0, 256));
	}

	printf("\nreading what the settings file holds\n");
	{
		unsigned b = 0, c = 0;

		check("\"40.7\" parses",
		    guest_subnet_parse("40.7", &b, &c) && b == 40 && c == 7);
		check("so does the full \"10.40.7.0\"",
		    guest_subnet_parse("10.40.7.0", &b, &c) && b == 40 && c == 7);
		check("and \"10.40.7.0/24\"",
		    guest_subnet_parse("10.40.7.0/24", &b, &c) && b == 40 && c == 7);

		/* A first octet other than 10 is not a typo to correct silently: it is
		   somebody asking for a network this cannot give them. */
		check("a network outside 10.0.0.0/8 is refused",
		    !guest_subnet_parse("192.168.1.0/24", &b, &c));
		check("an octet out of range is refused",
		    !guest_subnet_parse("10.300.1.0", &b, &c));
		check("nonsense is refused", !guest_subnet_parse("banana", &b, &c));
		check("an empty string is refused", !guest_subnet_parse("", &b, &c));
		check("NULL is refused", !guest_subnet_parse(NULL, &b, &c));
	}

	printf("\nwriting it back\n");
	{
		char out[16];

		guest_subnet_format(40, 7, out, sizeof(out));
		check("is stored as \"40.7\"", strcmp(out, "40.7") == 0);

		guest_subnet_format(GUEST_SUBNET_DEFAULT_B, GUEST_SUBNET_DEFAULT_C,
		    out, sizeof(out));
		check("the default is \"10.10\"", strcmp(out, "10.10") == 0);

		/* Round trip, which is what the settings file actually does. */
		{
			unsigned b = 0, c = 0;

			guest_subnet_format(200, 45, out, sizeof(out));
			check("what is written parses back the same",
			    guest_subnet_parse(out, &b, &c) && b == 200 && c == 45);
		}
	}

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");
	return failures != 0;
}
