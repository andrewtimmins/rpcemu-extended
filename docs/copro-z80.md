# Z80 and 8080 co-processor

Zilog's Z80 and Intel's 8080, as processors for the OPEN Bus co-processor card.
Fit either with `--openbus-card=z80` or `--openbus-card=8080`, or from the machine
editor's **Co-Processor Card** tab.

The card itself, its register map, the `RPCEmuCoPro` module and the SWIs are in
[openbus.md](openbus.md). Source is `src/copro/cpu_z80.c`, tested by
`tests/test_cpu_z80.c`.

## One core, two parts

The 8080 is the same source with a flag set. They share their instruction set
almost entirely, the Z80 having been designed as a superset.

★ **But an 8080 is not a Z80 with instructions removed, and calling it one was
wrong.** The instruction overlap is not the cost of supporting it; **the flag
semantics are**. See below - it is the kind of difference that makes software
appear to work and produce wrong answers.

## What is implemented

The documented instruction set: the main page, `CB`, `ED` including the block
instructions, and the `DD`/`FD`/`DDCB`/`FDCB` index-prefixed pages. A separate
64K input/output space, reached through the same address decode as memory.

## What is not

Undocumented opcodes fault, along with `SLL` and the `IXH`/`IXL` register halves.

## Where the 8080 differs, and it is the flags

Three things, and the first is the one that matters:

- ★ **The parity/overflow bit is always parity.** The Z80 redefined that bit as
  overflow for arithmetic and kept parity for logic; on an 8080 it is parity for
  everything. So every add, subtract, increment and decrement reports something
  different on the two parts, and software that tests it after arithmetic behaves
  differently on each.
- **`DAA` always adds**, there being no subtract flag to consult.
- The flag register reads **bit 1 as one and bits 3 and 5 as zero**, which
  anything doing `PUSH PSW` can see.

Both live in one choke point, `store_flags()`, which every ALU helper routes
through.

**The Z80's own instructions fault on an 8080**: the `CB`, `DD`, `ED` and `FD`
prefixes, `JR`, `DJNZ`, `EXX` and `EX AF,AF'`. A real 8080 does something with
those encodings - mostly an undocumented no-op or an alias - and reproducing that
would be modelling a defect nobody should rely on. Faulting says "this is not an
8080 program", which is the useful answer.

## How a program stops

`HALT` halts the core, with **A as the exit code**. `HLT` on the 8080 is the same
opcode and does the same thing.

## Interrupts

The maskable interrupt with all three modes, and the non-maskable one. `IRQCTRL`'s
vector-byte field is what mode 2 needs.

★ **A repeating instruction is interruptible.** `LDIR` and its relatives step the
program counter back over their own two bytes when they have more to do, which is
what the hardware does - and it matters here for a reason the hardware did not
care about. A card is given a bounded slice of the host's cycles, so a 64K block
move is spread across slices instead of stalling the machine for the whole of it.

## Card RAM

| | |
| --- | --- |
| Default | 64K |
| Maximum | 16MB |
| Flat limit | 64K |

64K is the whole of what the processor can name. The card may carry up to 16MB
because banking reaches it - a Spectrum 128 had more memory than the processor
could address and paged it through a window, and the address map does the same
with `COPRO_REGION_OFFSET`.

## Cycles

Real T-states, per the documented timing: the extra cycle an indexed read pays for
its displacement, the prefix costs, and 21 rather than 16 for an iteration of
`LDIR` that has more to do. **A Spectrum frame is 69888 T-states**, which is what
`CoPro_RunFor` is for.

The 8080 differs on about ninety opcodes. Those follow a handful of rules and were
derived from them rather than transcribed, then each entry checked to actually
differ from the Z80's value - a "delta" that matches is not a delta, and being in
the list is a mistake.

**Memory contention is not modelled.** A Spectrum's ULA steals cycles from the Z80
while it is drawing, which is what makes some border effects work, and nothing
here reproduces that.

## Registers through `REGSEL`

0 to 7 are A, F, B, C, D, E, H and L; 8 and 9 are IX and IY; 10 is the stack
pointer and 11 the program counter; 12 is I, 13 the interrupt mode and 14 the
interrupt enable.
