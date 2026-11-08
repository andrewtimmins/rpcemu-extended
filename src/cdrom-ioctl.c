/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "rpcemu.h"
#include "ide.h"

#define IOCTL_DEVICE "/dev/cdrom"

static ATAPI ioctl_atapi;

static int ioctl_fd = -1;
static int ioctl_discchanged = 0;
static int ioctl_empty = 0;

static void ioctl_release(void)
{
	if (ioctl_fd >= 0) {
		close(ioctl_fd);
		ioctl_fd = -1;
	}
	ioctl_empty = 1;
}

static int ioctl_ready(void)
{
	if (ioctl_empty)
		return 0;
	if (ioctl_discchanged) {
		ioctl_discchanged = 0;
		atapi_discchanged();
		return 0;
	}
	return 1;
}

static void ioctl_readsector(uint8_t *b, int sector)
{
	if (ioctl_empty || ioctl_fd < 0)
		return;
	lseek(ioctl_fd, (off_t) sector * 2048, SEEK_SET);
	read(ioctl_fd, b, 2048);
}

static int ioctl_readtoc(unsigned char *b, unsigned char starttrack, int msf)
{
	int len = 4;
	int blocks;
	off_t size;

	if (ioctl_empty || ioctl_fd < 0)
		return 0;

	size = lseek(ioctl_fd, 0, SEEK_END);
	if (size <= 0)
		return 0;
	blocks = (int) (size / 2048);

	if (starttrack <= 1) {
		b[len++] = 0; /* Reserved */
		b[len++] = 0x14; /* ADR, control */
		b[len++] = 1; /* Track number */
		b[len++] = 0; /* Reserved */

		/* Start address */
		if (msf) {
			b[len++] = 0; /* reserved */
			b[len++] = 0; /* minute */
			b[len++] = 2; /* second */
			b[len++] = 0; /* frame */
		} else {
			b[len++] = 0;
			b[len++] = 0;
			b[len++] = 0;
			b[len++] = 0; /* logical sector 0 */
		}
	}

	b[2] = b[3] = 1; /* First and last track numbers */
	b[len++] = 0; /* Reserved */
	b[len++] = 0x16; /* ADR, control */
	b[len++] = 0xaa; /* Track number */
	b[len++] = 0; /* Reserved */

	if (msf) {
		b[len++] = 0; /* reserved */
		b[len++] = (uint8_t) (((blocks + 150) / 75) / 60); /* minute */
		b[len++] = (uint8_t) (((blocks + 150) / 75) % 60); /* second */
		b[len++] = (uint8_t) ((blocks + 150) % 75); /* frame */
	} else {
		b[len++] = (uint8_t) ((blocks >> 24) & 0xff);
		b[len++] = (uint8_t) ((blocks >> 16) & 0xff);
		b[len++] = (uint8_t) ((blocks >> 8) & 0xff);
		b[len++] = (uint8_t) ((blocks >> 0) & 0xff);
	}
	b[0] = (uint8_t) (((len - 4) >> 8) & 0xff);
	b[1] = (uint8_t) ((len - 4) & 0xff);
	return len;
}

static uint8_t ioctl_getcurrentsubchannel(uint8_t *b, int msf)
{
	NOT_USED(msf);

	memset(b, 0, 2048);
	return 0;
}

static void ioctl_playaudio(uint32_t pos, uint32_t len)
{
	NOT_USED(pos);
	NOT_USED(len);

	UNIMPLEMENTED("Linux CDROM", "ioctl_playaudio");
}

static void ioctl_seek(uint32_t pos)
{
	NOT_USED(pos);

	UNIMPLEMENTED("Linux CDROM", "ioctl_seek");
}

static void ioctl_null(void)
{
}

static void ioctl_exit(void)
{
	ioctl_release();
}

void ioctl_init(void)
{
	ioctl_release();
	atapi = &ioctl_atapi;

	ioctl_fd = open(IOCTL_DEVICE, O_RDONLY | O_NONBLOCK);
	if (ioctl_fd >= 0) {
		ioctl_empty = 0;
		ioctl_discchanged = 1;
		rpclog("CD-ROM: opened host device '%s'\n", IOCTL_DEVICE);
	} else {
		ioctl_empty = 1;
		rpclog("CD-ROM: failed to open host device '%s'\n", IOCTL_DEVICE);
	}
	atapi_discchanged();
}

static ATAPI ioctl_atapi =
{
	ioctl_ready,
	ioctl_readtoc,
	ioctl_getcurrentsubchannel,
	ioctl_readsector,
	ioctl_playaudio,
	ioctl_seek,
	ioctl_null, ioctl_null, ioctl_null, ioctl_null, ioctl_null,
	ioctl_exit
};
