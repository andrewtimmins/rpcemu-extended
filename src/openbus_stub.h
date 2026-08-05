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

#ifndef OPENBUS_STUB_H
#define OPENBUS_STUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A card that exists to prove the bus works, and for nothing else.
 *
 * It is not a model of any real hardware and does not pretend to be: no such
 * card was ever made, the register layout below is ours, and the ID register
 * says so. It exists because every part of an OPEN Bus card that ISN'T the
 * processor can be exercised without a processor - the register window, bus
 * mastering, the interrupt line, reset and the timeslice - and proving those
 * against something trivial is a great deal easier than debugging them for the
 * first time underneath a 386.
 *
 * The one interesting thing it does is a block copy performed AS A BUS MASTER:
 * write a source, a destination and a length, and on its next slice it moves
 * words through the host's physical memory exactly as an x86 card would. If that
 * works, the hard half of any real card's plumbing works.
 *
 * Register map, at OPENBUS_REG_BASE. All 32-bit; narrower accesses read and
 * write the low bytes of the addressed word, which is enough for a test device.
 *
 *   0x00 ID       R   0x4F425354 ('OBST'). Write: ignored.
 *   0x04 CTRL     RW  bit 0 raise nPIRQ
 *                     bit 1 raise FIQ
 *                     bit 2 start the copy described by SRC/DST/LEN
 *                     bit 3 write 1 to reset the card (self-clearing)
 *   0x08 STATUS   R   bit 0 copy in progress
 *                     bit 1 nPIRQ asserted
 *                     bit 2 FIQ asserted
 *                     bit 3 last copy completed
 *   0x0c SRC      RW  source physical address
 *   0x10 DST      RW  destination physical address
 *   0x14 LEN      RW  length in 32-bit words, counted down as the copy runs
 *   0x18 SCRATCH  RW  any value, proving the window round-trips
 *   0x1c RUNS     R   how many times the card has been given a timeslice
 *   0x20 COPIED   R   words copied by the last or current operation
 *
 * Anything else reads 0xffffffff.
 */

#define OPENBUS_STUB_ID			0x4f425354u	/* 'OBST' */

#define OPENBUS_STUB_REG_ID		0x00
#define OPENBUS_STUB_REG_CTRL		0x04
#define OPENBUS_STUB_REG_STATUS		0x08
#define OPENBUS_STUB_REG_SRC		0x0c
#define OPENBUS_STUB_REG_DST		0x10
#define OPENBUS_STUB_REG_LEN		0x14
#define OPENBUS_STUB_REG_SCRATCH	0x18
#define OPENBUS_STUB_REG_RUNS		0x1c
#define OPENBUS_STUB_REG_COPIED		0x20

#define OPENBUS_STUB_CTRL_IRQ		0x01
#define OPENBUS_STUB_CTRL_FIQ		0x02
#define OPENBUS_STUB_CTRL_COPY		0x04
#define OPENBUS_STUB_CTRL_RESET		0x08

#define OPENBUS_STUB_STATUS_BUSY	0x01
#define OPENBUS_STUB_STATUS_IRQ		0x02
#define OPENBUS_STUB_STATUS_FIQ		0x04
#define OPENBUS_STUB_STATUS_DONE	0x08

/** Words moved per cycle granted, so a copy takes believable time. */
#define OPENBUS_STUB_WORDS_PER_CYCLE	1

/**
 * Fit the stub to the second slot.
 *
 * @return zero on success, non-zero if the bus is not started or a card is
 *         already fitted.
 */
int openbus_stub_fit(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENBUS_STUB_H */
