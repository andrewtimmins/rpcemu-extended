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
 * usb_host.c - a real USB device on the host, presented to the guest.
 *
 * The emulated controller asks a device three things: describe yourself, do
 * this control request, and have you anything on this endpoint. A device here
 * answers them by putting the same question to real hardware through libusb, so
 * the guest is talking to the device itself and not to a model of it.
 *
 * The whole design turns on one constraint: a real transfer takes milliseconds
 * and the emulator thread cannot wait that long. Nothing here ever blocks
 * waiting for a transfer. A request is submitted, the guest is told NAK, and
 * the answer is collected on a later frame when it has arrived. NAK is what a
 * device says when it has nothing ready yet, so a driver already knows how to
 * cope: the descriptor stays active and it asks again. Passthrough therefore
 * needs nothing of the guest that ordinary USB does not.
 *
 * Completions are noticed by usb_host_poll(), which the controller calls once
 * per USB frame from the emulator thread. libusb's callbacks run inside that
 * call, on that same thread, which is why there is not a lock anywhere in this
 * file. Do not call anything here from the interface thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "usb_device.h"
#include "usb_host.h"

#ifndef RPCEMU_HAVE_LIBUSB

/*
 * Built without libusb. Everything fails politely: the interface will say so
 * rather than offering a list of devices that could never be attached.
 */

int
usb_host_available(void)
{
	return 0;
}

const char *
usb_host_unavailable_reason(void)
{
	return "This build of RPCEmu was made without libusb, so it cannot reach "
	       "the host's USB devices.";
}

int
usb_host_enumerate(UsbHostDeviceInfo *out, int max)
{
	(void) out;
	(void) max;

	return -1;
}

UsbDevice *
usb_host_create(uint16_t vendor, uint16_t product, const char **why)
{
	(void) vendor;
	(void) product;

	if (why != NULL) {
		*why = usb_host_unavailable_reason();
	}

	return NULL;
}

void
usb_host_destroy(UsbDevice *dev)
{
	(void) dev;
}

int
usb_host_device_gone(const UsbDevice *dev)
{
	(void) dev;

	return 0;
}

void
usb_host_poll(void)
{
}

#else /* RPCEMU_HAVE_LIBUSB */

#include <libusb.h>

/**
 * The most one transfer will move.
 *
 * Matches OHCI_MAX_TRANSFER on the controller side: one OHCI descriptor can
 * cover two 4K pages, so a guest can legitimately ask for 8K at once. The old
 * value of 1K came from the ISP1161's ten-bit byte count and silently truncated
 * anything bigger - a webcam's configuration descriptor among them.
 */
#define HOST_MAX_TRANSFER	8192

/*
 * One slot per endpoint and direction, so that a driver polling an interrupt
 * endpoint does not stop it making a control request at the same time. Slots
 * 0-15 are the OUT direction and control, 16-31 are IN.
 */
#define HOST_SLOTS		32
#define HOST_SLOT_IN(ep)	(16 + ((ep) & 0xf))
#define HOST_SLOT_OUT(ep)	((ep) & 0xf)

/** How long a control request is given before it is called a failure. */
#define HOST_CONTROL_TIMEOUT_MS	1000

typedef enum {
	SLOT_IDLE = 0,		/* nothing in flight */
	SLOT_PENDING,		/* submitted, waiting for the device */
	SLOT_DONE		/* finished, and the answer not yet collected */
} SlotState;

struct HostDevice;

typedef struct {
	SlotState state;
	struct libusb_transfer *xfer;
	uint8_t *buffer;	/* setup packet, then the data */
	UsbSetup setup;		/* the request this slot is answering */
	UsbResult result;
	unsigned actual;

	/* Which device this belongs to, so a transfer that comes back saying the
	   device has gone can say which one. */
	struct HostDevice *owner;
} Slot;

typedef struct HostDevice {
	libusb_device_handle *handle;
	uint16_t vendor;
	uint16_t product;
	char name[128];

	/**
	 * The device has left the host's bus.
	 *
	 * Set from the hotplug callback, or by a transfer coming back to say
	 * nobody answered. Nothing is torn down here: the controller notices on
	 * its next frame and unplugs the port, so that the guest is told in the
	 * one way it understands.
	 */
	int gone;

	/*
	 * Its own copy of the operations, because the name in them is its own.
	 * The synthesised devices can share one static set; these cannot.
	 */
	UsbDeviceOps ops;

	Slot slots[HOST_SLOTS];

	/* What each endpoint is, learnt from the active configuration. */
	uint8_t ep_type[HOST_SLOTS];		/* libusb transfer type, or 0xff */
	uint16_t ep_packet[HOST_SLOTS];

	/* Interfaces taken from the host, to be given back on the way out. */
	uint8_t claimed[32];
	int claimed_count;

	/* The last submit failure reported, so a request that keeps failing says
	   so once rather than a thousand times a second. */
	int last_control_error;
} HostDevice;

/*
 * The devices currently handed to the guest. Only ever two, one per root hub
 * port, but kept as a list because the hotplug callback is told which device
 * left and has to find out whether it was one of ours.
 */
#define HOST_MAX_DEVICES	4

static libusb_context *usb_host_context;
static int usb_host_started;		/* an attempt has been made */
static int usb_host_attached;		/* how many devices are in the ports */
static char usb_host_reason[256];
static HostDevice *usb_host_devices[HOST_MAX_DEVICES];
static libusb_hotplug_callback_handle usb_host_hotplug_handle;
static int usb_host_hotplug_registered;

/* --- Starting up --------------------------------------------------------- */

/**
 * A device has left the host's bus.
 *
 * Runs inside usb_host_poll(), on the emulator thread, like every other libusb
 * callback here - so it can mark the device without a lock. It does no more
 * than mark it: libusb forbids most calls on a device that has gone, and the
 * tidying up wants to happen where the controller can tell the guest about it.
 */
static int LIBUSB_CALL
usb_host_left(libusb_context *ctx, libusb_device *device,
              libusb_hotplug_event event, void *user_data)
{
	int i;

	(void) ctx;
	(void) event;
	(void) user_data;

	for (i = 0; i < HOST_MAX_DEVICES; i++) {
		HostDevice *hd = usb_host_devices[i];

		if (hd != NULL && hd->handle != NULL && !hd->gone &&
		    libusb_get_device(hd->handle) == device) {
			hd->gone = 1;
			rpclog("USB: %s (%04x:%04x) has been unplugged from the host\n",
			    hd->name, hd->vendor, hd->product);
		}
	}

	return 0;		/* keep the registration */
}

/** Note a device as one of ours, so the hotplug callback can recognise it. */
static void
usb_host_remember(HostDevice *hd)
{
	int i;

	for (i = 0; i < HOST_MAX_DEVICES; i++) {
		if (usb_host_devices[i] == NULL) {
			usb_host_devices[i] = hd;
			return;
		}
	}
}

static void
usb_host_forget(HostDevice *hd)
{
	int i;

	for (i = 0; i < HOST_MAX_DEVICES; i++) {
		if (usb_host_devices[i] == hd) {
			usb_host_devices[i] = NULL;
		}
	}
}

/**
 * Start libusb, once.
 *
 * Deliberately lazy. Most machines have no passthrough configured and there is
 * no reason to talk to the host's USB bus at all in that case.
 */
static int
usb_host_start(void)
{
	int err;

	if (usb_host_started) {
		return usb_host_context != NULL;
	}

	usb_host_started = 1;

	err = libusb_init(&usb_host_context);
	if (err != 0) {
		usb_host_context = NULL;
		snprintf(usb_host_reason, sizeof(usb_host_reason),
		    "The host's USB bus could not be opened: %s.",
		    libusb_strerror(err));
		rpclog("USB: libusb_init failed: %s\n", libusb_strerror(err));
		return 0;
	}

	/*
	 * Ask to be told when a device leaves. Not every platform can do this, and
	 * it is not worth refusing passthrough over: a transfer coming back to say
	 * nobody answered notices the same thing, only later and only if something
	 * was being transferred at the time.
	 */
	if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		err = libusb_hotplug_register_callback(usb_host_context,
		    LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
		    LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
		    LIBUSB_HOTPLUG_MATCH_ANY, usb_host_left, NULL,
		    &usb_host_hotplug_handle);

		if (err == LIBUSB_SUCCESS) {
			usb_host_hotplug_registered = 1;
		} else {
			rpclog("USB: cannot watch for devices being unplugged: %s\n",
			    libusb_strerror(err));
		}
	} else {
		rpclog("USB: this libusb cannot report devices being unplugged; a "
		       "device that goes will be noticed on its next transfer\n");
	}

	return 1;
}

int
usb_host_available(void)
{
	return usb_host_start();
}

const char *
usb_host_unavailable_reason(void)
{
	if (usb_host_context != NULL) {
		return "";
	}
	if (usb_host_reason[0] == '\0') {
		return "The host's USB bus is not available.";
	}

	return usb_host_reason;
}

/* --- Describing what the host has ---------------------------------------- */

/**
 * The class a device should be listed under.
 *
 * A device that declares no class of its own is saying the class belongs to its
 * interfaces, so the first interface is asked instead. That is the same rule
 * *USBDevices follows in the guest, and for the same reason: zero tells a user
 * nothing.
 */
static uint8_t
usb_host_class_of(libusb_device *dev, const struct libusb_device_descriptor *dd)
{
	struct libusb_config_descriptor *config;
	uint8_t result = dd->bDeviceClass;

	if (result != 0) {
		return result;
	}

	if (libusb_get_active_config_descriptor(dev, &config) != 0) {
		return 0;
	}

	if (config->bNumInterfaces > 0 && config->interface[0].num_altsetting > 0) {
		result = config->interface[0].altsetting[0].bInterfaceClass;
	}

	libusb_free_config_descriptor(config);

	return result;
}

/**
 * Fill in what a device calls itself.
 *
 * The strings live on the device and reading them means opening it, which is
 * exactly what a host without the right permissions cannot do. That is not a
 * failure worth reporting: the identifiers are enough to tell devices apart, so
 * the fallback says what is known rather than nothing.
 */
static void
usb_host_describe(libusb_device_handle *handle,
                  const struct libusb_device_descriptor *dd,
                  char *out, size_t max)
{
	unsigned char manufacturer[96];
	unsigned char product[96];
	int got_manufacturer = 0;
	int got_product = 0;

	manufacturer[0] = '\0';
	product[0] = '\0';

	if (handle != NULL) {
		if (dd->iManufacturer != 0 &&
		    libusb_get_string_descriptor_ascii(handle, dd->iManufacturer,
		        manufacturer, sizeof(manufacturer)) > 0) {
			got_manufacturer = 1;
		}
		if (dd->iProduct != 0 &&
		    libusb_get_string_descriptor_ascii(handle, dd->iProduct,
		        product, sizeof(product)) > 0) {
			got_product = 1;
		}
	}

	if (got_manufacturer && got_product) {
		snprintf(out, max, "%s %s", manufacturer, product);
	} else if (got_product) {
		snprintf(out, max, "%s", product);
	} else if (got_manufacturer) {
		snprintf(out, max, "%s device", manufacturer);
	} else {
		snprintf(out, max, "Unnamed device %04x:%04x",
		    dd->idVendor, dd->idProduct);
	}
}

int
usb_host_enumerate(UsbHostDeviceInfo *out, int max)
{
	libusb_device **list;
	ssize_t count, i;
	int found = 0;

	if (!usb_host_start() || out == NULL || max <= 0) {
		return -1;
	}

	count = libusb_get_device_list(usb_host_context, &list);
	if (count < 0) {
		return -1;
	}

	for (i = 0; i < count && found < max; i++) {
		struct libusb_device_descriptor dd;
		libusb_device_handle *handle = NULL;
		UsbHostDeviceInfo *info = &out[found];
		int speed;

		if (libusb_get_device_descriptor(list[i], &dd) != 0) {
			continue;
		}

		/*
		 * Hubs are left out. The root hub is the host's own controller
		 * rather than a device on the bus, and taking a real hub away
		 * from the host would take everything below it too.
		 */
		if (dd.bDeviceClass == LIBUSB_CLASS_HUB) {
			continue;
		}

		memset(info, 0, sizeof(*info));
		info->vendor = dd.idVendor;
		info->product = dd.idProduct;
		info->bus = libusb_get_bus_number(list[i]);
		info->address = libusb_get_device_address(list[i]);
		info->device_class = usb_host_class_of(list[i], &dd);
		info->in_use = -1;

		speed = libusb_get_device_speed(list[i]);
		info->high_speed = (speed == LIBUSB_SPEED_HIGH ||
		                    speed == LIBUSB_SPEED_SUPER ||
		                    speed == LIBUSB_SPEED_SUPER_PLUS);

		if (libusb_open(list[i], &handle) == 0) {
			info->openable = 1;
			info->in_use = libusb_kernel_driver_active(handle, 0) == 1;
		}

		usb_host_describe(handle, &dd, info->name, sizeof(info->name));

		if (handle != NULL) {
			libusb_close(handle);
		}

		found++;
	}

	libusb_free_device_list(list, 1);

	return found;
}

/* --- Transfers ----------------------------------------------------------- */

/**
 * A transfer has finished, one way or another.
 *
 * Runs inside usb_host_poll() on the emulator thread, so it can touch the slot
 * without any locking. It records the outcome in the words the controller uses
 * and leaves the collecting to whichever call next asks about that endpoint.
 */
static void LIBUSB_CALL
usb_host_transfer_done(struct libusb_transfer *xfer)
{
	Slot *slot = xfer->user_data;

	slot->actual = (unsigned) (xfer->actual_length < 0 ? 0 : xfer->actual_length);

	switch (xfer->status) {
	case LIBUSB_TRANSFER_COMPLETED:
		slot->result = USB_ACK;
		break;

	case LIBUSB_TRANSFER_STALL:
		slot->result = USB_STALL;
		break;

	case LIBUSB_TRANSFER_TIMED_OUT:
		/* Nothing to say within the time allowed, which is what an idle
		   input device looks like rather than an error. */
		slot->result = USB_NAK;
		break;

	case LIBUSB_TRANSFER_NO_DEVICE:
		/*
		 * It has been unplugged. The hotplug callback usually says so first,
		 * but not every platform has one, and this is the same news arriving
		 * by the other route.
		 */
		if (slot->owner != NULL && !slot->owner->gone) {
			slot->owner->gone = 1;
			rpclog("USB: %s (%04x:%04x) did not answer; treating it as "
			       "unplugged\n", slot->owner->name, slot->owner->vendor,
			    slot->owner->product);
		}
		slot->result = USB_NO_DEVICE;
		break;

	default:
		slot->result = USB_NO_DEVICE;
		break;
	}

	slot->state = SLOT_DONE;
}

/** Make sure a slot has somewhere to put a transfer. */
static int
usb_host_slot_ready(Slot *slot)
{
	if (slot->xfer == NULL) {
		slot->xfer = libusb_alloc_transfer(0);
		if (slot->xfer == NULL) {
			return 0;
		}
	}
	if (slot->buffer == NULL) {
		slot->buffer = malloc(LIBUSB_CONTROL_SETUP_SIZE + HOST_MAX_TRANSFER);
		if (slot->buffer == NULL) {
			return 0;
		}
	}

	return 1;
}

/** Start a control request on its way. */
static void
usb_host_start_control(HostDevice *hd, const UsbSetup *setup,
                       const uint8_t *out_data)
{
	Slot *slot = &hd->slots[0];
	unsigned length = setup->length;
	int err;

	if (!usb_host_slot_ready(slot)) {
		return;
	}
	if (length > HOST_MAX_TRANSFER) {
		length = HOST_MAX_TRANSFER;
	}

	libusb_fill_control_setup(slot->buffer, setup->request_type,
	    setup->request, setup->value, setup->index, (uint16_t) length);

	if (!(setup->request_type & USB_DIR_IN) && length > 0 && out_data != NULL) {
		memcpy(slot->buffer + LIBUSB_CONTROL_SETUP_SIZE, out_data, length);
	}

	libusb_fill_control_transfer(slot->xfer, hd->handle, slot->buffer,
	    usb_host_transfer_done, slot, HOST_CONTROL_TIMEOUT_MS);

	slot->setup = *setup;
	slot->state = SLOT_PENDING;

	err = libusb_submit_transfer(slot->xfer);
	if (err != 0) {
		/*
		 * Worth saying out loud. A request that cannot even be started looks
		 * from the guest exactly like a device that never answers, and without
		 * this the difference is invisible.
		 */
		slot->state = SLOT_IDLE;
		if (err != hd->last_control_error) {
			hd->last_control_error = err;
			rpclog("USB: %04x:%04x would not take a request: %s\n",
			    hd->vendor, hd->product, libusb_strerror(err));
		}
		return;
	}

	hd->last_control_error = 0;
}

/**
 * Start a transfer on an endpoint other than 0.
 *
 * Given no timeout on purpose. An interrupt IN transfer that simply waits is
 * how a real host polls an input device: it sits there until the device has
 * something, and the guest sees NAK in the meantime.
 */
static void
usb_host_start_endpoint(HostDevice *hd, unsigned slot_index, unsigned char ep,
                        unsigned length, const uint8_t *out_data)
{
	Slot *slot = &hd->slots[slot_index];
	uint8_t type = hd->ep_type[slot_index];

	if (!usb_host_slot_ready(slot)) {
		return;
	}
	if (length > HOST_MAX_TRANSFER) {
		length = HOST_MAX_TRANSFER;
	}
	if (length == 0) {
		return;
	}

	if (out_data != NULL) {
		memcpy(slot->buffer, out_data, length);
	}

	if (type == LIBUSB_TRANSFER_TYPE_BULK) {
		libusb_fill_bulk_transfer(slot->xfer, hd->handle, ep, slot->buffer,
		    (int) length, usb_host_transfer_done, slot, 0);
	} else {
		/* Interrupt is the assumption for anything not known to be bulk:
		   the endpoints this can reach at all are one or the other, and
		   an interrupt transfer of the wrong guess still moves the data. */
		libusb_fill_interrupt_transfer(slot->xfer, hd->handle, ep, slot->buffer,
		    (int) length, usb_host_transfer_done, slot, 0);
	}

	slot->state = SLOT_PENDING;

	if (libusb_submit_transfer(slot->xfer) != 0) {
		slot->state = SLOT_IDLE;
	}
}

/** Two requests are the same request if all eight bytes of them agree. */
static int
usb_host_same_setup(const UsbSetup *a, const UsbSetup *b)
{
	return a->request_type == b->request_type && a->request == b->request &&
	       a->value == b->value && a->index == b->index && a->length == b->length;
}

/* --- Configuration ------------------------------------------------------- */

/**
 * Give every interface back to the host.
 *
 * In the reverse of the order they were taken, and that matters. Releasing an
 * interface is what puts the host's driver back on it, and a driver that spans
 * several - a webcam's control interface and the streaming interface beneath it,
 * say - looks for the others the moment it is given the first. Handing back the
 * lowest-numbered one first therefore wakes the driver while the rest are still
 * held, and it gives up: the device comes back attached but useless, until it is
 * unplugged and plugged in again. Going backwards means the interface that wakes
 * the driver is the last one released, by which time the others are there.
 */
static void
usb_host_release_interfaces(HostDevice *hd)
{
	int i;

	for (i = hd->claimed_count - 1; i >= 0; i--) {
		libusb_release_interface(hd->handle, hd->claimed[i]);
	}

	hd->claimed_count = 0;
}

/**
 * Take the active configuration's interfaces, and learn its endpoints.
 *
 * Claiming is what takes a device away from the host's own driver, and it is
 * also what has to happen before any transfer on those interfaces is allowed.
 * The endpoint map is worth building at the same time: a transfer has to be
 * submitted as the right kind, and only the configuration says which that is.
 */
static void
usb_host_adopt_configuration(HostDevice *hd)
{
	struct libusb_config_descriptor *config;
	libusb_device *dev = libusb_get_device(hd->handle);
	int i, j, k;

	memset(hd->ep_type, 0xff, sizeof(hd->ep_type));
	memset(hd->ep_packet, 0, sizeof(hd->ep_packet));

	{
		const int err = libusb_get_active_config_descriptor(dev, &config);

		if (err != 0) {
			/* Without this nothing can be claimed and no endpoint is known,
			   which shows up much later as a device that will not answer. */
			rpclog("USB: %04x:%04x has no active configuration to adopt: %s\n",
			    hd->vendor, hd->product, libusb_strerror(err));
			return;
		}
	}

	for (i = 0; i < config->bNumInterfaces; i++) {
		const struct libusb_interface *iface = &config->interface[i];

		for (j = 0; j < iface->num_altsetting; j++) {
			const struct libusb_interface_descriptor *alt = &iface->altsetting[j];

			if (j == 0 && hd->claimed_count < (int) sizeof(hd->claimed)) {
				if (libusb_claim_interface(hd->handle,
				        alt->bInterfaceNumber) == 0) {
					hd->claimed[hd->claimed_count++] = alt->bInterfaceNumber;
				} else {
					rpclog("USB: could not claim interface %d of %04x:%04x\n",
					    alt->bInterfaceNumber, hd->vendor, hd->product);
				}
			}

			for (k = 0; k < alt->bNumEndpoints; k++) {
				const struct libusb_endpoint_descriptor *ep =
				    &alt->endpoint[k];
				const unsigned index = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
				    ? HOST_SLOT_IN(ep->bEndpointAddress)
				    : HOST_SLOT_OUT(ep->bEndpointAddress);

				hd->ep_type[index] =
				    ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
				hd->ep_packet[index] = ep->wMaxPacketSize;
			}
		}
	}

	libusb_free_config_descriptor(config);
}

/**
 * Requests libusb has to make happen rather than pass on.
 *
 * Changing configuration or alternate setting, and clearing a stalled endpoint,
 * are all things the host's own stack has to know about. Sent as plain control
 * requests they would confuse it, so they are turned into the libusb call that
 * means the same thing.
 *
 * @return 1 if this call dealt with the request.
 */
static int
usb_host_control_is_ours(HostDevice *hd, const UsbSetup *setup, UsbResult *result)
{
	if ((setup->request_type & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
		return 0;
	}

	switch (setup->request) {
	case USB_REQ_SET_CONFIGURATION:
		usb_host_release_interfaces(hd);
		if (libusb_set_configuration(hd->handle, (int) (setup->value & 0xff)) != 0) {
			/*
			 * Not fatal. A device with one configuration is already in
			 * it, and some hosts refuse to set the one that is current;
			 * the interfaces still need taking either way.
			 */
			rpclog("USB: %04x:%04x would not take configuration %u\n",
			    hd->vendor, hd->product, setup->value & 0xff);
		}
		usb_host_adopt_configuration(hd);
		*result = USB_ACK;
		return 1;

	case USB_REQ_SET_INTERFACE:
		if (libusb_set_interface_alt_setting(hd->handle,
		        (int) (setup->index & 0xff), (int) (setup->value & 0xff)) != 0) {
			*result = USB_STALL;
		} else {
			usb_host_adopt_configuration(hd);
			*result = USB_ACK;
		}
		return 1;

	case USB_REQ_CLEAR_FEATURE:
		/* ENDPOINT_HALT, addressed to an endpoint. Anything else is the
		   device's own business and goes through unaltered. */
		if ((setup->request_type & 0x1f) == 0x02 && setup->value == 0) {
			libusb_clear_halt(hd->handle,
			    (unsigned char) (setup->index & 0xff));
			*result = USB_ACK;
			return 1;
		}
		return 0;

	default:
		return 0;
	}
}

/* --- What the controller asks of a device -------------------------------- */

static void
usb_host_reset(UsbDevice *dev)
{
	HostDevice *hd = dev->state;
	int i;

	/*
	 * A port reset here is not passed on as a bus reset to the real device.
	 * The host is still using its own bus and resetting a device underneath
	 * it would be rude at best; the guest's reset is about the emulated port,
	 * so all that is dropped is whatever this end had in flight.
	 */
	for (i = 0; i < HOST_SLOTS; i++) {
		Slot *slot = &hd->slots[i];

		if (slot->state == SLOT_DONE) {
			slot->state = SLOT_IDLE;
		}
	}
}

static UsbResult
usb_host_control(UsbDevice *dev, const UsbSetup *setup, uint8_t *data,
                 unsigned *len)
{
	HostDevice *hd = dev->state;
	Slot *slot = &hd->slots[0];
	const unsigned max = *len;
	UsbResult result;

	*len = 0;

	if (hd->handle == NULL || hd->gone) {
		return USB_NO_DEVICE;
	}

	if (usb_host_control_is_ours(hd, setup, &result)) {
		return result;
	}


	if (slot->state == SLOT_DONE && usb_host_same_setup(&slot->setup, setup)) {
		unsigned actual = slot->actual;

		slot->state = SLOT_IDLE;

		if (slot->result == USB_ACK && (setup->request_type & USB_DIR_IN)) {
			if (actual > max) {
				actual = max;
			}
			memcpy(data, slot->buffer + LIBUSB_CONTROL_SETUP_SIZE, actual);
			*len = actual;
		}

		return slot->result;
	}

	if (slot->state == SLOT_IDLE) {
		usb_host_start_control(hd, setup,
		    (setup->request_type & USB_DIR_IN) ? NULL : data);
	}

	/* Ask again: the device is being asked in the meantime. */
	return USB_NAK;
}

static UsbResult
usb_host_read(UsbDevice *dev, unsigned endpoint, uint8_t *data, unsigned *len)
{
	HostDevice *hd = dev->state;
	const unsigned index = HOST_SLOT_IN(endpoint);
	Slot *slot = &hd->slots[index];
	const unsigned max = *len;
	unsigned wanted = hd->ep_packet[index];

	*len = 0;

	if (hd->handle == NULL || hd->gone) {
		return USB_NO_DEVICE;
	}
	if (hd->ep_type[index] == 0xff) {
		/* Nothing in the configuration says this endpoint exists. */
		return USB_STALL;
	}

	if (wanted == 0 || wanted > max) {
		wanted = max;
	}

	if (slot->state == SLOT_DONE) {
		const UsbResult result = slot->result;
		unsigned actual = slot->actual;

		slot->state = SLOT_IDLE;

		if (result == USB_ACK) {
			if (actual > max) {
				actual = max;
			}
			memcpy(data, slot->buffer, actual);
			*len = actual;
		}

		if (result == USB_ACK || result == USB_NAK) {
			/* Keep one in flight. An interrupt endpoint is polled without
			   pause, and having asked already is what makes the next
			   report ready by the time the driver comes back. */
			usb_host_start_endpoint(hd, index,
			    (unsigned char) (endpoint | LIBUSB_ENDPOINT_IN), wanted, NULL);
		}

		return result;
	}

	if (slot->state == SLOT_IDLE) {
		usb_host_start_endpoint(hd, index,
		    (unsigned char) (endpoint | LIBUSB_ENDPOINT_IN), wanted, NULL);
	}

	return USB_NAK;
}

static UsbResult
usb_host_write(UsbDevice *dev, unsigned endpoint, const uint8_t *data,
               unsigned len)
{
	HostDevice *hd = dev->state;
	const unsigned index = HOST_SLOT_OUT(endpoint);
	Slot *slot = &hd->slots[index];

	if (hd->handle == NULL || hd->gone) {
		return USB_NO_DEVICE;
	}
	if (hd->ep_type[index] == 0xff) {
		return USB_STALL;
	}

	if (slot->state == SLOT_DONE) {
		const UsbResult result = slot->result;

		slot->state = SLOT_IDLE;

		return result;
	}

	if (slot->state == SLOT_IDLE) {
		usb_host_start_endpoint(hd, index, (unsigned char) (endpoint & 0x7f),
		    len, data);
	}

	return USB_NAK;
}

/* --- Coming and going ---------------------------------------------------- */

/** Find a device by what it says it is, rather than by where it is plugged in. */
static libusb_device *
usb_host_find(uint16_t vendor, uint16_t product, libusb_device ***list_out)
{
	libusb_device **list;
	ssize_t count, i;

	count = libusb_get_device_list(usb_host_context, &list);
	if (count < 0) {
		*list_out = NULL;
		return NULL;
	}

	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor dd;

		if (libusb_get_device_descriptor(list[i], &dd) != 0) {
			continue;
		}
		if (dd.idVendor == vendor && dd.idProduct == product) {
			*list_out = list;
			return list[i];
		}
	}

	libusb_free_device_list(list, 1);
	*list_out = NULL;

	return NULL;
}

UsbDevice *
usb_host_create(uint16_t vendor, uint16_t product, const char **why)
{
	static char trouble[256];

	struct libusb_device_descriptor dd;
	libusb_device **list = NULL;
	libusb_device *found;
	libusb_device_handle *handle = NULL;
	UsbDevice *dev;
	HostDevice *hd;
	int err, speed, i;

	if (why != NULL) {
		*why = "";
	}

	if (!usb_host_start()) {
		if (why != NULL) {
			*why = usb_host_unavailable_reason();
		}
		return NULL;
	}

	found = usb_host_find(vendor, product, &list);
	if (found == NULL) {
		snprintf(trouble, sizeof(trouble),
		    "No device %04x:%04x is plugged into this computer.",
		    vendor, product);
		if (why != NULL) {
			*why = trouble;
		}
		return NULL;
	}

	err = libusb_open(found, &handle);
	if (err != 0) {
		if (err == LIBUSB_ERROR_ACCESS) {
			snprintf(trouble, sizeof(trouble),
			    "Device %04x:%04x cannot be opened: this account is not "
			    "allowed to use it. See docs/usb.md for the udev rule that "
			    "grants access.", vendor, product);
		} else {
			snprintf(trouble, sizeof(trouble),
			    "Device %04x:%04x could not be opened: %s.",
			    vendor, product, libusb_strerror(err));
		}
		if (why != NULL) {
			*why = trouble;
		}
		libusb_free_device_list(list, 1);
		return NULL;
	}

	dev = calloc(1, sizeof(UsbDevice));
	hd = calloc(1, sizeof(HostDevice));

	if (dev == NULL || hd == NULL) {
		free(dev);
		free(hd);
		libusb_close(handle);
		libusb_free_device_list(list, 1);
		if (why != NULL) {
			*why = "Out of memory.";
		}
		return NULL;
	}

	hd->handle = handle;
	hd->vendor = vendor;
	hd->product = product;

	if (libusb_get_device_descriptor(found, &dd) == 0) {
		usb_host_describe(handle, &dd, hd->name, sizeof(hd->name));
	} else {
		snprintf(hd->name, sizeof(hd->name), "USB device %04x:%04x",
		    vendor, product);
	}

	speed = libusb_get_device_speed(found);

	/*
	 * Detaching the host's driver is what makes the device answer to us
	 * instead of to the host, and re-attaching it on the way out is what
	 * gives it back. libusb will do both around a claim if asked.
	 */
	libusb_set_auto_detach_kernel_driver(handle, 1);
	usb_host_adopt_configuration(hd);

	for (i = 0; i < HOST_SLOTS; i++) {
		hd->slots[i].owner = hd;
	}

	hd->ops.name = hd->name;
	hd->ops.reset = usb_host_reset;
	hd->ops.control = usb_host_control;
	hd->ops.read = usb_host_read;
	hd->ops.write = usb_host_write;

	dev->ops = &hd->ops;
	dev->state = hd;
	dev->address = 0;
	dev->low_speed = (speed == LIBUSB_SPEED_LOW);

	usb_host_remember(hd);
	usb_host_attached++;

	rpclog("USB: %s (%04x:%04x) taken over from the host\n",
	    hd->name, vendor, product);

	libusb_free_device_list(list, 1);

	return dev;
}

void
usb_host_destroy(UsbDevice *dev)
{
	HostDevice *hd;
	int i, rounds, waiting = 0;

	if (dev == NULL) {
		return;
	}

	hd = dev->state;
	if (hd == NULL) {
		free(dev);
		return;
	}

	/*
	 * A submitted transfer belongs to libusb until its callback has run, so
	 * cancelling is only half of it: the events have to be pumped until each
	 * one has actually come back before its memory can go.
	 */
	for (i = 0; i < HOST_SLOTS; i++) {
		if (hd->slots[i].state == SLOT_PENDING) {
			libusb_cancel_transfer(hd->slots[i].xfer);
			waiting = 1;
		}
	}

	/*
	 * Bounded, because this runs on the emulator thread and the case that most
	 * needs cancelling is a device that has been pulled out - the one least
	 * likely to answer. A hundred turns of ten milliseconds is a second, far
	 * longer than a cancellation takes, and stopping is better than a machine
	 * that hangs on the way out.
	 */
	for (rounds = 0; waiting && rounds < 100; rounds++) {
		struct timeval tv;

		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		libusb_handle_events_timeout_completed(usb_host_context, &tv, NULL);

		waiting = 0;
		for (i = 0; i < HOST_SLOTS; i++) {
			if (hd->slots[i].state == SLOT_PENDING) {
				waiting = 1;
			}
		}
	}

	for (i = 0; i < HOST_SLOTS; i++) {
		if (hd->slots[i].xfer != NULL) {
			/*
			 * Still in libusb's hands after all that. Letting it go would be
			 * a use-after-free when it finally came back, so the transfer and
			 * its buffer are deliberately leaked instead. Two allocations,
			 * once, against a crash.
			 */
			if (hd->slots[i].state == SLOT_PENDING) {
				rpclog("USB: a transfer to %04x:%04x would not come back; "
				       "leaving it to libusb\n", hd->vendor, hd->product);
				continue;
			}
			libusb_free_transfer(hd->slots[i].xfer);
		}
		free(hd->slots[i].buffer);
	}

	if (hd->handle != NULL) {
		usb_host_release_interfaces(hd);
		libusb_close(hd->handle);
		rpclog("USB: %s (%04x:%04x) given back to the host\n",
		    hd->name, hd->vendor, hd->product);
	}

	usb_host_forget(hd);

	if (usb_host_attached > 0) {
		usb_host_attached--;
	}

	free(hd);
	free(dev);
}

int
usb_host_device_gone(const UsbDevice *dev)
{
	const HostDevice *hd;
	int i;

	if (dev == NULL || dev->state == NULL) {
		return 0;
	}

	/*
	 * Checked against the list rather than trusted, because this is called
	 * for whatever is in a port and only a passed-through device has a
	 * HostDevice behind it.
	 */
	hd = dev->state;
	for (i = 0; i < HOST_MAX_DEVICES; i++) {
		if (usb_host_devices[i] == hd) {
			return hd->gone;
		}
	}

	return 0;
}

void
usb_host_poll(void)
{
	static unsigned idle_frames;
	struct timeval tv;

	if (usb_host_context == NULL) {
		return;
	}

	if (usb_host_attached == 0) {
		/*
		 * Nothing of ours is in flight, but libusb still has to be given the
		 * chance to notice a device arriving. Once a hotplug callback is
		 * registered it keeps its device list from those events rather than
		 * re-reading the bus on demand, so a device plugged in while nothing
		 * was attached would otherwise stay invisible - to the dialogue's
		 * list, and to a machine trying to take up the device its
		 * configuration names.
		 *
		 * Ten times a second is ample for that, and saves a syscall on every
		 * one of the thousand frames a second where there is nothing to do.
		 */
		if (!usb_host_hotplug_registered) {
			return;
		}
		if (++idle_frames < 100) {
			return;
		}
		idle_frames = 0;
	}

	/* Zero timeout: this runs on the emulator thread once a frame and must
	   never wait. Anything that has finished is dealt with, and anything that
	   has not is looked at again next time. */
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	libusb_handle_events_timeout_completed(usb_host_context, &tv, NULL);
}

#endif /* RPCEMU_HAVE_LIBUSB */
