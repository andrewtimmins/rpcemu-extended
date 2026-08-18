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
 * net_dissect.h - saying what an Ethernet frame is.
 *
 * Enough of a dissector to answer "what is this and who is it between" for a
 * list of frames, and to break one open into a tree of fields. Not a rival to
 * Wireshark and not trying to be: a capture can be written out and opened
 * there, and `rpcemu-netcap --pcap - | wireshark -k -i -` does it live.
 *
 * ★ What this knows that Wireshark does not is the RISC OS end of it.
 *
 * Freeway, Access and ShareFS are just UDP on unregistered ports to any other
 * dissector, so a capture of a RISC OS network reads as anonymous traffic
 * between anonymous ports. Naming them is most of the value here, and it is
 * why this exists at all rather than only ever shelling out to something else.
 *
 * Everything reads through bounds-checked helpers. The frames come from a
 * guest that can emit whatever it likes, including a header that claims a
 * length the frame does not have.
 */

#ifndef NET_DISSECT_H
#define NET_DISSECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** One line of the summary, as a list of frames shows it. */
typedef struct {
	char protocol[16];	/**< "ARP", "TCP", "NetBEUI"; never empty */
	char source[48];	/**< Address most meaningful at the top layer found */
	char dest[48];
	char info[160];		/**< What the frame is doing, in words */
} NetDissectSummary;

#define NETDIS_LINE_LEN		120
#define NETDIS_MAX_LINES	48

/** One line of the field tree, for a detail pane. */
typedef struct {
	uint8_t depth;			/**< 0 = a protocol, 1 = a field of it */
	char text[NETDIS_LINE_LEN];
} NetDissectLine;

/**
 * Summarise a frame: what it is, who it is between, what it is doing.
 *
 * Always fills every field, whatever the frame turns out to be, so a caller
 * never has to decide what to show for something unrecognised.
 */
void netdis_summary(const uint8_t *frame, uint32_t length,
    NetDissectSummary *out);

/**
 * Break a frame into a tree of fields.
 *
 * @return how many lines were written, at most @p max
 */
unsigned netdis_detail(const uint8_t *frame, uint32_t length,
    NetDissectLine *out, unsigned max);

/**
 * Format a MAC address, naming the ones that mean something.
 *
 * @param out Buffer of at least 32 bytes
 */
void netdis_format_mac(const uint8_t *mac, char *out, uint32_t out_len);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* NET_DISSECT_H */
