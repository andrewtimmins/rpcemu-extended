# The Community Network

One shared network that anybody running RPCEmu Extended can join, for reaching
each other's machines: ShareFS discs, printers, and anything else RISC OS does
over a local wire. Tick one box; there is nothing to configure.

It uses the same transport as [JSON networking](json-networking.md), joining a
server that is already running rather than one you run yourself. The address is
fixed, `nexus.branchthroughzero.co.uk` on the standard port 33445, so that
everybody who turns it on lands on the same wire.

## Turning it on

In the machine editor, under **Network**, in the **Community Network** box: tick
**Join the Community Network**. The first time, you are asked to agree to what
it means; agreeing is remembered, so you are not asked again for your other
machines.

The change takes effect when the machine next resets.

## Read this first

**This is an open network of strangers, and it is not secure.** That is not a
caveat at the bottom of the page, it is the main thing to know about it.

- **Nothing is encrypted or authenticated.** Every frame your machine sends can
  be read by everyone else on the network. Anyone can claim to be anyone.
- **RISC OS has no meaningful security.** ShareFS, Access and the rest were
  designed for a trusted office network in the 1990s. A disc you share is shared
  with everybody; file protection is a convention rather than a defence.
- **Anything the machine offers is public.** Discs, folders and printers it
  shares are reachable by people you do not know, and anything they send you
  arrives with no check on where it came from.
- **Treat the machine as disposable.** Anything the guest can write to, somebody
  else can ask it to write to. Use a machine you would not mind losing, and keep
  personal data, credentials and work files off it - including other people's
  personal data, which is your responsibility under UK data protection law if
  you put it there.
- **Your IP address is visible** to the server, and whoever runs it may log
  connections.

The network is provided as it is, with no undertaking that it works, stays
available, or is free of other people's mistakes or bad behaviour. So far as the
law allows, the authors and contributors of RPCEmu Extended accept no liability
for any loss, damage or disclosure arising from your use of it.

## Using it alongside a server of your own

Both can be on at once. They are separate connections that know nothing about
each other, so one being down does not affect the other, and a machine can reach
its own people and the community at the same time.

The cost is duplication. Both servers are hubs that copy every frame to every
client, so if two machines are on **both** networks, each hears the other twice.
IP tolerates that. Broadcast discovery does not tolerate it as gracefully:
ShareFS and Access will show the same machine from both directions, and
broadcast traffic doubles. If you do not need both, one is quieter.

## What it does to the rest of the networking

The same as JSON networking does, for the same reason:

- **The machine leaves the local wire.** Machines started on this computer are
  no longer reachable directly; they are reachable only if they are on the same
  server. Both are hubs, and a machine on both would see every frame twice.
- **NAT is unaffected.** Reaching the internet, and any port forwarding, works
  exactly as before.

## Addresses

Every machine gets an address of its own computed from its MAC address, so two
people joining with default settings do not collide. A MAC address that is
already in use on the network is reported in the log, which is worth watching
for: on a public network, someone else may have copied a machine you published.

## Troubleshooting

The log names which server it is talking about, so with both networks on it is
clear which one is which:

```
net_json: on the Community Network at nexus.branchthroughzero.co.uk:33445; ...
net_json: cannot reach the Community Network at ..., retrying every 10 seconds
```

A machine keeps retrying by itself, so one started before the server is
reachable joins when it becomes reachable. Nothing needs restarting.
