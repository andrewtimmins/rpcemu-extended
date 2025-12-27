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
 * Serial Port Bus Implementation
 *
 * This module provides the virtual wiring between SuperIO serial ports
 * and attached peripheral devices.
 */

#include <string.h>

#include "rpcemu.h"
#include "serial.h"
#include "superio.h"



/* ========================================================================
 * Per-Port State
 * ======================================================================== */

typedef struct {
    /* Bus signals */
    uint8_t control;        /* Control signals from host (MCR) */
    uint8_t status;         /* Status signals from device (MSR) */
    
    /* Attached device */
    int has_device;
    SerialDevice device;
} SerialBusPort;

static SerialBusPort ports[SERIAL_PORT_COUNT];

/* ========================================================================
 * Initialization
 * ======================================================================== */

void
serial_bus_init(void)
{
    memset(ports, 0, sizeof(ports));
    serial_bus_reset();
    rpclog("Serial Bus: Initialized\n");
}

void
serial_bus_reset(void)
{
    int i;
    
    for (i = 0; i < SERIAL_PORT_COUNT; i++) {
        ports[i].control = 0;
        ports[i].status = 0; 
        
        /* Reset attached device if any */
        if (ports[i].has_device && ports[i].device.on_reset) {
            ports[i].device.on_reset(ports[i].device.userdata);
        }
    }
}

/* ========================================================================
 * Device Attachment
 * ======================================================================== */

int
serial_bus_attach(SerialPortID port, const SerialDevice *device)
{
    if (port >= SERIAL_PORT_COUNT) {
        return -1;
    }
    
    if (ports[port].has_device) {
        rpclog("Serial Bus: COM%d already has device attached\n", port + 1);
        return -1;
    }
    
    memcpy(&ports[port].device, device, sizeof(SerialDevice));
    ports[port].has_device = 1;
    
    rpclog("Serial Bus: Attached '%s' to COM%d\n", 
           device->name ? device->name : "unknown", port + 1);
    
    return 0;
}

void
serial_bus_detach(SerialPortID port)
{
    if (port >= SERIAL_PORT_COUNT) {
        return;
    }
    
    if (ports[port].has_device) {
        rpclog("Serial Bus: Detached '%s' from COM%d\n",
               ports[port].device.name ? ports[port].device.name : "unknown",
               port + 1);
    }
    
    ports[port].has_device = 0;
    memset(&ports[port].device, 0, sizeof(SerialDevice));
}

int
serial_bus_has_device(SerialPortID port)
{
    if (port >= SERIAL_PORT_COUNT) {
        return 0;
    }
    return ports[port].has_device;
}

/* ========================================================================
 * Host -> Device (called by SuperIO)
 * ======================================================================== */

void
serial_bus_write_data(SerialPortID port, uint8_t data)
{
    if (port >= SERIAL_PORT_COUNT) {
        return;
    }

    if (ports[port].has_device && ports[port].device.on_write) {
        // rpclog("Serial Bus: COM%d TX byte 0x%02X\n", port + 1, data);
        ports[port].device.on_write(data, ports[port].device.userdata);
    }
}

void
serial_bus_set_ctrl(SerialPortID port, uint8_t ctrl)
{
    SerialBusPort *p;
    
    if (port >= SERIAL_PORT_COUNT) {
        return;
    }
    
    p = &ports[port];
    
    /* Detect changes if we want to log or do edge detection */
    // uint8_t diff = p->control ^ ctrl;
    
    p->control = ctrl;
    
    /* Notify device of control changes */
    if (p->has_device && p->device.on_ctrl) {
        p->device.on_ctrl(ctrl, p->device.userdata);
    }
}

uint8_t
serial_bus_get_status(SerialPortID port)
{
    SerialBusPort *p;
    
    if (port >= SERIAL_PORT_COUNT) {
        return 0;
    }
    
    p = &ports[port];
    
    /* Get live status from device if attached */
    if (p->has_device && p->device.get_status) {
        p->status = p->device.get_status(p->device.userdata);
    }
    
    return p->status;
}

/* ========================================================================
 * Device -> Host (called by attached devices)
 * ======================================================================== */

void
serial_bus_device_write_data(SerialPortID port, uint8_t data)
{
    if (port >= SERIAL_PORT_COUNT) {
        return;
    }

    /* Push received byte into UART's RX buffer */
    superio_serial_rx(port, data);
}

void
serial_bus_device_status(SerialPortID port, uint8_t status)
{
    if (port >= SERIAL_PORT_COUNT) {
        return;
    }
    
    ports[port].status = status;
    
    /* Update UART's modem status register */
    superio_serial_update_msr(port, status);
}

