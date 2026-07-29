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
 * usb_host.h - a real USB device on the host, presented to the guest.
 *
 * The emulated controller does not know where the answers come from: a device is
 * three questions to it, and these ones are put to real hardware on the other
 * side of libusb rather than answered by code.
 *
 * Everything is optional. If libusb was not found when RPCEmu was built, or the
 * host will not let a device be opened, these calls all fail politely and the
 * rest of the emulator carries on without USB passthrough.
 */

#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>

/**
 * How long an identifier is in a machine's configuration.
 *
 * A device is remembered as "vvvv:pppp", its vendor and product identifiers,
 * rather than by where it is plugged in. Bus and port numbers change every time
 * something is unplugged; the identifiers do not, so a machine started again
 * tomorrow finds the same device.
 */
#define USB_HOST_ID_LEN		16

/** One device on the host's own USB bus, as the interface offers it. */
typedef struct {
	uint16_t vendor;
	uint16_t product;
	uint8_t bus;
	uint8_t address;
	uint8_t device_class;	/**< From the device, or its first interface */

	/** The host has a driver of its own bound to it: -1 if that is unknown. */
	int in_use;

	/** It can be opened at all. Zero usually means permissions. */
	int openable;

	/** True for a device faster than the emulated controller can be. */
	int high_speed;

	/** Manufacturer and product, or something made up if they cannot be read. */
	char name[128];
} UsbHostDeviceInfo;

/**
 * Whether passthrough can be attempted at all.
 *
 * @return Non-zero if libusb was built in and started up.
 */
int usb_host_available(void);

/** Why it is not available, for an interface to show. Never NULL. */
const char *usb_host_unavailable_reason(void);

/**
 * List what is plugged into the host, for a user to choose from.
 *
 * Hubs are left out: there is nothing useful to be done with one, and the root
 * hubs in particular are the host's own bus rather than devices on it.
 *
 * @param out Array to fill.
 * @param max How many it can hold.
 * @return How many were written, or a negative number if the list could not be
 *         read.
 */
int usb_host_enumerate(UsbHostDeviceInfo *out, int max);

struct UsbDevice;

/**
 * Take over a host device and present it to the guest.
 *
 * The host's own driver is detached from it while it is attached here, which is
 * why doing this to something the host is using takes it away from the host.
 *
 * @param vendor  Its vendor identifier.
 * @param product Its product identifier.
 * @param why     Set to an explanation if this returns NULL. May be NULL.
 * @return The device, or NULL.
 */
struct UsbDevice *usb_host_create(uint16_t vendor, uint16_t product,
                                  const char **why);

/** Give a host device back to the host. */
void usb_host_destroy(struct UsbDevice *dev);

/**
 * Let transfers that have finished be noticed.
 *
 * Called once per USB frame from the controller, on the emulator thread. Real
 * transfers take milliseconds, so nothing here ever waits for one: a transfer
 * is started, the guest is told to ask again, and the answer is picked up
 * whenever this finds it. That is exactly what a device answering NAK looks
 * like to a driver, which is why it needs nothing special of the guest.
 */
void usb_host_poll(void);

#endif /* USB_HOST_H */
