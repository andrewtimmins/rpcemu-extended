/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

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
 * Serial Port Bus Abstraction
 *
 * This module provides a virtual "wire" between the SuperIO serial port
 * hardware (UARTs) and connected peripheral devices (switches, modems, etc.)
 *
 * RS-232 Signal Lines:
 *   Host -> Device (Control):
 *     - DTR (Data Terminal Ready)
 *     - RTS (Request To Send)
 *     - OUT1/OUT2 (General purpose)
 *
 *   Device -> Host (Status):
 *     - CTS (Clear To Send)
 *     - DSR (Data Set Ready)
 *     - RI  (Ring Indicator)
 *     - DCD (Data Carrier Detect)
 *
 *   Bidirectional:
 *     - TX/RX Data
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Serial Bus Signals (Bitmasks matching 16550 registers where possible)
 * ======================================================================== */

/* Modem Control Register (MCR) signals - Host -> Device */
#define SERIAL_CTRL_DTR     0x01  /* Data Terminal Ready */
#define SERIAL_CTRL_RTS     0x02  /* Request To Send */
#define SERIAL_CTRL_OUT1    0x04  /* Output 1 */
#define SERIAL_CTRL_OUT2    0x08  /* Output 2 (often enables interrupts) */
#define SERIAL_CTRL_LOOP    0x10  /* Loopback mode */

/* Modem Status Register (MSR) signals - Device -> Host */
/* Note: Using standard bit positions for MSR */
#define SERIAL_STAT_CTS     0x10  /* Clear To Send */
#define SERIAL_STAT_DSR     0x20  /* Data Set Ready */
#define SERIAL_STAT_RI      0x40  /* Ring Indicator */
#define SERIAL_STAT_DCD     0x80  /* Data Carrier Detect */

/* ========================================================================
 * Serial Device Interface
 * ======================================================================== */

/**
 * Callback type for when host transmits data (TX)
 * @param data      The byte transmitted
 * @param userdata  Device-specific context
 */
typedef void (*serial_write_cb)(uint8_t data, void *userdata);

/**
 * Callback type for when host changes control signals (DTR, RTS, etc.)
 * @param ctrl      Control signals state (SERIAL_CTRL_*)
 * @param userdata  Device-specific context
 */
typedef void (*serial_ctrl_cb)(uint8_t ctrl, void *userdata);

/**
 * Callback type for device to provide current status (CTS, DSR, etc.)
 * @param userdata  Device-specific context
 * @return Status signals state (SERIAL_STAT_*)
 */
typedef uint8_t (*serial_status_cb)(void *userdata);

/**
 * Callback type for device reset
 * @param userdata  Device-specific context
 */
typedef void (*serial_reset_cb)(void *userdata);

/**
 * Serial device descriptor
 */
typedef struct {
    const char *name;              /* Device name for logging */
    serial_write_cb on_write;      /* Called when Host TX */
    serial_ctrl_cb on_ctrl;        /* Called when Host MCR changes */
    serial_status_cb get_status;   /* Get Device MSR status */
    serial_reset_cb on_reset;      /* Called on bus reset */
    void *userdata;                /* Device-specific context */
} SerialDevice;

/* ========================================================================
 * Serial Port Identifiers
 * ======================================================================== */

typedef enum {
    SERIAL_PORT_COM1 = 0,  /* Primary UART (0x3F8) */
    SERIAL_PORT_COM2 = 1,  /* Secondary UART (0x2F8) */
    SERIAL_PORT_COUNT
} SerialPortID;

/* ========================================================================
 * Serial Bus API
 * ======================================================================== */

/**
 * Initialize the serial bus subsystem.
 */
void serial_bus_init(void);

/**
 * Reset the serial bus subsystem.
 */
void serial_bus_reset(void);

/**
 * Attach a device to a serial port.
 * @param port    Which port to attach to
 * @param device  Device descriptor (copied internally)
 * @return 0 on success, -1 if port already has a device
 */
int serial_bus_attach(SerialPortID port, const SerialDevice *device);

/**
 * Detach any device from a serial port.
 * @param port  Which port to detach from
 */
void serial_bus_detach(SerialPortID port);

/**
 * Check if a device is attached to a port.
 * @param port  Which port to check
 * @return Non-zero if a device is attached
 */
int serial_bus_has_device(SerialPortID port);

/* ---- Called by SuperIO serial port emulation ---- */

/**
 * Write byte to serial bus (Host TX).
 * Called by SuperIO when host writes to THR.
 * @param port  Which port
 * @param data  Byte to write
 */
void serial_bus_write_data(SerialPortID port, uint8_t data);

/**
 * Update control signals on the bus.
 * Called by SuperIO when MCR is written.
 * @param port  Which port
 * @param ctrl  Control signal lines
 */
void serial_bus_set_ctrl(SerialPortID port, uint8_t ctrl);

/**
 * Read status signals from the bus.
 * Called by SuperIO to update MSR.
 * @param port  Which port
 * @return Status signal lines
 */
uint8_t serial_bus_get_status(SerialPortID port);

/* ---- Called by attached devices ---- */

/**
 * Device sends a byte to Host (Device TX / Host RX).
 * This typically triggers a Data Ready interrupt on the UART.
 * @param port  Which port
 * @param data  Byte sent
 */
void serial_bus_device_write_data(SerialPortID port, uint8_t data);

/**
 * Device updates its status lines (CTS, DSR, RI, DCD).
 * Changes may trigger Modem Status interrupts.
 * @param port    Which port
 * @param status  New status lines
 */
void serial_bus_device_status(SerialPortID port, uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_H */
