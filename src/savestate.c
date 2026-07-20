/*
  RPCEmu - An Acorn system emulator

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

/* Suspend/resume machine state snapshots.

   File layout (all values little-endian):

     magic     8 bytes  "RPCESTAT"
     version   u32
     model     u32      Model enum of the machine that was suspended
     mem_size  u32      RAM in megabytes
     vram_size u32      VRAM in megabytes
     rom_size  u32      Size in bytes of the loaded ROM image
     rom_crc   u32      CRC32 of the loaded ROM image
     flags     u32      bit 0: written by a dynarec build (informational)
     cdrom_enabled u32  Whether the CD-ROM drive was enabled (affects
                        whether IDE drive 1 is a hard disc or ATAPI)
     cdrom_type    u32  config.cdromtype (only checked when enabled)
     name_len      u16  Length of the machine name that follows
     name          bytes  Machine name (for reporting mismatched loads)

   followed by tagged chunks until EOF:

     tag       4 bytes
     length    u32
     payload   'length' bytes

   Unknown chunks are skipped on load. Only guest-visible state is stored;
   derived state (TLBs, fast-path maps, compiled code blocks, cached video
   state) is rebuilt after loading, and host resources (open image files,
   sound device, network) are those of the running emulator. */

#include <string.h>

#include "rpcemu.h"
#include "savestate.h"
#include "arm.h"
#include "mem.h"
#include "romload.h"

#define SNAPSHOT_MAGIC		"RPCESTAT"
#define SNAPSHOT_VERSION	4

#define SNAPSHOT_FLAG_DYNAREC	(1u << 0)

int savestate_error = 0;

/* Serialization helpers. Multi-byte values are written explicitly
   little-endian so host struct padding and endianness never leak into the
   file format. Errors latch into savestate_error. */

void
savestate_write(FILE *f, const void *data, size_t len)
{
	if (len != 0 && fwrite(data, len, 1, f) != 1) {
		savestate_error = 1;
	}
}

void
savestate_write_u8(FILE *f, uint8_t v)
{
	savestate_write(f, &v, 1);
}

void
savestate_write_u16(FILE *f, uint16_t v)
{
	uint8_t b[2] = { (uint8_t) v, (uint8_t) (v >> 8) };

	savestate_write(f, b, 2);
}

void
savestate_write_u32(FILE *f, uint32_t v)
{
	uint8_t b[4] = {
		(uint8_t) v, (uint8_t) (v >> 8),
		(uint8_t) (v >> 16), (uint8_t) (v >> 24)
	};

	savestate_write(f, b, 4);
}

void
savestate_write_i32(FILE *f, int32_t v)
{
	savestate_write_u32(f, (uint32_t) v);
}

void
savestate_write_u64(FILE *f, uint64_t v)
{
	savestate_write_u32(f, (uint32_t) v);
	savestate_write_u32(f, (uint32_t) (v >> 32));
}

void
savestate_write_f64(FILE *f, double v)
{
	uint64_t u;

	memcpy(&u, &v, 8);
	savestate_write_u64(f, u);
}

void
savestate_read(FILE *f, void *data, size_t len)
{
	if (len != 0 && fread(data, len, 1, f) != 1) {
		memset(data, 0, len);
		savestate_error = 1;
	}
}

uint8_t
savestate_read_u8(FILE *f)
{
	uint8_t v = 0;

	savestate_read(f, &v, 1);
	return v;
}

uint16_t
savestate_read_u16(FILE *f)
{
	uint8_t b[2] = { 0, 0 };

	savestate_read(f, b, 2);
	return (uint16_t) (b[0] | (b[1] << 8));
}

uint32_t
savestate_read_u32(FILE *f)
{
	uint8_t b[4] = { 0, 0, 0, 0 };

	savestate_read(f, b, 4);
	return (uint32_t) b[0] | ((uint32_t) b[1] << 8) |
	       ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
}

int32_t
savestate_read_i32(FILE *f)
{
	return (int32_t) savestate_read_u32(f);
}

uint64_t
savestate_read_u64(FILE *f)
{
	uint64_t lo = savestate_read_u32(f);
	uint64_t hi = savestate_read_u32(f);

	return lo | (hi << 32);
}

double
savestate_read_f64(FILE *f)
{
	uint64_t u = savestate_read_u64(f);
	double v;

	memcpy(&v, &u, 8);
	return v;
}

/* Run-length encoding for the large payloads (RAM, VRAM, disc buffers).
   Guest memory is dominated by long runs of identical bytes, so this
   self-contained scheme recovers most of what a general compressor would.

   The encoded stream is:
     u32 uncompressed length
   followed by records until that many bytes have been reproduced:
     u32 count, high bit set:   one byte follows, repeated 'count' times
     u32 count, high bit clear: 'count' literal bytes follow */

#define RLE_MIN_RUN 16	/* Shortest run worth encoding as a repeat record */

void
savestate_write_rle(FILE *f, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t i = 0, lit_start = 0;

	savestate_write_u32(f, (uint32_t) len);

	while (i < len) {
		size_t run = 1;

		while (i + run < len && p[i + run] == p[i] &&
		       run < 0x7fffffff)
		{
			run++;
		}
		if (run >= RLE_MIN_RUN) {
			if (i > lit_start) {
				savestate_write_u32(f, (uint32_t) (i - lit_start));
				savestate_write(f, p + lit_start, i - lit_start);
			}
			savestate_write_u32(f, (uint32_t) run | 0x80000000);
			savestate_write_u8(f, p[i]);
			i += run;
			lit_start = i;
		} else {
			i += run;
		}
	}
	if (len > lit_start) {
		savestate_write_u32(f, (uint32_t) (len - lit_start));
		savestate_write(f, p + lit_start, len - lit_start);
	}
}

void
savestate_read_rle(FILE *f, void *data, size_t len)
{
	uint8_t *p = data;
	size_t pos = 0;

	if (savestate_read_u32(f) != (uint32_t) len) {
		savestate_error = 1;
		return;
	}
	while (pos < len && !savestate_error) {
		uint32_t c = savestate_read_u32(f);
		size_t n = c & 0x7fffffff;

		if (n == 0 || n > len - pos) {
			savestate_error = 1;
			return;
		}
		if (c & 0x80000000) {
			memset(p + pos, savestate_read_u8(f), n);
		} else {
			savestate_read(f, p + pos, n);
		}
		pos += n;
	}
}

/**
 * Calculate the CRC32 (IEEE 802.3 polynomial) of a block of data.
 * Used to verify the ROM image matches between suspend and resume.
 */
static uint32_t
crc32_buf(const uint8_t *data, size_t len)
{
	static uint32_t table[256];
	static int table_built = 0;
	uint32_t crc;
	size_t i;

	if (!table_built) {
		uint32_t c;
		int n, k;

		for (n = 0; n < 256; n++) {
			c = (uint32_t) n;
			for (k = 0; k < 8; k++) {
				c = (c & 1) ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
			}
			table[n] = c;
		}
		table_built = 1;
	}

	crc = 0xffffffff;
	for (i = 0; i < len; i++) {
		crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	}
	return crc ^ 0xffffffff;
}

/* Floppy disc image paths. Written by savestate.c itself because the
   knowledge of which image is in which drive lives in rpcemu.c (discname).
   Must be loaded before the FDC/DISC/ADF/HFE chunks so the correct images
   are mounted before their in-flight state is overlaid. */
static void
flpy_savestate(FILE *f)
{
	savestate_write(f, discname[0], sizeof(discname[0]));
	savestate_write(f, discname[1], sizeof(discname[1]));
}

static void
flpy_loadstate(FILE *f)
{
	char name[2][260];
	int d;

	savestate_read(f, name[0], sizeof(name[0]));
	savestate_read(f, name[1], sizeof(name[1]));
	name[0][sizeof(name[0]) - 1] = '\0';
	name[1][sizeof(name[1]) - 1] = '\0';

	for (d = 0; d < 2; d++) {
		if (strcmp(name[d], discname[d]) != 0) {
			rpcemu_floppy_load(d, name[d]);
		}
	}
}

typedef struct {
	const char tag[5];
	void (*save)(FILE *f);
	void (*load)(FILE *f);
} SnapshotChunk;

/* Save order is load order; FLPY must precede the floppy state chunks */
static const SnapshotChunk snapshot_chunks[] = {
	{ "ARM ", arm_savestate,      arm_loadstate      },
	{ "CP15", cp15_savestate,     cp15_loadstate     },
	{ "FPA ", fpa_savestate,      fpa_loadstate      },
	{ "MEM ", mem_savestate,      mem_loadstate      },
	{ "IOMD", iomd_savestate,     iomd_loadstate     },
	{ "VIDC", vidc20_savestate,   vidc20_loadstate   },
	{ "SIO ", superio_savestate,  superio_loadstate  },
	{ "8042", i8042_savestate,    i8042_loadstate    },
	{ "KBD ", keyboard_savestate, keyboard_loadstate },
	{ "IDE ", ide_savestate,      ide_loadstate      },
	{ "ICS ", icside_savestate,   icside_loadstate   },
	{ "FLPY", flpy_savestate,     flpy_loadstate     },
	{ "FDC ", fdc_savestate,      fdc_loadstate      },
	{ "DISC", disc_savestate,     disc_loadstate     },
	{ "ADF ", adf_savestate,      adf_loadstate      },
	{ "HFE ", hfe_savestate,      hfe_loadstate      },
	{ "CMOS", cmos_savestate,     cmos_loadstate     },
	{ "SND ", sound_savestate,    sound_loadstate    },
	{ "EXEC", rpcemu_savestate,   rpcemu_loadstate   },
	{ "HFS ", hostfs_savestate,   hostfs_loadstate   },
#ifdef RPCEMU_NETWORKING
	{ "NET ", network_savestate,  network_loadstate  },
#endif
};

#define SNAPSHOT_CHUNK_COUNT \
	(sizeof(snapshot_chunks) / sizeof(snapshot_chunks[0]))

static void
write_header(FILE *f)
{
	uint32_t flags = 0;

	if (arm_is_dynarec()) {
		flags |= SNAPSHOT_FLAG_DYNAREC;
	}

	savestate_write(f, SNAPSHOT_MAGIC, 8);
	savestate_write_u32(f, SNAPSHOT_VERSION);
	savestate_write_u32(f, (uint32_t) machine.model);
	savestate_write_u32(f, config.mem_size);
	savestate_write_u32(f, config.vram_size);
	savestate_write_u32(f, rom_loaded_size);
	savestate_write_u32(f, crc32_buf(romb, rom_loaded_size));
	savestate_write_u32(f, flags);
	savestate_write_u32(f, config.cdromenabled ? 1 : 0);
	savestate_write_u32(f, (uint32_t) config.cdromtype);

	/* Machine name, so a mismatched load can report which machine the
	   snapshot belongs to. Length-prefixed, capped at 255 bytes. */
	{
		size_t name_len = strlen(config.name);

		if (name_len > 255) {
			name_len = 255;
		}
		savestate_write_u16(f, (uint16_t) name_len);
		savestate_write(f, config.name, name_len);
	}
}

/**
 * Read and validate the snapshot header against the running configuration.
 *
 * @param f          Open snapshot file positioned at the start
 * @param errbuf     Filled with a description of any mismatch
 * @param errbuf_len Size of errbuf
 * @return 0 if the snapshot is compatible
 */
static int
check_header(FILE *f, char *errbuf, size_t errbuf_len)
{
	char magic[8];
	uint32_t version, model, mem_size, vram_size, rom_size, rom_crc;
	uint32_t cdrom_enabled, cdrom_type;
	char snapshot_name[256];
	uint16_t name_len, name_copy;

	savestate_read(f, magic, 8);
	if (savestate_error || memcmp(magic, SNAPSHOT_MAGIC, 8) != 0) {
		snprintf(errbuf, errbuf_len, "This is not an RPCEmu suspend file.");
		return -1;
	}

	version = savestate_read_u32(f);
	if (version != SNAPSHOT_VERSION) {
		snprintf(errbuf, errbuf_len,
		         "This suspend file (version %u) was made by a different "
		         "version of RPCEmu and cannot be loaded.", version);
		return -1;
	}

	model     = savestate_read_u32(f);
	mem_size  = savestate_read_u32(f);
	vram_size = savestate_read_u32(f);
	rom_size  = savestate_read_u32(f);
	rom_crc   = savestate_read_u32(f);
	savestate_read_u32(f); /* flags - informational only */
	cdrom_enabled = savestate_read_u32(f);
	cdrom_type    = savestate_read_u32(f);

	/* Machine name (length-prefixed). Consume exactly name_len bytes so the
	   file position is left at the first chunk for state_load(). */
	name_len = savestate_read_u16(f);
	name_copy = (name_len < sizeof(snapshot_name) - 1)
	            ? name_len : (uint16_t) (sizeof(snapshot_name) - 1);
	savestate_read(f, snapshot_name, name_copy);
	snapshot_name[name_copy] = '\0';
	if (name_len > name_copy) {
		fseek(f, name_len - name_copy, SEEK_CUR);
	}
	if (snapshot_name[0] == '\0') {
		snprintf(snapshot_name, sizeof(snapshot_name), "(unnamed)");
	}

	if (savestate_error) {
		snprintf(errbuf, errbuf_len, "The suspend file is truncated or corrupt.");
		return -1;
	}
	if (model != (uint32_t) machine.model) {
		snprintf(errbuf, errbuf_len,
		         "This snapshot belongs to machine '%s', which is a different "
		         "machine model to the current machine '%s'.",
		         snapshot_name, config.name);
		return -1;
	}
	if (mem_size != config.mem_size || vram_size != config.vram_size) {
		snprintf(errbuf, errbuf_len,
		         "This snapshot belongs to machine '%s' (%u MB RAM, %u MB VRAM), "
		         "which does not match the current machine '%s' (%u MB RAM, %u MB VRAM).",
		         snapshot_name, mem_size, vram_size,
		         config.name, config.mem_size, config.vram_size);
		return -1;
	}
	if (rom_size != rom_loaded_size ||
	    rom_crc != crc32_buf(romb, rom_loaded_size))
	{
		snprintf(errbuf, errbuf_len,
		         "This snapshot belongs to machine '%s', which uses a different "
		         "ROM image to the current machine '%s'.",
		         snapshot_name, config.name);
		return -1;
	}
	if (cdrom_enabled != (uint32_t) (config.cdromenabled ? 1 : 0) ||
	    (cdrom_enabled && cdrom_type != (uint32_t) config.cdromtype))
	{
		snprintf(errbuf, errbuf_len,
		         "This snapshot belongs to machine '%s', which has a different "
		         "CD-ROM configuration to the current machine '%s'.",
		         snapshot_name, config.name);
		return -1;
	}

	return 0;
}

/**
 * Write the complete machine state to 'path'.
 *
 * Must be called from the emulator thread between execrpcemu() calls, so
 * that no instruction (or dynarec block) is mid-execution.
 *
 * @param path Filename to write the snapshot to
 * @return 0 on success
 */
int
state_save(const char *path)
{
	char tmppath[1024];
	FILE *f;
	size_t c;

	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

	f = fopen(tmppath, "wb");
	if (f == NULL) {
		rpclog("state_save: cannot create '%s'\n", tmppath);
		return -1;
	}

	savestate_error = 0;
	write_header(f);

	for (c = 0; c < SNAPSHOT_CHUNK_COUNT; c++) {
		long len_pos, end_pos;

		savestate_write(f, snapshot_chunks[c].tag, 4);
		len_pos = ftell(f);
		savestate_write_u32(f, 0); /* length, patched below */

		snapshot_chunks[c].save(f);

		end_pos = ftell(f);
		if (len_pos < 0 || end_pos < 0 ||
		    fseek(f, len_pos, SEEK_SET) != 0)
		{
			savestate_error = 1;
			break;
		}
		savestate_write_u32(f, (uint32_t) (end_pos - len_pos - 4));
		if (fseek(f, end_pos, SEEK_SET) != 0) {
			savestate_error = 1;
			break;
		}
	}

	if (fflush(f) != 0) {
		savestate_error = 1;
	}
	if (fclose(f) != 0) {
		savestate_error = 1;
	}

	if (savestate_error) {
		rpclog("state_save: error writing '%s'\n", tmppath);
		remove(tmppath);
		return -1;
	}

	remove(path);
	if (rename(tmppath, path) != 0) {
		rpclog("state_save: cannot rename '%s' to '%s'\n",
		       tmppath, path);
		remove(tmppath);
		return -1;
	}

	rpclog("state_save: machine state saved to '%s'\n", path);
	return 0;
}

/**
 * Check that a snapshot file exists and matches the current configuration.
 *
 * @param path       Filename of the snapshot
 * @param errbuf     Filled with a description of any mismatch
 * @param errbuf_len Size of errbuf
 * @return 0 if the snapshot can be resumed
 */
/**
 * Read the machine name recorded in a snapshot's header, without loading
 * or validating the machine state. Used by the GUI to boot the correct
 * machine when a snapshot file is opened directly.
 *
 * @param path      Filename of the snapshot
 * @param name      Buffer filled with the machine name (always terminated)
 * @param name_len  Size of the buffer
 * @return 0 on success, -1 if the file is missing or not a snapshot
 */
int
state_get_machine_name(const char *path, char *name, size_t name_len)
{
	FILE *f;
	char magic[8];
	uint32_t version;
	uint16_t stored_len, copy;
	int saved_error = savestate_error;
	int ret = -1;

	if (name_len > 0) {
		name[0] = '\0';
	}

	f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}

	savestate_error = 0;
	savestate_read(f, magic, 8);
	version = savestate_read_u32(f);
	if (!savestate_error && memcmp(magic, SNAPSHOT_MAGIC, 8) == 0 &&
	    version == SNAPSHOT_VERSION)
	{
		/* Skip the eight u32 config words (model, mem, vram, rom size,
		   rom crc, flags, cdrom enabled, cdrom type) to reach the name */
		fseek(f, 8 * 4, SEEK_CUR);
		stored_len = savestate_read_u16(f);
		copy = (name_len > 0 && stored_len < name_len - 1)
		       ? stored_len
		       : (uint16_t) (name_len > 0 ? name_len - 1 : 0);
		savestate_read(f, name, copy);
		if (name_len > 0) {
			name[copy] = '\0';
		}
		if (!savestate_error) {
			ret = 0;
		}
	}

	fclose(f);
	savestate_error = saved_error;
	return ret;
}

int
state_check(const char *path, char *errbuf, size_t errbuf_len)
{
	FILE *f;
	int ret;
	int saved_error = savestate_error;

	f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(errbuf, errbuf_len, "Cannot open suspend file");
		return -1;
	}

	savestate_error = 0;
	ret = check_header(f, errbuf, errbuf_len);
	fclose(f);

	savestate_error = saved_error;
	return ret;
}

/**
 * Restore the machine state from 'path'.
 *
 * Must be called from the emulator thread (or before it has started),
 * after rpcemu_start() has initialised all subsystems. The machine is
 * reset first; on any failure it is reset again, leaving a clean cold
 * boot. Returns 0 on success.
 *
 * @param path Filename of the snapshot
 * @return 0 on success
 */
int
state_load(const char *path)
{
	char errbuf[256];
	FILE *f;

	f = fopen(path, "rb");
	if (f == NULL) {
		rpclog("state_load: cannot open '%s'\n", path);
		return -1;
	}

	savestate_error = 0;
	if (check_header(f, errbuf, sizeof(errbuf)) != 0) {
		rpclog("state_load: %s\n", errbuf);
		fclose(f);
		return -1;
	}

	/* Take all emulated hardware to a known baseline; the chunks below
	   overlay the guest-visible state on top of it */
	resetrpc();

	for (;;) {
		char tag[4];
		uint32_t len;
		long payload_start;
		size_t c;

		if (fread(tag, 4, 1, f) != 1) {
			break; /* end of file */
		}
		len = savestate_read_u32(f);
		payload_start = ftell(f);
		if (savestate_error || payload_start < 0) {
			break;
		}

		for (c = 0; c < SNAPSHOT_CHUNK_COUNT; c++) {
			if (memcmp(tag, snapshot_chunks[c].tag, 4) == 0) {
				snapshot_chunks[c].load(f);
				break;
			}
		}

		if (savestate_error) {
			break;
		}

		if (c < SNAPSHOT_CHUNK_COUNT &&
		    ftell(f) != payload_start + (long) len)
		{
			rpclog("state_load: chunk '%.4s' size mismatch\n", tag);
			savestate_error = 1;
			break;
		}

		/* Position after the payload (also skips unknown chunks) */
		if (fseek(f, payload_start + (long) len, SEEK_SET) != 0) {
			savestate_error = 1;
			break;
		}
	}

	fclose(f);

	if (savestate_error) {
		rpclog("state_load: error reading '%s', resetting machine\n",
		       path);
		resetrpc();
		return -1;
	}

	/* Rebuild derived state that the chunks cannot restore:
	   - compiled code blocks reference host addresses; flush them all
	     and let the dynarec recompile on demand (no-op on interpreter)
	   - IRQ/FIQ lines in arm.event are recomputed from the restored
	     IOMD state for consistency */
	resetcodeblocks();
	updateirqs();

	rpclog("state_load: machine state restored from '%s'\n", path);
	return 0;
}
