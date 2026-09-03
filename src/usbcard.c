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
 * usbcard.c - the USB expansion card.
 *
 * An OHCI host controller and a ROM to carry the module that tells RISC OS
 * about it. A card of its own rather than part of the support card, because it
 * needs that ROM and because an expansion card with one interrupt and one thing
 * raising it is a great deal easier to reason about - the previous arrangement,
 * where the USB controller shared the support card with the scroll wheel, wedged
 * machines outright.
 *
 * Which slot it lands in no longer matters. The card's ROM carries RISC OS's own
 * USB stack - USBDriver and OHCIDriver - and a card ROM starts the modules it
 * carries with r11 pointing at the card, so OHCIDriver drives the card it came
 * out of rather than a slot number somebody wrote down. The support card has its
 * historical slot 0 back.
 *
 * The stock OHCIDriver cannot do that: it finds controllers by asking the HAL
 * what USB hardware exists, and the IOMD HAL has no USB support at all. See
 * docs/usb.md for the patch that adds the expansion card case.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rpcemu.h"
#include "support_files.h"
#include "podules.h"
#include "usbcard.h"
#include "usb_ohci.h"

/* As many module files as the card will carry. */
#define USBCARD_MAX_ROMS	16

static char romfns[USBCARD_MAX_ROMS][256];

/** Order two ROM file names, for qsort(). */
static int
usbcard_romfn_compare(const void *a, const void *b)
{
	return strcmp((const char *) a, (const char *) b);
}

static uint8_t *cardrom = NULL;
static uint32_t cardromsize = 0;
static uint32_t chunkbase;
static uint32_t filebase;

const char usbcard_description[] = "RPCEmu USB";

/*
 * A product type of our own, so the card is identifiable even to something that
 * does not read the description chunk. The graphics card does the same; leaving
 * it zero is what makes a card show up as "Extended type &0000".
 */
#define USBCARD_PRODUCT_TYPE	0x0002

static podule *self = NULL;

/**
 * The card's interrupt status byte, as the guest reads it.
 *
 * One bit, and every other bit zero. That matters more than it looks: RISC OS
 * claims a podule interrupt by handing the kernel this address and a mask, and
 * the kernel calls the handler whenever the masked bits are set. The support
 * card answers &fa in the bits around its flag, which is harmless only for as
 * long as nothing takes its interrupt declaration seriously.
 */
#define USBCARD_IRQ_BIT		0x01

/* --- ROM ---------------------------------------------------------------- */

/** Add one entry to the ROM's chunk directory. */
static void
makechunk(uint8_t type, uint32_t base, uint32_t size)
{
	cardrom[chunkbase++] = type;
	cardrom[chunkbase++] = (uint8_t) size;
	cardrom[chunkbase++] = (uint8_t) (size >> 8);
	cardrom[chunkbase++] = (uint8_t) (size >> 16);

	cardrom[chunkbase++] = (uint8_t) base;
	cardrom[chunkbase++] = (uint8_t) (base >> 8);
	cardrom[chunkbase++] = (uint8_t) (base >> 16);
	cardrom[chunkbase++] = (uint8_t) (base >> 24);
}

void
usbcard_init(void)
{
	char romdirectory[512];
	DIR *dir;
	const struct dirent *d;
	int files = 0;
	int i;

	snprintf(romdirectory, sizeof(romdirectory), "%susbroms/",
	         support_root_for("usbroms"));

	free(cardrom);
	cardrom = NULL;
	cardromsize = 0;

	/* An empty or absent directory is the normal case today, and not worth
	   complaining about: the card is useful for its controller alone. */
	dir = opendir(romdirectory);
	if (dir != NULL) {
		while ((d = readdir(dir)) != NULL && files < USBCARD_MAX_ROMS) {
			const char *ext = rpcemu_file_get_extension(d->d_name);
			char filepath[512];
			struct stat buf;

			/* Truncation would stat a different path and quietly skip a
			   ROM the user had put there, so it is reported and skipped
			   deliberately instead. */
			if (snprintf(filepath, sizeof(filepath), "%s%s", romdirectory,
			             d->d_name) >= (int) sizeof(filepath)) {
				rpclog("usbcard: path too long, ignoring %s%s\n",
				       romdirectory, d->d_name);
				continue;
			}

			if (stat(filepath, &buf) == 0 && S_ISREG(buf.st_mode) &&
			    strcasecmp(ext, "txt") != 0 && d->d_name[0] != '.') {
				snprintf(romfns[files++], sizeof(romfns[0]), "%s", d->d_name);
			}
		}
		closedir(dir);
	}

	/*
	 * readdir() hands them over in whatever order the host filing system
	 * feels like, and RISC OS starts the modules in a card's ROM in the
	 * order they appear in it. Sorting by name means the same machine comes
	 * up the same way twice running.
	 */
	qsort(romfns, (size_t) files, sizeof(romfns[0]), usbcard_romfn_compare);

	/*
	 * The chunk directory holds one entry per file plus one for the
	 * description, and then a zero entry to end it. Leaving that terminator
	 * out lets the podule manager read the description text as if it were
	 * another entry, and it gives up on the card - which is why it showed as
	 * "Extended type &0000" rather than by name.
	 */
	/* Room for one chunk per file, one for the description, and the word that
	   ends the directory - the same shape the graphics card's ROM has. */
	chunkbase = 0x10;
	filebase = chunkbase + 8 * (files + 1) + 4;
	cardromsize = filebase + ((sizeof(usbcard_description) + 3) & ~3u);
	cardrom = calloc(cardromsize, 1);
	if (cardrom == NULL) {
		fatal("usbcard_init: Out of Memory");
	}

	cardrom[0] = 0;	/* Acorn conformant card, extended EcID */
	cardrom[1] = 3;	/* Interrupt status relocated, chunk directory, byte access */
	cardrom[2] = 0;					/* mandatory */
	cardrom[3] = (uint8_t) USBCARD_PRODUCT_TYPE;
	cardrom[4] = (uint8_t) (USBCARD_PRODUCT_TYPE >> 8);
	cardrom[5] = 0;					/* manufacturer */
	cardrom[6] = 0;
	cardrom[7] = 0;

	cardrom[12] = USBCARD_IRQ_BIT;	/* IRQ status bit mask */

	memcpy(cardrom + filebase, usbcard_description, sizeof(usbcard_description));
	makechunk(0xf5, filebase, sizeof(usbcard_description));	/* Device data, description */
	filebase += (sizeof(usbcard_description) + 3) & ~3u;

	for (i = 0; i < files; i++) {
		char filepath[512];
		FILE *f;
		long len;

		if (snprintf(filepath, sizeof(filepath), "%s%s", romdirectory,
		             romfns[i]) >= (int) sizeof(filepath)) {
			fatal("usbcard_init: path too long: %s%s", romdirectory,
			      romfns[i]);
		}

		f = fopen(filepath, "rb");
		if (f == NULL) {
			fatal("usbcard_init: Can't open '%s': %s", romfns[i],
			      strerror(errno));
		}
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		if (len < 0 || len > 4096 * 1024) {
			fclose(f);
			fatal("usbcard_init: '%s' is not a size a card ROM can carry",
			      romfns[i]);
		}

		cardromsize += ((uint32_t) len + 3) & ~3u;
		if (cardromsize > 4096 * 1024) {
			fclose(f);
			fatal("usbcard_init: more than 4MB of ROM files");
		}
		cardrom = realloc(cardrom, cardromsize);
		if (cardrom == NULL) {
			fclose(f);
			fatal("usbcard_init: Out of Memory");
		}

		fseek(f, 0, SEEK_SET);
		if (fread(cardrom + filebase, 1, (size_t) len, f) != (size_t) len) {
			fclose(f);
			fatal("usbcard_init: Failed to read '%s': %s", romfns[i],
			      strerror(errno));
		}
		fclose(f);

		rpclog("usbcard: loaded '%s' into the USB card's ROM\n", romfns[i]);
		makechunk(0x81, filebase, (uint32_t) len);	/* RISC OS BBC ROM */
		filebase += ((uint32_t) len + 3) & ~3u;
	}

}

/* --- The card in the backplane ------------------------------------------ */

/** True if this EASI address is inside the controller's port window. */
static int
usbcard_is_controller(PoduleIoType io_type, uint32_t addr)
{
	const uint32_t offset = addr & 0x00ffffff;

	return io_type == PODULE_IO_TYPE_EASI &&
	       offset >= USB_OHCI_EASI_OFFSET &&
	       offset < USB_OHCI_EASI_OFFSET + USB_OHCI_EASI_SIZE;
}

static uint8_t
usbcard_read8(podule *p, PoduleIoType io_type, uint32_t addr)
{
	if (io_type == PODULE_IO_TYPE_EASI && cardromsize > 0) {
		addr = (addr & 0x00ffffff) >> 2;
		if (addr < cardromsize) {
			return cardrom[addr];
		}
		return 0x00;
	}

	if (io_type == PODULE_IO_TYPE_IOC) {
		switch (addr & 0x3ffc) {
		case 0:
			/*
			 * This byte does double duty, and the second job is the one
			 * that matters at startup: it is both the card's interrupt
			 * status and the first byte of its expansion card identity.
			 * The podule manager reads it before anything else and decides
			 * from it whether the card has EASI space - which is where the
			 * ROM below is - so returning a bare interrupt flag makes the
			 * card describe itself as nothing at all.
			 */
			return 0xfa | (p->irq ? USBCARD_IRQ_BIT : 0x00);

		default:
			return 0x00;
		}
	}

	return 0xff;
}

static uint32_t
usbcard_read32(podule *p, PoduleIoType io_type, uint32_t addr)
{
	if (usbcard_is_controller(io_type, addr)) {
		return usb_ohci_read((addr & 0x00ffffff) - USB_OHCI_EASI_OFFSET);
	}

	NOT_USED(p);

	/*
	 * Everything else reads as all ones, which is what a card with no word
	 * handler gives and what the support card has always given. The ROM is
	 * read a byte at a time.
	 */
	return 0xffffffff;
}

static void
usbcard_write32(podule *p, PoduleIoType io_type, uint32_t addr, uint32_t val)
{
	NOT_USED(p);

	if (usbcard_is_controller(io_type, addr)) {
		usb_ohci_write((addr & 0x00ffffff) - USB_OHCI_EASI_OFFSET, val);
	}
}

/**
 * Follow the controller's interrupt line.
 *
 * The card has one interrupt and one thing that raises it, which is the whole
 * point of the card being its own.
 */
static void
usbcard_irq_change(int level)
{
	if (self == NULL) {
		return;
	}

	if (level) {
		podule_irq_raise(self);
	} else {
		podule_irq_lower(self);
	}
}

void
usbcard_reset(void)
{
	self = addpodule(usbcard_write32, NULL, NULL,
	                 usbcard_read32, NULL, usbcard_read8,
	                 NULL, NULL);

	usb_ohci_init(usbcard_irq_change);

	/* And plug in whatever this machine's configuration asks for, so a
	   machine started again tomorrow has the same devices in the same ports
	   without anyone having to open the dialogue. */
	usb_ohci_apply_config();
}
