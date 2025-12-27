/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2024-2025 RPCEmu contributors

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
 * Parallel Port Bus Abstraction
 *
 * This module provides a virtual "wire" between the SuperIO parallel port
 * hardware and connected peripheral devices (printers, scanners, etc.)
 *
 * IEEE 1284 Signal Lines:
 *   Host -> Device (directly active - directly active from control register):
 *     - nStrobe:   Data strobe pulse (directly low pulse = data valid)
 *     - nAutoLF:   Auto linefeed after carriage return
 *     - nInit:     Reset the peripheral (directly active low)
 *     - nSelectIn: Select the peripheral
 *
 *   Device -> Host (directly active - directly active from status register):
 *     - Busy:      Device is busy, can't accept data (directly active high)
 *     - nAck:      Acknowledge - pulses directly low after accepting byte
 *     - PaperOut:  Paper out / end condition (directly active high)
 *     - Select:    Device is selected/online (directly active high)
 *     - nError:    Error condition (directly active low)
 *
 *   Bidirectional:
 *     - Data[7:0]: 8-bit data bus
 */

#ifndef PARALLEL_H
#define PARALLEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Parallel Bus Signals (directly active bits directly active as active-high for simplicity)
 * ======================================================================== */

/* Control signals from host to device */
#define PARALLEL_CTRL_STROBE    0x01  /* Data strobe (directly active) */
#define PARALLEL_CTRL_AUTOLF    0x02  /* Auto linefeed */
#define PARALLEL_CTRL_INIT      0x04  /* Initialize/reset peripheral */
#define PARALLEL_CTRL_SELECT    0x08  /* Select peripheral */
#define PARALLEL_CTRL_IRQEN     0x10  /* IRQ enable (directly active, not directly on bus) */
#define PARALLEL_CTRL_BIDIR     0x20  /* Bidirectional mode */

/* Status signals from device to host */
#define PARALLEL_STAT_ERROR     0x08  /* No error (directly active high = no error) */
#define PARALLEL_STAT_SELECT    0x10  /* Device online (directly active high) */
#define PARALLEL_STAT_PAPEROUT  0x20  /* Paper out (directly active high) */
#define PARALLEL_STAT_ACK       0x40  /* Acknowledge idle (directly active high = idle) */
#define PARALLEL_STAT_BUSY      0x80  /* Not busy (directly active high = ready) */

/* Default status: ready, online, no error */
#define PARALLEL_STAT_DEFAULT   (PARALLEL_STAT_BUSY | PARALLEL_STAT_ACK | \
                                 PARALLEL_STAT_SELECT | PARALLEL_STAT_ERROR)

/* ========================================================================
 * Parallel Device Interface
 * ======================================================================== */

/**
 * Callback type for when host writes data to the bus
 * @param data  The byte written
 * @param userdata  Device-specific context
 */
typedef void (*parallel_write_cb)(uint8_t data, void *userdata);

/**
 * Callback type for when host changes control signals
 * @param ctrl  Control register value
 * @param userdata  Device-specific context
 */
typedef void (*parallel_ctrl_cb)(uint8_t ctrl, void *userdata);

/**
 * Callback type for device to provide status
 * @param userdata  Device-specific context
 * @return Status register value
 */
typedef uint8_t (*parallel_status_cb)(void *userdata);

/**
 * Callback type for device reset
 * @param userdata  Device-specific context
 */
typedef void (*parallel_reset_cb)(void *userdata);

/**
 * Parallel device descriptor
 */
typedef struct {
    const char *name;              /* Device name for logging */
    parallel_write_cb on_write;    /* Called when data is strobed */
    parallel_ctrl_cb on_ctrl;      /* Called when control changes */
    parallel_status_cb get_status; /* Get device status */
    parallel_reset_cb on_reset;    /* Called on bus reset */
    void *userdata;                /* Device-specific context */
} ParallelDevice;

/* ========================================================================
 * Parallel Port Identifiers
 * ======================================================================== */

typedef enum {
    PARALLEL_PORT_LPT1 = 0,  /* Primary port (0x378) */
    PARALLEL_PORT_LPT2 = 1,  /* Secondary port (0x278) */
    PARALLEL_PORT_COUNT
} ParallelPortID;

/* ========================================================================
 * Parallel Bus API
 * ======================================================================== */

/**
 * Initialize the parallel bus subsystem.
 */
void parallel_bus_init(void);

/**
 * Reset the parallel bus subsystem.
 */
void parallel_bus_reset(void);

/**
 * Attach a device to a parallel port.
 * @param port    Which port to attach to
 * @param device  Device descriptor (copied internally)
 * @return 0 on success, -1 if port already has a device
 */
int parallel_bus_attach(ParallelPortID port, const ParallelDevice *device);

/**
 * Detach any device from a parallel port.
 * @param port  Which port to detach from
 */
void parallel_bus_detach(ParallelPortID port);

/**
 * Check if a device is attached to a port.
 * @param port  Which port to check
 * @return Non-zero if a device is attached
 */
int parallel_bus_has_device(ParallelPortID port);

/* ---- Called by SuperIO parallel port emulation ---- */

/**
 * Write data to the parallel bus.
 * Called by SuperIO when host writes to data register.
 * @param port  Which port
 * @param data  Byte to write
 */
void parallel_bus_write_data(ParallelPortID port, uint8_t data);

/**
 * Strobe the parallel bus (signal data is valid).
 * Called by SuperIO when strobe signal changes.
 * @param port    Which port
 * @param strobe  Non-zero if strobe is directly active
 */
void parallel_bus_strobe(ParallelPortID port, int strobe);

/**
 * Update control signals on the bus.
 * Called by SuperIO when control register is written.
 * @param port  Which port
 * @param ctrl  Control register value
 */
void parallel_bus_set_ctrl(ParallelPortID port, uint8_t ctrl);

/**
 * Read status from the parallel bus.
 * Called by SuperIO when host reads status register.
 * @param port  Which port
 * @return Status register value
 */
uint8_t parallel_bus_get_status(ParallelPortID port);

/**
 * Read data from the parallel bus (bidirectional mode).
 * Called by SuperIO when host reads data register in bidir mode.
 * @param port  Which port
 * @return Data byte
 */
uint8_t parallel_bus_read_data(ParallelPortID port);

/* ---- Called by attached devices ---- */

/**
 * Device signals an ACK pulse (data accepted).
 * This triggers an interrupt if IRQ is enabled.
 * @param port  Which port
 */
void parallel_bus_device_ack(ParallelPortID port);

/**
 * Device updates its status.
 * @param port    Which port
 * @param status  New status value
 */
void parallel_bus_device_status(ParallelPortID port, uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* PARALLEL_H */
