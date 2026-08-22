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

#ifndef COPRO_BUS_H
#define COPRO_BUS_H

#include <stdint.h>

#include "cpu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The co-processor card's address decode: what a core's loads and stores mean.
 *
 * ★ WHY THIS EXISTS. The cores index their RAM as a flat array, which is right
 * for running a program that only talks to itself, and useless for emulating a
 * machine. A Spectrum's ULA answers ports; a BBC's SHEILA is memory-mapped; a
 * Mac sizes its own memory by deliberately touching addresses that are not
 * there. None of that is expressible as "the address space is an array", so a
 * core's accesses come through here instead, and the guest says what each part
 * of the address space is.
 *
 * ★ WHAT IT DELIBERATELY IS NOT: a callback into RISC OS. The core runs on the
 * host side of the emulator while the ARM is stalled, so there is no guest code
 * to call and no stack to call it on. That rules out the design everyone reaches
 * for first, and it is why writes are LOGGED and reads come from a LATCH the
 * guest fills in ahead of time: both sides get to run without waiting for the
 * other. The one case that cannot be done that way - a register whose read has a
 * side effect - stops the core instead, and the guest answers it. That is
 * correct and comparatively slow, which is the trade the caller chooses per
 * region.
 *
 * ★ EVERYTHING HERE IS BYTE-WIDE. Every core decomposes a wider access into
 * bytes at the bottom, so one byte path means one place for the decode to be
 * right. A 68000 word access is two calls, in the order the 68000 makes them.
 *
 * The control area is memory the CARD owns and the core cannot see: region
 * tables, the write log and the latch pages live there. Without it they would
 * have to sit in the core's own address space, and on a 6502 or a Z80 that space
 * is 64K which the machine being emulated needs all of.
 */

/** What a region of the core's address space is. */
typedef enum {
	COPRO_REGION_RAM = 0,	/**< plain memory: reads and writes hit card RAM */
	COPRO_REGION_ROM,	/**< reads hit card RAM; writes are logged and dropped */
	COPRO_REGION_LOG,	/**< reads hit card RAM; writes hit it and are logged */
	COPRO_REGION_LATCH,	/**< reads come from the latch page; writes are logged only */
	COPRO_REGION_STALL,	/**< both stop the core for the guest to answer */
	COPRO_REGION_UNMAPPED,	/**< nothing is there: reads give 0xff and a bus error */
	COPRO_REGION_KIND_COUNT
} copro_region_kind;

/**
 * Set in a region's kind field to put it in the PORT space rather than the
 * memory space. Only the Z80 has a separate 64K of I/O, and this is how it is
 * described without a second table.
 */
#define COPRO_REGION_PORT	0x100u

/**
 * Set in a region's kind field to say that the region is a WINDOW onto card RAM
 * at the offset its `latch` field gives, rather than lying at its own address.
 *
 * ★ THIS IS HOW BANKING WORKS, and banking is the only way an 8-bit core gets
 * more than 64K. A 6502 cannot form an address above &FFFF, so a flat space
 * larger than that would be memory no instruction could name - which is why a
 * Spectrum 128, a BBC's sideways ROMs and a C64's REU all page a larger store
 * through a window instead. Give the card 512K, describe &8000-&BFFF as a window
 * at offset 0, and switching bank is one word written into this field: the same
 * addresses then read and write a different part of card RAM.
 *
 * Without the flag a region's data lies at its own address in card RAM, which is
 * what everything did before banking existed and what a machine with no paging
 * wants.
 *
 * Meaningless on a LATCH region, whose `latch` field already points into the
 * control area, and on UNMAPPED and STALL regions, which have no storage at all.
 */
#define COPRO_REGION_OFFSET	0x200u

/** The kind, with the flags masked off. */
#define COPRO_REGION_KIND(k)	((k) & 0xffu)

/**
 * One entry of the region table, as the guest writes it into the control area.
 * Sixteen bytes, naturally aligned, little-endian - the same shape on the host
 * and in the guest, so neither side has to pack anything.
 */
typedef struct {
	uint32_t base;		/**< first address in the core's space */
	uint32_t size;		/**< bytes; zero disables the entry */
	uint32_t kind;		/**< copro_region_kind, optionally | COPRO_REGION_PORT */
	/**
	 * What this means depends on the kind: for a LATCH region it is the offset
	 * in the CONTROL AREA that reads come from, and for a RAM, ROM or LOG
	 * region carrying COPRO_REGION_OFFSET it is the offset in CARD RAM that the
	 * region's base maps to. One field with two meanings because no region has
	 * both, and because a fifth word would take the entry off a sixteen-byte
	 * boundary for a field most maps never set.
	 */
	uint32_t latch;
} copro_region;

/**
 * One logged write. Also sixteen bytes: the guest drains these in bulk and
 * wants to walk them without unpacking.
 *
 * The cycle is what makes a raster-accurate display possible: the guest learns
 * not only that the border colour changed but when, so it knows which scanline
 * the change belongs to.
 */
typedef struct {
	uint32_t cycle;		/**< the core's cycle count when the write happened */
	uint32_t addr;		/**< the address written, in the core's space */
	uint32_t value;		/**< the byte written */
	uint32_t info;		/**< COPRO_LOG_* below */
} copro_log_entry;

#define COPRO_LOG_INFO_PORT	0x01u	/**< the port space, not memory */
#define COPRO_LOG_INFO_DROPPED	0x02u	/**< writes were lost before this one */

/** What an access did. */
typedef enum {
	COPRO_BUS_OK = 0,	/**< done; a read's value is in *value */
	COPRO_BUS_STALL,	/**< the core must stop; the guest will answer */
	COPRO_BUS_BUSERROR	/**< nothing is mapped there */
} copro_bus_result;

typedef struct {
	/* The core's address space. Not owned. */
	uint8_t *ram;
	uint32_t ram_size;

	/* The card's own memory, invisible to the core. Not owned. */
	uint8_t *ctl;
	uint32_t ctl_size;

	/* The region table, as an offset into the control area. A count of zero
	   means no map has been set, and then everything is plain RAM - which is
	   what the three cores that predate this expect. */
	uint32_t map_off;
	uint32_t map_count;

	/* The write log ring, as an offset into the control area. */
	uint32_t log_off;
	uint32_t log_entries;
	uint32_t log_head;	/**< the card writes here */
	uint32_t log_tail;	/**< the guest has read up to here */
	uint32_t log_dropped;	/**< writes lost because the guest did not drain */

	/* A stalled access, waiting for the guest. */
	int stalled;
	uint32_t wait_addr;
	uint32_t wait_value;	/**< the value written, or the guest's answer to a read */
	uint32_t wait_is_write;
	uint32_t wait_is_port;

	/*
	 * The guest's answer, waiting for the access to come round again.
	 *
	 * A core cannot stop half way through an instruction, so a stalled
	 * instruction is abandoned and retried whole (see cpu_mem.h). The retry
	 * repeats the access that stalled, and it must not stall a second time or
	 * nothing would ever get past it. So the answer is held here and consumed
	 * by the next access to that same address - once, because an instruction
	 * that reads a register twice is making two accesses and deserves two
	 * answers.
	 */
	int answer_valid;
	uint32_t answer_addr;
	uint32_t answer_value;
	uint32_t answer_is_write;
} copro_bus;

/**
 * Attach a bus to a core's RAM and the card's control area. Neither is owned.
 * The control area may be NULL, in which case no map, log or latch is possible
 * and every access is plain RAM.
 */
void copro_bus_init(copro_bus *bus, uint8_t *ram, uint32_t ram_size,
                    uint8_t *ctl, uint32_t ctl_size);

/**
 * Point the bus at a region table in the control area. @count of zero removes
 * the map. Returns 0 if the table would not fit, leaving the previous map alone.
 */
int copro_bus_set_map(copro_bus *bus, uint32_t offset, uint32_t count);

/**
 * Point the bus at a write log ring in the control area. @entries of zero
 * removes it and writes stop being logged. Returns 0 if it would not fit.
 */
int copro_bus_set_log(copro_bus *bus, uint32_t offset, uint32_t entries);

/** How many logged writes the guest has not drained. */
uint32_t copro_bus_log_pending(const copro_bus *bus);

/**
 * Read one logged write into @out. Returns 0 when the log is empty. Advances
 * the tail, so a caller drains by looping until it returns 0.
 */
int copro_bus_log_pop(copro_bus *bus, copro_log_entry *out);

/** Read one byte for the core. @cycle is only used if the access is logged. */
copro_bus_result copro_bus_read(copro_bus *bus, uint32_t addr, int is_port,
                                uint32_t cycle, uint8_t *value);

/** Write one byte for the core. */
copro_bus_result copro_bus_write(copro_bus *bus, uint32_t addr, int is_port,
                                 uint32_t cycle, uint8_t value);

/**
 * Answer a stalled access: @value is the byte a stalled READ should return, and
 * is ignored for a stalled write. Clears the stall. Returns the value the core
 * should see.
 */
uint8_t copro_bus_resume(copro_bus *bus, uint8_t value);

/**
 * Fill in a core's memory hook so its accesses come through this bus.
 * @can_stall is set from whether the map has any stalling region in it, which is
 * what lets a core skip saving its registers when nothing can ever be abandoned.
 */
void copro_bus_hook(copro_bus *bus, cpu_mem_hook *hook);

/** Does the map contain a region that can stall? */
int copro_bus_can_stall(const copro_bus *bus);

/** The region covering an address, or NULL if the map does not cover it. */
const copro_region *copro_bus_region(const copro_bus *bus, uint32_t addr, int is_port);

#ifdef __cplusplus
}
#endif

#endif
