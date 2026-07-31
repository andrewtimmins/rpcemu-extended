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
 * usb_ohci.c - an OHCI USB host controller.
 *
 * See usb_ohci.h for why this replaced the ISP1161 that was here.
 *
 * The shape of OHCI is quite different from what came before. The ISP1161 kept
 * its descriptors in its own buffer RAM and the driver pushed them through a
 * data port, so the emulation ran a list when the driver had finished handing
 * one over. OHCI instead keeps everything in main memory - a communication area
 * the driver tells us about once, and linked lists of endpoint and transfer
 * descriptors hanging off it - and the controller is expected to walk them
 * under its own steam, once a frame. So the work happens on the frame tick, and
 * this file reaches into guest memory to do it.
 *
 * That reaching-in is the thing a real Risc PC expansion card could not do, and
 * the reason the ISP1161 was chosen in the first place. It is free here.
 *
 * Everything below the descriptor layer is unchanged: a device is still the
 * three questions in usb_device.h, and a passed-through device from the host
 * still answers them through libusb, none the wiser.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "mem.h"
#include "usb_device.h"
#include "usb_host.h"
#include "usb_ohci.h"

/* --- Registers, as byte offsets in the window --------------------------- */

#define HcRevision		0x00
#define HcControl		0x04
#define HcCommandStatus		0x08
#define HcInterruptStatus	0x0c
#define HcInterruptEnable	0x10
#define HcInterruptDisable	0x14
#define HcHCCA			0x18
#define HcPeriodCurrentED	0x1c
#define HcControlHeadED		0x20
#define HcControlCurrentED	0x24
#define HcBulkHeadED		0x28
#define HcBulkCurrentED		0x2c
#define HcDoneHead		0x30
#define HcFmInterval		0x34
#define HcFmRemaining		0x38
#define HcFmNumber		0x3c
#define HcPeriodicStart		0x40
#define HcLSThreshold		0x44
#define HcRhDescriptorA		0x48
#define HcRhDescriptorB		0x4c
#define HcRhStatus		0x50
#define HcRhPortStatus1		0x54	/* one per port, four bytes apart */

/* HcControl */
#define OHCI_CBSR_MASK		0x00000003
#define OHCI_PLE		0x00000004
#define OHCI_IE			0x00000008
#define OHCI_CLE		0x00000010
#define OHCI_BLE		0x00000020
#define OHCI_HCFS_MASK		0x000000c0
#define OHCI_HCFS_RESET		0x00000000
#define OHCI_HCFS_RESUME	0x00000040
#define OHCI_HCFS_OPERATIONAL	0x00000080
#define OHCI_HCFS_SUSPEND	0x000000c0
#define OHCI_IR			0x00000100

/* HcCommandStatus */
#define OHCI_HCR		0x00000001	/* Host Controller Reset */
#define OHCI_CLF		0x00000002	/* Control List Filled */
#define OHCI_BLF		0x00000004	/* Bulk List Filled */
#define OHCI_OCR		0x00000008	/* Ownership Change Request */

/* HcInterruptStatus / Enable / Disable */
#define OHCI_SO			0x00000001	/* Scheduling Overrun */
#define OHCI_WDH		0x00000002	/* Writeback Done Head */
#define OHCI_SF			0x00000004	/* Start of Frame */
#define OHCI_RD			0x00000008	/* Resume Detected */
#define OHCI_UE			0x00000010	/* Unrecoverable Error */
#define OHCI_FNO		0x00000020	/* Frame Number Overflow */
#define OHCI_RHSC		0x00000040	/* Root Hub Status Change */
#define OHCI_OC			0x40000000	/* Ownership Change */
#define OHCI_MIE		0x80000000	/* Master Interrupt Enable */

#define OHCI_ALL_INTRS		(OHCI_SO | OHCI_WDH | OHCI_SF | OHCI_RD | \
				 OHCI_UE | OHCI_FNO | OHCI_RHSC | OHCI_OC)

/* Endpoint descriptor, four words in guest memory. */
#define ED_FLAGS		0
#define ED_TAILP		4
#define ED_HEADP		8
#define ED_NEXTED		12

#define ED_FA(f)		((f) & 0x7f)		/* function address */
#define ED_EN(f)		(((f) >> 7) & 0xf)	/* endpoint number */
#define ED_DIR_MASK		0x00001800
#define ED_DIR_TD		0x00000000		/* take it from the TD */
#define ED_DIR_OUT		0x00000800
#define ED_DIR_IN		0x00001000
#define ED_SPEED_LOW		0x00002000
#define ED_SKIP			0x00004000
#define ED_FORMAT_ISO		0x00008000
#define ED_MAXP(f)		(((f) >> 16) & 0x7ff)

#define ED_HALTED		0x00000001
#define ED_TOGGLECARRY		0x00000002
#define ED_HEADMASK		0xfffffffc

/* Transfer descriptor, four words in guest memory. */
#define TD_FLAGS		0
#define TD_CBP			4	/* current buffer pointer */
#define TD_NEXTTD		8
#define TD_BE			12	/* buffer end, inclusive */

#define TD_ROUNDING		0x00040000
#define TD_DP_MASK		0x00180000
#define TD_DP_SETUP		0x00000000
#define TD_DP_OUT		0x00080000
#define TD_DP_IN		0x00100000
#define TD_DI(f)		(((f) >> 21) & 7)	/* delay interrupt */
#define TD_TOGGLE_MASK		0x03000000
#define TD_TOGGLE_USE_ED	0x00000000
#define TD_CC_SHIFT		28

/*
 * Isochronous transfer descriptor, eight words in guest memory.
 *
 * A different shape from the general one. Where a general descriptor describes
 * one transfer, this describes up to eight - one per frame, starting at
 * StartingFrame - and carries a halfword for each. That halfword is an offset
 * into the buffer on the way in and a status word on the way out, which is why
 * the specification names it both ways round.
 */
#define ITD_FLAGS		0
#define ITD_BP0			4	/* the buffer's first page */
#define ITD_NEXTTD		8	/* same word as a general descriptor */
#define ITD_BE			12	/* buffer end, inclusive */
#define ITD_PSW			16	/* eight halfwords */

#define ITD_SF(f)		((f) & 0xffff)		/* starting frame */
#define ITD_FC(f)		((((f) >> 24) & 7) + 1)	/* held as count - 1 */
#define ITD_PACKETS		8

/*
 * The offset/status halfword.
 *
 * Going in it is a thirteen-bit offset, whose top bit chooses which of the two
 * pages the packet starts in, and the driver leaves the remaining bits set so
 * that a packet the controller never reached reads back as NotAccessed. Coming
 * out it is a twelve-bit length and a condition code.
 */
#define PSW_OFFSET_MASK		0x1fff
#define PSW_PAGE_SELECT		0x1000
#define PSW_LENGTH_MASK		0x0fff
#define PSW_CC_SHIFT		12

/* Condition codes, as OHCI numbers them. */
#define CC_NO_ERROR		0x0
#define CC_STALL		0x4
#define CC_DEVICE_NOT_RESPONDING 0x5
#define CC_DATA_OVERRUN		0x8
#define CC_DATA_UNDERRUN	0x9
#define CC_NOT_ACCESSED		0xe

/**
 * Both spellings of NotAccessed.
 *
 * OHCI gives it two values, 0xe and 0xf, and a driver tests the code against
 * this mask rather than for either one. The shipped OHCIDriver does exactly
 * that, so a packet must be left with one of the two and not with anything else
 * that happens to mean nothing was moved.
 */
#define CC_NOT_ACCESSED_MASK	0xe

/* Root hub port status, which is where OHCI's names came from. */
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

/* HcRhStatus, whose write side is requests rather than values. */
#define RHS_LPS			(1u << 0)	/* clear global power */
#define RHS_OCI			(1u << 1)
#define RHS_DRWE		(1u << 15)
#define RHS_LPSC		(1u << 16)	/* set global power */
#define RHS_CRWE		(1u << 31)

/* The HCCA, as offsets into the area the driver gives us. */
#define HCCA_INTERRUPT_TABLE	0x00	/* 32 words */
#define HCCA_FRAME_NUMBER	0x80	/* halfword */
#define HCCA_DONE_HEAD		0x84

/**
 * Most bytes one transfer descriptor will move in one go.
 *
 * An OHCI descriptor has no byte count: the length is the distance from
 * CurrentBufferPointer to BufferEnd, and one descriptor may cover two 4K
 * pages, so 8K is the real limit. This was 1K, inherited from the ISP1161
 * whose descriptors did carry a ten-bit count - and a webcam asks for its
 * configuration descriptor in one go, which here is 1126 bytes, so anything
 * smaller stops enumeration dead at the first interesting device.
 */
#define OHCI_MAX_TRANSFER	8192

/**
 * Most bytes one isochronous packet can carry.
 *
 * A full speed isochronous endpoint may name up to 1023 bytes a frame, which is
 * the most the controller we present should ever be asked for. The allowance is
 * larger than that on purpose: a passed-through device may be a high speed one
 * whose descriptors name a bigger packet, and refusing those outright would turn
 * a device that half works into one that does not work at all. Anything beyond
 * this is reported as an overrun rather than quietly truncated.
 */
#define ITD_PACKET_MAX		4096

/**
 * Frames a root hub port change waits before being reported.
 *
 * USB has a hub debounce a connection for around 100ms, and a frame is 1ms, so
 * this is that. It also keeps a change from being announced while the driver is
 * still starting the controller, which is when there is nothing to hear it.
 */
#define RH_DEBOUNCE_FRAMES	100

/* --- State -------------------------------------------------------------- */

typedef struct {
	int present;

	uint32_t control;
	uint32_t command_status;
	uint32_t interrupt_status;
	uint32_t interrupt_enable;
	uint32_t hcca;
	uint32_t period_current_ed;
	uint32_t control_head_ed;
	uint32_t control_current_ed;
	uint32_t bulk_head_ed;
	uint32_t bulk_current_ed;
	uint32_t done_head;
	uint32_t fm_interval;
	uint32_t fm_remaining;
	uint32_t fm_number;
	uint32_t periodic_start;
	uint32_t ls_threshold;
	uint32_t rh_descriptor_a;
	uint32_t rh_descriptor_b;
	uint32_t rh_status;
	uint32_t rh_port_status[USB_OHCI_PORTS];

	/* What is plugged into each port, and what it was asked to be. */
	UsbDevice *port_device[USB_OHCI_PORTS];
	int port_kind[USB_OHCI_PORTS];
	char port_id[USB_OHCI_PORTS][16];

	/*
	 * The control transfer in progress. A control transfer arrives as
	 * several descriptors - the request, then data, then status - so the
	 * request has to outlive the descriptor that carried it.
	 */
	UsbSetup setup;
	int setup_valid;
	unsigned setup_taken;		/* how much of the answer has gone */

	/*
	 * An address the device has been told to take but has not taken yet.
	 *
	 * USB has a device keep answering to address 0 until the status stage of
	 * SET_ADDRESS has finished, and that status stage is itself addressed to
	 * 0. Move the device the moment the request arrives and the status stage
	 * has nobody to talk to, which reads as a device that stopped responding
	 * half way through being enumerated.
	 */
	int address_pending;
	uint8_t pending_address;
	uint8_t control_data[OHCI_MAX_TRANSFER];
	unsigned control_length;
	int control_done;

	/* The done queue built during a frame, newest first as OHCI wants. */
	uint32_t done_queue;
	int done_pending;

	/*
	 * Frames left before the root hub owns up to a change on one of its
	 * ports. USB has a hub debounce a connection for about 100ms before
	 * reporting it, and modelling that is not politeness: a change reported
	 * the instant the controller is reset arrives before the driver has a
	 * hub attached to hear it. OHCIDriver answers an unusable one by turning
	 * the root hub status change interrupt off and arranging to turn it back
	 * on from a timeout - so getting in too early costs the connection
	 * outright if that timeout does not come.
	 */
	int rh_debounce;

	int irq_asserted;
	void (*irq_change)(int level);
} Ohci;

static Ohci ohci;

/* --- Interrupts --------------------------------------------------------- */

static void
usb_ohci_update_irq(void)
{
	const int level = (ohci.interrupt_enable & OHCI_MIE) != 0 &&
	                  (ohci.interrupt_status & ohci.interrupt_enable &
	                   OHCI_ALL_INTRS) != 0;

	if (level != ohci.irq_asserted) {
		ohci.irq_asserted = level;
		if (ohci.irq_change != NULL) {
			ohci.irq_change(level);
		}
	}
}

static void
usb_ohci_raise(uint32_t sources)
{
	ohci.interrupt_status |= sources;
	usb_ohci_update_irq();
}

/* --- Guest memory ------------------------------------------------------- */

/*
 * Descriptors live in guest RAM and the controller reads and writes them
 * directly. Everything here is word-aligned by the specification, so there is
 * no unaligned case to worry about.
 */

static uint32_t
ohci_read32(uint32_t addr)
{
	return mem_phys_read32(addr);
}

static void
ohci_write32(uint32_t addr, uint32_t val)
{
	mem_phys_write32(addr, val);
}

/** Read a run of bytes out of guest memory, which need not be word aligned. */
static void
ohci_read_bytes(uint32_t addr, uint8_t *out, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++) {
		const uint32_t word = mem_phys_read32((addr + i) & ~3u);

		out[i] = (uint8_t) (word >> (((addr + i) & 3) * 8));
	}
}

/** Write a run of bytes into guest memory, preserving neighbours. */
static void
ohci_write_bytes(uint32_t addr, const uint8_t *in, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++) {
		const uint32_t at = (addr + i) & ~3u;
		const unsigned shift = ((addr + i) & 3) * 8;
		uint32_t word = mem_phys_read32(at);

		word &= ~(0xffu << shift);
		word |= (uint32_t) in[i] << shift;
		mem_phys_write32(at, word);
	}
}

/* --- The root hub ------------------------------------------------------- */

static void
usb_ohci_root_hub_changed(void)
{
	int port, changed = 0;

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		if (ohci.rh_port_status[port] & 0xffff0000u) {
			changed = 1;
		}
	}

	if (changed) {
		usb_ohci_raise(OHCI_RHSC);
	} else {
		ohci.interrupt_status &= ~OHCI_RHSC;
		usb_ohci_update_irq();
	}
}

int
usb_ohci_attach(int port, UsbDevice *dev)
{
	if (!ohci.present || port < 0 || port >= USB_OHCI_PORTS || dev == NULL) {
		return -1;
	}
	if (ohci.port_device[port] != NULL) {
		return -1;
	}

	ohci.port_device[port] = dev;
	dev->address = 0;

	ohci.rh_port_status[port] |= RH_CONNECTED | RH_CONNECT_CHANGE;
	if (dev->low_speed) {
		ohci.rh_port_status[port] |= RH_LOW_SPEED;
	}
	/* Debounced, as a hub does, rather than announced this instant. */
	ohci.rh_debounce = RH_DEBOUNCE_FRAMES;

	rpclog("USB: %s attached to port %d\n",
	       dev->ops->name != NULL ? dev->ops->name : "device", port + 1);

	return 0;
}

UsbDevice *
usb_ohci_detach(int port)
{
	UsbDevice *dev;

	if (!ohci.present || port < 0 || port >= USB_OHCI_PORTS) {
		return NULL;
	}

	dev = ohci.port_device[port];
	ohci.port_device[port] = NULL;

	if (dev != NULL) {
		ohci.rh_port_status[port] &=
		    ~(RH_CONNECTED | RH_ENABLED | RH_LOW_SPEED);
		ohci.rh_port_status[port] |= RH_CONNECT_CHANGE | RH_ENABLE_CHANGE;
		usb_ohci_root_hub_changed();

		rpclog("USB: %s detached from port %d\n",
		       dev->ops->name != NULL ? dev->ops->name : "device", port + 1);
	}

	return dev;
}

UsbDevice *
usb_ohci_device_in_port(int port)
{
	if (!ohci.present || port < 0 || port >= USB_OHCI_PORTS) {
		return NULL;
	}

	return ohci.port_device[port];
}

/** The device answering to an address, or NULL if nobody does. */
static UsbDevice *
usb_device_at_address(unsigned address)
{
	int port;

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		UsbDevice *dev = ohci.port_device[port];

		/*
		 * Only an enabled port takes part. A device whose port the driver
		 * has not enabled must not answer, or a driver would appear to work
		 * without ever resetting the port and would then fail on real
		 * hardware.
		 */
		if (dev != NULL && (ohci.rh_port_status[port] & RH_ENABLED) &&
		    dev->address == address) {
			return dev;
		}
	}

	return NULL;
}

/* --- Transfer descriptors ----------------------------------------------- */

/**
 * Work out how many bytes a transfer descriptor is asking to move.
 *
 * OHCI describes a buffer by its first byte and its last, and allows it to
 * straddle two pages that need not be next to each other - so a length is a
 * subtraction only when both ends are in the same page.
 */
static unsigned
td_length(uint32_t cbp, uint32_t be)
{
	if (cbp == 0) {
		return 0;
	}
	if ((cbp & ~0xfffu) == (be & ~0xfffu)) {
		return be - cbp + 1;
	}

	return (0x1000 - (cbp & 0xfff)) + (be & 0xfff) + 1;
}

/**
 * Move bytes to or from a descriptor's buffer.
 *
 * The buffer may cross one page boundary, and the two pages need not be
 * adjacent in physical memory, so it is done in at most two runs.
 */
static void
td_buffer(uint32_t cbp, uint32_t be, uint8_t *data, unsigned len, int writing)
{
	const unsigned first = ((cbp & ~0xfffu) == (be & ~0xfffu))
	    ? len : (0x1000 - (cbp & 0xfff));
	const unsigned head = (first < len) ? first : len;

	if (writing) {
		ohci_write_bytes(cbp, data, head);
	} else {
		ohci_read_bytes(cbp, data, head);
	}

	if (head < len) {
		const uint32_t second = be & ~0xfffu;

		if (writing) {
			ohci_write_bytes(second, data + head, len - head);
		} else {
			ohci_read_bytes(second, data + head, len - head);
		}
	}
}

/** Put a finished descriptor on the done queue, newest first. */
static void
td_retire(uint32_t td, unsigned cc)
{
	uint32_t flags = ohci_read32(td + TD_FLAGS);

	flags &= 0x0fffffffu;
	flags |= (uint32_t) cc << TD_CC_SHIFT;
	ohci_write32(td + TD_FLAGS, flags);

	ohci_write32(td + TD_NEXTTD, ohci.done_queue);
	ohci.done_queue = td;
	ohci.done_pending = 1;
}

/**
 * Retire a descriptor that failed, and stop the endpoint.
 *
 * OHCI halts an endpoint on any error, not only on a stall: the driver is
 * meant to find it stopped, deal with whatever went wrong and set it going
 * again. Halting only for stalls leaves the controller marching on down the
 * queue after a failure - through the rest of a transfer that has already
 * failed, through the dummy descriptor at the tail, and then into whatever
 * the last NextTD happens to hold. That is how a done list ends up with an
 * address of zero in it, and how a driver walking that list reads the
 * exception vectors as a transfer descriptor.
 */
static void
td_fail(uint32_t ed, uint32_t td, unsigned cc)
{
	td_retire(td, cc);
	ohci_write32(ed + ED_HEADP, ohci_read32(ed + ED_HEADP) | ED_HALTED);
}

/**
 * Run one transfer descriptor of an endpoint.
 *
 * @return Non-zero if the descriptor was dealt with and the endpoint may go on
 *         to the next one. Zero leaves it where it is, which is how a device
 *         with nothing to say yet is handled - the driver comes back next frame.
 */
static int
ohci_run_td(uint32_t ed, uint32_t ed_flags, uint32_t td)
{
	const uint32_t td_flags = ohci_read32(td + TD_FLAGS);
	const uint32_t cbp = ohci_read32(td + TD_CBP);
	const uint32_t be = ohci_read32(td + TD_BE);
	const unsigned want = td_length(cbp, be);
	const unsigned endpoint = ED_EN(ed_flags);
	const unsigned address = ED_FA(ed_flags);

	unsigned dp = td_flags & TD_DP_MASK;
	uint8_t buffer[OHCI_MAX_TRANSFER];
	UsbDevice *dev;
	UsbResult result = USB_NO_DEVICE;
	unsigned moved = 0;
	unsigned cc;

	/* The endpoint may name the direction instead of the descriptor. */
	if ((ed_flags & ED_DIR_MASK) == ED_DIR_OUT) {
		dp = TD_DP_OUT;
	} else if ((ed_flags & ED_DIR_MASK) == ED_DIR_IN) {
		dp = TD_DP_IN;
	}

	dev = usb_device_at_address(address);
	if (dev == NULL) {
		td_fail(ed, td, CC_DEVICE_NOT_RESPONDING);
		return 1;
	}

	if (want > sizeof(buffer)) {
		td_fail(ed, td, CC_DATA_UNDERRUN);
		return 1;
	}

	switch (dp) {
	case TD_DP_SETUP:
		if (endpoint == 0 && want >= 8) {
			uint8_t s[8];

			td_buffer(cbp, be, s, 8, 0);
			ohci.setup.request_type = s[0];
			ohci.setup.request = s[1];
			ohci.setup.value = (uint16_t) (s[2] | (s[3] << 8));
			ohci.setup.index = (uint16_t) (s[4] | (s[5] << 8));
			ohci.setup.length = (uint16_t) (s[6] | (s[7] << 8));

			/*
			 * SET_ADDRESS is the controller's business: the address is
			 * how transactions are routed, so it is recorded here and
			 * the device never sees it.
			 *
			 * Recorded, not applied. The device has to keep answering to
			 * the address it already has until the status stage of this
			 * request has been through, because that status stage is
			 * addressed to the old one.
			 */
			if ((ohci.setup.request_type & USB_TYPE_MASK) ==
			        USB_TYPE_STANDARD &&
			    ohci.setup.request == USB_REQ_SET_ADDRESS) {
				ohci.pending_address =
				    (uint8_t) (ohci.setup.value & 0x7f);
				ohci.address_pending = 1;
				ohci.setup_valid = 0;
			} else {
				ohci.setup_valid = 1;
			}
			ohci.setup_taken = 0;
			ohci.control_length = 0;
			ohci.control_done = 0;
			result = USB_ACK;
			moved = want;
		} else {
			result = USB_STALL;
		}
		break;

	case TD_DP_IN:
		if (endpoint == 0) {
			if (!ohci.setup_valid) {
				/* The status stage of a request that went out, which
				   is where a no-data request reaches the device. An
				   address the device was told to take takes effect
				   here, now that this stage has reached it. */
				if (ohci.address_pending) {
					dev->address = ohci.pending_address;
					ohci.address_pending = 0;
				}
				result = USB_ACK;
				moved = 0;
				break;
			}
			if (!(ohci.setup.request_type & USB_DIR_IN)) {
				if (!ohci.control_done) {
					unsigned len = ohci.setup_taken;

					result = dev->ops->control(dev, &ohci.setup,
					    ohci.control_data, &len);
					if (result != USB_ACK) {
						break;
					}
					ohci.control_done = 1;
				}
				result = USB_ACK;
				moved = 0;
				break;
			}
			if (ohci.setup_taken == 0) {
				unsigned len = sizeof(ohci.control_data);

				result = dev->ops->control(dev, &ohci.setup,
				    ohci.control_data, &len);
				ohci.control_length = (result == USB_ACK) ? len : 0;
				if (result != USB_ACK) {
					break;
				}
			}
			moved = ohci.control_length - ohci.setup_taken;
			if (moved > want) {
				moved = want;
			}
			if (moved > 0) {
				td_buffer(cbp, be, &ohci.control_data[ohci.setup_taken],
				    moved, 1);
			}
			ohci.setup_taken += moved;
			result = USB_ACK;
		} else {
			unsigned len = want;

			if (dev->ops->read == NULL) {
				result = USB_STALL;
				break;
			}
			result = dev->ops->read(dev, endpoint, buffer, &len);
			if (result == USB_ACK) {
				moved = len;
				if (moved > 0) {
					td_buffer(cbp, be, buffer, moved, 1);
				}
			}
		}
		break;

	case TD_DP_OUT:
		if (endpoint == 0) {
			if (ohci.setup_valid &&
			    !(ohci.setup.request_type & USB_DIR_IN) &&
			    ohci.setup.length > 0 && !ohci.control_done) {
				unsigned room = sizeof(ohci.control_data) -
				    ohci.setup_taken;
				unsigned take = (want < room) ? want : room;

				if (take > 0) {
					td_buffer(cbp, be,
					    &ohci.control_data[ohci.setup_taken], take, 0);
					ohci.setup_taken += take;
				}
				moved = want;
				result = USB_ACK;
			} else {
				/* The status stage of a read. */
				result = USB_ACK;
				moved = want;
				ohci.setup_valid = 0;
			}
		} else if (dev->ops->write != NULL) {
			if (want > 0) {
				td_buffer(cbp, be, buffer, want, 0);
			}
			result = dev->ops->write(dev, endpoint, buffer, want);
			moved = (result == USB_ACK) ? want : 0;
		} else {
			result = USB_STALL;
		}
		break;

	default:
		result = USB_STALL;
		break;
	}

	if (result == USB_NAK) {
		/*
		 * Nothing to say yet. The descriptor stays exactly where it is and
		 * the endpoint is left alone, so the controller comes back to it on
		 * a later frame - which is what a real one does with an idle
		 * interrupt endpoint, and what makes a passed-through device work
		 * without the guest knowing.
		 */
		return 0;
	}

	switch (result) {
	case USB_ACK:
		cc = CC_NO_ERROR;
		break;
	case USB_STALL:
		cc = CC_STALL;
		break;
	default:
		cc = CC_DEVICE_NOT_RESPONDING;
		break;
	}

	/*
	 * Report how far the buffer got. OHCI says CurrentBufferPointer is zero
	 * when everything asked for was moved, and otherwise points at the next
	 * byte that would have been.
	 */
	if (moved >= want) {
		ohci_write32(td + TD_CBP, 0);
	} else {
		ohci_write32(td + TD_CBP, cbp + moved);

		/* Less than was asked for. That is an underrun unless the descriptor
		   says a short packet is acceptable. */
		if (cc == CC_NO_ERROR && (td_flags & TD_ROUNDING) == 0) {
			cc = CC_DATA_UNDERRUN;
		}
	}

	if (cc != CC_NO_ERROR) {
		td_fail(ed, td, cc);
	} else {
		td_retire(td, cc);
	}

	return 1;
}

/* --- Isochronous transfer descriptors ----------------------------------- */

/*
 * Isochronous is the one kind of transfer that is about time rather than about
 * data. A general descriptor says "move these bytes and tell me how it went",
 * and if the device has nothing to say the controller comes back to the same
 * descriptor next frame. An isochronous descriptor instead says "in frame N,
 * whatever this device has to say goes here" for up to eight consecutive
 * frames, and each of those frames happens once whether the device spoke in it
 * or not. Nothing is retried, because by the time anyone noticed, the frame it
 * belonged to has gone.
 *
 * That is why this is a separate path rather than a flag on the general one:
 * a descriptor is visited on eight frames instead of one, a device saying
 * nothing is a result rather than a reason to wait, and an error must not halt
 * the endpoint, because halting a stream to report one lost packet loses all
 * the rest as well.
 */

/** Read one of the eight offset/status halfwords. */
static uint16_t
itd_psw_read(uint32_t itd, unsigned index)
{
	const uint32_t word = ohci_read32(itd + ITD_PSW + (index & ~1u) * 2);

	return (uint16_t) ((index & 1) ? (word >> 16) : word);
}

/** Write one of them back, leaving its neighbour alone. */
static void
itd_psw_write(uint32_t itd, unsigned index, uint16_t val)
{
	const uint32_t at = itd + ITD_PSW + (index & ~1u) * 2;
	uint32_t word = ohci_read32(at);

	if (index & 1) {
		word = (word & 0x0000ffffu) | ((uint32_t) val << 16);
	} else {
		word = (word & 0xffff0000u) | val;
	}

	ohci_write32(at, word);
}

/**
 * Turn an offset within the descriptor's buffer into a physical address.
 *
 * The buffer is described as two pages, and an offset's top bit says which of
 * them it is in. The two need not be next to each other in physical memory, so
 * this is a choice between two bases rather than an addition.
 */
static uint32_t
itd_address(uint32_t page0, uint32_t page1, unsigned offset)
{
	return ((offset & PSW_PAGE_SELECT) ? page1 : page0) | (offset & 0xfff);
}

/**
 * Move one frame's packet.
 *
 * @param index Which of the descriptor's packets, which is how far into its run
 *              of frames this frame is.
 */
static void
itd_run_packet(uint32_t itd, uint32_t bp0, uint32_t be, unsigned count,
               unsigned index, UsbDevice *dev, unsigned endpoint, int reading)
{
	const uint32_t page0 = bp0 & ~0xfffu;
	const uint32_t page1 = be & ~0xfffu;
	const unsigned offset = itd_psw_read(itd, index) & PSW_OFFSET_MASK;
	const unsigned end = (page1 == page0) ? (be & 0xfff)
	    : (PSW_PAGE_SELECT | (be & 0xfff));

	uint8_t buffer[ITD_PACKET_MAX];
	unsigned room, len = 0, cc = CC_NO_ERROR;

	/*
	 * How much room this packet has. Every packet but the last is bounded by
	 * where the next one starts; the last is bounded by the end of the buffer,
	 * which the specification gives as the address of its final byte.
	 */
	if (index + 1 < count) {
		const unsigned next = itd_psw_read(itd, index + 1) & PSW_OFFSET_MASK;

		room = (next > offset) ? next - offset : 0;
	} else {
		room = (end >= offset) ? end - offset + 1 : 0;
	}

	if (room > sizeof(buffer)) {
		cc = CC_DATA_OVERRUN;
		room = sizeof(buffer);
	}

	if (reading) {
		unsigned got = room;

		if (dev->ops->read == NULL) {
			cc = CC_STALL;
		} else {
			switch (dev->ops->read(dev, endpoint, buffer, &got)) {
			case USB_ACK:
				len = (got > room) ? room : got;
				if (len > 0) {
					td_buffer(itd_address(page0, page1, offset),
					    itd_address(page0, page1, offset + len - 1),
					    buffer, len, 1);
				}
				break;

			case USB_NAK:
				/*
				 * Nothing from the device this frame. On any other kind of
				 * transfer that would mean come back later; here it is the
				 * answer, and a truthful one - a camera between frames and
				 * a microphone in silence both send a packet with no bytes
				 * in it, every frame, and a driver expects exactly that.
				 */
				len = 0;
				break;

			case USB_STALL:
				cc = CC_STALL;
				break;

			default:
				cc = CC_DEVICE_NOT_RESPONDING;
				break;
			}
		}
	} else if (dev->ops->write == NULL) {
		cc = CC_STALL;
	} else {
		if (room > 0) {
			td_buffer(itd_address(page0, page1, offset),
			    itd_address(page0, page1, offset + room - 1),
			    buffer, room, 0);
		}

		switch (dev->ops->write(dev, endpoint, buffer, room)) {
		case USB_STALL:
			cc = CC_STALL;
			break;

		case USB_NO_DEVICE:
			cc = CC_DEVICE_NOT_RESPONDING;
			break;

		default:
			/*
			 * Taken, or dropped because the host end could not keep up.
			 * Both are reported to the guest as sent, because that is what
			 * a real controller would have done with the frame and there is
			 * no way to hand the packet back for a later one. A drop is
			 * logged where it happens, in usb_host.c, which is the only
			 * place that knows it happened.
			 */
			len = room;
			break;
		}
	}

	itd_psw_write(itd, index,
	    (uint16_t) ((cc << PSW_CC_SHIFT) | (len & PSW_LENGTH_MASK)));
}

/**
 * The condition code for a descriptor as a whole, from its packets.
 *
 * The first packet that went wrong is what the descriptor reports. Packets that
 * were never reached are not failures: a driver reads them as having moved
 * nothing, which is what happened.
 */
static unsigned
itd_result(uint32_t itd, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		const unsigned cc = itd_psw_read(itd, i) >> PSW_CC_SHIFT;

		if (cc != CC_NO_ERROR &&
		    (cc & CC_NOT_ACCESSED_MASK) != CC_NOT_ACCESSED) {
			return cc;
		}
	}

	return CC_NO_ERROR;
}

/**
 * Run an isochronous descriptor for this frame.
 *
 * @return Non-zero when the descriptor is finished with and the endpoint may go
 *         on to the next one. Zero leaves it at the head of the queue, which is
 *         how a descriptor covering several frames is visited on each of them.
 */
static int
ohci_run_itd(uint32_t ed_flags, uint32_t itd)
{
	const uint32_t flags = ohci_read32(itd + ITD_FLAGS);
	const uint32_t bp0 = ohci_read32(itd + ITD_BP0);
	const uint32_t be = ohci_read32(itd + ITD_BE);
	const unsigned count = ITD_FC(flags);
	const unsigned index = (ohci.fm_number - ITD_SF(flags)) & 0xffff;

	/*
	 * An isochronous endpoint names its own direction and its descriptors
	 * carry no direction of their own. Anything that is not explicitly OUT is
	 * read as IN, which for a malformed endpoint returns nothing rather than
	 * sending a device whatever happened to be in the buffer.
	 */
	const int reading = (ed_flags & ED_DIR_MASK) != ED_DIR_OUT;
	UsbDevice *dev;

	/*
	 * Its first frame has not arrived yet. Frame numbers are sixteen bits and
	 * wrap, so "still to come" is the far half of the difference rather than a
	 * comparison - which also gets the descriptor scheduled just before a wrap
	 * right, where a comparison would run it 65535 frames early.
	 */
	if (index >= 0x8000) {
		return 0;
	}

	dev = usb_device_at_address(ED_FA(ed_flags));
	if (dev == NULL) {
		td_retire(itd, CC_DEVICE_NOT_RESPONDING);
		return 1;
	}

	if (index < count) {
		itd_run_packet(itd, bp0, be, count, index, dev, ED_EN(ed_flags),
		    reading);
	}

	/*
	 * Finished once its last frame has been and gone. Note the retirement is
	 * a plain one: unlike a general descriptor, a failed packet must not halt
	 * the endpoint, because a stream that stops at the first lost packet loses
	 * everything after it as well, and the driver is told which packet went
	 * wrong either way.
	 *
	 * The test is "past the end" rather than "at the end" so that a descriptor
	 * whose frames went by while the machine was busy elsewhere is still
	 * retired rather than sitting at the head of the queue for ever. Its
	 * unreached packets keep the NotAccessed the driver left in them.
	 */
	if (index + 1 < count) {
		return 0;
	}

	td_retire(itd, itd_result(itd, count));

	return 1;
}

/**
 * Run one endpoint descriptor: its queue of transfer descriptors, head first.
 *
 * @return The next endpoint in the list.
 */
static uint32_t
ohci_run_ed(uint32_t ed)
{
	const uint32_t flags = ohci_read32(ed + ED_FLAGS);
	const uint32_t tail = ohci_read32(ed + ED_TAILP) & ED_HEADMASK;
	const uint32_t next = ohci_read32(ed + ED_NEXTED);
	uint32_t head = ohci_read32(ed + ED_HEADP);
	uint32_t halted;
	int guard = 0;

	if (flags & ED_SKIP) {
		return next;
	}
	if (head & ED_HALTED) {
		return next;
	}

	/* An endpoint's queue runs until it is empty, or until a descriptor says
	   it is not ready. The guard is because the list lives in guest memory
	   and a bad one must not spin the emulator. */
	while ((head & ED_HEADMASK) != tail && guard++ < 64) {
		const uint32_t td = head & ED_HEADMASK;
		uint32_t td_next;

		/*
		 * A queue that reaches zero without reaching the tail is a broken
		 * one, and address zero is the exception vectors: read four words
		 * from there and they look like a plausible descriptor, which is
		 * far worse than stopping.
		 */
		if (td == 0) {
			break;
		}

		td_next = ohci_read32(td + TD_NEXTTD) & ED_HEADMASK;

		/*
		 * The endpoint says which shape its descriptors are, and the two are
		 * read quite differently - eight words against four, and a packet per
		 * frame against one transfer. Only the link word is in the same place,
		 * which is what lets one queue walk serve both.
		 */
		if (flags & ED_FORMAT_ISO) {
			if (!ohci_run_itd(flags, td)) {
				break;	/* its frames are not all done with yet */
			}
		} else if (!ohci_run_td(ed, flags, td)) {
			break;		/* not ready; leave the queue alone */
		}

		/*
		 * Step past the descriptor. If running it halted the endpoint,
		 * the halt has to survive being written back here - reading it
		 * out of the endpoint again is what keeps that from being undone
		 * by this very assignment - and the queue stops. OHCI leaves
		 * HeadP pointing past the failed descriptor with the halt set,
		 * so the driver can see where it got to.
		 */
		halted = ohci_read32(ed + ED_HEADP) & ED_HALTED;
		head = td_next | (head & ED_TOGGLECARRY) | halted;
		ohci_write32(ed + ED_HEADP, head);

		if (halted) {
			break;
		}
	}

	return next;
}

/** Walk a list of endpoint descriptors. */
static void
ohci_run_list(uint32_t ed)
{
	int guard = 0;

	while (ed != 0 && guard++ < 128) {
		ed = ohci_run_ed(ed) & ED_HEADMASK;
	}
}

/**
 * Hand the done queue back to the driver.
 *
 * OHCI does this by writing the head of the queue into the communication area
 * and raising WritebackDoneHead; the driver takes the list and works back along
 * it. The queue is only published once the driver has acknowledged the last
 * one, which is what HcDoneHead being non-zero means.
 */
static void
ohci_writeback_done(void)
{
	if (!ohci.done_pending || ohci.hcca == 0) {
		return;
	}
	if (ohci.interrupt_status & OHCI_WDH) {
		return;		/* the driver has not taken the last list yet */
	}

	ohci_write32(ohci.hcca + HCCA_DONE_HEAD, ohci.done_queue);
	ohci.done_head = ohci.done_queue;
	ohci.done_queue = 0;
	ohci.done_pending = 0;

	usb_ohci_raise(OHCI_WDH);
}

/* --- Frames ------------------------------------------------------------- */

void
usb_ohci_frame(void)
{
	int port;

	if (!ohci.present) {
		return;
	}

	ohci.fm_number = (ohci.fm_number + 1) & 0xffffu;
	ohci.fm_remaining = ohci.fm_interval & 0x3fff;

	/* Give passed-through devices a moment to notice a real transfer has
	   come back, and to notice one being unplugged from the host. */
	usb_host_poll();

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		if (usb_host_device_gone(ohci.port_device[port])) {
			rpclog("USB: port %d unplugged, its device having left the "
			       "host\n", port + 1);
			usb_host_destroy(usb_ohci_detach(port));
			ohci.port_kind[port] = 0;
			ohci.port_id[port][0] = '\0';
		}
	}

	if ((ohci.control & OHCI_HCFS_MASK) != OHCI_HCFS_OPERATIONAL) {
		return;
	}

	/*
	 * A port change that has finished settling is now worth reporting.
	 *
	 * Once only. The root hub status change is edge triggered - OHCI 6.3.4
	 * has it set "when the content of HcRhStatus or the content of any
	 * HcRhPortStatus has changed" - so it is raised when something moves and
	 * then left alone until the driver clears it. Re-deriving it from the
	 * port change bits every frame instead looks tempting, because the
	 * driver's own comment says it expects the interrupt to stay on until
	 * the port has been reset, but it live-locks the machine: the driver
	 * turns an interrupt off in its own mask before turning it off in the
	 * controller, and anything raised in that window is asserted with
	 * nobody willing to handle it.
	 */
	if (ohci.rh_debounce > 0 && --ohci.rh_debounce == 0) {
		usb_ohci_root_hub_changed();
	}

	if (ohci.hcca != 0) {
		ohci_write32(ohci.hcca + HCCA_FRAME_NUMBER, ohci.fm_number);
	}

	/*
	 * The periodic list first, as a real controller does: it is the one with
	 * a deadline. Its head for this frame is one of the thirty-two entries
	 * in the communication area, chosen by the frame number.
	 */
	if ((ohci.control & OHCI_PLE) && ohci.hcca != 0) {
		const uint32_t entry = ohci_read32(ohci.hcca +
		    HCCA_INTERRUPT_TABLE + ((ohci.fm_number & 31) * 4));

		ohci_run_list(entry & ED_HEADMASK);
	}

	if ((ohci.control & OHCI_CLE) && (ohci.command_status & OHCI_CLF)) {
		ohci_run_list(ohci.control_head_ed & ED_HEADMASK);
	}
	if ((ohci.control & OHCI_BLE) && (ohci.command_status & OHCI_BLF)) {
		ohci_run_list(ohci.bulk_head_ed & ED_HEADMASK);
	}

	ohci_writeback_done();

	usb_ohci_raise(OHCI_SF);
}

/* --- The register file -------------------------------------------------- */

uint32_t
usb_ohci_read(uint32_t offset)
{
	if (!ohci.present) {
		return 0xffffffff;
	}

	if (offset >= HcRhPortStatus1 &&
	    offset < HcRhPortStatus1 + USB_OHCI_PORTS * 4) {
		return ohci.rh_port_status[(offset - HcRhPortStatus1) / 4];
	}

	switch (offset) {
	case HcRevision:		return 0x00000010;	/* OHCI 1.0 */
	case HcControl:			return ohci.control;
	case HcCommandStatus:		return ohci.command_status;
	case HcInterruptStatus:		return ohci.interrupt_status;
	case HcInterruptEnable:
	case HcInterruptDisable:	return ohci.interrupt_enable;
	case HcHCCA:			return ohci.hcca;
	case HcPeriodCurrentED:		return ohci.period_current_ed;
	case HcControlHeadED:		return ohci.control_head_ed;
	case HcControlCurrentED:	return ohci.control_current_ed;
	case HcBulkHeadED:		return ohci.bulk_head_ed;
	case HcBulkCurrentED:		return ohci.bulk_current_ed;
	case HcDoneHead:		return ohci.done_head;
	case HcFmInterval:		return ohci.fm_interval;
	case HcFmRemaining:		return ohci.fm_remaining;
	case HcFmNumber:		return ohci.fm_number;
	case HcPeriodicStart:		return ohci.periodic_start;
	case HcLSThreshold:		return ohci.ls_threshold;
	case HcRhDescriptorA:		return ohci.rh_descriptor_a;
	case HcRhDescriptorB:		return ohci.rh_descriptor_b;
	case HcRhStatus:		return ohci.rh_status;
	default:			return 0;
	}
}

/** A write to a root hub port register is a set of requests, not a value. */
static void
usb_ohci_write_port(int port, uint32_t val)
{
	uint32_t status = ohci.rh_port_status[port];
	const int connected = (status & RH_CONNECTED) != 0;

	if (val & (1u << 0)) {			/* ClearPortEnable */
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
	if (val & (1u << 3)) {			/* ClearSuspendStatus */
		if (status & RH_SUSPENDED) {
			status &= ~RH_SUSPENDED;
			status |= RH_SUSPEND_CHANGE;
		}
	}
	if ((val & (1u << 4)) && connected) {	/* SetPortReset */
		/*
		 * A reset finishes far faster than a driver can look, so there is
		 * no point modelling the 10ms: the port comes back reset and
		 * enabled with the change bit set, which is the state a driver
		 * waits for. The device goes back to address 0, which is how
		 * enumeration starts.
		 */
		UsbDevice *dev = ohci.port_device[port];

		status &= ~RH_RESET;
		status |= RH_ENABLED | RH_RESET_CHANGE;

		if (dev != NULL) {
			dev->address = 0;
			if (dev->ops->reset != NULL) {
				dev->ops->reset(dev);
			}
		}
		ohci.setup_valid = 0;
		ohci.address_pending = 0;
	}
	if (val & (1u << 8)) {			/* SetPortPower */
		status |= RH_POWERED;
	}
	if (val & (1u << 9)) {			/* ClearPortPower */
		status &= ~RH_POWERED;
	}

	status &= ~(val & 0xffff0000u);		/* acknowledgements */

	ohci.rh_port_status[port] = status;

	if (val & 0xffff0000u) {
		usb_ohci_root_hub_changed();
	}
}

void
usb_ohci_write(uint32_t offset, uint32_t val)
{
	if (!ohci.present) {
		return;
	}

	if (offset >= HcRhPortStatus1 &&
	    offset < HcRhPortStatus1 + USB_OHCI_PORTS * 4) {
		usb_ohci_write_port((int) ((offset - HcRhPortStatus1) / 4), val);
		return;
	}

	switch (offset) {
	case HcControl:
		ohci.control = val;
		break;

	case HcCommandStatus:
		if (val & OHCI_HCR) {
			usb_ohci_reset();
			return;
		}
		/* The list-filled bits are set by the driver and cleared by us
		   when the list has been walked, so they are only ever set here. */
		ohci.command_status |= val & (OHCI_CLF | OHCI_BLF | OHCI_OCR);
		break;

	case HcInterruptStatus:
		/* Write to clear. */
		ohci.interrupt_status &= ~val;
		if (val & OHCI_WDH) {
			ohci.done_head = 0;
		}
		usb_ohci_update_irq();
		break;

	case HcInterruptEnable:
		ohci.interrupt_enable |= val;
		usb_ohci_update_irq();
		break;

	case HcInterruptDisable:
		ohci.interrupt_enable &= ~val;
		usb_ohci_update_irq();
		break;

	case HcHCCA:			ohci.hcca = val & ~0xffu; break;
	case HcPeriodCurrentED:		ohci.period_current_ed = val; break;
	case HcControlHeadED:		ohci.control_head_ed = val; break;
	case HcControlCurrentED:	ohci.control_current_ed = val; break;
	case HcBulkHeadED:		ohci.bulk_head_ed = val; break;
	case HcBulkCurrentED:		ohci.bulk_current_ed = val; break;
	case HcDoneHead:		ohci.done_head = val; break;
	case HcFmInterval:		ohci.fm_interval = val; break;
	case HcFmNumber:		ohci.fm_number = val & 0xffff; break;
	case HcPeriodicStart:		ohci.periodic_start = val; break;
	case HcLSThreshold:		ohci.ls_threshold = val; break;

	case HcRhDescriptorA:
		/* The port count is ours to state, not the driver's to change. */
		ohci.rh_descriptor_a = (val & 0xffffff00u) | USB_OHCI_PORTS;
		break;

	case HcRhDescriptorB:		ohci.rh_descriptor_b = val; break;

	case HcRhStatus:
		if (val & RHS_LPSC) {		/* SetGlobalPower */
			int port;

			for (port = 0; port < USB_OHCI_PORTS; port++) {
				ohci.rh_port_status[port] |= RH_POWERED;
			}
		}
		if (val & RHS_LPS) {		/* ClearGlobalPower */
			int port;

			for (port = 0; port < USB_OHCI_PORTS; port++) {
				ohci.rh_port_status[port] &= ~RH_POWERED;
			}
		}
		if (val & RHS_DRWE) {
			ohci.rh_status |= RHS_DRWE;
		}
		if (val & RHS_CRWE) {
			ohci.rh_status &= ~RHS_DRWE;
		}
		break;

	default:
		break;
	}
}

/* --- Coming and going --------------------------------------------------- */

void
usb_ohci_reset(void)
{
	int port;

	/*
	 * A controller reset is not an unplug: whatever is in the ports is still
	 * in them afterwards, as it would be on a real machine. Emptying them is
	 * a machine reset, which goes through usb_ohci_init().
	 */
	ohci.control = OHCI_HCFS_RESET;
	ohci.command_status = 0;
	ohci.interrupt_status = 0;
	ohci.interrupt_enable = 0;
	ohci.hcca = 0;
	ohci.period_current_ed = 0;
	ohci.control_head_ed = 0;
	ohci.control_current_ed = 0;
	ohci.bulk_head_ed = 0;
	ohci.bulk_current_ed = 0;
	ohci.done_head = 0;
	ohci.done_queue = 0;
	ohci.done_pending = 0;
	ohci.fm_interval = 0x2edf;		/* 11999, the OHCI frame interval */
	ohci.fm_remaining = 0;
	ohci.periodic_start = 0x2a2f;		/* 90% of the frame */
	ohci.ls_threshold = 0x0628;

	/* Four ports, per-port power switching, per-port over-current. */
	ohci.rh_descriptor_a = 0x01000000u | USB_OHCI_PORTS;
	ohci.rh_descriptor_b = 0;
	ohci.rh_status = 0;

	ohci.setup_valid = 0;
	ohci.address_pending = 0;
	ohci.setup_taken = 0;
	ohci.control_length = 0;
	ohci.control_done = 0;

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		uint32_t status = 0;

		if (ohci.port_device[port] != NULL) {
			status = RH_CONNECTED | RH_CONNECT_CHANGE;
			if (ohci.port_device[port]->low_speed) {
				status |= RH_LOW_SPEED;
			}
		}
		ohci.rh_port_status[port] = status;
	}

	/*
	 * Anything still plugged in has just had its connect-change bit set
	 * again above, and the root hub status change interrupt is defined by
	 * those bits - so it has to be raised again rather than left at the
	 * zero the status word was cleared to. Getting this wrong loses the
	 * interrupt permanently: the driver resets the controller as the last
	 * step of starting it, so a device present at that moment would sit
	 * there connected, with its change bit set, and never be looked at.
	 *
	 * After the debounce, though, not now - see rh_debounce. Right now the
	 * driver is part way through starting the controller and has no hub to
	 * hand the change to.
	 */
	ohci.rh_debounce = RH_DEBOUNCE_FRAMES;
	usb_ohci_update_irq();
}

void
usb_ohci_init(void (*irq_change)(int level))
{
	int port;

	/* Anything the last machine had plugged in has to be given back to the
	   host rather than merely dropped. */
	for (port = 0; port < USB_OHCI_PORTS; port++) {
		usb_host_destroy(ohci.port_device[port]);
		ohci.port_device[port] = NULL;
	}

	memset(&ohci, 0, sizeof(ohci));
	ohci.irq_change = irq_change;
	ohci.present = 1;
	usb_ohci_reset();

	rpclog("USB: OHCI host controller at EASI +0x%06x, %d root hub port(s)\n",
	       USB_OHCI_EASI_OFFSET, USB_OHCI_PORTS);
}

int
usb_ohci_enabled(void)
{
	return ohci.present;
}

/** Let go of whatever was in a port, and give it back to the host. */
static void
usb_ohci_release(int port)
{
	usb_host_destroy(usb_ohci_detach(port));

	ohci.port_kind[port] = 0;
	ohci.port_id[port][0] = '\0';
}

void
usb_ohci_shutdown(void)
{
	int port;

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		usb_ohci_release(port);
	}
}

void
usb_ohci_apply_config(void)
{
	int port;

	if (!ohci.present) {
		return;
	}

	for (port = 0; port < USB_OHCI_PORTS; port++) {
		const int wanted = config.usb_port[port];
		const char *wanted_id = config.usb_host[port];
		UsbDevice *dev = NULL;

		/* Already exactly this? Then leave it alone: re-plugging a device
		   the guest is using would look to it like someone pulling the lead
		   out and putting it back. */
		if (ohci.port_kind[port] == wanted &&
		    strcmp(ohci.port_id[port], wanted_id) == 0) {
			continue;
		}

		usb_ohci_release(port);

		if (wanted == UsbAttachment_Host) {
			unsigned vendor, product;
			const char *why = NULL;

			if (sscanf(wanted_id, "%4x:%4x", &vendor, &product) != 2) {
				rpclog("USB: port %d asks for host device '%s', which is "
				       "not an identifier\n", port + 1, wanted_id);
				continue;
			}

			dev = usb_host_create((uint16_t) vendor, (uint16_t) product,
			    &why);
			if (dev == NULL) {
				rpclog("USB: port %d could not take %s: %s\n", port + 1,
				       wanted_id,
				       (why != NULL && *why != '\0') ? why
				                                     : "no reason given");
				continue;
			}
		}

		if (dev == NULL) {
			continue;
		}

		if (usb_ohci_attach(port, dev) != 0) {
			usb_host_destroy(dev);
			continue;
		}

		ohci.port_kind[port] = wanted;
		snprintf(ohci.port_id[port], sizeof(ohci.port_id[port]), "%s",
		         wanted_id);
	}
}
