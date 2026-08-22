/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andy Timmins

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

#include "socket-compat.h"

#ifdef _WIN32

#include <iphlpapi.h>

#define relay_closesocket(s) closesocket(s)
#define RELAY_WOULDBLOCK     WSAEWOULDBLOCK
#define sock_strerror()      relay_sock_strerror()

#else

#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>

#define relay_closesocket(s) close(s)
#define RELAY_WOULDBLOCK     EWOULDBLOCK
#define sock_strerror() strerror(errno)

#endif

typedef int relay_socket_t;
#define RELAY_INVALID_SOCKET (-1)
#define RELAY_SOCKET_ERROR   (-1)

#include "broadcast_relay.h"
#include "rpcemu.h"
#include "network.h"
#include "network-nat.h"

#ifdef _WIN32
/* Winsock reports errors via WSAGetLastError(), not errno. Format the code
   for the (diagnostic-only) log messages. */
static const char *
relay_sock_strerror(void)
{
	static char buf[32];

	snprintf(buf, sizeof(buf), "winsock error %d", WSAGetLastError());
	return buf;
}
#endif

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

/*
 * How much of an arriving datagram the relay can read.
 *
 * This must be at least the largest UDP payload IP can carry, because recvfrom()
 * on a datagram socket copies what fits and throws the remainder away, returning
 * the buffer size - so a buffer one byte short truncates and reports success.
 * See the note at the receive buffer itself.
 */
#define RELAY_RECV_CAPACITY 65535
#define RELAY_MAX_LEGAL_UDP_PAYLOAD (65535 - IP_HDR_LEN - 8)

_Static_assert(RELAY_RECV_CAPACITY >= RELAY_MAX_LEGAL_UDP_PAYLOAD,
    "the relay's receive buffer must hold the largest datagram a peer can send, "
    "or recvfrom() truncates it silently");

/*
 * Rate limiting, in two budgets, because the two kinds of traffic here want
 * opposite things.
 *
 * ★ There used to be one budget of 100 packets a second for everything, which
 * is generous for discovery and hopeless for a file transfer: roughly 140KB/s at
 * full MTU, and that is the ceiling however small the datagrams are, because the
 * budget counts packets rather than bytes. (Fragments are charged once per
 * datagram, not once each - the paths that fragment or reassemble spend from the
 * budget one datagram at a time.) A small file fitted under it and a zip did
 * not, so a share would mount and then stall - and every dropped packet was
 * silent. Measured with a fake LAN peer at 200 packets a second: exactly 100
 * delivered, 100 discarded, every second.
 *
 * Discovery keeps a cap. A relayed broadcast really can multiply - guest to LAN,
 * LAN to the other emulated machines, and back - so a storm is a genuine risk
 * and 100 a second is far more than Access+ announcements need.
 *
 * File traffic gets a backstop rather than a throttle. It is set high enough
 * that nothing RISC OS can generate over an emulated 10Mbit card will reach it
 * (20,000 packets a second is around 28MB/s at full MTU), so it never shapes a
 * transfer; it exists only so that a routing loop or a runaway cannot spin the
 * host at full speed unnoticed. Hitting it means something is wrong, which is
 * why it is still counted and still reported.
 *
 * The budgets are also per direction. One shared counter meant a busy download
 * could exhaust the allowance the guest needed for its own announcements, which
 * is its own quiet failure.
 */
#define RELAY_BCAST_PER_SECOND    100
#define RELAY_UNICAST_PER_SECOND  20000

typedef enum {
    RELAY_DIR_IN,        /* host -> guest */
    RELAY_DIR_OUT,       /* guest -> host */
    RELAY_DIRS
} relay_dir_t;

typedef enum {
    RELAY_CLASS_BCAST,   /* discovery: broadcast or the announce port */
    RELAY_CLASS_UNICAST, /* share management and file operations */
    RELAY_CLASSES
} relay_class_t;

/*
 * Why a packet was dropped, and how often to say so.
 *
 * Every drop used to be one counter that nothing read - broadcast_relay_stats()
 * existed and had no callers - which is the same fault the send-failure note
 * below describes: the share is discovered, the transfer stalls, and nothing
 * anywhere says why. A rate limit sized for discovery broadcasts is invisible
 * when it starts throwing away a file transfer, so it says so, with the reason
 * separated because "we are over the cap" and "that would not fit in a frame"
 * want different fixes.
 *
 * Reported at most once a second, and only while something is being dropped, so
 * an idle machine writes nothing and a busy one cannot flood its own log.
 */
typedef enum {
    RELAY_DROP_RATE_IN_BCAST,    /* host -> guest, discovery budget */
    RELAY_DROP_RATE_IN_UNICAST,  /* host -> guest, file-traffic backstop */
    RELAY_DROP_RATE_OUT_BCAST,   /* guest -> host, discovery budget */
    RELAY_DROP_RATE_OUT_UNICAST, /* guest -> host, file-traffic backstop */
    RELAY_DROP_BUILD,        /* could not build a frame for the guest */
    RELAY_DROP_INJECT,       /* the guest's receive path would not take it */
    RELAY_DROP_FRAGMENT,     /* fragmenting a large datagram failed */
    RELAY_DROP_REASONS
} relay_drop_reason_t;

static const char *const relay_drop_names[RELAY_DROP_REASONS] = {
    "discovery rate limit (host->guest)",
    "file-traffic backstop (host->guest)",
    "discovery rate limit (guest->host)",
    "file-traffic backstop (guest->host)",
    "frame build failed",
    "guest would not accept",
    "fragmentation failed"
};

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
    relay_socket_t sockets[NUM_ACCESS_SOCKETS]; /* UDP sockets, RELAY_INVALID_SOCKET if disabled */
    int enabled;                     /* Runtime enable flag */

    struct sockaddr_in host_addr;    /* Host's IP address */
    struct sockaddr_in bcast_addr;   /* Subnet broadcast address */

    /* Learned guest IP from outgoing packets */
    uint32_t guest_ip;               /* Guest's IP in host byte order, 0 if unknown */

    /* Rate limiting: [direction][class], reset together each second. */
    uint32_t rate_count[RELAY_DIRS][RELAY_CLASSES];
    time_t last_rate_reset;

    /* Statistics */
    uint32_t tx_count;              /* Guest -> Host */
    uint32_t rx_count;              /* Host -> Guest */
    uint32_t dropped;               /* Rate limited or errors */

    /* Per-reason drop counts, and when they were last reported. */
    uint32_t drop_reason[RELAY_DROP_REASONS];
    uint32_t drop_reported[RELAY_DROP_REASONS];
    time_t last_drop_report;
} relay_state_t;

static relay_state_t relay = {
    .sockets = {RELAY_INVALID_SOCKET, RELAY_INVALID_SOCKET, RELAY_INVALID_SOCKET},
    .enabled = 0
};

/**
 * Report a failed relay send, once.
 *
 * A dropped frame used to be a counter and nothing else, which is the worst
 * possible outcome for the failure that actually happens: the share is
 * discovered and then never opens, and there is nothing anywhere to say why.
 *
 * EHOSTUNREACH on a LAN peer, on macOS, is almost always the local network
 * privacy control rather than the network. The system denies local network
 * access to an application it cannot prompt for - and the denial is partial,
 * blocking unicast while still allowing broadcast, which is exactly the shape
 * of "discovery works, connecting does not". The bundle declares
 * NSLocalNetworkUsageDescription so macOS can ask; if the answer was no, or the
 * prompt was dismissed, this is what it looks like from in here.
 */
/**
 * Count a dropped packet, and say so at most once a second per reason.
 *
 * The rate-limit reasons carry the cap in the message, because 100 packets a
 * second is ample for discovery and nowhere near enough for a file transfer -
 * roughly 140KB/s at best - so somebody reading this while a copy stalls should
 * not have to find that number in the source.
 */
/**
 * Spend a packet from a budget, or refuse it.
 *
 * @return non-zero if the packet may be relayed.
 */
static int
relay_rate_allow(relay_dir_t dir, relay_class_t cls)
{
    const time_t now = time(NULL);
    const uint32_t cap = (cls == RELAY_CLASS_BCAST) ? RELAY_BCAST_PER_SECOND
                                                    : RELAY_UNICAST_PER_SECOND;

    if (now != relay.last_rate_reset) {
        memset(relay.rate_count, 0, sizeof(relay.rate_count));
        relay.last_rate_reset = now;
    }

    if (relay.rate_count[dir][cls] >= cap) {
        return 0;
    }
    relay.rate_count[dir][cls]++;
    return 1;
}

static void
relay_drop(relay_drop_reason_t reason)
{
    const time_t now = time(NULL);
    int i;

    relay.dropped++;
    if (reason >= 0 && reason < RELAY_DROP_REASONS) {
        relay.drop_reason[reason]++;
    }

    if (now == relay.last_drop_report) {
        return;
    }
    relay.last_drop_report = now;

    for (i = 0; i < RELAY_DROP_REASONS; i++) {
        const uint32_t since = relay.drop_reason[i] - relay.drop_reported[i];

        if (since == 0) {
            continue;
        }
        relay.drop_reported[i] = relay.drop_reason[i];

        if (i <= RELAY_DROP_RATE_OUT_UNICAST) {
            const int cap = (i == RELAY_DROP_RATE_IN_BCAST ||
                             i == RELAY_DROP_RATE_OUT_BCAST)
                ? RELAY_BCAST_PER_SECOND : RELAY_UNICAST_PER_SECOND;

            rpclog("broadcast_relay: DROPPED %u packet%s - %s, cap is %d/second "
                   "(%u dropped in total; tx %u, rx %u)\n",
                (unsigned) since, since == 1 ? "" : "s", relay_drop_names[i],
                cap, (unsigned) relay.drop_reason[i],
                (unsigned) relay.tx_count, (unsigned) relay.rx_count);
        } else {
            rpclog("broadcast_relay: DROPPED %u packet%s - %s (%u in total)\n",
                (unsigned) since, since == 1 ? "" : "s", relay_drop_names[i],
                (unsigned) relay.drop_reason[i]);
        }
    }
}

static void
relay_report_send_failure(void)
{
    static int reported;

    if (reported) {
        return;
    }
    reported = 1;

#ifdef __APPLE__
    if (errno == EHOSTUNREACH) {
        rpclog("broadcast_relay: cannot reach a machine on the local network "
               "(EHOSTUNREACH). On macOS this is usually local network access "
               "being denied: check System Settings > Privacy & Security > "
               "Local Network. Discovery will still appear to work, because "
               "broadcast is allowed and unicast is not.\n");
        return;
    }
#endif
    rpclog("broadcast_relay: send failed: %s. Further failures are counted "
           "(see the relay statistics) but not logged.\n", sock_strerror());
}

/* Outgoing fragment reassembly, defined further down with the transmit path */
static void reasm_reset(void);

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
#ifdef _WIN32
static int
get_broadcast_address(struct in_addr *bcast, struct in_addr *host)
{
    /* Enumerate IPv4 adapters via GetAdaptersInfo() (iphlpapi). For the first
       non-loopback adapter with a real address, derive the directed broadcast
       address from the IP and subnet mask (bcast = ip | ~mask). */
    IP_ADAPTER_INFO *adapters = NULL, *ad;
    ULONG size = 0;
    DWORD ret;
    int found = 0;

    ret = GetAdaptersInfo(NULL, &size);
    if (ret != ERROR_BUFFER_OVERFLOW) {
        rpclog("broadcast_relay: GetAdaptersInfo() sizing failed: %lu\n",
               (unsigned long) ret);
        return -1;
    }
    adapters = malloc(size);
    if (adapters == NULL) {
        return -1;
    }
    ret = GetAdaptersInfo(adapters, &size);
    if (ret != ERROR_SUCCESS) {
        rpclog("broadcast_relay: GetAdaptersInfo() failed: %lu\n",
               (unsigned long) ret);
        free(adapters);
        return -1;
    }

    for (ad = adapters; ad != NULL && !found; ad = ad->Next) {
        IP_ADDR_STRING *ip;

        if (ad->Type == MIB_IF_TYPE_LOOPBACK) {
            continue;
        }
        for (ip = &ad->IpAddressList; ip != NULL; ip = ip->Next) {
            struct in_addr ipaddr, mask;

            ipaddr.s_addr = inet_addr(ip->IpAddress.String);
            mask.s_addr = inet_addr(ip->IpMask.String);

            /* Skip unconfigured entries (0.0.0.0). */
            if (ipaddr.s_addr == 0 || ipaddr.s_addr == INADDR_NONE) {
                continue;
            }

            host->s_addr = ipaddr.s_addr;
            bcast->s_addr = ipaddr.s_addr | ~mask.s_addr;
            found = 1;

            rpclog("broadcast_relay: using adapter %s, host %s, ",
                   ad->Description, inet_ntoa(*host));
            rpclog("broadcast %s\n", inet_ntoa(*bcast));
            break;
        }
    }

    free(adapters);
    return found ? 0 : -1;
}
#else
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

        /* Must have a live carrier. IFF_UP only means administratively
           enabled; an idle virtual bridge (e.g. docker0 with nothing
           attached) is UP but not RUNNING. Without this check such an
           interface can be selected ahead of the real LAN NIC, and Access
           broadcasts then go out on a subnet with no peers. */
        if (!(ifa->ifa_flags & IFF_RUNNING)) {
            continue;
        }

        /* Get broadcast address. Use the standard ifa_broadaddr spelling: on
           glibc it is a macro over the ifa_ifu union; on macOS/BSD it is a
           direct struct member (there is no ifa_ifu union there). */
        if (ifa->ifa_broadaddr != NULL) {
            struct sockaddr_in *bcast_sa = (struct sockaddr_in *)ifa->ifa_broadaddr;
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
#endif /* _WIN32 */

/*
 * Host-wide ownership of the relay.
 *
 * The relay cannot simply be per-emulator. Access uses fixed UDP ports, and the
 * data sockets set SO_REUSEADDR so that a restart is not blocked by the old
 * socket - which also means a second emulator on the same host binds them quite
 * happily and receives the same broadcasts. Both then relay every packet, so the
 * network sees each one twice and each instance sees the other's relayed copies.
 * Nothing fails, which is what makes it nasty: there is no error to notice.
 *
 * So ownership is claimed explicitly, with an exclusive TCP bind on loopback that
 * no SO_REUSEADDR is set on. Exactly one process can hold it; the rest run without
 * a relay and say so. TCP on the same number as one of the Access ports, because
 * Access is UDP and the pair cannot collide.
 *
 * This is the small version of what a virtual switch would do properly: one relay
 * for the host, shared, rather than one per machine.
 */
#define RELAY_OWNER_PORT 49171

static relay_socket_t relay_owner_socket = RELAY_INVALID_SOCKET;

static int
claim_relay_ownership(void)
{
    struct sockaddr_in addr;

    relay_owner_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (relay_owner_socket == RELAY_INVALID_SOCKET) {
        return 1; /* cannot claim, so do not stand in the way of the relay */
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RELAY_OWNER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(relay_owner_socket, (struct sockaddr *) &addr, sizeof(addr))
        == RELAY_SOCKET_ERROR) {
        relay_closesocket(relay_owner_socket);
        relay_owner_socket = RELAY_INVALID_SOCKET;
        return 0;
    }
    return 1;
}

static void
release_relay_ownership(void)
{
    if (relay_owner_socket != RELAY_INVALID_SOCKET) {
        relay_closesocket(relay_owner_socket);
        relay_owner_socket = RELAY_INVALID_SOCKET;
    }
}

/**
 * Initialize the broadcast relay.
 */
int
broadcast_relay_init(void)
{
    int optval = 1;
    struct sockaddr_in bind_addr;
    struct in_addr bcast, host;
    int i;
    int success_count = 0;

    if (!claim_relay_ownership()) {
        rpclog("broadcast_relay: another emulator on this host already relays "
               "Access, so this one will not. Machines still reach the network "
               "through NAT; only Access sharing goes through the other "
               "instance.\n");
        return -1;
    }

    /* Find host's broadcast address */
    if (get_broadcast_address(&bcast, &host) < 0) {
        rpclog("broadcast_relay: no suitable network interface found\n");
        release_relay_ownership();
        return -1;
    }

    /* Create UDP sockets for each Access+ port */
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        relay.sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (relay.sockets[i] == RELAY_INVALID_SOCKET) {
            rpclog("broadcast_relay: socket() for port %d failed: %s\n",
                   access_ports[i], sock_strerror());
            continue;
        }

        /* Enable broadcast */
        if (setsockopt(relay.sockets[i], SOL_SOCKET, SO_BROADCAST,
                       (const char *) &optval, sizeof(optval)) == RELAY_SOCKET_ERROR) {
            rpclog("broadcast_relay: SO_BROADCAST for port %d failed: %s\n",
                   access_ports[i], sock_strerror());
            relay_closesocket(relay.sockets[i]);
            relay.sockets[i] = RELAY_INVALID_SOCKET;
            continue;
        }

        /* Allow address reuse (in case of restart) */
        if (setsockopt(relay.sockets[i], SOL_SOCKET, SO_REUSEADDR,
                       (const char *) &optval, sizeof(optval)) == RELAY_SOCKET_ERROR) {
            rpclog("broadcast_relay: SO_REUSEADDR for port %d failed: %s\n",
                   access_ports[i], sock_strerror());
            relay_closesocket(relay.sockets[i]);
            relay.sockets[i] = RELAY_INVALID_SOCKET;
            continue;
        }

        /* Bind to this port */
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(access_ports[i]);
        bind_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(relay.sockets[i], (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == RELAY_SOCKET_ERROR) {
            rpclog("broadcast_relay: cannot bind port %d: %s\n",
                   access_ports[i], sock_strerror());
            relay_closesocket(relay.sockets[i]);
            relay.sockets[i] = RELAY_INVALID_SOCKET;
            continue;
        }

        /* Set non-blocking */
        if (socket_set_nonblocking(relay.sockets[i]) != 0) {
            rpclog("broadcast_relay: set non-blocking for port %d failed: %s\n",
                   access_ports[i], sock_strerror());
            relay_closesocket(relay.sockets[i]);
            relay.sockets[i] = RELAY_INVALID_SOCKET;
            continue;
        }

        success_count++;
    }

    if (success_count == 0) {
        rpclog("broadcast_relay: no ports could be bound, so Access sharing is off "
               "for this machine\n");
        release_relay_ownership();
        return -1;
    }
    if (success_count < NUM_ACCESS_SOCKETS) {
        /* Worse than none: some traffic relays and some does not, so shares half
           appear and nobody can tell why. */
        rpclog("broadcast_relay: only %d of %d ports bound; Access sharing will "
               "behave erratically in this instance\n",
               success_count, NUM_ACCESS_SOCKETS);
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
    memset(relay.rate_count, 0, sizeof(relay.rate_count));
    memset(relay.drop_reason, 0, sizeof(relay.drop_reason));
    memset(relay.drop_reported, 0, sizeof(relay.drop_reported));
    relay.last_rate_reset = time(NULL);
    relay.last_drop_report = 0;
    relay.guest_ip = 0;  /* Will be learned from first outgoing packet */

    reasm_reset();

    relay.enabled = 1;

    return 0;
}

/**
 * Shutdown the broadcast relay.
 */
void
broadcast_relay_close(void)
{
    release_relay_ownership();
    int i;
    for (i = 0; i < NUM_ACCESS_SOCKETS; i++) {
        if (relay.sockets[i] != RELAY_INVALID_SOCKET) {
            relay_closesocket(relay.sockets[i]);
            relay.sockets[i] = RELAY_INVALID_SOCKET;
        }
    }
    reasm_reset();
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
    uint32_t src_ip_be;
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

    /*
     * The real sender, exactly as build_guest_frame() does for a datagram small
     * enough not to need fragmenting.
     *
     * This said 10.10.10.2, the SLiRP gateway. A guest that answers such a
     * datagram addresses its reply to the gateway, which is inside the NAT
     * network, so broadcast_relay_tx() classifies it as internal traffic, hands
     * it to SLiRP and it never reaches the peer. Every Access datagram over
     * 1472 bytes takes this path, which is all of ShareFS's bulk traffic.
     */
    memcpy(&src_ip_be, &from->sin_addr, 4);
    src_ip = ntohl(src_ip_be);

    /* Get unique IP ID for this datagram */
    ip_id = ip_id_counter++;

    /* A datagram is only useful to the guest once every fragment has arrived;
       delivering some fragments and then running out of inject-queue space
       leaves the guest with an unreassemblable partial that stalls the
       transfer (this is what broke large ShareFS copies). So require room for
       all fragments up front - if it is not there, drop the whole datagram and
       let the sender retransmit. The per-fragment step matches the loop below
       (max_data_per_frag rounded down to an 8-byte multiple). */
    {
        int frag_step = max_data_per_frag & ~7;
        int nfrags = (total_ip_payload + frag_step - 1) / frag_step;

        if (network_nat_inject_space() < nfrags) {
            return 0;
        }
    }

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
     * The receive buffer must be able to hold the LARGEST datagram a peer can
     * send, not merely a large one: recvfrom() on a datagram socket copies what
     * fits and discards the remainder silently, returning the buffer size, so a
     * buffer one byte short truncates and reports success.
     *
     * It was 8192, with a comment saying that avoided truncation. ShareFS sends
     * bulk file data as 8200-byte UDP payloads, so every single data datagram
     * lost its last 8 bytes, and the guest filled the hole with whatever was in
     * its own buffer. The result was a file that transferred at the right length
     * with no error reported and a corrupt 8 bytes near the end of each block -
     * worse the larger the file, because there were more blocks.
     *
     * 65535 is the ceiling of the IP total-length field, so nothing legal can
     * exceed it and truncation is now impossible by construction rather than by
     * being generous.
     */
    static uint8_t udp_payload[RELAY_RECV_CAPACITY];  /* Static to avoid stack overflow */

    /* One frame for the guest, so it has to fit nat.buffer (PKT_MAX_SIZE) once
       the 42 bytes of headers are on it. Only datagrams small enough to go in a
       single frame come this way; anything larger is fragmented instead. */
    uint8_t frame[2048];
    struct sockaddr_in from;
    socklen_t fromlen;
    ssize_t n;
    int frame_len;
    uint16_t port;
    int is_broadcast;

    if (relay.sockets[sock_idx] == RELAY_INVALID_SOCKET) {
        return 0;
    }

    port = access_ports[sock_idx];

    /* Non-blocking receive (socket is already set non-blocking) */
    fromlen = sizeof(from);
    n = recvfrom(relay.sockets[sock_idx], (char *) udp_payload, sizeof(udp_payload), MSG_DONTWAIT,
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

    /*
     * Determine if this should be delivered as broadcast or unicast. Port 32770
     * uses broadcast for discovery, others are unicast responses - and that is
     * also which budget it spends from, so it is worked out before the check.
     */
    is_broadcast = (port == ACCESS_PORT_ANNOUNCE);

    if (!relay_rate_allow(RELAY_DIR_IN, is_broadcast ? RELAY_CLASS_BCAST
                                                     : RELAY_CLASS_UNICAST)) {
        relay_drop(is_broadcast ? RELAY_DROP_RATE_IN_BCAST
                                : RELAY_DROP_RATE_IN_UNICAST);
        return 0;
    }

    /* Check if payload is too large for single Ethernet frame */
    /* Max UDP payload in single frame: 1500 - 20(IP) - 8(UDP) = 1472 */
    if (n > 1472) {
        /* Use IP fragmentation for large payloads */
        int frags = inject_fragmented_udp(&from, port, udp_payload, n, is_broadcast);
        if (frags > 0) {
            relay.rx_count++;
            return 1;
        } else {
            relay_drop(RELAY_DROP_FRAGMENT);
            return 0;
        }
    }

    /* Build frame for guest (single unfragmented frame) */
    frame_len = build_guest_frame(frame, sizeof(frame), &from, port, udp_payload, n, is_broadcast);
    if (frame_len < 0) {
        relay_drop(RELAY_DROP_BUILD);
        return 0;
    }

    /* Inject into guest via direct buffer delivery */
    if (network_nat_inject_packet(frame, frame_len)) {
        relay.rx_count++;
        return 1;
    } else {
        relay_drop(RELAY_DROP_INJECT);
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

/*
 * Reassembly of guest-originated fragmented Access+ datagrams.
 *
 * RISC OS fragments ShareFS bulk writes that exceed the Ethernet MTU. Only the
 * first fragment carries a UDP header, so the remaining fragments cannot be
 * matched to an Access+ port on their own. Handing them to SLiRP instead is not
 * good enough: SLiRP would reassemble the datagram and re-emit it from its own
 * NAT socket under a masqueraded source port, splitting the ShareFS
 * conversation across two ports and stalling the transfer. So collect the
 * fragments here and send the completed datagram from the correct port.
 *
 * Fragments are expected in ascending offset order, which is what the guest
 * emits. Anything else (a gap, an overlap, a datagram whose first fragment was
 * not ours) is declined and left to SLiRP.
 */
#define REASM_SLOTS     4
#define REASM_MAX_LEN   65535   /* Largest IP payload a datagram can carry */
#define REASM_TIMEOUT   8       /* Seconds to wait for the rest of a datagram */

typedef struct {
    int      in_use;
    uint32_t src_ip;        /* Datagram identity, host byte order */
    uint32_t dst_ip;
    uint16_t ip_id;
    uint16_t dst_port;      /* Learned from the first fragment */
    int      sock_idx;
    uint32_t next_offset;   /* Offset the next fragment must start at */
    uint32_t total_len;     /* IP payload length, 0 until the last fragment */
    time_t   started;
    uint8_t  data[REASM_MAX_LEN];
} reasm_slot_t;

static reasm_slot_t reasm[REASM_SLOTS];

static void
reasm_reset(void)
{
    int i;

    for (i = 0; i < REASM_SLOTS; i++) {
        reasm[i].in_use = 0;
    }
}

static reasm_slot_t *
reasm_lookup(uint32_t src_ip, uint32_t dst_ip, uint16_t ip_id)
{
    int i;

    for (i = 0; i < REASM_SLOTS; i++) {
        if (reasm[i].in_use && reasm[i].src_ip == src_ip &&
            reasm[i].dst_ip == dst_ip && reasm[i].ip_id == ip_id)
        {
            return &reasm[i];
        }
    }
    return NULL;
}

/**
 * Drop any datagram that has been waiting too long for its remaining
 * fragments, so a lost fragment cannot tie up a slot indefinitely.
 */
static void
reasm_expire(time_t now)
{
    int i;

    for (i = 0; i < REASM_SLOTS; i++) {
        if (reasm[i].in_use && now - reasm[i].started > REASM_TIMEOUT) {
            reasm[i].in_use = 0;
            relay.dropped++;
        }
    }
}

/**
 * Claim a slot for a datagram, restarting it if it is already known (which is
 * what a retransmission looks like). Falls back to displacing the oldest
 * incomplete datagram when every slot is busy.
 */
static reasm_slot_t *
reasm_claim(uint32_t src_ip, uint32_t dst_ip, uint16_t ip_id, time_t now)
{
    reasm_slot_t *slot = reasm_lookup(src_ip, dst_ip, ip_id);
    int i;

    if (slot == NULL) {
        for (i = 0; i < REASM_SLOTS; i++) {
            if (!reasm[i].in_use) {
                slot = &reasm[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        slot = &reasm[0];
        for (i = 1; i < REASM_SLOTS; i++) {
            if (reasm[i].started < slot->started) {
                slot = &reasm[i];
            }
        }
        relay.dropped++; /* The datagram being displaced never completed */
    }

    slot->in_use = 1;
    slot->src_ip = src_ip;
    slot->dst_ip = dst_ip;
    slot->ip_id = ip_id;
    slot->next_offset = 0;
    slot->total_len = 0;
    slot->started = now;

    return slot;
}

/**
 * Absorb one fragment of a guest-originated datagram, sending the datagram on
 * once the last fragment has arrived.
 *
 * @param ip_hdr      Start of the IP header
 * @param ip_hdr_len  Length of the IP header in bytes
 * @param avail       Bytes of IP payload actually present in the frame
 * @param src_ip      Source address, host byte order
 * @param dst_ip      Destination address, host byte order
 * @param ip_id       IP identification field
 * @param frag_offset Offset of this fragment within the IP payload, in bytes
 * @param more_frags  Non-zero if the More Fragments flag is set
 * @param now         Current time
 *
 * @return 1 if the relay has taken ownership of this fragment, else 0
 */
static int
relay_tx_fragment(const uint8_t *ip_hdr, int ip_hdr_len, int avail,
                  uint32_t src_ip, uint32_t dst_ip, uint16_t ip_id,
                  uint32_t frag_offset, int more_frags, time_t now)
{
    reasm_slot_t *slot;
    const uint8_t *frag;
    struct sockaddr_in dest;
    int frag_len;
    int total_ip_len;
    int sent;

    /* Prefer the length the frame actually carries over the header's claim. */
    total_ip_len = (ip_hdr[2] << 8) | ip_hdr[3];
    frag_len = total_ip_len - ip_hdr_len;
    if (frag_len <= 0 || frag_len > avail) {
        frag_len = avail;
    }
    if (frag_len <= 0) {
        return 0;
    }
    frag = ip_hdr + ip_hdr_len;

    if (frag_offset == 0) {
        uint16_t src_port, dst_port;
        int sock_idx;

        /* Only the first fragment has the ports we match Access+ traffic on */
        if (frag_len < 8) {
            return 0;
        }
        src_port = (frag[0] << 8) | frag[1];
        dst_port = (frag[2] << 8) | frag[3];

        sock_idx = find_socket_for_port(dst_port);
        if (sock_idx < 0) {
            sock_idx = find_socket_for_port(src_port);
        }
        if (sock_idx < 0 || relay.sockets[sock_idx] == RELAY_INVALID_SOCKET) {
            return 0; /* Not Access+ traffic */
        }

        slot = reasm_claim(src_ip, dst_ip, ip_id, now);
        slot->dst_port = dst_port;
        slot->sock_idx = sock_idx;
    } else {
        slot = reasm_lookup(src_ip, dst_ip, ip_id);
        if (slot == NULL) {
            return 0; /* First fragment was not ours, or was missed */
        }
        if (frag_offset != slot->next_offset) {
            /* Out of order or overlapping - let SLiRP deal with the datagram */
            slot->in_use = 0;
            relay.dropped++;
            return 0;
        }
    }

    if (frag_offset + (uint32_t) frag_len > REASM_MAX_LEN) {
        slot->in_use = 0;
        relay.dropped++;
        return 1; /* Ours, but larger than a datagram may be */
    }

    memcpy(slot->data + frag_offset, frag, (size_t) frag_len);
    slot->next_offset = frag_offset + (uint32_t) frag_len;
    if (!more_frags) {
        slot->total_len = slot->next_offset;
    }

    if (slot->total_len == 0 || slot->next_offset < slot->total_len) {
        return 1; /* Still waiting for the rest */
    }

    /* Complete. The IP payload starts with the UDP header; the relay's socket
       is already bound to the right port, so only the UDP payload is sent. */
    slot->in_use = 0;

    if (slot->total_len < 8) {
        relay.dropped++;
        return 1;
    }

    /*
     * Charged per datagram rather than per fragment, so a fragmented transfer is
     * not billed several times over. Always the file-traffic budget: a datagram
     * only arrives here because it was fragmented, and Access+ discovery
     * broadcasts are far too small to fragment.
     */
    if (!relay_rate_allow(RELAY_DIR_OUT, RELAY_CLASS_UNICAST)) {
        relay_drop(RELAY_DROP_RATE_OUT_UNICAST);
        return 1;
    }
    (void) now;

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(slot->dst_port);
    dest.sin_addr.s_addr = htonl(slot->dst_ip);

    sent = sendto(relay.sockets[slot->sock_idx], (const char *) (slot->data + 8),
                  slot->total_len - 8, 0,
                  (struct sockaddr *) &dest, sizeof(dest));
    if (sent < 0) {
        relay.dropped++;
        relay_report_send_failure();
    } else {
        relay.tx_count++;
    }

    return 1;
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
    uint32_t src_ip;
    uint16_t ip_id;
    uint16_t frag_field;
    uint32_t frag_offset;
    int more_frags;
    int ip_hdr_len;
    int udp_len;
    int payload_len;
    int avail;
    const uint8_t *payload;
    struct sockaddr_in dest;
    int sent;
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

    /* The header length is taken from the frame, so check it is sane and that
       the frame really holds a header of that size before reading past it. */
    if (ip_hdr_len < IP_HDR_LEN || pkt_len < ETH_HLEN + ip_hdr_len) {
        return 0;
    }

    /* Get source IP (guest's IP) and learn it for responses */
    src_ip = ((uint32_t)ip_hdr[12] << 24) | ((uint32_t)ip_hdr[13] << 16) |
             ((uint32_t)ip_hdr[14] << 8) | (uint32_t)ip_hdr[15];
    /* Only learn if it's in SLiRP range and not broadcast */
    if ((src_ip & SLIRP_MASK) == SLIRP_NET && src_ip != SLIRP_BROADCAST) {
        relay.guest_ip = src_ip;
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

    now = time(NULL);
    reasm_expire(now);

    /* A fragmented datagram has no UDP header to match on except in its first
       fragment, so it is collected separately. Only unicast to an external peer
       is taken: Access+ discovery broadcasts are small enough not to fragment,
       and SLiRP still needs to see broadcasts for local delivery. */
    ip_id = (uint16_t) ((ip_hdr[4] << 8) | ip_hdr[5]);
    frag_field = (uint16_t) ((ip_hdr[6] << 8) | ip_hdr[7]);
    frag_offset = (uint32_t) (frag_field & 0x1fff) * 8;
    more_frags = (frag_field & 0x2000) != 0;

    if ((more_frags || frag_offset != 0) && is_external_unicast) {
        return relay_tx_fragment(ip_hdr, ip_hdr_len,
                                 pkt_len - ETH_HLEN - ip_hdr_len,
                                 src_ip, dst_ip, ip_id,
                                 frag_offset, more_frags, now);
    }

    /* Unfragmented from here on, so a UDP header must be present */
    if (pkt_len < ETH_HLEN + ip_hdr_len + 8) {
        return 0;
    }

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
    if (relay.sockets[sock_idx] == RELAY_INVALID_SOCKET) {
        return 0;
    }

    /* Rate limiting, from whichever budget this destination belongs to. */
    if (!relay_rate_allow(RELAY_DIR_OUT, is_broadcast ? RELAY_CLASS_BCAST
                                                      : RELAY_CLASS_UNICAST)) {
        relay_drop(is_broadcast ? RELAY_DROP_RATE_OUT_BCAST
                                : RELAY_DROP_RATE_OUT_UNICAST);
        return 1;  /* Handled (dropped) */
    }

    /* Extract UDP payload. The length field is only the guest's claim, so clamp
       it to what the frame actually holds rather than reading past the end of
       it and sending unrelated transmit-buffer bytes onto the network. */
    avail = pkt_len - ETH_HLEN - ip_hdr_len - 8;
    udp_len = (udp_hdr[4] << 8) | udp_hdr[5];
    payload_len = udp_len - 8;
    payload = udp_hdr + 8;

    if (payload_len > avail) {
        payload_len = avail;
    }
    if (payload_len <= 0) {
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
    sent = sendto(relay.sockets[sock_idx], (const char *) payload, payload_len, 0,
                  (struct sockaddr *)&dest, sizeof(dest));

    if (sent < 0) {
        relay.dropped++;
        relay_report_send_failure();
    } else {
        relay.tx_count++;
    }

    /* Return 1 for external unicast (we handle it completely) */
    /* Return 0 for broadcast (let SLiRP see it too for local loopback) */
    return is_external_unicast ? 1 : 0;
}

/**
 * How many bytes of a single datagram the relay can take.
 *
 * Exposed so a test can check the buffer against the largest datagram a peer may
 * legally send, rather than restating the number and testing its own copy of it.
 */
size_t
broadcast_relay_recv_capacity(void)
{
    return RELAY_RECV_CAPACITY;
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
