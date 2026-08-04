# Where RPCEmu keeps things

Two directories, the rules that pick them, and why the first-run question appears
as rarely as it does.

## The two directories

| | What is in it | Writable |
| --- | --- | --- |
| **Data directory** | Your machines, configs, ROMs, HostFS folders, CMOS, logs, packages | Yes |
| **Resource directory** | Read-only templates the data directory is seeded from | No, usually |

For a portable or in-tree copy they are the same folder. For an installed copy the
resources are under the install prefix and the data is yours.

## Choosing the data directory

Issue [#67](https://github.com/andrewtimmins/rpcemu-extended/issues/67): RPCEmu
created `~/RPCEmu` without saying so and gave no way to move it. On macOS the home
directory is also the wrong place by convention, where this sort of data belongs
under `~/Library/Application Support`.

So RPCEmu asks, once, on a genuine first run. The field is prefilled with the
location it would have used anyway, so pressing Return behaves exactly as every
earlier version did.

**The answer goes in the platform's own preference store**, not in `rpcemu.cfg`,
for the obvious reason that `rpcemu.cfg` lives inside the directory being chosen.
That is the registry on Windows, `~/Library/Preferences` on macOS and a dotfile on
Linux, reached through `wxConfig`.

### Precedence, strongest first

| Order | Source | Remembered? |
| --- | --- | --- |
| 1 | `--datadir` on the command line | No |
| 2 | `RPCEMU_DATADIR` | No |
| 3 | The folder chosen on first run, or via *Settings → Data Folder…* | Already stored |
| 4 | `configs/` beside the binary, or in the current directory | No |
| 5 | A macOS `.app` bundle: payload inside, data outside | No |
| 6 | `configs/` in the install prefix or `/usr/share/rpcemu` | No |
| 7 | `RPCEMU_RESOURCE_DIR` (settles the payload only) | No |
| 8 | An existing default location that is already set up | Yes, once |
| 9 | Nothing to go on, and a GUI to ask with: **ask** | Yes |
| 10 | Nothing to go on and no GUI: the default, silently | No |

Two of those deserve their reasons stated.

**The environment beats the remembered choice** (2 above 3). The other way round
would let a CI job or a packaging script be quietly redirected by whatever
somebody once clicked in a dialogue, and a reproducible run is worth more than
honouring a preference in the one case nobody is watching.

**Explicit inputs are not remembered** (1, 2). They are supplied afresh every run,
so recording them would let one scripted run with an unusual `--datadir` silently
become the default for every later interactive one. A portable layout is not
remembered either: the whole point is that it follows the binary, and pinning the
first one seen would break the next copy unpacked somewhere else.

### Who is never asked

Getting this wrong is worse than the bug being fixed, because every case is a real
person interrupted for no reason. Nothing is asked when:

- **The default location already exists and is set up.** An upgrade that
  interrogates you about data you have had for a year is not an improvement.
- **`configs/` sits beside the binary or in the current directory.** That is a
  deliberate portable or development layout, and already an answer.
- **An installed or bundled copy is running.**
- **`--datadir` or an environment variable was given.**
- **A choice has already been made.**
- **There is no GUI.** Headless, `--list-machines`, `--pkg-*`, `--fetch-riscos`,
  a container. The old behaviour stands, and nothing is remembered either, so the
  first interactive run still gets to ask rather than inheriting a choice nobody
  made.

Exactly one of the 512 possible input combinations produces a dialogue, and
`tests/test_data_dir.c` asserts that.

## Changing it afterwards

*Settings → Data Folder…*, then restart.

**Nothing is copied or moved.** Only the pointer changes. Relocating machines,
discs and ROMs that may be gigabytes and may be open is how data gets lost, so the
offer is the honest one, and the dialogue says what it means before doing it: if
the new folder is empty you will start with no machines and a freshly seeded
folder, the old folder is untouched, and you can point back at any time.

## Both entry points agree

The GUI and the no-GUI paths resolve this through the same
`data_dir_decide()`, in `src/gui/data_dir_choice.c`.

They did not always. `headless_main.cpp` carried its own copy of the precedence
chain, which was harmless while there were only environment variables to consider
and became a real problem as soon as the location could be chosen: `--datadir` was
ignored there, and so was the first-run choice, so one installation resolved two
different ways depending on whether you started it with a window or without one.

The resource directory is now also found **after** the data directory and can fall
back to it. Before, an existing installation with everything in `~/RPCEmu`, or a
perfectly good `--datadir`, was answered with "could not locate RPCEmu data"
whenever the current directory happened not to contain a `configs/` of its own.

## Diagnosing it

`rpclog.txt` records the outcome on every run:

```
Paths: data directory from existing default location: /home/andy/RPCEmu/
```

The name is the source from the table above, so a report about the wrong folder
being used says which rule chose it.

## Testing

`tests/test_data_dir.c`, 41 checks, on every platform. The decision is a pure
function over stated inputs, so the whole table can be walked without a filesystem
or a window, including all 512 input combinations for the invariants that must
hold regardless: no dialogue without a GUI, no dialogue when there was anything to
go on, and the command line and environment never remembered.

## Known gaps

- A stored choice pointing at a folder that has since been deleted or unmounted is
  used as given, and RPCEmu will create and seed it. That is predictable but it
  can look like lost machines. Nothing is deleted, and pointing back recovers it.
- The resource directory has no equivalent question and is still found only by
  looking.
