# MMU access permissions

**Fixed.** RPCEmu used not to enforce page access permissions on an access whose
address translation was already cached: a User-mode write to a page marked
Supervisor read/write, User read-only succeeded when it should have raised a Data
Abort, provided something had touched that page beforehand. That is the reason
[ADFFS](https://www.jaspp.org.uk/) told people to avoid RPCEmu from 2013 onwards.

`tests/test_mmu_perms` is the regression test, 19 checks, and it went from 7
failures to none. What follows is kept as the record of what was wrong, why the
fix is shaped as it is, and which two approaches were tried.

## The report

Jonathan Abbott (JASPP) on 29 January 2015: "RPCEmu doesn't emulate memory page
access correctly", and two days later "RPCEmu - avoid. There are several flaws in
it's emulation of the ARM MMC which make it unsuitable for running ADFFS, very
few games will run under ADFFS on this emulator." He raised it with RPCEmu's
developers in 2013. Every ADFFS release since carries this in its Known Issues:

> "JIT will not work under RPCEmu as it does not generate Page Faults when User
> mode writes to pages with Supervisor only write permissions"

That matters beyond ADFFS itself: its JIT uses page protection to detect
self-modifying code, so without working permission faults most games under ADFFS
do not run. 148 of the 169 packages in the JASPP repository depend on ADFFS.

## What was wrong

The permission logic is **correct**. `cp15_check_permissions()` in `src/cp15.c`
implements ARM DDI 0100B Table 7-7 exactly, all seven rows including the
UNPREDICTABLE case, and the fault codes and Fault Status/Address registers are
right too. The problem is that the check is usually never reached.

Three caches sit in front of it, and none records the permissions or the
privilege level it was filled under:

| Cache | Where | Consulted by |
| --- | --- | --- |
| `tlbcache` | `src/cp15.c` | the `translateaddress` macro in `src/mem.h` |
| `readmemcache` / `writememcache` | `src/mem.c` | `readmemfl` / `writememfl` |
| `vraddrl` / `vwaddrl` | `src/cp15.c` | `mem.h` inline accessors, and emitted code in both JIT backends |

The `translateaddress` macro takes a cached entry and **discards its own `rw`
argument**. The maps hold a bare host pointer. So once a page is cached, from any
access in any mode, every later access to it is permitted.

Five places consume those caches, and a fix has to cover all five: the eight
inline accessors in `src/mem.h`, the four `readmemfl`/`writememfl` functions,
`getpccache()`, and the emitted code in `codegen_amd64.c` and `codegen_arm64.c`.

This is inherited code. Both the permission check and the cache macro date from
the initial import from upstream RPCEmu and have never been modified here.

## What the hardware does

ARM DDI 0100B §7.6, on the TLB:

> "The TLB caches virtual to physical address translations **and access
> permissions** for each translation. If the TLB contains a translated entry for
> the virtual address, **the access control logic determines whether access is
> permitted**. If access is permitted, the MMU outputs the appropriate physical
> address... If access is not permitted, the MMU signals the CPU to abort."

So permissions are cached with the entry and evaluated on **every** access. That
is the behaviour to reproduce.

## The reproduction

`tests/test_mmu_perms` builds real page tables in emulated RAM, turns the MMU on,
and exercises Table 7-7 directly. Before the fix it reported **7 failures out of
19 checks**:

- Every **cold** access behaves correctly, which is what tells you the permission
  logic itself is sound.
- Every **illegal access that follows a legal one to the same page** is wrongly
  allowed: user write after user read, after supervisor write, and after
  supervisor read; and user read after supervisor read on a page User may not
  read at all.
- **`STRT`/`LDRT`** are wrongly allowed once a translation is cached.
- **Instruction fetch** is wrongly allowed after a privileged fetch has cached
  the page, so Prefetch Aborts are broken in the same way. This is not in
  Abbott's report; it was found while reading `getpccache()`.

All 19 pass now, and it runs as part of `ctest`. By hand:

```sh
cmake --build build --target test_mmu_perms
./build/tests/test_mmu_perms
```

## What was done

Two things, and both were needed.

**`tlbperm[]`**, one byte per page beside `tlbcache[]`, holding the four
precomputed outcomes for (privilege, read/write). `translateaddress` is now an
inline function instead of a macro and checks those bits on a TLB hit rather than
discarding its `rw` argument; when they refuse, it falls through to the full walk
so the fault is raised in one place with the right fault code and registers. The
three single-entry page caches remember the privilege level they were filled under
and drop themselves when it changes.

**Per-privilege fast maps.** `vraddrl`/`vwaddrl` became pointers into
`vraddrl_mode[2][]`/`vwaddrl_mode[2][]`, selected by `mem_set_privilege()`. A page
is installed only in the maps of privilege levels actually allowed the access, so
the maps stay permission-blind without being wrong, and the existing "low bits
clear means accessible" gate needs no change. Each JIT prologue loads the map base
from the pointer instead of a baked-in array address, one load per block.

Verified: 19/19 on the new test, all 16 tests pass, and a headless boot produces a
log identical to the unfixed build (including its pre-existing Data Abort) and
reaches the same milestone in the same wall time.

### Three traps this hit on the way

- **`sizeof(vraddrl)` became the size of a pointer**, so `cp15_reset()`'s memset
  initialised 8 bytes instead of 8MB. A map entry left as zero reads as
  *accessible* and dereferences host address 0 plus the guest address, so the
  emulator segfaulted during boot. Memset the arrays, never through the pointer.
- **amd64 `MOV (%r13),%r13` needs an explicit disp8.** ModRM `rm=101` with
  `mod=00` means RIP-relative, so `4d 8b 2d` is not `(%r13)`; it has to be
  `4d 8b 6d 00`.
- **The JIT tests fill the maps by hand** and had to be taught there are two of
  them, or an `LDRT` mid-case switches to a map the test never populated.

## Two approaches, one of them ruled out by measurement

**Flush every cache when privilege changes.** Ruled out, on two counts.

It is *semantically insufficient*: `mem_user_read32`/`mem_user_write32`, which
implement `LDRT`/`STRT`, set `memmode` directly and put it back, so they never go
through `updatemode()`. A hook there cannot see them, and the test cases above
prove those accesses are exactly where a cached entry does the damage.

It is also far too expensive. Instrumenting `cp15_tlb_flush_all()` and booting
RISC OS measured **over 900,000 privilege changes in 55 seconds**, about 16,000 a
second, each one walking roughly 3,000 array slots and discarding every cached
translation.

**Install only where both privilege levels agree.** Also tried, also ruled out,
and this one is worth recording because it looks appealing: it needs no codegen
changes at all. Measured, it declined **3.1 billion fast-map installs in 70
seconds, 100% of them**. The declined pages report permissions of Supervisor
read/write with no User access, which is what nearly all of RISC OS's kernel
memory looks like, so the rule removes the fast path from the majority of all
accesses.

**Per-privilege maps, permissions decided when the entry is filled.** This is what
was done, and it mirrors the hardware.

Keep a user and a privileged version of each map. A page enters the user write map
only if User writes are permitted, the privileged write map only if Supervisor
writes are, and likewise for reads. The permission decision moves to fill time,
where it costs nothing, and the existing "low bits set means not accessible" gate
does the rest.

Three things make this cheaper than it sounds:

1. **The gate already exists and is already checked on every access.** All three
   consumers use the same convention: `vraddrl[page] & 1` for reads and
   `vwaddrl[page] & 3` for writes, in `mem.h`, as `TEST $1,%dl`/`TEST $3,%dl` on
   amd64, and as `tbnz`/`cbnz` on arm64. Nothing new goes in the hot path.
2. **Compiled code can never change privilege mid-block.** `MSR` and `MRS` are not
   recompiled (entries 0x10, 0x12, 0x14, 0x16 of `canrecompile` are 0), and
   exceptions leave the block. So the map base can be loaded from a variable at
   block entry, one load per block, instead of the immediate used today.
3. **`LDRT`/`STRT` are not recompiled either** (0x42, 0x43, 0x46, 0x47 are 0), so
   they are interpreter-only and simply index the user maps.

Cost: roughly 16MB more host RAM for the doubled maps, plus about 1MB if
`tlbcache` carries cached permission bits in a parallel array. No extra
instructions per memory access. One extra load per compiled block.

### Things not to break

- **`getpccache()` clears `vwaddrl[page]`** on every page it fetches from, so that
  writes to executed pages take the slow path and the code cache can be
  invalidated. With two write maps it must clear **both**, or a page being
  executed stays directly writable in the privilege level that was missed. That
  would corrupt the code cache rather than fault, so it would surface later as
  something baffling.
- **DMA must keep bypassing the MMU.** `usb_ohci.c` uses
  `mem_phys_read32`/`mem_phys_write32`, and the debugger uses
  `mem_phys_read8_debug`. That is correct for a bus master and for a debugger.
  Do not route them through permission checks.
- **The `f` argument to `vradd`/`vwadd` is currently discarded.** All 33 call
  sites pass a value (2 for the ROM read cases, 0 otherwise), both functions do
  `NOT_USED(f)`, and the `| f` is commented out. The vestigial `& ~3` masks in
  `readmemfl` are left over from when it was live. What bit 1 was originally for
  is not clear and is worth understanding before reusing the channel.
- **`memmode` as a bool is correct**, including in 26-bit modes:
  `ARM_MODE_PRIV(mode)` is `mode & 0xf`, so USR26 (0x00) and USR32 (0x10) are
  both unprivileged and every other mode is privileged.
- **Save/load state is already sound.** Only architectural CP15 state is saved and
  `cp15_loadstate()` ends with a full flush, so a restored snapshot cannot carry
  stale permissions.

## Related gap: alignment faults

`CP15_CTRL_ALIGNMENT_FAULT` is defined in `src/cp15.c` and **never referenced**.
Nothing checks alignment and aborts, so the fourth of the four MMU fault types in
DDI 0100B §7.11 is absent entirely. RISC OS runs with the A bit clear and relies
on the unaligned-word rotate, which is implemented, so this is lower priority than
the permission bypass. It belongs on the same list.

## Reference material

The datasheets are in `reference/arm-docs/` (gitignored), from
<https://home.marutan.net/arcemdocs/>. Note the TLS certificate covers
`home.marutan.net` but not `www.home.marutan.net`.

- **ARM DDI 0100B** and revisions D, E and I. Chapter 7 is the MMU; §7.6
  architecture, §7.8 access permissions, §7.9 domains, §7.10-7.11 aborts and
  faults. Revision E is the one with 26-bit mode detail.
- **ARM610, ARM710, ARM7500, ARM7500FE, ARM810, SA-110** for the per-part detail
  the architecture manual leaves open.

Worth knowing about the StrongARM: the SA-110 has **separate 32-entry instruction
and data TLBs**, where the data TLB supports flush-single-entry and the
instruction TLB only flush-all. We model one unified TLB and turn every SA-110 TLB
operation into a full flush, which is conservative and safe but slower than
hardware. It also means that if the split TLBs are ever modelled properly, the
per-privilege maps become per-privilege-per-stream.

Arculator is not a reference for any of this, despite Abbott rating its accuracy:
its `cp15.c` is ARM3 cache control and it has `memc.c`, so it emulates MEMC
machines, which have no ARM page tables.
