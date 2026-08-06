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

Thirty-four tests, twenty-six of which build on every platform and eight of
which need a native recompiler backend.

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
| `test_savestate` | snapshot serialisation, the run-length codec and the CRC-32 |
| `test_machine_lock` | the lock that stops one machine being run twice |
| `test_net_slot` | the per-instance slot that gives each guest its own address |
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

Beyond the unit tests, every CI job runs `tests/cli_smoke.sh` against the staged
binary and then `tests/boot_smoke.py`, which boots a real machine with a real ROM
and checks over VNC that RISC OS drew something, asks the guest its version over
HostCmd, and checks its clock. That is the test that covers the CPU, memory,
VIDC, ROM loading and the VNC server together.

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
