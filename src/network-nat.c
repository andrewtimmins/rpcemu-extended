/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 RPCEmu contributors
  Copyright (C) 2025-2026 Andy Timmins

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

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "rpcemu.h"
#include "app_settings.h"
#include "mem.h"
#include "network.h"
#include "network-nat.h"
#include "net_slot.h"
#include "savestate.h"
#include "podules.h"
#include "broadcast_relay.h"

#include "slirp/libslirp.h"

#define PIPE_PATH	"/tmp/rpcemu_net_nat"

#define HEADERLEN	14

/* Packet queue for incoming packets from broadcast relay */
#define PKT_QUEUE_SIZE  32      /* Number of packets in queue */
#define PKT_MAX_SIZE    2048    /* Max size of each packet */

typedef struct {
	uint8_t  data[PKT_MAX_SIZE];
	size_t   len;
} queued_packet_t;

static struct {
	Slirp		*slirp;

	uint32_t	irq_status;	///< Address of a word in RAM, used as the IRQ status register

	/* The received frame the guest's next receive call will take, and the
	   queue of frames behind it. */
	uint8_t		buffer[PKT_MAX_SIZE];

	size_t		buffer_len;

	/* Transmit has a buffer of its own, and must keep it. Sharing the receive
	   buffer meant a transmit issued while a received frame was still waiting
	   overwrote that frame without clearing buffer_len, so the guest then read
	   its own outgoing frame back as if it had arrived. The window is real,
	   because a transmit can be issued with interrupts off, or by a protocol
	   module called from the driver's own receive path. */
	uint8_t		tx_buffer[PKT_MAX_SIZE];

	FILE		*capture;	///< Handle for debug capture file, or NULL if not in use

	struct in_addr	forward_addr;	///< Which IP address to apply NAT forward rules to

	/* Packet queue for relay-injected packets */
	queued_packet_t pkt_queue[PKT_QUEUE_SIZE];
	int             pkt_queue_head;  /* Next slot to write */
	int             pkt_queue_tail;  /* Next slot to read */
	int             pkt_queue_count; /* Number of packets in queue */
} nat;

/* Forward declarations */
static void deliver_queued_packet(void);
static int rx_space(void);
static int rx_deliver(const uint8_t *pkt, size_t pkt_len);

static void
write16(FILE *f, uint16_t x)
{
	(void) fwrite(&x, sizeof(uint16_t), 1, f);
}

static void
write32(FILE *f, uint32_t x)
{
	(void) fwrite(&x, sizeof(uint32_t), 1, f);
}

/**
 */
static void
write_global_header(FILE *f)
{
	// The magic number is adaptive-endian.
	// It's written in the host's native order, and the reader is
	// expected to interpret all data fields accordingly.
	write32(f, 0xa1b2c3d4); // magic number

	write16(f, 2); // version_major
	write16(f, 4); // version_minor
	write32(f, 0); // thiszone
	write32(f, 0); // sigfigs
	write32(f, 65535); // snaplen
	write32(f, 1); // data link type - 1 for Ethernet
	(void) fflush(f);
}

/**
 * Write a packet to the capture file.
 *
 * @param f
 * @param data
 * @param data_len
 */
static void
write_packet(FILE *f, const void *data, size_t data_len)
{
	struct timeval tv;

	// Check that the File was opened successfully
	if (f == NULL) {
		return;
	}

	if (gettimeofday(&tv, NULL) != 0) {
		return;
	}

	write32(f, (uint32_t) tv.tv_sec); // timestamp seconds
	write32(f, (uint32_t) tv.tv_usec); // timestamp microseconds
	write32(f, (uint32_t) data_len); // number of octets of packet saved in file
	write32(f, (uint32_t) data_len); // actual length of packet
	(void) fwrite(data, 1, data_len, f); // packet data
	(void) fflush(f);
}

/**
 * Number of received frames the guest side can still take: the delivery slot
 * if it is free, plus the free entries in the queue behind it.
 */
static int
rx_space(void)
{
	return ((nat.buffer_len == 0) ? 1 : 0) +
	       (PKT_QUEUE_SIZE - nat.pkt_queue_count);
}

/**
 * Hand a received frame to the guest, queueing it behind the delivery slot if
 * that slot is still occupied.
 *
 * Both sources of received frames come through here: SLiRP, and the Access
 * broadcast relay. They used to have a path each, and only the relay's was
 * queued.
 *
 * @param pkt     Complete Ethernet frame
 * @param pkt_len Length of frame in bytes
 *
 * @return 1 if the frame was taken, 0 if it was dropped
 */
static int
rx_deliver(const uint8_t *pkt, size_t pkt_len)
{
	queued_packet_t *slot;

	if (pkt_len == 0 || pkt_len > PKT_MAX_SIZE) {
		return 0;
	}

	/* Write to capture file for debug. Done on arrival rather than on
	   delivery to the guest, so the capture keeps the order and the timing
	   the frames actually turned up in. */
	write_packet(nat.capture, pkt, pkt_len);

	if (nat.irq_status == 0 || network_poduleinfo == NULL) {
		// The guest driver has not registered, so there is nowhere to put it
		return 0;
	}

	if (nat.buffer_len == 0 && nat.pkt_queue_count == 0) {
		memcpy(nat.buffer, pkt, pkt_len);
		nat.buffer_len = pkt_len;
		network_irq_raise();
		return 1;
	}

	if (nat.pkt_queue_count >= PKT_QUEUE_SIZE) {
		return 0;
	}

	slot = &nat.pkt_queue[nat.pkt_queue_head];
	memcpy(slot->data, pkt, pkt_len);
	slot->len = pkt_len;
	nat.pkt_queue_head = (nat.pkt_queue_head + 1) % PKT_QUEUE_SIZE;
	nat.pkt_queue_count++;

	return 1;
}

/**
 */
void
slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
	NOT_USED(opaque);

	if (pkt_len <= 0) {
		return;
	}

	(void) rx_deliver(pkt, (size_t) pkt_len);
}

/**
 * Tell SLiRP whether it may hand over another frame.
 *
 * This gates if_start(), which is only reached from network_nat_poll(). While
 * this answered "only when the single delivery slot is free" the guest could
 * take at most one frame per poll, and the guest cannot drain the slot during a
 * poll because both run on the emulation thread. That put a hard ceiling of one
 * received packet per ~2.5 million emulated instructions on everything
 * arriving through NAT. Reporting the queue behind the slot lets one poll drain
 * a burst.
 */
int
slirp_can_output(void *opaque)
{
	NOT_USED(opaque);

	return rx_space() > 0;
}

/**
 */
static void
network_nat_init_mac_address(void)
{
	if (config.macaddress != NULL) {
		// Parse supplied MAC address
		if (network_macaddress_parse(config.macaddress, network_hwaddr)) {
			return;
		}
		error("Unable to parse '%s' as a MAC address", config.macaddress);
	}

	// Generate MAC address. The last byte follows this emulator's slot, because the
	// address was otherwise the same constant in every instance and two machines
	// with one MAC address on one network is fatal to both. Slot 0 keeps the
	// original value, so a single machine is unchanged.
	network_hwaddr[0] = 0x06;
	network_hwaddr[1] = 0x02;
	network_hwaddr[2] = 0x03;
	network_hwaddr[3] = 0x04;
	network_hwaddr[4] = 0x05;
	network_hwaddr[5] = (unsigned char) (0x06 + net_slot_acquire());
}

/**
 */
static void
network_nat_open(void)
{
	struct in_addr host;
	struct in_addr mask;
	struct in_addr net_addr;
	struct in_addr dns;
	struct in_addr dhcp;
	const int restricted = 0;
	const char *vhostname = NULL;
	const char *bootfile = NULL;

	host.s_addr = htonl(0x0a0a0a02); // 10.10.10.2
	mask.s_addr = htonl(0xffffff00); // 255.255.255.0
	net_addr.s_addr = htonl(0x0a0a0a00); // 10.10.10.0
	dns.s_addr = htonl(0x0a0a0a03); // 10.10.10.3

	// 10.10.10.10 upwards, one address per emulator on this host. Every instance ran
	// its own NAT with these same constants, so every guest believed it was
	// 10.10.10.10: two machines could not have addressed each other whatever carried
	// the frames between them. Slot 0 gets the original .10, leaving a single
	// machine exactly as it was. The gateway and DNS stay put, since each machine
	// keeps its own NAT and only the guest's own address has to differ.
	dhcp.s_addr = htonl(0x0a0a0a0a + (uint32_t) net_slot_acquire());

	// Port Forwarding
	nat.forward_addr.s_addr = dhcp.s_addr; // Which address to apply port forwards to (same as address given out by DHCP)

	// Initialise, but only once
	if (nat.slirp == NULL) {
		int i;

		nat.slirp = slirp_init(restricted,
		    net_addr, mask, host, vhostname, "", bootfile, dhcp, dns, NULL);

		// TODO log NAT details

		// Forwarded Ports
		for (i = 0; i < MAX_PORT_FORWARDS; i++) {
			if (port_forward_rules[i].type != PORT_FORWARD_NONE) {
				network_nat_forward_add(port_forward_rules[i]);

				rpcemu_send_nat_rule_to_gui(port_forward_rules[i]);
			}
		}
	}
}

int
network_nat_init(void)
{
	//nat.buffer_len = 0;

	// Initialize packet queue
	nat.pkt_queue_head = 0;
	nat.pkt_queue_tail = 0;
	nat.pkt_queue_count = 0;

	// MAC address
	network_nat_init_mac_address();

	network_nat_open();

	// Open capture file if requested
	if (config.network_capture != NULL) {
		if ((nat.capture = fopen(config.network_capture, "wb")) != NULL) {
			// Write header
			write_global_header(nat.capture);

			rpclog("Networking: capturing to \"%s\"\n", config.network_capture);
		}
	}

	// Initialize broadcast relay for Access+ support
	/* One relay per host: the Access ports are fixed by the protocol, so a second
	   emulator cannot have them. Turning it off deliberately is better than
	   racing for them and losing quietly. */
	if (app_settings_relay_enabled()) {
		broadcast_relay_init();
	} else {
		rpclog("network: Access broadcast relay disabled for this instance "
		       "(--no-relay)\n");
	}

	return 1;
}

void
network_nat_reset(void)
{
	network_irq_lower();
	nat.buffer_len = 0;

	// Clear packet queue
	nat.pkt_queue_head = 0;
	nat.pkt_queue_tail = 0;
	nat.pkt_queue_count = 0;
}

void
network_nat_poll(void)
{
	fd_set rfds, wfds, efds;
	int fd_max, ret;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	fd_max = -1;

	slirp_select_fill(nat.slirp, &fd_max, &rfds, &wfds, &efds);
	if (fd_max == -1) {
		return;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	ret = select(fd_max + 1, &rfds, &wfds, &efds, &tv);
	if (ret < 0) {
		return;
	}

	slirp_select_poll(nat.slirp, &rfds, &wfds, &efds, ret <= 0);

	// Poll for incoming broadcasts from host network
	broadcast_relay_poll();
}

/**
 * Transmit data to the network
 *
 * @param errbuf    Address of buffer to return error string
 * @param mbufs     Address of mbuf chain containing data to send
 * @param dest      Address of destination MAC address
 * @param src       Address of source MAC address, or 0 to use default
 * @param frametype EtherType of frame
 *
 * @return errbuf on error, else zero
 */
uint32_t
network_nat_tx(uint32_t errbuf, uint32_t mbufs, uint32_t dest, uint32_t src, uint32_t frametype)
{
	uint8_t *buf = nat.tx_buffer;
	struct ro_mbuf_part txb;
	uint32_t packet_length;

	memcpytohost(buf, dest, 6);
	buf += 6;

	if (src != 0) {
		memcpytohost(buf, src, 6);
	} else {
		memcpy(buf, network_hwaddr, 6);
	}
	buf += 6;

	*buf++ = (uint8_t) (frametype >> 8);
	*buf++ = (uint8_t) frametype;

	packet_length = HEADERLEN;

	// Copy the mbuf chain as the payload
	while (mbufs != 0) {
		memcpytohost(&txb, mbufs, sizeof(txb));
		packet_length += txb.m_len;
		if (packet_length > sizeof(nat.tx_buffer)) {
			strcpyfromhost(errbuf, "RPCEmu: Packet too large to send");
			return errbuf;
		}
		memcpytohost(buf, mbufs + txb.m_off, txb.m_len);
		buf += txb.m_len;
		mbufs = txb.m_next;
	}

	// Write to capture file for debug
	write_packet(nat.capture, nat.tx_buffer, packet_length);

	// Offer the packet to the Access+/ShareFS broadcast relay. If it returns
	// non-zero it has taken full ownership of the packet (an external unicast
	// relayed to the host network); passing it to SLiRP as well would send a
	// duplicate out of SLiRP's NAT with a masqueraded source port, splitting
	// the ShareFS conversation across two ports and stalling disc opens.
	// Broadcasts and non-Access traffic return zero and still go to SLiRP.
	if (!broadcast_relay_tx(nat.tx_buffer, packet_length)) {
		slirp_input(nat.slirp, nat.tx_buffer, packet_length);
	}

	return 0;
}

/**
 * Receive data from the network
 *
 * A frame that cannot be delivered is dropped and reported to the log rather
 * than raised to the guest as an error: see the comment in the body for why
 * returning an error here was worse than losing the frame. Nothing is left for
 * the caller to report, so errbuf goes unused.
 *
 * @param errbuf     Address of buffer to return error string (unused)
 * @param mbuf       Address of mbuf to hold received payload
 * @param rxhdr      Address of mbuf to hold received header
 * @param data_avail Address of flag to return indication of data available
 *
 * @return Always zero
 */
uint32_t
network_nat_rx(uint32_t errbuf, uint32_t mbuf, uint32_t rxhdr, uint32_t *data_avail)
{
	struct ro_mbuf_part rxb;
	struct rx_hdr hdr;
	size_t packet_length;

	NOT_USED(errbuf);

	*data_avail = 0;

	/* Promote a queued frame, in case the slot fell empty without one
	   following it in */
	if (nat.buffer_len == 0) {
		deliver_queued_packet();
	}

	if (nat.buffer_len == 0) {
		// No data
		return 0;
	}

	/*
	 * From here the frame is consumed whatever happens to it. Every path that
	 * left it in the slot and returned an error stopped slirp_can_output()
	 * from ever returning true again, so a single undeliverable frame took the
	 * machine's networking down for the rest of the session, silently and with
	 * no way back short of a reset. Dropping the frame costs a
	 * retransmission.
	 */
	packet_length = nat.buffer_len;
	nat.buffer_len = 0;

	if (mbuf == 0 || packet_length <= HEADERLEN) {
		// Nowhere to put it, or nothing in it beyond the header
		rpclog("Network: dropped a received frame of %zu bytes (%s)\n",
		       packet_length,
		       (mbuf == 0) ? "no mbuf supplied" : "no payload");
		deliver_queued_packet();
		return 0;
	}

	memset(&hdr, 0, sizeof(hdr));

	// Fill in received header structure
	memcpy(hdr.rx_dst_addr, nat.buffer + 0, 6);
	memcpy(hdr.rx_src_addr, nat.buffer + 6, 6);
	hdr.rx_frame_type = (nat.buffer[12] << 8) | nat.buffer[13];
	hdr.rx_error_level = 0;

	packet_length -= HEADERLEN;

	memcpytohost(&rxb, mbuf, sizeof(rxb));

	if (packet_length > rxb.m_inilen) {
		// Mbuf too small for received packet
		rpclog("Network: dropped a received frame, its %zu byte payload does "
		       "not fit the guest's %u byte mbuf\n",
		       packet_length, (unsigned) rxb.m_inilen);
		deliver_queued_packet();
		return 0;
	}

	/* Only now that the frame is known to be deliverable, so a dropped one
	   leaves the guest's header untouched rather than half filled in */
	memcpyfromhost(rxhdr, &hdr, sizeof(hdr));

	// Copy payload in to the mbuf
	rxb.m_off = rxb.m_inioff;
	memcpyfromhost(mbuf + rxb.m_off, nat.buffer + HEADERLEN, packet_length);
	rxb.m_len = packet_length;
	memcpyfromhost(mbuf, &rxb, sizeof(rxb));

	*data_avail = 1;

	// Try to deliver next queued packet
	deliver_queued_packet();

	return 0;
}

/**
 * @param address
 */
void
network_nat_setirqstatus(uint32_t address)
{
	nat.irq_status = address;
}

/**
 * Add a forwarding rule to the NAT, and activate it
 *
 * @param rule Details of NAT rule
 */
void
network_nat_forward_add(PortForwardRule rule)
{
	struct in_addr bind = { 0 };
	int retval;

	// Inform SLIRP of the rule added
	retval = slirp_add_hostfwd(nat.slirp, rule.type == PORT_FORWARD_UDP ? 1 : 0,
	    bind, rule.host_port, nat.forward_addr, rule.emu_port);
	if (retval != 0) {
		error("Failed to add NAT Network port forwarding rule, %s emu_port %u host_port %u, %d %d %s",
		    rule.type == PORT_FORWARD_UDP ? "UDP" : "TCP", rule.emu_port, rule.host_port,
		    retval, errno, strerror(errno));
	}
}

/**
 * Remove a forwarding rule in the NAT, and deactivate it
 *
 * @param rule Details of NAT rule
 */
void
network_nat_forward_remove(PortForwardRule rule)
{
	struct in_addr bind = { 0 };

	// Inform SLIRP of the rule removal
	slirp_remove_hostfwd(nat.slirp, rule.type == PORT_FORWARD_UDP ? 1 : 0,
	    bind, rule.host_port);
}

/**
 * Edit an existing forwarding rule in the NAT, and deactivate and reactive it
 *
 * @param old_rule Details of NAT rule being replaced
 * @param new_rule Details of NAT rule replacement
 */
void
network_nat_forward_edit(PortForwardRule old_rule, PortForwardRule new_rule)
{
	network_nat_forward_remove(old_rule);
	network_nat_forward_add(new_rule);
}

/**
 * Shutdown NAT networking and release resources.
 */
void
network_nat_close(void)
{
	broadcast_relay_close();
	net_slot_release();

	if (nat.capture != NULL) {
		fclose(nat.capture);
		nat.capture = NULL;
	}

	// Note: SLiRP cleanup would go here if needed
}

/**
 * Try to deliver a queued packet to the guest.
 * Called when the main buffer becomes available.
 */
static void
deliver_queued_packet(void)
{
	queued_packet_t *pkt;

	if (nat.pkt_queue_count == 0) {
		return;  // No packets queued
	}

	if (nat.buffer_len != 0) {
		return;  // Buffer still busy
	}

	// Get packet from queue
	pkt = &nat.pkt_queue[nat.pkt_queue_tail];

	// Copy to main buffer. Already written to the capture file on arrival.
	memcpy(nat.buffer, pkt->data, pkt->len);
	nat.buffer_len = pkt->len;

	// Advance tail
	nat.pkt_queue_tail = (nat.pkt_queue_tail + 1) % PKT_QUEUE_SIZE;
	nat.pkt_queue_count--;

	network_irq_raise();
}

/**
 * Report how many more packets can currently be accepted for delivery to the
 * guest.
 *
 * A caller that must deliver a set of packets all-or-nothing (the relay's IP
 * fragments - a partial datagram cannot be reassembled by the guest) can check
 * there is room for the whole set before injecting any of it.
 *
 * @return Number of free slots (0 if networking is not ready to receive)
 */
int
network_nat_inject_space(void)
{
	if (nat.irq_status == 0 || network_poduleinfo == NULL) {
		return 0;
	}

	return rx_space();
}

/**
 * Inject a packet into the guest network.
 * Used by broadcast relay to deliver packets from host network.
 * Packets are queued if the main buffer is busy.
 *
 * @param pkt     Complete Ethernet frame
 * @param pkt_len Length of frame in bytes
 *
 * @return 1 if packet was queued, 0 if queue was full
 */
int
network_nat_inject_packet(const uint8_t *pkt, int pkt_len)
{
	if (pkt_len <= 0) {
		return 0;
	}

	return rx_deliver(pkt, (size_t) pkt_len);
}

/**
 * Save the guest-visible NAT networking state to a suspend snapshot.
 *
 * The only piece of host-side state the guest cannot re-establish itself on
 * resume is irq_status: the address of the word in guest RAM used to signal
 * received packets. The driver registers it once at init and never again, so
 * without restoring it the emulator can no longer deliver packets to a
 * resumed guest (every receive path bails on irq_status == 0). Slirp, the
 * packet buffer and the relay queue are transient and rebuilt from scratch.
 */
void
network_nat_savestate(FILE *f)
{
	savestate_write_u32(f, nat.irq_status);
}

void
network_nat_loadstate(FILE *f)
{
	uint32_t address = savestate_read_u32(f);

	if (address != 0) {
		network_nat_setirqstatus(address);
	}
}
