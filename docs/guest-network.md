# Which network the guests are on

Every machine RPCEmu starts sits on a private `/24` behind its own NAT. The
machine is given an address on it by DHCP, the gateway and DNS live on it, and
nothing on it is visible to the host's real network unless a port is forwarded.

By default that network is `10.10.10.0/24`, which is what every version used
before it could be set.

| | |
| --- | --- |
| Network | `10.<b>.<c>.0/24` |
| Gateway (the host, as the guest sees it) | `.2` |
| DNS | `.3` |
| Machines | `.10` upwards, one per emulator running on this computer |
| Broadcast | `.255` |

## Why it is settable

The last part follows a **slot**, claimed when a machine starts, so the first
machine started is `.10`, the second `.11`, and so on. That is enough to tell
apart machines started by **one** RPCEmu on one computer, which is all it ever
had to do.

It is not enough once machines on different computers share a network — over a
JSON tun/tap server, for instance (see
[pyromaniac-networking.md](pyromaniac-networking.md)). Those machines were
started by different RPCEmus, and each hands its first machine the same address.
Two hosts arrive on one wire with distinct MAC addresses and the same IP, which
cannot work, and nothing says why: there is no error, no warning and no clue in
the log. Traffic simply does not arrive.

So the network is a setting, and two installations that need to meet can be put
on different ones.

## Changing it

**Settings → Networking**, in the Manager window:

```
Guest network:  10 . [ 10 ] . [ 10 ] . 0 / 24    [ Random ]
```

Only the middle two parts are yours to set, each 0 to 255. The first is fixed at
`10` because `10.0.0.0/8` is a private range in its own right, so nothing chosen
here can collide with a real network the host is on; the mask stays `/24`.

**Random** picks a pair at random, which is the quickest way to be reasonably
sure of not matching somebody else's.

The change takes effect when a machine next starts. A machine already running
keeps the network it started on.

## Where it is kept

`rpcemu.cfg` in the data directory, as the middle two parts:

```
guest_subnet=10.10
```

It is a property of the **installation**, not of a machine: every machine here is
on one network, so it cannot sensibly differ between them. Nothing reads a
per-machine `guest_subnet`.

## What a new installation gets

A data directory with no machines in it is a first run, and RPCEmu picks a random
network for it and writes it down. Two people who install RPCEmu and then connect
their machines are then unlikely to have to think about any of this.

**An existing installation is left alone.** A data directory that already has
machines keeps `10.10.10.0/24`, on an upgrade and forever after, unless it is
changed by hand: moving the guests of somebody with port forwards or firewall
rules pointing at `10.10.10.10` would be a poor trade for a problem they may not
have.

The check is made before the data directory is seeded, so a packaged install that
ships a machine still counts as new.

## What has to agree with it

Nothing outside RPCEmu, but two things inside it derive addresses from this and
must not be allowed to disagree:

- **NAT** (`network-nat.c`) hands out the guest address, gateway and DNS.
- **The Access relay** (`broadcast_relay.c`) decides from the network which
  traffic is local to the guests and which is external, and relays accordingly.

Both take them from `guest_subnet.h`, so there is one definition.
`tests/test_guest_subnet.c` pins the arithmetic down.
