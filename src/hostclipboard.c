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
 * Text and images (PNG or JPEG). The filetype travels with the data, which is
 * what let images be added without the guest module or this interface changing.
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
static clipboard_host_image_setter host_image_setter;

/* What is on the clipboard, held in whichever form suits it: text as UCS-4, so
   it can be converted to whatever alphabet the guest asks for, and an image as
   the encoded file itself, since neither side gains anything from this one
   decoding a PNG only to encode it again. clip_file_type says which of the two
   is live; they are never both set. */
static uint32_t *clip_ucs;
static unsigned int clip_ucs_len;	/* in characters */
static unsigned char *clip_image;
static unsigned int clip_image_len;	/* in bytes */
static int clip_file_type = CLIPBOARD_TYPE_TEXT;

/**
 * Convert what is held to UTF-8, which is the form every host clipboard we deal
 * with wants.
 *
 * @param buf    Where to put it, always terminated when there is room
 * @param buflen Size of buf
 * @return bytes written excluding the terminator, or -1 if what is held is not
 *         text, or if it does not fit
 */
static int
clip_to_utf8(char *buf, unsigned int buflen)
{
	unsigned int len = 0;
	unsigned int i;

	if (clip_ucs == NULL || buflen == 0) {
		return -1;
	}
	for (i = 0; i < clip_ucs_len; i++) {
		const uint32_t ch = clip_ucs[i];
		char enc[4];
		unsigned int n;

		if (ch < 0x80) {
			enc[0] = (char) ch;
			n = 1;
		} else if (ch < 0x800) {
			enc[0] = (char) (0xc0 | (ch >> 6));
			enc[1] = (char) (0x80 | (ch & 0x3f));
			n = 2;
		} else if (ch < 0x10000) {
			enc[0] = (char) (0xe0 | (ch >> 12));
			enc[1] = (char) (0x80 | ((ch >> 6) & 0x3f));
			enc[2] = (char) (0x80 | (ch & 0x3f));
			n = 3;
		} else {
			enc[0] = (char) (0xf0 | (ch >> 18));
			enc[1] = (char) (0x80 | ((ch >> 12) & 0x3f));
			enc[2] = (char) (0x80 | ((ch >> 6) & 0x3f));
			enc[3] = (char) (0x80 | (ch & 0x3f));
			n = 4;
		}
		if (len + n >= buflen) {
			return -1;	/* would not fit, and half a string is no use */
		}
		memcpy(buf + len, enc, n);
		len += n;
	}
	buf[len] = '\0';
	return (int) len;
}

int
clipboard_get_text(char *buf, unsigned int buflen)
{
	if (clipboard_type_is_image(clip_file_type)) {
		return -1;		/* an image is not text */
	}
	return clip_to_utf8(buf, buflen);
}

int
clipboard_get_type(void)
{
	if (clip_image != NULL && clip_image_len > 0) {
		return clip_file_type;
	}
	if (clip_ucs != NULL && clip_ucs_len > 0) {
		return CLIPBOARD_TYPE_TEXT;
	}
	return 0;			/* nothing on it */
}

int
clipboard_type_is_image(int file_type)
{
	return file_type == CLIPBOARD_TYPE_PNG || file_type == CLIPBOARD_TYPE_JPEG;
}

/** Drop whatever is held, so only one of the two forms is ever live. */
static void
clip_discard(void)
{
	free(clip_ucs);
	clip_ucs = NULL;
	clip_ucs_len = 0;
	free(clip_image);
	clip_image = NULL;
	clip_image_len = 0;
}

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

	clip_discard();
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
clipboard_set_host_image_setter(clipboard_host_image_setter setter)
{
	host_image_setter = setter;
}

void
clipboard_reset(void)
{
	/* The pollword address belonged to the machine that registered it, so that
	   must go. What is on the clipboard does not: the front end only passes text
	   on when it changes, so throwing it away here would leave the new machine
	   with nothing until the user copied something again. */
	pollword_addr = 0;
	have_ucstable = 0;
	guest_alphabet = 0;
}

void
clipboard_host_changed(int file_type, const char *data, unsigned int data_len)
{
	if (!config.clipboard_enabled) {
		return;
	}
	if (clipboard_type_is_image(file_type)) {
		unsigned char *image;

		if (data_len == 0) {
			return;
		}
		image = malloc(data_len);
		if (image == NULL) {
			rpclog("Clipboard: out of memory for a %u byte image from the host\n",
			       data_len);
			return;
		}
		memcpy(image, data, data_len);

		clip_discard();
		clip_image = image;
		clip_image_len = data_len;
		clip_file_type = file_type;
	} else if (file_type == CLIPBOARD_TYPE_TEXT) {
		clip_set_from_utf8(data, data_len);
	} else {
		return;		/* nothing else is carried */
	}

	/* Wake the guest's task, if one has told us where to knock. */
	if (pollword_addr != 0) {
		mem_write32(pollword_addr, CLIPBOARD_POLLWORD_HOST_CHANGED);
	}
	if (clipboard_type_is_image(clip_file_type)) {
		rpclog("Clipboard: a %u byte image (type &%03x) from the host, %s\n",
		       clip_image_len, (unsigned) clip_file_type,
		       pollword_addr != 0 ? "guest told" : "no guest listening");
	} else {
		rpclog("Clipboard: %u characters from the host, %s\n", clip_ucs_len,
		       pollword_addr != 0 ? "guest told" : "no guest listening");
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
			/* Something may already be on the clipboard from before this guest
			   started. Knock now, so it starts up in step with the host. */
			if (clip_ucs != NULL && clip_ucs_len > 0) {
				mem_write32(pollword_addr, CLIPBOARD_POLLWORD_HOST_CHANGED);
			}
		} else {
			have_ucstable = 0;
			rpclog("Clipboard: guest task gone\n");
		}
		break;

	case ARCEM_SWI_CLIPBOARD_HOST_CHECK:
		/* How much is there to fetch, and of what type? Text is counted
		   as the guest will see it: one byte per character, plus a
		   terminator, as Cloverleaf's module expects. */
		if (clip_image != NULL && clip_image_len > 0) {
			/* Exactly the file's length: an image is not a string, and
			   the guest saves the byte count we give it. Its buffer has
			   room beyond this for the terminator it writes anyway. */
			*retr0 = clip_image_len;
			*retr1 = (uint32_t) clip_file_type;
		} else if (clip_ucs != NULL && clip_ucs_len > 0) {
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

		if (room == 0) {
			break;
		}
		if (clip_image != NULL && clip_image_len > 0) {
			/* Straight through: the guest asked for the file, not for
			   pixels, so there is nothing to convert. */
			const unsigned int n = (clip_image_len < room) ? clip_image_len : room;

			memcpyfromhost(r1, clip_image, n);
			*retr0 = n;
			rpclog("Clipboard: guest fetched a %u byte image\n", n);
			break;
		}
		if (clip_ucs == NULL) {
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
		rpclog("Clipboard: guest fetched %u bytes\n", len);
		break;
	}

	case ARCEM_SWI_CLIPBOARD_HOST_SET: {
		/* The guest is putting something on the clipboard:
		   r1 = data, r2 = length, r3 = filetype. */
		unsigned char *in;
		uint32_t *ucs;
		unsigned int i;
		unsigned int out = 0;

		if (r2 == 0) {
			break;
		}
		if (clipboard_type_is_image((int) r3)) {
			unsigned char *image = malloc(r2);

			if (image == NULL) {
				rpclog("Clipboard: out of memory for a %u byte image "
				       "from the guest\n", (unsigned) r2);
				break;
			}
			memcpytohost(image, r1, r2);

			clip_discard();
			clip_image = image;
			clip_image_len = r2;
			clip_file_type = (int) r3;
			rpclog("Clipboard: a %u byte image (type &%03x) from the guest\n",
			       clip_image_len, (unsigned) clip_file_type);

			if (host_image_setter != NULL) {
				host_image_setter(clip_file_type, clip_image, clip_image_len);
			}
			break;
		}
		if (r3 != CLIPBOARD_TYPE_TEXT) {
			break;		/* nothing else is carried */
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

		clip_discard();
		clip_ucs = ucs;
		clip_ucs_len = out;
		clip_file_type = CLIPBOARD_TYPE_TEXT;
		rpclog("Clipboard: %u characters from the guest\n", out);

		/* Hand it to the front end as UTF-8. */
		if (host_setter != NULL) {
			char *utf8 = malloc(out * 4 + 1);

			if (utf8 != NULL) {
				const int len = clip_to_utf8(utf8, out * 4 + 1);

				if (len >= 0) {
					host_setter(utf8, (unsigned int) len);
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
