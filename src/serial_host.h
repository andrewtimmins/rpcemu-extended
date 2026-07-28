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
 * serial_host.h - a serial port on the host, wired to an emulated UART.
 *
 * The guest's serial port becomes a real one: /dev/ttyUSB0, COM3, a pseudo
 * terminal, whatever the host offers. Bytes are carried in both directions and
 * the modem control lines are mirrored, so a terminal, a modem or another
 * machine on the other end behaves as it would on a real Risc PC.
 *
 * What is translated is configuration, not timing. The host's own UART does the
 * signalling; this reads the divisor and line control register the guest
 * programmed and asks the host port for the same speed and framing.
 */

#ifndef SERIAL_HOST_H
#define SERIAL_HOST_H

#include <stddef.h>

#include "serial.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open a host serial device and attach it to an emulated port.
 *
 * @param port    Which emulated port (COM1 or COM2)
 * @param device  Host device: a path, or a name like COM3 on Windows
 * @param error   Filled with a reason the user can act on when this fails
 * @param errlen  Size of @error
 * @return 0 on success, -1 on failure
 */
int serial_host_attach(SerialPortID port, const char *device,
                       char *error, size_t errlen);

/** Close the host device on a port, if any, and detach it from the bus. */
void serial_host_close(SerialPortID port);

/** True if a port currently has a host device open. */
int serial_host_is_open(SerialPortID port);

/**
 * Carry bytes and line states in both directions.
 *
 * Runs on the emulation thread beside the UARTs, like serial_modem_poll(), and
 * never blocks. Reads no more than the guest's receive FIFO can take, so a
 * flood from the host applies backpressure rather than being dropped.
 */
void serial_host_poll(void);

/** Close every host device. Called when the emulator shuts down. */
void serial_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_HOST_H */
