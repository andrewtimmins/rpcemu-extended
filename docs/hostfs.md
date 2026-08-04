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

## Known gaps

- The sharing warning is given when you **choose** a folder, not when two
  machines are actually running on it. Detecting that at runtime wants a lock
  outside the folder itself, since anything written inside it would appear to the
  guest as a file.
- Nothing migrates an existing `hostfs` folder when you change the setting. The
  new folder is created empty and the old one is left alone.
