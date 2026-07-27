# Shared clipboard

Copy text on the host, paste it in RISC OS, and the other way about.

It is **off by default**, because it puts your host clipboard within the guest's
reach. Turn it on in *Settings → Share Clipboard with RISC OS*. Nothing else is
needed: the guest half loads itself from the expansion card ROM and starts with
the desktop.

Switching it on while a machine is running is enough; the guest reconnects the
next time you touch the keyboard or mouse. A **reset is only needed the first
time**, so the machine picks up the guest module from the expansion card ROM.
`*SharedClipStatus` says which of those two things is missing:

```
Shared clipboard: task running        <- the module is loaded
  Connected to the host               <- ...and sharing is on
```

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
their copyright. **The guest module is theirs**, with the clipboard protocol
untouched and four changes for this fork:

- mouse wheel scrolling removed. This emulator does that itself, and theirs
  claimed Alt+PageUp/PageDown outright, so applications in the guest never saw
  those keys. That is why it could not simply be used as it was.
- renamed: module `SharedClipboard`, task "Shared Clipboard", commands
  `*SharedClipStart`, `*SharedClipStartTask`, `*SharedClipDebug`.
- it starts its own task on `Service_StartWimp`, so nothing has to run a command
  at boot.
- small changes for the Norcroft compiler, since it is built with the RISC OS DDE.

Their copyright and 2-clause BSD terms are in every file, our modifications are
noted at the top of the ones we touched, and `c/ucstables` remains **NetSurf's**
(Copyright 2005 John M Bell, GPL v2).

An earlier attempt to reimplement the module in assembler is in the history
(deleted at commit 097ec54). It passed every test written for it and never worked
with a real editor, which is worth knowing before anyone tries again: see
"Testing it" below.

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
`*RMEnsure`, the module header); **Shared Clipboard** is what `*Modules` and the
Task Manager show.

It is C, built **inside the emulator** with the Acorn DDE (`cc`, `cmhg`, `link`)
and OSLib, both of which are in the guest's own `AcornC/C++` directory. `build.sh`
therefore cannot rebuild it and says so; the module is committed as
`poduleroms/sharedclipboard,ffa`. The recipe, and the traps, are in
`riscos-progs/SharedClipboard/README.md`.

It becomes a Wimp task the way ROM modules do: on `Service_StartWimp` it claims
the service and hands back a command for the Wimp to run. Two things there cost
an afternoon and are worth writing down:

- **Calling `Wimp_StartTask` from inside the service call** re-enters the Wimp
  while it is still starting: the machine aborts during boot and the desktop never
  appears. Claim the service and return a command instead.
- **The Wimp reissues `Service_StartWimp` until nobody claims it**, so that every
  module wanting a task gets one. Claim it every time and it starts another copy
  each round until it gives up with "Too many tasks".

## Adding images later

Nothing in the interface needs to change: the filetype travels with the data, and
both halves already carry it. What is needed is encode and decode on the host
(`wxImage` can do PNG and JPEG) and letting the two non-text filetypes through in
`hostclipboard.c` and the module's `take_host_clipboard`. Cloverleaf's module
offers `&C85` (JPEG) and `&B60` (PNG) in its filetype list, which is the list to
match.

## Testing it

`tests/test_clipboard.c` covers the host side: conversion both ways, characters
one alphabet has and the other does not, truncation, the feature being off, and
what survives a machine reset.

**The guest half can only be judged with real applications.** Small Wimp tasks
written to speak the protocol by the book are not a proxy for Edit or StrongED:
they ask the clipboard's owner directly, and on a desktop with a Clipboard Manager
running nothing answers that, so such tests pass against a module that real
editors cannot talk to, and fail against one they can. Test with an editor.
