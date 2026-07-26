/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

#ifndef PODULES_H
#define PODULES_H

#include <stdint.h>

#include "peripheral_snapshot.h"
#include "podule_api.h"

/* RPCEmu historically used PoduleIoType; it is now an alias for the canonical
   podule_io_type defined by the podule plugin ABI in podule_api.h. */
typedef podule_io_type PoduleIoType;

void podules_write32(int num, PoduleIoType io_type, uint32_t addr, uint32_t val);
void podules_write16(int num, PoduleIoType io_type, uint32_t addr, uint16_t val);
void podules_write8(int num, PoduleIoType io_type, uint32_t addr, uint8_t val);
uint32_t podules_read32(int num, PoduleIoType io_type, uint32_t addr);
uint16_t podules_read16(int num, PoduleIoType io_type, uint32_t addr);
uint8_t  podules_read8(int num, PoduleIoType io_type, uint32_t addr);

typedef struct podule {
	/* Legacy native dispatch, used by addpodule() (podulerom, network). */
	void (*writeb)(struct podule *p, PoduleIoType io_type, uint32_t addr, uint8_t val);
	void (*writew)(struct podule *p, PoduleIoType io_type, uint32_t addr, uint16_t val);
	void (*writel)(struct podule *p, PoduleIoType io_type, uint32_t addr, uint32_t val);
	uint8_t  (*readb)(struct podule *p, PoduleIoType io_type, uint32_t addr);
	uint16_t (*readw)(struct podule *p, PoduleIoType io_type, uint32_t addr);
	uint32_t (*readl)(struct podule *p, PoduleIoType io_type, uint32_t addr);
	int (*timercallback)(struct podule *p);
	void (*reset)(struct podule *p);
	int irq;
	int fiq;
	int msectimer;

	/* Plugin-ABI dispatch (Arculator-style). When api.header is non-NULL this
	   slot is driven through api.header->functions instead of the legacy
	   pointers above. */
	podule_t api;

	/* Microsecond timer for plugin-ABI podules. Drives functions.run() and
	   backs the set/get/stop_timer host callbacks. timer_remaining_us counts
	   down to the next run() call; timer_elapsed_us accumulates the real time
	   since the last run() call (passed to run() as timeslice_us). */
	int64_t timer_remaining_us;
	int64_t timer_elapsed_us;
	int     timer_enabled;

	/* User config slot (0..PODULE_CONFIG_SLOTS-1) this header podule was
	   installed from. Used as the stable config-section index so the GUI and
	   the running device agree regardless of backplane install order. */
	int config_slot;
} podule;

void podule_fiq_raise(podule *p);
void podule_fiq_lower(podule *p);

void podule_irq_raise(podule *p);
void podule_irq_lower(podule *p);

/* Backplane slot a podule was given (0-7), or -1 if not one of ours. */
extern int podule_slot_number(const podule *p);

podule *addpodule(void (*writel)(podule *p, PoduleIoType io_type, uint32_t addr, uint32_t val),
              void (*writew)(podule *p, PoduleIoType io_type, uint32_t addr, uint16_t val),
              void (*writeb)(podule *p, PoduleIoType io_type, uint32_t addr, uint8_t val),
              uint32_t (*readl)(podule *p, PoduleIoType io_type, uint32_t addr),
              uint16_t (*readw)(podule *p, PoduleIoType io_type, uint32_t addr),
              uint8_t  (*readb)(podule *p, PoduleIoType io_type, uint32_t addr),
              int (*timercallback)(podule *p),
              void (*reset)(podule *p));

void runpoduletimers(int t);
void podules_reset(void);
void podules_get_snapshot(PodulesStateSnapshot *snapshot);

/* Plugin-ABI registry (Arculator-compatible podules) */
void podule_build_list(void);
const podule_header_t *podule_find(const char *short_name);
void podules_init_headers(void);

/* Enumerate registered podules for UI selection */
int podule_get_available_count(void);
const char *podule_get_available_short_name(int idx);
const char *podule_get_available_name(int idx);
int podule_get_available_has_config(int idx);

extern const podule_callbacks_t podule_callbacks_def;

#endif
