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

There is no RISC OS C toolchain on the host, so the Acorn tools (`cc`, `objasm`,
`cmunge`, `link`) have to run somewhere that RISC OS does. `build.sh` cannot
rebuild this. There are two routes, and the first is the one to reach for.

### Through the RISC OS Build Service (preferred)

`build.riscos.online` runs the toolchain under emulation and hands back the
linked module. It is driven by two files here: `.robuild.yaml`, which lists the
commands to run on RISC OS, and `MakefileROBS,fe1`, the makefile those commands
drive. This is also what CI does, as the `etherrpcem-riscos` job in
`.github/workflows/build.yml`, which drives the service through
[`gerph/riscos-build-service-action`](https://github.com/gerph/riscos-build-service-action)
rather than calling the client itself.

To trigger a build by hand you need the `riscos-build-online` client. Either use
the copy in the RISC OS build environment, or fetch it:

```sh
curl -s -L -o riscos-build-online \
    https://github.com/gerph/robuild-client/releases/download/v0.07/riscos-build-online
chmod +x riscos-build-online
```

Then, from this directory:

```sh
# The archive must be rooted here, because .robuild.yaml and the makefile
# expect to be at the top of it. bin2c_src is the C source of the bin2c tool,
# kept for reference and not needed to build.
zip -q9r /tmp/etherrpcem-src.zip . -x 'bin2c_src/*'

riscos-build-online -i /tmp/etherrpcem-src.zip -a off -t 120 -o /tmp/built
```

The build log comes back on stdout. On success the module is returned as a RISC
OS Zip archive, `/tmp/built,a91`:

```sh
unzip -o /tmp/built,a91 -d /tmp/etherrpcem
cp /tmp/etherrpcem/EtherRPCEm ../../netroms/EtherRPCEm,ffa
```

`riscos-build-online` exits non-zero if the build fails, and prints `RC: 0` on
a build that ran to completion. Note that a successful run can still return an
empty archive if `.robuild.yaml`'s `artifacts.path` is wrong, so check what came
back rather than trusting the exit code alone.

`MakefileROBS,fe1` also works with `riscos-amu` in the RISC OS build
environment, with one exception: `c.AutoSense` is produced by `bin2c`, which is
a RISC OS absolute and cannot run on a Linux host. Override it to build there:

```sh
riscos-amu -f MakefileROBS,fe1 BUILD32=1 BIN2C="path/to/host/bin2c"
```

### Inside an emulated machine

The original route, using the Acorn DDE in a running guest, exactly as
`SharedClipboard` is built. Still works, and is the only way to build against
the DDE actually installed in a machine.

1. Copy this directory into a machine's HostFS as `$.EtherBld`. It is already in
   RISC OS layout (`c.Module`, `s.intveneer`, `h.*`, `cmhg.ModHdr`, `o.`), so no
   renaming is needed. `bin2c,ff8`, `AutoSense/` and `MkEther,feb` must come too;
   `bin2c_src/`, `MakefileROBS,fe1` and the `.md` files are not needed in the guest.
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

### Which files are generated

Neither `c.AutoSense` nor `h.ModHdr` is source, and neither is committed.
`c.AutoSense` is the tokenised BASIC in `AutoSense.EtherRPCEm` turned into a C
array by `bin2c`, so that the BASIC stays the only copy anybody has to edit;
`h.ModHdr` comes from `cmhg.ModHdr`. Both makefiles build them.

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
- `c.Module`, `find_protocol`: was `static inline`. `inline` is C99, and Norcroft
  rejects it outright rather than ignoring it, which was the one thing stopping
  the sources compiling as C89. Dropped, not worked around: it is a small
  single-caller function and the compiler is free to inline it anyway.
- `cmhg.ModHdr`: the licence header used `#` for its comments. CMunge is run
  with `-px`, which puts the file through the C preprocessor first, and every
  one of those lines then read as an unknown preprocessor directive. Now `;`,
  which is CMHG's own comment character, so the text is unchanged.

Note that the `//` comments throughout are also not C89, but Norcroft accepts
them, and converting them would touch nearly every line for no gain and make
future comparison against upstream RPCEmu harder. They have been left alone.

## Known gaps

Not defects introduced here, but things the driver claims and does not do:

- `INQUIRE_FLAGS` advertises `INQ_MULTICAST` and `INQ_PROMISCUOUS`. Neither is
  implemented. `work.flags` is computed from the claims list and then never used
  or passed to the host, and `DCI4MulticastRequest` is a stub.
- `DCI4Stats` reports three frame counters. Byte counts and every error counter
  are permanently zero, while the `supported` table says they are gathered.
- Only unit 0 exists, and the MTU is fixed at 1500 (`DCI4SetNetworkMTU` returns
  "not supported").
