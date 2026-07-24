# AArch64 (arm64) dynarec backend

`codegen_arm64.c` is the AArch64 code generator for the dynamic recompiler. It
lets arm64 hosts — Apple Silicon, Raspberry Pi 4/5, arm64 Linux, Windows on ARM
— run the recompiler rather than the interpreter. Read [dynarec.md](dynarec.md)
first for how the recompiler is structured as a whole; this document covers only
the arm64-specific backend.

## Where it fits

The recompiler is split into a target-independent front-end (`arm_dynarec.c`,
which decodes guest ARM, owns the block cache, and drives code generation) and a
host-specific back-end that emits machine code. `codegen_arm64.c` is one such
back-end, selected automatically by CMake when `CMAKE_SYSTEM_PROCESSOR` is
`aarch64`/`arm64` (the amd64 and x86 backends are `codegen_amd64.c` and
`codegen_x86.c`).

The back-end natively recompiles the common instruction classes and emits a call
to the existing C interpreter helper (`arm_opcode_fn`) for everything else, so
correctness never depends on full coverage.

## Host register assignment

Chosen to mirror the amd64 backend while respecting AAPCS64 (x19–x28 are
callee-saved):

| Host register | Role                                    |
|---------------|-----------------------------------------|
| `x19`         | pointer to `ARMState` (`&arm`)          |
| `x20`         | guest R15 cache (PC, plus flags in 26-bit mode) |
| `x21`         | `&vwaddrl[0]` — write TLB base          |
| `x22`         | `&vraddrl[0]` — read TLB base           |
| `x23`         | memory address, preserved across helper calls |
| `x9`–`x14`    | code-generation scratch                 |
| `x0`–`x2`     | C-helper arguments (AAPCS64)            |
| `x29` / `x30` | frame pointer / link register           |

Each block's prologue saves the callee-saved registers it uses, loads the four
base pointers, and caches guest R15 in `x20`; the epilogue writes `x20` back to
`arm.reg[15]` and returns. A block is entered as `void (*)(void)` at offset
`BLOCKSTART`; the epilogue lives at offset 0 and the body branches there to exit.

## Instruction encoding

AArch64 instructions are fixed 32-bit little-endian words, so the emitter is a
set of small helpers (`a64_*`) that assemble one word each. Two points specific
to the architecture:

- **Helper calls** load the absolute 64-bit address of the C helper into a
  scratch register and use `BLR`, because `BL` only reaches ±128 MB and helper
  addresses are not guaranteed within range. AAPCS64 passes arguments in `x0…`,
  so no argument shuffling or shadow space is needed.
- **Instruction-cache coherency** is not automatic on ARM. After a block is
  written and before it executes, `endblock()` calls
  `__builtin___clear_cache()` over the block's range.

## Condition flags

The guest condition codes map directly onto AArch64's: `ADDS`/`SUBS`/`ADCS`/
`SBCS` set N, Z, C and V exactly as ARM does, and `SUBS` sets carry as
NOT-borrow like ARM — so no translation is required. `gen_native_flags()` reads
the host flags with `MRS …,NZCV` and merges them into the guest flag word (the
`x20` cache in 26-bit mode, or `arm.reg[16]` in 32-bit mode). Arithmetic ops take
all four flags; logical ops take only N and Z and preserve C and V.

Conditional execution is resolved through the shared `flaglookup` table: the
back-end loads the flag nibble, indexes the table, and skips the instruction with
a `CBZ` when the condition fails.

## Memory access

`genldr`/`genldrb`/`genstr`/`genstrb` inline the fast path: index the read/write
TLB (`vraddrl`/`vwaddrl`), and if the page is present, compute
`host = guest + tlb_entry` and load/store directly (rotating unaligned word loads
with `RORV`). A TLB miss, or a store to a read-only page, falls back to
`readmemf*`/`writememf*`. `gen_single_data_transfer()` builds the address for all
addressing modes (pre/post-index, up/down, register/immediate offset, base
writeback); `gen_block_transfer()` handles LDM/STM, including the page-boundary
check and the C-helper fallback for the awkward cases.

## Block linking

When a block ends, `endblock()` decrements the cycle budget, checks for a pending
event, then looks the next guest PC up in the block cache; on a hit it branches
straight into that block (past its prologue, since `x20` is already live),
avoiding a round-trip through the dispatcher. Any miss falls through to the
epilogue.

## What is recompiled

Natively recompiled: data-processing (all ALU operations and the TST/TEQ/CMP/CMN
compares, register and immediate forms), MUL/MLA/UMULL, LDR/STR/LDRB/STRB, LDM/STM
(S-flag clear), and B/BL.

Handled by falling back to the interpreter (matching the amd64 backend): v4
halfword and signed transfers (LDRH/STRH/LDRSB/LDRSH), register-specified shifts,
flag-setting logical operations that carry a real shift, ADC/SBC/RSC, the signed
and accumulating long multiplies, and any instruction writing R15 that would need
special block-flow handling.

## Testing

The differential testers in `tests/` build the recompiled form and the
interpreter form of each instruction and assert they produce identical register
and flag state. They are architecture-generic (they also run against the amd64
backend) and are built by CMake when `RPCEMU_BUILD_TESTS` is on:

| Test              | Covers                                  |
|-------------------|-----------------------------------------|
| `test_jit_flags`  | data-processing NZCV, 26- and 32-bit    |
| `test_jit_mem`    | LDR/STR/LDRB/STRB addressing modes      |
| `test_jit_branch` | B / BL                                  |
| `test_jit_mul`    | MUL / MLA / UMULL                       |
| `test_jit_ldmstm` | LDM / STM                               |
| `test_jit_e2e`    | block linking, running a real program through `arm_exec` |

On an x86-64 development host the arm64 backend can be built with
`aarch64-linux-gnu-gcc` and the tests run under `qemu-aarch64`; the memory tests
point the `vraddrl`/`vwaddrl` TLB entries at a host buffer (no MMU set-up needed,
since the interpreter and the recompiled code share the same TLB), and the
end-to-end test runs with the MMU off and code in RAM, comparing the recompiler
(`dcache = 1`) against the interpreter (`dcache = 0`).

## Platform note

On Linux and Windows the code cache is a static array made executable once with
`mprotect`/`VirtualProtect` (`set_memory_executable()`).

On Apple Silicon the hardened runtime rejects a static RWX array, so the cache
is allocated with `mmap(..., MAP_JIT)` and each thread toggles it between
writable and executable with `pthread_jit_write_protect_np()`. `initcodeblock()`
enables writing before a block is generated and `endblock()` returns the cache
to executable once the block is complete; `codegen_jit_writable()` /
`codegen_jit_executable()` are no-ops on every other platform. This requires the
`com.apple.security.cs.allow-jit` entitlement, so a recompiler build has to be
signed with it before it will run:

```
codesign -s - --force \
    --entitlements resources/rpcemu-jit.entitlements \
    rpcemu
```

`build-macos.sh` does this automatically when `RPCEMU_MACOS_CODESIGN=1` is set.
The prebuilt macOS release ships the arm64 slice as the interpreter, so it is
unsigned and needs neither `MAP_JIT` nor the entitlement.

## Adding a new recompiled instruction

1. Add the emitter helpers for any AArch64 forms you need, and confirm their
   encodings (for example by disassembling emitted words with
   `aarch64-linux-gnu-objdump`).
2. Handle the instruction in `recompile()` (or one of the `gen_*` dispatchers),
   returning 0 to fall back for cases you do not cover.
3. Mark the opcode recompilable in `canrecompile[]`.
4. Extend the relevant differential test and run it under qemu.
