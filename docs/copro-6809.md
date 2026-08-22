# 6809 co-processor

The Motorola 6809, as a processor for the OPEN Bus co-processor card. Fit it with
`--openbus-card=6809`, or from the machine editor's **Co-Processor Card** tab.
It unlocks the Dragon 32 and 64, the Tandy CoCo and the Vectrex.

The card itself, its register map, the `RPCEmuCoPro` module and the SWIs are in
[openbus.md](openbus.md). Source is `src/copro/cpu_6809.c`, tested by
`tests/test_cpu_6809.c`.

## Why it is the largest of the 8-bit cores

Not the instruction count. Its opcode map is more regular than the 6502's or the
Z80's - Motorola laid it out as a table on purpose, so the position of an opcode
tells you what it does. **The cost is the addressing.**

One of its four addressing modes is "indexed", and indexed on a 6809 is a whole
addressing language in a postbyte: a constant offset of five, eight or sixteen
bits from any of four registers, an offset taken from A, B or D, auto-increment
and auto-decrement by one or two, program-counter relative, and an indirect form
of nearly all of them. Fourteen forms wearing one opcode.

## What is implemented

The documented instruction set, all three pages, and every indexed addressing form
the postbyte encodes. Two accumulators and the 16-bit D they form, X and Y, both
stack pointers, the direct-page register, and all three interrupt lines with their
own vectors and their own idea of how much state to push.

## What is not

- Undocumented opcodes fault.
- ★ **The postbyte forms that do not exist fault too.** There is no indirect
  `,R+` or `,-R`, because incrementing by one and then reading a 16-bit pointer
  would read half of one pointer and half of the next. A core that invented a
  meaning for those would make a program look portable when it is not.
- ★ **`SYNC` and `CWAI` fault**, and this is worth saying plainly rather than
  leaving to be discovered. Both stop the processor until an interrupt arrives,
  which is a state this card does not model - exactly as `WAI` is left out of the
  65C02. **Software written for a Dragon or a CoCo that idles in `SYNC` will stop
  on it.** The 68000's `STOP` will not have this limitation, because that core is
  being built with a full interrupt model; if `SYNC` turns out to matter in
  practice it is worth revisiting on the same basis.

## How a program stops

`SWI` halts the core, with **A as the exit code** - the same bargain `BRK` makes
on the 6502.

`SWI2` and `SWI3` do vector, because those are the two a guest operating system
would claim and a program calling one expects to come back from it. Neither sets
an interrupt mask, for the same reason.

## Interrupts

Three lines, and how much state each pushes is the thing to know.

| | Pushes | Vector | Masked by |
| --- | --- | --- | --- |
| `IRQ` | Everything, twelve bytes | `&FFF8` | I |
| `FIRQ` | The program counter and condition codes only, three bytes | `&FFF6` | F |
| `NMI` | Everything | `&FFFC` | nothing |

★ **`FIRQ` is fast because of what it does not push**, and it records that by
leaving the E flag clear. `RTI` reads E to know how much to take back off the
stack, so a handler that pushes more itself and does not restore it will return to
the wrong place.

Both `FIRQ` and `NMI` mask both lines, not just their own: a fast handler is not
meant to be interrupted by the slow one either.

`IRQCTRL`'s level field selects between them - **level 0 is `IRQ` and level 1 is
`FIRQ`**. The difference is not speed but how much state each pushes, so a handler
written for one cannot serve the other, and a guest must mean the one it asks for.

## Card RAM

| | |
| --- | --- |
| Default | 64K |
| Maximum | 16MB |
| Flat limit | 64K |

## Cycles

Real cycles per the documented timing, and on this processor the opcode alone does
not decide them:

- **An indexed instruction's cost is its opcode plus what the postbyte charged.**
  The same `LDA` costs anything from four cycles to eleven. Indirection adds three
  to whatever the form already cost.
- **A push or pull costs five plus one per byte** its register list moves, so the
  register list is part of the price.
- **`RTI` costs six or fifteen**, decided by the E flag it has just pulled.
- A short branch costs the same taken or not; **a long conditional branch taken
  costs one more.**

Note that an indexed instruction here costs **less** than the extended form of the
same thing, and on the 6800 it costs **more**. Neither is a mistake in the table.

The timings are checked against published values in `tests/test_cpu_cycles.c`,
including every postbyte's own cost, and the pair that catches a postbyte decoded
as the wrong half of its encoding: an explicit zero offset in five bits costs one
more than a bare `,X`, because they are different forms and only one of them has
an offset to add.

## Registers through `REGSEL`

0 is A and 1 is B, then 2 to 5 are X, Y, U and S, 6 is the direct page, 7 the
condition codes and 8 the program counter.

D is not there, because it is not a register - it is A and B side by side, and a
guest that wants it reads both.

## Big-endian

The high byte of a 16-bit quantity sits at the lower address, in memory and on the
stack alike. The 6800 and the 68000 agree; the 6502, the Z80 and RV32IM do the
opposite.
