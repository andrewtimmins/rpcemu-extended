/*
  RPCEmu - An Acorn system emulator

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

/*
  Podule configuration store.

  A small generic key/value store for podules, plus per-slot podule selection.
  RPCEmu's main configuration is a fixed Config struct; podules instead need an
  open-ended store (arbitrary device-defined keys), so this provides one. The
  core holds the data in memory; the GUI layer (settings.cpp) persists it to the
  machine .cfg, exactly as it does for the fixed Config struct.
 */

#ifndef PODULE_CONFIG_H
#define PODULE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Number of user-configurable podule slots. The Risc PC expansion bus exposes
   eight expansion-card slots; configured podules are installed into the slots
   left free by the built-in extension-ROM and network podules. */
#define PODULE_CONFIG_SLOTS 8

/**
 * The first slot a user may put a podule in.
 *
 * Slots 0 and 1 carry the two cards RPCEmu fits itself: the USB card in 0,
 * because the only RISC OS driver for its chip addresses podule 0 and nothing
 * else, and the support card in 1, which carries HostFS, the scroll wheel and
 * the rest of the module ROM.
 *
 * They are not offered in the interface and are refused if a configuration
 * names them, because a card put on top of either would take away something the
 * machine depends on - and a driver claiming a shared card's interrupt wedges
 * the machine outright, which is how this arrangement came about.
 */
#define PODULE_CONFIG_FIRST_USER_SLOT 2
#define PODULE_CONFIG_NAME_LEN 16

/* Slot assignment - which podule short_name occupies each configurable slot.
   Empty string means the slot is unused. */
void podule_cfg_set_slot(int idx, const char *short_name);
const char *podule_cfg_get_slot(int idx);

/* Generic key/value store. 'section' is conventionally "<short_name>.<slot>".
   Values are held as strings; get_int parses with atoi(). */
int  podule_cfg_get_int(const char *section, const char *name, int def);
const char *podule_cfg_get_string(const char *section, const char *name, const char *def);
void podule_cfg_set_int(const char *section, const char *name, int val);
void podule_cfg_set_string(const char *section, const char *name, const char *val);

/* Iteration over stored entries, for persistence by the GUI layer. Returns
   non-zero and fills the pointers for a valid index, zero otherwise. */
int  podule_cfg_entry_count(void);
int  podule_cfg_get_entry(int idx, const char **section, const char **name, const char **value);

/* Clear all slot assignments and stored entries (called before a config load). */
void podule_cfg_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PODULE_CONFIG_H */
