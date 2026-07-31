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
 * test_ohci_iso.c - the OHCI controller's isochronous transfer descriptors
 *
 * An isochronous descriptor is the one part of the controller a driver cannot
 * be lenient about, because it is the only one where the emulator writes
 * something back that the driver reads as a length. Get the offsets wrong and a
 * webcam streams into the wrong half of its buffer; get the status halfword
 * wrong and a driver either reads packets that were never filled or discards
 * ones that were. Neither shows up as a failure - both show up as a stream that
 * looks like it works and produces rubbish.
 *
 * So this drives the descriptors themselves: the emulated controller is given a
 * communication area and endpoint and transfer descriptors in a stand-in for
 * guest memory, exactly as the shipped OHCIDriver builds them, and what it
 * writes back is checked against what that driver reads. The device on the
 * other end is a synthetic one, so the answers are known.
 *
 * The unit is compiled directly and its surroundings stubbed: linking the real
 * memory system would drag in the whole emulator, and libusb with it.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "usb_device.h"
#include "usb_ohci.h"

/* ---- guest memory, and the little of the emulator the controller wants ---- */

/*
 * A stand-in for guest RAM.
 *
 * Deliberately larger than the descriptors need: the tests place a buffer
 * across a page boundary, and one of them puts the two pages of a descriptor's
 * buffer out of order, which is legal and is the case a subtraction gets wrong.
 */
#define TEST_RAM_SIZE	0x10000

static uint8_t test_ram[TEST_RAM_SIZE];

uint32_t
mem_phys_read32(uint32_t addr)
{
	if (addr + 3 >= TEST_RAM_SIZE) {
		printf("FAIL: read outside the test's memory at 0x%08x\n", addr);
		exit(2);
	}

	return (uint32_t) test_ram[addr] | ((uint32_t) test_ram[addr + 1] << 8) |
	       ((uint32_t) test_ram[addr + 2] << 16) |
	       ((uint32_t) test_ram[addr + 3] << 24);
}

void
mem_phys_write32(uint32_t addr, uint32_t val)
{
	if (addr + 3 >= TEST_RAM_SIZE) {
		printf("FAIL: write outside the test's memory at 0x%08x\n", addr);
		exit(2);
	}

	test_ram[addr] = (uint8_t) val;
	test_ram[addr + 1] = (uint8_t) (val >> 8);
	test_ram[addr + 2] = (uint8_t) (val >> 16);
	test_ram[addr + 3] = (uint8_t) (val >> 24);
}

Config config;

void rpclog(const char *format, ...) { (void) format; }
void fatal(const char *format, ...) { (void) format; printf("fatal() called\n"); exit(2); }

/* No passthrough here: every device in this test is a synthetic one. */
void usb_host_poll(void) { }
int usb_host_device_gone(const struct UsbDevice *dev) { (void) dev; return 0; }
void usb_host_destroy(struct UsbDevice *dev) { (void) dev; }
struct UsbDevice *usb_host_create(uint16_t vendor, uint16_t product,
    const char **why)
{
	(void) vendor; (void) product;
	if (why != NULL) {
		*why = "no passthrough in the test";
	}
	return NULL;
}

/* ---- the register file, as byte offsets, mirrored from the unit ---------- */

#define HcControl		0x04
#define HcInterruptStatus	0x0c
#define HcInterruptEnable	0x10
#define HcHCCA			0x18
#define HcFmNumber		0x3c
#define HcRhStatus		0x50
#define HcRhPortStatus1		0x54

#define OHCI_PLE		0x00000004
#define OHCI_HCFS_OPERATIONAL	0x00000080
#define OHCI_WDH		0x00000002

#define ED_DIR_OUT		0x00000800
#define ED_DIR_IN		0x00001000
#define ED_FORMAT_ISO		0x00008000

#define PSW_NOT_ACCESSED	0xe000	/* what a driver leaves in an offset */

/* Where things live in the stand-in memory. */
#define HCCA_AT			0x1000
#define ED_AT			0x1200
#define ITD_AT			0x1240
#define ITD2_AT			0x1280
#define TAIL_AT			0x12c0	/* the dummy descriptor a queue ends on */
#define BUFFER_AT		0x2000

static int failures;

static void
check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

static void
check_eq(unsigned got, unsigned want, const char *what)
{
	if (got != want) {
		printf("FAIL: %s: got %u (0x%x), wanted %u (0x%x)\n", what, got, got,
		    want, want);
		failures++;
	} else {
		printf("ok  : %s\n", what);
	}
}

/* ---- a synthetic device that streams --------------------------------------- */

/*
 * It answers an IN endpoint with a run of bytes whose first byte counts the
 * packets, so a packet's contents say which packet it was and a buffer can be
 * checked for both order and placement. It can be told to send short packets,
 * nothing at all, or to refuse.
 */
typedef struct {
	unsigned packets_sent;
	unsigned send_len;		/* bytes per packet */
	int nak;			/* nothing to say */
	int stall;
	unsigned writes;
	unsigned written[16];		/* lengths the controller sent us */
	uint8_t last_write[64];
} Streamer;

static Streamer streamer;

static UsbResult
streamer_read(UsbDevice *dev, unsigned endpoint, uint8_t *data, unsigned *len)
{
	Streamer *s = dev->state;
	unsigned i, take = s->send_len;

	(void) endpoint;

	if (s->stall) {
		return USB_STALL;
	}
	if (s->nak) {
		return USB_NAK;
	}
	if (take > *len) {
		take = *len;
	}

	for (i = 0; i < take; i++) {
		data[i] = (uint8_t) (s->packets_sent + 1);
	}

	s->packets_sent++;
	*len = take;

	return USB_ACK;
}

static UsbResult
streamer_write(UsbDevice *dev, unsigned endpoint, const uint8_t *data,
               unsigned len)
{
	Streamer *s = dev->state;

	(void) endpoint;

	if (s->stall) {
		return USB_STALL;
	}

	if (s->writes < 16) {
		s->written[s->writes] = len;
	}
	if (len > 0 && len <= sizeof(s->last_write)) {
		memcpy(s->last_write, data, len);
	}
	s->writes++;

	return USB_ACK;
}

static void streamer_reset(UsbDevice *dev) { (void) dev; }

static const UsbDeviceOps streamer_ops = {
	"streaming test device",
	streamer_reset,
	NULL,			/* no control requests: the address is set by hand */
	streamer_read,
	streamer_write
};

static UsbDevice streamer_device;

/* ---- building descriptors, the way the driver does ------------------------ */

static void
ram_write32(uint32_t addr, uint32_t val)
{
	mem_phys_write32(addr, val);
}

static uint32_t
ram_read32(uint32_t addr)
{
	return mem_phys_read32(addr);
}

static uint16_t
psw_read(uint32_t itd, unsigned index)
{
	const uint32_t word = ram_read32(itd + 16 + (index & ~1u) * 2);

	return (uint16_t) ((index & 1) ? (word >> 16) : word);
}

static void
psw_write(uint32_t itd, unsigned index, uint16_t val)
{
	const uint32_t at = itd + 16 + (index & ~1u) * 2;
	uint32_t word = ram_read32(at);

	if (index & 1) {
		word = (word & 0x0000ffffu) | ((uint32_t) val << 16);
	} else {
		word = (word & 0xffff0000u) | val;
	}

	ram_write32(at, word);
}

/**
 * Build an isochronous descriptor.
 *
 * @param offsets The offset of each packet within the buffer, thirteen bits,
 *                whose top bit chooses the second page - exactly what the
 *                driver puts there.
 * @param be      The address of the buffer's last byte.
 */
static void
make_itd(uint32_t itd, uint32_t next, unsigned start_frame, unsigned packets,
         uint32_t bp0, uint32_t be, const unsigned *offsets)
{
	unsigned i;

	/* NoCC in the condition code, the starting frame, and the packet count
	   held one less than it is. */
	ram_write32(itd + 0, 0xf0000000u | (start_frame & 0xffff) |
	    ((packets - 1) << 24));
	ram_write32(itd + 4, bp0);
	ram_write32(itd + 8, next);
	ram_write32(itd + 12, be);

	for (i = 0; i < 8; i++) {
		psw_write(itd, i, (uint16_t) (i < packets
		    ? (PSW_NOT_ACCESSED | (offsets[i] & 0x1fff))
		    : 0));
	}
}

static void
make_ed(uint32_t ed, uint32_t head, uint32_t tail, unsigned address,
        unsigned endpoint, uint32_t direction, unsigned max_packet)
{
	ram_write32(ed + 0, (address & 0x7f) | ((endpoint & 0xf) << 7) |
	    direction | ED_FORMAT_ISO | (max_packet << 16));
	ram_write32(ed + 4, tail);
	ram_write32(ed + 8, head);
	ram_write32(ed + 12, 0);
}

/** Start the controller, with one device on port 1 at address 3. */
static void
controller_up(void)
{
	unsigned i;

	memset(test_ram, 0, sizeof(test_ram));
	memset(&streamer, 0, sizeof(streamer));

	streamer_device.ops = &streamer_ops;
	streamer_device.state = &streamer;
	streamer_device.address = 0;
	streamer_device.low_speed = 0;

	usb_ohci_init(NULL);
	usb_ohci_reset();

	usb_ohci_write(HcHCCA, HCCA_AT);
	usb_ohci_write(HcControl, OHCI_HCFS_OPERATIONAL | OHCI_PLE);
	usb_ohci_write(HcInterruptEnable, 0x80000000u | OHCI_WDH);

	if (usb_ohci_attach(0, &streamer_device) != 0) {
		printf("FAIL: could not attach the test device\n");
		exit(2);
	}

	/* Power and enable the port, as a driver does before using it, then let
	   the connection finish being debounced. */
	usb_ohci_write(HcRhStatus, 1u << 16);
	usb_ohci_write(HcRhPortStatus1, (1u << 8) | (1u << 1));

	for (i = 0; i < 200; i++) {
		usb_ohci_frame();
	}

	/* The address the descriptors will name. Set directly: the control
	   transfer that would do it is not what this test is about. */
	streamer_device.address = 3;
}

/** Put an endpoint on the periodic list, for every one of the 32 entries. */
static void
schedule(uint32_t ed)
{
	unsigned i;

	for (i = 0; i < 32; i++) {
		ram_write32(HCCA_AT + i * 4, ed);
	}
}

/** Run frames until the descriptor is retired, or give up. */
static int
run_until_done(uint32_t itd, unsigned limit)
{
	unsigned i;

	for (i = 0; i < limit; i++) {
		usb_ohci_frame();

		if ((ram_read32(HCCA_AT + 0x84) & ~3u) == itd) {
			return 1;
		}
	}

	return 0;
}

static unsigned
current_frame(void)
{
	return usb_ohci_read(HcFmNumber) & 0xffff;
}

/* ---- the tests ------------------------------------------------------------ */

/**
 * One descriptor of eight packets, read from a device that always has data.
 *
 * The heart of it: a packet per frame, each one written where its own offset
 * says and reported with its own length, and the descriptor retired to the done
 * queue once its last frame has gone by.
 */
static void
test_eight_packets_in(void)
{
	unsigned offsets[8];
	unsigned i;
	uint32_t flags;

	printf("\n-- eight packets in, one per frame --\n");

	controller_up();
	streamer.send_len = 16;

	for (i = 0; i < 8; i++) {
		offsets[i] = i * 16;
	}

	make_itd(ITD_AT, TAIL_AT, current_frame() + 2, 8, BUFFER_AT,
	    BUFFER_AT + 8 * 16 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 16);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40), "the descriptor reaches the done queue");

	flags = ram_read32(ITD_AT + 0);
	check_eq(flags >> 28, 0, "the descriptor reports no error");
	check_eq(streamer.packets_sent, 8, "the device was asked eight times");

	for (i = 0; i < 8; i++) {
		const uint16_t psw = psw_read(ITD_AT, i);
		char what[64];

		snprintf(what, sizeof(what), "packet %u reports 16 bytes", i);
		check_eq(psw & 0x0fff, 16, what);

		snprintf(what, sizeof(what), "packet %u reports no error", i);
		check_eq(psw >> 12, 0, what);

		/* And landed where its offset said, with the contents that say which
		   packet it was. */
		snprintf(what, sizeof(what), "packet %u is at its own offset", i);
		check_eq(test_ram[BUFFER_AT + i * 16], i + 1, what);
	}
}

/**
 * A device with nothing to say.
 *
 * Isochronous has no handshake, so a frame in which the device sent nothing is a
 * packet of no bytes and not a reason to come back to the descriptor. Getting
 * this wrong wedges the endpoint: the queue would never advance, and a driver
 * would wait for a descriptor that is never returned.
 */
static void
test_silence_is_a_packet(void)
{
	unsigned offsets[4];
	unsigned i;

	printf("\n-- a silent device still fills its frames --\n");

	controller_up();
	streamer.nak = 1;

	for (i = 0; i < 4; i++) {
		offsets[i] = i * 8;
	}

	make_itd(ITD_AT, TAIL_AT, current_frame() + 2, 4, BUFFER_AT,
	    BUFFER_AT + 4 * 8 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 8);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40),
	    "a descriptor whose device said nothing is still retired");
	check_eq(ram_read32(ITD_AT + 0) >> 28, 0, "and reports no error");

	for (i = 0; i < 4; i++) {
		const uint16_t psw = psw_read(ITD_AT, i);
		char what[64];

		snprintf(what, sizeof(what), "packet %u reports no bytes", i);
		check_eq(psw & 0x0fff, 0, what);

		snprintf(what, sizeof(what), "packet %u reports no error", i);
		check_eq(psw >> 12, 0, what);
	}
}

/**
 * A buffer whose two pages are not next to each other.
 *
 * The case a subtraction gets wrong. A descriptor's buffer is two pages and the
 * offsets run across both of them, but the pages are wherever the guest's memory
 * manager put them - so the second page is found through BufferEnd and not by
 * adding to the first. Here the second page is deliberately placed *below* the
 * first, which no amount of adding can reach.
 */
static void
test_buffer_across_two_pages(void)
{
	const uint32_t page0 = 0x5000;
	const uint32_t page1 = 0x3000;		/* below page0, on purpose */
	unsigned offsets[4];
	unsigned i;

	printf("\n-- a buffer split across two unrelated pages --\n");

	controller_up();
	streamer.send_len = 32;

	/*
	 * Four packets of 32 bytes starting 48 bytes before the end of the first
	 * page: the first fits, the second straddles the boundary, and the last two
	 * are wholly in the second page.
	 */
	for (i = 0; i < 4; i++) {
		offsets[i] = (0x1000 - 48) + i * 32;
	}

	make_itd(ITD_AT, TAIL_AT, current_frame() + 2, 4, page0,
	    page1 + (offsets[3] + 32 - 1 - 0x1000), offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 32);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40), "the descriptor reaches the done queue");

	for (i = 0; i < 4; i++) {
		char what[80];

		snprintf(what, sizeof(what), "packet %u reports 32 bytes", i);
		check_eq(psw_read(ITD_AT, i) & 0x0fff, 32, what);
	}

	/* The first packet is wholly in the first page. */
	check_eq(test_ram[page0 + 0x1000 - 48], 1, "packet 0 is in the first page");

	/* The second starts 16 bytes before the end of the first page and finishes
	   in the second, which is the crossing the code has to get right. */
	check_eq(test_ram[page0 + 0x1000 - 16], 2,
	    "packet 1 starts at the end of the first page");
	check_eq(test_ram[page1], 2, "packet 1 continues into the second page");

	/* And the two after it are in the second page, below where the first is. */
	check_eq(test_ram[page1 + 16], 3, "packet 2 is in the second page");
	check_eq(test_ram[page1 + 48], 4, "packet 3 is in the second page");
}

/**
 * A device that sends less than the packet had room for.
 *
 * Normal for a stream rather than an error: a camera's last packet of a frame is
 * whatever is left. The length has to be what arrived, because that is what the
 * driver adds up to find how much of the buffer is worth looking at.
 */
static void
test_short_packets(void)
{
	unsigned offsets[4];
	unsigned i;

	printf("\n-- packets shorter than the room allowed --\n");

	controller_up();
	streamer.send_len = 5;

	for (i = 0; i < 4; i++) {
		offsets[i] = i * 64;
	}

	make_itd(ITD_AT, TAIL_AT, current_frame() + 2, 4, BUFFER_AT,
	    BUFFER_AT + 4 * 64 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 64);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40), "the descriptor reaches the done queue");
	check_eq(ram_read32(ITD_AT + 0) >> 28, 0,
	    "a short packet is not an error on a stream");

	for (i = 0; i < 4; i++) {
		char what[64];

		snprintf(what, sizeof(what), "packet %u reports the 5 bytes sent", i);
		check_eq(psw_read(ITD_AT, i) & 0x0fff, 5, what);
	}
}

/**
 * A stream out, to a device that is listening.
 *
 * The other direction, where the lengths come from the descriptor rather than
 * from the device, and the controller is the one doing the reading.
 */
static void
test_stream_out(void)
{
	unsigned offsets[4];
	unsigned i;

	printf("\n-- a stream out --\n");

	controller_up();

	for (i = 0; i < 4; i++) {
		offsets[i] = i * 10;
	}

	/* Something recognisable in the buffer for the device to receive. */
	for (i = 0; i < 40; i++) {
		test_ram[BUFFER_AT + i] = (uint8_t) (0xa0 + i);
	}

	make_itd(ITD_AT, TAIL_AT, current_frame() + 2, 4, BUFFER_AT,
	    BUFFER_AT + 40 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 2, ED_DIR_OUT, 10);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40), "the descriptor reaches the done queue");
	check_eq(streamer.writes, 4, "the device was written to four times");

	for (i = 0; i < 4; i++) {
		char what[64];

		snprintf(what, sizeof(what), "write %u was 10 bytes", i);
		check_eq(streamer.written[i], 10, what);
	}

	/* The last packet's bytes, which say it was read from the right place. */
	check_eq(streamer.last_write[0], 0xa0 + 30, "the last packet's first byte");
	check_eq(streamer.last_write[9], 0xa0 + 39, "the last packet's last byte");
}

/**
 * A refusing device.
 *
 * The endpoint must not be halted. A general descriptor that fails halts its
 * endpoint so the driver can deal with it, but a stream that stops at the first
 * bad packet loses every packet after it too - and the driver is told which
 * packet went wrong regardless, through the descriptor's own condition code.
 */
static void
test_error_does_not_halt_the_endpoint(void)
{
	unsigned offsets[2];
	unsigned i;

	printf("\n-- an error on a stream leaves the endpoint running --\n");

	controller_up();
	streamer.stall = 1;

	for (i = 0; i < 2; i++) {
		offsets[i] = i * 8;
	}

	make_itd(ITD_AT, ITD2_AT, current_frame() + 2, 2, BUFFER_AT,
	    BUFFER_AT + 16 - 1, offsets);
	make_itd(ITD2_AT, TAIL_AT, current_frame() + 6, 2, BUFFER_AT + 64,
	    BUFFER_AT + 64 + 16 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 8);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 40), "the failed descriptor is retired");
	check_eq(ram_read32(ITD_AT + 0) >> 28, 4, "and reports the stall");
	check_eq(psw_read(ITD_AT, 0) >> 12, 4, "as does its first packet");
	check_eq(ram_read32(ED_AT + 8) & 1, 0, "the endpoint is not halted");

	/* And the queue has moved on to the next descriptor rather than stopping. */
	check_eq(ram_read32(ED_AT + 8) & ~3u, ITD2_AT,
	    "the queue has moved on to the next descriptor");
}

/**
 * A descriptor scheduled for frames that have already gone.
 *
 * It has to be retired rather than left at the head of the queue: the frames it
 * asked for cannot be replayed, and a descriptor that is never returned is a
 * driver that waits for ever. Its packets keep the NotAccessed the driver put
 * in them, which is how it learns nothing was moved.
 */
static void
test_late_descriptor_is_retired(void)
{
	unsigned offsets[2];
	unsigned i;

	printf("\n-- a descriptor whose frames have gone --\n");

	controller_up();
	streamer.send_len = 8;

	for (i = 0; i < 2; i++) {
		offsets[i] = i * 8;
	}

	/* Twenty frames in the past. */
	make_itd(ITD_AT, TAIL_AT, (current_frame() - 20) & 0xffff, 2, BUFFER_AT,
	    BUFFER_AT + 16 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 8);
	schedule(ED_AT);

	check(run_until_done(ITD_AT, 10), "a late descriptor is retired at once");
	check_eq(psw_read(ITD_AT, 0) >> 12 & 0xe, 0xe,
	    "its packets are left as not accessed");
	check_eq(streamer.packets_sent, 0, "and the device was not asked");
}

/**
 * A descriptor whose frames are still to come is left alone.
 *
 * The other half of the frame arithmetic. Frame numbers are sixteen bits and
 * wrap, so a descriptor scheduled just after a wrap must not be mistaken for one
 * scheduled 65535 frames ago and run immediately.
 */
static void
test_future_descriptor_waits(void)
{
	unsigned offsets[2];
	unsigned i;

	printf("\n-- a descriptor whose frames are still to come --\n");

	controller_up();
	streamer.send_len = 8;

	for (i = 0; i < 2; i++) {
		offsets[i] = i * 8;
	}

	make_itd(ITD_AT, TAIL_AT, (current_frame() + 100) & 0xffff, 2, BUFFER_AT,
	    BUFFER_AT + 16 - 1, offsets);
	make_ed(ED_AT, ITD_AT, TAIL_AT, 3, 1, ED_DIR_IN, 8);
	schedule(ED_AT);

	for (i = 0; i < 20; i++) {
		usb_ohci_frame();
	}

	check_eq(streamer.packets_sent, 0, "the device is not asked early");
	check_eq(ram_read32(ED_AT + 8) & ~3u, ITD_AT,
	    "and the descriptor is still at the head of the queue");
	check(psw_read(ITD_AT, 0) == (PSW_NOT_ACCESSED | 0),
	    "with its offsets untouched");
}

int
main(void)
{
	printf("OHCI isochronous transfer descriptors\n");

	test_eight_packets_in();
	test_silence_is_a_packet();
	test_buffer_across_two_pages();
	test_short_packets();
	test_stream_out();
	test_error_does_not_halt_the_endpoint();
	test_late_descriptor_is_retired();
	test_future_descriptor_waits();

	printf("\n%s\n", failures == 0 ? "all checks passed"
	    : "there were failures");

	return failures == 0 ? 0 : 1;
}
