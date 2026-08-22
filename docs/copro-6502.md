# 6502 and 65C02 co-processor

The MOS 6502 and WDC's CMOS 65C02, as processors for the OPEN Bus co-processor
card. Fit either with `--openbus-card=6502` or `--openbus-card=65c02`, or from the
machine editor's **Co-Processor Card** tab.

The card itself, its register map, the `RPCEmuCoPro` module and the SWIs are in
[openbus.md](openbus.md). Source is `src/copro/cpu_6502.c`, tested by
`tests/test_cpu_6502.c`.

There is a pleasing symmetry in a Risc PC hosting one of these, since the
machine's ancestry runs back through the Electron and the BBC Micro to this
processor - and the second-processor idea itself is the BBC's Tube.

## One core, two parts

The 65C02 is the same source with a flag set, not a second file. The two share
every one of the 151 documented NMOS opcodes, and a copy would be seven hundred
lines that have to be fixed twice.

## What is implemented

All 151 documented opcodes, thirteen addressing modes, binary and decimal
arithmetic, and **the NMOS indirect-`JMP` page-crossing bug**, which is emulated
on purpose. A 6502 that gets that wrong is not the processor anybody is nostalgic
about.

The 65C02 adds `BRA`, `PHX`/`PHY`/`PLX`/`PLY`, `STZ`, `TRB`/`TSB`, `INC A` and
`DEC A`, `BIT` with immediate and indexed addressing, the `(zp)` indirect forms of
the ALU operations, `JMP (abs,X)` and `STP`. It also **fixes** two things: the
indirect-`JMP` page bug is gone, as the real part fixed it, and N and Z are
meaningful after decimal arithmetic.

## What is not

- The undocumented opcodes - `SLO`, `RLA`, `LAX` and the rest - fault as illegal
  rather than doing what a real NMOS part happened to do.
- On the 65C02, **Rockwell's `RMB`/`SMB`/`BBR`/`BBS`**, which are an R65C02
  extension rather than part of the CMOS part every 65C02 has.
- `WAI`, which waits for an interrupt - a state this card does not model. It
  faults rather than doing something plausible. The 6809's `SYNC` and the 6800's
  `WAI` are left out for the same reason; the 68000's `STOP` will not be, because
  that core is being built with a full interrupt model.

A 65C02-only opcode **faults on a 6502**, which is checked. A core that quietly
executed one would make software look portable when it is not.

## How a program stops

`BRK` halts the core, with **the accumulator as the exit code**.

On real hardware `BRK` pushes a frame and vectors through `$FFFE`, but there is no
operating system here to vector to, and "stop, and hand back the accumulator" is
what a program on a co-processor card wants to do at its end. On a 65C02, `STP`
halts as well.

`RTI` exists but has nothing to return from unless a program has pushed a frame
itself.

## Interrupts

Both lines. `IRQ` is ignored while the I flag is set, as the hardware ignores it,
and vectors through `$FFFE`; `NMI` is never ignored and vectors through `$FFFA`.
The break flag is pushed **clear** by an interrupt and set by `BRK`, which is how
a handler tells them apart.

## Card RAM

| | |
| --- | --- |
| Default | 64K |
| Maximum | 16MB |
| Flat limit | 64K |

64K is not a default, it is the whole of what the processor can name - no 6502
instruction can form an address above `$FFFF`.

The card may still carry up to 16MB, because **banking reaches it**: a BBC's
sideways ROMs and a C64's REU both had more memory than the processor could
address and paged it through a window, and the address map does the same with
`COPRO_REGION_OFFSET`. 64K is visible at a time and the guest chooses which 64K.

## Cycles

Real per-instruction cycles, per the documented NMOS timing: the extra cycle an
indexed read pays when the index carries into the high byte of the address, and
the cost of a taken branch, and one more again if it crossed a page.

A 65C02 differs on some opcodes and pays one more for decimal arithmetic, which
depends on the D flag rather than on the opcode and so cannot live in a table.

The timings are checked against published values in `tests/test_cpu_cycles.c`,
both polarities of every conditional.

## Registers through `REGSEL`

0 is A, 1 is X, 2 is Y, 3 is the stack pointer, 4 is the status register and 5 is
the program counter.
