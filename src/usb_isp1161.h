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
 * usb_isp1161.h - the USB host controller on the support card.
 *
 * A Philips ISP1161, reached through four ports in the card's EASI space. See
 * docs/usb.md for the card and why it is this chip: the short version is
 * that the podule bus cannot bus-master, and this controller does not need to,
 * because its descriptors and data live in its own buffer RAM and the host moves
 * them through a data port.
 *
 * The emulation is written to satisfy the driver RISC OS Open publish, which is
 * Apache licensed and so cannot be part of RPCEmu; this is an independent
 * implementation of the same programmer's model.
 */

#ifndef USB_ISP1161_H
#define USB_ISP1161_H

#include <stdint.h>

/** Offset of the controller's ports within the card's EASI space. */
#define USB_ISP1161_EASI_OFFSET	0x800000

/** Size of the port window, four words. */
#define USB_ISP1161_EASI_SIZE	0x10

/**
 * Start the controller up.
 *
 * @param irq_change Called when the card's interrupt line should change, with 1
 *                   to assert and 0 to release. The controller does not know how
 *                   the card is wired, so the card supplies this.
 */
void usb_isp1161_init(void (*irq_change)(int level));

/** Return the controller to its power-on state, as a bus reset would. */
void usb_isp1161_reset(void);

/**
 * Unplug everything, on the way out.
 *
 * This matters for a device passed through from the host: it is only the
 * emulator's for as long as it holds it, and giving it back is what puts the
 * host's own driver on it again. Leaving without doing so leaves the device
 * attached to the host but bound to nothing, which needs unplugging to cure.
 */
void usb_isp1161_shutdown(void);

/**
 * Read one of the four ports.
 *
 * @param offset Byte offset within the port window, 0 to 12.
 * @return The value, in the bottom 16 bits.
 */
uint32_t usb_isp1161_read(uint32_t offset);

/** Write one of the four ports. Only the bottom 16 bits of @val are used. */
void usb_isp1161_write(uint32_t offset, uint32_t val);

/**
 * Advance the frame counter and run whatever a frame boundary triggers.
 *
 * Called once per millisecond, a USB frame being 1ms.
 */
void usb_isp1161_frame(void);

/** True if the controller is present and switched on for this machine. */
int usb_isp1161_enabled(void);

/* --- What is plugged in -------------------------------------------------- */

struct UsbDevice;

/**
 * Plug a device into a root hub port.
 *
 * The guest sees a connect status change, exactly as it would if someone had
 * pushed a plug in, so a driver enumerates it in its own time rather than being
 * told about it out of band.
 *
 * @param port 0 or 1.
 * @return 0 if it went in, -1 if the port is occupied or there is no controller.
 */
int usb_isp1161_attach(int port, struct UsbDevice *dev);

/**
 * Unplug whatever is in a port and hand it back, or NULL if it was empty.
 *
 * The caller owns the device again and is responsible for freeing it.
 */
struct UsbDevice *usb_isp1161_detach(int port);

/** What is in a port, without disturbing it. */
struct UsbDevice *usb_isp1161_device_in_port(int port);

/**
 * Make the ports match this machine's configuration, plugging in and unplugging
 * as needed. Safe to call at any time; ports already as asked for are untouched,
 * so it does not disturb a device the guest is using.
 */
void usb_isp1161_apply_config(void);

#endif /* USB_ISP1161_H */
