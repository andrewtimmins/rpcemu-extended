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

#ifndef SUPERIO_H
#define SUPERIO_H

#include "peripheral_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SuperIO chip type.
 */
typedef enum {
	SuperIOType_FDC37C665GT,
	SuperIOType_FDC37C672
} SuperIOType;

extern void superio_reset(SuperIOType chosen_super_type);
extern uint8_t superio_read(uint32_t addr);
extern void superio_write(uint32_t addr, uint32_t val);
extern void superio_write_byte(uint32_t addr, uint8_t byte);

extern void superio_smi_setint1(uint8_t i);
extern void superio_smi_setint2(uint8_t i);
extern void superio_smi_clrint1(uint8_t i);
extern void superio_smi_clrint2(uint8_t i);
extern void superio_get_snapshot(SuperIOStateSnapshot *snapshot);

/* Serial port interface (called by serial bus) */
#include "serial.h"  /* For SerialPortID */
extern void superio_serial_rx(SerialPortID port, uint8_t data);
extern void superio_serial_update_msr(SerialPortID port, uint8_t status);

/* Free space (in bytes) in a UART's receive FIFO. Lets a backend feed incoming
 * data at the rate the guest drains it, instead of overrunning the 16-byte FIFO. */
extern int superio_serial_rx_space(SerialPortID port);
extern int superio_serial_get_line(SerialPortID port, uint16_t *divisor, uint8_t *lcr);

#ifdef __cplusplus
}
#endif

#endif /* SUPERIO_H */
