# The disassembler

RPCEmu disassembles ARM instructions for the Machine Inspector's disassembly
pane, for the `dis` command on the [DebugCmd socket](debugcmd.md), and for the
SWI names shown in the trace log. It is `src/arm_disasm.c`, written for this
emulator rather than taken from a library, and this note explains why.

## What it covers

The instruction sets of the cores RPCEmu implements, and nothing beyond them:

- **ARMv3** — ARM610, ARM710, ARM7500, ARM7500FE
- **ARMv4** — ARM810, StrongARM SA110

That means data processing, multiplies (including the long forms), single and
block transfers, halfword and signed loads, branches, `BX`, `SWP`, `MRS`/`MSR`,
`SWI`, and coprocessor operations.

Two additions are specific to this machine and are the reason the disassembler
is hand-written.

**RISC OS SWI names.** `SWI &20` disassembles as `SWI OS_File`, from a table of
around 790 names. A `SWI` number is the single most informative thing in a
listing of RISC OS code, and a general-purpose ARM disassembler has no idea
what any of them mean.

**FPA10 floating point.** `src/fpa.c` emulates an FPA on coprocessors 1 and 2,
and RISC OS C code is thick with its instructions. They disassemble properly —
`ADFD F0, F1, F2`, `LDFS F3, [R1, #16]`, `CMF F0, #1.0`, `FIXZ R2, F5` — with
the precision (`S`/`D`/`E`/`P`) and rounding (`P`/`M`/`Z`) suffixes. Coprocessor
numbers other than 1 and 2 still decode generically, as `CDP`, `LDC`, `MRC` and
so on, which is right for CP15.

There is **no Thumb support**, because no core RPCEmu emulates has a T bit.

## Why not Capstone, or another library

It was considered. For this machine a general ARM disassembler is a downgrade,
not an upgrade:

- **It cannot decode FPA.** Capstone is derived from LLVM, which never supported
  the FPA — the format predates VFP and was Acorn's. The single biggest gap in
  a RISC OS disassembler is the one a library cannot fill.
- **It knows no SWI names.** `SWI OS_File` would become `svc #0x20`.
- **Its extra coverage is unreachable.** Thumb, ARMv5 through v8, NEON and VFP
  are all irrelevant to an ARM610 or a StrongARM.
- **It costs something.** As a system dependency it means edits in four
  cross-build scripts, two of which build dependencies from source per
  architecture; vendored, it means megabytes of generated tables in a tree that
  compiles warning-free.

What a library would genuinely have brought — structured operand and
control-flow information rather than only text — was a bounded change to a
closed instruction set that was already decoded correctly. That is `arm_decode()`
below.

## Two entry points

```c
const char *arm_disasm(uint32_t opcode, uint32_t address,
                       char *buffer, size_t buflen);

int arm_decode(uint32_t opcode, uint32_t address, ArmInsnInfo *out);
```

`arm_disasm()` renders text. `arm_decode()` describes what the instruction
*does*: its class, the registers it reads and writes, and — the part the
debugger is built on — whether it is a call, whether it is a return, and where
it branches to.

Those three fields are what make step-over, step-out and backtraces possible.
Step-over has to know whether the instruction at the PC is a call, because if it
is, the right move is to run the subroutine and stop after it; if it is not, the
right move is a single step. Text cannot answer that, which is why for a long
time there was no step-over.

`arm_disasm_sym()` is `arm_disasm()` with a symbol resolver, which annotates a
branch target with the name it lands in.

## Testing

`tests/test_arm_disasm.c` covers both entry points: a sample of every encoding
class as a regression lock on the text, and the `ArmInsnInfo` fields — where a
mistake no longer shows up as an odd-looking line but as a debugger that runs
away when asked to step.

The FPA encodings there were hand-assembled from the operation tables in
`fpa.c`, so that test is also the written record of what those bit patterns
mean.
