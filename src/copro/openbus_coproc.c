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

/*
 * openbus_coproc.c - a co-processor card for the OPEN Bus, with a choice of core.
 *
 * The register map, why the core has its own RAM, why the mailbox is asymmetric
 * and why CYCLES counts instructions are all in openbus_coproc.h.
 *
 * The three cores are reached through the small dispatch block below rather than
 * through function pointers in a vtable. There are three of them, they are chosen
 * once when the card is fitted and never change, and a switch says what is going
 * on where a table of pointers would need to be read alongside three
 * implementations of each entry.
 */

#include "openbus_coproc.h"

#include "cpu_6502.h"
#include "cpu_rv32i.h"
#include "cpu_z80.h"
#include "openbus.h"

#include <stdlib.h>
#include <string.h>

/*
 * How much RAM each core gets.
 *
 * The 8-bit cores get the 64K that is their whole address space, so their
 * aperture covers everything they can reach and no address they can form is
 * outside it. RV32I gets a megabyte: enough for compiled C with its data and a
 * stack, and small enough that a card is not quietly asking for real memory the
 * guest might want.
 */
#define COPROC_RAM_RV32I	(1024u * 1024u)
#define COPROC_RAM_6502		(64u * 1024u)
#define COPROC_RAM_Z80		(64u * 1024u)

typedef struct {
	openbus_coproc_core core;

	uint8_t *ram;
	uint32_t ram_size;

	/* One core is fitted, so the states share their space. */
	union {
		rv32i_state rv32i;
		cpu6502_state m6502;
		cpu_z80_state z80;
	} cpu;

	uint32_t ctrl;
	uint32_t entry;
	uint32_t addr;		/* the aperture */
	uint32_t mbox_tx;
	uint32_t mbox_rx;

	int irq;		/* is this card holding nPIRQ? */
	int stop_reported;	/* has the stop that raised it been acted on? */

	uint32_t dma_host;
	uint32_t dma_local;
	uint32_t dma_len;	/* words still to move */
	int dma_to_host;
	int dma_busy;
} coproc_state;

/* One card, one slot, so the state is static rather than allocated - as the stub
   card's is. Only ram is owned, and close() frees it. */
static coproc_state cp;

/* Set from the command line, long before there is a bus to fit anything to. */
static int coproc_requested;
static openbus_coproc_core coproc_requested_core;

/* ------------------------------------------------------------ core dispatch */

static void
core_reset(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:
		rv32i_reset(&cp.cpu.rv32i, cp.entry);
		break;
	case OPENBUS_COPROC_6502:
		cpu6502_reset(&cp.cpu.m6502, (uint16_t) cp.entry);
		break;
	default:
		cpu_z80_reset(&cp.cpu.z80, (uint16_t) cp.entry);
		break;
	}
}

static int
core_run(int cycles)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return rv32i_run(&cp.cpu.rv32i, cycles);
	case OPENBUS_COPROC_6502:	return cpu6502_run(&cp.cpu.m6502, cycles);
	default:			return cpu_z80_run(&cp.cpu.z80, cycles);
	}
}

static int
core_step(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return rv32i_step(&cp.cpu.rv32i);
	case OPENBUS_COPROC_6502:	return cpu6502_step(&cp.cpu.m6502);
	default:			return cpu_z80_step(&cp.cpu.z80);
	}
}

static int
core_halted(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.halted;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.halted;
	default:			return cp.cpu.z80.halted;
	}
}

static int
core_faulted(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.faulted;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.faulted;
	default:			return cp.cpu.z80.faulted;
	}
}

static uint32_t
core_pc(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.pc;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.pc;
	default:			return cp.cpu.z80.pc;
	}
}

static uint32_t
core_cycles(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return (uint32_t) cp.cpu.rv32i.cycles;
	case OPENBUS_COPROC_6502:	return (uint32_t) cp.cpu.m6502.cycles;
	default:			return (uint32_t) cp.cpu.z80.cycles;
	}
}

static uint32_t
core_fault_cause(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.fault_cause;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.fault_cause;
	default:			return cp.cpu.z80.fault_cause;
	}
}

static uint32_t
core_fault_addr(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.fault_addr;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.fault_addr;
	default:			return cp.cpu.z80.fault_addr;
	}
}

static uint32_t
core_exit_code(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return cp.cpu.rv32i.exit_code;
	case OPENBUS_COPROC_6502:	return cp.cpu.m6502.exit_code;
	default:			return cp.cpu.z80.exit_code;
	}
}

static uint32_t
core_id(void)
{
	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:	return OPENBUS_COPROC_CORE_RV32I_ID;
	case OPENBUS_COPROC_6502:	return OPENBUS_COPROC_CORE_6502_ID;
	default:			return OPENBUS_COPROC_CORE_Z80_ID;
	}
}

/* ------------------------------------------------- the Z80's port space */

/*
 * Port 0 is the mailbox; every other port is the latch inside the core, so a
 * program that only talks to itself still gets 255 working ports. See the
 * asymmetry note in openbus_coproc.h for why only the Z80 has this.
 */

static uint8_t
coproc_io_in(void *ctx, uint8_t port)
{
	(void) ctx;

	if (port == 0) {
		return (uint8_t) cp.mbox_tx;
	}
	return cp.cpu.z80.ports[port];
}

static void
coproc_io_out(void *ctx, uint8_t port, uint8_t val)
{
	(void) ctx;

	cp.cpu.z80.ports[port] = val;
	if (port == 0) {
		cp.mbox_rx = val;
	}
}

/* ------------------------------------------------------------- the aperture */

/**
 * Read @size bytes of card RAM at the aperture, advancing it.
 *
 * The advance is a side effect of a read, which is normally a thing to avoid -
 * but it is what makes the aperture worth having, and it is safe here for a
 * reason worth writing down: mem_phys_read8_debug() handles only ROM, VRAM and
 * RAM, so the debugger and the inspector cannot reach this window at all. There
 * is no way for a passive observer to disturb the aperture. See openbus.h.
 */
static uint32_t
aperture_read(openbus_size size)
{
	uint32_t val = 0;
	unsigned i;

	if (cp.ram == NULL || cp.ram_size == 0) {
		return 0xffffffffu;
	}

	for (i = 0; i < (unsigned) size; i++) {
		/* Wraps at the end of card RAM rather than faulting: the aperture
		   is a window a guest walks, and stopping dead at the top would
		   need an error path in every caller for no benefit. */
		val |= (uint32_t) cp.ram[(cp.addr + i) % cp.ram_size] << (i * 8);
	}
	cp.addr = (cp.addr + (uint32_t) size) % cp.ram_size;
	return val;
}

static void
aperture_write(openbus_size size, uint32_t val)
{
	unsigned i;

	if (cp.ram == NULL || cp.ram_size == 0) {
		return;
	}

	for (i = 0; i < (unsigned) size; i++) {
		cp.ram[(cp.addr + i) % cp.ram_size] = (uint8_t) (val >> (i * 8));
	}
	cp.addr = (cp.addr + (uint32_t) size) % cp.ram_size;
}

/* ------------------------------------------------------------------ the card */

static void
coproc_drop_irq(void)
{
	if (cp.irq) {
		cp.irq = 0;
		openbus_master_set_irq(0);
	}
}

static void
coproc_reset(void *p)
{
	(void) p;

	coproc_drop_irq();

	cp.ctrl = 0;
	cp.addr = 0;
	cp.mbox_tx = 0;
	cp.mbox_rx = 0;
	cp.stop_reported = 0;
	cp.dma_host = 0;
	cp.dma_local = 0;
	cp.dma_len = 0;
	cp.dma_to_host = 0;
	cp.dma_busy = 0;

	/* ENTRY survives, because a machine reset should not lose where the guest
	   said the program starts; the core is put back to that entry point. */
	core_reset();
}

static uint32_t
coproc_status(void)
{
	uint32_t val = 0;

	if ((cp.ctrl & OPENBUS_COPROC_CTRL_RUN) && !core_halted() && !core_faulted()) {
		val |= OPENBUS_COPROC_STATUS_RUNNING;
	}
	if (core_halted()) {
		val |= OPENBUS_COPROC_STATUS_HALTED;
	}
	if (core_faulted()) {
		val |= OPENBUS_COPROC_STATUS_FAULT;
	}
	if (cp.irq) {
		val |= OPENBUS_COPROC_STATUS_IRQ;
	}
	if (cp.dma_busy) {
		val |= OPENBUS_COPROC_STATUS_DMA;
	}
	return val;
}

/** Every register except DATA, which has its own path because it moves memory. */
static uint32_t
coproc_read_word(uint32_t reg)
{
	switch (reg) {
	case OPENBUS_COPROC_REG_ID:		return OPENBUS_COPROC_ID;
	case OPENBUS_COPROC_REG_CORE:		return core_id();
	case OPENBUS_COPROC_REG_RAMSIZE:	return cp.ram_size;
	case OPENBUS_COPROC_REG_CTRL:		return cp.ctrl;
	case OPENBUS_COPROC_REG_STATUS:		return coproc_status();
	case OPENBUS_COPROC_REG_ENTRY:		return cp.entry;
	case OPENBUS_COPROC_REG_ADDR:		return cp.addr;
	case OPENBUS_COPROC_REG_MBOX_TX:	return cp.mbox_tx;
	case OPENBUS_COPROC_REG_MBOX_RX:	return cp.mbox_rx;
	case OPENBUS_COPROC_REG_CYCLES:		return core_cycles();
	case OPENBUS_COPROC_REG_PC:		return core_pc();
	case OPENBUS_COPROC_REG_FAULT:
		/* Zero unless there is a fault, so that a guest reading FAULT
		   without checking STATUS first is not misled by the cause of an
		   older one. */
		return core_faulted() ? core_fault_cause() : 0;
	case OPENBUS_COPROC_REG_FAULTADDR:
		return core_faulted() ? core_fault_addr() : 0;
	case OPENBUS_COPROC_REG_DMAHOST:	return cp.dma_host;
	case OPENBUS_COPROC_REG_DMALOCAL:	return cp.dma_local;
	case OPENBUS_COPROC_REG_DMALEN:		return cp.dma_len;
	case OPENBUS_COPROC_REG_DMACTRL:
		return (uint32_t) ((cp.dma_busy ? OPENBUS_COPROC_DMA_START : 0) |
		                   (cp.dma_to_host ? OPENBUS_COPROC_DMA_TO_HOST : 0));
	default:
		/* Nothing driving this address, including IRQCLEAR: it is write
		   only, and reading it would suggest otherwise. */
		return 0xffffffffu;
	}
}

/**
 * Note the core having stopped, once.
 *
 * Raising nPIRQ is what saves the guest from polling. It happens once per stop,
 * not once per timeslice, or the line would be re-asserted immediately after the
 * guest cleared it and the machine would spend its life in an interrupt handler.
 */
static void
coproc_note_stop(void)
{
	if (cp.stop_reported) {
		return;
	}
	if (!core_halted() && !core_faulted()) {
		return;
	}

	cp.stop_reported = 1;

	if (core_halted()) {
		cp.mbox_rx = core_exit_code();
	}

	/* RUN is cleared so that STATUS stops claiming the core is running and a
	   later timeslice does not try to execute a halted core. */
	cp.ctrl &= ~(uint32_t) OPENBUS_COPROC_CTRL_RUN;

	if (cp.ctrl & OPENBUS_COPROC_CTRL_IRQ_ON_HALT) {
		cp.irq = 1;
		openbus_master_set_irq(1);
	}
}

static void
coproc_start_dma(void)
{
	/* A transfer that would run outside card RAM does not start, and DMALEN
	   keeps its value so the guest can see that nothing happened. Clamping
	   instead would move somebody's data by an amount they did not ask for. */
	if (cp.ram == NULL || cp.dma_len == 0) {
		return;
	}
	if ((cp.dma_local & 3u) != 0) {
		return;		/* words, so the local address must be aligned */
	}
	/* The bound is checked before the subtraction that uses it, or an address
	   past the end of RAM would underflow and look like room to spare. */
	if (cp.dma_local >= cp.ram_size) {
		return;
	}
	if (cp.dma_len > (cp.ram_size - cp.dma_local) / 4u) {
		return;
	}

	cp.dma_busy = 1;
}

/**
 * Move the DMA on, as a bus master.
 *
 * @return cycles used, which the caller charges against the host's budget - so a
 *         transfer costs the guest real time rather than being free, exactly as
 *         the stub card's copy does.
 */
static int
coproc_run_dma(int budget)
{
	int used = 0;

	while (used < budget && cp.dma_len > 0) {
		int words = OPENBUS_COPROC_DMA_WORDS_PER_CYCLE;

		while (words-- > 0 && cp.dma_len > 0) {
			if (cp.dma_to_host) {
				const uint32_t word =
				    (uint32_t) cp.ram[cp.dma_local] |
				    ((uint32_t) cp.ram[cp.dma_local + 1] << 8) |
				    ((uint32_t) cp.ram[cp.dma_local + 2] << 16) |
				    ((uint32_t) cp.ram[cp.dma_local + 3] << 24);

				openbus_master_write32(cp.dma_host, word);
			} else {
				const uint32_t word = openbus_master_read32(cp.dma_host);

				cp.ram[cp.dma_local] = (uint8_t) word;
				cp.ram[cp.dma_local + 1] = (uint8_t) (word >> 8);
				cp.ram[cp.dma_local + 2] = (uint8_t) (word >> 16);
				cp.ram[cp.dma_local + 3] = (uint8_t) (word >> 24);
			}
			cp.dma_host += 4;
			cp.dma_local += 4;
			cp.dma_len--;
		}
		used++;
	}

	if (cp.dma_len == 0) {
		cp.dma_busy = 0;
	}
	return used;
}

static uint32_t
coproc_reg_read(void *p, uint32_t offset, openbus_size size)
{
	const uint32_t reg = offset & ~3u;
	const unsigned shift = (offset & 3u) * 8u;
	uint32_t word;

	(void) p;

	if (reg == OPENBUS_COPROC_REG_DATA) {
		return aperture_read(size);
	}

	word = coproc_read_word(reg);

	if (size == OPENBUS_SIZE_32) {
		return word;
	}

	/* A narrow access sees the addressed part of the word, little-endian, as
	   it would on the bus. The stub card answers the whole word whatever the
	   width; doing it properly here costs two lines and means a guest reading
	   the second byte of a register gets the second byte. */
	word >>= shift;
	return (size == OPENBUS_SIZE_8) ? (word & 0xffu) : (word & 0xffffu);
}

static void
coproc_reg_write(void *p, uint32_t offset, openbus_size size, uint32_t val)
{
	const uint32_t reg = offset & ~3u;

	(void) p;

	if (reg == OPENBUS_COPROC_REG_DATA) {
		aperture_write(size, val);
		return;
	}
	(void) size;

	/*
	 * Narrow writes to the control registers are not merged into the word
	 * they address: every register here is a word, a guest driving this card
	 * writes words, and a byte write that quietly changed one quarter of
	 * CTRL would be a harder thing to explain than one that plainly writes
	 * the value given. The aperture above is the exception because its width
	 * is the point of it.
	 */

	switch (reg) {
	case OPENBUS_COPROC_REG_CTRL:
		/* RESET first and on its own: a write asking to reset and to run
		   in one go is a contradiction, and honouring the reset is the
		   less surprising reading. Self-clearing, so the bit never
		   sticks. */
		if (val & OPENBUS_COPROC_CTRL_RESET) {
			coproc_reset(NULL);
			return;
		}

		cp.ctrl = val & (OPENBUS_COPROC_CTRL_RUN |
		                 OPENBUS_COPROC_CTRL_IRQ_ON_HALT);

		/* Starting a core that has stopped arms the report again, so the
		   next halt raises nPIRQ as the first one did. */
		if ((cp.ctrl & OPENBUS_COPROC_CTRL_RUN) && !core_halted() &&
		    !core_faulted()) {
			cp.stop_reported = 0;
		}

		if (val & OPENBUS_COPROC_CTRL_STEP) {
			/* Executed here rather than on the next timeslice: a guest
			   that writes STEP and then reads PC expects the step to
			   have happened, and this write is already running on the
			   thread that owns the machine. */
			(void) core_step();
			coproc_note_stop();
		}
		break;

	case OPENBUS_COPROC_REG_ENTRY:
		cp.entry = val;
		break;

	case OPENBUS_COPROC_REG_ADDR:
		cp.addr = (cp.ram_size != 0) ? (val % cp.ram_size) : 0;
		break;

	case OPENBUS_COPROC_REG_MBOX_TX:
		cp.mbox_tx = val;
		break;

	case OPENBUS_COPROC_REG_IRQCLEAR:
		coproc_drop_irq();
		break;

	case OPENBUS_COPROC_REG_DMAHOST:
		cp.dma_host = val;
		break;

	case OPENBUS_COPROC_REG_DMALOCAL:
		cp.dma_local = val;
		break;

	case OPENBUS_COPROC_REG_DMALEN:
		cp.dma_len = val;
		break;

	case OPENBUS_COPROC_REG_DMACTRL:
		cp.dma_to_host = (val & OPENBUS_COPROC_DMA_TO_HOST) != 0;
		if (val & OPENBUS_COPROC_DMA_START) {
			coproc_start_dma();
		}
		break;

	default:
		/* Read-only or unimplemented. Dropped, as an undriven write is. */
		break;
	}
}

static int
coproc_run(void *p, int cycles)
{
	int used = 0;

	(void) p;

	if (cycles <= 0) {
		return 0;
	}

	/* The transfer goes first: a program waiting for its data cannot make
	   progress until the data is there, and the card is one bus master
	   whichever of the two is using it. */
	if (cp.dma_busy) {
		used += coproc_run_dma(cycles);
	}

	if ((cp.ctrl & OPENBUS_COPROC_CTRL_RUN) && used < cycles) {
		used += core_run(cycles - used);
		coproc_note_stop();
	}

	return used;
}

static void
coproc_close(void *p)
{
	(void) p;

	free(cp.ram);
	cp.ram = NULL;
	cp.ram_size = 0;
}

/* Named per core, because this is the string the log and the inspector show and
   "which processor is in the slot" is the only thing anyone wants from it. The
   bus copies the master struct but not the string, so these have static
   lifetime. */
static const char coproc_name_rv32i[] = "OPEN Bus co-processor card (RV32IM)";
static const char coproc_name_6502[]  = "OPEN Bus co-processor card (6502)";
static const char coproc_name_z80[]   = "OPEN Bus co-processor card (Z80)";

/* --------------------------------------------------------------- interface */

const char *
openbus_coproc_core_name(openbus_coproc_core core)
{
	switch (core) {
	case OPENBUS_COPROC_RV32I:	return "rv32i";
	case OPENBUS_COPROC_6502:	return "6502";
	case OPENBUS_COPROC_Z80:	return "z80";
	default:			return NULL;
	}
}

int
openbus_coproc_request(const char *name)
{
	int core;

	if (name == NULL) {
		return -1;
	}

	for (core = OPENBUS_COPROC_RV32I; core <= OPENBUS_COPROC_Z80; core++) {
		const char *candidate =
		    openbus_coproc_core_name((openbus_coproc_core) core);
		size_t i;

		/* Compared without regard to case, and without strcasecmp, which
		   is neither C99 nor available everywhere this builds. */
		for (i = 0; ; i++) {
			const char a = name[i];
			char b = candidate[i];

			if (a >= 'A' && a <= 'Z') {
				/* Only the name needs folding: the candidates are
				   written in lower case above. */
				if ((char) (a + ('a' - 'A')) != b) {
					break;
				}
			} else if (a != b) {
				break;
			}
			if (a == '\0') {
				coproc_requested = 1;
				coproc_requested_core = (openbus_coproc_core) core;
				return 0;
			}
		}
	}

	return -1;
}

int
openbus_coproc_requested(void)
{
	return coproc_requested;
}

openbus_coproc_core
openbus_coproc_requested_core(void)
{
	return coproc_requested_core;
}

int
openbus_coproc_fit(void)
{
	openbus_master master;
	uint32_t size;

	memset(&master, 0, sizeof(master));

	switch (coproc_requested_core) {
	case OPENBUS_COPROC_RV32I:
		size = COPROC_RAM_RV32I;
		master.name = coproc_name_rv32i;
		break;
	case OPENBUS_COPROC_6502:
		size = COPROC_RAM_6502;
		master.name = coproc_name_6502;
		break;
	case OPENBUS_COPROC_Z80:
		size = COPROC_RAM_Z80;
		master.name = coproc_name_z80;
		break;
	default:
		return -1;
	}

	/*
	 * ★ REFUSE AN OCCUPIED SLOT BEFORE TOUCHING ANY STATE. openbus_fit() is
	 * what enforces one card per slot, and asking it last looks harmless
	 * until you notice what happens on the way: the lines below free the
	 * card's RAM and clear its state, so an earlier version of this function
	 * DESTROYED the working card that was about to refuse the request. Its
	 * register window then read as an empty slot. Found by
	 * tests/test_openbus_coproc.c, which asks for a second card on purpose.
	 */
	if (openbus_present()) {
		return -1;
	}

	/* Any RAM from a previous fit goes first, so fitting twice cannot leak. */
	free(cp.ram);
	memset(&cp, 0, sizeof(cp));

	cp.core = coproc_requested_core;
	cp.ram = calloc(size, 1);
	if (cp.ram == NULL) {
		return -1;
	}
	cp.ram_size = size;

	switch (cp.core) {
	case OPENBUS_COPROC_RV32I:
		rv32i_init(&cp.cpu.rv32i, cp.ram, cp.ram_size);
		break;
	case OPENBUS_COPROC_6502:
		cpu6502_init(&cp.cpu.m6502, cp.ram, cp.ram_size);
		break;
	default:
		cpu_z80_init(&cp.cpu.z80, cp.ram, cp.ram_size);
		cp.cpu.z80.io_in = coproc_io_in;
		cp.cpu.z80.io_out = coproc_io_out;
		cp.cpu.z80.io_ctx = NULL;
		break;
	}

	master.reg_read = coproc_reg_read;
	master.reg_write = coproc_reg_write;
	master.run = coproc_run;
	master.reset = coproc_reset;
	master.close = coproc_close;

	if (openbus_fit(&master) != 0) {
		free(cp.ram);
		cp.ram = NULL;
		cp.ram_size = 0;
		return -1;
	}

	return 0;
}
