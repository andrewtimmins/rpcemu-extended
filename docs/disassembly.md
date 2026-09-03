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

## How it is written down

Five options change the rendering without changing what is decoded, set through
`arm_disasm_set_options()` and exposed as checkboxes beside the Machine
Inspector's disassembly view. They came out of discussion #223, from somebody
reading the disassembly against his own assembler source: a listing that does
not look like the code you are comparing it against costs a translation step on
every line.

| Option | Off | On |
| --- | --- | --- |
| `hex_immediates` | `MOV R0, #128` | `MOV R0, #&80` |
| `apcs_registers` | `MOV PC, R14` | `MOV pc, lr` |
| `collapse_reglists` | `{R0, R1, R2, R3, R7}` | `{R0-R3, R7}` |
| `resolve_pc_relative` | `LDR R11, [PC, #-120]` | `LDR R11, &00007F90` |
| `lowercase` | `LDMIA R13!, {R0-R3, PC}` | `ldmia r13!, {r0-r3, pc}` |

`hex_immediates` governs the whole line - branch targets, SWI numbers and MSR
immediates as well as operands - because a listing mixing `&80` with `0x374C` is
the inconsistency the option exists to remove. A named SWI keeps its name: the
name is the useful part and is not a number to reformat.

Two things are deliberately not options. Register names are consistent whichever
setting is chosen: with `apcs_registers` clear the answer is R0-R14 and PC, where
this disassembler used to print R13 and R14 as SP and LR while numbering
everything else - the mixture the request complained about. And a range is only
collapsed for three or more registers, since `{R0-R1}` is longer than
`{R0, R1}`.

The options are global rather than per call, because the whole disassembler is a
set of static tables and every caller wants the same answer. That includes the
debug socket's `dis`, so one machine disassembles one way whoever asked. The GUI
thread writes them when a checkbox moves while the emulator thread may be
reading: the fields are single bytes, and the worst a race can do is render one
line with a mixture of old and new settings.

`lowercase` **is** a post-pass over the finished string, but not a blind one.
Lower-casing the whole line would turn `SWI OS_WriteC` into `swi os_writec`,
and `os_writec` is not a lower-case style of `OS_WriteC` - it is wrong, because
that is somebody's identifier and not a mnemonic. So as the line is built, the
spans holding a SWI name and a symbol annotation are recorded, and `apply_case()`
lowers everything outside them. Doubling the mnemonic and operand tables was the
alternative and would have to be kept in step for ever; four recorded spans do
not.

The `X` of an X-form SWI belongs to the name for this purpose. `XOS_Exit` is one
identifier, so the protected span starts one character to the left of the name
proper - the first version of this ate the `X` and printed `xOS_Exit`.

## SWI names for your own modules

The built-in table is the OS's. A call into a module of your own disassembles as
`SWI &42C40`, which has to be looked up by hand every time, so names can be
loaded from a file:

```
# a module's own SWIs
&42C40,MyModule_Doit
&42C41,MyModule_Undo
```

One SWI per line, the number and the name separated by a comma. Numbers may be
written `&hex` as RISC OS writes them, `0xhex`, or plain decimal. Blank lines and
lines beginning with `#` are ignored, and so is any line that is not a
number-and-name pair - a file that is half wrong loads the half that is right
rather than failing altogether.

Give the **chunk base**, as the module header declares it. The X bit is ignored
when matching, so `&42C40` in the file also names `&62C40` in the code, and the
`X` is still printed from the opcode: the line reads `SWI XMyModule_Doit`. Listing
both forms would double the file for no information.

Names from a file are searched before the built-in table, so an OS SWI can be
renamed as well as a new one added.

Load them with the **SWI names...** button beside the disassembly view, or with
`swi load <path>` on the [debug socket](debugcmd.md); `swi clear` forgets them.
A saved inspector session records the path and reloads it - see
[debugger-tracing.md](debugger-tracing.md).

## Testing

`tests/test_arm_disasm.c` covers both entry points: a sample of every encoding
class as a regression lock on the text, and the `ArmInsnInfo` fields — where a
mistake no longer shows up as an odd-looking line but as a debugger that runs
away when asked to step.

The FPA encodings there were hand-assembled from the operation tables in
`fpa.c`, so that test is also the written record of what those bit patterns
mean.
