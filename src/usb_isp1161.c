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
 * usb_isp1161.c - the USB host controller on the support card.
 *
 * Every access is a command followed by data, so the interesting part of this
 * file is a small state machine rather than a table of registers. Writing to the
 * command port latches which register is wanted and which direction, and resets
 * the half-phase; each subsequent data port access moves 16 bits and advances it.
 * A 32-bit register is two halves, low first, and the buffer ports stream
 * through buffer memory until the transfer counter runs out.
 *
 * Getting that phase wrong is the failure to expect, and it is invisible from a
 * single access: a driver that reads a 32-bit register would get the low half
 * twice and see a plausible-looking wrong answer. So the phase is reset by the
 * command and nothing else, exactly as the hardware does it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "usb_device.h"
#include "usb_host.h"
#include "usb_isp1161.h"

/* --- The ports, as byte offsets in the window --------------------------- */

#define PORT_HC_DATA	0x0
#define PORT_HC_COMMAND	0x4
#define PORT_DC_DATA	0x8
#define PORT_DC_COMMAND	0xc

/* Bit 7 of a command means "I am going to write". */
#define COMMAND_WRITE	0x80
#define COMMAND_REGISTER 0x7f

/* --- Register numbers --------------------------------------------------- */

#define HcRevision		0x00
#define HcControl		0x01
#define HcCommandStatus		0x02
#define HcInterruptStatus	0x03
#define HcInterruptEnable	0x04
#define HcInterruptDisable	0x05
#define HcFmInterval		0x0d
#define HcFmRemaining		0x0e
#define HcFmNumber		0x0f
#define HcLSThreshold		0x11
#define HcRhDescriptorA		0x12
#define HcRhDescriptorB		0x13
#define HcRhStatus		0x14
#define HcRhPortStatus1		0x15
#define HcRhPortStatus2		0x16

#define HcHardwareConfiguration	0x20
#define HcDMAConfiguration	0x21
#define HcTransferCounter	0x22
#define HcuPInterrupt		0x24
#define HcuPInterruptEnable	0x25
#define HcChipID		0x27
#define HcScratch		0x28
#define HcSoftwareReset		0x29
#define HcITLBufferLength	0x2a
#define HcATLBufferLength	0x2b
#define HcBufferStatus		0x2c
#define HcReadBackITL0Length	0x2d
#define HcReadBackITL1Length	0x2e
#define HcITLBufferPort		0x40
#define HcATLBufferPort		0x41

/* HcuPInterrupt sources. */
#define uPInt_SOFITL		(1u << 0)
#define uPInt_ATL		(1u << 1)
#define uPInt_AllEOT		(1u << 2)
#define uPInt_OPR_Reg		(1u << 4)
#define uPInt_HCSuspended	(1u << 5)
#define uPInt_ClkReady		(1u << 6)
#define uPInt_All		0x77

/*
 * A transfer descriptor, as four little-endian halfwords. Taken from the
 * ISP1161/ISP1362 data sheet's PTD bit description rather than inferred: the one
 * hand-built example in ROOL's driver has two comments that disagree with its own
 * bytes, so the descriptor was the wrong thing to learn this from.
 */
#define PTD_WORDS		4
#define PTD_HEADER_SIZE		8

/* Word 0 */
#define PTD_ACTUAL_BYTES(w)	((w) & 0x3ff)
#define PTD_TOGGLE		(1u << 10)
#define PTD_ACTIVE		(1u << 11)
#define PTD_CC_SHIFT		12

/* Word 1 */
#define PTD_MAX_PACKET(w)	((w) & 0x3ff)
#define PTD_SPEED_LOW		(1u << 10)
#define PTD_LAST		(1u << 11)
#define PTD_ENDPOINT(w)		(((w) >> 12) & 0xf)

/* Word 2 */
#define PTD_TOTAL_BYTES(w)	((w) & 0x3ff)
#define PTD_DIR(w)		(((w) >> 10) & 3)
#define PTD_DIR_SETUP		0
#define PTD_DIR_OUT		1
#define PTD_DIR_IN		2

/* Word 3 */
#define PTD_ADDRESS(w)		((w) & 0x7f)

/* Completion codes, as the data sheet numbers them. */
#define PTD_CC_NO_ERROR		0x0
#define PTD_CC_STALL		0x4
#define PTD_CC_NO_RESPONSE	0x5
#define PTD_CC_NOT_ACCESSED	0xf

/* Root hub port status, which follows OHCI. */
#define RH_CONNECTED		(1u << 0)
#define RH_ENABLED		(1u << 1)
#define RH_SUSPENDED		(1u << 2)
#define RH_RESET		(1u << 4)
#define RH_POWERED		(1u << 8)
#define RH_LOW_SPEED		(1u << 9)
#define RH_CONNECT_CHANGE	(1u << 16)
#define RH_ENABLE_CHANGE	(1u << 17)
#define RH_SUSPEND_CHANGE	(1u << 18)
#define RH_RESET_CHANGE		(1u << 20)

/* HcInterruptStatus, the OHCI side. Only the root hub bit is reported. */
#define HcInt_RootHubStatusChange	(1u << 6)

/*
 * What the chip identifies itself as. A driver is entitled to check this and
 * ROOL's does, insisting on &61 in the top byte before it will start, so it is
 * the single most useful thing to get right first.
 */
#define CHIP_ID			0x6110

/* The magic a driver writes to HcSoftwareReset to reset the chip. */
#define SOFTWARE_RESET_MAGIC	0xf6

/* Buffer RAM on the chip, shared between the isochronous and ATL lists. */
#define BUFFER_RAM_SIZE		0x1000

/* Two downstream ports, which is what the chip provides. */
#define ROOT_HUB_PORTS		2

/* --- State -------------------------------------------------------------- */

/** A latched command: which register, which direction, how far through. */
typedef struct {
	uint8_t reg;		/* register number, without the direction bit */
	int writing;		/* the command asked to write */
	unsigned phase;		/* halfwords moved since the command */
} PortCommand;

typedef struct {
	int present;		/* the card carries a controller at all */

	PortCommand host;	/* the host controller's command/data pair */
	PortCommand device;	/* the device controller's pair, unimplemented */

	/* OHCI-derived registers, 32-bit. */
	uint32_t control;
	uint32_t command_status;
	uint32_t interrupt_status;
	uint32_t interrupt_enable;
	uint32_t fm_interval;
	uint32_t fm_remaining;
	uint32_t fm_number;
	uint32_t ls_threshold;
	uint32_t rh_descriptor_a;
	uint32_t rh_descriptor_b;
	uint32_t rh_status;
	uint32_t rh_port_status[ROOT_HUB_PORTS];

	/* The chip's own registers, 16-bit. */
	uint16_t hardware_configuration;
	uint16_t dma_configuration;
	uint16_t transfer_counter;
	uint16_t up_interrupt;
	uint16_t up_interrupt_enable;
	uint16_t scratch;
	uint16_t itl_buffer_length;
	uint16_t atl_buffer_length;
	uint16_t buffer_status;

	/*
	 * Buffer memory, and where the open buffer port has reached. The ATL lives
	 * above the two ITL halves, which is how the length registers divide it.
	 */
	uint8_t buffer[BUFFER_RAM_SIZE];
	uint32_t buffer_pos;		/* byte offset reached by the open port */
	uint32_t buffer_base;		/* where the open port started */
	uint32_t buffer_limit;		/* one past the last byte it may touch */
	uint32_t buffer_remaining;	/* bytes left of HcTransferCounter */
	int buffer_is_atl;		/* the open port is the ATL, not the ITL */

	/* The low half of a 32-bit register write, held until its partner comes. */
	uint32_t pending_write_low;

	/* What is plugged into each root hub port, or NULL. */
	UsbDevice *port_device[ROOT_HUB_PORTS];

	/*
	 * And what it was asked to be, so that applying the configuration again
	 * can tell "the same thing is still there" from "a different device is
	 * wanted now". The device itself cannot answer that: two host devices
	 * look alike from the outside, and re-plugging one the guest is using
	 * would be a disconnection it did not deserve.
	 */
	int port_kind[ROOT_HUB_PORTS];
	char port_id[ROOT_HUB_PORTS][16];

	/*
	 * The control transfer in progress. A control transfer arrives as several
	 * descriptors - the request, then data, then status - so the request has to
	 * outlive the descriptor that carried it.
	 */
	UsbSetup setup;
	int setup_valid;
	unsigned setup_length;		/* answer taken so far, or data sent so far */
	uint8_t control_data[1024];
	unsigned control_length;	/* how much answer there is */
	int control_done;		/* an outgoing request has been carried out */

	int irq_asserted;
	void (*irq_change)(int level);
} Isp1161;

static Isp1161 usb;

static void usb_run_atl(uint32_t base, uint32_t bytes);

/* --- Interrupts --------------------------------------------------------- */

/**
 * Follow the interrupt line to whatever is now pending and enabled.
 *
 * The card's line is not a separate thing a driver has to clear: acknowledging
 * in HcuPInterrupt is what releases it, so every path that changes those two
 * registers ends up here.
 */
static void
usb_isp1161_update_irq(void)
{
	const int level = (usb.up_interrupt & usb.up_interrupt_enable & uPInt_All) != 0;

	if (level != usb.irq_asserted) {
		usb.irq_asserted = level;
		if (usb.irq_change != NULL) {
			usb.irq_change(level);
		}
	}
}

/** Raise one or more HcuPInterrupt sources. */
static void
usb_isp1161_raise(uint16_t sources)
{
	usb.up_interrupt |= sources;
	usb_isp1161_update_irq();
}

/* --- Reset -------------------------------------------------------------- */

void
usb_isp1161_reset(void)
{
	void (*irq_change)(int level) = usb.irq_change;
	const int present = usb.present;
	UsbDevice *attached[ROOT_HUB_PORTS];
	int port;

	/* A controller reset is not an unplug: whatever is in the ports is still
	   physically there, and comes back connected but unaddressed. A machine
	   reset goes through usb_isp1161_init(), which starts from nothing and lets
	   the configuration say what is plugged in. */
	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		attached[port] = usb.port_device[port];
	}

	memset(&usb, 0, sizeof(usb));
	usb.irq_change = irq_change;
	usb.present = present;

	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		usb.port_device[port] = attached[port];
		if (attached[port] != NULL) {
			attached[port]->address = 0;
			usb.rh_port_status[port] = RH_CONNECTED | RH_CONNECT_CHANGE |
			    (attached[port]->low_speed ? RH_LOW_SPEED : 0);
		}
	}

	/*
	 * Power-on values. HcFmInterval is the number of 12MHz bit times in a
	 * millisecond frame, less one, which is the 11999 every OHCI controller
	 * starts at; a driver that never writes it still gets 1ms frames.
	 */
	usb.fm_interval = 11999;
	usb.fm_remaining = 11999;
	usb.ls_threshold = 0x0628;

	/*
	 * Two downstream ports, each individually powered and individually
	 * over-current protected, and no port is a compound device. Bits 0 to 7 of
	 * HcRhDescriptorA are the port count.
	 */
	usb.rh_descriptor_a = ROOT_HUB_PORTS | (1u << 8) | (1u << 11);
	usb.rh_descriptor_b = 0;

	/* Nothing is plugged in yet, so every port reads as disconnected. */

	/*
	 * The clock comes ready shortly after a reset and the chip says so. A
	 * driver may wait for it, so report it from the start rather than making
	 * it wait for something that will never arrive.
	 */
	usb.up_interrupt = uPInt_ClkReady;

	usb.itl_buffer_length = 0;
	usb.atl_buffer_length = 0;

	usb.irq_asserted = 0;
	if (irq_change != NULL) {
		irq_change(0);
	}
}

void
usb_isp1161_init(void (*irq_change)(int level))
{
	int port;

	/* A new machine starts with empty ports, and anything the last one had
	   plugged in has to be given back to the host rather than merely dropped. */
	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		usb_host_destroy(usb.port_device[port]);
		usb.port_device[port] = NULL;
	}

	memset(&usb, 0, sizeof(usb));
	usb.irq_change = irq_change;
	usb.present = 1;
	usb_isp1161_reset();

	rpclog("USB: ISP1161 host controller at EASI +0x%06x, chip id 0x%04x, "
	       "%d root hub port(s)\n", USB_ISP1161_EASI_OFFSET, CHIP_ID,
	       ROOT_HUB_PORTS);

	usb_isp1161_apply_config();
}

int
usb_isp1161_enabled(void)
{
	return usb.present;
}

/* --- The root hub -------------------------------------------------------- */

/**
 * Tell the driver something about a port has changed, or stop saying so.
 *
 * A root hub reports through OHCI's HcInterruptStatus, and the ISP1161 passes
 * that up as its own OPR_Reg source, so both have to move together or a driver
 * sees an interrupt it cannot account for.
 */
static void
usb_isp1161_root_hub_changed(void)
{
	int port, changed = 0;

	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		if (usb.rh_port_status[port] & 0xffff0000u) {
			changed = 1;
		}
	}

	if (changed) {
		usb.interrupt_status |= HcInt_RootHubStatusChange;
		usb_isp1161_raise(uPInt_OPR_Reg);
	} else {
		usb.interrupt_status &= ~HcInt_RootHubStatusChange;
		usb.up_interrupt &= (uint16_t) ~uPInt_OPR_Reg;
		usb_isp1161_update_irq();
	}
}

int
usb_isp1161_attach(int port, UsbDevice *dev)
{
	if (!usb.present || port < 0 || port >= ROOT_HUB_PORTS || dev == NULL) {
		return -1;
	}
	if (usb.port_device[port] != NULL) {
		return -1;		/* something is already in that port */
	}

	usb.port_device[port] = dev;
	dev->address = 0;

	usb.rh_port_status[port] |= RH_CONNECTED | RH_CONNECT_CHANGE;
	if (dev->low_speed) {
		usb.rh_port_status[port] |= RH_LOW_SPEED;
	}
	usb_isp1161_root_hub_changed();

	rpclog("USB: %s attached to port %d\n",
	       dev->ops->name != NULL ? dev->ops->name : "device", port + 1);

	return 0;
}

UsbDevice *
usb_isp1161_detach(int port)
{
	UsbDevice *dev;

	if (!usb.present || port < 0 || port >= ROOT_HUB_PORTS) {
		return NULL;
	}

	dev = usb.port_device[port];
	usb.port_device[port] = NULL;

	if (dev != NULL) {
		/* Unplugging drops the connection and disables the port, and the
		   change bits are what tell the driver to go and look. */
		usb.rh_port_status[port] &= ~(RH_CONNECTED | RH_ENABLED | RH_LOW_SPEED);
		usb.rh_port_status[port] |= RH_CONNECT_CHANGE | RH_ENABLE_CHANGE;
		usb_isp1161_root_hub_changed();

		rpclog("USB: %s detached from port %d\n",
		       dev->ops->name != NULL ? dev->ops->name : "device", port + 1);
	}

	return dev;
}

UsbDevice *
usb_isp1161_device_in_port(int port)
{
	if (!usb.present || port < 0 || port >= ROOT_HUB_PORTS) {
		return NULL;
	}

	return usb.port_device[port];
}

/**
 * Plug in whatever this machine's configuration asks for.
 *
 * Called at reset, and again when the interface changes an attachment, so the
 * configuration is the single account of what is plugged in and the dialogue is
 * only a way of editing it.
 */
/** Let go of whatever was in a port, and give it back to the host. */
static void
usb_isp1161_release(int port)
{
	usb_host_destroy(usb_isp1161_detach(port));

	usb.port_kind[port] = UsbAttachment_None;
	usb.port_id[port][0] = '\0';
}

void
usb_isp1161_shutdown(void)
{
	int port;

	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		usb_isp1161_release(port);
	}
}

void
usb_isp1161_apply_config(void)
{
	int port;

	if (!usb.present) {
		return;
	}

	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		const int wanted = (port < 2) ? config.usb_port[port]
		                              : UsbAttachment_None;
		const char *wanted_id = (port < 2) ? config.usb_host[port] : "";
		UsbDevice *dev = NULL;

		/* Already exactly this? Then leave it alone: re-plugging a device
		   the guest is using would look to it like someone pulling the
		   lead out and putting it back. */
		if (usb.port_kind[port] == wanted &&
		    strcmp(usb.port_id[port], wanted_id) == 0) {
			continue;
		}

		usb_isp1161_release(port);

		if (wanted == UsbAttachment_Host) {
			unsigned vendor, product;
			const char *why = NULL;

			if (sscanf(wanted_id, "%4x:%4x", &vendor, &product) != 2) {
				rpclog("USB: port %d asks for host device '%s', which is not "
				       "an identifier\n", port + 1, wanted_id);
				continue;
			}

			dev = usb_host_create((uint16_t) vendor, (uint16_t) product, &why);
			if (dev == NULL) {
				/* Left empty rather than substituted with something else:
				   the guest is better told nothing is there than given a
				   device that is not the one asked for. */
				rpclog("USB: port %d could not take %s: %s\n", port + 1,
				       wanted_id, (why != NULL && *why != '\0') ? why
				                                                : "no reason given");
				continue;
			}
		}

		if (dev == NULL) {
			continue;
		}

		if (usb_isp1161_attach(port, dev) != 0) {
			usb_host_destroy(dev);
			continue;
		}

		usb.port_kind[port] = wanted;
		snprintf(usb.port_id[port], sizeof(usb.port_id[port]), "%s", wanted_id);
	}
}

/* --- Buffer ports ------------------------------------------------------- */

/**
 * Open a buffer port for streaming.
 *
 * The ATL sits above both ITL halves, so where it starts depends on what the
 * driver gave the isochronous list. A transfer that would run past the end of
 * buffer memory is clamped rather than allowed to wrap into the other list,
 * because a wrap would corrupt descriptors belonging to somebody else and be
 * very hard to see from the guest.
 */
static void
usb_isp1161_open_buffer(int atl)
{
	const uint32_t itl_total = (uint32_t) usb.itl_buffer_length * 2u;

	if (atl) {
		usb.buffer_base = (itl_total < BUFFER_RAM_SIZE) ? itl_total : 0;
		usb.buffer_limit = usb.buffer_base + usb.atl_buffer_length;
	} else {
		usb.buffer_base = 0;
		usb.buffer_limit = itl_total;
	}

	if (usb.buffer_limit > BUFFER_RAM_SIZE) {
		usb.buffer_limit = BUFFER_RAM_SIZE;
	}
	if (usb.buffer_base > usb.buffer_limit) {
		usb.buffer_base = usb.buffer_limit;
	}

	usb.buffer_pos = usb.buffer_base;
	usb.buffer_remaining = usb.transfer_counter;
	usb.buffer_is_atl = atl;
}

/** Move the next halfword out of the open buffer port. */
static uint16_t
usb_isp1161_buffer_read(void)
{
	uint16_t val = 0;

	if (usb.buffer_remaining >= 2 && usb.buffer_pos + 1 < usb.buffer_limit) {
		val = (uint16_t) (usb.buffer[usb.buffer_pos] |
		                  (usb.buffer[usb.buffer_pos + 1] << 8));
		usb.buffer_pos += 2;
		usb.buffer_remaining -= 2;
	}

	return val;
}

/** Move the next halfword into the open buffer port. */
static void
usb_isp1161_buffer_write(uint16_t val)
{
	if (usb.buffer_remaining >= 2 && usb.buffer_pos + 1 < usb.buffer_limit) {
		usb.buffer[usb.buffer_pos] = (uint8_t) val;
		usb.buffer[usb.buffer_pos + 1] = (uint8_t) (val >> 8);
		usb.buffer_pos += 2;
		usb.buffer_remaining -= 2;

		if (usb.buffer_remaining == 0 && usb.buffer_is_atl) {
			/* The driver has finished handing over a list, so run it. */
			usb_run_atl(usb.buffer_base, usb.buffer_pos - usb.buffer_base);
		}
	}
}

/* --- The transfer engine ------------------------------------------------ */

/** Read one halfword out of buffer memory. */
static uint16_t
usb_buffer_get16(uint32_t at)
{
	if (at + 1 >= BUFFER_RAM_SIZE) {
		return 0;
	}

	return (uint16_t) (usb.buffer[at] | (usb.buffer[at + 1] << 8));
}

/** Write one halfword into buffer memory. */
static void
usb_buffer_put16(uint32_t at, uint16_t val)
{
	if (at + 1 >= BUFFER_RAM_SIZE) {
		return;
	}

	usb.buffer[at] = (uint8_t) val;
	usb.buffer[at + 1] = (uint8_t) (val >> 8);
}

/** The device answering to an address, or NULL if nothing does. */
static UsbDevice *
usb_device_at_address(unsigned address)
{
	int port;

	for (port = 0; port < ROOT_HUB_PORTS; port++) {
		UsbDevice *dev = usb.port_device[port];

		/*
		 * Only an enabled port takes part. A device that is plugged in but
		 * whose port the driver has not enabled must not answer, or a driver
		 * would appear to work without ever resetting the port and would then
		 * fail on real hardware.
		 */
		if (dev != NULL && (usb.rh_port_status[port] & RH_ENABLED) &&
		    dev->address == address) {
			return dev;
		}
	}

	return NULL;
}

/**
 * Run one transfer descriptor.
 *
 * @param at Offset of the PTD header in buffer memory.
 * @return Bytes the whole descriptor occupies, header and payload, so the caller
 *         can step to the next one. Zero if the list should stop here.
 */
static uint32_t
usb_run_ptd(uint32_t at)
{
	uint16_t w0 = usb_buffer_get16(at);
	const uint16_t w1 = usb_buffer_get16(at + 2);
	const uint16_t w2 = usb_buffer_get16(at + 4);
	const uint16_t w3 = usb_buffer_get16(at + 6);

	const unsigned total = PTD_TOTAL_BYTES(w2);
	const unsigned endpoint = PTD_ENDPOINT(w1);
	const unsigned dir = PTD_DIR(w2);
	const unsigned address = PTD_ADDRESS(w3);
	const uint32_t payload = at + PTD_HEADER_SIZE;

	/* The payload is padded to a whole number of words. */
	const uint32_t occupies = PTD_HEADER_SIZE + ((total + 3u) & ~3u);

	UsbDevice *dev;
	UsbResult result = USB_NO_DEVICE;
	unsigned actual = 0;
	unsigned cc;

	if (!(w0 & PTD_ACTIVE)) {
		/* Already dealt with, or the driver has parked it. Skip, do not stop:
		   a list may legitimately hold a mixture. */
		return occupies;
	}

	dev = usb_device_at_address(address);

	if (dev == NULL) {
		/*
		 * Address 0 before SET_ADDRESS is the exception: a freshly reset device
		 * answers on 0, and that is how it gets enumerated at all.
		 */
		usb_buffer_put16(at, (uint16_t) ((w0 & ~PTD_ACTIVE) |
		    (PTD_CC_NO_RESPONSE << PTD_CC_SHIFT)));
		return occupies;
	}

	switch (dir) {
	case PTD_DIR_SETUP:
		if (endpoint == 0 && total >= 8) {
			UsbSetup setup;

			setup.request_type = usb.buffer[payload];
			setup.request = usb.buffer[payload + 1];
			setup.value = usb_buffer_get16(payload + 2);
			setup.index = usb_buffer_get16(payload + 4);
			setup.length = usb_buffer_get16(payload + 6);

			/*
			 * SET_ADDRESS is the controller's business, not the device's: the
			 * address is how transactions are routed, so it is recorded here
			 * and the device never sees it. Everything else is the device's.
			 */
			if ((setup.request_type & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
			    setup.request == USB_REQ_SET_ADDRESS) {
				dev->address = (uint8_t) (setup.value & 0x7f);
				usb.setup_length = 0;
				result = USB_ACK;
			} else {
				/* Remember it: the data stage arrives as its own PTD. */
				usb.setup = setup;
				usb.setup_valid = 1;
				usb.setup_length = 0;
				usb.control_length = 0;
				usb.control_done = 0;
				result = USB_ACK;
			}
			actual = total;
		} else {
			result = USB_STALL;
		}
		break;

	case PTD_DIR_IN:
		if (endpoint == 0) {
			/*
			 * The data stage of a control transfer. The device produced its
			 * answer when the request arrived; a control transfer can be read
			 * in several packets, so what has already been taken is tracked
			 * and this hands over the next slice.
			 */
			if (!usb.setup_valid) {
				result = USB_STALL;
				break;
			}
			if (!(usb.setup.request_type & USB_DIR_IN)) {
				/*
				 * Not a data stage at all: this is the status stage of a
				 * request that went the other way. It carries no data, but
				 * it is where a request with nothing to send - SET_CONFIGURATION
				 * and its like - actually reaches the device.
				 */
				if (!usb.control_done) {
					unsigned len = usb.setup_length;

					result = dev->ops->control(dev, &usb.setup,
					    usb.control_data, &len);
					if (result != USB_ACK) {
						break;
					}
					usb.control_done = 1;
				}
				actual = 0;
				result = USB_ACK;
				break;
			}
			if (usb.setup_length == 0) {
				unsigned len = sizeof(usb.control_data);

				result = dev->ops->control(dev, &usb.setup,
				    usb.control_data, &len);
				usb.control_length = (result == USB_ACK) ? len : 0;
				if (result != USB_ACK) {
					break;
				}
			}
			actual = usb.control_length - usb.setup_length;
			if (actual > total) {
				actual = total;
			}
			if (actual > 0) {
				memcpy(&usb.buffer[payload],
				    &usb.control_data[usb.setup_length], actual);
			}
			usb.setup_length += actual;
			result = USB_ACK;
		} else {
			unsigned len = total;

			if (dev->ops->read == NULL) {
				result = USB_STALL;
				break;
			}
			result = dev->ops->read(dev, endpoint, &usb.buffer[payload], &len);
			actual = (result == USB_ACK) ? len : 0;
		}
		break;

	case PTD_DIR_OUT:
		if (endpoint == 0) {
			if (usb.setup_valid && !(usb.setup.request_type & USB_DIR_IN) &&
			    usb.setup.length > 0 && !usb.control_done) {
				/*
				 * Data going out with a request. It is gathered rather than
				 * handed over a packet at a time, because a device is given a
				 * request and its data together; only once all of it has
				 * arrived is there anything to carry out.
				 */
				if (usb.setup_length < usb.setup.length) {
					unsigned room = sizeof(usb.control_data) - usb.setup_length;
					unsigned take = (total < room) ? total : room;

					if (take > 0) {
						memcpy(&usb.control_data[usb.setup_length],
						    &usb.buffer[payload], take);
					}
					usb.setup_length += take;
				}
				/* A device that answers NAK is asked again with the same
				   descriptor, so what has already been gathered must not be
				   gathered a second time. */
				actual = total;
				result = USB_ACK;

				if (usb.setup_length >= usb.setup.length) {
					unsigned len = usb.setup_length;

					result = dev->ops->control(dev, &usb.setup,
					    usb.control_data, &len);
					if (result == USB_ACK) {
						usb.control_done = 1;
					} else {
						actual = 0;
					}
				}
			} else {
				/* The status stage of a read: the driver has the answer, so
				   the transfer is over. */
				result = USB_ACK;
				actual = total;
				usb.setup_valid = 0;
			}
		} else if (dev->ops->write != NULL) {
			result = dev->ops->write(dev, endpoint, &usb.buffer[payload],
			    total);
			actual = (result == USB_ACK) ? total : 0;
		} else {
			result = USB_STALL;
		}
		break;

	default:
		result = USB_STALL;
		break;
	}

	switch (result) {
	case USB_ACK:
		cc = PTD_CC_NO_ERROR;
		break;
	case USB_STALL:
		cc = PTD_CC_STALL;
		break;
	case USB_NAK:
		/*
		 * Nothing new to report. The descriptor stays active and keeps its
		 * NotAccessed code, so the driver leaves it in the list and asks
		 * again, which is what a real interrupt endpoint does when idle.
		 */
		usb_buffer_put16(at, (uint16_t) ((w0 & ~0xfc00u) | PTD_ACTIVE |
		    (w0 & PTD_TOGGLE) | (PTD_CC_NOT_ACCESSED << PTD_CC_SHIFT)));
		return occupies;
	default:
		cc = PTD_CC_NO_RESPONSE;
		break;
	}

	/* Report what happened: bytes moved, how it ended, and no longer active. */
	w0 = (uint16_t) ((actual & 0x3ff) | (w0 & PTD_TOGGLE) |
	    (cc << PTD_CC_SHIFT));
	usb_buffer_put16(at, w0);

	return occupies;
}

/**
 * Run the whole ATL, then tell the driver it has been dealt with.
 *
 * The list ends at whichever comes first: a descriptor marked Last, the end of
 * what the driver handed over, or the end of the buffer. Trusting only the Last
 * bit would leave a runaway loop one bad byte away.
 */
static void
usb_run_atl(uint32_t base, uint32_t bytes)
{
	uint32_t at = base;
	const uint32_t end = base + bytes;
	int descriptors = 0;

	while (at + PTD_HEADER_SIZE <= end && at + PTD_HEADER_SIZE <= BUFFER_RAM_SIZE) {
		const uint16_t w1 = usb_buffer_get16(at + 2);
		const uint32_t occupies = usb_run_ptd(at);

		descriptors++;

		if (occupies == 0 || (w1 & PTD_LAST)) {
			break;
		}
		at += occupies;

		if (descriptors > 64) {
			/* More descriptors than the buffer can hold at their smallest.
			   Something is wrong with the list; stop rather than spin. */
			rpclog("USB: giving up on an ATL of more than 64 descriptors\n");
			break;
		}
	}

	usb_isp1161_raise(uPInt_ATL | uPInt_AllEOT);
}

/* --- The register file -------------------------------------------------- */

/** Read a 32-bit register. */
static uint32_t
usb_isp1161_read_register32(uint8_t reg)
{
	switch (reg) {
	case HcRevision:
		return 0x00000110;	/* OHCI 1.1 */
	case HcControl:
		return usb.control;
	case HcCommandStatus:
		return usb.command_status;
	case HcInterruptStatus:
		return usb.interrupt_status;
	case HcInterruptEnable:
	case HcInterruptDisable:
		return usb.interrupt_enable;
	case HcFmInterval:
		return usb.fm_interval;
	case HcFmRemaining:
		return usb.fm_remaining;
	case HcFmNumber:
		return usb.fm_number;
	case HcLSThreshold:
		return usb.ls_threshold;
	case HcRhDescriptorA:
		return usb.rh_descriptor_a;
	case HcRhDescriptorB:
		return usb.rh_descriptor_b;
	case HcRhStatus:
		return usb.rh_status;
	case HcRhPortStatus1:
		return usb.rh_port_status[0];
	case HcRhPortStatus2:
		return usb.rh_port_status[1];
	default:
		return 0;
	}
}

/** Write a 32-bit register. */
static void
usb_isp1161_write_register32(uint8_t reg, uint32_t val)
{
	switch (reg) {
	case HcControl:
		usb.control = val;
		break;

	case HcCommandStatus:
		/*
		 * HostControllerReset is self-clearing: the driver sets bit 0 and the
		 * controller puts itself back to its power-on state and clears it. A
		 * driver that polled for it to clear would spin forever otherwise.
		 */
		if (val & 1u) {
			usb_isp1161_reset();
		} else {
			usb.command_status = val & ~1u;
		}
		break;

	case HcInterruptStatus:
		usb.interrupt_status &= ~val;	/* write to clear */
		break;

	case HcInterruptEnable:
		usb.interrupt_enable |= val;
		break;

	case HcInterruptDisable:
		usb.interrupt_enable &= ~val;
		break;

	case HcFmInterval:
		usb.fm_interval = val;
		break;

	case HcLSThreshold:
		usb.ls_threshold = val;
		break;

	case HcRhDescriptorA:
		/* The port count is fixed by the hardware; the rest is the driver's. */
		usb.rh_descriptor_a = (val & ~0xffu) | ROOT_HUB_PORTS;
		break;

	case HcRhDescriptorB:
		usb.rh_descriptor_b = val;
		break;

	case HcRhStatus:
		usb.rh_status = val;
		break;

	case HcRhPortStatus1:
	case HcRhPortStatus2:
		/*
		 * Port status writes are requests, not values: each bit asks for
		 * something to happen, and the bits in the top half acknowledge a
		 * change the driver has seen. Writing the status back verbatim must
		 * therefore not be treated as setting it.
		 */
		{
			const int port = (reg == HcRhPortStatus1) ? 0 : 1;
			uint32_t status = usb.rh_port_status[port];
			const int connected = (status & RH_CONNECTED) != 0;

			if (val & (1u << 0)) {		/* ClearPortEnable */
				if (status & RH_ENABLED) {
					status &= ~RH_ENABLED;
					status |= RH_ENABLE_CHANGE;
				}
			}
			if ((val & (1u << 1)) && connected) {	/* SetPortEnable */
				status |= RH_ENABLED;
			}
			if ((val & (1u << 2)) && connected) {	/* SetPortSuspend */
				status |= RH_SUSPENDED;
			}
			if (val & (1u << 3)) {		/* ClearSuspendStatus */
				if (status & RH_SUSPENDED) {
					status &= ~RH_SUSPENDED;
					status |= RH_SUSPEND_CHANGE;
				}
			}
			if ((val & (1u << 4)) && connected) {	/* SetPortReset */
				/*
				 * A reset finishes far faster than a driver can look, so
				 * there is no point modelling the 10ms: the port comes back
				 * reset and enabled, with the change bit set, which is the
				 * state a driver is waiting to see. The device goes back to
				 * address 0, which is how enumeration starts.
				 */
				UsbDevice *dev = usb.port_device[port];

				status &= ~RH_RESET;
				status |= RH_ENABLED | RH_RESET_CHANGE;

				if (dev != NULL) {
					dev->address = 0;
					if (dev->ops->reset != NULL) {
						dev->ops->reset(dev);
					}
				}
				usb.setup_valid = 0;
			}
			if (val & (1u << 8)) {		/* SetPortPower */
				status |= RH_POWERED;
			}
			if (val & (1u << 9)) {		/* ClearPortPower */
				status &= ~RH_POWERED;
			}

			/* Acknowledgements, in the top half. */
			status &= ~(val & 0xffff0000u);

			usb.rh_port_status[port] = status;

			if (val & 0xffff0000u) {
				/* A change the driver has now seen may be the last one
				   outstanding, so re-evaluate the root hub interrupt. */
				usb_isp1161_root_hub_changed();
			}
		}
		break;

	default:
		break;
	}
}

/** Read a 16-bit register. */
static uint16_t
usb_isp1161_read_register16(uint8_t reg)
{
	switch (reg) {
	case HcHardwareConfiguration:
		return usb.hardware_configuration;
	case HcDMAConfiguration:
		return usb.dma_configuration;
	case HcTransferCounter:
		return usb.transfer_counter;
	case HcuPInterrupt:
		return usb.up_interrupt;
	case HcuPInterruptEnable:
		return usb.up_interrupt_enable;
	case HcChipID:
		return CHIP_ID;
	case HcScratch:
		return usb.scratch;
	case HcITLBufferLength:
		return usb.itl_buffer_length;
	case HcATLBufferLength:
		return usb.atl_buffer_length;
	case HcBufferStatus:
		return usb.buffer_status;
	case HcReadBackITL0Length:
	case HcReadBackITL1Length:
		return usb.itl_buffer_length;
	default:
		return 0;
	}
}

/** Write a 16-bit register. */
static void
usb_isp1161_write_register16(uint8_t reg, uint16_t val)
{
	switch (reg) {
	case HcHardwareConfiguration:
		usb.hardware_configuration = val;
		break;

	case HcDMAConfiguration:
		usb.dma_configuration = val;
		break;

	case HcTransferCounter:
		usb.transfer_counter = val;
		break;

	case HcuPInterrupt:
		usb.up_interrupt &= (uint16_t) ~val;	/* write to clear */
		usb_isp1161_update_irq();
		break;

	case HcuPInterruptEnable:
		usb.up_interrupt_enable = val;
		usb_isp1161_update_irq();
		break;

	case HcScratch:
		/* Does nothing but remember, which is exactly what it is for: it is
		   how a driver proves it can reach the chip at all. */
		usb.scratch = val;
		break;

	case HcSoftwareReset:
		if (val == SOFTWARE_RESET_MAGIC) {
			usb_isp1161_reset();
		}
		break;

	case HcITLBufferLength:
		usb.itl_buffer_length = (val > BUFFER_RAM_SIZE / 2)
		    ? BUFFER_RAM_SIZE / 2 : val;
		break;

	case HcATLBufferLength:
		usb.atl_buffer_length = (val > BUFFER_RAM_SIZE)
		    ? BUFFER_RAM_SIZE : val;
		break;

	case HcBufferStatus:
		usb.buffer_status = val;
		break;

	default:
		break;
	}
}

/** True if this register number is one of the 32-bit OHCI-derived ones. */
static int
usb_isp1161_register_is_32bit(uint8_t reg)
{
	return reg <= HcRhPortStatus2;
}

/* --- The ports ---------------------------------------------------------- */

/** Handle a write to a command port. */
static void
usb_isp1161_command(PortCommand *cmd, uint32_t val)
{
	cmd->reg = (uint8_t) (val & COMMAND_REGISTER);
	cmd->writing = (val & COMMAND_WRITE) != 0;
	cmd->phase = 0;

	if (cmd == &usb.host &&
	    (cmd->reg == HcATLBufferPort || cmd->reg == HcITLBufferPort)) {
		usb_isp1161_open_buffer(cmd->reg == HcATLBufferPort);
	}
}

/** Handle a read of the host controller's data port. */
static uint32_t
usb_isp1161_host_data_read(void)
{
	const uint8_t reg = usb.host.reg;

	if (reg == HcATLBufferPort || reg == HcITLBufferPort) {
		return usb_isp1161_buffer_read();
	}

	if (usb_isp1161_register_is_32bit(reg)) {
		const uint32_t val = usb_isp1161_read_register32(reg);
		const unsigned half = usb.host.phase++ & 1u;

		return (half == 0) ? (val & 0xffffu) : (val >> 16);
	}

	usb.host.phase++;

	return usb_isp1161_read_register16(reg);
}

/** Handle a write to the host controller's data port. */
static void
usb_isp1161_host_data_write(uint32_t val)
{
	const uint8_t reg = usb.host.reg;

	if (reg == HcATLBufferPort || reg == HcITLBufferPort) {
		usb_isp1161_buffer_write((uint16_t) val);
		return;
	}

	if (usb_isp1161_register_is_32bit(reg)) {
		/*
		 * Two halves, low first. Hold the low half until its partner arrives
		 * so the register is only ever written whole: a 32-bit register that
		 * saw the low half on its own would briefly hold a value the driver
		 * never asked for, and for HcCommandStatus that would mean resetting
		 * the controller by accident.
		 */
		const unsigned half = usb.host.phase++ & 1u;

		if (half == 0) {
			usb.pending_write_low = val & 0xffffu;
		} else {
			usb_isp1161_write_register32(reg,
			    usb.pending_write_low | ((val & 0xffffu) << 16));
		}
		return;
	}

	usb.host.phase++;
	usb_isp1161_write_register16(reg, (uint16_t) val);
}

uint32_t
usb_isp1161_read(uint32_t offset)
{
	if (!usb.present) {
		return 0xffffffff;
	}

	switch (offset & 0xc) {
	case PORT_HC_DATA:
		return usb_isp1161_host_data_read();

	case PORT_HC_COMMAND:
		/* Reading a command port is not something a driver does; a real chip
		   gives nothing useful back and neither do we. */
		return 0;

	case PORT_DC_DATA:
		/*
		 * The device (peripheral) side of the chip. There is no reason to
		 * pretend it works: a driver asking it for its chip id gets zero,
		 * which reads as "not this chip" and stops it going further.
		 */
		return 0;

	case PORT_DC_COMMAND:
	default:
		return 0;
	}
}

void
usb_isp1161_write(uint32_t offset, uint32_t val)
{
	if (!usb.present) {
		return;
	}

	switch (offset & 0xc) {
	case PORT_HC_DATA:
		usb_isp1161_host_data_write(val);
		break;

	case PORT_HC_COMMAND:
		usb_isp1161_command(&usb.host, val);
		break;

	case PORT_DC_DATA:
		break;

	case PORT_DC_COMMAND:
		usb_isp1161_command(&usb.device, val);
		break;

	default:
		break;
	}
}

/* --- Frames ------------------------------------------------------------- */

void
usb_isp1161_frame(void)
{
	if (!usb.present) {
		return;
	}

	usb.fm_number = (usb.fm_number + 1) & 0xffffu;
	usb.fm_remaining = usb.fm_interval;

	/*
	 * A frame boundary is where a real controller would walk the transfer
	 * lists. Ours are run when the driver hands them over instead, so the only
	 * thing a frame is needed for is the counter a driver watches to see that
	 * the controller is alive - and giving passed-through devices a moment to
	 * notice that a real transfer has come back.
	 */
	usb_host_poll();
}
