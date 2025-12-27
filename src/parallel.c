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
 * Parallel Port Bus Implementation
 *
 * This module provides the virtual wiring between SuperIO parallel port
 * and attached peripheral devices.
 */

#include <string.h>

#include "rpcemu.h"
#include "parallel.h"
#include "iomd.h"

/* ========================================================================
 * Per-Port State
 * ======================================================================== */

typedef struct {
    /* Bus signals */
    uint8_t data;           /* Current data on bus */
    uint8_t control;        /* Control signals from host */
    uint8_t status;         /* Status signals from device */
    
    /* State */
    int strobe_active;      /* Strobe signal state */
    int irq_enabled;        /* IRQ on ACK enabled */
    int ack_pending;        /* Latch for polling drivers */
    
    /* Attached device */
    int has_device;
    ParallelDevice device;
} ParallelBusPort;

static ParallelBusPort ports[PARALLEL_PORT_COUNT];

/* ========================================================================
 * Initialization
 * ======================================================================== */

void
parallel_bus_init(void)
{
    memset(ports, 0, sizeof(ports));
    parallel_bus_reset();
    rpclog("Parallel Bus: Initialized\n");
}

void
parallel_bus_reset(void)
{
    int i;
    
    for (i = 0; i < PARALLEL_PORT_COUNT; i++) {
        ports[i].data = 0;
        ports[i].control = PARALLEL_CTRL_INIT;  /* nInit high = not resetting */
        ports[i].status = PARALLEL_STAT_DEFAULT;
        ports[i].strobe_active = 0;
        ports[i].irq_enabled = 0;
        
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
parallel_bus_attach(ParallelPortID port, const ParallelDevice *device)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return -1;
    }
    
    if (ports[port].has_device) {
        rpclog("Parallel Bus: LPT%d already has device attached\n", port + 1);
        return -1;
    }
    
    memcpy(&ports[port].device, device, sizeof(ParallelDevice));
    ports[port].has_device = 1;
    
    rpclog("Parallel Bus: Attached '%s' to LPT%d\n", 
           device->name ? device->name : "unknown", port + 1);
    
    return 0;
}

void
parallel_bus_detach(ParallelPortID port)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    if (ports[port].has_device) {
        rpclog("Parallel Bus: Detached '%s' from LPT%d\n",
               ports[port].device.name ? ports[port].device.name : "unknown",
               port + 1);
    }
    
    ports[port].has_device = 0;
    memset(&ports[port].device, 0, sizeof(ParallelDevice));
}

int
parallel_bus_has_device(ParallelPortID port)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return 0;
    }
    return ports[port].has_device;
}

/* ========================================================================
 * Host -> Device (called by SuperIO)
 * ======================================================================== */

void
parallel_bus_write_data(ParallelPortID port, uint8_t data)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    ports[port].data = data;
    /* Data is just latched here; actual transfer happens on strobe */
}

void
parallel_bus_strobe(ParallelPortID port, int strobe)
{
    ParallelBusPort *p;
    
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    p = &ports[port];
    
    /* Detect falling edge of strobe (data valid moment) */
    if (p->strobe_active && !strobe) {
        /* Strobe falling edge - transfer the data */
        if (p->has_device && p->device.on_write) {
            rpclog("Parallel Bus: LPT%d TX byte 0x%02X '%c'\n",
                   port + 1, p->data,
                   (p->data >= 32 && p->data < 127) ? p->data : '.');
            p->device.on_write(p->data, p->device.userdata);
        }
    }
    
    p->strobe_active = strobe;
}

void
parallel_bus_set_ctrl(ParallelPortID port, uint8_t ctrl)
{
    ParallelBusPort *p;
    int old_irq, new_irq;
    
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    p = &ports[port];
    old_irq = p->irq_enabled;
    new_irq = (ctrl & PARALLEL_CTRL_IRQEN) != 0;
    
    /* Handle strobe changes */
    if ((p->control ^ ctrl) & PARALLEL_CTRL_STROBE) {
        parallel_bus_strobe(port, ctrl & PARALLEL_CTRL_STROBE);
    }
    
    /* Handle init signal (active low reset) */
    if (!(ctrl & PARALLEL_CTRL_INIT)) {
        /* nInit low - reset the device */
        if (p->has_device && p->device.on_reset) {
            rpclog("Parallel Bus: LPT%d device reset\n", port + 1);
            p->device.on_reset(p->device.userdata);
        }
    }
    
    /* Notify device of control changes */
    if (p->has_device && p->device.on_ctrl) {
        p->device.on_ctrl(ctrl, p->device.userdata);
    }
    
    /* Update state */
    p->control = ctrl;
    p->irq_enabled = new_irq;
    
    /* If IRQ mode just enabled, do NOT generate initial ACK. 
       ACK should only occur on actual device acknowledgement. */
    if (!old_irq && new_irq) {
        rpclog("Parallel Bus: LPT%d IRQ enabled\n", port + 1);
    }
}

uint8_t
parallel_bus_get_status(ParallelPortID port)
{
    ParallelBusPort *p;
    uint8_t status_val;
    
    if (port >= PARALLEL_PORT_COUNT) {
        return PARALLEL_STAT_DEFAULT;
    }
    
    p = &ports[port];
    
    /* Get live status from device if attached */
    if (p->has_device && p->device.get_status) {
        p->status = p->device.get_status(p->device.userdata);
    }
    
    status_val = p->status;
    
    /* If an ACK pulse is pending/latched, force the ACK bit low (Active).
       Hold it for multiple reads to ensure polling driver sees it. */
    if (p->ack_pending > 0) {
        status_val &= ~PARALLEL_STAT_ACK;
        p->ack_pending--; /* Decrement counter */
    } else {
        /* Otherwise ensure it's High (Inactive) unless device pulls it low */
        /* (Virtual device usually keeps it high unless pulsing) */
        /* Actually, trust p->status from device, but verify ACK logic */
    }
    
    return status_val;
}

uint8_t
parallel_bus_read_data(ParallelPortID port)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return 0xFF;
    }
    
    /* In bidirectional mode, device could drive the bus */
    /* For now, just return the latched data */
    return ports[port].data;
}

/* ========================================================================
 * Device -> Host (called by attached devices)
 * ======================================================================== */

void
parallel_bus_device_ack(ParallelPortID port)
{
    ParallelBusPort *p;
    
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    p = &ports[port];
    
    /* Generate ACK interrupt if enabled */
    /* Generate ACK interrupt if enabled */
    if (p->irq_enabled) {
        /* Pulse nACK low (we set it low in status temporarily) */
        p->status &= ~PARALLEL_STAT_ACK;
        
        /* Raise interrupt */
        iomd.irqa.status |= IOMD_IRQA_PARALLEL;
        updateirqs();
        
        /* ACK returns high immediately (in real hardware this is ~5µs) */
        p->status |= PARALLEL_STAT_ACK;
        
        rpclog("Parallel Bus: LPT%d ACK interrupt generated (IRQA status=0x%02X mask=0x%02X)\n", 
               port + 1, iomd.irqa.status, iomd.irqa.mask);
    } else {
        /* IRQs disabled, so checking driver must be polling. 
           Latch the ACK (Active Low) for a few reads. */
        p->ack_pending = 5;
        rpclog("Parallel Bus: LPT%d ACK latched for polling\n", port + 1);
    }
}

void
parallel_bus_device_status(ParallelPortID port, uint8_t status)
{
    if (port >= PARALLEL_PORT_COUNT) {
        return;
    }
    
    ports[port].status = status;
}
