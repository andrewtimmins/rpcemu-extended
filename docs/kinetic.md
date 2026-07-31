# Kinetic StrongARM

The Acorn Risc PC **Kinetic** was a StrongARM processor card that carried its own
memory. Instead of being limited to the 256MB the motherboard's IOMD can address,
a Kinetic machine has two 128MB SDRAM banks on the card itself, giving **512MB**.
RPCEmu emulates it, and RISC OS 5 boots to the desktop on it.

Choose **Risc PC - Kinetic** as the model in *Settings → Machine…*. RAM is then
fixed at 512MB and VRAM at 2MB, both for reasons below, and the dialogue says so
rather than just greying them out.

**64-bit hosts only.** 512MB of guest RAM plus a code cache does not fit in a
32-bit address space alongside everything else.

## How this was worked out

RISC OS Open publish the sources this behaviour lives in: `HAL_IOMD`, which is the
Risc PC and Kinetic HAL, and the `Kernel`. Those were read as a **reference for
how the hardware behaves**, and the emulation was then written from scratch. None
of their code is here, and none of it can be: it is Apache 2.0 licensed and RPCEmu
is GPL v2, so copying it in would not be permissible even if it were desirable.
What was taken is knowledge of what the machine does, which is the same thing
anyone writing a driver from a datasheet takes.

That mattered here because almost nothing about a Kinetic is discoverable by
guessing. The important findings were these.

## Kinetic detection is behavioural, not an identity register

There is no chip ID to answer. The HAL decides whether it is running on a Kinetic
by **probing for memory**: it writes a pattern at 0x20000000 and 0x30000000, reads
it back, and walks the address lines to work out how much is decoded.

So the emulation cannot simply present 128MB at each base. The probe toggles high
address lines that a real bank does not decode, and expects the access to fold
back into the bank. Both banks are therefore decoded across their **full aliasing
window**, 0x20000000 to 0x2fffffff and 0x30000000 to 0x3fffffff, with the bank
address masked to 0x07ffffff. Decode only the exact 128MB and the probe fails, and
RISC OS quietly concludes it is on an ordinary Risc PC.

The same aliasing is what makes two other addresses work without special cases:
the cache cleaner at 0xE8000000 and the RAM steering register at 0xEC000000 fold
into the gaps after each bank once 30 address bits are decoded.

## Two registers have to behave

**`phys_space_mask` becomes 0x3fffffff.** An ordinary Risc PC connects 29 address
lines to IOMD, giving a 512MB physical map, and the Kinetic's SDRAM sits above
that. Thirty bits are needed, which `Model_Phoebe` already used. Leave it at
0x1fffffff and 0x20000000 aliases onto the ROM, so the machine appears to have no
SDRAM and, worse, executes ROM when it means to read memory.

**IOMD Sound DMA 0 EndA (offset 0x184) must round-trip.** The HAL borrows this
register as scratch: it stashes the result of the SDRAM probe there and reads it
back later to set its own `IsKinetic` flag. Every other sound DMA address register
in RPCEmu reads as zero, which is fine for them and fatal for this one, so on a
Kinetic it returns the value last written.

## A new memory region needs wiring into three separate paths

This is the part worth remembering, because it is not obvious and it cost the most
time. Adding RAM to the emulator is not one change. It is three, and each is
independently capable of silently returning the wrong memory:

1. **Data read and write** - the five physical address switches in `mem.c`, plus
   the four dynarec fast paths.
2. **The MMU page-table walk** - `cp15.c` decodes the translation table base to
   pick the bank holding the page tables. On a 512MB machine RISC OS puts them
   near the top of SDRAM bank 1, so a decode that does not know about SDRAM reads
   every first-level descriptor from the wrong bank.
3. **Instruction fetch** - `getpccache()` in `cp15.c`, which is a different decode
   again.

The last was the bug that kept the machine off the desktop longest, and the reason
it bites specifically on a Kinetic is this: **the HAL relocates the operating
system to the top of RAM when it finds SDRAM.** That is also why a Kinetic
configured with 256MB boots fine while 512MB did not. The relocated ROM runs from
around 0x37fxxxxx, `getpccache()` masked the address with 0x1f000000, so 0x37... was
decoded as though it were motherboard DRAM, and the emulator fetched zeros and ran
off through empty memory. All three decodes now mask with 0x3f000000 and know both
SDRAM banks.

## Why VRAM is fixed at 2MB

Because 512MB of RAM and 16MB of VRAM together do not fit in the RISC OS memory
map. That is a limit in the operating system, not a defect in the emulator or in
any particular ROM build, and it is the reason this is settled rather than
outstanding: a Kinetic is defined by its 512MB, so on this machine the VRAM is
what has to give. The clamp is applied in three places so no route round it
exists: when the GUI applies new settings, when a configuration file is loaded,
and in the machine editor, which also locks the combo box.

The symptom is a data abort during boot. It survives long enough to reach the
supervisor prompt, which makes it easy to mistake for working, and then fails once
a full `!Boot` brings up the desktop and networking, which is where enough of the
map is in use for the overrun to matter. Anything tested against this limit has to
be tested with a complete desktop boot; a prompt is not evidence.

The ceiling this used to imply for the display does not apply, because the
[graphics card](gfxcard.md) carries its own 15MB of display memory, is not limited
by the fitted VRAM, and is not bound by the OS memory map at all. That is the
supported route to high resolutions on a Kinetic.

Two records exist of attempts to lift the limit by patching the ROM, and neither
should be treated as a starting point. The patch and its anchor sequences are in
the message of commit 9b4dd61. They were revisited on 31/07/2026 and applied
successfully to RISC OS 5.31 once two anchors were repaired, only one of the three
having matched as recorded: `MOVEQ ip,#8` occurs three times in that build so it
cannot anchor anything on its own, and the gap-fill `BL` is three words after the
`RSB` it is located from, not two. The patched machine still aborted, which is
what established the limit above. The ROM detail is nonetheless correct and worth
not re-deriving: the HAL guards its VRAM region fill with `MOVNE`/`LDRNE`/`BLNE`
but leaves the following gap fill unguarded, so at exactly 16MB, where
`RSB r3,r3,#0x1000000` leaves zero and the count is therefore zero, the do-while
runs the fill anyway. Only 16MB degenerates that way, so 4MB and 8MB were never
affected by that particular fault, though they remain clamped along with
everything else.

## What a working Kinetic looks like

RISC OS reports **514MB** at boot, being 512MB of RAM plus the 2MB of VRAM, and
the bank table shows four 64MB motherboard banks and two 128MB SDRAM banks. If it
reports 256MB instead, SDRAM detection has failed and the machine has fallen back
to behaving as an ordinary Risc PC, which is the failure to expect from any change
to the memory decode.
