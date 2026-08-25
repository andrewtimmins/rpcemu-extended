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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_subnet.h"

uint32_t
guest_subnet_network(unsigned b, unsigned c)
{
	return 0x0a000000u | ((b & 0xffu) << 16) | ((c & 0xffu) << 8);
}

int
guest_subnet_valid(unsigned b, unsigned c)
{
	return b <= 255u && c <= 255u;
}

int
guest_subnet_parse(const char *text, unsigned *b, unsigned *c)
{
	unsigned o[4];
	unsigned vb, vc;

	if (text == NULL || b == NULL || c == NULL) {
		return 0;
	}

	/* "10.b.c.0/24" and "10.b.c.0" as the dialogue and a hand-edited file might
	   write them, with the leading 10 checked rather than assumed. */
	if (sscanf(text, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) == 4) {
		if (o[0] != 10u || !guest_subnet_valid(o[1], o[2])) {
			return 0;
		}
		*b = o[1];
		*c = o[2];
		return 1;
	}

	/* "b.c", which is what the settings file holds: the 10 and the .0/24 are
	   not the user's to change, so storing them invites someone to try. */
	if (sscanf(text, "%u.%u", &vb, &vc) == 2) {
		if (!guest_subnet_valid(vb, vc)) {
			return 0;
		}
		*b = vb;
		*c = vc;
		return 1;
	}

	return 0;
}

void
guest_subnet_format(unsigned b, unsigned c, char *out, size_t len)
{
	if (out == NULL || len < 8) {
		return;
	}
	snprintf(out, len, "%u.%u", b & 0xffu, c & 0xffu);
}

uint32_t
guest_subnet_gateway(uint32_t network)
{
	return network | 0x02u;
}

uint32_t
guest_subnet_dns(uint32_t network)
{
	return network | 0x03u;
}

uint32_t
guest_subnet_broadcast(uint32_t network)
{
	return network | 0xffu;
}

uint32_t
guest_subnet_guest(uint32_t network, int slot)
{
	if (slot < 0) {
		slot = 0;
	}
	return network + 0x0au + (uint32_t) slot;
}
