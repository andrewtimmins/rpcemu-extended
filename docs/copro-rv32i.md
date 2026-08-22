# RV32IM co-processor

The 32-bit RISC-V base integer instruction set plus the M extension for multiply
and divide, as a processor for the OPEN Bus co-processor card. Fit it with
`--openbus-card=rv32i`, or from the machine editor's **Co-Processor Card** tab.

The card itself, its register map, the `RPCEmuCoPro` module and the SWIs are in
[openbus.md](openbus.md). Source is `src/copro/cpu_rv32i.c`, tested by
`tests/test_cpu_rv32i.c`.

## Why this one exists

It was the first, and it was chosen over a second ARM deliberately.

A second ARM sounds like the free option and is the most expensive one here.
`ARMState` is a single global and the recompiler's state is global with it, so a
second ARM means making the whole core re-entrant - and the OPEN Bus serialises
the bus by design, so that work would buy no concurrency at the end of it. A
processor we have never implemented brings its own small struct and collides with
nothing.

RISC-V in particular has under fifty instructions in its base set, and **the
payload is a file we compile and ship ourselves** rather than somebody else's ROM
image. Every other choice of processor needs software that belongs to someone
else; this one needs `gcc -march=rv32im`.

## What is implemented

The whole of RV32I and the whole of RV32M, user mode, little-endian, with strict
natural alignment on loads and stores.

## What is not

Said plainly rather than left to be discovered:

- No privileged architecture and no CSRs, so no Zicsr - `csrrw` is an illegal
  instruction.
- No A, F, D or C extensions.
- No interrupts into the core. It is the one processor on this card that cannot
  accept one, and `IRQCTRL` says so rather than silently doing nothing.
- No memory ordering to get wrong. `FENCE` executes as a no-op, which is a
  correct implementation on a core that completes every access before it starts
  the next instruction.

Compile with `-march=rv32im -mabi=ilp32` and nothing here will surprise you.

## How a program stops

`ecall` and `ebreak` both halt the core rather than trapping, because there is no
machine mode here to trap to. **a0 (x10) is the exit code**, which is the calling
convention a C `main` already uses, and the card's status says which of the two it
was.

The card raises its interrupt when the core halts, so a guest learns of it
without polling.

## Card RAM

| | |
| --- | --- |
| Default | 1MB |
| Maximum | 64MB |
| Flat limit | 64MB - it has a 32-bit address space, so no paging is needed |

Unlike the 8-bit cores, the ceiling here is a judgement rather than a count of
address lines: 64MB is more than any program written for this card will want, and
still a fraction of what the host has.

## Cycles

★ **This core counts instructions, not cycles, and that is deliberate.** RV32
has no canonical timing to be accurate to - it is an instruction set
specification, not a part with a datasheet - so a cycle table here would be a
number invented to look precise. Nothing being reproduced on this core depends
on one.

Every other core on this card charges its processor's real documented cycles. If
you are pacing something, use one of those.

## Registers through `REGSEL`

0 to 31 are x0 to x31, and 32 is the program counter.

x0 is hardwired to zero and stays that way: a guest writing it would break every
instruction that reads zero from it, so the write is ignored rather than obeyed.
