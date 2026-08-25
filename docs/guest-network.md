# Which addresses the guests are on

Every machine RPCEmu starts sits on a private network behind its own NAT. The
machine is given an address on it by DHCP, the gateway and DNS live on it, and
nothing on it is visible to the host's real network unless a port is forwarded.

That network is the same everywhere, and there is nothing to configure.

| | |
| --- | --- |
| Network | `100.64.0.0/10` |
| Gateway (the host, as the guest sees it) | `100.64.0.1` |
| DNS | `100.64.0.2` |
| A machine | Computed from its MAC address |
| Broadcast | `100.127.255.255` |

## Why a machine's address comes from its MAC

The obvious way to number machines is in the order they start: first `.10`,
second `.11`, and so on. That works while every machine on a network was started
by one copy of RPCEmu on one computer.

It stops working the moment machines on different computers share a network —
over a JSON server, for instance (see
[json-networking.md](json-networking.md)). Each copy of RPCEmu counts from the
beginning, so each gives its first machine the same address. Two machines then
arrive on one wire with different MAC addresses and the same IP, which cannot
work, and nothing says why: traffic simply does not arrive.

Every machine already has a MAC address of its own, generated once and kept in
its configuration. Deriving the IP address from it means:

- **Nothing to configure and nothing to agree.** Two installations that have
  never met compute different addresses for their machines without being told
  anything.
- **The address is stable.** The same machine gets the same address on every
  start, whatever else is running and in whatever order — which the slot scheme
  could not promise.
- **It is the same wherever it is worked out.** Any RPCEmu computing the address
  of a machine from its MAC gets the answer that machine will use.

## Why `100.64.0.0/10`

It has to be one network for every installation, so it has to be somewhere a
host's real LAN is unlikely to be.

`10.0.0.0/8` is the obvious large private range and the wrong choice. Home and
office LANs live there, and SLiRP treats any address inside its own network as
one of its own — so a guest could no longer reach a real `10.x` machine on the
host's LAN.

`100.64.0.0/10` is the shared address space of RFC 6598, set aside for carrier
NAT. It is vanishingly rare on the networks this runs on, and its 22 host bits
are enough for the next part.

## It is a hash, not an encoding

A MAC address has 46 usable bits and there are 22 to put them in, so two machines
**can** compute the same address. It is unlikely — about one in a million for two
machines, and around 0.1% for a hundred on one wire — but not impossible.

So it is checked rather than assumed. A machine that sees an ARP from somebody
else claiming its own address says so, once, in `rpclog.txt`:

```
Networking: another machine on this network is using 100.100.51.77, this
machine's address (it is aa:bb:cc:dd:ee:ff). Neither will work properly until
one of them is given a different MAC address.
```

The fix is to give one of the two machines a different MAC address, in **Machine
Settings → Network**. Clearing that field and saving is enough: a machine with no
MAC address is given a new one.

## What has to agree with it

Nothing outside RPCEmu, but two things inside it derive addresses from this and
must not be allowed to disagree:

- **NAT** (`network-nat.c`) hands out the guest address, gateway and DNS.
- **The Access relay** (`broadcast_relay.c`) decides from the network which
  traffic is local to the guests and which is external, and relays accordingly.

Both take them from `guest_subnet.h`, so there is one definition.
`tests/test_guest_subnet.c` pins the arithmetic down, including the exact address
one MAC address produces — two installations have to agree on that, so changing
how it is computed has to be a deliberate act rather than a tidy-up nobody
noticed.
