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
 * ★ CYCLES ARE INSTRUCTIONS, NOT BUS CYCLES. Every core here returns one cycle
 * per instruction, so the CYCLES register counts instructions retired. Being
 * honest about that matters because the alternative would be a fiction: a real
 * card's processor ran on its own crystal, at a rate that has nothing to do with
 * how many host cycles the emulator hands over, and per-instruction timing tables
 * would cost host time to maintain a number that still would not mean what it
 * says. A program that wants to know how long it took should ask RISC OS.
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
 *   0x28 CYCLES     R   instructions retired since reset (low 32 bits).
 *   0x2c PC         R   the core's program counter.
 *   0x30 FAULT      R   fault cause, core specific; 0 when STATUS says no fault.
 *   0x34 FAULTADDR  R   the address or opcode that faulted.
 *   0x38 IRQCLEAR   W   any value drops nPIRQ.
 *   0x3c DMAHOST    RW  host PHYSICAL address for a bus-master copy.
 *   0x40 DMALOCAL   RW  card RAM address for the same.
 *   0x44 DMALEN     RW  length in 32-bit words, counted down as it runs.
 *   0x48 DMACTRL    RW  bit 0  START (self-clearing)
 *                       bit 1  direction: 0 host to card, 1 card to host
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

/** Which core to fit. */
typedef enum {
	OPENBUS_COPROC_RV32I = 0,
	OPENBUS_COPROC_6502,
	OPENBUS_COPROC_Z80
} openbus_coproc_core;

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
