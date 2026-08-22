# 6800, 6802 and 6808 co-processor

The Motorola 6800, as a processor for the OPEN Bus co-processor card. Fit it with
`--openbus-card=6800`, or from the machine editor's **Co-Processor Card** tab.

The card itself, its register map, the `RPCEmuCoPro` module and the SWIs are in
[openbus.md](openbus.md). Source is `src/copro/cpu_6800.c`, tested by
`tests/test_cpu_6800.c`.

## One core covers three parts

★ **The 6802 and the 6808 are this same instruction set**, and that is a fact
about the hardware rather than a shortcut. The 6802 is a 6800 with 128 bytes of
RAM and a clock oscillator on the chip; the 6808 is a 6802 without the RAM.
Neither added an instruction, a register or an addressing mode, so to a program
there is nothing to tell apart - and a card here carries its own RAM anyway, so
even the 6802's on-chip RAM is not a difference this can express.

Hence one core, and one entry in the machine editor rather than three that do the
same thing.

## Why it is not a flag on the 6809

The 65C02 is a flag on the 6502 core and the 8080 is a flag on the Z80 core, so
this looks like it should be a flag on the 6809. It is not.

**The 6809 was source compatible with the 6800, not object-code compatible.** An
assembler would take 6800 source and produce 6809 binaries, but the two disagree
about most of the map:

| Opcode | On a 6800 | On a 6809 |
| --- | --- | --- |
| `&08` | `INX` | `ASL` direct |
| `&30` | `TSX` | `LEAX` |
| `&36` | `PSHA` | `PSHU` |
| `&8E` | `LDS` | `LDX` |
| `&CE` | `LDX` | `LDU` |

A flag that changed what nearly every opcode meant would not be a flag, it would
be a second core wearing one. There are structural differences behind those too:
no direct-page register, so direct addressing is page zero and nowhere else; one
indexed form with an unsigned 8-bit offset, where the 6809 has fourteen forms in a
postbyte; one stack, one index register, and no 16-bit accumulator.

`tests/test_cpu_6800.c` checks those five opcodes specifically, so nobody later
decides the two can share a decoder after all.

## What is implemented

The 197 valid opcodes, all four addressing modes, both accumulators, and the
interrupts with their own vectors.

## What is not

Undocumented opcodes fault. **`WAI` faults**, for the reason `SYNC` does on the
6809: it stops the processor until an interrupt arrives, which is a state this card
does not model.

There is no `BRN`. `&21` is not an instruction - the always-fail branch was a 6809
addition, and asking for it here faults.

## ★ The quirks, which are the reason to model this part

A 6800 that gets these wrong is not the processor anybody's software was written
against, and every one of them would pass a test that only looked at results.

- **`CPX` sets N, Z and V and leaves the carry alone.** This makes an unsigned
  16-bit comparison genuinely awkward and is the single most complained-about
  thing about the part. The 6809 fixed it.
- **`INX` and `DEX` affect Z and nothing else.** No N, no V. So a loop counting an
  index register down and branching on `BPL` never ends on this processor, and one
  branching on `BNE` does.
- **`TST` clears the carry**, where the 6809's leaves it alone.
- **The shifts and rotates define V as N exclusive-or C** after the operation. For
  `ASL` that agrees with the 6809 by arithmetic, and for `LSR` it does not agree at
  all: N is always clear afterwards, so V follows the bit shifted out, where a 6809
  leaves V untouched entirely.
- **The stack pointer addresses the next free byte**, one below the top of the
  stack, so a push stores and *then* decrements. The off-by-one is visible in the
  instruction set: `TSX` loads X with the pointer plus one, and `TXS` sets it to X
  minus one.
- The top two bits of the condition-code register do not exist and **read as
  ones**, which anything doing `TPA` or examining a pushed frame can see.

## How a program stops

`SWI` halts the core, with **A as the exit code**, as `BRK` does on the 6502 and
`SWI` on the 6809. Unlike the 6809 there is no second or third software interrupt
to leave vectoring, so this is the only one.

## Interrupts

Two lines, one frame size. `IRQ` is masked by the I flag and vectors through
`&FFF8`; `NMI` is never masked and vectors through `&FFFC`. Both push seven bytes -
the program counter, the index register, both accumulators and the condition codes.

There is no shorter frame here. The 6809's fast interrupt, which pushes three
bytes, was an addition and this part has no equivalent, so `IRQCTRL`'s level field
means nothing to it.

## Card RAM

| | |
| --- | --- |
| Default | 64K |
| Maximum | 16MB |
| Flat limit | 64K |

## Cycles

Real cycles per the documented timing, and the table is the whole answer - unlike
the 6809, nothing here needs adjusting at run time. Branches cost the same taken
or not, indexed addressing has one form with one price, and the stack instructions
move one register each.

★ **Its one surprise is real: an indexed instruction costs MORE than the extended
form of the same thing.** The offset has to be added to X, where a full address
needs no work. On the 6809 it is the other way round, and a table copied from one
to the other would be wrong for both.

The cycle table was derived from the group rules rather than transcribed, then
cross-checked against 65 values read off the datasheet independently. It came out
with exactly the 197 valid opcodes the part is documented to have, which is a
second check on the same work.

## Registers through `REGSEL`

0 is A and 1 is B, then 2 is X, 3 the stack pointer, 4 the condition codes and 5
the program counter.

Its numbering is the 6809's with what it does not have taken out. Keeping the
shared registers at the same numbers would mean leaving holes, and a hole a guest
reads as all-ones is worse than a shorter list.

## Big-endian

The high byte of a 16-bit quantity sits at the lower address. The 6809 and the
68000 agree.
