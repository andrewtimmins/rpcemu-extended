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
 * unzip.h - reading PKZIP archives that carry RISC OS file metadata.
 *
 * A self-contained reader for stored and deflated archives, with no
 * dependency on zlib or on any host archiving library, so it builds
 * unchanged on Linux, Windows and macOS.
 *
 * Archives produced on RISC OS record each file's load and execution
 * addresses in an "Acorn" extra field, which is where the filetype lives.
 * This reader surfaces that metadata, and can write an extracted tree out
 * using the ",xxx" leafname convention HostFS expects, so an archive such
 * as ROOL's HardDisc4 lands on a machine's HostFS with its filetypes intact
 * and boots as it would on real hardware.
 */

#ifndef UNZIP_H
#define UNZIP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Result codes. Negative so a count-returning call can share the space. */
#define UNZIP_OK		0
#define UNZIP_ERR_OPEN		-1	/**< Archive could not be opened */
#define UNZIP_ERR_FORMAT	-2	/**< Not a zip, or a malformed one */
#define UNZIP_ERR_CRC		-3	/**< Data did not match its checksum */
#define UNZIP_ERR_UNSUPPORTED	-4	/**< Compression method we do not have */
#define UNZIP_ERR_WRITE		-5	/**< Could not create or write output */
#define UNZIP_ERR_MEMORY	-6	/**< Allocation failed */
#define UNZIP_ERR_INFLATE	-7	/**< Compressed data is corrupt */
#define UNZIP_ERR_TOOBIG	-8	/**< Entry exceeds the size limit */
#define UNZIP_ERR_CANCELLED	-9	/**< Caller's progress callback said stop */

/* Compression methods understood. */
#define UNZIP_METHOD_STORE	0
#define UNZIP_METHOD_DEFLATE	8

/*
 * Largest single member we will decompress. An archive states its own sizes,
 * so without a ceiling a small hostile file can demand an enormous allocation.
 * ROOL's 4MB softload ROM is the biggest thing we legitimately extract.
 */
#define UNZIP_MAX_ENTRY_SIZE	(64u * 1024u * 1024u)

/*
 * Longest member name accepted. The format allows 64KB; anything remotely
 * near that is hostile rather than useful. HardDisc4's longest is 112.
 */
#define UNZIP_MAX_NAME		1024

/** One member of an archive. */
typedef struct {
	const char *name;		/**< Member name, '/' separated, as stored */
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t crc32;
	uint32_t local_header_offset;

	/* RISC OS metadata from the Acorn extra field, when present. */
	uint32_t load_addr;		/**< Load address; holds the filetype */
	uint32_t exec_addr;		/**< Execution address */
	uint32_t attributes;		/**< RISC OS access attributes */
	bool has_riscos_info;		/**< The three fields above are valid */

	uint16_t method;		/**< UNZIP_METHOD_* */
	bool is_directory;		/**< Name ended in '/' */
} UnzipEntry;

/** An open archive, with its central directory already read. */
typedef struct {
	FILE *fp;
	UnzipEntry *entries;		/**< entry_count members */
	char *name_pool;		/**< Backing store for every entry->name */
	int entry_count;
} UnzipArchive;

/**
 * Progress reporting during unzip_extract_all().
 *
 * @param done    Members written so far
 * @param total   Members that will be written
 * @param name    Member just written
 * @param opaque  Caller's context
 * @return        true to carry on, false to abandon the extraction
 */
typedef bool (*UnzipProgressFn)(int done, int total, const char *name,
                                void *opaque);

/** How unzip_extract_all() should lay the archive out on the host. */
typedef struct {
	/**
	 * Leading path components to drop from every member. HardDisc4 wraps
	 * its whole tree in a "HardDisc4/" directory which should not appear
	 * on the machine's HostFS, so that wants 1.
	 */
	int strip_components;

	/**
	 * Append HostFS's ",xxx" filetype suffix (or ",llll-eeee" for an
	 * untyped file) to each leafname, taken from the Acorn extra field.
	 * Members with no RISC OS metadata are written under their plain name.
	 */
	bool apply_filetypes;

	/** Optional; may be NULL. */
	UnzipProgressFn progress;
	void *progress_opaque;
} UnzipExtractOptions;

/**
 * Open an archive and read its central directory.
 *
 * On success the caller owns @archive and must pass it to unzip_close().
 *
 * @return UNZIP_OK, or a negative UNZIP_ERR_*
 */
int unzip_open(UnzipArchive *archive, const char *path);

/** Release everything unzip_open() acquired. Safe on an already-closed archive. */
void unzip_close(UnzipArchive *archive);

/** Number of members, or 0 if @archive is NULL. */
int unzip_entry_count(const UnzipArchive *archive);

/**
 * Borrow a member by index. The result points into @archive and stays valid
 * until unzip_close().
 *
 * @return The entry, or NULL if @index is out of range
 */
const UnzipEntry *unzip_entry(const UnzipArchive *archive, int index);

/**
 * Find a member by its full stored name, case insensitively.
 *
 * @return Index of the member, or -1 if there is no such member
 */
int unzip_find(const UnzipArchive *archive, const char *name);

/**
 * Decompress a member into a caller-supplied buffer, which must be at least
 * entry->uncompressed_size bytes. The member's CRC is verified.
 *
 * @return UNZIP_OK, or a negative UNZIP_ERR_*
 */
int unzip_extract_to_memory(UnzipArchive *archive, const UnzipEntry *entry,
                            uint8_t *buffer, size_t buffer_size);

/**
 * Decompress a member straight to a host file, under exactly the name given.
 * Intended for pulling a single known member out of an archive, such as the
 * ROM image inside a softload download.
 *
 * @return UNZIP_OK, or a negative UNZIP_ERR_*
 */
int unzip_extract_to_file(UnzipArchive *archive, const UnzipEntry *entry,
                          const char *dest_path);

/**
 * Extract every member below @dest_dir, creating directories as needed.
 *
 * Member names are used as host paths unchanged. That is deliberate: RISC OS
 * archives already swap '.' and '/' when storing a name, which is the same
 * swap HostFS performs, so a member named "!Boot/Choices" is precisely the
 * host path wanted. Only the filetype suffix is added, when asked for.
 *
 * @param options May be NULL for defaults (no stripping, no filetypes)
 * @return Number of files written, or a negative UNZIP_ERR_*
 */
int unzip_extract_all(UnzipArchive *archive, const char *dest_dir,
                      const UnzipExtractOptions *options);

/**
 * Build the leafname HostFS expects for a member: "name,xxx" for a typed
 * file, "name,llllllll-eeeeeeee" for one with only load and exec addresses,
 * or the bare name when the archive carried no RISC OS metadata.
 *
 * @return true if the result fitted in @out
 */
bool unzip_hostfs_leafname(const UnzipEntry *entry, const char *leaf,
                           char *out, size_t out_size);

/**
 * CRC-32 of a buffer, as the zip format computes it.
 *
 * Start with @crc of 0; feeding the result back in continues the sum across
 * successive chunks. Exposed so a caller can ask whether a file it already has
 * is the same content as a member of an archive, without unpacking it.
 */
uint32_t unzip_crc32(uint32_t crc, const uint8_t *data, size_t len);

/** True if @path looks like a zip archive. Cheap; reads only the signature. */
bool unzip_is_zip_file(const char *path);

/** Human-readable form of a UNZIP_ERR_* code, for logs and error boxes. */
const char *unzip_error_string(int error);

#endif /* UNZIP_H */
