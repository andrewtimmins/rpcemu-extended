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
 * usbcard.h - the USB expansion card.
 *
 * A card of its own, carrying the OHCI host controller (usb_ohci.c) and a ROM
 * for the module that tells RISC OS the controller is there.
 *
 * A card of its own rather than part of the support card for two reasons. It
 * needs a ROM of its own to carry that module; and an expansion card with one
 * interrupt and one thing raising it is far easier to reason about. When the
 * controller shared the support card with the scroll wheel, two drivers ended
 * up claiming the same podule interrupt and each clearing the other's, which
 * wedged machines solid.
 *
 * Which slot it lands in does not matter. OHCIDriver finds the controller by
 * asking the HAL what USB hardware exists, not by looking in a slot.
 */

#ifndef USBCARD_H
#define USBCARD_H

/** Build the card's ROM image. Called once at startup. */
void usbcard_init(void);

/** Put the card in the backplane and start the controller. */
void usbcard_reset(void);

#endif /* USBCARD_H */
