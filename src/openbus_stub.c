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
 * openbus_stub.c - the card that proves the bus.
 *
 * What it is and why it exists are in openbus_stub.h. Nothing here models real
 * hardware.
 */

#include "openbus_stub.h"

#include "openbus.h"

#include <stddef.h>
#include <string.h>

typedef struct {
	uint32_t ctrl;
	uint32_t src;
	uint32_t dst;
	uint32_t len;		/* words still to move */
	uint32_t scratch;
	uint32_t runs;
	uint32_t copied;
	int busy;
	int done;
} stub_state;

/* One card, one slot, so the state is static rather than allocated. close()
   therefore has nothing to free, which is why it is absent below. */
static stub_state stub;

static void
stub_reset(void *p)
{
	(void) p;

	/* Interrupt lines are dropped by the bus on reset, so only the card's own
	   state is cleared here. Clearing them again would be harmless but would
	   suggest the responsibility sits here, and it does not. */
	memset(&stub, 0, sizeof(stub));
}

static uint32_t
stub_reg_read(void *p, uint32_t offset, openbus_size size)
{
	uint32_t val;

	(void) p;
	(void) size;

	switch (offset & ~3u) {
	case OPENBUS_STUB_REG_ID:
		return OPENBUS_STUB_ID;

	case OPENBUS_STUB_REG_CTRL:
		return stub.ctrl;

	case OPENBUS_STUB_REG_STATUS:
		val = 0;
		if (stub.busy) {
			val |= OPENBUS_STUB_STATUS_BUSY;
		}
		if (stub.ctrl & OPENBUS_STUB_CTRL_IRQ) {
			val |= OPENBUS_STUB_STATUS_IRQ;
		}
		if (stub.ctrl & OPENBUS_STUB_CTRL_FIQ) {
			val |= OPENBUS_STUB_STATUS_FIQ;
		}
		if (stub.done) {
			val |= OPENBUS_STUB_STATUS_DONE;
		}
		return val;

	case OPENBUS_STUB_REG_SRC:	return stub.src;
	case OPENBUS_STUB_REG_DST:	return stub.dst;
	case OPENBUS_STUB_REG_LEN:	return stub.len;
	case OPENBUS_STUB_REG_SCRATCH:	return stub.scratch;
	case OPENBUS_STUB_REG_RUNS:	return stub.runs;
	case OPENBUS_STUB_REG_COPIED:	return stub.copied;
	}

	/* Nothing driving this address. */
	return 0xffffffffu;
}

static void
stub_reg_write(void *p, uint32_t offset, openbus_size size, uint32_t val)
{
	(void) p;
	(void) size;

	switch (offset & ~3u) {
	case OPENBUS_STUB_REG_CTRL:
		/* Reset first and return: a write that both resets and asks for
		   something else is a contradiction, and honouring the reset is the
		   less surprising reading. Self-clearing, so the bit never sticks. */
		if (val & OPENBUS_STUB_CTRL_RESET) {
			openbus_master_set_irq(0);
			openbus_master_set_fiq(0);
			stub_reset(NULL);
			return;
		}

		stub.ctrl = val & (OPENBUS_STUB_CTRL_IRQ | OPENBUS_STUB_CTRL_FIQ |
		                   OPENBUS_STUB_CTRL_COPY);

		/* The interrupt lines follow the control bits directly, which is what
		   makes this useful as a test of the wire. */
		openbus_master_set_irq((val & OPENBUS_STUB_CTRL_IRQ) != 0);
		openbus_master_set_fiq((val & OPENBUS_STUB_CTRL_FIQ) != 0);

		if ((val & OPENBUS_STUB_CTRL_COPY) && stub.len > 0) {
			stub.busy = 1;
			stub.done = 0;
			stub.copied = 0;
		}
		break;

	case OPENBUS_STUB_REG_SRC:	stub.src = val; break;
	case OPENBUS_STUB_REG_DST:	stub.dst = val; break;
	case OPENBUS_STUB_REG_LEN:	stub.len = val; break;
	case OPENBUS_STUB_REG_SCRATCH:	stub.scratch = val; break;

	default:
		/* Read-only or unimplemented. Dropped, as an undriven write is. */
		break;
	}
}

static int
stub_run(void *p, int cycles)
{
	int budget;
	int used = 0;

	(void) p;

	stub.runs++;

	if (!stub.busy || cycles <= 0) {
		return 0;
	}

	budget = cycles;
	while (budget > 0 && stub.len > 0) {
		int words = OPENBUS_STUB_WORDS_PER_CYCLE;

		while (words-- > 0 && stub.len > 0) {
			/* The point of the whole exercise: reaching host memory as a bus
			   master, with physical addresses and no ARM MMU involved. */
			const uint32_t word = openbus_master_read32(stub.src);

			openbus_master_write32(stub.dst, word);
			stub.src += 4;
			stub.dst += 4;
			stub.len--;
			stub.copied++;
		}
		budget--;
		used++;
	}

	if (stub.len == 0) {
		stub.busy = 0;
		stub.done = 1;
		stub.ctrl &= ~(uint32_t) OPENBUS_STUB_CTRL_COPY;
	}

	/* Cycles used are charged against the ARM's budget by the caller, which is
	   how a card holding the bus stalls the host. A copy of any size therefore
	   costs the guest real time rather than being free. */
	return used;
}

static const openbus_master stub_master = {
	.name = "OPEN Bus test card (stub)",
	.p = NULL,
	.reg_read = stub_reg_read,
	.reg_write = stub_reg_write,
	.run = stub_run,
	.reset = stub_reset,
	.close = NULL,		/* state is static, so there is nothing to free */
};

int
openbus_stub_fit(void)
{
	memset(&stub, 0, sizeof(stub));
	return openbus_fit(&stub_master);
}
