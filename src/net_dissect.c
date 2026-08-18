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
 * net_dissect.c - see net_dissect.h.
 *
 * ★ Every read is bounds-checked, and that is not belt and braces.
 *
 * A frame here came out of the guest, which is free to emit a header claiming
 * a length the frame has not got - by mistake or otherwise. Reading an IP
 * header length field and trusting it is how a packet dissector becomes the
 * most exposed thing in the program. So the frame is carried around as a
 * pointer and a length together, and nothing is read without asking first.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "net_dissect.h"

/* ---- bounds-checked reads ---------------------------------------------- */

static int
have(uint32_t length, uint32_t offset, uint32_t bytes)
{
	return offset + bytes >= offset && offset + bytes <= length;
}

static uint8_t
u8at(const uint8_t *f, uint32_t length, uint32_t offset)
{
	return have(length, offset, 1) ? f[offset] : 0;
}

static uint16_t
be16at(const uint8_t *f, uint32_t length, uint32_t offset)
{
	if (!have(length, offset, 2)) {
		return 0;
	}
	return (uint16_t) ((f[offset] << 8) | f[offset + 1]);
}

static uint32_t
be32at(const uint8_t *f, uint32_t length, uint32_t offset)
{
	if (!have(length, offset, 4)) {
		return 0;
	}
	return ((uint32_t) f[offset] << 24) | ((uint32_t) f[offset + 1] << 16) |
	       ((uint32_t) f[offset + 2] << 8) | (uint32_t) f[offset + 3];
}

/* ---- addresses --------------------------------------------------------- */

void
netdis_format_mac(const uint8_t *mac, char *out, uint32_t out_len)
{
	/*
	 * The named ones are worth naming: a list full of
	 * "03:00:00:00:00:01" tells you nothing, and that particular address is
	 * the one every Access announcement on a RISC OS network goes to.
	 */
	static const struct {
		uint8_t addr[6];
		const char *name;
	} known[] = {
		{ { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, "Broadcast" },
		{ { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 }, "NetBEUI" },
		{ { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 }, "STP" },
	};
	unsigned i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (memcmp(mac, known[i].addr, 6) == 0) {
			snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x (%s)",
			    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
			    known[i].name);
			return;
		}
	}
	snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void
format_ipv4(uint32_t addr, char *out, uint32_t out_len)
{
	snprintf(out, out_len, "%u.%u.%u.%u", (addr >> 24) & 0xff,
	    (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);
}

/**
 * What a UDP or TCP port is for, where we know.
 *
 * ★ This is the part no other dissector has.
 *
 * The RISC OS ones are not registered anywhere, so to anything else they are
 * anonymous numbers and a capture of a RISC OS network reads as traffic
 * between nothing in particular.
 */
static const char *
port_name(uint16_t port)
{
	switch (port) {
	case 32770:	return "Freeway";	/* Access discovery and announcements */
	case 49171:	return "ShareFS";	/* the file transfers themselves */
	case 32771:	return "Freeway";
	case 67:	return "DHCP";
	case 68:	return "DHCP";
	case 53:	return "DNS";
	case 123:	return "NTP";
	case 137:	return "NetBIOS-NS";
	case 138:	return "NetBIOS-DGM";
	case 139:	return "NetBIOS-SSN";
	case 20:	return "FTP-data";
	case 21:	return "FTP";
	case 22:	return "SSH";
	case 23:	return "Telnet";
	case 25:	return "SMTP";
	case 80:	return "HTTP";
	case 110:	return "POP3";
	case 143:	return "IMAP";
	case 443:	return "HTTPS";
	default:	return NULL;
	}
}

/** "10.0.0.1:80 (HTTP)" where the port is known, else "10.0.0.1:1234". */
static void
format_endpoint(const char *addr, uint16_t port, char *out, uint32_t out_len)
{
	const char *name = port_name(port);

	if (name != NULL) {
		snprintf(out, out_len, "%s:%u (%s)", addr, port, name);
	} else {
		snprintf(out, out_len, "%s:%u", addr, port);
	}
}

/* ---- the summary ------------------------------------------------------- */

#define ETH_HEADER	14
#define ETHERTYPE_IPV4	0x0800
#define ETHERTYPE_ARP	0x0806
#define ETHERTYPE_RARP	0x8035
#define ETHERTYPE_IPV6	0x86dd

/** TCP flags, most significant first, as a printable set. */
static void
tcp_flags(uint8_t bits, char *out, uint32_t out_len)
{
	static const char *names[] = { "FIN", "SYN", "RST", "PSH", "ACK", "URG" };
	unsigned i;
	size_t used = 0;

	out[0] = '\0';
	for (i = 0; i < 6; i++) {
		if ((bits & (1u << i)) == 0) {
			continue;
		}
		used += (size_t) snprintf(out + used, (used < out_len) ? out_len - used : 0,
		    "%s%s", (used > 0) ? ", " : "", names[i]);
		if (used >= out_len) {
			break;
		}
	}
	if (out[0] == '\0') {
		snprintf(out, out_len, "none");
	}
}

static void
summarise_ipv4(const uint8_t *f, uint32_t length, uint32_t off,
               NetDissectSummary *out)
{
	const uint8_t ihl = (uint8_t) ((u8at(f, length, off) & 0x0f) * 4u);
	const uint8_t proto = u8at(f, length, off + 9);
	char src[24], dst[24];
	uint32_t l4;

	format_ipv4(be32at(f, length, off + 12), src, sizeof(src));
	format_ipv4(be32at(f, length, off + 16), dst, sizeof(dst));

	/*
	 * A header shorter than the minimum means the length field is not to be
	 * trusted, so the payload is not looked at rather than guessed at.
	 */
	if (ihl < 20) {
		snprintf(out->protocol, sizeof(out->protocol), "IPv4");
		snprintf(out->source, sizeof(out->source), "%s", src);
		snprintf(out->dest, sizeof(out->dest), "%s", dst);
		snprintf(out->info, sizeof(out->info),
		    "Header length %u is too short to be real", ihl);
		return;
	}
	l4 = off + ihl;

	switch (proto) {
	case 1: {	/* ICMP */
		const uint8_t type = u8at(f, length, l4);
		const char *what;

		switch (type) {
		case 0:  what = "Echo reply"; break;
		case 3:  what = "Destination unreachable"; break;
		case 8:  what = "Echo request"; break;
		case 11: what = "Time exceeded"; break;
		default: what = "ICMP"; break;
		}
		snprintf(out->protocol, sizeof(out->protocol), "ICMP");
		snprintf(out->source, sizeof(out->source), "%s", src);
		snprintf(out->dest, sizeof(out->dest), "%s", dst);
		snprintf(out->info, sizeof(out->info), "%s, id %u, seq %u", what,
		    be16at(f, length, l4 + 4), be16at(f, length, l4 + 6));
		break;
	}
	case 6: {	/* TCP */
		const uint16_t sp = be16at(f, length, l4);
		const uint16_t dp = be16at(f, length, l4 + 2);
		char flags[48];

		tcp_flags(u8at(f, length, l4 + 13) & 0x3f, flags, sizeof(flags));
		snprintf(out->protocol, sizeof(out->protocol), "TCP");
		format_endpoint(src, sp, out->source, sizeof(out->source));
		format_endpoint(dst, dp, out->dest, sizeof(out->dest));
		snprintf(out->info, sizeof(out->info), "%u > %u [%s] seq %u", sp, dp,
		    flags, be32at(f, length, l4 + 4));
		break;
	}
	case 17: {	/* UDP */
		const uint16_t sp = be16at(f, length, l4);
		const uint16_t dp = be16at(f, length, l4 + 2);
		const uint16_t ulen = be16at(f, length, l4 + 4);
		const char *sname = port_name(sp);
		const char *dname = port_name(dp);

		snprintf(out->protocol, sizeof(out->protocol), "%s",
		    (dname != NULL) ? dname : (sname != NULL) ? sname : "UDP");
		format_endpoint(src, sp, out->source, sizeof(out->source));
		format_endpoint(dst, dp, out->dest, sizeof(out->dest));
		snprintf(out->info, sizeof(out->info), "%u > %u, %u byte%s of payload",
		    sp, dp, (ulen >= 8) ? (unsigned) (ulen - 8) : 0u,
		    (ulen == 9) ? "" : "s");
		break;
	}
	default:
		snprintf(out->protocol, sizeof(out->protocol), "IPv4");
		snprintf(out->source, sizeof(out->source), "%s", src);
		snprintf(out->dest, sizeof(out->dest), "%s", dst);
		snprintf(out->info, sizeof(out->info), "Protocol %u", proto);
		break;
	}
}

static void
summarise_arp(const uint8_t *f, uint32_t length, uint32_t off, int rarp,
              NetDissectSummary *out)
{
	const uint16_t op = be16at(f, length, off + 6);
	char sender_ip[24], target_ip[24], sender_mac[32];

	format_ipv4(be32at(f, length, off + 14), sender_ip, sizeof(sender_ip));
	format_ipv4(be32at(f, length, off + 24), target_ip, sizeof(target_ip));
	if (have(length, off + 8, 6)) {
		netdis_format_mac(f + off + 8, sender_mac, sizeof(sender_mac));
	} else {
		snprintf(sender_mac, sizeof(sender_mac), "?");
	}

	snprintf(out->protocol, sizeof(out->protocol), "%s", rarp ? "RARP" : "ARP");
	snprintf(out->source, sizeof(out->source), "%s", sender_ip);
	snprintf(out->dest, sizeof(out->dest), "%s", target_ip);
	switch (op) {
	case 1:
		snprintf(out->info, sizeof(out->info), "Who has %s? Tell %s",
		    target_ip, sender_ip);
		break;
	case 2:
		snprintf(out->info, sizeof(out->info), "%s is at %s", sender_ip,
		    sender_mac);
		break;
	default:
		snprintf(out->info, sizeof(out->info), "Opcode %u", op);
		break;
	}
}

/**
 * 802.3 with an LLC header, which on a RISC OS network means Access.
 *
 * The length field being 1500 or less is what separates this from Ethernet II:
 * every real EtherType is above 1536, which is why the two can share a slot.
 */
static void
summarise_llc(const uint8_t *f, uint32_t length, uint32_t off, uint16_t len_field,
              NetDissectSummary *out)
{
	const uint8_t dsap = u8at(f, length, off);
	const uint8_t ssap = u8at(f, length, off + 1);

	if (dsap == 0xf0 && ssap == 0xf0) {
		snprintf(out->protocol, sizeof(out->protocol), "NetBEUI");
		snprintf(out->info, sizeof(out->info),
		    "NetBIOS over LLC, %u bytes", len_field);
	} else if (dsap == 0xaa && ssap == 0xaa) {
		snprintf(out->protocol, sizeof(out->protocol), "SNAP");
		snprintf(out->info, sizeof(out->info),
		    "SNAP, protocol %04x", be16at(f, length, off + 6));
	} else {
		snprintf(out->protocol, sizeof(out->protocol), "802.3");
		snprintf(out->info, sizeof(out->info),
		    "LLC, DSAP %02x SSAP %02x, %u bytes", dsap, ssap, len_field);
	}
}

void
netdis_summary(const uint8_t *frame, uint32_t length, NetDissectSummary *out)
{
	uint16_t type;

	memset(out, 0, sizeof(*out));

	if (frame == NULL || length < ETH_HEADER) {
		snprintf(out->protocol, sizeof(out->protocol), "?");
		snprintf(out->source, sizeof(out->source), "-");
		snprintf(out->dest, sizeof(out->dest), "-");
		snprintf(out->info, sizeof(out->info),
		    "Runt: %u byte%s, shorter than an Ethernet header", length,
		    (length == 1) ? "" : "s");
		return;
	}

	/* Filled in from the Ethernet header, then overwritten by whatever
	   understands the payload better. */
	netdis_format_mac(frame + 6, out->source, sizeof(out->source));
	netdis_format_mac(frame, out->dest, sizeof(out->dest));

	type = be16at(frame, length, 12);
	if (type <= 1500) {
		summarise_llc(frame, length, ETH_HEADER, type, out);
		return;
	}

	switch (type) {
	case ETHERTYPE_IPV4:
		summarise_ipv4(frame, length, ETH_HEADER, out);
		break;
	case ETHERTYPE_ARP:
		summarise_arp(frame, length, ETH_HEADER, 0, out);
		break;
	case ETHERTYPE_RARP:
		summarise_arp(frame, length, ETH_HEADER, 1, out);
		break;
	case ETHERTYPE_IPV6:
		snprintf(out->protocol, sizeof(out->protocol), "IPv6");
		snprintf(out->info, sizeof(out->info), "IPv6, next header %u",
		    u8at(frame, length, ETH_HEADER + 6));
		break;
	default:
		snprintf(out->protocol, sizeof(out->protocol), "0x%04x", type);
		snprintf(out->info, sizeof(out->info),
		    "Ethernet II, type 0x%04x, %u bytes", type, length);
		break;
	}
}

/* ---- the detail tree --------------------------------------------------- */

typedef struct {
	NetDissectLine *out;
	unsigned max;
	unsigned used;
} LineSink;

static void
emit(LineSink *sink, uint8_t depth, const char *fmt, ...)
{
	va_list ap;

	if (sink->used >= sink->max) {
		return;
	}
	sink->out[sink->used].depth = depth;
	va_start(ap, fmt);
	vsnprintf(sink->out[sink->used].text, NETDIS_LINE_LEN, fmt, ap);
	va_end(ap);
	sink->used++;
}

unsigned
netdis_detail(const uint8_t *frame, uint32_t length, NetDissectLine *out,
              unsigned max)
{
	LineSink sink = { out, max, 0 };
	char a[48], b[48];
	uint16_t type;

	if (out == NULL || max == 0) {
		return 0;
	}
	if (frame == NULL || length < ETH_HEADER) {
		emit(&sink, 0, "Frame too short to dissect (%u bytes)", length);
		return sink.used;
	}

	netdis_format_mac(frame, a, sizeof(a));
	netdis_format_mac(frame + 6, b, sizeof(b));
	type = be16at(frame, length, 12);

	emit(&sink, 0, "Ethernet, %u bytes", length);
	emit(&sink, 1, "Destination: %s", a);
	emit(&sink, 1, "Source: %s", b);
	if (type <= 1500) {
		emit(&sink, 1, "Length: %u", type);
		emit(&sink, 0, "Logical Link Control");
		emit(&sink, 1, "DSAP: 0x%02x", u8at(frame, length, ETH_HEADER));
		emit(&sink, 1, "SSAP: 0x%02x", u8at(frame, length, ETH_HEADER + 1));
		emit(&sink, 1, "Control: 0x%02x", u8at(frame, length, ETH_HEADER + 2));
		return sink.used;
	}
	emit(&sink, 1, "Type: 0x%04x", type);

	if (type == ETHERTYPE_ARP || type == ETHERTYPE_RARP) {
		const uint32_t o = ETH_HEADER;
		char ip[24];

		emit(&sink, 0, "%s", (type == ETHERTYPE_ARP) ? "Address Resolution "
		    "Protocol" : "Reverse Address Resolution Protocol");
		emit(&sink, 1, "Opcode: %u", be16at(frame, length, o + 6));
		if (have(length, o + 8, 6)) {
			netdis_format_mac(frame + o + 8, a, sizeof(a));
			emit(&sink, 1, "Sender hardware address: %s", a);
		}
		format_ipv4(be32at(frame, length, o + 14), ip, sizeof(ip));
		emit(&sink, 1, "Sender protocol address: %s", ip);
		if (have(length, o + 18, 6)) {
			netdis_format_mac(frame + o + 18, a, sizeof(a));
			emit(&sink, 1, "Target hardware address: %s", a);
		}
		format_ipv4(be32at(frame, length, o + 24), ip, sizeof(ip));
		emit(&sink, 1, "Target protocol address: %s", ip);
		return sink.used;
	}

	if (type == ETHERTYPE_IPV4) {
		const uint32_t o = ETH_HEADER;
		const uint8_t ihl = (uint8_t) ((u8at(frame, length, o) & 0x0f) * 4u);
		const uint8_t proto = u8at(frame, length, o + 9);
		uint32_t l4 = o + ihl;
		char ip[24];

		emit(&sink, 0, "Internet Protocol Version 4");
		emit(&sink, 1, "Header length: %u bytes", ihl);
		emit(&sink, 1, "Total length: %u", be16at(frame, length, o + 2));
		emit(&sink, 1, "Identification: 0x%04x", be16at(frame, length, o + 4));
		emit(&sink, 1, "Time to live: %u", u8at(frame, length, o + 8));
		emit(&sink, 1, "Protocol: %u", proto);
		format_ipv4(be32at(frame, length, o + 12), ip, sizeof(ip));
		emit(&sink, 1, "Source: %s", ip);
		format_ipv4(be32at(frame, length, o + 16), ip, sizeof(ip));
		emit(&sink, 1, "Destination: %s", ip);

		if (ihl < 20) {
			emit(&sink, 1, "Header length is too short to be real; payload "
			    "not decoded");
			return sink.used;
		}

		if (proto == 6) {
			char flags[48];
			const uint16_t sp = be16at(frame, length, l4);
			const uint16_t dp = be16at(frame, length, l4 + 2);

			tcp_flags(u8at(frame, length, l4 + 13) & 0x3f, flags, sizeof(flags));
			emit(&sink, 0, "Transmission Control Protocol");
			emit(&sink, 1, "Source port: %u%s%s%s", sp,
			    port_name(sp) ? " (" : "", port_name(sp) ? port_name(sp) : "",
			    port_name(sp) ? ")" : "");
			emit(&sink, 1, "Destination port: %u%s%s%s", dp,
			    port_name(dp) ? " (" : "", port_name(dp) ? port_name(dp) : "",
			    port_name(dp) ? ")" : "");
			emit(&sink, 1, "Sequence number: %u", be32at(frame, length, l4 + 4));
			emit(&sink, 1, "Acknowledgement: %u", be32at(frame, length, l4 + 8));
			emit(&sink, 1, "Flags: %s", flags);
			emit(&sink, 1, "Window: %u", be16at(frame, length, l4 + 14));
		} else if (proto == 17) {
			const uint16_t sp = be16at(frame, length, l4);
			const uint16_t dp = be16at(frame, length, l4 + 2);

			emit(&sink, 0, "User Datagram Protocol");
			emit(&sink, 1, "Source port: %u%s%s%s", sp,
			    port_name(sp) ? " (" : "", port_name(sp) ? port_name(sp) : "",
			    port_name(sp) ? ")" : "");
			emit(&sink, 1, "Destination port: %u%s%s%s", dp,
			    port_name(dp) ? " (" : "", port_name(dp) ? port_name(dp) : "",
			    port_name(dp) ? ")" : "");
			emit(&sink, 1, "Length: %u", be16at(frame, length, l4 + 4));
		} else if (proto == 1) {
			emit(&sink, 0, "Internet Control Message Protocol");
			emit(&sink, 1, "Type: %u", u8at(frame, length, l4));
			emit(&sink, 1, "Code: %u", u8at(frame, length, l4 + 1));
			emit(&sink, 1, "Identifier: %u", be16at(frame, length, l4 + 4));
			emit(&sink, 1, "Sequence: %u", be16at(frame, length, l4 + 6));
		}
		return sink.used;
	}

	return sink.used;
}
