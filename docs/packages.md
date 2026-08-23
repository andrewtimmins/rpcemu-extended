# Package manager

RPCEmu can install software packaged for RISC OS straight onto a machine's hard disc,
from the same repositories a real RISC OS machine would use. Open a machine, then
**Tools → Package Manager**.

There are around 200 packages: applications, games, fonts, libraries and system
components. Installing one puts its files on the disc and **records what it put there**,
so it can be removed again cleanly.

> **Not in every build.** The package manager needs a wxWidgets with
> `wxWebRequest` support, which some distributions do not provide - Debian 12
> bookworm and the Raspberry Pi OS built on it among them. RPCEmu still builds
> and runs there, but *Tools -> Package Manager* and the `--pkg-*` options are
> absent and say so. See
> [wxWidgets and wxWebRequest](../COMPILE.md#wxwidgets-and-wxwebrequest).

---

## Where the packages come from

Each source is an index in the format the RISC OS Packaging Project defines. Four are
shipped, and the list is yours to change.

| Source | What it holds |
| --- | --- |
| `rool` | Applications from the RISC OS disc image, built nightly by RISC OS Open |
| `thirdparty` | Programs and libraries from other authors, for any ARM machine |
| `community` | Released packages from the RISC OS Community repository |
| `jaspp` | Games from the [Archimedes Software Preservation Project](https://www.jaspp.org.uk/), packaged to run on a machine like this one |

JASPP is the largest of the four by a distance and is almost entirely commercial games of
the period, preserved and packaged so they run on a 32-bit machine. Most of them need
**ADFFS**, which is in the same index and is offered as a dependency in the ordinary way.
They are marked `Licence: Non free`, which is what preservation of commercial software
looks like; what you may do with each one is between you and its copyright holder.

### ★ ADFFS and the graphics card cannot both be used

**ADFFS writes straight to screen memory at its physical address.** The
[graphics card](gfxcard.md) moves the framestore into its own memory, so those writes land
on nothing, the game takes a translation fault and aborts as soon as it draws. Confirmed
the hard way: with the card fitted the JASPP games abort immediately, and with it off they
run.

This is not really a bug in either. ADFFS assumes it can reach VRAM the way the hardware of
the day did, and a card with its own framestore legitimately breaks that assumption. A real
ViewFinder would do the same.

So the package manager says so rather than letting you find out:

- Selecting anything that is ADFFS or depends on it puts a line at the **top** of the
  details pane. If the card is fitted it names the setting to change; if not, it just notes
  the requirement for later.
- **Installing** one on a machine with the card fitted asks first, and can be cancelled.
- `--pkg-install` prints the note once. No machine is running for it to inspect, so it
  cannot tell whether the card is fitted and says it unconditionally.

Turn the card off in **Settings → Machine → Video** (`gfxcard_enabled=0`) and restart the
machine.

### Adding, editing and removing sources

**Tools → Package Manager → Sources…** Add a repository, edit one, turn one off without
losing it, remove one, or put the shipped list back. Changing anything refetches the
catalogue, because showing the old one afterwards would read as the change not having
taken.

The list lives in **`pkgsources`** in your data directory, and it is a plain text file in
the same shape as the indexes themselves, so it can be edited by hand:

```
Name: jaspp
URL: https://www.jaspp.org.uk/packages/release
Description: Games from the Archimedes Software Preservation Project (JASPP)
Enabled: yes
```

`--pkg-sources` prints the list and where it is stored, without touching the network.
Delete the file to get the shipped list back.

Three things are worth knowing before adding one:

- **The URL is the index file itself**, not the page it is linked from.
- **Use https where the server offers it.** macOS refuses cleartext HTTP by default, so an
  `http://` source silently fetches nothing there while working everywhere else.
- **The name becomes a filename**, because each source's index is cached under it, so it
  is limited to letters, digits, `-` and `_`. A name that would escape the cache directory
  is refused, whether it is typed into the dialogue or edited into the file.

A record the file parser cannot use is reported and skipped rather than costing you the
rest of the file, and a file with nothing usable in it falls back to the shipped list
rather than leaving you with an empty catalogue that looks like every repository is down.

**Only packages whose code can run here are offered.** RISC OS Open also publish a
`programs-armv5` index and two Raspberry Pi ones; the Risc PC's StrongARM is **ARMv4**, so
those hold code that cannot run on it, and they are not in the shipped list. Individual
packages are filtered the same way, on their `Environment` field: `any`, `arm` and `arm32`
are accepted, and a package with no `Environment` is treated as portable, which is how the
older third-party records read. JASPP's handful of 26-bit-only entries are dropped by that
same rule, and the count at the bottom of the window says how many.

The `arm32/testing` index at riscoscommunity.org is deliberately not in the shipped list.
It is unstable by design. Nothing stops you adding it.

The catalogue is cached under your data directory (`pkgcache/`) for six hours, so
browsing costs nothing and the servers are not asked for the same file repeatedly. Press
**Refresh list** to fetch regardless. Every request identifies itself as RPCEmu, the same
courtesy as the RISC OS download.

---

## Finding something

The search box matches on name, section and description, so typing `pinball` or
`printing` both work.

Above the list is a row of **section buttons**, one per category with the number of
packages in it, and **All** to clear the filter. They are counted from the catalogue
rather than being a fixed list in the code, so adding a repository makes its categories
appear by themselves. The eight largest get a button; the rest are reachable by typing
the section name into the search box, and the row says how many are not shown. Without
that cap, adding a couple of repositories would push the list of packages off the bottom
of the window, which would take more than it gave.

The section filter and the search box **narrow together** rather than replacing each
other: press *Games*, then type `pinball`. Pressing the section already selected turns it
off again, which is the same act as pressing *All*, and neither touches what is in the
search box. The count underneath says what is being shown and what it is filtered to,
for example "185 packages shown in Games, 0 installed".

A note on the categories themselves: they are whatever the packagers wrote in each
record's `Section` field, so they are not a tidy taxonomy. There is no "Applications"
section; the everyday programs are spread across `Desktop`, `Graphics`, `Document`,
`Printing` and others. The catalogue also carries both `Miscellaneous` and `Misc`, and
both `Document` and `Documentation`, which are counted separately because that is what
the indexes say.

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

The database lives **on the machine's own disc**, not in a registry kept by the emulator,
and in the place RISC OS itself keeps one:

```
machines/<machine>/hostfs/!Boot/Resources/!Packages/
```

That is deliberate, and it has consequences worth knowing:

- Two machines have entirely separate package lists. Installing on one does not affect
  the other.
- Copy, clone or rename a machine and its package list travels with it, because the
  record and the files are on the same disc.
- Delete a machine and nothing is left orphaned elsewhere.
- **RISC OS can read it too.** `*Type <Packages$Dir>.Status` in the guest lists what is
  installed, and other RISC OS package tools see these as ordinary installed packages.

### Sharing the database with PackMan

That location is not a choice. It is where **PackMan** and **RiscPkg** keep the database
and where their `!Boot` publishes it as `Packages$Dir`, so a machine can already have one
before RPCEmu ever looks, with packages in it that RPCEmu did not install. HardDisc4
ships PackMan, so most machines do.

The rule is therefore to **join that database rather than keep a second one beside it**.
Two databases on one disc means each manager overwrites files the other believes it owns,
and neither can uninstall what the other put down.

What that means in practice:

| On the disc | What happens |
| --- | --- |
| `!Boot.Resources.!Packages` already there | it is used as it is, PackMan's records and all |
| a `!Boot.Resources` but no `!Packages` | a complete one is created there |
| no `!Boot` at all | `$.!Packages`, as before, because nothing else is looking |

Only `Status`, `Info` and `Version` are shared. `Available`, `Lists` and `Cache` are
PackMan's working files for its own catalogue and are never touched; RPCEmu's catalogue
cache is entirely host-side.

A database whose `Version` file says something other than `2` is **left completely
alone** and the legacy location used instead. Appending to a format from the future is
how a working PackMan installation gets corrupted.

### Migrating an existing database

Earlier versions kept the database at `$.!Packages`, which no RISC OS tool looks in. A
machine with one there gets it merged into the proper location the next time the package
manager is opened: each package's `Info` directory is copied across and its `Status` line
appended.

A package recorded in **both** is left as the shared database has it. Its record there
was written by whichever manager actually installed the files, and ours would be a guess
at a version and a file list we did not put down.

The old directory is **renamed to `!Packages-migrated`, not deleted**. If the merge is
wrong, the evidence of what was in it is the only way to find out how.

### A proper !Packages, not just a directory

When RPCEmu creates the database it lays down the whole thing from a template in
`resources/ro5/packages`: the `!Boot` that publishes `Packages$Dir`, a `!Run`, the
standard `Paths`, and icons for all three sprite resolutions. Without `!Boot` it would be
a database no RISC OS tool could find, which is the entire point of putting it there.

The icons are generated by `tools/riscos/mksprites.py`, which draws them and writes real
RISC OS sprite files. That exists so the icons have a source: sprite files committed as
binaries cannot be altered without starting again. Run it and commit the result; nothing
in the build needs Python.

### Logical paths

A package's manifest is written against **logical** paths, not directories. `System` means
wherever this disc keeps `!System`, which is `!Boot.Resources.!System` and not `$.System`.
The `Paths` file in the database says where each logical name is, and installation
resolves through it, so a package's system modules land where the OS loads them from.

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
./rpcemu-recompiler --pkg-sources                 # the repositories, and where the list lives
./rpcemu-recompiler --pkg-list                    # the whole catalogue
./rpcemu-recompiler --pkg-list=games              # matching name, section or description
./rpcemu-recompiler --pkg-info=ChangeFSI          # everything about one package
./rpcemu-recompiler --pkg-install=Flasher --pkg-machine=Default
./rpcemu-recompiler --pkg-remove=Flasher  --pkg-machine=Default
```

`--pkg-machine` is required for installing and removing: there is no ambiguity about which
machine's disc is being written to.

`--pkg-sources` is the only one of these that touches no network, so it answers "why is
that repository not showing up?" straight away rather than after four indexes have been
fetched. It reports anything wrong with the file as it goes. Adding and removing sources
without a display means editing `pkgsources` directly, which is what the format is for.

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
