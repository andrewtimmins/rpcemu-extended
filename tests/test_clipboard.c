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
 * test_clipboard.c - the shared clipboard's host half
 *
 * The interesting part for text is the conversion: the host works in UTF-8 and
 * the guest in whatever alphabet RISC OS is configured for, and the mapping
 * between them comes from a table the guest hands over. Get that wrong and text
 * arrives mangled or truncated, which is the sort of thing that is tedious to
 * chase through a running emulator and quick to pin down here.
 *
 * For an image the interesting part is that nothing happens to it: it is carried
 * as the encoded PNG or JPEG, so the bytes must come back exactly as they went
 * in - which the text path, terminating at a NUL and converting through the
 * alphabet, would not manage.
 *
 * The unit is included directly and the emulated memory stubbed, so this links
 * nothing.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"

/* ---- the little of the emulator the clipboard touches ---- */

#define FAKE_MEM_SIZE 4096
static uint8_t fake_mem[FAKE_MEM_SIZE];
static uint32_t last_pollword_addr;
static uint32_t last_pollword_value;

Config config;
Machine machine;

void
rpclog(const char *format, ...)
{
	(void) format;
}

void
fatal(const char *format, ...)
{
	(void) format;
	exit(1);
}

/* Emulated memory, as a flat array with addresses as offsets. */
void
memcpytohost(void *dest, uint32_t src, uint32_t len)
{
	memcpy(dest, fake_mem + (src % FAKE_MEM_SIZE), len);
}

void
memcpyfromhost(uint32_t dest, const void *src, uint32_t len)
{
	memcpy(fake_mem + (dest % FAKE_MEM_SIZE), src, len);
}

/* mem.h defines mem_write32() as a static inline over the emulated memory map,
   which needs a machine behind it. Include the header first so its own
   definition stands, then redirect the one call the clipboard makes. */
#include "mem.h"

static void
test_pollword_write(uint32_t addr, uint32_t val)
{
	last_pollword_addr = addr;
	last_pollword_value = val;
}

#define mem_write32(a, v) test_pollword_write((a), (v))

#include "hostclipboard.c"

/* ---- what the front end would be handed ---- */

static char host_text[1024];
static unsigned int host_text_len;
static int host_setter_calls;

static void
fake_host_setter(const char *utf8, unsigned int len)
{
	host_setter_calls++;
	host_text_len = len < sizeof(host_text) - 1 ? len : sizeof(host_text) - 1;
	memcpy(host_text, utf8, host_text_len);
	host_text[host_text_len] = '\0';
}

static unsigned char host_image[1024];
static unsigned int host_image_len;
static int host_image_type;
static int host_image_setter_calls;

static void
fake_host_image_setter(int file_type, const void *data, unsigned int len)
{
	host_image_setter_calls++;
	host_image_type = file_type;
	host_image_len = len < sizeof(host_image) ? len : (unsigned int) sizeof(host_image);
	memcpy(host_image, data, host_image_len);
}

static int failures;

static void
check(int cond, const char *what)
{
	printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

/* The guest's table, as its module passes it: Latin-1, with 0x80-0x9f being
   RISC OS's own additions and 0xffffffff meaning "no equivalent". */
static void
setup_guest(uint32_t table_addr, uint32_t pollword_addr)
{
	unsigned int i;

	for (i = 0; i < 256; i++) {
		uint32_t ucs = i;

		if (i == 0x8c) {
			ucs = 0x2026;		/* horizontal ellipsis */
		} else if (i == 0x83) {
			ucs = 0xffffffffu;	/* nothing here */
		}
		memcpy(fake_mem + table_addr + i * 4, &ucs, 4);
	}

	uint32_t r0 = 0, r1 = 0;
	clipboard_swi(ARCEM_SWI_CLIPBOARD_SETUP, pollword_addr, 101, table_addr,
	              0, 0, &r0, &r1);
}

/** How much the guest would be told there is, and of what type. */
static void
host_check(uint32_t *len, uint32_t *type)
{
	clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_CHECK, 0, 0, 0, 0, 0, len, type);
}

/** Fetch into emulated memory at addr, returning what the guest would see. */
static const char *
host_get(uint32_t addr, uint32_t room)
{
	uint32_t r0 = 0, r1 = 0;

	clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_GET, addr, room, 0, 0, 0, &r0, &r1);
	return (const char *) (fake_mem + addr);
}

int
main(void)
{
	const uint32_t table_addr = 0x400;
	const uint32_t pollword_addr = 0x100;
	const uint32_t buf_addr = 0x900;
	uint32_t len = 0, type = 0;

	config.clipboard_enabled = 1;
	clipboard_set_host_setter(fake_host_setter);
	clipboard_set_host_image_setter(fake_host_image_setter);

	printf("setup\n");
	setup_guest(table_addr, pollword_addr);
	check(pollword_addr == 0x100, "the guest's pollword address is noted");

	printf("\nhost to guest\n");
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "Hello", 5);
	host_check(&len, &type);
	check(len == 6, "five characters are offered as five plus a terminator");
	check(type == CLIPBOARD_TYPE_TEXT, "offered as text");
	check(last_pollword_addr == pollword_addr &&
	      last_pollword_value == CLIPBOARD_POLLWORD_HOST_CHANGED,
	      "the guest's pollword is set");
	check(strcmp(host_get(buf_addr, 64), "Hello") == 0, "the text arrives");

	printf("\nhost to guest, through the guest's own alphabet\n");
	/* U+2026 is 0x8c in RISC OS Latin-1, and there is no Latin-1 for U+4e2d. */
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "a\xe2\x80\xa6z", 5);
	check(strcmp(host_get(buf_addr, 64), "a\x8c" "z") == 0,
	      "an ellipsis becomes the character RISC OS uses for it");
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "a\xe4\xb8\xadz", 5);
	check(strcmp(host_get(buf_addr, 64), "az") == 0,
	      "a character the guest has no equivalent for is dropped");

	printf("\nhost to guest, into a buffer that is too small\n");
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "0123456789", 10);
	check(strcmp(host_get(buf_addr, 5), "0123") == 0,
	      "truncated to fit, still terminated");

	printf("\nguest to host\n");
	host_setter_calls = 0;
	memcpy(fake_mem + buf_addr, "From RISC OS", 12);
	{
		uint32_t r0 = 0, r1 = 0;

		clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_SET, buf_addr, 12,
		              CLIPBOARD_TYPE_TEXT, 0, 0, &r0, &r1);
	}
	check(host_setter_calls == 1, "the front end is told once");
	check(strcmp(host_text, "From RISC OS") == 0, "the text arrives as UTF-8");

	/* 0x8c is the ellipsis, which is three bytes of UTF-8. */
	memcpy(fake_mem + buf_addr, "a\x8c" "z", 3);
	{
		uint32_t r0 = 0, r1 = 0;

		clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_SET, buf_addr, 3,
		              CLIPBOARD_TYPE_TEXT, 0, 0, &r0, &r1);
	}
	check(strcmp(host_text, "a\xe2\x80\xa6z") == 0,
	      "a RISC OS character becomes its UTF-8 equivalent");

	memcpy(fake_mem + buf_addr, "a\x83" "z", 3);
	{
		uint32_t r0 = 0, r1 = 0;

		clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_SET, buf_addr, 3,
		              CLIPBOARD_TYPE_TEXT, 0, 0, &r0, &r1);
	}
	check(strcmp(host_text, "az") == 0,
	      "a character the guest's table has no mapping for is dropped");

	printf("\nimages\n");
	/* An image is carried as the encoded file, byte for byte: nothing here
	   decodes it, so a PNG with a NUL and a high byte in it must survive
	   unaltered - which the text path, being NUL-terminated and converted
	   through the alphabet table, would not manage. */
	{
		static const unsigned char png[] = {
			0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0xff, 0x41
		};
		const unsigned int png_len = (unsigned int) sizeof(png);
		uint32_t r0 = 0, r1 = 0;

		host_setter_calls = 0;
		host_image_setter_calls = 0;
		memcpy(fake_mem + buf_addr, png, png_len);
		clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_SET, buf_addr, png_len,
		              CLIPBOARD_TYPE_PNG, 0, 0, &r0, &r1);

		check(host_image_setter_calls == 1 && host_setter_calls == 0,
		      "an image from the guest goes to the front end as an image");
		check(host_image_type == CLIPBOARD_TYPE_PNG &&
		      host_image_len == png_len &&
		      memcmp(host_image, png, png_len) == 0,
		      "and arrives byte for byte, NULs and all");

		host_check(&len, &type);
		check(len == png_len && type == CLIPBOARD_TYPE_PNG,
		      "the guest is offered its exact length, with no terminator");

		memset(fake_mem + buf_addr, 0, 64);
		host_get(buf_addr, 64);
		check(memcmp(fake_mem + buf_addr, png, png_len) == 0,
		      "and fetches it back unchanged");

		/* An image and text are never both live: whichever arrived last is
		   what the clipboard holds. */
		clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "back to text", 12);
		host_check(&len, &type);
		check(type == CLIPBOARD_TYPE_TEXT,
		      "text from the host replaces the image");

		clipboard_host_changed(CLIPBOARD_TYPE_PNG, (const char *) png, png_len);
		host_check(&len, &type);
		check(len == png_len && type == CLIPBOARD_TYPE_PNG,
		      "and an image from the host replaces the text");

		/* A JPEG is carried the same way; anything else is not carried. */
		clipboard_host_changed(CLIPBOARD_TYPE_JPEG, (const char *) png, png_len);
		host_check(&len, &type);
		check(type == CLIPBOARD_TYPE_JPEG, "a JPEG is carried too");

		clipboard_host_changed(0xff9, (const char *) png, png_len);
		host_check(&len, &type);
		check(type == CLIPBOARD_TYPE_JPEG,
		      "a sprite is not, and leaves what was there alone");

		host_image_setter_calls = 0;
		memcpy(fake_mem + buf_addr, png, png_len);
		clipboard_swi(ARCEM_SWI_CLIPBOARD_HOST_SET, buf_addr, png_len,
		              0xff9, 0, 0, &r0, &r1);
		check(host_image_setter_calls == 0,
		      "nor is a sprite from the guest");

		/* Leave text on it, as the checks below expect. */
		clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "Hello", 5);
	}

	printf("\nwhen the feature is off\n");
	config.clipboard_enabled = 0;
	host_setter_calls = 0;
	host_check(&len, &type);
	check(len == 0 && type == 0, "nothing is offered to the guest");
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "ignored", 7);
	check(host_setter_calls == 0, "and nothing is taken from the host");

	printf("\nafter a reset\n");
	config.clipboard_enabled = 1;
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "Kept", 4);
	clipboard_reset();
	host_check(&len, &type);
	check(len == 5, "what is on the clipboard is kept, for the next machine");
	last_pollword_addr = 0;
	clipboard_host_changed(CLIPBOARD_TYPE_TEXT, "Hello again", 11);
	check(last_pollword_addr == 0,
	      "but nothing is written to guest memory until one sets up again");

	printf("\nwhen a guest starts with the host holding text\n");
	last_pollword_addr = 0;
	setup_guest(table_addr, pollword_addr);
	check(last_pollword_addr == pollword_addr &&
	      last_pollword_value == CLIPBOARD_POLLWORD_HOST_CHANGED,
	      "it is told at once, so it starts up in step");
	check(strcmp(host_get(buf_addr, 64), "Hello again") == 0,
	      "and can fetch what was already there");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");

	return failures ? 1 : 0;
}
