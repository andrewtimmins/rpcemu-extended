# EtherRPCEm (guest network driver)

The RISC OS half of the emulated network card: a **DCI4 driver** that presents
itself to the Internet stack as an Ethernet interface and moves frames with a
private SWI (`&56AC4`) the emulator intercepts. There is no NIC chip to drive,
so the hardware-specific half of a real driver is simply absent.

## Whose work this is

It is Castle Technology's **EtherY** driver with the hardware removed, by
**J Ballance**, adapted for RPCEmu by **Alex Waugh**, and transcribed into
assembler for this fork by **Andy Timmins** in 2026. The DCI4 structure layouts
it hardcodes are **Acorn Computers Ltd's**, from their published DCI4 headers.

    Copyright (C) 2003 J Ballance / Castle Technology   (as EtherY)
    Copyright (C) 2007 Alex Waugh                       (adaptation for RPCEmu)
    Copyright (C) 2026 Andy Timmins                     (this assembler port)

Released under **GPL v2**, the terms EtherY was released under. That chain and
the licence notice head `etherrpcem.s`; the full licence is in `LICENSE`; and the
credit is also in the module's own help string, so `*Help EtherRPCEm` shows it
inside the machine.

Two things from the original EtherY release notes, which are worth stating here
because they are allocations rather than history. The driver name (`EtherRPCEm`
and `rpcem`), the SWI chunk and the error chunk are allocated to **RPCEmu**, not
to this fork, and not to Castle: EtherY's own name (`EtherY`, `ey`) and chunks
stayed with Castle, and anyone adapting this driver for different hardware needs
their own allocation rather than reusing either set. Castle released their
snapshot with no warranty that it would even build. The notes themselves are in
git history at `9e44930:riscos-progs/EtherRPCEm/Notes`.

The assembler is a transcription, not new work built from a description: the
behaviour, the structure offsets and the DCI4 protocol are theirs. Only the five
deliberate departures listed below, and the two bugs noted with them, are this
fork's.

The assembled module is committed as `netroms/EtherRPCEm,ffa` and the emulator
builds a podule ROM around it at run time (`src/network.c`).

## Building it

`etherrpcem.s` is assembled on the host with the ARM binutils, like the other
guest modules:

    ./build.sh --podules          # or: make AS=arm-linux-gnueabi-as ...

Install the tools with `./setup-build-env.sh --podules`. The result lands in
`netroms/`, and `tests/check-guest-modules.sh` rebuilds it and fails if what is
committed no longer matches the source - a check CI runs, so a change to this
file that is not rebuilt cannot go unnoticed.

`tests/test_etherrpcem_layout.c` reads the structure offsets back out of the
assembler source and checks them against `src/network.h` and against its own
models of Acorn's DCI4 structures, transcribed from Acorn's own headers and
pinned during the port by compiling them with `gcc -m32` (see below for where
those headers are now). It does not include them, and nothing in the build does:
assembler cannot include a C header, which is the whole reason for the test - a
wrong offset assembles perfectly and shows up as a data abort in the middle of a
boot.

### The C it was ported from

The module was C until 2026, built **inside the emulator** with the Acorn DDE
(`cc`, `objasm`, `cmhg`, `link`) exactly as `SharedClipboard` still is, because
there is no RISC OS C toolchain on the host. `build.sh` could not touch it, so
nobody could change the driver without first setting up a DDE inside a guest.

Those sources are not kept here. `c/Module`, `s/intveneer`, `s/errors`,
`cmhg/ModHdr`, the `h/` headers, the `MkEther,feb` Obey file and the DDE
`!Make` project file that drove them are in git history, complete, at the
commit that made the port:

    git show 9e44930:riscos-progs/EtherRPCEm/c/Module
    git ls-tree -r 9e44930 riscos-progs/EtherRPCEm

Anyone comparing the two should read them there. Nothing in the tree built them
after the port, and keeping a second copy of code the emulator cannot compile
invited it drifting out of step with `etherrpcem.s`, which is the file that is
actually built.

Two things about the C are worth carrying forward, because they are the argument
for the port rather than history:

- **It needed the shared C library**, from a module that loads out of a podule
  ROM before `System:` exists. It RMEnsured `SharedCLibrary` 5.17, `FPEmulator`
  4.03 and `CallASWI` 0.02 at initialisation, and an RMLoad would have failed
  that early if the ROM had not already satisfied them. It always did, on RISC OS
  5.30 and 3.71 both. The assembler needs no C library at all, so the question
  does not arise.
- **Size.** 4,624 bytes assembled, against 9,952 for the last C build, and 12,884
  for the binary shipped in October 2024, which was linked against `stubsg` and
  so carried 26-bit veneers and library code (`qsort`, `bsearch`,
  `partition_sort`, `atexit`, signal veneers) the driver never called.

## Changes made in this fork

The assembler port, 2026, is the largest: same behaviour, built on the host. What
it deliberately does differently from the C is listed at the top of
`etherrpcem.s`, and comes to five things - no shared C library, workspace claimed
in the RMA rather than living in the module's static data, the expansion card base
saved at initialisation instead of trusting R11 at finalisation, the
initialisation callback removed at finalisation, and a failed initialisation
undoing what it had already done (RISC OS never calls the finalisation entry of a
module whose initialisation returned an error, so the C left the Mbuf session open
and the device vector claimed).

Verified against the C build on a real boot of RISC OS 5.30 at both 256MB and
128MB: same module list entry, byte-identical `*ERPCEmInfo` output including all
four filter claims, 4/4 ping, an 8,008-byte ping (six inbound fragments) and a
`*RMKill` / `*RMReInit` cycle after which networking still worked.

Before that, two bugs fixed in the C itself (both at `9e44930`, if you want to
see them):

- `s.intveneer`: `networktxswi` tested its result with `TST a1, #0`. `TST` is a
  bitwise AND, so with `#0` the result is always zero and Z is always set, which
  made the `LDRNE a1, =errbuf` on the next line dead code. A transmit error
  therefore returned a pointer to the *message text* rather than to the error
  block, and RISC OS read the first four characters of the message as the error
  number. Now `TEQ`, which is what `networkrxswi` had always used.
- `c.Module`, `DCI4MulticastRequest`: the flag test was
  `if (r->r[0] && ~3)`, a logical AND against a constant that is never zero, so
  it fired for any non-zero flags instead of the undefined ones it meant to
  catch. It only guarded a `printf`, which is not safe from a SWI handler.
  Both are gone; the behaviour they wrapped (accept and ignore) is unchanged.

## Known gaps

Not defects introduced here, but things the driver claims and does not do:

- `INQUIRE_FLAGS` advertises `INQ_MULTICAST` and `INQ_PROMISCUOUS`. Neither is
  implemented. The interface flags are computed from the claims list and then
  never used or passed to the host, and `Multicastreq` accepts and ignores.
- `DCI4Stats` reports three frame counters. Byte counts and every error counter
  are permanently zero, while the "what is gathered" table says they are
  gathered.
- Only unit 0 exists, and the MTU is fixed at 1500 (`SetNetworkMTU` returns
  "not supported").
- `init_chip` clears the claims list. It is called again if the Mbuf manager
  restarts, so on that path any existing claims are dropped without being freed.
  Carried over from the C rather than changed, because every protocol module has
  to re-register across a manager restart anyway.
- The AutoSense file is saved with the file type alone in the load address, so it
  ends up untyped rather than as a BASIC file. Carried over from the C; what
  reads it back finds it by name.
- Kept from the C and still true: the module does not appear in `*Modules` on
  RISC OS 3.71 at all. Verified by A/B with the C build, so the assembler port
  did not cause it, and it predates both.
