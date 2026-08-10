# Pyromaniac Networking

A machine here and a RISC OS Pyromaniac elsewhere can sit on the same virtual
network, by both connecting to Charles Ferguson's JSON tun/tap server.

The server is a single Python file:
<https://github.com/gerph/tuntap-json-server>. It listens on TCP, and every
frame it is given it replicates to every other client, and to a real tap
interface if it has been given one. Pyromaniac has spoken this for years, as its
`etherdriverjson` configuration group; this is the other end of the same wire.

## Why it exists, when there is already a local wire

Machines running on one host already see each other without any of this, over a
loopback hub that needs no configuration (see [multi-machine.md](multi-machine.md)).
The JSON server is for the things that hub cannot do:

- **It crosses hosts.** The server can run on a Linux box while emulators on
  Windows and macOS connect to it over the network. Only the machine running the
  server needs a tap, and creating one on Linux is far easier than on the other
  two.
- **It is one place to watch the whole network.** Every frame passes through the
  server, so the traffic can be captured or traced in one place rather than per
  machine.
- **Other emulators can join.** Pyromaniac today; anything else that speaks the
  format tomorrow.

## Setting it up

Run the server somewhere. With no tap it needs no privileges and no setup:

```sh
git clone https://github.com/gerph/tuntap-json-server
python3 tuntap-json-server/tap_jsonserver.py --port 33445
```

Add `--tap-enable` when the virtual network should also reach the real one; that
is the part that needs a tap and the privileges to make one.

Then point machines at it, in the machine editor under **Network**, in the
**Pyromaniac Networking** box: tick the option, give the server's host name and
port. Or per run, without editing the machine:

```sh
./rpcemu-recompiler --machine os530 --pyromaniac-net myserver:33445
./rpcemu-recompiler --machine os371 --pyromaniac-net myserver --headless
./rpcemu-recompiler --machine os530 --pyromaniac-net off
```

`off` takes a machine whose settings have it on out of the network for that run.
The port defaults to 33445, the server's own default. The option works the same
for the window and for `--headless`.

## What it does to the rest of the networking

**The machine stops using the local wire.** Both are hubs that carry every frame
to everyone, so a machine on both would send each frame to its local peers and
to the server, which would replicate it to those same peers if they were also
connected. Everything would arrive twice. A machine that names a server uses the
server and nothing else; `network_nat_init()` enforces it, and the log says which
wire a machine is on.

**Its own way out to the internet is unaffected.** NAT, bridging or tunnelling
carry on as before; this decides which other emulators the machine can see, not
how it reaches the outside world.

## Addresses are yours to arrange

This carries Ethernet frames and nothing more. Two machines that are to talk IP
still have to agree addresses.

Guests here are given `10.10.10.10 + slot` by their own SLiRP, where the slot is
claimed at startup, so the first machine started is `.10`, the second `.11`, and
so on. Anything else joining the same network needs an address on it that does
not collide - a Pyromaniac configured statically, for instance.

MAC addresses are derived from the same slot unless a machine sets `macaddress`
explicitly. Two emulators from different projects are unlikely to collide, but
if they do, set one of them by hand.

## The format

One JSON object per line, and the Ethernet header is taken apart rather than
sent whole:

```json
{"frame_type": 2048, "src": [6, 2, 3, 4, 5, 6], "dst": [255, 255, 255, 255, 255, 255], "data": "aGVsbG8="}
```

`dst` is bytes 0 to 5 of the frame, `src` is 6 to 11, `frame_type` is the
big-endian halfword at 12 and 13, and `data` is base64 of everything from byte 14
on - the payload, not the frame. Getting that last part wrong is the easy
mistake, and it is why `tests/test_net_json.c` checks the encoding byte for byte
rather than only round-tripping it through itself.

## What has been checked

The encoding and decoding are tested against the format as the server implements
it, including every byte value through base64, payload lengths either side of a
base64 group, malformed lines being refused, and lines without the spacing that
the server's own `json.dumps` happens to produce. Our encoder's output has been
compared against the server's encoder for the same frame and is identical.

**Not yet done: a running RPCEmu and a running Pyromaniac exchanging traffic
through a live server.** The format is right and the transport works; whether
two guest operating systems then talk to each other is the next thing to
confirm.
