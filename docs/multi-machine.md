# Running several machines at once

People want what VirtualBox and VMware give them: a list of machines, and several of
them running together. Developers want RISC OS 3.71 beside 5.30 beside a 5.31 beta
to check software against each. Everyone else wants to potter about with more than
one machine, and to have those machines see each other.

This describes how that works today, where it is going, and one thing that has been
ruled out, so that the parts already in the tree are not mistaken for the finished
design.

## One machine per process, and why that is not a workaround

The emulator core is one machine per process, structurally. `arm`, `config`,
`machine`, `ram00`, `vram`, `rom`, `iomd`, `vidc`, the cp15 translation tables and
the recompiler's code cache are all file-scope globals. Two of those are decisive:

- **The soft TLB is 32MB per machine.** `vraddrl_mode[2][0x100000]` and
  `vwaddrl_mode[2][0x100000]`, 16MB each.
- **The recompiler bakes the addresses of globals into generated code.** Generated
  blocks load `&arm.reg[16]`, `&flaglookup[...]` and the fast-map bases as absolute
  values, and the code cache is itself a static array. Two machines in one process
  would need separate compilations of everything, with the cache rebased.

Turning that into a per-machine context is a months-long refactor of a twenty-year-old
core, and the recompiler is the hard half. **It is not planned.**

Nor is it necessary. VirtualBox runs each VM as its own process with a manager front
end; VMware Workstation runs one `vmware-vmx` per VM. What users are asking for is a
manager, not a single address space. So the plan is a manager over processes, and
the emulator's job is to be well behaved when several copies of it are running.

## What exists now

Several machines can be run side by side by hand. Each needs its own channels, or
they fight over one port and one socket path. Give each machine its own in the
machine editor, under **Host access** on the Options page, and they can then simply
be started:

```bash
./rpcemu-recompiler --machine os371 &
./rpcemu-recompiler --machine os530 &
```

Or say so per instance, which overrides what the machine holds and is the way to run
the same set of machines two different ways without editing them:

```bash
./rpcemu-recompiler --machine os371 --vnc-port 5901 --hostcmd-socket /tmp/os371.sock &
./rpcemu-recompiler --machine os530 --vnc-port 5902 --hostcmd-socket /tmp/os530.sock &
```

`--vnc-port`, `--no-vnc`, `--hostcmd-socket`, `--debug-socket` and `--no-relay` are
applied last of all, after both settings files. See
[vnc.md](vnc.md#which-file-wins).

**Exchanging files between machines already works** and needs no networking: the
`shared/` folder is exposed to every machine as `HostFS::Shared.$`. For the
"does my software still run on 3.71" case that is most of the job.

**A machine can only be run once at a time.** Two copies of the same machine write
the same `cmos.ram` and the same configuration when they exit, so the later one
silently discards the other's changes, and they interleave sector writes into the
same hard disc image. Each write is flushed to the host, which makes the
interleaving more thorough rather than less.

Starting a machine therefore takes an exclusive lock on `running.lock` in the
machine's own directory, and a second attempt refuses with a message naming the
process that holds it and the VNC port it is on. The lock is an operating-system
lock rather than a file whose existence means "locked", so the kernel releases it
however the process dies: a crash or a `kill -9` cannot leave a machine
permanently unstartable, and there is nothing to clean up.

**One instance relays Access.** The relay bridges Access broadcasts to the real
network, and Access uses fixed UDP ports, so it is a host-wide service. The first
emulator to start claims it and the rest decline and say so. Without that claim they
all bind the same ports successfully, because the sockets set `SO_REUSEADDR`, and
every packet is relayed once per instance: the network sees duplicates and each
instance sees the others' copies, with no error to notice. The claim is an exclusive
loopback TCP bind, which is the smallest thing that makes ownership explicit.

## What was tried and rejected: displays embedded in a manager

A manager window listing machines, with their screens embedded in tabs, was built and
abandoned. It worked, and it was far too slow to use.

The reasoning behind it was that embedded displays came almost free, because each
instance already runs a VNC server. They do not. The direct path draws the guest's
framebuffer into the window; going through VNC on the same machine means the emulator
copies the frame and encodes it, and the viewer decodes it, converts it to the
toolkit's pixel format, scales it and blits it. Some of that first attempt was simply
wasteful and could have been fixed, but the copies that remain are inherent to using
a network protocol as a local display path, and the gap against a direct draw was
obvious to anyone using it.

The premise was also wrong about the thing it was imitating. **VirtualBox does not
embed VM displays in its manager.** The manager is a list; starting a machine launches
a separate process with its own window and its own direct rendering. VMware
Workstation does put displays in tabs, and does it over shared memory rather than a
network protocol. So a manager that starts machines in their own windows is not a
compromised version of the idea, it is what the model being copied actually does.

If embedded displays are ever wanted, the route is a shared framebuffer between the
emulator and the viewer, not VNC. That needs cross-platform shared memory, a
tear-free hand-off, and input over a side channel. VNC keeps the job it is good at:
reaching a machine that is headless or on another computer, where the alternative is
nothing at all.

## What is planned

### The wire between machines

Each machine used to sit behind its own NAT, so two emulated machines could not see
each other at all. `net_switch.c` connects them.

It is a hub rather than a switch, and deliberately has no owner. Every instance
already claims a slot of its own (`net_slot.c`); each slot gets a loopback UDP port,
every instance binds its own and sends each frame the guest transmits to all the
other slots' ports. Receivers drop what is not addressed to them, exactly as a card
on a hub does.

That shape was chosen over a switch process - including one living in the Manager -
because of the questions a central service brings: who starts it, what happens to
machines started by hand, what happens when it dies with machines still running.
A machine started any way at all joins this by existing, and an instance that dies
takes nothing with it but itself. Loopback UDP because it is the only transport
available on all three platforms; bound and sent to 127.0.0.1 only, so this LAN
cannot leave the host.

Each machine still keeps its own SLiRP for outbound traffic, which is the "much less
work and probably indistinguishable in use" option below. SLiRP answers ARP only for
its own addresses, so it does not answer for another guest and cannot hijack a
conversation between two of them.

**What is verified.** Frames cross between two running instances, and each machine
keeps only what is addressed to it (`tests/test_net_switch.c`). What has *not* been
demonstrated is a guest operating system using it - a ping between two RISC OS
machines, or ShareFS finding a share on the other - because that needs a guest with
its TCP/IP stack configured, and it is the obvious next thing to check.

The Access relay still binds its ports on one instance only. Now that there is a LAN
for the machines to sit on, the relay belongs to it - bound once on behalf of every
machine and bridged to the real network - which is strictly better than the ownership
claim described above, and is not done yet.

MAC addresses are derived from the slot (`network-nat.c`), but `macaddress` is still a
per-machine setting and cloned machines that set it explicitly will collide.

## Decisions still open

- **Where the switch lives.** Inside the manager is convenient, since it is already
  long-lived, but then machines started by hand cannot join. A separate small service
  that the manager starts if it is not running follows how the relay behaves today.
- **Transport.** A UNIX datagram socket is simplest and is not available on Windows,
  which is a supported platform here. UDP on loopback works everywhere, provided it
  is pinned to loopback with a zero TTL so a virtual LAN cannot leak onto the real
  network.
- **Whether the uplink is shared.** One SLiRP for the whole switch is the VirtualBox
  model and means one NAT table, but it is more surgery. Leaving each machine its own
  NAT for outbound traffic and using the switch only for machine-to-machine and
  Access is much less work and probably indistinguishable in use.
- **Whether emulated machines should appear to real hardware** on the network, or
  whether a private LAN between them is enough. The first keeps the relay central
  and the switch bridging; the second is considerably simpler.

## What it costs

Each instance carries its own guest RAM, 32MB of translation tables and a code
cache, and its emulator thread will use a core. Three machines is roughly a gigabyte
and three busy cores. That is unremarkable on a development machine, and worth
knowing before starting six.
