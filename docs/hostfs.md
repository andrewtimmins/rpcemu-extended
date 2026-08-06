# HostFS

The drive RISC OS sees as a host directory, where it lives, and how to point
several machines at one folder.

## The two drives

| Drive | Host directory | Shared between machines |
| --- | --- | --- |
| **HostFS** | `<machine directory>/hostfs` by default | No, unless you say so |
| **Shared** | `<data directory>/shared` | Yes, always |

The Shared drive has always been common to every machine and needs no
configuration. If all you want is a folder every machine can see, use that.

## Choosing where HostFS is

*Settings → Machine → System → HostFS folder*, or the `hostfs_path` key in the
machine's `.cfg`.

Discussion [#77](https://github.com/andrewtimmins/rpcemu-extended/discussions/77)
asked for this so that several machine configurations can share one folder, which
is much easier when testing the same software across different machines.

Three forms, the same convention `hd4_path` and `rom_dir` already use:

| Setting | Means |
| --- | --- |
| *empty* | `<machine directory>/hostfs`. What every machine had before this existed. |
| `discs/work` | Relative, resolved under the machine's own directory. |
| `/srv/riscos/shared` | Absolute, used as given. This is how machines share a folder. |

**Empty stores nothing**, which is the point. A configuration that records only
the default has nothing in it to go stale, so moving your data folder does not
break it. A relative path moves with the machine for the same reason. An absolute
path is the only form that can end up pointing at somewhere that no longer
exists, and it is the only form you have to type deliberately.

That is not a hypothetical worry. `debug_socket` stored a *resolved absolute*
path, so moving a data folder left it naming a directory that had gone and the
debug socket could not bind. HostFS failing that way would be worse, since it is
the drive the guest boots from.

The folder is created if it does not exist, because refusing would leave the
machine with no HostFS at all. A mistyped path is therefore caught where it is
typed: the machine editor says what the setting resolves to, and whether that
folder exists yet.

## Sharing one folder between machines

Supported, and the reason the setting exists. Point two machines at the same
absolute path and both see the same files.

**Do not run both at the same time.** HostFS holds open file handles and RISC OS
caches directory contents, so two guests writing the same tree together can lose
work. Nothing stops you, but the machine editor says which other machine is
already using a folder when you choose it.

Sharing `!Boot` between machines deserves a thought too: it is shared
configuration as well as shared files, so a change one machine makes to its boot
sequence applies to the other.

## Diagnosing it

`rpclog.txt` names the root whenever it is not the default:

```
HostFS: root is '/srv/riscos/shared' (from hostfs_path '/srv/riscos/shared')
```

If a machine comes up with no HostFS at all, the usual cause is nothing to do
with this setting: `poduleroms/` holds `hostfs,ffa`, and that is loaded from the
**resource** directory. A machine with the resource directory pointed somewhere
without a payload has no HostFS module to register. See
[docs/paths.md](paths.md).

## Testing

`tests/test_hostfs_path.c`, 37 checks, on every platform. The resolution is a
pure function so the Windows drive-letter and UNC forms are all checked from
whichever platform runs the suite:

- Empty, relative and absolute each resolve as documented.
- The same relative setting under a different data folder gives a different,
  correct answer, which is the property that makes a move safe.
- Separator style is normalised, repeated separators collapse, and a trailing one
  is dropped, but a UNC prefix and a drive root keep theirs.
- A path too long to fit is refused rather than truncated. A truncated path names
  a *different* directory, and since the root is created at startup that would
  put the guest's files somewhere nobody chose.
- Two machines pointed at one folder are recognised however the paths are spelled,
  and two machines on their own defaults are not.

Writing those found a real bug: a machine directory ending in `//` produced a
doubled separator, which would also have made two identical shared folders compare
as different and suppressed the warning.

## An empty folder is an empty hard disc

Pointing HostFS at a new folder gives the machine a **blank disc**. That is the
setting working, not failing, but it does not look like it, because RISC OS does
not say so. What you get depends on the version:

| RISC OS | What an empty boot drive looks like |
| --- | --- |
| 5.31 | A bare grey screen with a mouse pointer. No icon bar, no banner, nothing to click. |
| 5.30 | The supervisor prompt, with `Error: Use *Desktop to start TaskManager`. |

Neither mentions a folder, so neither tells you what you just did. Nothing is
lost: the old folder is untouched, and pointing the setting back at it brings the
machine straight back.

Both screens were reproduced deliberately, on Linux and on the Windows build under
Wine, which is how the machine editor came to warn about it before you save rather
than after you reboot. The warning is decided in `src/hostfs_advice.c` and checked
by `tests/test_hostfs_advice.c` — separated from the dialog because the decision
had been wrong: the warning lived in the branch for a folder that already existed,
so a folder about to be **created**, which is necessarily empty, was the one case
that never got it. Reported by David Ramsden.

## Editing a config by hand on Windows

Use the machine editor if you can, and **write forward slashes if you edit the
file by hand**. `wxFileConfig` escapes backslashes when it writes a value and
unescapes them when it reads one, so a file the emulator wrote round-trips
correctly and a hand-edited one does not. What you get back depends on the letter
after the backslash:

| Typed by hand | Read back as | |
| --- | --- | --- |
| `C:\temp` | `C:<tab>emp` | refused, falls back to the machine's own folder with a line in the log |
| `C:\new` | `C:<newline>ew` | refused |
| `C:\run` | `C:<CR>un` | refused |
| `C:\foo` | `C:oo` | **used as given** — see below |
| `C:\bar` | `C:ar` | used as given |
| `C:\\foo` | `C:\foo` | correct; this is what the editor writes |
| `C:/foo` | `C:/foo` | correct, and needs no escaping at all |

The first three become control characters, which name directories Windows will not
create: HostFS then served a folder that was not there and the guest's disc
appeared empty, with nothing to connect it to the file that had been edited. Those
are now refused.

The next two cannot be caught. An escape wx does not recognise loses **both**
characters, and `C:oo` is a perfectly well-formed drive-relative path — nothing
can tell it from one somebody meant to type. So it is used, and the machine
quietly runs from the wrong folder. This is the reason for the advice at the top of
this section rather than a bug that can be fixed.

Measured by reading a hand-written file with `wxFileConfig`, not assumed; the
results are pinned in `tests/test_hostfs_path.c`.

## Taking your files with you

Point HostFS somewhere new and RPCEmu offers to bring the files across:

    Your files are in:
        /home/you/RPCEmu/machines/Box/hostfs
    The new folder is:
        /mnt/data/riscos

    There is 412 MB to bring across.

    Move - the files are copied and checked, and only then removed from the old
    folder.
    Copy - the old folder is left exactly as it is, so it doubles as a backup.
    Needs room for both.

    Either way, please make sure you have a backup first: this is your hard disc.
    Nothing is deleted until every file has been copied and checked, but an
    automatic move is not a substitute for a backup.

      [ Move my files ]  [ Copy my files ]  [ I'll do it myself ]  [ Cancel ]

"I'll do it myself" is the old behaviour: the setting changes, the files stay put,
and the warning above about an empty disc applies until you move them.

A progress window names each file with a running count and a remaining time, and
its Cancel leaves everything as it was.

### What makes it safe

- **The setting is only changed after the transfer has succeeded.** If anything
  fails part way, both copies are still on disc and the machine still boots from the
  folder it was booting from before. A failed move is then an inconvenience rather
  than a lost hard disc, which matters more than any wording, because people click
  through warnings.
- **Nothing is deleted until every file has been copied *and verified*** — MD5 on
  both sides, not a size comparison. A copy truncated by a disc filling up has the
  wrong content and the right length often enough to matter.
- **A move within one filesystem is a rename**, so it is instant whatever the size
  and cannot half-happen.
- **A destination with files in it is refused, never merged.** Deciding which copy
  of a clashing file wins is not a choice RPCEmu should make silently. Checked
  twice: once when deciding what to offer, and again inside the transfer itself,
  because the cleanup-on-failure step deletes everything at the destination and is
  only safe if everything there is ours.
- **It is refused outright while a machine is running from either folder.** HostFS
  opens files as the guest asks for them, so moving them under a live machine would
  damage whatever it read next.
- **Free space is checked first**, with a margin, since a copy that exactly fills
  the disc leaves nothing for the machine to write afterwards.

The decision is `src/folder_move.c` (pure, `tests/test_folder_move.c`); the doing
is `src/gui/folder_transfer.cpp` (`tests/test_folder_transfer.cpp`, which runs the
real copy, verify and delete on real files, including a transfer that fails half
way).

The same offer is made for the **data folder**, from Options > Data Folder in the
machine selector, where it moves the machines, ROMs and settings together. That one
takes effect on restart, and closes the log file first because it lives in the
folder being moved and Windows will not move an open file.

## Moving the data folder

The *relative* form travels: a machine with `hostfs_path=disc` resolves under
wherever its machine directory now is, and boots exactly as before. Verified by
moving a whole data folder and booting from the new location.

An *absolute* path does not, and the way it fails is worth knowing. HostFS creates
its root **and every missing level above it**, so a machine whose absolute path
pointed inside the data folder that moved does not report an error: the old tree
is recreated, empty, and the machine boots from a blank disc — the grey screen
above. The resurrected tree also makes the old location look like it is still in
use.

That is reachable without moving anything by hand, because the one-time migration
of `~/.local/share/rpcemu` to `~/RPCEmu` renames the tree without rewriting
configurations.

The log now says so when the root is absent **and its parent is absent too**,
which is what distinguishes a stale path from somebody asking for a new folder on
a drive that exists:

    HostFS: '/old/machines/Box/hostfs' does not exist and neither does
    '/old/machines/Box'. If this machine's data folder has moved, this is a stale
    absolute hostfs_path: the folder will be created empty and the guest will see
    a blank disc. Its own folder is '/new/machines/.../hostfs'.

**Whether it should instead refuse and fall back** to the machine's own folder is
an open question, not an oversight. In the moved-folder case falling back would be
right — the files are there, under the new location — but it would also override
somebody who deliberately named a folder on a drive that is not mounted yet, and
that is a behaviour change rather than a bug fix. Prefer a relative path if you
expect to move things.

## Known gaps

- The sharing warning is given when you **choose** a folder, not when two
  machines are actually running on it. Detecting that at runtime wants a lock
  outside the folder itself, since anything written inside it would appear to the
  guest as a file.
- Nothing migrates an existing `hostfs` folder when you change the setting. The
  new folder is created empty and the old one is left alone. The editor now warns
  that it will be empty, but offering to copy or move the old contents would be
  friendlier still.
