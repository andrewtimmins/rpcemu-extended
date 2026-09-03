# Testing

RPCEmu Extended Edition is built for Linux, Windows and macOS from one source tree,
and the three are tested the same way: the same unit tests, the same
command-line checks and the same real boot, run by the same scripts on each
platform's CI job. Where a platform genuinely cannot run something, it is said
out loud rather than skipped quietly.

## Running the tests

The build scripts run the unit tests themselves, so an ordinary build is also a
test run:

```
./build.sh              # Linux
./build-windows.sh      # Windows, under MSYS2 MINGW64
./build-macos.sh        # macOS
```

To run them against a build directory you already have:

```
bash tests/run-ctest.sh build
```

That is what the build scripts and CI call, rather than `ctest` directly, and it
is stricter: it fails if the tests were never configured, or if the number of
tests in the build directory is not the number `tests/CMakeLists.txt` says there
should be. A suite that has quietly lost half its tests otherwise looks exactly
like one that passed.

Individual tests are ordinary programs and can be run on their own, which is the
quickest way to read a failure:

```
./build/tests/test_mmu_perms
./build/tests/test_jit_fuzz 12345 5000000    # seed, iterations
```

Set `RPCEMU_TEST_LOG=1` to see what a unit under test is logging.

## Driving the window without a pointer

Some faults are only in the front end, and a wxWidgets control cannot be read or
clicked by a script on every platform - on macOS a checkbox is invisible to
accessibility. Three environment variables ask the window to do the thing
itself and write what happened to the machine's `rpclog.txt`, so a GUI fault can
be measured rather than only described. Each takes a delay in seconds from the
machine starting:

| Variable | What it does |
| --- | --- |
| `RPCEMU_TEST_CLOSE_AFTER` | Asks the window to close, exactly as the close button does, so the confirmation is put to the user rather than answered for them |
| `RPCEMU_TEST_FULLSCREEN_AFTER` | Enters full screen and leaves it again, logging the window size and the state of the menu, tool and status bars at each step (issues #173, #206) |
| `RPCEMU_TEST_INSPECTOR_AFTER` | Sets the machine trapping and tracing, opens the Machine Inspector, closes it, opens it again, and logs whether the Trace tab's controls agree with the machine each time (issue #221) |
| `RPCEMU_TEST_PODULES_AFTER` | Builds the Edit Machine dialog without showing it and logs every podule slot - what it says, whether it can be changed, and what is selected - as configured, then with the graphics card switched on and off again, so the slots the machine fits itself can be watched moving (issue #254) |
| `RPCEMU_TEST_DISPLAY_TIMING` | Not a delay: set it to 1 and every guest mode change logs where its time went - what the guest asked for, how long the window waited before following, each step of the resize, and whether the GPU or the CPU is drawing (issue #220) |

They need a real display, so none of them work headless. Grep the machine's log
for `TEST_` or `DISPLAY_TIMING` afterwards; the machine directory's `rpclog.txt`
is the one with these lines in it, not the data directory's.

`RPCEMU_TEST_DISPLAY_TIMING` is the one to reach for when somebody reports the
display as slow, because it separates the three things that "slow" can mean:
the guest taking a while to adopt the mode, the window waiting before it
follows, and the resize itself. Measured on this Mac with RISC OS 4.39, those
are about 300ms, 80ms and 2ms - so a report of anything much larger says which
of the three to look at.

## Before pushing

A pre-push hook builds the tree and runs the suite, so a broken commit is caught
on your own machine rather than ten minutes later in CI. Enable it once per
clone:

```
git config core.hooksPath .githooks
```

It builds in `build-prepush/` so it cannot disturb whatever `build/` is
configured for, and it is incremental after the first run. Push with
`--no-verify`, or set `RPCEMU_SKIP_PRE_PUSH=1`, when you have a reason.

## Testing the Windows build on Linux, with Wine

`./build-windows.sh` now runs the whole suite against the cross-built `.exe`
files under Wine, so the Windows build can be checked before pushing instead of
when CI gets to it. Nothing to pass: it looks for `wine64`, then `wine`, then
`/usr/lib/wine/wine64`, and runs the tests if it finds one.

```
apt install wine64          # the loader; the `wine` wrapper is a separate package
./build-windows.sh
==> Running the Windows test suite under Wine (/usr/lib/wine/wine64)
==> [wine] all 34 tests passed
```

Set `RPCEMU_SKIP_WINE_TESTS=1` to opt out.

Two things are worth knowing:

- **`WINEPATH` is set for the run**, pointing at the mingw sysroot. The test
  executables link the same DLLs the emulator does, and at that point nothing has
  been staged beside them. Without it every test exits **53** — a Windows "DLL not
  found", with no message at all, which reads exactly like a crash.
- **Wine is not Windows.** It is enough to catch what it catches; the
  `windows-amd64` CI job on a real MSYS2 toolchain is still the authority.

The emulator itself runs under Wine too, which is how `tests/cli_smoke.sh` can be
pointed at the Windows binary, and how the RISC OS 5.31 empty-HostFS screen in
[hostfs.md](hostfs.md) was reproduced on a Linux desktop.

## What the suite covers

Fifty-two tests: forty-three that build anywhere, eight that need a native
recompiler backend, and one that needs a Python 3 interpreter. (The count in
`tests/CMakeLists.txt` is the one that is enforced; this sentence is prose and
had fallen behind it, which is worth knowing before trusting it.)

| Test | Covers |
| --- | --- |
| `test_jit_flags` | data-processing NZCV flags, recompiler against interpreter |
| `test_jit_mem` | LDR/STR/LDRB/STRB addressing modes |
| `test_jit_branch` | B and BL |
| `test_jit_mul` | MUL, MLA, UMULL |
| `test_jit_ldmstm` | LDM and STM |
| `test_jit_e2e` | block linking and whole programs |
| `test_jit_fuzz` | random instructions, recompiler against interpreter |
| `test_jit_seqfuzz` | random programs through `arm_exec`, likewise |
| `test_mmu_perms` | ARM DDI 0100B page permissions, including after a translation is cached |
| `test_arm_disasm` | ARM and FPA10 disassembly, and the instruction description step-over is built on |
| `test_debugger_gate` | the debugger's per-instruction fast gate is raised and lowered on every route |
| `test_debugexpr` | breakpoint condition expressions: precedence, and refusing malformed ones |
| `test_backtrace` | walking the APCS frame chain, 26-bit masking, and chains that are not chains |
| `test_stepping` | step over / out / run to, and that the temporary target never outlives them |
| `test_debugsym` | guest symbols, and what an address outside the table must *not* be called |
| `test_debugcmd` | the debugger wire protocol, over a real socket |
| `test_abort_watch` | how CI decides an abort during a boot is a problem |
| `test_savestate` | snapshot serialisation, the run-length codec and the CRC-32 |
| `test_machine_lock` | the lock that stops one machine being run twice |
| `test_net_slot` | the per-instance slot that gives each guest its own address |
| `test_relay_datagram` | the Access relay reads a whole datagram and answers from the sender's address |
| `test_md5` | MD5 against RFC 1321, for package checksums |
| `test_unzip` | the zip reader used for downloaded archives |
| `test_ip_reass` | SLiRP IP fragment reassembly, including two backported fixes |
| `test_relay_reasm` | relay-side reassembly of guest Access+ datagrams |
| `test_gfxcard` | the graphics card's registers and framestore bounds |
| `test_gfxrom` | the card's driver module, read as RISC OS reads it |
| `test_ohci_iso` | OHCI isochronous transfer descriptors |
| `test_clipboard` | shared clipboard text conversion |
| `test_mode_fit` | display mode selection against a VRAM budget |
| `test_keymap` | host keys to RISC OS keys, for **all three platforms from any one of them** |
| `test_openbus` | the second processor bus: window, bus mastering, nPIRQ, reset, timeslice |
| `test_openbus_decode` | that the machine's own memory decode reaches a fitted card, which an unhooked window would hide |
| `test_hostfs_path` | where a machine's HostFS drive resolves to, on all three platforms |
| `test_hostfs_advice` | what the machine editor warns about when a HostFS folder is chosen |
| `test_folder_move` | whether moving somebody's files can be offered: empty destination, free space, in use |
| `test_folder_transfer` | the transfer itself on real files, including one that fails half way through |
| `test_data_dir` | which data directory is used, and above all when the user is asked |
| `test_held_keys` | which keys the guest believes are held, when several map to one |
| `test_machine_selector` | the VNC machine selector's navigation and drawing |
| `test_app_settings` | emulator settings, and the migration from per-machine ones |
| `test_vdu_filter` | turning guest VDU output into host text |
| `test_etherrpcem_layout` | the DCI4 structure offsets the guest network driver hardcodes, read back out of its assembler source |

Beyond the unit tests, every CI job runs `tests/cli_smoke.sh` against the staged
binary and then `tests/boot_smoke.py`, which boots a real machine with a real ROM
and checks over VNC that RISC OS drew something, asks the guest its version over
HostCmd, and checks its clock. That is the test that covers the CPU, memory,
VIDC, ROM loading and the VNC server together.

That includes `windows-arm64`, which was briefly the exception and is not any
more: leaving the boot test off a brand new platform had it backwards, because a
new compiler and a new architecture are exactly where a CPU core, VIDC or a
libvncserver build is most likely to be wrong, and `linux-arm64` had already
shown that an interpreter build can pass it in CI. It gets a longer timeout there
for the same reason that one does - the interpreter rather than the recompiler.

What that job establishes, in order: RPCEmu compiles natively (`file` reports
`PE32+ ... ARM64`, asserted rather than admired), the staged release starts with
every runtime DLL it needs - clang's `libc++` and `libunwind` rather than GCC's -
the ROM downloader works over WinHTTP on ARM64, and RISC OS boots and draws.
What it cannot establish is somebody using the thing: sound, USB, a window on a
real ARM laptop. That is why its build is an artifact rather than a release asset.
See [windows-build.md](windows-build.md).

## The guest modules

The RISC OS modules under `riscos-progs/` run inside the emulated machine: HostFS
and its filer, RPCEmuSupport, SyncClock, the USB and PCI support modules, the
graphics card's display driver and the network driver. They are assembled with the
ARM binutils, and the assembled images are committed — under `poduleroms/`,
`gfxroms/` and `netroms/` — because somebody building RPCEmu is not asked to
install a cross-assembler.

That invites one quiet failure: edit a module's source, forget to rebuild it, and
the emulator goes on loading the stale image. Nothing fails, and the source stops
describing what the machine runs.

```bash
./setup-build-env.sh --podules        # arm-linux-gnueabi binutils
bash tests/check-guest-modules.sh     # rebuild all eight images and compare
```

A difference is an error naming the file. `--rebuild-only` copies the rebuilds
over the committed images instead, for when the source has deliberately moved on.

The `linux-amd64` job runs this **before** `build.sh`, because `build.sh` rebuilds
these itself when the tools are present and copies them over the committed images
— comparing afterwards would compare a file with itself. Only that job runs it:
the modules are ARM binaries and come out the same whatever host builds them.

Two things this does not cover. `ScrollWheel` is not in the list, because its
makefile wants clang and `build.sh` does not build it either; its image is
committed as it stands. And `SharedClipboard` is C built with the Acorn DDE inside
a guest, so it cannot be rebuilt on the host at all — see
`riscos-progs/SharedClipboard/README.md`. EtherRPCEm used to be in that position
and is not any more: it is assembler now, so it is in the list above, and the C
it was ported from has been removed from the tree rather than left sitting beside
a source nothing checked it against (it is in git history at 9e44930).

## Writing a test

The house pattern is a plain C program that prints a line per check and exits
non-zero if any failed - look at `tests/test_app_settings.c`. There is no test
framework and none is wanted; the output is meant to be readable in a CI log by
somebody who did not write the test.

Two rules that matter more than they look:

**Compile the unit under test directly rather than linking `rpcemu_core`, where
you can.** The core drags in SDL and, through the front end, wxWidgets - and
arm64 Homebrew has no separate SDL2main, so linking the core for a test that
needs none of it has broken the macOS build before now. Most units need only
`rpclog()`, which `tests/test_log_stub.c` supplies. Link the core when the unit
genuinely reaches the rest of the emulator, as `test_savestate` and
`test_mmu_perms` do.

**Test through the code's own entry points, not a copy of them.** The first
draft of `test_net_slot` reimplemented the bind that `net_slot.c` performs, and
so could not see the exclusivity being broken - a deliberately sabotaged
`claim_slot()` passed the test, because the copy in the test was still correct.
Going through `claim_slot()` itself caught it immediately.

Then raise `RPCEMU_EXPECTED_TESTS` in `tests/CMakeLists.txt`. Configuring will
fail until you do, which is the point: the number can only be changed
deliberately, so a test cannot go missing by accident.

### Does the test work?

A new test that passes has proved nothing yet - it has to be shown to fail. Break
the code it covers, deliberately and temporarily, and check it goes red. Every
test added in this round was checked that way, and two of them were found to be
blind and rewritten.

## Sanitisers

A CI job builds with AddressSanitizer and UndefinedBehaviorSanitizer, runs the
unit tests and then boots a machine. To reproduce it:

```
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRPCEMU_DYNAREC=OFF -DRPCEMU_ENABLE_GHOSTPDL=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=0 bash tests/run-ctest.sh build-san
```

The recompiler is off: AddressSanitizer instruments code the compiler generated,
and the recompiler writes its own at run time into a page it maps itself, which
the instrumentation cannot see and which looks like a stranger scribbling on
executable memory. The interpreter runs the same CPU semantics through code the
sanitisers can read. The recompiler's answer to this is the eight differential
tests, which check it against the interpreter instruction by instruction.

Leak detection is off. The emulator holds its guest RAM, ROM and framestore for
the life of the process and frees none of it on the way out, so LeakSanitizer
reports hundreds of megabytes that are not leaks in any useful sense. Turning it
on wants those exit paths tidied first.

A boot is where this earns its keep. The unit tests reach a codec, a parser and a
register file; a boot runs a real operating system through the CPU core, the MMU,
IOMD, VIDC, the disc controllers and the video path. The first time one was run
under the sanitisers it found two genuine cases in the first few seconds: a shift
by 32 on every word-aligned load, and a signed overflow on every palette update.
Both had produced the intended answer on every compiler this project has been
built with, and no test could have caught either.

```
python3 tests/boot_smoke.py --binary build-san/bin/rpcemu-interpreter \
  --datadir <a data directory with a ROM> --screen-check advisory
```

`--screen-check advisory` because an instrumented interpreter is tens of times
slower than the recompiler a user runs, so how far RISC OS gets in a fixed time
measures the runner rather than the code. The verdict there is what the
sanitisers said. Whether the desktop is reached is judged by the four ordinary
boot tests, on real builds, on every platform.

Anything the sanitisers report fails `boot_smoke.py` on every platform and every
build, not only in that job - an ordinary build cannot produce those lines, so
the check costs nothing and means a report can never be scrolled past.

## Warnings

`-Wall -Wextra` is on everywhere, and CI fails if code this project maintains
compiles with a warning:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build 2>&1 | tee build.log
bash tests/check-warnings.sh build.log
```

Both parts of that matter. It has to be a **full** build, because an incremental
one only recompiles what changed and will happily report a clean tree that is
nothing of the sort. And it has to be a **Release** build, because which warnings
exist depends on the optimisation level: `-Wformat-truncation` and
`-Wmaybe-uninitialized` come out of analysis the optimiser performs, so `-O0`
gives a different set rather than a smaller one. This tree is clean at `-O2` and
shows three format-truncation warnings at `-O0` - two in `debugcmd.c`, one in
`machine_selector.c`, all safe truncations of a JSON error string or a display
line. Release is the calibration because it is what CI and the pre-push hook
build.

There is deliberately no `-Werror` in the build system. `-Werror` turns a new
compiler release into a tree that will not build at all, on somebody else's
machine, over a warning that has nothing to do with them; that is a poor trade
for a project people build from source on three platforms. Being strict in CI
costs a maintainer ten minutes instead.

`tests/warnings-exempt.txt` lists the files allowed to warn, each with its
reason - inherited SLiRP, the Arculator-derived podule code, and a handful of
upstream RPCEmu files. It is a debt register and it is meant to get shorter.

## Aborts during the boot test

`tests/boot_smoke.py` boots a real machine in CI, and as well as checking the
screen and asking the guest its version, it watches the emulated CPU for data
and prefetch aborts through the whole run. This is `tests/abort_watch.py`,
driving the [DebugCmd socket](debugcmd.md).

It exists because an abort is invisible to every other check. RISC OS handles
the abort and carries on, so an MMU regression, a bad memory decode or a
recompiled block branching somewhere it should not still boots to a perfect
desktop and still answers `*FX 0`. The screenshot looks right.

**A healthy boot takes aborts on purpose.** RISC OS sizes memory and probes for
hardware by reading addresses that may not answer, and an abort is how it finds
out. A check that failed on "an abort happened" would fail on every green build
and be switched off within a week. So aborts are split by when they happened:

| Phase | Meaning | Verdict |
| --- | --- | --- |
| **boot** — up to the moment RISC OS answers over HostCmd | probing for hardware | reported, does not fail |
| **running** — after that, with the machine idle | nothing should be faulting | **fails the build** |

That line is drawn from the guest's own behaviour rather than from a clock, so
it does not drift with how fast the runner is.

**Undefined instructions never fail.** They are routine here: the FPA10 in
`fpa.c` implements the common operations and raises an undefined-instruction
exception for the rest, exactly as the real one did, so RISC OS's floating point
support code can emulate them. A machine with working floating point produces a
stream of them. The count is reported and nothing more.

Only logging is turned on, never trapping — a trap would engage the debugger's
per-instruction path and drop the recompiler to interpretation, and a CI boot
would take far longer than the job allows.

### Baselines

Boot-phase aborts are accepted by default. To be stricter, record the sites a
known-good boot produces and require the set not to grow:

```sh
python3 tests/boot_smoke.py --binary … --write-abort-baseline boot-aborts.txt
# look at what it wrote, then commit it
python3 tests/boot_smoke.py --binary … --abort-baseline boot-aborts.txt
```

A site in the baseline that does not occur is not a failure — probing depends on
what hardware is configured, and demanding every known abort happen would fail
on a machine with less of it.

No baseline is committed. One is only meaningful against a pinned ROM, and the
sites would have to be looked at by somebody who can say they are the expected
probing rather than recorded from whatever the first run did.

## Slack notifications

The `notify` job posts to a Slack channel through an incoming webhook, held in
the repository secret `SLACK_WEBHOOK_URL`:

| When | Message |
| --- | --- |
| A push to `main` fails | Which jobs failed, the commit, who pushed it, a link to the run |
| A push to `main` passes after a failure | That it is green again |
| A push to `main` passes otherwise | That it passed, with the commit |
| A tag builds and publishes a release | The release page and the build log |

Every completed push is reported, so the channel shows CI is running rather
than only that it once broke. A recovery still reads differently from an
ordinary green run, coming back from red being the one green result somebody
is actually waiting for.

Cancelled runs say nothing. Cancelled is neither a pass nor a failure and is
treated as its own state: the concurrency group cancels superseded runs as a
matter of course, and calling one of those green would announce a build for a
commit nobody waited on that never finished.

Three details worth knowing before changing it:

- **Every job that can fail has to be in `notify`'s `needs`.** The verdict is
  worked out from `toJSON(needs)`, so a job left out is one whose failures
  nobody hears about. Naming an aggregate is not enough to cover the jobs behind
  it: when `macos-x86_64` fails, `macos-universal` is *skipped* rather than
  failed, and a skip is not a failure, so a list naming only the aggregate
  announces a green build for a run that is red. The gates themselves
  (`build-gate`, `linux`, `windows`) are left out on purpose, because they would
  report which tier stopped in place of the job that broke.
- **Pull requests are deliberately excluded.** A pull request from a fork cannot
  read repository secrets, so the step would do nothing on exactly the runs an
  outside contributor wants feedback on, while looking configured.
- **Nothing here can fail the build.** An unset secret is treated as "not
  configured" and a non-200 from Slack is logged and passed over. A workflow
  that goes red because Slack was unreachable is reporting on itself.

To point it at a different channel, create a new incoming webhook and replace
the secret; no change to the workflow is needed:

```sh
gh secret set SLACK_WEBHOOK_URL --repo <owner>/<repo> < webhook.txt
```

## Known gaps

Said here rather than left to be discovered:

- **The sanitiser job is Linux only.** It is the same C on all three platforms,
  so a fault it finds is a fault everywhere, but the Win32 and Cocoa paths are
  not covered by it.
- **The arm64 builds run sixteen of the twenty-four tests.** They ship the
  interpreter, so there is no recompiler for the eight differential tests to
  compare against. This is expected, and the configure step says so.
- **A 32-bit x86 build has a recompiler backend (`codegen_x86.c`) but no
  differential tests**: `i386`/`i686` is excluded from the condition that
  compiles the JIT test hooks. No release is built for it.
- **The GUI is not unit tested** beyond the machine selector's drawing and
  navigation. wxWidgets dialogues are exercised only by hand.
- **Windows and macOS have no equivalent of the pre-push hook's warning check**,
  since it runs only where the hook does.
- **The boot abort check has no baseline committed**, so it only fails on aborts
  after the machine is up. That is the rule worth having and it needs no
  maintenance, but a boot-phase regression that adds a new probing site will
  pass until somebody records one. See *Aborts during the boot test*.
