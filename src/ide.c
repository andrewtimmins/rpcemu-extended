/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

/* IDE emulation */

void callbackide(void);

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <string.h>

#include <sys/types.h>

#include "lfs-compat.h"
#include "rpcemu.h"
#include "vidc20.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "cmos.h"
#include "arm.h"
#include "savestate.h"

/* Bits of 'atastat' */
#define ERR_STAT		0x01
#define DRQ_STAT		0x08 /* Data request */
#define READY_STAT		0x40
#define BUSY_STAT		0x80
#define DRQ_READY_STAT		(DRQ_STAT | READY_STAT)

/* Bits of 'error' */
#define ABRT_ERR		0x04 /* Command aborted */
#define MCR_ERR			0x08 /* Media change request */
#define IDNF_ERR		0x10 /* ID not found / invalid sector */

/* ATA Commands */
#define WIN_SRST			0x08 /* ATAPI Device Reset */
#define WIN_RECAL			0x10
#define WIN_RESTORE			WIN_RECAL
#define WIN_READ			0x20 /* 28-Bit Read */
#define WIN_WRITE			0x30 /* 28-Bit Write */
#define WIN_VERIFY			0x40 /* 28-Bit Verify */
#define WIN_FORMAT			0x50
#define WIN_SEEK			0x70
#define WIN_SPECIFY			0x91 /* Initialize Drive Parameters */
#define WIN_PACKETCMD			0xA0 /* Send a packet command. */
#define WIN_PIDENTIFY			0xA1 /* Identify ATAPI device */
#define WIN_SETIDLE1			0xE3
#define WIN_IDENTIFY			0xEC /* Ask drive to identify itself */
#define WIN_SETMULT			0xC6 /* Set multiple mode */
#define WIN_SETFEATURES			0xEF /* Set features */
#define WIN_STANDBY			0xE0 /* Standby immediate */
#define WIN_IDLEIMMEDIATE		0xE1 /* Idle immediate */
#define WIN_CHECKPOWER			0xE5 /* Check power mode */
#define WIN_READ_NATIVE_MAX		0xF8 /* Read native max address */

/* ATAPI Commands */
#define GPCMD_INQUIRY			0x12
#define GPCMD_MODE_SELECT_10		0x55
#define GPCMD_MODE_SENSE_10		0x5a
#define GPCMD_PAUSE_RESUME		0x4b
#define GPCMD_PLAY_AUDIO_12		0xa5
#define GPCMD_READ_CD			0xbe
#define GPCMD_READ_HEADER		0x44
#define GPCMD_READ_SUBCHANNEL		0x42
#define GPCMD_READ_TOC_PMA_ATIP		0x43
#define GPCMD_REQUEST_SENSE		0x03
#define GPCMD_SEEK			0x2b
#define GPCMD_SEND_DVD_STRUCTURE	0xad
#define GPCMD_SET_SPEED			0xbb
#define GPCMD_START_STOP_UNIT		0x1b
#define GPCMD_TEST_UNIT_READY		0x00

/* Mode page codes for mode sense/set */
#define GPMODE_R_W_ERROR_PAGE		0x01
#define GPMODE_CDROM_PAGE		0x0d
#define GPMODE_CAPABILITIES_PAGE	0x2a
#define GPMODE_ALL_PAGES		0x3f

/* ATAPI Sense Keys */
#define SENSE_NONE		0
#define SENSE_NOT_READY		2
#define SENSE_ILLEGAL_REQUEST	5
#define SENSE_UNIT_ATTENTION	6

/* ATAPI Additional Sense Codes */
#define ASC_ILLEGAL_OPCODE		0x20
#define ASC_MEDIUM_NOT_PRESENT		0x3a

/* Tell RISC OS that we have a 4x CD-ROM drive (600kb/sec data, 706kb/sec raw).
   Not that it means anything */
#define CDROM_SPEED	706

/** Evaluate to non-zero if the currently selected drive is an ATAPI device */
#define IDE_DRIVE_IS_CDROM(ide) \
	(config.cdromenabled && (ide.drive == 1))

ATAPI *atapi;
int idecallback = 0;

static void callreadcd(void);
static void atapicommand(void);
static void ide_next_sector(void);

static struct
{
        uint8_t atastat;
        unsigned char error;
        int secount,sector,cylinder,head,drive,cylprecomp;
        unsigned char command;
        unsigned char fdisk;
        int pos;
        int packlen;
        int spt[2], hpc[2];
        int packetstatus;
        int cdpos,cdlen;
        unsigned char asc;
        int discchanged;
        int reset;
        FILE *hdfile[2];
        off64_t hd_filesize[2];
        int skip512[2];
        int lba_cmd[2];
        uint16_t buffer[65536];
} ide;

static void
ide_run_callback(void)
{
	idecallback = 0;
	callbackide();
}

static inline void
ide_read_complete(void)
{
	ide.pos = 0;
	ide.atastat = READY_STAT;
	if (ide.command == WIN_READ) {
		ide.secount--;
		if (ide.secount) {
			ide_next_sector();
			ide.atastat = BUSY_STAT;
			ide_run_callback();
		}
	}
}

static inline void
ide_write_complete(void)
{
	ide.pos = 0;
	ide.atastat = BUSY_STAT;
	ide_run_callback();
}

/** ATA post-reset task-file values expected by RISC OS after SRST. */
static void
ide_apply_post_reset_signature(void)
{
	ide.atastat = READY_STAT;
	ide.error = 0x01; /* Diagnostic code: device passed */
	ide.secount = 1;
	ide.sector = 1;
	ide.cylinder = 0;
	ide.head = 0;
	ide.command = 0;
	ide.pos = 0;
	ide.reset = 0;
	ide.packetstatus = 0;
	ide.packlen = 0;
	ide.cdlen = ide.cdpos = 0;
	memset(ide.buffer, 0, 512);
}

static void
ide_reset_controller_state(void)
{
	ide.drive = 0;
	ide.fdisk = 0;
	ide.cylprecomp = 0;
	ide.lba_cmd[0] = ide.lba_cmd[1] = 0;
	ide.discchanged = 0;
	ide.asc = 0;
	ide_apply_post_reset_signature();
}

static inline void
ide_irq_raise(void)
{
	iomd.irqb.status |= IOMD_IRQB_IDE;
	updateirqs();
}

static inline void
ide_irq_lower(void)
{
	iomd.irqb.status &= ~IOMD_IRQB_IDE;
	updateirqs();
}

static inline int
ide_drive_is_hdd(int drive)
{
	return ide.hdfile[drive] != NULL && !(config.cdromenabled && drive == 1);
}

static off64_t
ide_hd_logical_sectors(int drive)
{
	if (!ide_drive_is_hdd(drive)) {
		return 0;
	}
	return (ide.hd_filesize[drive] / 512) - ide.skip512[drive];
}

static void
ide_media_error_idnf(void)
{
	ide.atastat = READY_STAT | ERR_STAT;
	ide.error = IDNF_ERR;
	ide_irq_raise();
}

void
ide_get_snapshot(IDEStateSnapshot *snapshot)
{
        if (snapshot == NULL) {
                return;
        }

        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->atastat = ide.atastat;
        snapshot->error = ide.error;
        snapshot->secount = ide.secount;
        snapshot->sector = ide.sector;
        snapshot->cylinder = ide.cylinder;
        snapshot->head = ide.head;
        snapshot->drive = (uint8_t) ide.drive;
        snapshot->cylprecomp = ide.cylprecomp;
        snapshot->command = ide.command;
        snapshot->fdisk = ide.fdisk;
        snapshot->pos = ide.pos;
        snapshot->packlen = ide.packlen;
        snapshot->packet_status = (uint8_t) ide.packetstatus;
        snapshot->cdpos = ide.cdpos;
        snapshot->cdlen = ide.cdlen;
        snapshot->asc = ide.asc;
        snapshot->disc_changed = (uint8_t) (ide.discchanged != 0);
        snapshot->reset_in_progress = (uint8_t) (ide.reset != 0);
        snapshot->command = ide.command;
        snapshot->fdisk = ide.fdisk;
        snapshot->pos = ide.pos;
        snapshot->packlen = ide.packlen;
        snapshot->packet_status = (uint8_t) ide.packetstatus;
        snapshot->cdpos = ide.cdpos;
        snapshot->cdlen = ide.cdlen;
        snapshot->asc = ide.asc;
        snapshot->disc_changed = (uint8_t) (ide.discchanged != 0);
        snapshot->reset_in_progress = (uint8_t) (ide.reset != 0);
        for (int d = 0; d < 2; d++) {
                snapshot->spt[d] = ide.spt[d];
                snapshot->hpc[d] = ide.hpc[d];
                snapshot->drive_skip512[d] = (uint8_t) (ide.skip512[d] != 0);
                snapshot->drive_lba[d] = (uint8_t) (ide.lba_cmd[d] != 0);
                snapshot->drive_present[d] = (uint8_t) (ide.hdfile[d] != NULL);
                snapshot->drive_is_cdrom[d] = (uint8_t) (config.cdromenabled && (d == 1));
        }
}

/**
 * Copy a string into a buffer, padding with spaces, and placing characters as
 * if they were packed into 16-bit values, stored little-endian.
 *
 * @param str Destination buffer
 * @param src Source string
 * @param len Length of destination buffer to fill in. Strings shorter than
 *            this length will be padded with spaces.
 */
static void
ide_padstr(char *str, const char *src, int len)
{
	int i, v;

	for (i = 0; i < len; i++) {
		if (*src != '\0') {
			v = *src++;
		} else {
			v = ' ';
		}
		str[i ^ 1] = v;
	}
}

/**
 * Copy a string into a buffer, padding with spaces. Does not add string
 * terminator.
 *
 * @param buf      Destination buffer
 * @param buf_size Size of destination buffer to fill in. Strings shorter than
 *                 this length will be padded with spaces.
 * @param src      Source string
 */
static void
ide_padstr8(uint8_t *buf, int buf_size, const char *src)
{
	int i;

	for (i = 0; i < buf_size; i++) {
		if (*src != '\0') {
			buf[i] = *src++;
		} else {
			buf[i] = ' ';
		}
	}
}

/**
 * Fill in ide.buffer with the output of the "IDENTIFY DEVICE" command
 */
static void
ide_identify(int drive)
{
	uint32_t sectors;
	const int spt = ide.spt[drive];
	const int hpc = ide.hpc[drive];

	memset(ide.buffer, 0, 512);

	sectors = (uint32_t) ide_hd_logical_sectors(drive);
	if (sectors > 0x0FFFFFFFU) {
		sectors = 0x0FFFFFFFU;
	}

	/* Word 0: fixed ATA device */
	ide.buffer[0] = 0x0040;
	/* Legacy CHS translation geometry */
	ide.buffer[1] = 65535; /* Cylinders */
	ide.buffer[3] = hpc;   /* Heads */
	ide.buffer[6] = spt;   /* Sectors per track */
	ide_padstr((char *) (ide.buffer + 10), "", 20); /* Serial Number */
	ide_padstr((char *) (ide.buffer + 23), "v1.0", 8); /* Firmware */
	ide_padstr((char *) (ide.buffer + 27), "RPCEmuHD", 40); /* Model */
	/* Capabilities: LBA supported (bit 9) */
	ide.buffer[49] = 0x2f00;
	ide.buffer[50] = 0x4000; /* Capabilities (bit 14 set per ATA spec) */
	/* Validity bits for words 54-70 and 88 */
	ide.buffer[53] = 0x0007;
	ide.buffer[54] = hpc;
	ide.buffer[55] = 0; /* Current cylinders - translation */
	ide.buffer[56] = spt;
	ide.buffer[57] = (uint16_t) (sectors & 0xFFFF);
	ide.buffer[58] = (uint16_t) ((sectors >> 16) & 0xFFFF);
	/* Current and total addressable sectors in LBA mode (28-bit) */
	ide.buffer[60] = (uint16_t) (sectors & 0xFFFF);
	ide.buffer[61] = (uint16_t) ((sectors >> 16) & 0xFFFF);
	/* ATA version and command-set support (expected by RISC OS 5 ADFS) */
	ide.buffer[80] = 0x007E;
	ide.buffer[81] = 0x0026;
	ide.buffer[83] = 0x7400;
	ide.buffer[84] = 0x7400;
	ide.buffer[85] = 0x7400;
	ide.buffer[86] = 0x7400;
	ide.buffer[88] = 0x0020;
	/* 48-bit capacity (low 32 bits; sufficient for images under 128 GiB) */
	ide.buffer[100] = (uint16_t) (sectors & 0xFFFF);
	ide.buffer[101] = (uint16_t) ((sectors >> 16) & 0xFFFF);
	ide.buffer[102] = 0;
	ide.buffer[103] = 0;
}

/**
 * Fill in ide.buffer with the output of the "IDENTIFY PACKET DEVICE" command
 */
static void
ide_atapi_identify(void)
{
	memset(ide.buffer, 0, 512);

	ide.buffer[0] = 0x8000 | (5<<8) | 0x80; /* ATAPI device, CD-ROM drive, removable media */
	ide_padstr((char *) (ide.buffer + 10), "", 20); /* Serial Number */
	ide_padstr((char *) (ide.buffer + 23), "v1.0", 8); /* Firmware */
	ide_padstr((char *) (ide.buffer + 27), "RPCEmuCD", 40); /* Model */
	ide.buffer[49] = 0x200; /* LBA supported */
}

/**
 * Fill in ide.buffer with the output of the ATAPI "MODE SENSE" command
 *
 * @param pos Offset within the buffer to start filling in data
 *
 * @return Offset within the buffer after the end of the data
 */
static uint32_t
ide_atapi_mode_sense(uint32_t pos)
{
	uint8_t *buf = (uint8_t *) ide.buffer;

	/* &01 - Read error recovery */
	buf[pos++] = GPMODE_R_W_ERROR_PAGE;
	buf[pos++] = 6; /* Page length */
	buf[pos++] = 0; /* Error recovery parameters */
	buf[pos++] = 3; /* Read retry count */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 0; /* Reserved */

	/* &0D - CD-ROM Parameters */
	buf[pos++] = GPMODE_CDROM_PAGE;
	buf[pos++] = 6; /* Page length */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 1; /* Inactivity time multiplier *NEEDED BY RISCOS* value is a guess */
	buf[pos++] = 0; buf[pos++] = 60; /* MSF settings */
	buf[pos++] = 0; buf[pos++] = 75; /* MSF settings */

	/* &2A - CD-ROM capabilities and mechanical status */
	buf[pos++] = GPMODE_CAPABILITIES_PAGE;
	buf[pos++] = 0x12; /* Page length */
	buf[pos++] = 0; buf[pos++] = 0; /* CD-R methods */
	buf[pos++] = 1; /* Supports audio play, not multisession */
	buf[pos++] = 0; /* Some other stuff not supported */
	buf[pos++] = 0; /* Some other stuff not supported (lock state + eject) */
	buf[pos++] = 0; /* Some other stuff not supported */
	buf[pos++] = (uint8_t) (CDROM_SPEED >> 8);
	buf[pos++] = (uint8_t) CDROM_SPEED; /* Maximum speed */
	buf[pos++] = 0; buf[pos++] = 2; /* Number of audio levels - on and off only */
	buf[pos++] = 0; buf[pos++] = 0; /* Buffer size - none */
	buf[pos++] = (uint8_t) (CDROM_SPEED >> 8);
	buf[pos++] = (uint8_t) CDROM_SPEED; /* Current speed */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 0; /* Drive digital format */
	buf[pos++] = 0; /* Reserved */
	buf[pos++] = 0; /* Reserved */

	return pos;
}

/**
 * Return the logical block address from the current register values
 * (CHS or 28-bit LBA, without any image boot-block offset).
 */
static uint32_t
ide_get_lba_address(void)
{
	if (ide.lba_cmd[ide.drive]) {
		/* ATA-3: head bits 27:24, cylinder bits 23:8, sector bits 7:0 */
		return (uint32_t) ((ide.head << 24) | (ide.cylinder << 8) | ide.sector);
	}

	const int heads = ide.hpc[ide.drive];
	const int sectors = ide.spt[ide.drive];

	return (uint32_t) ((((off64_t) ide.cylinder * heads) + ide.head) *
	    sectors) + (ide.sector - 1);
}

/**
 * Return the sector index used to locate data in the host image file.
 * Images with a 512-byte RISC OS boot block at offset 0 skip one sector.
 */
static off64_t
ide_get_sector(void)
{
	return (off64_t) ide_get_lba_address() + ide.skip512[ide.drive];
}

static int
ide_sector_byte_offset_valid(off64_t byte_offset)
{
	if (!ide_drive_is_hdd(ide.drive)) {
		return 0;
	}
	return byte_offset >= 0 && byte_offset + 512 <= ide.hd_filesize[ide.drive];
}

/**
 * Move to the next sector using CHS addressing
 */
static void
ide_next_sector(void)
{
	if (ide.lba_cmd[ide.drive]) {
		uint32_t lba = ide_get_lba_address() + 1;
		ide.head = (lba >> 24) & 0xf;
		ide.cylinder = (lba >> 8) & 0xffff;
		ide.sector = lba & 0xff;
	} else {
		ide.sector++;
		if (ide.sector == (ide.spt[ide.drive] + 1)) {
			ide.sector = 1;
			ide.head++;
			if (ide.head == ide.hpc[ide.drive]) {
				ide.head = 0;
				ide.cylinder++;
			}
		}
	}
}

/**
 * Given an open harddisc image, attempt to use a heuristic to determine
 * if the image is one of the 'bugged' (offset by 512 bytes/1 sector)
 * and also fill in the sectors per track and heads per cylinder values from
 * the image file (or use defaults if not valid)
 *
 * @param fh Open file handle
 * @param d drive number
 */
static void
ide_image_set_spt_hpc_skip512(FILE *fh, int d)
{
	int log2_sec_size;

	ide.skip512[d] = 0;

	// Check Wrong Offset first
	fseeko64(fh, 0xfc0, SEEK_SET);
	log2_sec_size = getc(ide.hdfile[d]);
	ide.spt[d] = getc(ide.hdfile[d]);
	ide.hpc[d] = getc(ide.hdfile[d]);

	if ((ide.spt[d] == 0 || ide.spt[d] == EOF)
	    || (ide.hpc[d] == 0 || ide.hpc[d] == EOF))
	{
		// Check the correct offset
		fseeko64(ide.hdfile[d], 0xdc0, SEEK_SET);
		log2_sec_size = getc(ide.hdfile[d]);
		ide.spt[d] = getc(ide.hdfile[d]);
		ide.hpc[d] = getc(ide.hdfile[d]);
		if ((ide.spt[d] == 0 || ide.spt[d] == EOF)
		    || (ide.hpc[d] == 0 || ide.hpc[d] == EOF))
		{
			// Nothing found at either location, set
			// sensible defaults
			ide.spt[d] = 63;
			ide.hpc[d] = 16;
		}
	} else {
		ide.skip512[d] = 1;
	}
	rpclog("IDE: drive %d: log2_sec_size %d, spt %d, hpc %d\n",
	    d, log2_sec_size, ide.spt[d], ide.hpc[d]);
}

static int
path_is_absolute(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return 0;
	}
	if (path[0] == '/' || path[0] == '\\') {
		return 1;
	}
	if (strncmp(path, "./", 2) == 0) {
		return 1;
	}
	/* Drive letter path (harmless on Linux; useful for imported configs) */
	if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
	    && path[1] == ':') {
		return 1;
	}
	return 0;
}

static void
resolve_hd_path(const char *filename, char *pathname, size_t pathname_size)
{
	if (path_is_absolute(filename)) {
		snprintf(pathname, pathname_size, "%s", filename);
		return;
	}

	snprintf(pathname, pathname_size, "%s%s", rpcemu_get_datadir(), filename);
}

/**
 * Prepare a hard disc image, open the file and attach it to the IDE device
 *
 * @param d drive number 0 or 1
 * @param filename path to HD image (absolute, or relative to datadir)
 */
static void
loadhd(int d, const char *filename)
{
	char pathname[512];

	if (filename == NULL || filename[0] == '\0') {
		return;
	}

	resolve_hd_path(filename, pathname, sizeof(pathname));

	if (ide.hdfile[d] == NULL) {
		ide.hdfile[d] = fopen64(pathname, "rb+");
		if (ide.hdfile[d] == NULL) {
			if (errno == ENOENT) {
				rpclog("IDE: No hard disc image '%s' (drive %d) - skipping\n",
				       pathname, d);
				return;
			}
			error("Cannot open hard disc image '%s': %s",
			      pathname, strerror(errno));
			return;
		}
	}

	fseeko64(ide.hdfile[d], 0, SEEK_END);
	const off64_t filesize = ftello64(ide.hdfile[d]);

	if (filesize == 0) {
		fclose(ide.hdfile[d]);
		ide.hdfile[d] = NULL;
		rpclog("IDE: Skipping empty hard disc image '%s' (drive %d) - "
		       "create or copy a disc image and reset\n",
		       pathname, d);
		return;
	}

	ide.hd_filesize[d] = filesize;
	ide_image_set_spt_hpc_skip512(ide.hdfile[d], d);

	rpclog("IDE: Loaded file '%s' as IDE disc %d, size %" PRId64 " MB (%" PRId64
	       "), %" PRId64 " logical sectors%s\n",
		filename,
		d,
		(int64_t) filesize / 1024 / 1024,
		(int64_t) filesize,
		(int64_t) ide_hd_logical_sectors(d),
		ide.skip512[d] ? ", 512-byte boot block at start" : "");
}

static void
ide_log_drive_summary(void)
{
	for (int d = 0; d < 2; d++) {
		if (ide_drive_is_hdd(d)) {
			rpclog("IDE: Drive %d attached, %" PRId64 " logical sectors (%" PRId64 " bytes)\n",
			       d,
			       (int64_t) ide_hd_logical_sectors(d),
			       (int64_t) ide.hd_filesize[d]);
		} else if (config.cdromenabled && d == 1) {
			rpclog("IDE: Drive %d CD-ROM (HD5 unavailable)\n", d);
		} else {
			rpclog("IDE: Drive %d not present\n", d);
		}
	}
}

int
ide_attached_hdd_count(void)
{
	int count = 0;

	for (int d = 0; d < 2; d++) {
		if (ide_drive_is_hdd(d)) {
			count++;
		}
	}
	return count;
}

void
ide_reload_images(void)
{
	resetide();
	cmos_sync_ide_drive_count();
}

void resetide(void)
{
        int d;
        char hd_path[1024];

        /* Close hard disk image files (if previously open) */
        for (d = 0; d < 2; d++) {
                if (ide.hdfile[d] != NULL) {
                        fclose(ide.hdfile[d]);
                        ide.hdfile[d] = NULL;
                }
                ide.hd_filesize[d] = 0;
        }

	ide_reset_controller_state();
        idecallback = 0;

	/* Load HD4: optional override in config, otherwise machine directory */
	if (config.hd4_path[0] != '\0') {
		if (path_is_absolute(config.hd4_path)) {
			loadhd(0, config.hd4_path);
		} else {
			snprintf(hd_path, sizeof(hd_path), "%s%s",
			         rpcemu_get_machine_datadir(), config.hd4_path);
			loadhd(0, hd_path);
		}
	} else {
		snprintf(hd_path, sizeof(hd_path), "%shd4.hdf", rpcemu_get_machine_datadir());
		loadhd(0, hd_path);
	}

	/* Load HD5: only when CD-ROM is disabled */
	if (!config.cdromenabled) {
		snprintf(hd_path, sizeof(hd_path), "%shd5.hdf", rpcemu_get_machine_datadir());
		loadhd(1, hd_path);
	}

	ide_log_drive_summary();
}

void writeidew(uint16_t val)
{
#ifdef _RPCEMU_BIG_ENDIAN
		val=(val>>8)|(val<<8);
#endif
        ide.buffer[ide.pos >> 1] = val;
        ide.pos+=2;

        if (ide.packetstatus==4)
        {
                if (ide.pos>=(ide.packlen+2))
                {
                        ide.packetstatus=5;
                        idecallback=6;
//                        rpclog("Packet over!\n");
                        ide_irq_lower();
                }
                return;
        }
        else if (ide.packetstatus==5) return;
        else if (ide.command == WIN_PACKETCMD && ide.pos>=0xC)
        {
                ide.pos=0;
                ide.atastat = BUSY_STAT;
                ide.packetstatus=1;
                idecallback=60;
//                rpclog("Packet now waiting!\n");
        }
        else if (ide.pos>=512)
        {
                ide_write_complete();
        }
}

void writeide(uint16_t addr, uint8_t val)
{
        uint8_t *idebufferb = (uint8_t *) ide.buffer;

        switch (addr)
        {
        case 0x1F0: /* Data */
                idebufferb[ide.pos++]=val;
                if (ide.pos>=512)
                {
                        ide_write_complete();
                }
                return;

        case 0x1F1: /* Features */
                ide.cylprecomp=val;
                return;

        case 0x1F2: /* Sector count */
                ide.secount=val;
                return;

        case 0x1F3: /* Sector */
                ide.sector=val;
                return;

        case 0x1F4: /* Cylinder low */
                ide.cylinder=(ide.cylinder&0xFF00)|val;
                return;

        case 0x1F5: /* Cylinder high */
                ide.cylinder=(ide.cylinder&0xFF)|(val<<8);
                return;

        case 0x1F6: /* Drive/Head */
                ide.head=val&0xF;
                if (((val>>4)&1)!=ide.drive)
                {
                        idecallback=0;
                        ide.atastat = READY_STAT;
                        ide.error=0;
                        ide.secount=1;
                        ide.sector=1;
                        ide.head=0;
                        ide.cylinder=0;
                        ide.cylprecomp=0;
                        ide.reset = 0;
                        ide.command=0;
                        ide.packetstatus=0;
                        ide.packlen=0;
                        ide.cdlen=ide.cdpos=ide.pos=0;
                        memset(ide.buffer, 0, 512);
                        ide_irq_lower();
                }
                ide.drive=(val>>4)&1;
		if (ide.lba_cmd[ide.drive] != (val & 0x40)) {
			// prm bit 6 is LBA addressing flag, per command
			ide.lba_cmd[ide.drive] = val & 0x40;
			rpclog("IDE: Drive %d command using %s\n", ide.drive, ide.lba_cmd[ide.drive] ? "LBA Mode" : "CHS Mode");
		}
                ide.pos=0;
                ide.atastat = READY_STAT;
                return;

        case 0x1F7: /* Command register */
                ide.command=val;
                ide.error=0;
                rpclog("IDE: command %02X drive %d%s\n",
                       val, ide.drive,
                       ide_drive_is_hdd(ide.drive) ? "" : " (no HDD image)");
                switch (val)
                {
                case WIN_SRST: /* ATAPI Device Reset */
                        ide.atastat = READY_STAT;
                        idecallback=100;
                        return;

                case WIN_RESTORE:
                case WIN_SEEK:
                        ide.atastat = READY_STAT;
                        idecallback=100;
                        return;

                case WIN_READ:
                case WIN_VERIFY:
                case WIN_FORMAT:
                case WIN_SPECIFY: /* Initialize Drive Parameters */
                case WIN_PIDENTIFY: /* Identify Packet Device */
                case WIN_SETIDLE1: /* Idle */
                case WIN_IDENTIFY: /* Identify Device */
                case WIN_SETFEATURES:
                case WIN_STANDBY:
                case WIN_IDLEIMMEDIATE:
                case WIN_CHECKPOWER:
                case WIN_SETMULT:
                case WIN_READ_NATIVE_MAX:
                        ide.atastat = BUSY_STAT;
                        ide_run_callback();
                        return;

                case WIN_WRITE:
                        ide.atastat = DRQ_READY_STAT;
                        ide.pos=0;
                        return;

                case WIN_PACKETCMD: /* ATAPI Packet */
                        ide.packetstatus=0;
                        ide.atastat = BUSY_STAT;
                        idecallback=30;
                        ide.pos=0;
                        return;
                }
                rpclog("IDE: unimplemented command %02X\n", val);
                ide.atastat = READY_STAT | ERR_STAT;
                ide.error = ABRT_ERR;
                ide_irq_raise();
                return;

        case 0x3F6: /* Device control */
                if ((ide.fdisk&4) && !(val&4))
                {
                        idecallback=500;
                        ide.reset = 1;
                        ide.atastat = BUSY_STAT;
//                        rpclog("IDE Reset\n");
                }
                ide.fdisk=val;
                return;
        }
        //fatal("Bad IDE write %04X %02X\n", addr, val);
}

uint8_t readide(uint16_t addr)
{
        const uint8_t *idebufferb = (const uint8_t *) ide.buffer;
        uint8_t temp;

        switch (addr)
        {
        case 0x1F0: /* Data */
//                rpclog("Read data %08X ",ide.pos);
                temp=idebufferb[ide.pos++];
//                rpclog("%04X\n",temp);
                if (ide.pos>=512)
                {
                        ide_read_complete();
                }
                return temp;

        case 0x1F1: /* Error */
                return ide.error;

        case 0x1F2: /* Sector count */
                return (uint8_t)ide.secount;

        case 0x1F3: /* Sector */
                return (uint8_t)ide.sector;

        case 0x1F4: /* Cylinder low */
                return (uint8_t)(ide.cylinder&0xFF);

        case 0x1F5: /* Cylinder high */
                return (uint8_t)(ide.cylinder>>8);

        case 0x1F6: /* Drive/Head */
                return (uint8_t) (0xA0 | (ide.drive << 4) |
                    (ide.lba_cmd[ide.drive] ? 0x40 : 0) | (ide.head & 0x0F));

        case 0x1F7: /* Status */
                ide_irq_lower();
                if (!ide_drive_is_hdd(ide.drive) && !IDE_DRIVE_IS_CDROM(ide)) {
                        return 0x00; /* No device selected / not present */
                }
                return ide.atastat;

        case 0x3F6: /* Alternate Status */
                if (!ide_drive_is_hdd(ide.drive) && !IDE_DRIVE_IS_CDROM(ide)) {
                        return 0x00;
                }
                return ide.atastat;
        }
	//fatal("Bad IDE read %04X\n", addr);
	return 0;
}

uint16_t readidew(void)
{
        uint16_t temp;

        temp = ide.buffer[ide.pos >> 1];
	#ifdef _RPCEMU_BIG_ENDIAN
		temp=(temp>>8)|(temp<<8);
	#endif
        ide.pos+=2;
        if ((ide.pos>=512 && ide.command != WIN_PACKETCMD) || (ide.command == WIN_PACKETCMD && ide.pos>=ide.packlen))
        {
//                rpclog("Over! packlen %i %i\n",ide.packlen,ide.pos);
                ide.pos=0;
                if (ide.command == WIN_PACKETCMD && ide.packetstatus==6)
                {
                        callreadcd();
                }
                else
                {
                        ide.atastat = READY_STAT;
                        ide.packetstatus=0;
                        if (ide.command == WIN_READ)
                        {
                                ide.secount--;
                                if (ide.secount)
                                {
                                        ide_next_sector();
                                        ide.atastat = BUSY_STAT;
                                        ide_run_callback();
                                }
                        }
                }
        }
        return temp;
}

void callbackide(void)
{
        off64_t addr;
        int c;
        if (ide.reset)
        {
                ide_apply_post_reset_signature();
                ide_irq_raise();
                return;
        }
        switch (ide.command)
        {
        case WIN_SRST: /*ATAPI Device Reset */
                ide.atastat = READY_STAT;
                ide.error=1; /*Device passed*/
                ide.secount = ide.sector = 1;
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        ide.cylinder = 0xeb14;
                } else {
                        ide.cylinder = 0;
                }
                ide_irq_raise();
                return;

        case WIN_RESTORE:
        case WIN_SEEK:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_SETFEATURES:
        case WIN_STANDBY:
        case WIN_IDLEIMMEDIATE:
        case WIN_CHECKPOWER:
        case WIN_SETMULT:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                if (!ide_drive_is_hdd(ide.drive)) {
                        goto abort_cmd;
                }
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_READ_NATIVE_MAX:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                if (!ide_drive_is_hdd(ide.drive)) {
                        goto abort_cmd;
                }
                {
                        uint32_t max_lba = (uint32_t) ide_hd_logical_sectors(ide.drive);
                        if (max_lba > 0) {
                                max_lba--;
                        }
                        ide.sector = max_lba & 0xff;
                        ide.cylinder = (max_lba >> 8) & 0xffff;
                        ide.head = (max_lba >> 24) & 0x0f;
                }
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_READ:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide_activity_increment();
                addr = ide_get_sector() * 512;
                if (!ide_sector_byte_offset_valid(addr)) {
                        ide_media_error_idnf();
                        return;
                }
                fseeko64(ide.hdfile[ide.drive], addr, SEEK_SET);
                if (fread(ide.buffer, 1, 512, ide.hdfile[ide.drive]) != 512) {
                        ide_media_error_idnf();
                        return;
                }
                ide.pos=0;
                ide.atastat = DRQ_READY_STAT;
                ide_irq_raise();
                return;

        case WIN_WRITE:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide_activity_increment();
                addr = ide_get_sector() * 512;
                if (!ide_sector_byte_offset_valid(addr)) {
                        ide_media_error_idnf();
                        return;
                }
                fseeko64(ide.hdfile[ide.drive], addr, SEEK_SET);
                if (fwrite(ide.buffer, 512, 1, ide.hdfile[ide.drive]) != 1) {
                        rpclog("IDE: sector write failed at offset %llu: %s\n",
                               (unsigned long long) addr, strerror(errno));
                }
                ide_irq_raise();
                ide.secount--;
                if (ide.secount != 0) {
                        ide.atastat = DRQ_READY_STAT;
                        ide.pos=0;
                        ide_next_sector();
                } else {
                        ide.atastat = READY_STAT;
                }
                return;

        case WIN_VERIFY:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector() * 512;
                if (!ide_sector_byte_offset_valid(addr)) {
                        ide_media_error_idnf();
                        return;
                }
                fseeko64(ide.hdfile[ide.drive], addr, SEEK_SET);
                if (fread(ide.buffer, 1, 512, ide.hdfile[ide.drive]) != 512) {
                        ide_media_error_idnf();
                        return;
                }
                ide.secount--;
                if (ide.secount != 0) {
                        ide_next_sector();
                        ide.atastat = BUSY_STAT;
                        idecallback = 200;
                        return;
                }
                ide.pos = 0;
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_FORMAT:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector() * 512;
                if (!ide_sector_byte_offset_valid(addr)) {
                        ide_media_error_idnf();
                        return;
                }
                fseeko64(ide.hdfile[ide.drive], addr, SEEK_SET);
                memset(ide.buffer, 0, 512);
                for (c=0;c<ide.secount;c++)
                {
                        if (fwrite(ide.buffer, 512, 1, ide.hdfile[ide.drive]) != 1) {
                                rpclog("IDE: format write failed: %s\n",
                                       strerror(errno));
                                break;
                        }
                }
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_SPECIFY: /* Initialize Drive Parameters */
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide.spt[ide.drive] = ide.secount;
                ide.hpc[ide.drive] = ide.head + 1;
                ide.atastat = READY_STAT;
                ide_irq_raise();
                return;

        case WIN_PIDENTIFY: /* Identify Packet Device */
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        ide_atapi_identify();
                        ide.pos=0;
                        ide.error=0;
                        ide.atastat = DRQ_READY_STAT;
                        ide_irq_raise();
                        return;
                }
                goto abort_cmd;

        case WIN_SETIDLE1: /* Idle */
                goto abort_cmd;

        case WIN_IDENTIFY: /* Identify Device */
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        ide.secount=1;
                        ide.sector=1;
                        ide.cylinder=0xEB14;
                        ide.drive=ide.head=0;
                        goto abort_cmd;
                }
                if (!ide_drive_is_hdd(ide.drive)) {
                        goto abort_cmd;
                }
                ide_identify(ide.drive);
                ide.pos=0;
                ide.atastat = DRQ_READY_STAT;
                ide_irq_raise();
                return;

        case WIN_PACKETCMD: /* ATAPI Packet */
//                rpclog("Packet callback! %i\n",ide.packetstatus);
                if (!ide.packetstatus)
                {
                        ide.pos=0;
                        ide.error=(uint8_t)((ide.secount&0xF8)|1);
                        ide.atastat = DRQ_READY_STAT;
                        ide_irq_raise();
//                        rpclog("Preparing to recieve packet max DRQ count %04X\n",ide.cylinder);
                }
                else if (ide.packetstatus==1)
                {
                        ide.atastat = BUSY_STAT;
                        atapicommand();
//                        exit(-1);
                }
                else if (ide.packetstatus==2)
                {
                        ide.atastat = READY_STAT;
                        ide_irq_raise();
                }
                else if (ide.packetstatus==3)
                {
                        ide.atastat = DRQ_READY_STAT;
//                        rpclog("Recieve data packet!\n");
                        ide_irq_raise();
                        ide.packetstatus=0xFF;
                }
                else if (ide.packetstatus==4)
                {
                        ide.atastat = DRQ_READY_STAT;
//                        rpclog("Send data packet!\n");
                        ide_irq_raise();
//                        ide.packetstatus=5;
                        ide.pos=2;
                }
                else if (ide.packetstatus==5)
                {
//                        rpclog("Packetstatus 5 !\n");
                        atapicommand();
                }
                else if (ide.packetstatus==6) /*READ CD callback*/
                {
                        ide.atastat = DRQ_READY_STAT;
//                        rpclog("Recieve data packet 6!\n");
                        ide_irq_raise();
//                        ide.packetstatus=0xFF;
                }
                else if (ide.packetstatus==0x80) /*Error callback*/
                {
                        ide.atastat = READY_STAT | ERR_STAT;
                        ide_irq_raise();
                }
                return;
        }

abort_cmd:
	ide.atastat = READY_STAT | ERR_STAT;
	ide.error = ABRT_ERR;
	ide_irq_raise();
}

/*ATAPI CD-ROM emulation
  Depends on driver files — cdrom-iso.c for ISO images and cdrom-ioctl.c
  for host drive access. There is an ATAPI interface defined in ide.h.
  There are a couple of bugs in the CD audio handling.
  */

static void atapi_notready(void)
{
        /*Medium not present is 02/3A/--*/
        /*cylprecomp is error number*/
        /*SENSE/ASC/ASCQ*/
        ide.atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
        ide.error = (SENSE_NOT_READY << 4) | ABRT_ERR;
        if (ide.discchanged) {
                ide.error |= MCR_ERR;
        }
        ide.discchanged=0;
        ide.asc = ASC_MEDIUM_NOT_PRESENT;
        ide.packetstatus=0x80;
        idecallback=50;
}

void atapi_discchanged(void)
{
        ide.discchanged=1;
}

static void atapicommand(void)
{
        uint8_t *idebufferb = (uint8_t *) ide.buffer;
        int c;
        int len;
        int msf;
        int pos=0;
        ide_activity_increment();
//        rpclog("New ATAPI command %02X\n",idebufferb[0]);
                msf=idebufferb[1]&2;

        switch (idebufferb[0])
        {
        case GPCMD_TEST_UNIT_READY:
                if (!atapi->ready()) { atapi_notready(); return; }
//                if (atapi->ready())
//                {
                        ide.packetstatus=2;
                        idecallback=50;
//                }
//                else
//                {
//                        rpclog("Medium not present!\n");
//                }
                break;

        case GPCMD_REQUEST_SENSE: /* Used by ROS 4+ */
                /*Will return 18 bytes of 0*/
                memset(idebufferb,0,512);
                ide.packetstatus=3;
                ide.cylinder=18;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=18;
                break;

        case GPCMD_SET_SPEED:
                ide.packetstatus=2;
                idecallback=50;
                break;

        case GPCMD_READ_TOC_PMA_ATIP:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (idebufferb[9]>>7)
                {
                        rpclog("Bad read TOC format\n");
                        rpclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            rpclog("%02X ",idebufferb[c]);
                        rpclog("\n");
                        exit(-1);
                }
                len=atapi->readtoc(idebufferb,idebufferb[6],msf);
  /*      rpclog("ATAPI buffer len %i\n",len);
        for (c=0;c<len;c++) rpclog("%02X ",idebufferb[c]);
        rpclog("\n");*/
        ide.packetstatus=3;
        ide.cylinder=len;
        ide.secount=2;
//        ide.atastat = DRQ_READY_STAT;
        ide.pos=0;
                idecallback=60;
                ide.packlen=len;
//        rpclog("Sending packet\n");
        return;
        
                switch (idebufferb[6])
                {
                        case 0xAA: /*Start address of lead-out*/
                        break;
                }
                rpclog("Read bad track %02X in read TOC\n",idebufferb[6]);
                rpclog("Packet data :\n");
                for (c=0;c<12;c++)
                    rpclog("%02X\n",idebufferb[c]);
                exit(-1);
                
        case GPCMD_READ_CD:
                if (!atapi->ready()) { atapi_notready(); return; }
//                rpclog("Read CD : start LBA %02X%02X%02X%02X Length %02X%02X%02X Flags %02X\n",idebufferb[2],idebufferb[3],idebufferb[4],idebufferb[5],idebufferb[6],idebufferb[7],idebufferb[8],idebufferb[9]);
                if (idebufferb[9]!=0x10)
                {
                        rpclog("Bad flags bits %02X\n",idebufferb[9]);
                        exit(-1);
                }
/*                if (idebufferb[6] || idebufferb[7] || (idebufferb[8]!=1))
                {
                        rpclog("More than 1 sector!\n");
                        exit(-1);
                }*/
                ide.cdlen=(idebufferb[6]<<16)|(idebufferb[7]<<8)|idebufferb[8];
                ide.cdpos=(idebufferb[2]<<24)|(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
//                rpclog("Read at %08X %08X\n",ide.cdpos,ide.cdpos*2048);
                atapi->readsector(idebufferb,ide.cdpos);

                ide.cdpos++;
                ide.cdlen--;
                if (ide.cdlen>=0) ide.packetstatus=6;
                else              ide.packetstatus=3;
                ide.cylinder=2048;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=2048;
                return;
                
        case GPCMD_READ_HEADER:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (msf)
                {
                        rpclog("Read Header MSF!\n");
                        exit(-1);
                }
                for (c=0;c<4;c++) idebufferb[c+4]=idebufferb[c+2];
                idebufferb[0]=1; /*2048 bytes user data*/
                idebufferb[1]=idebufferb[2]=idebufferb[3]=0;
                
                ide.packetstatus=3;
                ide.cylinder=8;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=8;
                return;
                
        case GPCMD_MODE_SENSE_10:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (idebufferb[2] != GPMODE_ALL_PAGES)
                {
                        rpclog("Bad mode sense - not 3F\n");
                        rpclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            rpclog("%02X\n",idebufferb[c]);
                        exit(-1);
                }
                len=(idebufferb[8]|(idebufferb[7]<<8));
//                rpclog("Mode sense! %i\n",len);
                for (c=0;c<len;c++) idebufferb[c]=0;
                /*Set mode parameter header - bytes 0 & 1 are data length (filled out later),
                  byte 2 is media type*/
                idebufferb[2]=1; /*120mm data CD-ROM*/

                len = ide_atapi_mode_sense(8);

                idebufferb[0]=len>>8;
                idebufferb[1]=len&255;
/*        rpclog("ATAPI buffer len %i\n",len);
        for (c=0;c<len;c++) rpclog("%02X ",idebufferb[c]);
        rpclog("\n");*/
        ide.packetstatus=3;
        ide.cylinder=len;
        ide.secount=2;
//        ide.atastat = DRQ_READY_STAT;
        ide.pos=0;
                idecallback=60;
                ide.packlen=len;
//        rpclog("Sending packet\n");
        return;

        case GPCMD_MODE_SELECT_10:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (ide.packetstatus==5)
                {
                        ide.atastat = 0;
//                        rpclog("Recieve data packet!\n");
                        ide_irq_raise();
                        ide.packetstatus=0xFF;
                        ide.pos=0;
                }
                else
                {
                        len=(idebufferb[7]<<8)|idebufferb[8];
                        ide.packetstatus=4;
                        ide.cylinder=len;
                        ide.secount=2;
                        ide.pos=0;
                        idecallback=6;
                        ide.packlen=len;
/*                        rpclog("Waiting for ARM to send packet %i\n",len);
                rpclog("Packet data :\n");
                for (c=0;c<12;c++)
                    rpclog("%02X ",idebufferb[c]);
                    rpclog("\n");*/
                }
                return;

        case GPCMD_PLAY_AUDIO_12:
                if (!atapi->ready()) { atapi_notready(); return; }
                /*This is apparently deprecated in the ATAPI spec, and apparently
                  has been since 1995 (!). Hence I'm having to guess most of it*/
                pos=(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
                len=(idebufferb[7]<<16)|(idebufferb[8]<<8)|idebufferb[9];
                atapi->playaudio(pos,len);
                ide.packetstatus=2;
                idecallback=50;
                break;

        case GPCMD_READ_SUBCHANNEL:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (idebufferb[3]!=1)
                {
                        rpclog("Bad read subchannel!\n");
                        rpclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            rpclog("%02X\n",idebufferb[c]);
                        arm_dump();
                        exit(-1);
                }
                pos=0;
                idebufferb[pos++]=0;
                idebufferb[pos++]=0; /*Audio status*/
                idebufferb[pos++]=0; idebufferb[pos++]=0; /*Subchannel length*/
                idebufferb[pos++]=1; /*Format code*/
                idebufferb[1]=atapi->getcurrentsubchannel(&idebufferb[5],msf);
                len=11+5;
                ide.packetstatus=3;
                ide.cylinder=len;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=len;
                break;

        case GPCMD_START_STOP_UNIT:
                if (idebufferb[4]!=2 && idebufferb[4]!=3 && idebufferb[4])
                {
                        rpclog("Bad start/stop unit command\n");
                        rpclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            rpclog("%02X\n",idebufferb[c]);
                        exit(-1);
                }
                if (!idebufferb[4])        atapi->stop();
                else if (idebufferb[4]==2) atapi->eject();
                else                       atapi->load();
                ide.packetstatus=2;
                idecallback=50;
                break;
                
        case GPCMD_INQUIRY:
                idebufferb[0] = 5; /*CD-ROM*/
                idebufferb[1] = 0;
                idebufferb[2] = 0;
                idebufferb[3] = 0;
                idebufferb[4] = 31;
                idebufferb[5] = 0;
                idebufferb[6] = 0;
                idebufferb[7] = 0;
                ide_padstr8(idebufferb + 8, 8, "RPCEmu"); /* Vendor */
                ide_padstr8(idebufferb + 16, 16, "RPCEmuCD"); /* Product */
                ide_padstr8(idebufferb + 32, 4, "v1.0"); /* Revision */

                len=36;
                ide.packetstatus=3;
                ide.cylinder=len;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=len;
                break;
                
        case GPCMD_PAUSE_RESUME:
                if (!atapi->ready()) { atapi_notready(); return; }
                if (idebufferb[8]&1) atapi->resume();
                else                 atapi->pause();
                ide.packetstatus=2;
                idecallback=50;
                break;

        case GPCMD_SEEK:
                if (!atapi->ready()) { atapi_notready(); return; }
                pos=(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
                atapi->seek(pos);
                ide.packetstatus=2;
                idecallback=50;
                break;

        case GPCMD_SEND_DVD_STRUCTURE:
        default:
                ide.atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                ide.error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                if (ide.discchanged) {
                        ide.error |= MCR_ERR;
                }
                ide.discchanged=0;
                ide.asc = ASC_ILLEGAL_OPCODE;
                ide.packetstatus=0x80;
                idecallback=50;
                break;
                
/*                default:
                rpclog("Bad ATAPI command %02X\n",idebufferb[0]);
                rpclog("Packet data :\n");
                for (c=0;c<12;c++)
                    rpclog("%02X\n",idebufferb[c]);
                exit(-1);*/
        }
}

static void callreadcd(void)
{
        ide_irq_lower();
        if (ide.cdlen<=0)
        {
                ide.packetstatus=2;
                idecallback=20;
                return;
        }
//        rpclog("Continue readcd! %i blocks left\n",ide.cdlen);
        ide.atastat = BUSY_STAT;
        
        atapi->readsector((uint8_t *) ide.buffer, ide.cdpos);

                ide.cdpos++;
                ide.cdlen--;
                ide.packetstatus=6;
                ide.cylinder=2048;
                ide.secount=2;
                ide.pos=0;
                idecallback=60;
                ide.packlen=2048;
}

/**
 * Write the IDE controller state to a suspend snapshot.
 *
 * The disc image file handles are not stored: resume re-opens the images
 * from the data directory, and every transfer seeks before accessing.
 * The sector buffer is stored so an in-flight transfer carries across.
 */
void
ide_savestate(FILE *f)
{
	int d;

	savestate_write_u8(f, ide.atastat);
	savestate_write_u8(f, ide.error);
	savestate_write_i32(f, ide.secount);
	savestate_write_i32(f, ide.sector);
	savestate_write_i32(f, ide.cylinder);
	savestate_write_i32(f, ide.head);
	savestate_write_i32(f, ide.drive);
	savestate_write_i32(f, ide.cylprecomp);
	savestate_write_u8(f, ide.command);
	savestate_write_u8(f, ide.fdisk);
	savestate_write_i32(f, ide.pos);
	savestate_write_i32(f, ide.packlen);
	for (d = 0; d < 2; d++) {
		savestate_write_i32(f, ide.spt[d]);
		savestate_write_i32(f, ide.hpc[d]);
		savestate_write_i32(f, ide.skip512[d]);
		savestate_write_i32(f, ide.lba_cmd[d]);
	}
	savestate_write_i32(f, ide.packetstatus);
	savestate_write_i32(f, ide.cdpos);
	savestate_write_i32(f, ide.cdlen);
	savestate_write_u8(f, ide.asc);
	savestate_write_i32(f, ide.discchanged);
	savestate_write_i32(f, ide.reset);
	savestate_write_rle(f, ide.buffer, sizeof(ide.buffer));

	savestate_write_i32(f, idecallback);
}

/**
 * Restore the IDE controller state from a suspend snapshot.
 */
void
ide_loadstate(FILE *f)
{
	int d;

	ide.atastat = savestate_read_u8(f);
	ide.error = savestate_read_u8(f);
	ide.secount = savestate_read_i32(f);
	ide.sector = savestate_read_i32(f);
	ide.cylinder = savestate_read_i32(f);
	ide.head = savestate_read_i32(f);
	ide.drive = savestate_read_i32(f);
	ide.cylprecomp = savestate_read_i32(f);
	ide.command = savestate_read_u8(f);
	ide.fdisk = savestate_read_u8(f);
	ide.pos = savestate_read_i32(f);
	ide.packlen = savestate_read_i32(f);
	for (d = 0; d < 2; d++) {
		ide.spt[d] = savestate_read_i32(f);
		ide.hpc[d] = savestate_read_i32(f);
		ide.skip512[d] = savestate_read_i32(f);
		ide.lba_cmd[d] = savestate_read_i32(f);
	}
	ide.packetstatus = savestate_read_i32(f);
	ide.cdpos = savestate_read_i32(f);
	ide.cdlen = savestate_read_i32(f);
	ide.asc = savestate_read_u8(f);
	ide.discchanged = savestate_read_i32(f);
	ide.reset = savestate_read_i32(f);
	savestate_read_rle(f, ide.buffer, sizeof(ide.buffer));

	idecallback = savestate_read_i32(f);
}
