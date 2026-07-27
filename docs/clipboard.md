# Shared clipboard

Copy text on the host, paste it in RISC OS, and the other way about.

It is **off by default**, because it puts your host clipboard within the guest's
reach. Turn it on in *Settings → Share Clipboard with RISC OS*. Nothing else is
needed: the guest half loads itself from the expansion card ROM and starts with
the desktop.

Text only at the moment. Images are not carried, though the interface has room
for them (see below).

## Where it came from

The design and the interface are **RiscOS Cloverleaf's**, from the RpcemuHelper
module in their RPCEmu fork at <https://github.com/riscoscloverleaf/rpcemu>. We
kept their SWI number and reason codes exactly, so their guest module and ours are
interchangeable, and two ideas of theirs are worth naming because they are what
makes this work well:

- **The guest hands the host RISC OS's own UCS conversion table.** Text is then
  converted through the alphabet the machine is actually configured for, rather
  than one the host assumes.
- **The host announces a change with a pollword in guest memory**, which the
  guest's Wimp task waits on. Neither side polls the other.

`src/hostclipboard.c` is derived from their `src/hostclipboard.c` and carries
their copyright. The guest module is a fresh implementation in assembler, because
there is no RISC OS C toolchain in this build environment, but it speaks their
protocol; their copyright and the terms of their 2-clause BSD licence are in its
header. The Latin-1 UCS table it carries is **NetSurf's** (Copyright 2005
John M Bell, GPLv2), by way of Cloverleaf's `ucstables.c`.

Their module also did mouse wheel scrolling. That part is not here, since this
emulator does wheel scrolling itself.

## Using it

Turn it on, and it works. Copy in a host application, then paste in a RISC OS one
(Ctrl+V in Edit, say); copy in RISC OS and paste on the host.

There is no notification when a RISC OS application copies something, because
RISC OS has no such event. Cloverleaf's answer, kept here, is to take key and
mouse activity as the cue to go and ask whoever owns the clipboard whether its
contents have changed. In practice that means a copy in the guest reaches the
host as soon as you touch the keyboard or mouse again, which is immediately in
any normal use.

Three commands, mostly for looking into it:

```
*SharedClipStatus     whether the task is running, and what is on the clipboard
*SharedClipPut <text> put text on the host's clipboard
*SharedClipGet        show what is on the host's clipboard
```

The last two are how the connection can be tested without a desktop application,
and they are useful over the HostCmd socket.

## How it fits together

| Piece | What it does |
| --- | --- |
| `src/hostclipboard.c` | The SWI, the host's copy of the clipboard, and the conversion between UTF-8 and the guest's alphabet |
| `src/gui/main_frame.cpp` | Watches the host clipboard and sets it. wxWidgets has no change notification, so it looks twice a second while the feature is on |
| `riscos-progs/SharedClipboard/` | The guest module: **Shared Clipboard Helper**, a Wimp task that speaks the RISC OS clipboard protocol |
| `poduleroms/sharedclipboard,ffa` | The assembled module, carried in the expansion card ROM so it loads at boot |

The SWI is `&56AD0` (the emulator's private chunk + `&10`), with the reason in R0:

| R0 | Reason | In | Out |
| --- | --- | --- | --- |
| 1 | Setup | R1 = pollword address (0 to withdraw), R2 = alphabet, R3 = 256-entry UCS-4 table | |
| 2 | Host set | R1 = data, R2 = length, R3 = filetype | |
| 3 | Host get | R1 = buffer, R2 = its size | R0 = length written |
| 4 | Host check | | R0 = bytes available, R1 = filetype |

With the feature off the SWI is not claimed at all, so the guest sees "Unknown
SWI" and can tell the difference between "off" and "nothing on the clipboard".

## The guest module

`SharedClipboard` is a single token where names are parsed (`*RMKill`,
`*RMEnsure`, the module header); **Shared Clipboard Helper** is what `*Modules`
and the Task Manager show.

It becomes a Wimp task the way ROM modules do: on `Service_StartWimp` it claims
the service and returns a command for the Wimp to run, which enters the module as
an application. Two things there are worth knowing if this is ever changed, since
both cost an afternoon:

- **Calling `Wimp_StartTask` from inside the service call re-enters the Wimp while
  it is still starting**, and the desktop never comes up: the machine stops with
  an abort during boot.
- **The Wimp keeps issuing `Service_StartWimp` until nobody claims it**, so that
  every module wanting a task gets one. Claim it every time and it starts another
  copy each time round, until the Wimp gives up with "Too many tasks". Claim
  exactly once.

Once running it holds the clipboard on the host's behalf (`Message_ClaimEntity`),
answers `Message_DataRequest` from applications with the text, and asks other
applications for theirs when they claim the clipboard. Transfers go through
`<Wimp$Scrap>` or in memory, whichever the other application asks for.

The clipboard buffer is 64K of RMA, claimed when the task starts. Anything larger
is truncated.

## Adding images later

Nothing in the interface needs to change: the filetype travels with the data, and
both halves already carry it. What is needed is encode and decode on the host
(`wxImage` can do PNG and JPEG) and letting the two non-text filetypes through in
`hostclipboard.c` and the module's `take_host_clipboard`. Cloverleaf's module
offers `&C85` (JPEG) and `&B60` (PNG) in its filetype list, which is the list to
match.

## Testing it

`tests/test_clipboard.c` covers the conversion in both directions, including
characters RISC OS has and Unicode does not (and the reverse), truncation into a
small buffer, and the feature being off.

The guest half was verified on a booted RISC OS 5.31 machine, in both directions,
with small BBC BASIC Wimp tasks standing in for applications: one broadcasting
`Message_DataRequest` for the clipboard and writing what came back to a file, the
other claiming the clipboard and handing over text when asked. Text put on the
host's X clipboard arrived in the guest application unchanged, and text an
application copied in RISC OS arrived on the host's clipboard unchanged.
