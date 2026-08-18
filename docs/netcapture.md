# Network capture

Every Ethernet frame a machine sends or receives can be recorded, watched as it
happens, or handed to another program. The idea is David Ramsden's.

Three ways in, all reading the same capture:

| | |
| --- | --- |
| *Settings → Network Capture…* | write a `.pcap` file |
| *Debug → Network Analyser…* | watch the frames, decoded, in a window |
| `rpcemu-netcap` | from a command line, or piped into Wireshark |

There is a fourth for anything driving the emulator over MCP: `riscos_netcap_status`,
`_start`, `_stop`, `_clear` and `_recent`.

Capture needs networking, so all of it is unavailable on a machine set to *Off*.

## Writing a file

*Settings → Network Capture…* asks for a path and writes
[pcap](https://wiki.wireshark.org/Development/LibpcapFileFormat), which
Wireshark, tcpdump and everything else of that kind reads. The file can be read
while it is still being written — each frame is flushed as it is recorded, so a
capture opened halfway through shows what has happened so far.

**Set the size limit.** A machine on a busy network fills a disc otherwise. The
limit is checked before each frame, so the file stops at or below it and never
part-way through a record: a truncated final record makes the whole file
unreadable to some tools rather than merely short. When it stops, it says so —
a capture that quietly stopped looks exactly like a network that quietly went
idle, which is what somebody would be trying to diagnose.

**"Start capturing when this machine starts"** is the one setting here that
outlives the window. It writes `network_capture` into the machine's
configuration, and is how the boot sequence is captured — DHCP, and Access
announcing itself — which cannot be caught by pressing Start afterwards.

Nothing else here needs a reset. That is why capture is on the Settings menu
rather than the machine editor's Network page: the reason to want a capture is
usually something going wrong now, and resetting destroys it.

## Watching the frames

*Debug → Network Analyser…* lists frames as they arrive, newest at the bottom:
number, local time, direction, protocol, source, destination, length, and what
the frame is doing. Selecting one breaks it into its fields and shows the bytes.

- **Capture** stops and starts keeping frames. Frames are only held in memory
  while something is looking, so the window turns it on when it opens and off
  when it closes.
- **Save as pcap…** writes out what is on screen, which is not the same as the
  capture file: this is the last few thousand frames the window has, whether or
  not a file was ever being written.
- **Follow** keeps the newest frame in view.

The window holds a few thousand frames and then forgets the oldest. It says how
many it has dropped rather than letting the list quietly stop being the whole
story.

## From a command line

`rpcemu-netcap` is the third of the host-side tools, beside `rpcemu-run`, which
drives the guest's command line, and `rpcemu-debug`, which drives the processor
underneath it.

```
rpcemu-netcap --status
rpcemu-netcap --tail 20
rpcemu-netcap --follow
rpcemu-netcap --start /tmp/rpc.pcap --max-bytes 104857600
rpcemu-netcap --stop
```

It finds the running machine by itself — a machine records the socket it bound
in its lock file, which is the only way to find one whose configuration named a
path of its own. Use `--machine NAME` when several are running, or `--socket
PATH` to say exactly.

### Live, in Wireshark

```
rpcemu-netcap --pcap - | wireshark -k -i -
```

Wireshark reads a pcap stream from a pipe, so this is the real thing — every
dissector it has, live, on a machine that has no network interface of its own
to point it at. Worth knowing about before reaching for the built-in analyser,
which is there for looking without leaving the emulator and for people who have
not got Wireshark.

`--pcap FILE` writes to a file instead, and `--raw` prints the JSON the socket
sends rather than a table.

## What the decode adds

Ethernet, ARP and RARP, IPv4 with TCP, UDP and ICMP, and 802.3 with LLC, which
is what Access uses.

The part no general-purpose tool has is the RISC OS end. **Freeway** and
**ShareFS** are unregistered UDP ports, so every other dissector shows a capture
of a RISC OS network as anonymous traffic between anonymous numbers:

```
No.  Time            Protocol  Source                      Destination            Len  Info
11   03:25:10.487  ↑ Freeway   10.112.5.6:32770 (Freeway)  10.255.255.255:32770    70  32770 > 32770, 28 bytes
```

Everything is read through bounds-checked helpers. A frame comes from a guest
that is free to emit a header claiming a length the frame has not got, and
trusting an IP header length field is how a dissector becomes the most exposed
thing in the program.

## The control socket

`netcapcmd` is the third socket, beside HostCmd and DebugCmd, and is built the
same way: one client at a time, serviced from the emulator thread, a line of
text in and a line of JSON out. On by default; `netcap_enabled` and
`netcap_socket` in the machine's configuration.

| | |
| --- | --- |
| `ping` | check it is alive |
| `status` | what is being captured, and how much |
| `start <path> [maxbytes]` | begin a file, 0 for no limit |
| `stop`, `clear` | |
| `ring on\|off` | keep frames in memory for `tail` and `follow` |
| `tail [n]` | the last n frames as JSON, then `end` |
| `follow` | every frame from now on, as JSON |
| `pcap` | the same, as a raw pcap stream — binary from here on |

## What it costs

**Nothing when nobody is capturing.** `netcap_frame()` is called on the emulator
thread for every frame, which is the thread that is the whole speed budget, and
with no file and nothing in memory it is two loads and a return. The counters do
not move either, which is deliberate: keeping them would mean taking a lock per
frame forever, and a diagnostic that becomes a cost on the path it watches is
the fault it was added to find.

**A slow reader cannot slow the guest.** Somebody piping a busy capture into
Wireshark can read slower than the machine sends. When the outbound buffer
fills, frames are dropped from *that stream* — not from the file, not from the
frames held in memory — and the client is told how many, because a capture with
a silent hole is worse than one that admits to it.

**Frames say what was missed.** Every frame has a serial that counts from one
and is never reused, so a reader that falls behind is told the oldest frame
still held and can tell exactly what it did not see.

## Limits

- The pcap format has no field for direction, so a file does not record which
  way a frame was going. The analyser and the socket both do.
- Frames longer than 1600 bytes are recorded truncated, with the real length
  kept — which is what pcap's two length fields are for.
- One client at a time on the socket, as with the other two.
