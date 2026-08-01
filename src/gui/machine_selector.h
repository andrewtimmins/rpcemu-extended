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
 * machine_selector - choose a machine over VNC, when there is no GUI to do it.
 *
 * Headless mode currently insists on --machine, because the selector is a wx
 * dialogue and there is no wx. That makes a remote emulator awkward in a way a
 * local one is not: changing machine means stopping the process and starting it
 * with different arguments. A VNC client has a screen and a keyboard, which is
 * all a list needs.
 *
 * This is the model and the drawing, with no VNC and no wx in it, so it can be
 * tested by rendering into a buffer and looking at the result. The caller owns
 * the framebuffer, feeds it keys, and acts on the outcome.
 *
 * Keys are X11 keysyms, because that is what RFB delivers and what the VNC
 * server's keyboard callback already receives.
 */

#ifndef MACHINE_SELECTOR_H
#define MACHINE_SELECTOR_H

#include <stdint.h>

#include "text_screen.h"

#define MACHINE_SELECTOR_MAX 64	/**< machines listed; more are counted, not shown */
#define MACHINE_SELECTOR_NAME 64

/** What a keypress did, so the caller knows whether to redraw or act. */
typedef enum {
	MACHINE_SELECTOR_IGNORED,	/**< nothing changed; no redraw needed */
	MACHINE_SELECTOR_REDRAW,	/**< selection moved */
	MACHINE_SELECTOR_CHOSEN,	/**< the user picked the current machine */
	MACHINE_SELECTOR_CANCELLED	/**< the user asked to give up */
} MachineSelectorAction;

typedef struct {
	char names[MACHINE_SELECTOR_MAX][MACHINE_SELECTOR_NAME];
	int count;		/**< entries in names[] */
	int total;		/**< machines found, which may exceed count */
	int selected;		/**< index into names[] */
	int first_visible;	/**< top of the scrolled window */
} MachineSelector;

#ifdef __cplusplus
extern "C" {
#endif

/** Start with an empty list. */
extern void machine_selector_init(MachineSelector *m);

/**
 * Add a machine.
 *
 * Names beyond MACHINE_SELECTOR_MAX are counted in `total` but not stored, so a
 * directory with hundreds of configs still draws something sensible and says how
 * many it left out.
 *
 * @return 1 if stored, 0 if only counted.
 */
extern int machine_selector_add(MachineSelector *m, const char *name);

/** The currently highlighted name, or NULL if the list is empty. */
extern const char *machine_selector_current(const MachineSelector *m);

/** Handle an X11 keysym. */
extern MachineSelectorAction machine_selector_key(MachineSelector *m,
                                                 uint32_t keysym);

/** Draw the whole screen. */
extern void machine_selector_draw(MachineSelector *m, TextScreen *s);

#ifdef __cplusplus
}
#endif

#endif /* MACHINE_SELECTOR_H */
