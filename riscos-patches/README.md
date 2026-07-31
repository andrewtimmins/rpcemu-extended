# Patches to third-party RISC OS components

Changes we make to RISC OS components that are not ours, kept as patches against
a named upstream revision rather than only as a modified binary.

`riscos-progs/` holds guest modules we wrote. This directory holds our changes to
somebody else's, so that a binary we ship can be inspected, rebuilt, offered
upstream, and rebased when a new upstream version appears.

Only one component has needed changing so far. The rest of what we ship from other
people is built from their source untouched, and is recorded at the bottom of this
file so that a binary can still be traced back to a commit.

## ohcidriver

RISC OS Open's OHCI host controller driver, shipped as
`usbroms/20-ohcidriver,ffa` and started from the USB expansion card's ROM. See
[docs/usb.md](../docs/usb.md) for the card, and `usbroms/LICENCES.txt` for the
licence notices these binaries oblige us to carry.

- **Upstream:** <https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/USB/Controllers/OHCIDriver>
- **Baseline:** tag `OHCIDriver-0_56`, commit `551b4047`, 01 Feb 2021
- **Licence:** Apache 2.0 for the ROOL and Castle files, BSD for the NetBSD ones

| | |
| --- | --- |
| `0001-search-expansion-cards-for-a-controller.patch` | The functional change. Without it the driver finds nothing on a Risc PC, because it asks the HAL and `HAL_IOMD` has no USB support. |
| `0002-debuglib-output-to-a-hostfs-file.patch` | A debugging aid, and deliberately separate: it hardcodes a HostFS path and compiles only into a `DEBUG=TRUE` build, so it is not for upstream and is not in the shipped module. |

Both apply with `patch -p1` from the root of that checkout, 0001 first. Applied in
order to a clean `OHCIDriver-0_56` they reproduce the guest build tree's
`c/ohcimodule` byte for byte.

```sh
git clone https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/USB/Controllers/OHCIDriver.git
cd OHCIDriver
git checkout OHCIDriver-0_56
patch -p1 < /path/to/rpcemu-extended/riscos-patches/ohcidriver/0001-search-expansion-cards-for-a-controller.patch
```

Only `c/ohcimodule` is touched. A checkout that has been built in the guest also
shows a modified `Makefile`, which is amu appending its own dependency list and
is not part of the change.

### The build is reproducible, and that is the point

Applying these patches to a clean `OHCIDriver-0_56` and building with
`amu standalone` produces `usbroms/20-ohcidriver,ffa` byte for byte:

```
69ba546e6f6d0c96c7c32e9b0af70047  usbroms/20-ohcidriver,ffa   (17,464 bytes)
```

Verified 30/07/2026 by cleaning every object and the cmhg-generated header,
rebuilding, and comparing. So the modified binary we ship is not something anyone
has to take on trust: the patch above is demonstrably the whole of the difference
between it and ROOL's published source.

Do not try to check this with `strings`. The link ends in `modsqz`, which
compresses the module body, so string literals from the C are not visible in the
file even when they are certainly compiled in. The only sound check is to rebuild
and compare the bytes.

### Rebuilding

The module is built inside the emulator with the RISC OS DDE, from
`RPCEmuBuild/OHCIDriver` in a machine's HostFS. It needs USBDriver built and
exported first, for `-IC:USB`, along with CALLXLIB and ASMUTILS. `amu standalone`
gives a RAM-loadable module; the `rom` target needs a generated `s.init` that is
not in the repository. See [docs/usb.md](../docs/usb.md).

## Components we ship unmodified

These have no patches, which is why they have no directory here. They are ROOL's
source built as it comes, and this is the record of which commit each binary was
built from - the thing a patch directory would otherwise tell you.

| Binary | Component | Version | Upstream commit |
| --- | --- | --- | --- |
| `usbroms/10-usbdriver,ffa` | USBDriver | 1.33 | tag `USBDriver-1_33` |
| `usbroms/30-rtsupport,ffa` | [RTSupport](https://gitlab.riscosopen.org/RiscOS/Sources/Programmer/RTSupport) | 0.18 | `c569874`, 11 Oct 2023 |
| `usbroms/40-scsidriver,ffa` | [SCSISwitch](https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/SCSI/SCSISwitch) | 2.15 | `5531025`, 13 Jul 2018 |
| `usbroms/50-scsisoftusb,ffa` | [SCSISoftUSB](https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/SCSI/SCSISoftUSB) | 0.28 | `dd44bd4`, 10 May 2020 |
| `usbroms/60-scsifs,ffa` | [SCSIFS](https://gitlab.riscosopen.org/RiscOS/Sources/FileSys/SCSIFS/SCSIFS) | 1.36 | `92870ae`, 15 Aug 2020 |

Each is built with `amu standalone` (`amu export` first, where a later component
needs its headers) in the same guest DDE as OHCIDriver, and each is reproducible
the same way: clean every object and the cmhg-generated header, build twice, and
the bytes match. Confirmed for SCSISoftUSB on 31/07/2026:

```
e76d382fe6f8aa54d6440a9296f588f4  usbroms/50-scsisoftusb,ffa   (11,524 bytes)
```

Note the trap that makes this worth stating. The obvious-looking component for the
SCSI side is ROOL's `HWSupport/SCSI/SCSIDriver`, and it is the wrong one: its
default build drives a WD33C93 chip and hangs a machine that has not got one. The
component that provides the SCSI SWIs and dispatches them to back ends is
SCSISwitch, whose own documentation calls itself "SCSIdriver 2" and which
registers under the name `SCSIDriver`. That is why the binary above is named
`40-scsidriver,ffa` while the component is SCSISwitch.

Three assembler headers the DDE does not carry are needed to build SCSIFS -
`FileCore`, `FileCoreErr` and `Portable` - taken from
[FileCore](https://gitlab.riscosopen.org/RiscOS/Sources/FileSys/FileCore) and
[Portable](https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/Portable) and
dropped into the build tree's own header set. See
[reference in docs/usb.md](../docs/usb.md).
