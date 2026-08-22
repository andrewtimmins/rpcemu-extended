/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef OPENBUS_COPROC_H
#define OPENBUS_COPROC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A co-processor card for the OPEN Bus second processor slot, with a choice of
 * processor: RV32IM, 6502 or Z80.
 *
 * ★ THIS IS ONE CARD DESIGN, NOT THREE. openbus.h is emphatic that the register
 * window is product specific and that generalising it would be a mistake, and
 * that stands - what it warns against is inventing a layout meant to cover cards
 * other people designed. This layout covers cards WE designed, so the three
 * share it, and the CORE register says which processor is fitted. One register
 * map means one guest module drives all three, which is the whole reason to do
 * it that way.
 *
 * No such card was ever made. Only two designs ever used the OPEN Bus - Aleph
 * One's x86 PC card and Simtec's Hydra - and this is neither of them; the ID
 * register says so.
 *
 * ★ THE CORE HAS ITS OWN RAM, and that is a deliberate design choice rather than
 * a shortcut. A real second-processor card carried its own memory: Aleph One's
 * PC card took a SIMM of its own, and the Tube's second processors had theirs.
 * Doing the same here buys three things. The core's whole address space is a
 * simple array, so each processor stays a processor and knows nothing about a
 * host. A 6502 or a Z80 gets the flat 64K it expects rather than a window into
 * somebody else's 256MB. And the guest can load a program with ordinary loads
 * and stores through the aperture below, without ever needing a PHYSICAL
 * address - which on RISC OS means no dynamic area, no page translation and no
 * contiguity to arrange, so the module that drives this is a few hundred bytes
 * rather than an exercise in memory management.
 *
 * Bus mastering is still here, because a card that cannot reach host memory is a
 * podule and not a second bus master: the DMA registers copy between host
 * physical memory and card RAM, on the card's own timeslice, exactly as the stub
 * card's block copy does. See the note on the guest module in docs/openbus.md
 * for which path that module uses and why.
 *
 * ★ CYCLES ARE THE PROCESSOR'S OWN CYCLES on the 6502 and the Z80, and
 * instructions on RV32IM.
 *
 * The 8-bit cores charge each instruction its documented timing, including the
 * extra cycle an indexed read pays when it carries into the high byte, the cost
 * of a taken branch, and the Z80's prefix and page costs. That is what makes a
 * machine emulated on this card able to pace a display and a sound chip: a PAL
 * C64 raster line is 63 cycles and a Spectrum frame is 69888 T-states, and no
 * count of instructions will say where in a frame you are.
 *
 * RV32IM keeps one per instruction, and deliberately: there is no canonical
 * RV32 timing to be accurate to - it depends entirely on the implementation -
 * so a table here would be a number invented to look precise. Nothing
 * period-accurate depends on it, because no RV32 machine is being reproduced.
 *
 * ★ AN INSTRUCTION IS INDIVISIBLE, so a run reaches its budget and then
 * overshoots by whatever the last instruction cost. RUNFOR of 69888 will stop at
 * 69888 or a few cycles past it, never before, and the guest should carry the
 * difference into its next frame rather than assume it got exactly what it asked
 * for.
 *
 * What is NOT modelled: memory contention. A Spectrum's ULA steals cycles from
 * the Z80 when it is drawing, which is what makes some border effects work, and
 * nothing here reproduces that.
 *
 * Register map, at OPENBUS_REG_BASE. Word registers; a byte or halfword access
 * reads or writes the addressed part of the word, little-endian, as the bus does.
 *
 *   0x00 ID         R   0x4f424350 ('OBCP'). This card, whatever core.
 *   0x04 CORE       R   0x52563332 ('RV32'), 0x36353032 ('6502') or
 *                       0x5a383020 ('Z80 ').
 *   0x08 RAMSIZE    R   bytes of card RAM.
 *   0x0c CTRL       RW  bit 0  RUN: the core executes when given a timeslice
 *                       bit 1  STEP: execute one instruction (self-clearing)
 *                       bit 2  RESET: reset the core (self-clearing)
 *                       bit 3  IRQ_ON_HALT: raise nPIRQ when the core stops
 *   0x10 STATUS     R   bit 0  RUNNING
 *                       bit 1  HALTED (the core executed its stop instruction)
 *                       bit 2  FAULT
 *                       bit 3  nPIRQ asserted by this card
 *                       bit 4  DMA in progress
 *   0x14 ENTRY      RW  address in card RAM the core starts at on reset.
 *   0x18 ADDR       RW  aperture address into card RAM.
 *   0x1c DATA       RW  the bytes at ADDR. ADDR advances by the access width, so
 *                       a program is loaded by setting ADDR once and then
 *                       writing DATA repeatedly.
 *   0x20 MBOX_TX    RW  guest to core. See the asymmetry note below.
 *   0x24 MBOX_RX    R   core to guest. Set to the exit code when the core halts.
 *   0x28 CYCLES     R   cycles since reset, low 32 bits (instructions on RV32IM;
 *                       see the note below).
 *   0x2c PC         R   the core's program counter.
 *   0x30 FAULT      R   fault cause, core specific; 0 when STATUS says no fault.
 *   0x34 FAULTADDR  R   the address or opcode that faulted.
 *   0x38 IRQCLEAR   W   any value drops nPIRQ.
 *   0x3c DMAHOST    RW  host PHYSICAL address for a bus-master copy.
 *   0x40 DMALOCAL   RW  card RAM address for the same.
 *   0x44 DMALEN     RW  length in 32-bit words, counted down as it runs.
 *   0x48 DMACTRL    RW  bit 0  START (self-clearing)
 *                       bit 1  direction: 0 host to card, 1 card to host
 *                       bit 2  the CONTROL AREA rather than card RAM
 *
 * ★ THE REGISTERS BELOW ARE WHAT AN EMULATOR IS WRITTEN AGAINST. Everything
 * above is enough to load a program and run it to completion; none of it is
 * enough to emulate a machine, which needs the guest to say what the address
 * space means, to be told what the program wrote, to present what it reads, to
 * interrupt it and to run it for exactly one frame at a time.
 *
 *   0x4c RUNFOR     RW  cycles a run may use before stopping. Zero means "until
 *                       the core stops by itself", which is what the card did
 *                       before this existed.
 *   0x50 STOPREASON R   why the last run ended: OPENBUS_COPROC_STOP_*.
 *   0x54 IRQCTRL    RW  bit 0     assert the core's maskable interrupt
 *                       bit 1     assert its non-maskable interrupt
 *                       bits 8-10 interrupt level, for a core that has them.
 *                       On a 6809, which has two maskable lines rather than
 *                       one, level 0 is IRQ and level 1 is the FAST interrupt;
 *                       the difference is not speed but how much state it
 *                       pushes, so a guest must mean the one it asks for.
 *                       bits 16-23 vector byte, for a Z80 in mode 2
 *   0x58 MAPOFF     RW  offset in the control area of the region table
 *   0x5c MAPCOUNT   RW  how many entries it has; zero means no map, and then
 *                       every address is plain card RAM
 *   0x60 LOGOFF     RW  offset in the control area of the write log ring
 *   0x64 LOGENTRIES RW  how many entries it holds; zero stops writes being
 *                       logged at all
 *   0x68 LOGHEAD    R   where the card will write the next entry
 *   0x6c LOGTAIL    RW  where the guest has read up to; it advances this
 *   0x70 WAITADDR   R   the address of an access waiting to be answered
 *   0x74 WAITDATA   RW  the value a waiting WRITE carried, or the value the
 *                       guest supplies to a waiting READ
 *   0x78 WAITINFO   R   bit 0 the waiting access is a write
 *                       bit 1 it is in the port space
 *   0x7c WAITACK    W   any value answers the waiting access and lets the core
 *                       continue
 *   0x80 CTLADDR    RW  aperture into the control area
 *   0x84 CTLDATA    RW  the bytes at CTLADDR, which advances as DATA does
 *   0x88 CTLSIZE    R   bytes of control area
 *   0x8c REGSEL     RW  which of the core's own registers REGDATA reaches
 *   0x90 REGDATA    RW  that register's value
 *
 * ★ REGSEL IS PER CORE, and deliberately not a common numbering. A 6502 has
 * five registers and a Z80 has twenty-odd; inventing an order that covered both
 * would mean a guest looking up which of its processor's registers RPCEmu decided
 * was number four. The numbering is the core's own, listed in each core's header,
 * and REGSEL out of range reads as 0xffffffff. This exists for a debugger and for
 * save states: a program does not need it.
 *
 * ★ WHY A SEPARATE CONTROL AREA. The region table, the write log and the latch
 * pages have to live somewhere the CARD can reach, and card RAM is the core's
 * address space: on a 6502 or a Z80 that is the 64K the machine being emulated
 * needs all of. So the card carries its own memory for them, which the core
 * cannot see and cannot corrupt, reached through its own aperture.
 *
 * Anything else reads 0xffffffff, as an undriven bus does.
 *
 * ★ THE MAILBOX IS ASYMMETRIC, on purpose. MBOX_RX carries the exit code from
 * every core, because every core has a stop instruction and every stop hands
 * back an accumulator or an a0. MBOX_TX can only be READ by a core that has an
 * input space to read it through, which of these three means the Z80 alone:
 * `IN A,(0)` returns its low byte and `OUT (0),A` posts a byte to MBOX_RX while
 * the core is still running. An RV32I or 6502 program takes its parameters in
 * card RAM, which is where a program of any size would want them anyway. Saying
 * that plainly beats inventing a channel two of the three cores cannot use.
 */

/** Register offsets from OPENBUS_REG_BASE. */
#define OPENBUS_COPROC_REG_ID		0x00
#define OPENBUS_COPROC_REG_CORE		0x04
#define OPENBUS_COPROC_REG_RAMSIZE	0x08
#define OPENBUS_COPROC_REG_CTRL		0x0c
#define OPENBUS_COPROC_REG_STATUS	0x10
#define OPENBUS_COPROC_REG_ENTRY	0x14
#define OPENBUS_COPROC_REG_ADDR		0x18
#define OPENBUS_COPROC_REG_DATA		0x1c
#define OPENBUS_COPROC_REG_MBOX_TX	0x20
#define OPENBUS_COPROC_REG_MBOX_RX	0x24
#define OPENBUS_COPROC_REG_CYCLES	0x28
#define OPENBUS_COPROC_REG_PC		0x2c
#define OPENBUS_COPROC_REG_FAULT	0x30
#define OPENBUS_COPROC_REG_FAULTADDR	0x34
#define OPENBUS_COPROC_REG_IRQCLEAR	0x38
#define OPENBUS_COPROC_REG_DMAHOST	0x3c
#define OPENBUS_COPROC_REG_DMALOCAL	0x40
#define OPENBUS_COPROC_REG_DMALEN	0x44
#define OPENBUS_COPROC_REG_DMACTRL	0x48

/** Identifiers. Four characters each, so a *command can print them. */
#define OPENBUS_COPROC_ID		0x4f424350u	/* 'OBCP' */
#define OPENBUS_COPROC_CORE_RV32I_ID	0x52563332u	/* 'RV32' */
#define OPENBUS_COPROC_CORE_6502_ID	0x36353032u	/* '6502' */
#define OPENBUS_COPROC_CORE_Z80_ID	0x5a383020u	/* 'Z80 ' */
#define OPENBUS_COPROC_CORE_65C02_ID	0x43303220u	/* 'C02 ' */
#define OPENBUS_COPROC_CORE_8080_ID	0x38303830u	/* '8080' */
#define OPENBUS_COPROC_CORE_6809_ID	0x36383039u	/* '6809' */

/** CTRL bits. */
#define OPENBUS_COPROC_CTRL_RUN		0x01
#define OPENBUS_COPROC_CTRL_STEP	0x02
#define OPENBUS_COPROC_CTRL_RESET	0x04
#define OPENBUS_COPROC_CTRL_IRQ_ON_HALT	0x08

/** STATUS bits. */
#define OPENBUS_COPROC_STATUS_RUNNING	0x01
#define OPENBUS_COPROC_STATUS_HALTED	0x02
#define OPENBUS_COPROC_STATUS_FAULT	0x04
#define OPENBUS_COPROC_STATUS_IRQ	0x08
#define OPENBUS_COPROC_STATUS_DMA	0x10

/** DMACTRL bits. */
#define OPENBUS_COPROC_DMA_START	0x01
#define OPENBUS_COPROC_DMA_TO_HOST	0x02

/** Words moved per cycle granted, matching the stub card so a copy costs time. */
#define OPENBUS_COPROC_DMA_WORDS_PER_CYCLE	1

#define OPENBUS_COPROC_REG_RUNFOR	0x4c
#define OPENBUS_COPROC_REG_STOPREASON	0x50
#define OPENBUS_COPROC_REG_IRQCTRL	0x54
#define OPENBUS_COPROC_REG_MAPOFF	0x58
#define OPENBUS_COPROC_REG_MAPCOUNT	0x5c
#define OPENBUS_COPROC_REG_LOGOFF	0x60
#define OPENBUS_COPROC_REG_LOGENTRIES	0x64
#define OPENBUS_COPROC_REG_LOGHEAD	0x68
#define OPENBUS_COPROC_REG_LOGTAIL	0x6c
#define OPENBUS_COPROC_REG_WAITADDR	0x70
#define OPENBUS_COPROC_REG_WAITDATA	0x74
#define OPENBUS_COPROC_REG_WAITINFO	0x78
#define OPENBUS_COPROC_REG_WAITACK	0x7c
#define OPENBUS_COPROC_REG_CTLADDR	0x80
#define OPENBUS_COPROC_REG_CTLDATA	0x84
#define OPENBUS_COPROC_REG_CTLSIZE	0x88
#define OPENBUS_COPROC_REG_REGSEL	0x8c
#define OPENBUS_COPROC_REG_REGDATA	0x90

/** STATUS bit: an access is waiting for the guest to answer it. */
#define OPENBUS_COPROC_STATUS_WAITING	0x20

/** DMACTRL bit: move to or from the control area rather than card RAM. */
#define OPENBUS_COPROC_DMA_CONTROL	0x04

/** Why a run ended, in STOPREASON. */
#define OPENBUS_COPROC_STOP_NONE	0	/**< it has not run yet */
#define OPENBUS_COPROC_STOP_BUDGET	1	/**< RUNFOR cycles were used */
#define OPENBUS_COPROC_STOP_HALTED	2	/**< the core executed its stop */
#define OPENBUS_COPROC_STOP_FAULT	3	/**< the core faulted */
#define OPENBUS_COPROC_STOP_WAITING	4	/**< an access is waiting */
#define OPENBUS_COPROC_STOP_STOPPED	5	/**< RUN was cleared */

/** IRQCTRL fields. */
#define OPENBUS_COPROC_IRQ_ASSERT	0x01
#define OPENBUS_COPROC_IRQ_NMI		0x02
#define OPENBUS_COPROC_IRQ_LEVEL(v)	(((v) >> 8) & 7u)
#define OPENBUS_COPROC_IRQ_VECTOR(v)	(((v) >> 16) & 0xffu)

/** How much control area a card carries. Region table, log ring and latches all
    live in it, and 256K is more than any of them will want. */
#define OPENBUS_COPROC_CTL_SIZE		(256u * 1024u)

/** Which core to fit. */
typedef enum {
	OPENBUS_COPROC_RV32I = 0,
	OPENBUS_COPROC_6502,
	OPENBUS_COPROC_Z80,
	/* ★ New cores go at the END. The value is what a machine's configuration
	   stores, so inserting one in the middle would silently change which
	   processor every existing machine has fitted. */
	OPENBUS_COPROC_65C02,
	OPENBUS_COPROC_8080,
	OPENBUS_COPROC_6809
} openbus_coproc_core;

/*
 * ★ HOW MUCH RAM A CARD MAY HAVE, and why the answer is per core rather than one
 * number.
 *
 * The card's RAM is the core's address space, so the ceiling is not a matter of
 * taste: it is how many address lines the processor has. A 6502 or a Z80 cannot
 * form an address above &FFFF, so 64K is not a default for them, it is the whole
 * of what they can reach and there is nothing for a 65th kilobyte to be. RV32I
 * has a 32-bit space and could in principle take any amount, so its ceiling is a
 * judgement instead: 64MB, which is more than any program written for this card
 * will want and still a fraction of what the host has.
 *
 * ★ SO WHY MAY AN 8-BIT CARD HAVE MEGABYTES? Because BANKING reaches it. The
 * flat limit above is real - no 6502 instruction can name an address past &FFFF -
 * but a Spectrum 128, a BBC's sideways ROMs and a C64's REU all had more memory
 * than that and paged it through a window, and the address map does the same with
 * COPRO_REGION_OFFSET (see copro_bus.h). So the card may carry up to 16MB for an
 * 8-bit core, of which 64K is visible at a time and the guest chooses which 64K.
 * The default stays at 64K, because a machine that is not paging anything wants
 * exactly its address space and nothing more.
 */
#define OPENBUS_COPROC_RAM_MIN		(4u * 1024u)

/** The most that can be FITTED. What is addressable at once is below. */
#define OPENBUS_COPROC_RAM_MAX_8BIT	(16u * 1024u * 1024u)
#define OPENBUS_COPROC_RAM_MAX_RV32I	(64u * 1024u * 1024u)

/**
 * How much of the card's RAM a core can address without paging: its flat address
 * space. Anything fitted above this is reachable only through a window with
 * COPRO_REGION_OFFSET set, and the machine editor says so beside the size.
 */
uint32_t openbus_coproc_ram_flat_limit(openbus_coproc_core core);

/** The default for a core, in bytes: what a card gets when nothing is chosen. */
uint32_t openbus_coproc_ram_default(openbus_coproc_core core);

/** The most a core can be given, in bytes. */
uint32_t openbus_coproc_ram_max(openbus_coproc_core core);

/**
 * Ask for a card RAM size, in bytes, for the next fit. Zero restores the
 * default. A size above what the core can address, or below
 * OPENBUS_COPROC_RAM_MIN, is clamped rather than refused: this is a setting
 * arriving from a configuration file that may have been edited by hand, and a
 * machine that will not start is a worse answer than one with a sensible amount
 * of memory.
 *
 * @return the size that will actually be used.
 */
uint32_t openbus_coproc_request_ram(uint32_t bytes);

/** The size the next fit will use, whether chosen or defaulted. */
uint32_t openbus_coproc_requested_ram(void);

/**
 * Ask for a co-processor card when the machine starts.
 *
 * @name is what the user typed after --openbus-card: "rv32i", "6502" or "z80",
 * matched without regard to case.
 *
 * @return zero if the name names a core, non-zero if it does not - in which case
 *         nothing is requested and the caller should complain rather than
 *         silently starting without a card.
 */
int openbus_coproc_request(const char *name);

/** Was a card asked for? */
int openbus_coproc_requested(void);

/** Which core was asked for. Only meaningful once requested. */
openbus_coproc_core openbus_coproc_requested_core(void);

/**
 * The name of a core, as --openbus-card accepts it. NULL if @core is not one.
 *
 * Kept here rather than in the option parser so that the list of cores exists
 * once: the help text, the parser and the log all read it from this function.
 */
const char *openbus_coproc_core_name(openbus_coproc_core core);

/**
 * Fit the requested card to the second slot.
 *
 * @return zero on success, non-zero if the bus is not started, a card is already
 *         fitted, or the card's RAM could not be allocated.
 */
int openbus_coproc_fit(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENBUS_COPROC_H */
