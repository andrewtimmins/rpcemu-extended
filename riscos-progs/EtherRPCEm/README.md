# EtherRPCEm (guest network driver)

The RISC OS half of the emulated network card: a **DCI4 driver** that presents
itself to the Internet stack as an Ethernet interface and moves frames with a
private SWI (`&56AC4`) the emulator intercepts. There is no NIC chip to drive,
so the hardware-specific half of a real driver is simply absent.

It is Castle's **EtherY** driver with the hardware removed, by J Ballance, and
adapted for RPCEmu by Alex Waugh. Their copyright and GPL v2 terms are retained
in each file; `Notes` has the original EtherY notes, including the fact that the
driver name (`EtherRPCEm`, `rpcem`), the SWI chunk and the error chunk are
allocated to RPCEmu.

The assembled module is committed as `netroms/EtherRPCEm,ffa` and the emulator
builds a podule ROM around it at run time (`src/network.c`).

## Building it

There is no RISC OS C toolchain on the host, so this is built **inside the
emulator** with the Acorn DDE (`cc`, `objasm`, `cmhg`, `link`), exactly as
`SharedClipboard` is. `build.sh` cannot rebuild it.

1. Copy this directory into a machine's HostFS as `$.EtherBld`. It is already in
   RISC OS layout (`c.Module`, `s.intveneer`, `h.*`, `cmhg.ModHdr`, `o.`), so no
   renaming is needed. `bin2c,ff8`, `AutoSense/` and `MkEther,feb` must come too;
   `bin2c_src/`, `Makefile` and the `.md` files are not needed in the guest.
2. With the machine running, from the host:
   `./build/bin/rpcemu-run --socket <datadir>/hostcmd.sock -- "Obey HostFS::HostFS.$.EtherBld.MkEther"`
3. Every tool's output lands in `$.EtherBld.log.*`, never through the command
   channel, which a compiler's output will otherwise block. `log.finished` only
   appears if the whole Obey file ran, so a build that fell over part way
   through cannot be mistaken for one that completed.
4. The module appears as `$.EtherBld.EtherRPCEm`. Copy it to `netroms/`.

`MkEther,feb` runs the tools in order. Its commands are the `Makefile`'s own,
minus `-throwback` and `-depend`: those want SrcEdit and a `!Depend` file and buy
nothing in a headless build. The `Makefile` is kept for building under the desktop
with `!Make`, but note it names `C:o.stubsg`, which the DDE in use here does not
have; `MkEther` links `C:o.stubs`. See below.

## The stubs the module is linked against

Worth knowing before comparing a rebuild with what is committed, because the two
will not match and that is expected.

The committed binary (12884 bytes, built 17 Oct 2024) was linked against
`stubsg`, a C library stubs variant that carries 26-bit veneers and pulls in
library code (`qsort`, `bsearch`, `partition_sort`, `atexit`, signal veneers).
The DDE installed in the OS-530 machine has only `C:o.stubs`, so a rebuild there
comes out **3 KB smaller** and instead RMEnsures its dependencies at
initialisation:

```
RMEnsure SharedCLibrary 5.17 RMLoad System:Modules.CLib
RMEnsure FPEmulator 4.03 RMLoad System:Modules.FPEmulator
RMEnsure UtilityModule 3.70 RMEnsure CallASWI 0.02 RMLoad System:Modules.CallASWI
```

That matters because the module loads from a podule ROM early in the boot, when
`System:` does not exist yet: if one of those RMEnsures were not already
satisfied by the ROM, the RMLoad would fail and the module would not
initialise. Checked, and satisfied without any RMLoad, on **RISC OS 5.30** and
**RISC OS 3.71** (both have SharedCLibrary 5.17 or newer).

Separately, and not caused by this: the module does not appear in `*Modules` on
the RISC OS 3.71 machine at all, with either the committed binary or a rebuild.
Whatever is behind that predates this work.

## Changes made in this fork

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
  implemented. `work.flags` is computed from the claims list and then never used
  or passed to the host, and `DCI4MulticastRequest` is a stub.
- `DCI4Stats` reports three frame counters. Byte counts and every error counter
  are permanently zero, while the `supported` table says they are gathered.
- Only unit 0 exists, and the MTU is fixed at 1500 (`DCI4SetNetworkMTU` returns
  "not supported").
