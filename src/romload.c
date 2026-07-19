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

/* ROM loader */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rpcemu.h"
#include "mem.h"
#include "romload.h"

#define MAXROMS 16 /**< Allow up to this many files for a romimage to be broken up into */
#define ROM_PROBE_SCAN_BYTES (256u * 1024u)

/* Website with help on finding romimages */
#define ROM_WEB_SITE "https://github.com/andrewtimmins/rpcemu-extended"

#define ROM_WEB_SITE_STRING "For information on how to acquire ROM images please visit\n" ROM_WEB_SITE

typedef struct {
	uint32_t	addr_data;	///< Address to try matching data
	uint32_t	data[4];	///< Data that must match
	uint32_t	addr_replace;	///< Address of replacement data
	uint32_t	replace;	///< Replacement data
	const char	*comment;	///< Comment that will be added to logfile
} rom_patch_t;

/* The VRAM-cap patches below overwrite the ROM's "MOVEQ R6, #<VRAM MB>"
   instruction that limits how much VRAM the OS will use. The table encodes the
   8MB form; on a machine fitted with 16MB VRAM we rewrite the immediate so the
   OS opens up the extra bank (and the higher screen modes it enables). */
#define VRAM_CAP_MOV_8MB	0x03a06008u	/* MOVEQ R6, #8  */
#define VRAM_CAP_MOV_16MB	0x03a06010u	/* MOVEQ R6, #16 */

static const rom_patch_t rom_patch[] = {
	// Patching for 8MB VRAM
	{ 0x138c0, { 0xe3a00402, 0xe2801004, 0xeb000128, 0x03a06002 }, 0x138cc, 0x03a06008, "8MB VRAM RISC OS 3.50" },
	{ 0x1411c, { 0xe3a00402, 0xe2801004, 0xeb000122, 0x03a06002 }, 0x14128, 0x03a06008, "8MB VRAM RISC OS 3.60" },
	{ 0x15874, { 0xe3a00402, 0xe2801004, 0xeb000143, 0x03a06002 }, 0x15880, 0x03a06008, "8MB VRAM RISC OS 3.70" },
	{ 0x15898, { 0xe3a00402, 0xe2801004, 0xeb000143, 0x03a06002 }, 0x158a4, 0x03a06008, "8MB VRAM RISC OS 3.71" },
	{ 0x14744, { 0xe3a00402, 0xe2801004, 0xeb000148, 0x03a06002 }, 0x14750, 0x03a06008, "8MB VRAM RISC OS 4.02" },
	{ 0x148e8, { 0xe3a00402, 0xe2801004, 0xeb0001ae, 0x03a06002 }, 0x148f4, 0x03a06008, "8MB VRAM RISC OS 4.04" },
	{ 0x14150, { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0x1415c, 0x03a06008, "8MB VRAM RISC OS 4.29" },
	{ 0x1473c, { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0x14748, 0x03a06008, "8MB VRAM RISC OS 4.33" },
	{ 0xe504,  { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0xe510,  0x03a06008, "8MB VRAM RISC OS 4.37" },
	{ 0xe248,  { 0xe3a00402, 0xe2801004, 0xeb0001ae, 0x03a06002 }, 0xe254,  0x03a06008, "8MB VRAM RISC OS 4.39" },
	{ 0xe248,  { 0xe3a00402, 0xe2801004, 0xeb0001ad, 0x03a06002 }, 0xe254,  0x03a06008, "8MB VRAM RISC OS 4.39 (Adjust)" },
	{ 0x8a764, { 0xe1a00001, 0xe2801004, 0xeb00000d, 0x03a06002 }, 0x8a770, 0x03a06008, "8MB VRAM RISC OS 6.02" },
};

/**
 * Scan through the table of ROM patches, looking for a match with the current
 * ROM. If a match is found, make the required change and log the patch name.
 */
static void
romload_patch(void)
{
	const rom_patch_t *p;
	size_t i;

	for (i = 0, p = rom_patch; i < sizeof(rom_patch) / sizeof(rom_patch[0]); i++, p++) {
		uint32_t addr = p->addr_data;
		const uint32_t *data = p->data;

		if (rom[addr >> 2] == data[0] &&
		    rom[(addr + 4) >> 2] == data[1] &&
		    rom[(addr + 8) >> 2] == data[2] &&
		    rom[(addr + 12) >> 2] == data[3])
		{
			uint32_t replace = p->replace;

			// The VRAM-cap patches raise the OS's native 2MB VRAM limit.
			// Match the cap to the VRAM actually fitted: leave it alone on
			// 2MB (or no-VRAM) machines so the OS never addresses VRAM it
			// hasn't got, and raise it to 16MB when 16MB is fitted.
			if (replace == VRAM_CAP_MOV_8MB) {
				if (config.vram_size < 8) {
					continue;
				}
				if (config.vram_size >= 16) {
					replace = VRAM_CAP_MOV_16MB;
				}
			}

			// Patch the data
			rom[p->addr_replace >> 2] = replace;

			// Log the patch
			rpclog("romload: ROM patch applied: %s%s\n", p->comment,
			       replace == VRAM_CAP_MOV_16MB ? " (raised to 16MB)" : "");
		}
	}
}

/**
 * qsort comparison function for alphabetical sorting of
 *  C char *pointers. From the qsort() manpage
 *
 * @param p1 First item to compare
 * @param p2 Second item to compare
 * @return Integer less than, equal to, or greater than zero
 */
static int cmpstringp(const void *p1, const void *p2)
{
        /* The actual arguments to this function are "pointers to
           pointers to char", so assign to variables of this type.
           Then we dereference as we pass them to strcmp(). */

        const char * const *pstr1 = p1;
        const char * const *pstr2 = p2;

        return strcmp(*pstr1, *pstr2);
}

int
model_supports_32bit_rom(Model model)
{
	if (model < 0 || model >= Model_MAX) {
		return 0;
	}

	switch (models[model].cpu_model) {
	case CPUModel_SA110:
	case CPUModel_ARM810:
		return 1;
	default:
		return 0;
	}
}

/**
 * Parse the MOS title version from the start of a ROM image.
 * Ignores incidental "RISC OS 3.x or later" help text inside newer ROMs.
 */
static int
rom_mos_version_scan(const uint8_t *buf, size_t len, int *major, int *minor)
{
	size_t scan = len < ROM_PROBE_SCAN_BYTES ? len : ROM_PROBE_SCAN_BYTES;
	size_t best_off = (size_t) -1;
	int best_major = -1;
	int best_minor = -1;

	for (size_t i = 0; i + 16 < scan; i++) {
		int tabs = 0;
		const char *p;

		if (strncasecmp((const char *) &buf[i], "MOS Utilities", 13) == 0) {
			p = (const char *) &buf[i] + 13;
		} else if (strncasecmp((const char *) &buf[i], "RISC OS", 7) == 0) {
			p = (const char *) &buf[i] + 7;
		} else {
			continue;
		}

		while (*p == '\t') {
			tabs++;
			p++;
		}
		while (*p == ' ') {
			p++;
		}

		if (!isdigit((unsigned char) *p)) {
			continue;
		}

		const int maj = *p - '0';
		p++;
		if (*p != '.') {
			continue;
		}
		p++;

		if (!isdigit((unsigned char) *p)) {
			continue;
		}

		int min = 0;
		while (isdigit((unsigned char) *p)) {
			min = (min * 10) + (*p - '0');
			p++;
		}

		/* Help text such as "RISC OS 3.1 or later" is not the MOS title. */
		if (tabs < 2 && maj < 5) {
			if (strncmp(p, " or", 3) == 0) {
				continue;
			}
		}

		/* Classic MOS titles include a build date in parentheses. */
		if (tabs < 2 && maj < 5) {
			int has_date = 0;

			for (int k = 0; k < 24 && p[k] != '\0'; k++) {
				if (p[k] == '(') {
					has_date = 1;
					break;
				}
				if (p[k] == '\n') {
					break;
				}
			}
			if (!has_date) {
				continue;
			}
		}

		if (i < best_off) {
			best_off = i;
			best_major = maj;
			best_minor = min;
		}
	}

	if (best_major < 0) {
		return 0;
	}

	*major = best_major;
	*minor = best_minor;
	return 1;
}

static RomAddressing
rom_classify_addressing(const uint8_t *header, size_t header_size, size_t total_size,
                        char *detail, size_t detail_len)
{
	int major;
	int minor;

	if (total_size >= (6u * 1024u * 1024u)) {
		if (detail != NULL && detail_len > 0) {
			snprintf(detail, detail_len, "6 MB+ image");
		}
		return RomAddressing_32Bit;
	}

	if (rom_mos_version_scan(header, header_size, &major, &minor)) {
		if (detail != NULL && detail_len > 0) {
			snprintf(detail, detail_len, "RISC OS %d.%02d", major, minor);
		}
		if (major >= 5) {
			return RomAddressing_32Bit;
		}
		if (major >= 3 && major <= 4) {
			return RomAddressing_26Bit;
		}
	}

	if (total_size == (2u * 1024u * 1024u)) {
		if (detail != NULL && detail_len > 0) {
			snprintf(detail, detail_len, "2 MB image");
		}
		return RomAddressing_26Bit;
	}

	if (detail != NULL && detail_len > 0) {
		detail[0] = '\0';
	}
	return RomAddressing_Unknown;
}

/**
 * Scan a directory for ROM image files.
 */
static int
romload_scan_directory(const char *romdirectory, char **romfilenames)
{
	int number_of_files = 0;
	DIR *dir;
	const struct dirent *d;

	dir = opendir(romdirectory);
	if (dir == NULL) {
		return 0;
	}

	while ((d = readdir(dir)) != NULL && number_of_files < MAXROMS) {
		const char *ext = rpcemu_file_get_extension(d->d_name);
		char filepath[512];
		struct stat buf;

		snprintf(filepath, sizeof(filepath), "%s%s", romdirectory, d->d_name);

		if (stat(filepath, &buf) == 0) {
			if (S_ISREG(buf.st_mode) && (strcasecmp(ext, "txt") != 0) && d->d_name[0] != '.') {
				romfilenames[number_of_files] = strdup(d->d_name);
				if (romfilenames[number_of_files] == NULL) {
					fatal("Out of memory in loadroms()");
				}
				number_of_files++;
			}
		}
	}
	closedir(dir);

	return number_of_files;
}

/**
 * Resolve the ROM image set and read only enough data for compatibility probing.
 */
static int
rom_probe_collect(const char *rom_dir, uint8_t *header, size_t header_cap, size_t *header_size,
                  size_t *total_size)
{
	int number_of_files = 0;
	char *romfilenames[MAXROMS];
	char romdirectory[512];
	char romsubdir[512];
	struct stat st;
	size_t header_pos = 0;

	snprintf(romdirectory, sizeof(romdirectory), "%sroms/", rpcemu_get_datadir());

	if (rom_dir != NULL && rom_dir[0] != '\0') {
		snprintf(romsubdir, sizeof(romsubdir), "%sroms/%s/", rpcemu_get_datadir(), rom_dir);
		if (stat(romsubdir, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(romdirectory, sizeof(romdirectory), "%s", romsubdir);
			number_of_files = romload_scan_directory(romdirectory, romfilenames);
		} else {
			snprintf(romsubdir, sizeof(romsubdir), "%sroms/%s", rpcemu_get_datadir(), rom_dir);
			if (stat(romsubdir, &st) == 0 && S_ISREG(st.st_mode)) {
				romfilenames[0] = strdup(rom_dir);
				if (romfilenames[0] == NULL) {
					return -1;
				}
				number_of_files = 1;
			} else {
				return -1;
			}
		}
	} else {
		number_of_files = romload_scan_directory(romdirectory, romfilenames);
	}

	if (number_of_files == 0) {
		return -1;
	}

	qsort(romfilenames, number_of_files, sizeof(char *), cmpstringp);

	*total_size = 0;
	*header_size = 0;
	for (int c = 0; c < number_of_files; c++) {
		FILE *f;
		long len;
		char filepath[512];
		size_t to_read;

		snprintf(filepath, sizeof(filepath), "%s%s", romdirectory, romfilenames[c]);
		f = fopen(filepath, "rb");
		if (f == NULL) {
			for (int i = c; i < number_of_files; i++) {
				free(romfilenames[i]);
			}
			return -1;
		}

		fseek(f, 0, SEEK_END);
		len = ftell(f);
		if (len < 0) {
			fclose(f);
			for (int i = c; i < number_of_files; i++) {
				free(romfilenames[i]);
			}
			return -1;
		}

		*total_size += (size_t) len;

		if (header_pos < header_cap) {
			to_read = header_cap - header_pos;
			if ((size_t) len < to_read) {
				to_read = (size_t) len;
			}

			rewind(f);
			if (fread(header + header_pos, to_read, 1, f) != 1) {
				fclose(f);
				for (int i = c; i < number_of_files; i++) {
					free(romfilenames[i]);
				}
				return -1;
			}
			header_pos += to_read;
		}

		fclose(f);
		free(romfilenames[c]);
	}

	*header_size = header_pos;
	return 0;
}

RomAddressing
rom_probe_addressing(const char *rom_dir, char *detail, size_t detail_len)
{
	uint8_t header[ROM_PROBE_SCAN_BYTES];
	size_t header_size = 0;
	size_t total_size = 0;

	if (detail != NULL && detail_len > 0) {
		detail[0] = '\0';
	}

	if (rom_probe_collect(rom_dir, header, sizeof(header), &header_size, &total_size) != 0) {
		return RomAddressing_Unknown;
	}

	return rom_classify_addressing(header, header_size, total_size, detail, detail_len);
}

int
rom_model_is_compatible(Model model, const char *rom_dir, char *msg, size_t msg_len)
{
	char detail[64];
	const RomAddressing addressing = rom_probe_addressing(rom_dir, detail, sizeof(detail));

	if (model < 0 || model >= Model_MAX) {
		if (msg != NULL && msg_len > 0) {
			snprintf(msg, msg_len, "Unknown machine model.");
		}
		return 0;
	}

	if (addressing == RomAddressing_32Bit && !model_supports_32bit_rom(model)) {
		if (msg != NULL && msg_len > 0) {
			if (detail[0] != '\0') {
				snprintf(msg, msg_len,
				         "%s requires StrongARM (not %s).",
				         detail, models[model].name_gui);
			} else {
				snprintf(msg, msg_len,
				         "ROM requires StrongARM (not %s).",
				         models[model].name_gui);
			}
		}
		return 0;
	}

	return 1;
}

uint32_t rom_loaded_size = 0; /**< Size in bytes of the loaded ROM image */

/**
 * Load the ROM images, calls fatal() on error.
 */
void loadroms(void)
{
        int number_of_files = 0;
        int c;
        int pos = 0;
        char dirname[280];
        char *romfilenames[MAXROMS];
	char romdirectory[512];
	char romsubdir[512];
	struct stat st;
	char compat_msg[512];

	if (!rom_model_is_compatible(machine.model, config.rom_dir, compat_msg, sizeof(compat_msg))) {
		fatal("%s\n\n%s", compat_msg, ROM_WEB_SITE_STRING);
	}

	{
		char detail[64];
		const RomAddressing addressing = rom_probe_addressing(config.rom_dir, detail, sizeof(detail));

		switch (addressing) {
		case RomAddressing_32Bit:
			rpclog("romload: ROM requires 32-bit CPU%s%s\n",
			       detail[0] != '\0' ? " (" : "", detail[0] != '\0' ? detail : "");
			break;
		case RomAddressing_26Bit:
			rpclog("romload: ROM uses 26-bit addressing%s%s\n",
			       detail[0] != '\0' ? " (" : "", detail[0] != '\0' ? detail : "");
			break;
		default:
			rpclog("romload: ROM addressing mode could not be determined\n");
			break;
		}
	}

	/* Build default rom directory path */
	snprintf(romdirectory, sizeof(romdirectory), "%sroms/", rpcemu_get_datadir());

	if (config.rom_dir[0] != '\0') {
		/* rom_dir is a subfolder within roms/ (e.g. ROM530) */
		snprintf(romsubdir, sizeof(romsubdir), "%sroms/%s/", rpcemu_get_datadir(), config.rom_dir);
		if (stat(romsubdir, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(romdirectory, sizeof(romdirectory), "%s", romsubdir);
			snprintf(dirname, sizeof(dirname), "roms/%s", config.rom_dir);
			number_of_files = romload_scan_directory(romdirectory, romfilenames);
		} else {
			/* Legacy: treat rom_dir as a single filename in roms/ */
			snprintf(romsubdir, sizeof(romsubdir), "%sroms/%s", rpcemu_get_datadir(), config.rom_dir);
			if (stat(romsubdir, &st) == 0 && S_ISREG(st.st_mode)) {
				romfilenames[0] = strdup(config.rom_dir);
				if (romfilenames[0] == NULL) {
					fatal("Out of memory in loadroms()");
				}
				number_of_files = 1;
				snprintf(dirname, sizeof(dirname), "roms");
			} else {
				fatal("Could not find ROM directory or file 'roms/%s'\n\n"
				      ROM_WEB_SITE_STRING "\n",
				      config.rom_dir);
			}
		}
	} else {
		snprintf(dirname, sizeof(dirname), "roms");
		number_of_files = romload_scan_directory(romdirectory, romfilenames);
		if (number_of_files == 0) {
			fatal("Could not open ROM files directory '%s': %s\n",
			      romdirectory, strerror(errno));
		}
	}

        /* Empty directory? or only .txt files? */
        if (number_of_files == 0) {
                fatal("Could not load ROM files from directory '%s'\n\n"
                      ROM_WEB_SITE_STRING "\n",
                      dirname);
        }

        /* Sort filenames into alphabetical order */
        qsort(romfilenames, number_of_files, sizeof(char *), cmpstringp);

        /* Load files */
        for (c = 0; c < number_of_files; c++) {
                FILE *f;
                long len;
                char filepath[512];

                snprintf(filepath, sizeof(filepath), "%s%s", romdirectory, romfilenames[c]);

                f = fopen(filepath, "rb");
                if (f == NULL) {
                        fatal("Can't open ROM file '%s': %s", filepath,
                              strerror(errno));
                }

                /* Calculate file size */
                fseek(f, 0, SEEK_END);
                len = ftell(f);

                if (len < 0) {
                        fatal("Error reading size of ROM file '%s': %s",
                              romfilenames[c], strerror(errno));
                }

                if (len > ROMSIZE || pos + len > ROMSIZE) {
                        fatal("ROM files larger than 8MB");
                }

                /* Read file data */
                rewind(f);
                if (fread(&romb[pos], (size_t) len, 1, f) != 1) {
                        fatal("Error reading from ROM file '%s': %s",
                              romfilenames[c], strerror(errno));
                }

                fclose(f);

		rpclog("romload: Loaded '%s' %ld bytes\n", romfilenames[c], len);

                pos += len;

                /* Free up filename allocated earlier */
                free(romfilenames[c]);
        }

        /* Reject ROMs that are not sensible sizes
         * Allow 2MB (RISC OS 3.50)
         *       4MB (RISC OS 3.60 -> Half way through Select)
         *       6MB (Later Select)
         *       8MB (Current maximum)
         */
        if (pos != (2 * 1024 * 1024) && pos != (4 * 1024 * 1024)
            && pos != (6 * 1024 * 1024) && pos != (8 * 1024 * 1024))
        {
                fatal("ROM Image of unsupported size: expecting 2MB, 4MB, 6MB or 8MB, got %d bytes", pos);
        }

	rpclog("romload: Total ROM size %d MB\n", pos / 1048576);

	rom_loaded_size = (uint32_t) pos;

#ifdef _RPCEMU_BIG_ENDIAN
	/* Endian swap */
	for (c = 0; c < pos; c += 4) {
		uint32_t temp = rom[c >> 2];

		rom[c >> 2] = (temp >> 24) |
		              ((temp >> 8) & 0x0000ff00) |
		              ((temp << 8) & 0x00ff0000) |
		              (temp << 24);
	}
#endif

	/* Patch ROM  */
	romload_patch();

	/* Patch Netstation versions of NCOS to bypass the results of the POST that we currently fail */
	/* NCOS 0.10 */
	if (rom[0x2714 >> 2] == 0xe1d70000) {
		rom[0x2714 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
		rom[0x2794 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
	}

	/* NCOS 1.06/1.11 */
	if (rom[0x26f0 >> 2] == 0xe1d70000) {
		rom[0x26f0 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
		rom[0x2750 >> 2] = 0xe3b00000; /* MOVS r0, #0 */
	}
}
