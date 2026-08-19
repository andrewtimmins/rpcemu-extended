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

#ifndef OPENBUS_H
#define OPENBUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OPEN Bus - the Risc PC's second processor interface.
 *
 * WHAT IT IS. Quoting the Acorn Risc PC Technical Reference Manual (issue 1,
 * September 1994), section "OPEN Bus": "Risc PC is Acorn's first 32 Bit ARM
 * based platform to offer support for more than one CPU bus master. The 'OPEN
 * Bus' interface allows a second processor device to be connected to the system,
 * whilst sharing resources such as memory & I/O with the native CPU." Physically
 * two 96-way DIN sockets, one of which must hold the host ARM; the manual
 * recommends the host takes slot 0 and a second card slot 1.
 *
 * It is not a peripheral bus and it is emphatically not a podule. A card on it is
 * a REAL BUS MASTER with full access to main memory and memory-mapped I/O, and
 * the TRM names "'foreign' CPUs, such as the Intel 80x86 processor family" as an
 * intended use. Only two designs ever used it: Aleph One's x86 PC card (the
 * Gemini ASIC) and Simtec's Hydra, a multiprocessor technology demonstrator
 * rather than a shipping product.
 *
 * ★ WHAT THIS FILE DELIBERATELY DOES NOT DO. The TRM reserves
 * 0x03600000-0x037FFFFF for "OPEN Bus second bus master system registers" but
 * the layout inside that window is PRODUCT SPECIFIC - the manual's own summary
 * table says as much. Gemini's registers are Aleph One's design, not Acorn's. So
 * this subsystem owns the bus and the window, and a card owns every register in
 * it. Resisting the urge to generalise the registers is the whole point: there
 * were three cards, they agreed on nothing above the bus, and an abstraction
 * that guessed at a fourth would be worse than none.
 *
 * WHAT IT MODELS. The bus, and only the bus:
 *
 *   - a slot holding at most one second master
 *   - decode of the register window, dispatched to that card
 *   - physical memory and I/O access on the card's behalf (bus mastering)
 *   - nPIRQ and the FIQ path, by which a card interrupts the ARM
 *   - bus ownership, so a card that holds the bus stalls the ARM, as nWAIT does
 *   - reset, and a share of the ARM's timeslice
 *
 * ★ HOW A SECOND MASTER CAN WORK AT ALL, given that ARMState is one global.
 * OPEN Bus hands the bus over: the second master asserts BREQ, takes the bus,
 * and drives nWAIT to stall the host ARM while it is active. Only one master
 * runs at a time, by design. So an implementation that runs one and then the
 * other, charging the cycles to a single budget, is not a shortcut around our
 * single global CPU state - it is what the hardware does.
 *
 * Host services arrive through openbus_host_ops rather than by calling the
 * emulator directly, so this whole subsystem builds and runs with no emulator
 * and no wxWidgets behind it. tests/test_openbus.c substitutes a fake memory and
 * a fake interrupt controller and checks the bus rules on every platform.
 */

/**
 * How many cycles a fitted card is offered per pass of the emulator's inner
 * loop.
 *
 * Not the whole remaining budget, which is what it used to be given. A card that
 * always wants time - a processor card running a program, as opposed to the stub
 * card which only wants time during a copy - would take the entire 20000-cycle
 * budget and leave the host ARM a single block of instructions per pass. The
 * guest would crawl, and a guest polling for the card's answer would be starving
 * the very loop it was waiting on.
 */
#define OPENBUS_SLICE		64

/** Register window reserved by the TRM for second bus master registers. */
#define OPENBUS_REG_BASE	0x03600000u
#define OPENBUS_REG_SIZE	0x00200000u
#define OPENBUS_REG_END		(OPENBUS_REG_BASE + OPENBUS_REG_SIZE)	/* exclusive */

/** Slots. The host ARM occupies slot 0 by the TRM's recommendation. */
#define OPENBUS_SLOT_HOST	0
#define OPENBUS_SLOT_SECOND	1
#define OPENBUS_SLOTS		2

/** Access width, for cards that care. Bytes, so it reads as a size. */
typedef enum {
	OPENBUS_SIZE_8 = 1,
	OPENBUS_SIZE_16 = 2,
	OPENBUS_SIZE_32 = 4
} openbus_size;

/*
 * What the host provides. Injected so the subsystem has no dependency on the
 * emulator, which is what makes it testable.
 */
typedef struct openbus_host_ops {
	/** Read/write physical memory as a bus master, bypassing the ARM's MMU. */
	uint32_t (*read32)(uint32_t phys_addr);
	void (*write32)(uint32_t phys_addr, uint32_t val);

	/**
	 * nPIRQ: the card interrupting the host ARM.
	 *
	 * The TRM routes a second master's interrupt down the podule interrupt
	 * line, noting the card "must" carry its own interrupt flag and mask
	 * registers so software can tell a podule apart from the second master.
	 * That register is the card's business; this is only the wire.
	 */
	void (*set_irq)(int state);
	void (*set_fiq)(int state);
} openbus_host_ops;

/*
 * A card fitted to the second slot.
 *
 * Every entry may be NULL except name: a card that only wants registers need not
 * pretend to execute anything, which is exactly what the stub does.
 */
typedef struct openbus_master {
	/** Shown in the log and the machine inspector. Required. */
	const char *name;

	/** Card private state, owned by the card. */
	void *p;

	/**
	 * Register window access, at @offset from OPENBUS_REG_BASE.
	 *
	 * Product specific in its entirety. A read from an offset a card does not
	 * implement should answer 0xffffffff, as an undriven bus does.
	 */
	uint32_t (*reg_read)(void *p, uint32_t offset, openbus_size size);
	void (*reg_write)(void *p, uint32_t offset, openbus_size size, uint32_t val);

	/**
	 * Give the card some execution time.
	 *
	 * @cycles: how many the host is willing to give up.
	 * Returns how many were used, which are charged against the ARM's own
	 * budget - the emulator's equivalent of nWAIT stalling the host while the
	 * second master holds the bus. Returning more than @cycles is clamped;
	 * returning zero means the card wanted nothing, which is the normal case
	 * for a card sitting idle.
	 */
	int (*run)(void *p, int cycles);

	/** System reset (nRESET). */
	void (*reset)(void *p);

	/** Release anything the card owns, including p. */
	void (*close)(void *p);
} openbus_master;

/**
 * Start the subsystem. @ops must outlive it; NULL entries are tolerated and
 * simply make that service unavailable to a card.
 */
void openbus_init(const openbus_host_ops *ops);

/** Remove any fitted card and forget the host ops. */
void openbus_close(void);

/**
 * Fit a card to the second slot.
 *
 * @return zero on success, non-zero if a card is already fitted, @master is
 *         unusable, or the subsystem has not been started. A card is never
 *         silently replaced: two cards in one slot is a configuration mistake
 *         and quietly dropping one would hide it.
 */
int openbus_fit(const openbus_master *master);

/** Remove the fitted card, calling its close(). Harmless if none is fitted. */
void openbus_remove(void);

/** Is a card fitted? */
int openbus_present(void);

/** Name of the fitted card, or NULL. */
const char *openbus_name(void);

/** Machine reset: passed to the card. */
void openbus_reset(void);

/** Does @addr fall in the second bus master register window? */
int openbus_addr_in_window(uint32_t addr);

/**
 * Register window access.
 *
 * Reads with nothing fitted, or from a card with no reg_read, answer all ones -
 * an undriven bus, which is what a real machine with an empty slot gives. Writes
 * are dropped.
 */
uint32_t openbus_reg_read(uint32_t addr, openbus_size size);
void openbus_reg_write(uint32_t addr, openbus_size size, uint32_t val);

/**
 * Give the fitted card up to @cycles, returning how many it used.
 *
 * Zero when nothing is fitted, so the caller's fast path is one test of a
 * pointer.
 */
int openbus_run(int cycles);

/*
 * Bus mastering, for a card to reach main memory and I/O.
 *
 * Physical addresses only: a bus master has no MMU of the ARM's, which is the
 * whole reason Gemini needed its own mapping registers.
 */
uint32_t openbus_master_read32(uint32_t phys_addr);
void openbus_master_write32(uint32_t phys_addr, uint32_t val);

/** Assert or release nPIRQ / the FIQ path on the host ARM. */
void openbus_master_set_irq(int state);
void openbus_master_set_fiq(int state);

#ifdef __cplusplus
}
#endif

#endif /* OPENBUS_H */
