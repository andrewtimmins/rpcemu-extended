# Porting RPCEmu Extended to another operating system

What is actually platform-specific, what a new port has to provide, and what it
costs to keep one alive once it exists.

This is written for somebody who wants to do the work. It is deliberately
specific about where the seams are, because the seams are much smaller than the
size of the tree suggests and the expensive part is not the code.

## Start here: how much is there to port

Every platform conditional in `src/`, counted:

| Symbol | Sites |
| --- | --- |
| `_WIN32` | 140 |
| `__APPLE__` | 31 |
| `__linux__` | 8 |
| `__HAIKU__` | 1 |

45 of 381 source files carry a platform conditional at all, and most of those are
Windows. Everything that is not Windows or macOS takes a generic POSIX path, so a
new Unix-like host starts out mostly working rather than mostly not.

The evidence for that is
[#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123), an OpenBSD
port contributed as five commits and about twenty lines, which reached a state
where the emulator built, started, and had working audio, VNC and HostFS. It is
based on `1.x` and is a draft, but it is the honest measure of the starting
distance.

## The eight places that assume Linux

These are the whole of the non-Windows, non-Apple porting surface. Anything else
you meet is a bug, and worth reporting as one.

| Where | What it assumes | Work |
| --- | --- | --- |
| `src/rpcemu.h:88` | `RPCEMU_NETWORKING` is defined only for Linux, Windows and macOS, so on any other host **all networking compiles out silently** | Add your platform |
| `src/arm_dynarec.c:34` | `<unistd.h>` and `<sys/mman.h>` are included only for `__linux__` or `__MACH__`, so the recompiler will not compile elsewhere | Add your platform |
| `src/arm_dynarec.c:710` | `set_memory_executable()` calls `mprotect(PROT_READ\|PROT_WRITE\|PROT_EXEC)` | **See W^X below.** The real work |
| `src/usb_host.c:47`, `:1717` | Host USB pass-through is Linux-only | Optional, degrades |
| `src/gui/emulator_host.cpp:69`, `:986` | The real-drive CD-ROM ioctl backend is Linux-only, and already falls back to ISO images with a message | Optional, degrades |
| `src/hostfs-unix.c:14` | 64-bit `struct timespec` conversion is enabled for LP64 Linux only | Cosmetic. Costs timestamp precision, nothing else |

Plus one gate outside `src/`:

- **`CMakeLists.txt:82`** refuses to configure on anything but Linux, Windows and
  macOS. This is a deliberate statement about what is supported, not a technical
  barrier. A port should replace it with a tier (see
  [Support tiers](#support-tiers)) rather than simply delete it, because a build
  that configures is not the same as a platform anybody has promised to fix.

## The one genuinely hard problem: W^X and the recompiler

This is what decides whether your port ships an emulator or a *slow* emulator,
and it is the reason [#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123)
was built with `-DRPCEMU_DYNAREC=OFF`.

The recompiler writes ARM instructions into a code cache and then executes them.
On Linux and Windows that cache is a static array made readable, writable and
executable in one call, and nothing objects. OpenBSD enforces W^X: a mapping may
be writable or executable and not both, so that call cannot succeed, and a JIT
which asks for it does not run at all. NetBSD's PaX `MPROTECT` can be configured
to do the same. Hardened Linux kernels increasingly behave this way too.

**You do not have to invent the answer, because the tree already contains it.**
macOS refuses an RWX static array under the hardened runtime and under Rosetta,
which was [#30](https://github.com/andrewtimmins/rpcemu-extended/issues/30), and
both code generators were reworked for it:

- `src/codegen_amd64.c:311` and `src/codegen_arm64.c:416` allocate the cache with
  `mmap(MAP_JIT)` rather than declaring a static array.
- `src/codegen_arm64.c:342` documents the discipline that goes with it: a region
  may be written or executed by a thread, but not both at the same moment, so the
  generator flips protection around each block it emits.

A W^X port follows the same shape with `mmap(MAP_ANON)` and a `mprotect` toggle,
or with a double mapping of the same anonymous memory where the platform allows
it. The work is contained in `set_memory_executable()` and the two generators'
allocation paths. It is real engineering, but it is bounded, it is worth having on
its own merits, and it is the single highest-value thing a porter can contribute
because it benefits every hardened host and not only the one being ported to.

Until it is done, `-DRPCEMU_DYNAREC=OFF` gives a correct emulator that is
substantially slower. That is a reasonable first milestone and a bad final one.

## What a port has to provide

### Required

| Dependency | Used for | Notes |
| --- | --- | --- |
| **wxWidgets 3.1.5+**, components `gl core base net` | The entire GUI | 3.1.5 is the floor because of `wxWebRequest`. `cmake/FindWxWidgets.cmake` probes `wxUSE_WEBREQUEST` and explains itself if the toolkit was built without it |
| **SDL2** | Audio, via `src/gui/plt_sound.cpp` | `pkg_check_modules(SDL2 REQUIRED sdl2)` |
| **CMake** and a C11 / C++17 compiler | | GCC and Clang are both used in CI |

### Optional, and degrading cleanly when absent

| Dependency | Absent means | Switch |
| --- | --- | --- |
| **OpenGL** | The guest's screen is rescaled on the CPU instead of being uploaded as a texture. Slower, correct | Not `REQUIRED`; `gl_display_canvas.cpp` compiles away behind `wxUSE_GLCANVAS` |
| **libvncserver** | No VNC server, so no headless access | `RPCEMU_ENABLE_VNC` |
| **libusb-1.0** | No host USB pass-through | `RPCEMU_REQUIRE_LIBUSB` is `OFF` by default so a casual build still works |
| **GhostPDL** | No in-process PDF conversion for printing | `RPCEMU_ENABLE_GHOSTPDL` |

### Networking is simpler than it used to be

Worth knowing before you go looking for work that no longer exists. Ethernet
bridging and IP tunnelling were network types 2 and 3, and they are **gone** from
`main` (`src/rpcemu.h:101`). Only Linux ever implemented them, every other
platform silently did nothing while still offering the choice in the machine
editor, and NAT with port forwarding covers what they were for.

What remains is NAT through the bundled slirp, which is ordinary sockets, and the
JSON network transport, which is TCP. Neither needs a per-platform backend. If
you are working from `1.x` you will still meet `src/network-tun.c` and its
`linux/if_tun.h` include; on `main` there is nothing there to port.

slirp already carries one `__HAIKU__` guard, at `src/slirp/slirp.h:29`, for
`O_BINARY`. Expect a small number of missing includes of the same kind rather
than anything structural: [#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123)
needed `<sys/select.h>` in `src/slirp/libslirp.h` for `fd_set` on OpenBSD.

### Large file support

`src/lfs-compat.h` maps `off64_t`, `fopen64` and friends onto the plain names for
platforms whose `off_t` is already 64-bit. It currently special-cases
`__APPLE__`; the BSDs need the same treatment and it is a one-line change.

## Platform notes

These are assessments, not promises, and none of them has been run by the
maintainers. Where something is stated as fact it was checked; where it is a
judgement it says so.

### OpenBSD

The furthest along, via [#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123).
Builds and runs with audio, VNC and HostFS working. `wxWidgets-gtk3`,
`libvncserver` and `cmake` are all in ports.

The outstanding work is W^X for the recompiler, which is the whole reason that
port runs interpreted, and then the ordinary business of warnings and tests. The
OpenBSD libc deliberately warns on `strcat` and similar, so expect the warnings
gate to have opinions.

### NetBSD

Untried, and expected to be the cheapest of the three. Same POSIX shape as
OpenBSD without the W^X wall, and pkgsrc carries wxGTK, SDL2 and libvncserver.
Realistically this is OpenBSD's patch set with less to fight.

### Haiku

More plausible than it sounds. HaikuPorts carries `media-libs/libsdl2`,
`net-libs/libvncserver` and `x11-libs/wxgtk` at 3.2.6, which clears the
wxWidgets floor. There is already a `__HAIKU__` conditional in slirp.

The unknowns, in order: whether that wxGTK build gives a usable `gl` component,
and how the display performs if it falls back to software rendering. Neither is a
blocker for a first build, because the CPU display path exists for exactly this
case.

### FreeBSD

Not attempted, and probably the easiest Unix target of all given how close it is
to what already works. Listed here mostly so nobody assumes it was ruled out.

## The part that actually costs: proving it works

Read this before starting, because it is the part that decides whether a port
survives.

Nobody maintaining this project has a machine running any of the platforms above.
The project's own rule is that **a platform nobody can run is only as trustworthy
as its CI**, and claiming a fix works on a platform it has never run on is the
fastest way to make the rest of a report untrustworthy. Two Windows issues were
fixed here without any way to try them, and that had to be said plainly in the
release notes rather than glossed over.

GitHub Actions provides Ubuntu, Windows and macOS runners and nothing else. The
usual route for anything else is `vmactions`, which runs the target OS under QEMU
on an Ubuntu runner:

| Runner | Maturity, as of August 2026 |
| --- | --- |
| `vmactions/freebsd-vm` | Widely used |
| `vmactions/openbsd-vm` | Established |
| `vmactions/netbsd-vm` | Smaller, active |
| `vmactions/haiku-vm` | Very new. Not something to hang a release gate on |

These work, but they are emulated, so they are slow, and a red job on one of them
is a job nobody here can reproduce locally. That is the real ongoing cost of a
port, and it is why the tiers below exist.

`.github/workflows/build.yml` is a chain of gates, currently sixteen jobs, where
each platform ends in a single job so the row can be read as "did Linux pass?".
A new platform should follow that shape. Note the comment at the top of the file
about `if: always() && <condition>`: a skipped job skips everything behind it, so
a new job needs the same treatment as the existing ones.

## Support tiers

A port should be honest about which of these it is, in the documentation and in
the release notes, because the difference matters to somebody deciding whether to
rely on it.

| Tier | Means | CI |
| --- | --- | --- |
| **Supported** | Someone can run it, reproduce a bug on it, and fix it. Linux, Windows, macOS | Gates the release |
| **Community** | It builds and is tested by CI. Bugs are fixed if somebody who runs it diagnoses them | Builds and reports, does not gate a tag |
| **Contributed** | Patches are carried so the platform is not actively broken. No claim beyond that | None, or a build job only |

The pattern for a non-gating job already exists: `etherrpcem-riscos` builds the
guest network driver on an external service and is watched by `notify` without
blocking a release, on the reasoning that a tag should not be held up by a
third-party service being unreachable. The same reasoning applies to a QEMU
runner for an OS nobody here can debug.

**The failure mode to avoid is promising support you cannot verify.** A platform
listed as supported, which nobody can run, is worse than one honestly labelled
community.

## A suggested order of work

1. **Get it configuring.** Replace the `CMakeLists.txt:82` guard with your
   platform, at whatever tier you are claiming.
2. **Get it building interpreted.** `-DRPCEMU_DYNAREC=OFF`. Expect missing
   includes and `lfs-compat.h`. This is roughly where
   [#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123) got to.
3. **Turn networking back on.** One line in `src/rpcemu.h:88`, then whatever
   slirp needs.
4. **Get the test suite passing.** `ctest` from the build directory. Report a
   test that is wrong rather than working around it.
5. **Clear the warnings gate.** `tests/check-warnings.sh` against a full release
   build. Run it on a release build specifically, because the optimiser is what
   raises several of the interesting ones.
6. **Add a CI job**, non-gating, so the work does not rot.
7. **Then the recompiler**, if your platform enforces W^X.

Steps 1 to 4 are a weekend for somebody who runs the OS. Step 7 is the project.

## Things that are out of scope

- **Big-endian hosts.** There is inherited scaffolding, 54 `_RPCEMU_BIG_ENDIAN`
  sites across `src/mem.c` (35), `src/vidc20.c` (14), `src/ide.c` and
  `src/mem.h` (2 each) and `src/romload.c` (1), but nothing in the build system
  has ever defined it, so it has never been compiled
  and should be assumed bitrotted. A big-endian port means reviving all of it
  and then proving it, which is a much larger undertaking than a little-endian
  Unix port and should not be started casually.
- **New recompiler backends.** `codegen_amd64.c`, `codegen_arm64.c` and
  `codegen_x86.c` exist. Anything else runs interpreted through
  `codegen_null.c`, which is correct and slow. Writing a fourth backend is a
  project in its own right, not part of an OS port.
- **MSVC.** `CMakeLists.txt` rejects it explicitly. Windows means MinGW-w64.

## If you are picking this up

Say which platform and which tier you are aiming at before writing much code, so
the CI question is settled early rather than after the patches exist. A port with
no CI is a port that breaks quietly, and the person it breaks for is the next
person who trusts it.

Small, separately reviewable commits are much easier to take than one large one,
which is what [#123](https://github.com/andrewtimmins/rpcemu-extended/pull/123)
got right.

See also [`docs/testing.md`](testing.md) for what the suite covers and how to run
the warnings gate, and [`docs/paths.md`](paths.md) for how the data and resource
directories are found, which is the other thing a new platform usually has an
opinion about.
