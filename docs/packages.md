# Package manager

RPCEmu can install software packaged for RISC OS straight onto a machine's hard disc,
from the same repositories a real RISC OS machine would use. Open a machine, then
**Tools → Package Manager**.

There are around 200 packages: applications, games, fonts, libraries and system
components. Installing one puts its files on the disc and **records what it put there**,
so it can be removed again cleanly.

---

## Where the packages come from

Three indexes are read, all of them in the format the RISC OS Packaging Project defines.

| Source | What it holds |
| --- | --- |
| `rool` | Applications from the RISC OS disc image, built nightly by RISC OS Open |
| `thirdparty` | Programs and libraries from other authors, for any ARM machine |
| `community` | Released packages from the RISC OS Community repository |

**Only sources whose code can run here are offered.** RISC OS Open also publish an
`programs-armv5` index and two Raspberry Pi ones; the Risc PC's StrongARM is **ARMv4**, so
those hold code that cannot run on it. Individual packages are filtered the same way, on
their `Environment` field: `any`, `arm` and `arm32` are accepted, and a package with no
`Environment` is treated as portable, which is how the older third-party records read.

The `arm32/testing` index at riscoscommunity.org is deliberately not offered. It is
unstable by design.

The catalogue is cached under your data directory (`pkgcache/`) for six hours, so
browsing costs nothing and the servers are not asked for the same file repeatedly. Press
**Refresh list** to fetch regardless. Every request identifies itself as RPCEmu, the same
courtesy as the RISC OS download.

---

## Installing

Select a package and press **Install**. What happens:

1. The zip is downloaded to a temporary file.
2. **Its MD5 is checked against the one the index gives.** A download that does not match
   is refused rather than half-installed.
3. The files are unpacked onto the machine's disc, with the RISC OS filetypes from the
   archive's Acorn extra field, so an Obey file arrives as `,feb` and RISC OS will run it.
4. What was written is recorded in the machine's package database.

If a package says it **depends** on others, you are told and offered them as well.
`Recommends` are not followed: they are suggestions, and installing more than you asked
for is not the manager's decision to make.

**If a Filer window for the target directory is already open, choose Refresh from its
menu.** RISC OS caches directory listings, so files that appear underneath it are not
shown until it re-reads.

## Removing

**Remove** deletes exactly the files the install recorded and prunes only the directories
that emptied as a result.

If some of those files are already gone, that is fine and it says how many. But if files
are still sitting where the record says they should be, something is wrong with the
paths: the database is left alone and the package stays recorded, rather than being
declared removed while its files remain. A package with nothing able to remove it is
worse than a package that refuses to go.

---

## Packages are per machine

The database lives **on the machine's own disc**, not in a registry kept by the emulator:

```
machines/<machine>/hostfs/!Packages/
```

That is deliberate, and it has consequences worth knowing:

- Two machines have entirely separate package lists. Installing on one does not affect
  the other.
- Copy, clone or rename a machine and its package list travels with it, because the
  record and the files are on the same disc.
- Delete a machine and nothing is left orphaned elsewhere.
- **RISC OS can read it too.** `*Type $.!Packages.Status` in the guest lists what is
  installed, and other RISC OS package tools see these as ordinary installed packages.

The paths come from the same rule the emulator uses for HostFS itself
(`<machine data directory>/hostfs`), so there is no second definition to get out of step
with. Only the catalogue cache is shared between machines, since the list of what exists
is not machine-specific.

---

## The database

In the RISC OS Packaging Project's own format, which is what makes the above true:

| File | What it is |
| --- | --- |
| `Status` | One tab-separated line per package: name, version, state |
| `Info/<Package>/Control` | The package's control record |
| `Info/<Package>/Copyright` | Its copyright and licence statement |
| `Info/<Package>/Files` | Every path it installed, as RISC OS names them |
| `Info/<Package>/HostFiles` | The same files as the host wrote them (ours; see below) |
| `Version` | The database format version |

`HostFiles` is the one addition, and it exists for a specific reason. `Files` holds RISC OS
paths, as the format requires, but a RISC OS path does not say what the file is called on
the host: HostFS adds a `,xxx` filetype suffix on the way down. Rediscovering that by
matching `leaf*` risks deleting a neighbour whose name merely begins the same way, so the
exact host paths are recorded alongside and removal unlinks precisely those. Other tools
have no reason to read it, and `Files` remains exactly as the format specifies.

**★ A trap for anyone working on this.** In a RISC OS archive `.` and `/` are
**exchanged**, because RISC OS uses `.` as its directory separator. A file called
`help.css` is stored with the two swapped and is `help/css` to RISC OS. Treating every dot
as a separator turns it into a directory `help` containing a file `css`, and then nothing
can find what it installed. The swap is its own inverse, which is what makes a path
survive the round trip.

---

## Without the interface

Every part of this works from the command line and needs no display, which is how it is
tested:

```bash
./rpcemu-recompiler --pkg-list                    # the whole catalogue
./rpcemu-recompiler --pkg-list=games              # matching name, section or description
./rpcemu-recompiler --pkg-info=ChangeFSI          # everything about one package
./rpcemu-recompiler --pkg-install=Flasher --pkg-machine=Default
./rpcemu-recompiler --pkg-remove=Flasher  --pkg-machine=Default
```

`--pkg-machine` is required for installing and removing: there is no ambiguity about which
machine's disc is being written to.

---

## What is not done yet

- **`Triggers` are not run, and `OSDepends` is not checked.** A package can carry scripts
  to run on install and removal, and can name RISC OS modules it needs. Both want asking
  the running machine, which needs an internal route into the HostCmd channel that does
  not exist yet. Most packages have neither.
- **No upgrade column.** A package already installed offers **Reinstall** rather than
  telling you a newer version exists, though the version comparison is already there.
- **Deprecated `Sprites` and `Sysvars` directories** are installed as ordinary files
  rather than merged into the sprite pool. The packaging guide describes both as
  deprecated in favour of the RISC OS Boot mechanism, but some packages still ship one.
- **A dead entry in an index cannot be known in advance.** `LibGreatest` in the community
  index points at a file that returns 404; installing it reports the address that failed
  rather than the entry being hidden, because whether a URL serves anything cannot be
  known without asking.

---

## With thanks

The **RISC OS Packaging Project** for the package and database format this implements,
defined in its policy manual; the format is **Graham Shaw's** design, and the manual is
maintained by **Alan Buckley**. **RISC OS Open Limited** and **riscoscommunity.org** host
the indexes and the packages themselves.

None of their code is used here. This is an implementation of a published specification,
and it keeps to that specification so that the machines it installs onto remain usable by
the project's own tools, **PackMan** and **RiscPkg**.
