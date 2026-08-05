/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Loadable-podule support via the Win32 dynamic loader. Shim the handful of
   dlfcn calls the loader below uses onto LoadLibrary/GetProcAddress. */
#include <windows.h>
#define RTLD_NOW 0
#define PODULE_DLL_EXT "dll"

static void *
dlopen(const char *path, int flag)
{
	(void) flag;
	return (void *) LoadLibraryA(path);
}

static void *
dlsym(void *lib, const char *symbol)
{
	return (void *) (uintptr_t) GetProcAddress((HMODULE) lib, symbol);
}

static int
dlclose(void *lib)
{
	return FreeLibrary((HMODULE) lib) ? 0 : -1;
}
#else
#include <dlfcn.h>
#define PODULE_DLL_EXT "so"
#endif

#include "rpcemu.h"
#include "iomd.h"
#include "podules.h"
#include "podule_config.h"

/* References
  Acorn Enhanced Expansion Card Specification
  Risc PC Technical Reference Manual
*/

/*Podules -
  0 is reserved for extension ROMs
  1 is for additional IDE interface
  2-3 are free
  4-7 are not implemented (yet)*/
static podule podules[8];
static int freepodule;

#ifndef container_of
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Buffer size for a "<short_name>.<slot>" config section name */
#define SECTION_NAME_LEN 40

static void rethinkpoduleints(void);

/* ----------------------------------------------------------------------------
 * Plugin-ABI registry (Arculator-compatible podules)
 *
 * Each device exposes a <name>_probe() that returns a podule_header_t. The
 * registry collects available headers; podules_init_headers() installs the
 * configured ones into slots and dispatches through header->functions.
 * -------------------------------------------------------------------------- */

extern const podule_header_t *aka05_probe(const podule_callbacks_t *callbacks, char *path);
extern const podule_header_t *midimax_probe(const podule_callbacks_t *callbacks, char *path);
extern const podule_header_t *aka16_probe(const podule_callbacks_t *callbacks, char *path);
extern const podule_header_t *aka12_probe(const podule_callbacks_t *callbacks, char *path);
extern const podule_header_t *lark_probe(const podule_callbacks_t *callbacks, char *path);

static const podule_header_t *(*internal_podules[])(const podule_callbacks_t *callbacks, char *path) =
{
	aka05_probe,
	midimax_probe,
	aka16_probe,
	aka12_probe,
	lark_probe,
};
#define NR_INTERNAL_PODULES (sizeof(internal_podules) / sizeof(internal_podules[0]))

typedef struct podule_list {
	const podule_header_t *header;
	struct podule_list *next;
} podule_list;

static podule_list *podule_list_head = NULL;

/* Base path handed to probe()/init() so devices can locate their ROM files. */
static char podule_resource_path[512];

static void
podule_add(const podule_header_t *header)
{
	podule_list *current = malloc(sizeof(podule_list));

	if (current == NULL) {
		return;
	}
	current->header = header;
	current->next = podule_list_head;
	podule_list_head = current;
}

/* Validate a podule header's API version and return the flags it may legally
   set. Zero means the podule is unsupported and must be rejected. */
static uint32_t
podule_validate_and_get_valid_flags(const podule_header_t *header)
{
	switch (header->version) {
	case MAKE_PODULE_API_VERSION(1, 1):
		return PODULE_FLAGS_VALID;
	case MAKE_PODULE_API_VERSION(1, 0):
		return PODULE_FLAGS_VALID_VERSION_1_0;
	default:
		rpclog("podules: '%s' has unsupported API version %u.%u\n",
		       header->short_name,
		       PODULE_API_VERSION_GET_MAJOR(header->version),
		       PODULE_API_VERSION_GET_MINOR(header->version));
		return 0;
	}
}

/* Keep dlopen handles for the process lifetime (podules stay loaded). */
typedef struct dll_list {
	void *lib;
	struct dll_list *next;
} dll_list;

static dll_list *dll_list_head = NULL;

/* Discover external podule plugins: each is a shared library at
   <resourcedir>/podules/<name>/<name>.so exporting podule_probe(). Mirrors
   Arculator's loader, so podules built against podule_api.h load unchanged. */
static void
load_external_podules(void)
{
	char podules_dir[512];
	DIR *dirp;
	struct dirent *dp;

	snprintf(podules_dir, sizeof(podules_dir), "%spodules", podule_resource_path);

	dirp = opendir(podules_dir);
	if (!dirp) {
		return; /* no podules directory - nothing to load */
	}

	while ((dp = readdir(dirp)) != NULL) {
		const podule_header_t *(*probe)(const podule_callbacks_t *, char *);
		const podule_header_t *header;
		char so_path[1024], probe_path[768];
		void *lib;
		dll_list *entry;

		if (dp->d_name[0] == '.') {
			continue;
		}

		snprintf(so_path, sizeof(so_path), "%s/%s/%s." PODULE_DLL_EXT,
		         podules_dir, dp->d_name, dp->d_name);

		lib = dlopen(so_path, RTLD_NOW);
		if (lib == NULL) {
			continue; /* not a loadable podule (e.g. ROM-only dir) */
		}

		probe = (const podule_header_t *(*)(const podule_callbacks_t *, char *))
		    dlsym(lib, "podule_probe");
		if (probe == NULL) {
			rpclog("podules: '%s' has no podule_probe\n", dp->d_name);
			dlclose(lib);
			continue;
		}

		snprintf(probe_path, sizeof(probe_path), "%s/%s/", podules_dir, dp->d_name);
		header = probe(&podule_callbacks_def, probe_path);
		if (!header) {
			rpclog("podules: probe failed for '%s'\n", dp->d_name);
			dlclose(lib);
			continue;
		}

		/* A library may expose several headers (PODULE_FLAGS_NEXT). */
		for (;;) {
			uint32_t valid_flags = podule_validate_and_get_valid_flags(header);
			uint32_t flags = header->flags;

			if (valid_flags == 0 || (flags & ~valid_flags)) {
				rpclog("podules: '%s' failed validation\n", header->short_name);
				break;
			}

			rpclog("podules: loaded external podule '%s'\n", header->short_name);
			podule_add(header);

			if (!(flags & PODULE_FLAGS_NEXT)) {
				break;
			}
			header++;
		}

		entry = malloc(sizeof(dll_list));
		if (entry) {
			entry->lib = lib;
			entry->next = dll_list_head;
			dll_list_head = entry;
		}
	}

	(void)closedir(dirp);
}

void
podule_build_list(void)
{
	size_t c;

	/* Idempotent - the list is built once and reused (e.g. by the GUI). */
	if (podule_list_head != NULL) {
		return;
	}

	snprintf(podule_resource_path, sizeof(podule_resource_path), "%s",
	         rpcemu_get_resourcedir());

	for (c = 0; c < NR_INTERNAL_PODULES; c++) {
		const podule_header_t *header =
		    internal_podules[c](&podule_callbacks_def, podule_resource_path);

		if (header) {
			podule_add(header);
		}
	}

	load_external_podules();
}

const podule_header_t *
podule_find(const char *short_name)
{
	podule_list *current = podule_list_head;

	while (current) {
		if (strcmp(short_name, current->header->short_name) == 0) {
			return current->header;
		}
		current = current->next;
	}
	return NULL;
}

/* Enumerate registered podules for UI selection. */
static const podule_header_t *
podule_get_available(int idx)
{
	podule_list *current = podule_list_head;

	podule_build_list(); /* ensure built if GUI asks before machine start */
	current = podule_list_head;
	while (current && idx-- > 0) {
		current = current->next;
	}
	return current ? current->header : NULL;
}

int
podule_get_available_count(void)
{
	podule_list *current;
	int count = 0;

	podule_build_list();
	for (current = podule_list_head; current; current = current->next) {
		count++;
	}
	return count;
}

const char *
podule_get_available_short_name(int idx)
{
	const podule_header_t *header = podule_get_available(idx);

	return header ? header->short_name : NULL;
}

const char *
podule_get_available_name(int idx)
{
	const podule_header_t *header = podule_get_available(idx);

	return header ? header->name : NULL;
}

int
podule_get_available_has_config(int idx)
{
	const podule_header_t *header = podule_get_available(idx);

	return (header && header->config) ? 1 : 0;
}

/* ----------------------------------------------------------------------------
 * Host callbacks (services the host provides to plugin-ABI podules)
 * -------------------------------------------------------------------------- */

static void
cb_set_irq(podule_t *p, int state)
{
	podule *slot = container_of(p, podule, api);

	slot->irq = state;
	rethinkpoduleints();
}

static void
cb_set_fiq(podule_t *p, int state)
{
	podule *slot = container_of(p, podule, api);

	slot->fiq = state;
	rethinkpoduleints();
}

/* Timer callbacks. These back the one-shot timer API; free-running devices
   instead return a reload delay from functions.run(). Both share the per-slot
   microsecond timer serviced by runpoduletimers(). Servicing granularity is the
   podule tick (currently 2ms), which the ABI explicitly tolerates (>= 1ms);
   run() always receives the true elapsed time so device timing stays accurate. */
static void
cb_set_timer_delay_us(podule_t *p, int delay_us)
{
	podule *slot = container_of(p, podule, api);

	slot->timer_remaining_us = (delay_us > 0) ? delay_us : 0;
	slot->timer_enabled = 1;
}

static int
cb_get_timer_remaining_us(podule_t *p)
{
	podule *slot = container_of(p, podule, api);

	if (slot->timer_enabled && slot->timer_remaining_us > 0) {
		return (int) slot->timer_remaining_us;
	}
	return 0;
}

static void
cb_stop_timer(podule_t *p)
{
	podule *slot = container_of(p, podule, api);

	slot->timer_enabled = 0;
}

/* Config store. Keyed by a per-instance section "<short_name>.<slot>", backed
   by the generic podule_cfg store (persisted to the machine .cfg by the GUI). */
static void
build_section(podule_t *p, char *buf, size_t len)
{
	podule *slot = container_of(p, podule, api);
	const char *name = slot->api.header ? slot->api.header->short_name : "?";

	/* Key by the stable user config slot, not the backplane slot index. */
	snprintf(buf, len, "%s.%d", name, slot->config_slot);
}

static int
cb_config_get_int(podule_t *p, const char *name, int def)
{
	char section[SECTION_NAME_LEN];

	build_section(p, section, sizeof(section));
	return podule_cfg_get_int(section, name, def);
}

static const char *
cb_config_get_string(podule_t *p, const char *name, const char *def)
{
	char section[SECTION_NAME_LEN];

	build_section(p, section, sizeof(section));
	return podule_cfg_get_string(section, name, def);
}

static void
cb_config_set_int(podule_t *p, const char *name, int val)
{
	char section[SECTION_NAME_LEN];

	build_section(p, section, sizeof(section));
	podule_cfg_set_int(section, name, val);
}

static void
cb_config_set_string(podule_t *p, const char *name, char *val)
{
	char section[SECTION_NAME_LEN];

	build_section(p, section, sizeof(section));
	podule_cfg_set_string(section, name, val);
}

const podule_callbacks_t podule_callbacks_def =
{
	.set_irq = cb_set_irq,
	.set_fiq = cb_set_fiq,
	.set_timer_delay_us = cb_set_timer_delay_us,
	.get_timer_remaining_us = cb_get_timer_remaining_us,
	.stop_timer = cb_stop_timer,
	.config_get_int = cb_config_get_int,
	.config_get_string = cb_config_get_string,
	.config_set_int = cb_config_set_int,
	.config_set_string = cb_config_set_string,
	/* config_open / config_file_selector / config_get_current /
	   config_set_current are GUI services added in a later phase. */
};

/* ----------------------------------------------------------------------------
 * Header podule installation
 * -------------------------------------------------------------------------- */

/* Install a header podule at an explicit backplane slot (0-7), so the dialog's
   "Slot N" maps directly to expansion-card N. Skips the slot if it is already
   occupied (e.g. by a built-in extension-ROM or network podule). */
static void
add_header_podule(const podule_header_t *header, int backplane_slot)
{
	podule *slot;

	if (backplane_slot < 0 || backplane_slot >= 8) {
		return;
	}

	/*
	 * The built-in cards own the low slots and are not up for negotiation.
	 * An old configuration may still name one - they used to be allocated in
	 * call order and a user podule could land there - so this is a refusal
	 * rather than an assertion.
	 */
	if (backplane_slot < PODULE_CONFIG_FIRST_USER_SLOT) {
		rpclog("podules: slot %d belongs to a built-in card, so '%s' is not "
		       "fitted; move it to slot %d or above\n", backplane_slot,
		       header->short_name, PODULE_CONFIG_FIRST_USER_SLOT);
		return;
	}

	slot = &podules[backplane_slot];
	if (slot->api.header != NULL ||
	    slot->readb != NULL || slot->readw != NULL || slot->readl != NULL ||
	    slot->writeb != NULL || slot->writew != NULL || slot->writel != NULL) {
		rpclog("podules: slot %d already occupied, skipping '%s'\n",
		       backplane_slot, header->short_name);
		return;
	}

	memset(slot, 0, sizeof(*slot));
	slot->api.header = header;
	slot->api.p = NULL;
	slot->config_slot = backplane_slot;

	if (header->functions.init) {
		if (header->functions.init(&slot->api) != 0) {
			rpclog("podules: failed to init '%s'\n", header->short_name);
			slot->api.header = NULL;
			return;
		}
	}

	/* Devices with a run() callback are free-running: start the timer so the
	   first tick calls run(), which then returns its own reload delay. */
	if (header->functions.run) {
		slot->timer_enabled = 1;
		slot->timer_remaining_us = 0;
		slot->timer_elapsed_us = 0;
	}

	rpclog("podules: installed '%s' in slot %d\n", header->short_name, backplane_slot);
}

/**
 * Install configured plugin-ABI podules into free slots.
 *
 * Called from resetrpc() after the legacy podules (extension ROM, network)
 * have claimed their slots.
 *
 * The configured podules come from the podule_cfg slot assignment (loaded from
 * the machine .cfg), so which podules are present is user-selectable.
 */
void
podules_init_headers(void)
{
	int i;

	for (i = 0; i < PODULE_CONFIG_SLOTS; i++) {
		const char *short_name = podule_cfg_get_slot(i);
		const podule_header_t *header;

		if (short_name == NULL || short_name[0] == '\0') {
			continue;
		}

		header = podule_find(short_name);
		if (header) {
			add_header_podule(header, i);
		} else {
			rpclog("podules: configured podule '%s' not found\n", short_name);
		}
	}
}

void
podules_get_snapshot(PodulesStateSnapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	for (int i = 0; i < 8; i++) {
		if (podules[i].readl != NULL || podules[i].readw != NULL || podules[i].readb != NULL ||
		    podules[i].writel != NULL || podules[i].writew != NULL || podules[i].writeb != NULL ||
		    podules[i].timercallback != NULL || podules[i].reset != NULL ||
		    podules[i].api.header != NULL) {
			snapshot->slot_used[i] = 1;
		}
		snapshot->irq[i] = (uint8_t) (podules[i].irq != 0);
		snapshot->fiq[i] = (uint8_t) (podules[i].fiq != 0);
	}
}

/**
 * Reset and empty all the podule slots
 *
 * Safe to call on program startup and user instigated virtual machine
 * reset.
 */
void
podules_reset(void)
{
	int c;

	/* Call any reset functions that an open podule may have to allow
	   then to tidy open files etc */
	for (c = 0; c < 8; c++) {
		if (podules[c].reset != NULL) {
			podules[c].reset(&podules[c]);
		}
		/* Plugin-ABI podules free their instance data in close() */
		if (podules[c].api.header != NULL &&
		    podules[c].api.header->functions.close != NULL) {
			podules[c].api.header->functions.close(&podules[c].api);
		}
	}

	// Blank all 8 podules
	memset(podules, 0, 8 * sizeof(podule));

	freepodule = 0;
}

/**
 * Add a new podule to the chain, with the specified functions, including reads and
 * writes to the podules memory areas, and calls for regular callbacks and reset.
 *
 * @param writel        Function pointer for the podule's 32-bit write function
 * @param writew        Function pointer for the podule's 16-bit write function
 * @param writeb        Function pointer for the podule's  8-bit write function
 * @param readl         Function pointer for the podule's 32-bit read function
 * @param readw         Function pointer for the podule's 16-bit read function
 * @param readb         Function pointer for the podule's  8-bit read function
 * @param timercallback
 * @param reset         Function pointer for the podule's reset function, called
 *                      at program startup and emulated machine reset
 * @return Pointer to entry in the podules array, or NULL on failure
 */
podule *
addpodule(void (*writel)(podule *p, PoduleIoType io_type, uint32_t addr, uint32_t val),
          void (*writew)(podule *p, PoduleIoType io_type, uint32_t addr, uint16_t val),
          void (*writeb)(podule *p, PoduleIoType io_type, uint32_t addr, uint8_t val),
          uint32_t (*readl)(podule *p, PoduleIoType io_type, uint32_t addr),
          uint16_t (*readw)(podule *p, PoduleIoType io_type, uint32_t addr),
          uint8_t  (*readb)(podule *p, PoduleIoType io_type, uint32_t addr),
          int (*timercallback)(podule *p),
          void (*reset)(podule *p))
{
	if (freepodule == 8) {
		return NULL; // All podules in use!
	}

	podules[freepodule].readl = readl;
	podules[freepodule].readw = readw;
	podules[freepodule].readb = readb;
	podules[freepodule].writel = writel;
	podules[freepodule].writew = writew;
	podules[freepodule].writeb = writeb;
	podules[freepodule].timercallback = timercallback;
	podules[freepodule].reset = reset;

	return &podules[freepodule++];
}

/**
 * Backplane slot a podule was given.
 *
 * A card whose framestore lives in its own EASI space has to be able to work out
 * where that is in the physical map, and the slot is what decides it.
 *
 * @param p Podule returned by addpodule()
 * @return Slot number 0-7, or -1 if this is not one of ours
 */
int
podule_slot_number(const podule *p)
{
	if (p < &podules[0] || p >= &podules[8]) {
		return -1;
	}

	return (int) (p - &podules[0]);
}

/*
 * The OPEN Bus second bus master's contribution to the podule interrupt line.
 *
 * The Risc PC TRM routes a second master's interrupt down nPIRQ, the same line
 * the podules share: "The use of the podule bus interrupt lines allows easy
 * software integration for second bus master interrupt servicing. An interrupt
 * flag register must be present in the bus interface ASIC to allow the system to
 * identify whether the interrupt was generated by a podule, or the second bus
 * master." Telling them apart is therefore the card's job, through registers it
 * provides; aggregating them is ours, and it happens here so there is one place
 * that decides the IOMD status.
 */
static int openbus_irq, openbus_fiq;

void
podules_set_openbus_irq(int state)
{
	openbus_irq = state ? 1 : 0;
	rethinkpoduleints();
}

void
podules_set_openbus_fiq(int state)
{
	openbus_fiq = state ? 1 : 0;
	rethinkpoduleints();
}

/**
 * Raise interrupts if any podules, or the OPEN Bus second bus master, have
 * requested them.
 */
static void
rethinkpoduleints(void)
{
	int c;

	iomd.irqb.status &= ~(IOMD_IRQB_PODULE | IOMD_IRQB_PODULE_FIQ_AS_IRQ);
	iomd.fiq.status  &= ~IOMD_FIQ_PODULE;

	if (openbus_irq) {
		iomd.irqb.status |= IOMD_IRQB_PODULE;
	}
	if (openbus_fiq) {
		iomd.irqb.status |= IOMD_IRQB_PODULE_FIQ_AS_IRQ;
		iomd.fiq.status  |= IOMD_FIQ_PODULE;
	}

	for (c = 0; c < 8; c++) {
		if (podules[c].irq) {
			iomd.irqb.status |= IOMD_IRQB_PODULE;
		}
		if (podules[c].fiq) {
			iomd.irqb.status |= IOMD_IRQB_PODULE_FIQ_AS_IRQ;
			iomd.fiq.status  |= IOMD_FIQ_PODULE;
		}
	}
	updateirqs();
}

/**
 * Raise FIQ for the specified podule.
 *
 * @param p Pointer to 'podule' struct for the specified Podule
 */
void
podule_fiq_raise(podule *p)
{
	p->fiq = 1;
	rethinkpoduleints();
}

/**
 * Clear FIQ for the specified podule.
 *
 * @param p Pointer to 'podule' struct for the specified Podule
 */
void
podule_fiq_lower(podule *p)
{
	p->fiq = 0;
	rethinkpoduleints();
}

/**
 * Raise IRQ for the specified podule.
 *
 * @param p Pointer to 'podule' struct for the specified Podule
 */
void
podule_irq_raise(podule *p)
{
	p->irq = 1;
	rethinkpoduleints();
}

/**
 * Clear IRQ for the specified podule.
 *
 * @param p Pointer to 'podule' struct for the specified Podule
 */
void
podule_irq_lower(podule *p)
{
	p->irq = 0;
	rethinkpoduleints();
}

/**
 * Handle a 32-bit write to the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Write to IOC, MEMC or EASI space
 * @param addr    Address to write to
 * @param val     Value to write
 */
void
podules_write32(int num, PoduleIoType io_type, uint32_t addr, uint32_t val)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;

	if (podules[num].api.header != NULL) {
		if (podules[num].api.header->functions.write_l) {
			podules[num].api.header->functions.write_l(&podules[num].api, io_type, addr, val);
		}
	} else if (podules[num].writel != NULL) {
		podules[num].writel(&podules[num], io_type, addr, val);
	}
	if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
		rethinkpoduleints();
	}
}

/**
 * Handle a 16-bit write to the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Write to IOC, MEMC or EASI space
 * @param addr    Address to write to
 * @param val     Value to write
 */
void
podules_write16(int num, PoduleIoType io_type, uint32_t addr, uint16_t val)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;

	if (podules[num].api.header != NULL) {
		if (podules[num].api.header->functions.write_w) {
			podules[num].api.header->functions.write_w(&podules[num].api, io_type, addr, val);
		}
	} else if (podules[num].writew != NULL) {
		podules[num].writew(&podules[num], io_type, addr, val);
	}
	if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
		rethinkpoduleints();
	}
}

/**
 * Handle an 8-bit write to the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Write to IOC, MEMC or EASI space
 * @param addr    Address to write to
 * @param val     Value to write
 */
void
podules_write8(int num, PoduleIoType io_type, uint32_t addr, uint8_t val)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;

	if (podules[num].api.header != NULL) {
		if (podules[num].api.header->functions.write_b) {
			podules[num].api.header->functions.write_b(&podules[num].api, io_type, addr, val);
		}
	} else if (podules[num].writeb != NULL) {
		podules[num].writeb(&podules[num], io_type, addr, val);
	}
	if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
		rethinkpoduleints();
	}
}

/**
 * Handle a 32-bit read from the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Read from IOC, MEMC or EASI space
 * @param addr    Address to read from
 * @return Value at memory address
 */
uint32_t
podules_read32(int num, PoduleIoType io_type, uint32_t addr)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;
	uint32_t temp;

	if (podules[num].api.header != NULL) {
		temp = podules[num].api.header->functions.read_l
		    ? podules[num].api.header->functions.read_l(&podules[num].api, io_type, addr)
		    : 0xffffffff;
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	if (podules[num].readl != NULL) {
		temp = podules[num].readl(&podules[num], io_type, addr);
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	return 0xffffffff;
}

/**
 * Handle a 16-bit read from the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Read from IOC, MEMC or EASI space
 * @param addr    Address to read from
 * @return Value at memory address
 */
uint16_t
podules_read16(int num, PoduleIoType io_type, uint32_t addr)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;
	uint16_t temp;

	if (podules[num].api.header != NULL) {
		temp = podules[num].api.header->functions.read_w
		    ? podules[num].api.header->functions.read_w(&podules[num].api, io_type, addr)
		    : 0xffff;
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	if (podules[num].readw != NULL) {
		temp = podules[num].readw(&podules[num], io_type, addr);
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	return 0xffff;
}

/**
 * Handle an 8-bit read from the podules memory map
 *
 * @param num     Podule number (0-7)
 * @param io_type Read from IOC, MEMC or EASI space
 * @param addr    Address to read from
 * @return Value at memory address
 */
uint8_t
podules_read8(int num, PoduleIoType io_type, uint32_t addr)
{
	const int oldirq = podules[num].irq;
	const int oldfiq = podules[num].fiq;
	uint8_t temp;

	if (podules[num].api.header != NULL) {
		temp = podules[num].api.header->functions.read_b
		    ? podules[num].api.header->functions.read_b(&podules[num].api, io_type, addr)
		    : 0xff;
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	if (podules[num].readb != NULL) {
		temp = podules[num].readb(&podules[num], io_type, addr);
		if (podules[num].irq != oldirq || podules[num].fiq != oldfiq) {
			rethinkpoduleints();
		}
		return temp;
	}
	return 0xff;
}

/**
 * Service podule timers. Called every @t milliseconds from gentimerirq().
 *
 * Handles both the legacy millisecond timercallback() and the plugin-ABI
 * microsecond run() callback. The 2ms call granularity satisfies the ABI's
 * "at least 1ms" requirement; run() is always passed the true elapsed time
 * (timer_elapsed_us) so device-side timing integrates correctly despite the
 * coarse polling.
 *
 * @param t Milliseconds elapsed since the previous call
 */
void
runpoduletimers(int t)
{
	const int64_t elapsed_us = (int64_t) t * 1000;
	int c, d;

	/* Loop through podules, ignoring 0 (extn rom) */
	for (c = 1; c < 8; c++) {
		/* Legacy millisecond timer */
		if (podules[c].timercallback != NULL && podules[c].msectimer != 0) {
			podules[c].msectimer -= t;
			d = 1;
			while (podules[c].msectimer <= 0 && d != 0) {
				const int oldirq = podules[c].irq;
				const int oldfiq = podules[c].fiq;

				d = podules[c].timercallback(&podules[c]);
				if (d == 0) {
					podules[c].msectimer = 0;
				} else {
					podules[c].msectimer += d;
				}
				if (podules[c].irq != oldirq || podules[c].fiq != oldfiq) {
					rethinkpoduleints();
				}
			}
		}

		/* Plugin-ABI microsecond run() timer */
		if (podules[c].api.header != NULL &&
		    podules[c].api.header->functions.run != NULL &&
		    podules[c].timer_enabled) {
			podules[c].timer_elapsed_us += elapsed_us;
			podules[c].timer_remaining_us -= elapsed_us;

			if (podules[c].timer_remaining_us <= 0) {
				const int oldirq = podules[c].irq;
				const int oldfiq = podules[c].fiq;
				int reload_us;

				reload_us = podules[c].api.header->functions.run(
				    &podules[c].api, (int) podules[c].timer_elapsed_us);
				podules[c].timer_elapsed_us = 0;

				if (reload_us > 0) {
					podules[c].timer_remaining_us = reload_us;
				} else {
					podules[c].timer_enabled = 0;
				}
				if (podules[c].irq != oldirq || podules[c].fiq != oldfiq) {
					rethinkpoduleints();
				}
			}
		}
	}
}
