/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2021 RiscOS Cloverleaf
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
 * Shared clipboard: the host half.
 *
 * Derived from src/hostclipboard.c in RiscOS Cloverleaf's RPCEmu fork
 * (https://github.com/riscoscloverleaf/rpcemu), whose interface and design this
 * keeps: see hostclipboard.h. Text is held here as UCS-4, converted through the
 * conversion table the guest hands over at setup, which is how the guest's own
 * alphabet is honoured rather than assumed.
 *
 * Text only at the moment. The filetype travels with the data, so images can be
 * added later without the guest module or this interface changing.
 */

#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "hostclipboard.h"
#include "mem.h"
#include "network.h"	/* memcpytohost() / memcpyfromhost() */

/* Where in guest memory to set the "host clipboard changed" bit, as the guest
   module told us. Zero when no module has set up (or after a reset). */
static uint32_t pollword_addr;

/* The guest's alphabet, and its 256-entry UCS-4 conversion table. */
static uint32_t guest_alphabet;
static uint32_t guest_ucstable[256];
static int have_ucstable;

/* Where guest copies go, once a front end has said it wants them. */
static clipboard_host_setter host_setter;

/* The host's clipboard, as UCS-4 for text. */
static uint32_t *clip_ucs;
static unsigned int clip_ucs_len;	/* in characters */
static int clip_file_type = CLIPBOARD_TYPE_TEXT;

/**
 * Convert one UCS-4 character to the guest's alphabet, or 0 if the guest has no
 * character for it.
 */
static unsigned char
ucs_to_guest(uint32_t ucs)
{
	unsigned int i;

	if (!have_ucstable) {
		/* No table yet: assume Latin-1, which is what RISC OS uses by
		   default and what the low 256 of UCS-4 already is. */
		return (ucs < 0x100) ? (unsigned char) ucs : 0;
	}

	for (i = 0; i < 256; i++) {
		if (guest_ucstable[i] == ucs) {
			return (unsigned char) i;
		}
	}
	return 0;
}

/**
 * Convert one character in the guest's alphabet to UCS-4, or 0xffffffff if the
 * guest's table says there is no equivalent.
 */
static uint32_t
guest_to_ucs(unsigned char c)
{
	if (!have_ucstable) {
		return c;
	}
	return guest_ucstable[c];
}

/**
 * Replace the host clipboard with UTF-8 text from the front end.
 */
static void
clip_set_from_utf8(const char *utf8, unsigned int len)
{
	uint32_t *ucs;
	unsigned int i = 0;
	unsigned int out = 0;

	ucs = malloc((len + 1) * sizeof(uint32_t));
	if (ucs == NULL) {
		rpclog("Clipboard: out of memory for %u bytes of text\n", len);
		return;
	}

	/* Decode UTF-8. Anything malformed is passed through as a single byte,
	   which keeps plain Latin-1 working if it ever reaches us. */
	while (i < len) {
		const unsigned char c = (unsigned char) utf8[i];
		uint32_t ch;
		unsigned int extra;

		if (c < 0x80) {
			ch = c;
			extra = 0;
		} else if ((c & 0xe0) == 0xc0) {
			ch = c & 0x1fu;
			extra = 1;
		} else if ((c & 0xf0) == 0xe0) {
			ch = c & 0x0fu;
			extra = 2;
		} else if ((c & 0xf8) == 0xf0) {
			ch = c & 0x07u;
			extra = 3;
		} else {
			ch = c;
			extra = 0;
		}

		i++;
		while (extra-- > 0 && i < len && ((unsigned char) utf8[i] & 0xc0) == 0x80) {
			ch = (ch << 6) | ((unsigned char) utf8[i] & 0x3fu);
			i++;
		}

		ucs[out++] = ch;
	}
	ucs[out] = 0;

	free(clip_ucs);
	clip_ucs = ucs;
	clip_ucs_len = out;
	clip_file_type = CLIPBOARD_TYPE_TEXT;
}

void
clipboard_set_host_setter(clipboard_host_setter setter)
{
	host_setter = setter;
}

void
clipboard_reset(void)
{
	pollword_addr = 0;
	have_ucstable = 0;
	guest_alphabet = 0;
	free(clip_ucs);
	clip_ucs = NULL;
	clip_ucs_len = 0;
}

void
clipboard_host_changed(int file_type, const char *data, unsigned int data_len)
{
	if (!config.clipboard_enabled) {
		return;
	}
	if (file_type != CLIPBOARD_TYPE_TEXT) {
		return;		/* text only, for now */
	}

	clip_set_from_utf8(data, data_len);

	/* Wake the guest's task, if one has told us where to knock. */
	if (pollword_addr != 0) {
		mem_write32(pollword_addr, CLIPBOARD_POLLWORD_HOST_CHANGED);
	}
}

void
clipboard_swi(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
              uint32_t r4, uint32_t r5, uint32_t *retr0, uint32_t *retr1)
{
	NOT_USED(r4);
	NOT_USED(r5);

	*retr0 = 0;
	*retr1 = 0;

	if (!config.clipboard_enabled) {
		return;
	}

	switch (r0) {
	case ARCEM_SWI_CLIPBOARD_SETUP:
		/* r1 = pollword address (0 to withdraw), r2 = alphabet,
		   r3 = the guest's 256-entry UCS-4 conversion table. */
		pollword_addr = r1;
		guest_alphabet = r2;
		if (r1 != 0 && r3 != 0) {
			memcpytohost(guest_ucstable, r3, sizeof(guest_ucstable));
			have_ucstable = 1;
			rpclog("Clipboard: guest task ready, alphabet %u, pollword at 0x%08x\n",
			       (unsigned) guest_alphabet, (unsigned) pollword_addr);
		} else {
			have_ucstable = 0;
			rpclog("Clipboard: guest task gone\n");
		}
		break;

	case ARCEM_SWI_CLIPBOARD_HOST_CHECK:
		/* How much is there to fetch, and of what type? Text is counted
		   as the guest will see it: one byte per character, plus a
		   terminator, as Cloverleaf's module expects. */
		if (clip_ucs != NULL && clip_ucs_len > 0) {
			*retr0 = clip_ucs_len + 1;
			*retr1 = (uint32_t) clip_file_type;
		}
		break;

	case ARCEM_SWI_CLIPBOARD_HOST_GET: {
		/* r1 = where to put it, r2 = how much room there is. */
		unsigned char *out;
		unsigned int room = r2;
		unsigned int i;
		unsigned int len = 0;

		if (clip_ucs == NULL || room == 0) {
			break;
		}
		out = malloc(room);
		if (out == NULL) {
			rpclog("Clipboard: out of memory for %u bytes\n", room);
			break;
		}
		for (i = 0; i < clip_ucs_len && len + 1 < room; i++) {
			const unsigned char c = ucs_to_guest(clip_ucs[i]);

			if (c != 0) {
				out[len++] = c;
			}
		}
		out[len] = '\0';
		memcpyfromhost(r1, out, len + 1);
		free(out);
		*retr0 = len;
		break;
	}

	case ARCEM_SWI_CLIPBOARD_HOST_SET: {
		/* The guest is putting something on the clipboard:
		   r1 = data, r2 = length, r3 = filetype. */
		unsigned char *in;
		uint32_t *ucs;
		unsigned int i;
		unsigned int out = 0;

		if (r3 != CLIPBOARD_TYPE_TEXT || r2 == 0) {
			break;		/* text only, for now */
		}
		in = malloc(r2);
		ucs = malloc((r2 + 1) * sizeof(uint32_t));
		if (in == NULL || ucs == NULL) {
			free(in);
			free(ucs);
			rpclog("Clipboard: out of memory for %u bytes from the guest\n",
			       (unsigned) r2);
			break;
		}
		memcpytohost(in, r1, r2);

		for (i = 0; i < r2; i++) {
			const uint32_t ch = guest_to_ucs(in[i]);

			if (ch != 0xffffffffu) {
				ucs[out++] = ch;
			}
		}
		ucs[out] = 0;
		free(in);

		free(clip_ucs);
		clip_ucs = ucs;
		clip_ucs_len = out;
		clip_file_type = CLIPBOARD_TYPE_TEXT;

		/* Hand it to the front end as UTF-8, which is what every host
		   clipboard we deal with wants. */
		{
			char *utf8 = malloc(out * 4 + 1);
			unsigned int len = 0;

			if (utf8 != NULL) {
				for (i = 0; i < out; i++) {
					const uint32_t ch = clip_ucs[i];

					if (ch < 0x80) {
						utf8[len++] = (char) ch;
					} else if (ch < 0x800) {
						utf8[len++] = (char) (0xc0 | (ch >> 6));
						utf8[len++] = (char) (0x80 | (ch & 0x3f));
					} else if (ch < 0x10000) {
						utf8[len++] = (char) (0xe0 | (ch >> 12));
						utf8[len++] = (char) (0x80 | ((ch >> 6) & 0x3f));
						utf8[len++] = (char) (0x80 | (ch & 0x3f));
					} else {
						utf8[len++] = (char) (0xf0 | (ch >> 18));
						utf8[len++] = (char) (0x80 | ((ch >> 12) & 0x3f));
						utf8[len++] = (char) (0x80 | ((ch >> 6) & 0x3f));
						utf8[len++] = (char) (0x80 | (ch & 0x3f));
					}
				}
				utf8[len] = '\0';
				if (host_setter != NULL) {
					host_setter(utf8, len);
				}
				free(utf8);
			}
		}
		break;
	}

	default:
		rpclog("Clipboard: unknown reason code %u\n", (unsigned) r0);
		break;
	}
}
