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
 * usb_device.h - a device on the emulated USB bus.
 *
 * The host controller knows nothing about what is plugged into it. It moves
 * bytes to and from an address and an endpoint, and a device answers. So a
 * device here is three things: what it says when asked to describe itself, what
 * it does with a control request, and what it has to send when polled.
 *
 * A device may be synthesised, or it may be a real one on the host reached
 * through libusb (usb_host.c). Nothing above this interface knows which, and
 * that is the point of having it: the controller moves bytes to an address and
 * an endpoint either way.
 */

#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdint.h>

/** How a transaction ended, in the words the controller reports upwards. */
typedef enum {
	USB_ACK = 0,		/* it worked */
	USB_STALL,		/* the endpoint refused it */
	USB_NAK,		/* nothing to say yet, ask again */
	USB_NO_DEVICE		/* nobody answered */
} UsbResult;

/** The eight bytes of a control request, as the host sends them. */
typedef struct {
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} UsbSetup;

/* bmRequestType fields worth naming. */
#define USB_DIR_IN		0x80
#define USB_TYPE_MASK		0x60
#define USB_TYPE_STANDARD	0x00
#define USB_TYPE_CLASS		0x20

/* Standard requests. */
#define USB_REQ_GET_STATUS		0x00
#define USB_REQ_CLEAR_FEATURE		0x01
#define USB_REQ_SET_FEATURE		0x03
#define USB_REQ_SET_ADDRESS		0x05
#define USB_REQ_GET_DESCRIPTOR		0x06
#define USB_REQ_GET_CONFIGURATION	0x08
#define USB_REQ_SET_CONFIGURATION	0x09
#define USB_REQ_GET_INTERFACE		0x0a
#define USB_REQ_SET_INTERFACE		0x0b

/* Descriptor types. */
#define USB_DT_DEVICE		1
#define USB_DT_CONFIG		2
#define USB_DT_STRING		3
#define USB_DT_INTERFACE	4
#define USB_DT_ENDPOINT		5
#define USB_DT_HID		0x21
#define USB_DT_HID_REPORT	0x22

struct UsbDevice;

/** What a device has to do. */
typedef struct {
	/** Its name, for the interface to show. */
	const char *name;

	/** Back to the power-on state, as a port reset does. */
	void (*reset)(struct UsbDevice *dev);

	/**
	 * Handle a control request on endpoint 0.
	 *
	 * @param setup The request.
	 * @param data  Buffer for the data stage, in or out as the request says.
	 * @param len   On entry the buffer size, on exit how much the device
	 *              supplied. Ignored for a request with no data stage.
	 * @return USB_ACK, or USB_STALL for a request it does not implement.
	 */
	UsbResult (*control)(struct UsbDevice *dev, const UsbSetup *setup,
	                     uint8_t *data, unsigned *len);

	/**
	 * Read from an IN endpoint other than 0.
	 *
	 * @return USB_ACK with @len set, or USB_NAK when there is nothing new,
	 *         which is the normal answer for an idle input device.
	 */
	UsbResult (*read)(struct UsbDevice *dev, unsigned endpoint,
	                  uint8_t *data, unsigned *len);

	/** Write to an OUT endpoint other than 0. */
	UsbResult (*write)(struct UsbDevice *dev, unsigned endpoint,
	                   const uint8_t *data, unsigned len);
} UsbDeviceOps;

/** A device attached to a port. */
typedef struct UsbDevice {
	const UsbDeviceOps *ops;

	/**
	 * The address the host has given it, 0 until SET_ADDRESS.
	 *
	 * Kept here rather than in the device, because every device gets one the
	 * same way and the controller is what needs to match on it.
	 */
	uint8_t address;

	/** Set if it reported itself as low speed at connect. */
	int low_speed;

	/** Whatever the device implementation needs. */
	void *state;
} UsbDevice;

/*
 * The only devices are real ones, from the host: see usb_host.h. There was a
 * synthesised gamepad here while the controller was being written, which is
 * what proved the transfer engine before there was anything real to plug in.
 * It has been removed now that there is, because a device that answers only
 * what the emulator already knows proves nothing further.
 */

#endif /* USB_DEVICE_H */
