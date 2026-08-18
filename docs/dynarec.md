# ARM dynamic recompiler (dynarec)

RPCEmu emulates the guest **ARM** CPU in two ways:

1. **Software interpreter** (`arm.c`) — decodes and executes each guest instruction in
   host code (`rpcemu-interpreter`).
2. **Dynamic recompiler** (`arm_dynarec.c` plus `codegen_*.c`) — translates guest ARM
   instruction sequences into native host code (x86-64, i386 or arm64) and runs that
   (`rpcemu-recompiler`).

Both are implemented in C. The choice is **how guest instructions run**, not what
language the emulator is written in.

Even in a recompiler build, `arm_dynarec.c` still uses the same software interpreter
loop to compile new blocks and whenever execution must fall back (debugger, guest cache
off, no compiled block yet).

---

## Building

CMake option `RPCEMU_DYNAREC` (default **ON**) selects the CPU backend:

| `RPCEMU_DYNAREC` | CPU backend | Release binary |
| --- | --- | --- |
| ON | Dynarec (JIT) | `rpcemu-recompiler` |
| OFF | Software interpreter | `rpcemu-interpreter` |

Debug builds append `-debug` to the binary name.

The codegen backend follows the **host** CPU (`src/CMakeLists.txt`):

| Host | Codegen source |
| --- | --- |
| x86_64 | `codegen_amd64.c` |
| i386 / i686 | `codegen_x86.c` |
| aarch64 / arm64 | `codegen_arm64.c` (see [arm64-dynarec.md](arm64-dynarec.md)) |
| Other | Configure fails if dynarec is ON |

On a host with no dynarec backend, build the interpreter instead:

```bash
./build.sh --interpreter
# or: cmake -S . -B build -DRPCEMU_DYNAREC=OFF
```

See [COMPILE.md](../COMPILE.md) for the full build reference.

---

## Source layout

| File | Role |
| --- | --- |
| `src/arm.c` | Software interpreter (interpreter build only) |
| `src/arm_dynarec.c` | Execution loop, block compilation, debugger hooks |
| `src/arm_dynarec_ops.h` | Opcode dispatch table for the dynarec build |
| `src/codegen_amd64.c` / `codegen_x86.c` | Emit x86 machine code, block cache |
| `src/codegen_arm64.c` | Emit AArch64 machine code (see [arm64-dynarec.md](arm64-dynarec.md)) |
| `src/codegen_x86_common.h` | Shared helpers (i386 codegen) |
| `src/codegen_null.c` | Stubs when dynarec is disabled |

Compiled guest code is stored in `rcodeblock[][]`. `set_memory_executable()` in
`arm_dynarec.c` marks that region executable — `mprotect()` on Linux and macOS,
`VirtualProtect()` on Windows.

---

## How it works

### Block boundaries

While building a block (interpreter loop in `arm_exec()`), compilation stops at
branches, SWIs, coprocessor instructions, writes to R15, and some LDR/LDM forms that
reload PC. The same rules apply when falling back to software execution.

### Block cache

- `BLOCKS` = 1024 slots; each holds up to 1792 bytes of native code (~1.8 MB total).
- Guest PCs are hashed into `codeblockpc[]` (collisions possible).
- When the cache is full, slots are reused round-robin (`blockpoint` in codegen).

### Self-modifying code

Guest RAM writes go through `mem.c`, which calls `cacheclearpage()` so compiled code
for that page is dropped. Execution then recompiles as needed.

`resetcodeblocks()` clears the whole JIT cache on relevant **CP15** writes (for
example enabling the instruction cache, MMU/ROM/system control changes, TLB maintenance).

### When JIT vs interpreter runs

In a recompiler build, native code runs only when:

- Guest **data cache** is enabled (`dcache` from CP15 — toggled by RISC OS `*cache`,
  and exposed as `isblockvalid()` in codegen headers, which is effectively `dcache`),
- A compiled block exists for the current PC (hash slot matches),
- The debugger does not require per-instruction hooks.

With `*cache 0`, or when debugging (breakpoints, single-step, watchpoints, paused),
execution uses the software interpreter path inside `arm_dynarec.c` — the same loop
that compiles blocks. That improves compatibility; speed is usually close to
`rpcemu-interpreter`.

### Debugger

`debugger_requires_instruction_hook()` forces the interpreter path when the Machine
Inspector debugger is paused, stepping, or has breakpoints/watchpoints active.

---

## CPU models

Dynarec development and testing historically targeted **StrongARM (SA110)**. ARM610,
ARM710, and ARM7500 are selectable but less exercised.

Many programs that use self-modifying code work (e.g. Ovation, SparkFS). If something
misbehaves under JIT, try `rpcemu-interpreter` or `*cache 0` in a recompiler build.

FPA10 emulation is available in both the interpreter and the dynarec.

---

## Codegen backends

All backends share the same driver in `arm_dynarec.c` and differ only in the
machine code they emit:

| | i386 (`codegen_x86.c`) | amd64 (`codegen_amd64.c`) | arm64 (`codegen_arm64.c`) |
| --- | --- | --- | --- |
| Host | 32-bit x86 | x86-64 | AArch64 |
| `BLOCKSTART` | 16 | 32 | 32 |
| Inline flag-setting ops | no | yes | yes |
| Block linking | yes | yes | yes |

The arm64 backend is described in detail in [arm64-dynarec.md](arm64-dynarec.md).

### Inline flag-setting instructions (amd64 and arm64)

The amd64 and arm64 backends recompile the flag-setting data-processing and
compare instructions inline rather than calling the interpreter.
`gen_native_flags()` produces the ARM `NZCV` flags from the host and merges them
into the cached R15 (26-bit mode) or `arm.reg[16]` (32-bit mode). On amd64 the
flags come from the x86 `EFLAGS` (via `SETcc`/`LAHF`); on arm64 they come straight
from the host `NZCV`, which lines up with the ARM condition flags bit-for-bit (and
`SUBS` sets carry as NOT-borrow, exactly as ARM does). Both cover:

- Arithmetic and compares — `ADDS`, `SUBS`, `RSBS`, `CMP`, `CMN` (register and
  immediate).
- Logical / move / test — `ANDS`, `EORS`, `ORRS`, `BICS`, `MOVS`, `MVNS`, `TST`,
  `TEQ` — when operand 2's shift cannot alter the carry; otherwise they fall back.
- `ADCS` / `SBCS` / `RSCS` fall back to the interpreter (carry-in sourcing differs
  by mode).

The i386 backend does not recompile flag-setting ops inline; it calls the
interpreter for them.

Correctness is covered by the differential tests in `tests/` (run via `ctest`),
which check each recompiled instruction class against the interpreter across
carry/overflow edge cases in both 26- and 32-bit modes. They are
architecture-generic and validate the amd64 and arm64 backends alike.

---

## Limitations

- A dynarec backend exists for x86-64, i386 and arm64; on any other host, build
  the interpreter.
- Block chaining (direct jump to the already-compiled successor block) helps loops;
  extending backward branches inside a single block was tried with little benefit.
- Guest prefetch and data aborts depend on correct interaction between generated loads,
  the MMU, and CP15 — problems here show up mainly on RISC OS 4.x.

---

## Origin

The dynarec was written by Sarah Walker for upstream RPCEmu. RPCEmu Extended builds
it via CMake, ties it to the integrated Machine Inspector debugger as described above,
and adds the AArch64 backend ([arm64-dynarec.md](arm64-dynarec.md)).
