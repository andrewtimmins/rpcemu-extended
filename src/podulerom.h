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

#ifndef PODULEROM_H
#define PODULEROM_H

#include <stdint.h>
#include "podules.h"

extern void podulerom_mouse_wheel_change(int dy);

/* The card's description chunk, which is what RISC OS prints for *Podules.
   Shared so that the machine editor can name the slot the same way. */
extern const char podulerom_description[];
extern void initpodulerom(void);
extern void podulerom_reset(void);

#endif
