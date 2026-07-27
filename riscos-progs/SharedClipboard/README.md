# Shared Clipboard (guest module)

The RISC OS half of the shared clipboard: **RiscOS Cloverleaf's** RpcemuHelper
module from <https://github.com/riscoscloverleaf/rpcemu>, with their clipboard
protocol untouched and these changes for this fork:

- mouse wheel scrolling removed. This emulator does wheel scrolling itself, and
  their module claimed Alt+PageUp/PageDown outright, so applications in the guest
  never saw those keys.
- renamed: module `SharedClipboard`, task "Shared Clipboard" (the Task Manager
  truncates anything longer), commands `*SharedClipStart`,
  `*SharedClipStartTask`, `*SharedClipDebug`.
- starts its own task on `Service_StartWimp`, so nothing has to run a command at
  boot. The service is claimed exactly once: the Wimp keeps asking until nobody
  claims, and claiming every time starts a task each round until it gives up with
  "Too many tasks".
- small changes for the Norcroft compiler: no `__attribute__`, no zero-length
  array, explicit casts on `_swix` results and OSLib's `wimp_t`.

Their copyright and 2-clause BSD terms are retained in each file. `c/ucstables`
is NetSurf's (Copyright 2005 John M Bell, GPL v2), as it was in theirs.

## Building it

There is no RISC OS C toolchain on the host, so this is built **inside the
emulator** with the Acorn DDE (`cc`, `cmhg`, `link`) and OSLib, both of which
live in the guest's `AcornC/C++` directory. `build.sh` therefore cannot rebuild
it, and the assembled module is committed as `sharedclipboard,ffa`.

To rebuild:

1. Copy this directory into a machine's HostFS as `$.SharedClip`, and `MkClip,feb`
   as `$.MkClip`.
2. With the machine running, from the host:
   `./build/bin/rpcemu-run --socket <datadir>/hostcmd.sock -- "Obey HostFS::HostFS.$.MkClip"`
3. Tool output lands in `$.SharedClip.log.*` (never through the command channel,
   which a compiler's output will otherwise block), and the module appears as
   `$.SharedClip.sharedclipboard`.
4. Copy it back to `poduleroms/`.

Two things that will waste your time otherwise: HostFS swaps `.` and `/`, so the
host's `AcornC.C++` directory is `AcornC/C++` inside RISC OS; and cmhg builds the
help string from the first word of `help-string:`, so a two-word name there comes
out mangled.
