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
 * usb_ohci.h - an OHCI USB host controller.
 *
 * This replaced a Philips ISP1161. The ISP1161 was the historically honest
 * choice - it is what the Simtec and STD podules carried, and it was chosen
 * because a real Risc PC's expansion bus cannot bus-master, which OHCI needs.
 * But the only RISC OS driver for it is a 2001 Pace orphan that assumes it is
 * the only expansion card in the machine, and wedges any machine that is not
 * (see docs/usb.md).
 *
 * OHCI is not physically possible on a real Risc PC podule. It is entirely
 * possible here, because we are the hardware and can do the bus mastering
 * ourselves; the guest cannot tell. What it buys is the driver stack RISC OS 5
 * actually uses and RISC OS Open actually maintain - OHCIDriver underneath
 * USBDriver - which brings working HID keyboard and mouse with it.
 *
 * The controller is found by the guest through a HAL device rather than by
 * where the card is plugged in: OHCIDriver walks HAL devices asking each one
 * HAL_USBControllerInfo until one says it is an OHCI. That is also what keeps
 * this out of the podule interrupt chain, which is what the ISP1161 foundered
 * on.
 */

#ifndef USB_OHCI_H
#define USB_OHCI_H

#include <stdint.h>

/** Offset of the register window within the card's EASI space. */
#define USB_OHCI_EASI_OFFSET	0x800000

/**
 * Size of the register window.
 *
 * OHCI defines registers up to HcRhPortStatus[NDP]. Four ports takes the last
 * one to &5c, and the window is rounded up.
 */
#define USB_OHCI_EASI_SIZE	0x100

/**
 * Downstream ports on the root hub.
 *
 * OHCI allows up to 15. Four is what the interface offers, which is more than
 * the two the ISP1161 had and enough that nobody has to unplug something to
 * try something else.
 */
#define USB_OHCI_PORTS		4

/**
 * Start the controller up.
 *
 * @param irq_change Called when the card's interrupt line should change, with 1
 *                   to assert and 0 to release.
 */
void usb_ohci_init(void (*irq_change)(int level));

/** Return the controller to its power-on state. */
void usb_ohci_reset(void);

/** Give every attached device back to the host, on the way out. */
void usb_ohci_shutdown(void);

/** Read a register. @param offset Byte offset within the window. */
uint32_t usb_ohci_read(uint32_t offset);

/** Write a register. */
void usb_ohci_write(uint32_t offset, uint32_t val);

/**
 * Advance the frame counter and run the lists.
 *
 * Called once per millisecond. Unlike the ISP1161, where the driver handed
 * descriptors over through a data port and the list ran when it arrived, OHCI
 * expects the controller to walk lists in main memory on its own schedule. So
 * this is where the work happens.
 */
void usb_ohci_frame(void);

/** True if the controller is present for this machine. */
int usb_ohci_enabled(void);

/* --- What is plugged in -------------------------------------------------- */

struct UsbDevice;

/**
 * Plug a device into a root hub port.
 *
 * The guest sees a connect status change, exactly as it would if someone had
 * pushed a plug in.
 *
 * @param port 0 to USB_OHCI_PORTS - 1.
 * @return 0 if it went in, -1 if the port is occupied or there is no controller.
 */
int usb_ohci_attach(int port, struct UsbDevice *dev);

/** Unplug whatever is in a port and hand it back, or NULL if it was empty. */
struct UsbDevice *usb_ohci_detach(int port);

/** What is in a port, without disturbing it. */
struct UsbDevice *usb_ohci_device_in_port(int port);

/**
 * Make the ports match this machine's configuration.
 *
 * Safe to call at any time; ports already as asked for are untouched, so it
 * does not disturb a device the guest is using.
 */
void usb_ohci_apply_config(void);

#endif /* USB_OHCI_H */
