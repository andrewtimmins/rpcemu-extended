/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

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
 * broadcast_relay.c - Access+/ShareFS networking for NAT mode
 *
 * Provides Access+ file sharing support over RPCEmu's NAT networking.
 * Access+ discovery uses UDP broadcasts which don't traverse NAT, so
 * this module bridges traffic between the guest's virtual network
 * (10.10.10.x) and the host's physical network.
 *
 * UDP ports handled:
 *   32770 - Discovery and share announcements (broadcast)
 *   32771 - Share management
 *   49171 - File operations
 *
 * Outgoing broadcasts are relayed to the host network; incoming
 * responses are repackaged and injected into the guest. Payloads
 * exceeding Ethernet MTU are delivered via IP fragmentation.
 */

#include <errno.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "broadcast_relay.h"
#include "rpcemu.h"
#include "network.h"
#include "network-nat.h"

/* Access+ ports */
#define ACCESS_PORT_ANNOUNCE    32770
#define ACCESS_PORT_SHARE       32771
#define ACCESS_PORT_POLL        49171

/* Number of Access+ sockets (one per port) */
#define NUM_ACCESS_SOCKETS      3

/* Ethernet constants */
#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_P_IP        0x0800

/* IP constants */
#define IP_PROTO_UDP    17
#define IP_HDR_LEN      20

/* Rate limiting */
#define MAX_PACKETS_PER_SECOND  100

/* SLiRP network constants */
#define SLIRP_NET       0x0a0a0a00  /* 10.10.10.0 */
#define SLIRP_MASK      0xffffff00  /* 255.255.255.0 */
#define SLIRP_BROADCAST 0x0a0a0aff  /* 10.10.10.255 */
#define SLIRP_HOST      0x0a0a0a02  /* 10.10.10.2 (gateway) */

/* Broadcast MAC address */
static const uint8_t broadcast_mac[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* 
 * Gateway/external MAC address - used as source for packets from outside.
 * This matches what SLiRP uses internally for the virtual gateway.
 * Format: 52:54:00:xx:xx:xx is QEMU/SLiRP convention
 */
static const uint8_t gateway_mac[ETH_ALEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

/* Port configuration for each socket */
static const uint16_t access_ports[NUM_ACCESS_SOCKETS] = {
    ACCESS_PORT_ANNOUNCE,   /* 32770 - broadcasts */
    ACCESS_PORT_SHARE,      /* 32771 - share management */
    ACCESS_PORT_POLL        /* 49171 - file operations */
};

/* Relay state */
typedef struct {
    int sockets[NUM_ACCESS_SOCKETS]; /* UDP sockets, -1 if disabled */
    int enabled;                     /* Runtime enable flag */

    struct sockaddr_in host_addr;    /* Host's IP address */
    struct sockaddr_in bcast_addr;   /* Subnet broadcast address */

    /* Learned guest IP from outgoing packets */
    uint32_t guest_ip;               /* Guest's IP in host byte order, 0 if unknown */

    /* Rate limiting */
    uint32_t packets_this_second;
    time_t last_rate_reset;

    /* Statistics */
    uint32_t tx_count;              /* Guest -> Host */
    uint32_t rx_count;              /* Host -> Guest */
    uint32_t dropped;               /* Rate limited or errors */
} relay_state_t;

static relay_state_t relay = {
    .sockets = {-1, -1, -1},
    .enabled = 0
};

/**
 * Find a socket index for a given port.
 * Returns -1 if port not in our list.
 */
static int
find_socket_for_port(uint16_t port)
{
    int i;
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        if (access_ports[i] == port) {
            return i;
        }
    }
    return -1;
}

/**
 * Find the broadcast address for the first suitable network interface.
 * Skips loopback and interfaces without broadcast capability.
 */
static int
get_broadcast_address(struct in_addr *bcast, struct in_addr *host)
{
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (getifaddrs(&ifaddr) < 0) {
        rpclog("broadcast_relay: getifaddrs() failed: %s\n", strerror(errno));
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        /* Only interested in IPv4 */
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        /* Skip loopback */
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }

        /* Must support broadcast */
        if (!(ifa->ifa_flags & IFF_BROADCAST)) {
            continue;
        }

        /* Must be up */
        if (!(ifa->ifa_flags & IFF_UP)) {
            continue;
        }

        /* Get broadcast address */
        if (ifa->ifa_ifu.ifu_broadaddr != NULL) {
            struct sockaddr_in *bcast_sa = (struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr;
            struct sockaddr_in *host_sa = (struct sockaddr_in *)ifa->ifa_addr;

            *bcast = bcast_sa->sin_addr;
            *host = host_sa->sin_addr;
            found = 1;

            rpclog("broadcast_relay: using interface %s, host %s, ",
                   ifa->ifa_name, inet_ntoa(*host));
            rpclog("broadcast %s\n", inet_ntoa(*bcast));
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found ? 0 : -1;
}

/**
 * Initialize the broadcast relay.
 */
int
broadcast_relay_init(void)
{
    int optval = 1;
    struct sockaddr_in bind_addr;
    int flags;
    struct in_addr bcast, host;
    int i;
    int success_count = 0;

    /* Find host's broadcast address */
    if (get_broadcast_address(&bcast, &host) < 0) {
        rpclog("broadcast_relay: no suitable network interface found\n");
        return -1;
    }

    /* Create UDP sockets for each Access+ port */
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        relay.sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (relay.sockets[i] < 0) {
            rpclog("broadcast_relay: socket() for port %d failed: %s\n",
                   access_ports[i], strerror(errno));
            continue;
        }

        /* Enable broadcast */
        if (setsockopt(relay.sockets[i], SOL_SOCKET, SO_BROADCAST,
                       &optval, sizeof(optval)) < 0) {
            rpclog("broadcast_relay: SO_BROADCAST for port %d failed: %s\n",
                   access_ports[i], strerror(errno));
            close(relay.sockets[i]);
            relay.sockets[i] = -1;
            continue;
        }

        /* Allow address reuse (in case of restart) */
        if (setsockopt(relay.sockets[i], SOL_SOCKET, SO_REUSEADDR,
                       &optval, sizeof(optval)) < 0) {
            rpclog("broadcast_relay: SO_REUSEADDR for port %d failed: %s\n",
                   access_ports[i], strerror(errno));
            close(relay.sockets[i]);
            relay.sockets[i] = -1;
            continue;
        }

        /* Bind to this port */
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(access_ports[i]);
        bind_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(relay.sockets[i], (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            rpclog("broadcast_relay: bind() for port %d failed: %s\n",
                   access_ports[i], strerror(errno));
            close(relay.sockets[i]);
            relay.sockets[i] = -1;
            continue;
        }

        /* Set non-blocking */
        flags = fcntl(relay.sockets[i], F_GETFL, 0);
        if (flags < 0 || fcntl(relay.sockets[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            rpclog("broadcast_relay: fcntl() for port %d failed: %s\n",
                   access_ports[i], strerror(errno));
            close(relay.sockets[i]);
            relay.sockets[i] = -1;
            continue;
        }

        success_count++;
    }

    if (success_count == 0) {
        rpclog("broadcast_relay: failed to bind any ports\n");
        return -1;
    }

    /* Store addresses for later use */
    memset(&relay.bcast_addr, 0, sizeof(relay.bcast_addr));
    relay.bcast_addr.sin_family = AF_INET;
    relay.bcast_addr.sin_port = htons(ACCESS_PORT_ANNOUNCE);
    relay.bcast_addr.sin_addr = bcast;

    memset(&relay.host_addr, 0, sizeof(relay.host_addr));
    relay.host_addr.sin_family = AF_INET;
    relay.host_addr.sin_addr = host;

    /* Initialize stats and guest IP tracking */
    relay.tx_count = 0;
    relay.rx_count = 0;
    relay.dropped = 0;
    relay.packets_this_second = 0;
    relay.last_rate_reset = time(NULL);
    relay.guest_ip = 0;  /* Will be learned from first outgoing packet */

    relay.enabled = 1;

    return 0;
}

/**
 * Shutdown the broadcast relay.
 */
void
broadcast_relay_close(void)
{
    int i;
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        if (relay.sockets[i] >= 0) {
            close(relay.sockets[i]);
            relay.sockets[i] = -1;
        }
    }
    relay.enabled = 0;
}

/**
 * Compute IP header checksum.
 */
static uint16_t
ip_checksum(const uint8_t *hdr, int len)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < len; i += 2) {
        sum += (hdr[i] << 8) | hdr[i + 1];
    }

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* IP identification counter for fragmentation - start high to avoid conflicts */
static uint16_t ip_id_counter = 0x8000;

/**
 * Inject a large UDP payload as multiple IP fragments.
 * This is needed when the payload exceeds Ethernet MTU.
 * 
 * Returns: number of fragments injected, or 0 on error
 */
static int
inject_fragmented_udp(const struct sockaddr_in *from,
                      uint16_t dest_port,
                      const uint8_t *payload, int payload_len,
                      int is_broadcast)
{
    uint8_t frame[1600];  /* Enough for one Ethernet frame */
    uint8_t *ip;
    uint8_t *data;
    uint32_t dest_ip;
    uint32_t src_ip;
    uint16_t ip_id;
    int offset;
    int frag_count = 0;
    int total_ip_payload;
    int max_data_per_frag;
    uint16_t cksum;
    const uint32_t SLIRP_GUEST_DEFAULT = 0x0a0a0a0f;

    /* Ethernet MTU is 1500, IP header is 20, so max IP payload per fragment is 1480 */
    /* Fragment offset must be multiple of 8, so use 1480 (divisible by 8) */
    max_data_per_frag = 1480;

    /* Total IP payload = UDP header (8) + UDP payload */
    total_ip_payload = 8 + payload_len;

    /* Get destination IP */
    if (is_broadcast) {
        dest_ip = SLIRP_BROADCAST;
    } else {
        dest_ip = relay.guest_ip ? relay.guest_ip : SLIRP_GUEST_DEFAULT;
    }

    /* Source IP: use SLiRP gateway (10.10.10.2) instead of real sender IP
     * This keeps everything within the virtual network which may help with
     * fragment reassembly on the guest side */
    src_ip = 0x0a0a0a02;  /* 10.10.10.2 - SLiRP gateway */

    /* Get unique IP ID for this datagram */
    ip_id = ip_id_counter++;

    /* First fragment includes UDP header */
    offset = 0;
    while (offset < total_ip_payload) {
        int frag_data_len;
        int is_first = (offset == 0);
        int is_last;
        int ip_payload_len;
        int frame_len;
        uint16_t flags_frag;

        /* Calculate how much data in this fragment */
        frag_data_len = total_ip_payload - offset;
        if (frag_data_len > max_data_per_frag) {
            /* Round down to multiple of 8 */
            frag_data_len = max_data_per_frag & ~7;
        }
        is_last = (offset + frag_data_len >= total_ip_payload);

        ip_payload_len = frag_data_len;
        frame_len = ETH_HLEN + IP_HDR_LEN + ip_payload_len;

        /* Ethernet header */
        if (is_broadcast) {
            memcpy(frame, broadcast_mac, ETH_ALEN);
        } else {
            memcpy(frame, network_hwaddr, ETH_ALEN);
        }
        memcpy(frame + ETH_ALEN, gateway_mac, ETH_ALEN);
        frame[12] = ETH_P_IP >> 8;
        frame[13] = ETH_P_IP & 0xff;

        /* IP header */
        ip = frame + ETH_HLEN;
        ip[0] = 0x45;  /* Version 4, IHL 5 */
        ip[1] = 0x00;
        ip[2] = (IP_HDR_LEN + ip_payload_len) >> 8;
        ip[3] = (IP_HDR_LEN + ip_payload_len) & 0xff;
        ip[4] = ip_id >> 8;
        ip[5] = ip_id & 0xff;

        /* Flags and fragment offset (in 8-byte units) */
        flags_frag = (offset / 8) & 0x1fff;
        if (!is_last) {
            flags_frag |= 0x2000;  /* More Fragments flag */
        }
        ip[6] = flags_frag >> 8;
        ip[7] = flags_frag & 0xff;

        ip[8] = 64;  /* TTL */
        ip[9] = IP_PROTO_UDP;
        ip[10] = 0; ip[11] = 0;  /* Checksum placeholder */

        /* Source IP */
        ip[12] = (src_ip >> 24) & 0xff;
        ip[13] = (src_ip >> 16) & 0xff;
        ip[14] = (src_ip >> 8) & 0xff;
        ip[15] = src_ip & 0xff;

        /* Dest IP */
        ip[16] = (dest_ip >> 24) & 0xff;
        ip[17] = (dest_ip >> 16) & 0xff;
        ip[18] = (dest_ip >> 8) & 0xff;
        ip[19] = dest_ip & 0xff;

        /* IP checksum */
        cksum = ip_checksum(ip, IP_HDR_LEN);
        ip[10] = cksum >> 8;
        ip[11] = cksum & 0xff;

        /* Fragment data */
        data = ip + IP_HDR_LEN;

        if (is_first) {
            /* First fragment: UDP header + start of payload */
            uint16_t udp_len = 8 + payload_len;  /* Total UDP length */
            int udp_data_in_frag = frag_data_len - 8;

            /* UDP header */
            data[0] = (ntohs(from->sin_port) >> 8) & 0xff;
            data[1] = ntohs(from->sin_port) & 0xff;
            data[2] = dest_port >> 8;
            data[3] = dest_port & 0xff;
            data[4] = udp_len >> 8;
            data[5] = udp_len & 0xff;
            data[6] = 0; data[7] = 0;  /* No UDP checksum */

            /* Copy UDP payload data for this fragment */
            if (udp_data_in_frag > 0) {
                memcpy(data + 8, payload, udp_data_in_frag);
            }
        } else {
            /* Subsequent fragments: just payload data */
            int payload_offset = offset - 8;  /* Offset into UDP payload */
            memcpy(data, payload + payload_offset, frag_data_len);
        }

        /* Inject this fragment */
        if (!network_nat_inject_packet(frame, frame_len)) {
            return frag_count;
        }

        frag_count++;
        offset += frag_data_len;
    }

    return frag_count;
}

/**
 * Build an Ethernet frame to inject into the guest.
 * Takes a UDP payload received from the host network and wraps it
 * in Ethernet + IP + UDP headers for the SLiRP virtual network.
 *
 * For broadcasts (is_broadcast=1), dest IP is 10.10.10.255
 * For unicasts (is_broadcast=0), use learned guest IP or fallback to .15
 */
static int
build_guest_frame(uint8_t *frame, int max_len,
                  const struct sockaddr_in *from,
                  uint16_t dest_port,
                  const uint8_t *payload, int payload_len,
                  int is_broadcast)
{
    int total_len;
    uint8_t *ip;
    uint8_t *udp;
    uint16_t ip_len;
    uint16_t udp_len;
    uint16_t cksum;
    uint32_t dest_ip;
    /* Default guest IP if we haven't learned it yet */
    const uint32_t SLIRP_GUEST_DEFAULT = 0x0a0a0a0f;  /* 10.10.10.15 */

    /* Calculate total frame size */
    total_len = ETH_HLEN + IP_HDR_LEN + 8 + payload_len;

    if (total_len > max_len) {
        return -1;
    }

    /* Destination IP depends on whether this is broadcast or unicast */
    if (is_broadcast) {
        dest_ip = SLIRP_BROADCAST;
    } else {
        /* Use learned guest IP, or default */
        dest_ip = relay.guest_ip ? relay.guest_ip : SLIRP_GUEST_DEFAULT;
    }

    /* Ethernet header */
    if (is_broadcast) {
        memcpy(frame, broadcast_mac, ETH_ALEN);    /* Dest: broadcast */
        memcpy(frame + ETH_ALEN, gateway_mac, ETH_ALEN);  /* Src: gateway MAC */
    } else {
        memcpy(frame, network_hwaddr, ETH_ALEN);   /* Dest: guest MAC */
        memcpy(frame + ETH_ALEN, gateway_mac, ETH_ALEN);  /* Src: gateway MAC */
    }
    frame[12] = ETH_P_IP >> 8;                     /* EtherType: IPv4 */
    frame[13] = ETH_P_IP & 0xff;

    /* IP header */
    ip = frame + ETH_HLEN;
    ip[0] = 0x45;                                  /* Version 4, IHL 5 (20 bytes) */
    ip[1] = 0x00;                                  /* DSCP/ECN */
    ip_len = IP_HDR_LEN + 8 + payload_len;
    ip[2] = ip_len >> 8;
    ip[3] = ip_len & 0xff;
    ip[4] = 0x00; ip[5] = 0x00;                    /* ID */
    ip[6] = 0x00; ip[7] = 0x00;                    /* Flags/Fragment */
    ip[8] = 64;                                    /* TTL */
    ip[9] = IP_PROTO_UDP;                          /* Protocol */
    ip[10] = 0x00; ip[11] = 0x00;                  /* Checksum (computed below) */

    /* Source IP: use the real sender's IP so replies work */
    memcpy(ip + 12, &from->sin_addr, 4);

    /* Dest IP */
    ip[16] = (dest_ip >> 24) & 0xff;
    ip[17] = (dest_ip >> 16) & 0xff;
    ip[18] = (dest_ip >> 8) & 0xff;
    ip[19] = dest_ip & 0xff;

    /* Compute IP checksum */
    cksum = ip_checksum(ip, IP_HDR_LEN);
    ip[10] = cksum >> 8;
    ip[11] = cksum & 0xff;

    /* UDP header */
    udp = ip + IP_HDR_LEN;
    udp[0] = (ntohs(from->sin_port) >> 8) & 0xff;  /* Source port */
    udp[1] = ntohs(from->sin_port) & 0xff;
    udp[2] = dest_port >> 8;                       /* Dest port */
    udp[3] = dest_port & 0xff;
    udp_len = 8 + payload_len;
    udp[4] = udp_len >> 8;
    udp[5] = udp_len & 0xff;
    udp[6] = 0x00; udp[7] = 0x00;                  /* Checksum (optional for IPv4) */

    /* Payload */
    memcpy(udp + 8, payload, payload_len);

    return total_len;
}

/**
 * Poll a single socket for incoming packets.
 * Returns 1 if a packet was processed, 0 otherwise.
 */
static int
poll_socket(int sock_idx)
{
    /* 
     * Buffer sizes: 
     * - UDP receive buffer: 8192 to avoid truncation (UDP datagrams can be up to 65535)
     * - Frame buffer: must fit in nat.buffer (2048 bytes) after adding headers
     * - Max UDP payload we can inject: 2048 - 14(eth) - 20(ip) - 8(udp) = 2006 bytes
     */
    static uint8_t udp_payload[8192];  /* Static to avoid stack overflow */
    uint8_t frame[2048];
    struct sockaddr_in from;
    socklen_t fromlen;
    ssize_t n;
    int frame_len;
    uint16_t port;
    int is_broadcast;
    time_t now;

    if (relay.sockets[sock_idx] < 0) {
        return 0;
    }

    port = access_ports[sock_idx];

    /* Non-blocking receive */
    fromlen = sizeof(from);
    n = recvfrom(relay.sockets[sock_idx], udp_payload, sizeof(udp_payload), MSG_DONTWAIT,
                 (struct sockaddr *)&from, &fromlen);

    if (n <= 0) {
        return 0;  /* No data or error */
    }

    /* Don't relay packets from localhost - these are SLiRP loopback */
    if (ntohl(from.sin_addr.s_addr) == INADDR_LOOPBACK) {
        return 0;
    }

    /* Don't relay our own packets back (from host IP) */
    if (from.sin_addr.s_addr == relay.host_addr.sin_addr.s_addr) {
        return 0;
    }

    /* Rate limiting */
    now = time(NULL);
    if (now != relay.last_rate_reset) {
        relay.packets_this_second = 0;
        relay.last_rate_reset = now;
    }
    if (relay.packets_this_second >= MAX_PACKETS_PER_SECOND) {
        relay.dropped++;
        return 0;
    }
    relay.packets_this_second++;

    /* Determine if this should be delivered as broadcast or unicast */
    /* Port 32770 uses broadcast for discovery, others are unicast responses */
    is_broadcast = (port == ACCESS_PORT_ANNOUNCE);

    /* Check if payload is too large for single Ethernet frame */
    /* Max UDP payload in single frame: 1500 - 20(IP) - 8(UDP) = 1472 */
    if (n > 1472) {
        /* Use IP fragmentation for large payloads */
        int frags = inject_fragmented_udp(&from, port, udp_payload, n, is_broadcast);
        if (frags > 0) {
            relay.rx_count++;
            return 1;
        } else {
            relay.dropped++;
            return 0;
        }
    }

    /* Build frame for guest (single unfragmented frame) */
    frame_len = build_guest_frame(frame, sizeof(frame), &from, port, udp_payload, n, is_broadcast);
    if (frame_len < 0) {
        relay.dropped++;
        return 0;
    }

    /* Inject into guest via direct buffer delivery */
    if (network_nat_inject_packet(frame, frame_len)) {
        relay.rx_count++;
        return 1;
    } else {
        relay.dropped++;
        return 0;
    }
}

/**
 * Poll for incoming packets from the host network on all Access+ ports.
 */
void
broadcast_relay_poll(void)
{
    int i;

    if (!relay.enabled) {
        return;
    }

    /* Poll all sockets */
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        poll_socket(i);
    }
}

/**
 * Check if an outgoing packet is Access+ traffic we should relay.
 * Handles both broadcasts (discovery) and unicast (file operations).
 */
int
broadcast_relay_tx(const uint8_t *pkt, int pkt_len)
{
    const uint8_t *ip_hdr;
    const uint8_t *udp_hdr;
    uint16_t ethertype;
    uint8_t ip_proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t dst_ip;
    int ip_hdr_len;
    int udp_len;
    int payload_len;
    const uint8_t *payload;
    struct sockaddr_in dest;
    ssize_t sent;
    time_t now;
    int sock_idx;
    int is_broadcast;
    int is_external_unicast;

    if (!relay.enabled) {
        return 0;
    }

    /* Minimum: Ethernet(14) + IP(20) + UDP(8) = 42 bytes */
    if (pkt_len < 42) {
        return 0;
    }

    /* Check EtherType is IPv4 */
    ethertype = (pkt[12] << 8) | pkt[13];
    if (ethertype != ETH_P_IP) {
        return 0;
    }

    /* Parse IP header */
    ip_hdr = pkt + ETH_HLEN;
    ip_hdr_len = (ip_hdr[0] & 0x0f) * 4;
    ip_proto = ip_hdr[9];

    if (ip_proto != IP_PROTO_UDP) {
        return 0;
    }

    /* Get source IP (guest's IP) and learn it for responses */
    {
        uint32_t src_ip = ((uint32_t)ip_hdr[12] << 24) | ((uint32_t)ip_hdr[13] << 16) |
                          ((uint32_t)ip_hdr[14] << 8) | (uint32_t)ip_hdr[15];
        /* Only learn if it's in SLiRP range and not broadcast */
        if ((src_ip & SLIRP_MASK) == SLIRP_NET && src_ip != SLIRP_BROADCAST) {
            relay.guest_ip = src_ip;
        }
    }

    /* Get destination IP */
    dst_ip = ((uint32_t)ip_hdr[16] << 24) | ((uint32_t)ip_hdr[17] << 16) |
             ((uint32_t)ip_hdr[18] << 8) | (uint32_t)ip_hdr[19];

    /* Check if this is a broadcast */
    is_broadcast = (memcmp(pkt, broadcast_mac, ETH_ALEN) == 0) ||
                   (dst_ip == 0xffffffff) ||
                   (dst_ip == SLIRP_BROADCAST);

    /* Check if this is unicast to an external (non-SLiRP) IP */
    is_external_unicast = !is_broadcast &&
                          ((dst_ip & SLIRP_MASK) != SLIRP_NET);

    /* Parse UDP header */
    udp_hdr = ip_hdr + ip_hdr_len;
    src_port = (udp_hdr[0] << 8) | udp_hdr[1];
    dst_port = (udp_hdr[2] << 8) | udp_hdr[3];

    /* Check if this is an Access+ port we handle */
    sock_idx = find_socket_for_port(dst_port);

    /* For broadcasts, only handle if it's an Access+ port */
    /* For unicast to external IPs, also check source port for Access+ */
    if (sock_idx < 0) {
        /* Check source port for unicast responses */
        sock_idx = find_socket_for_port(src_port);
        if (sock_idx < 0) {
            return 0;  /* Not Access+ traffic */
        }
    }

    /* Only handle broadcasts or external unicasts */
    if (!is_broadcast && !is_external_unicast) {
        return 0;  /* Let SLiRP handle internal traffic */
    }

    /* Make sure we have a socket for this port */
    if (relay.sockets[sock_idx] < 0) {
        return 0;
    }

    /* Rate limiting */
    now = time(NULL);
    if (now != relay.last_rate_reset) {
        relay.packets_this_second = 0;
        relay.last_rate_reset = now;
    }
    if (relay.packets_this_second >= MAX_PACKETS_PER_SECOND) {
        relay.dropped++;
        return 1;  /* Handled (dropped) */
    }
    relay.packets_this_second++;

    /* Extract UDP payload */
    udp_len = (udp_hdr[4] << 8) | udp_hdr[5];
    payload_len = udp_len - 8;
    payload = udp_hdr + 8;

    if (payload_len <= 0 || payload_len > 1500) {
        return 0;
    }

    /* Set up destination address */
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dst_port);

    if (is_broadcast) {
        /* Send to network broadcast address */
        dest.sin_addr = relay.bcast_addr.sin_addr;
    } else {
        /* Send to the actual destination IP */
        dest.sin_addr.s_addr = htonl(dst_ip);
    }

    /* Send from the appropriate socket (bound to correct source port) */
    sent = sendto(relay.sockets[sock_idx], payload, payload_len, 0,
                  (struct sockaddr *)&dest, sizeof(dest));

    if (sent < 0) {
        relay.dropped++;
    } else {
        relay.tx_count++;
    }

    /* Return 1 for external unicast (we handle it completely) */
    /* Return 0 for broadcast (let SLiRP see it too for local loopback) */
    return is_external_unicast ? 1 : 0;
}

/**
 * Get relay statistics.
 */
void
broadcast_relay_stats(uint32_t *tx_count, uint32_t *rx_count, uint32_t *dropped)
{
    if (tx_count) *tx_count = relay.tx_count;
    if (rx_count) *rx_count = relay.rx_count;
    if (dropped) *dropped = relay.dropped;
}
