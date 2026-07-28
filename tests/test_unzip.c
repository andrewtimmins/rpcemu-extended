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
 * test_unzip.c - the zip reader used to unpack downloaded RISC OS archives
 *
 * The archives this reads come off the network, and what it writes lands on a
 * machine's HostFS, so two things matter beyond "does it decompress": that a
 * member's RISC OS filetype survives into the host leafname, and that no
 * member can write outside the directory it was pointed at.
 *
 * Archives are built here rather than committed, so the test states its own
 * inputs and needs no network and no fixture files.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "unzip.h"

/* unzip.c logs through the emulator's logger; the test only needs it to exist. */
void
rpclog(const char *format, ...)
{
	va_list ap;

	if (getenv("TEST_UNZIP_VERBOSE") == NULL) {
		return;
	}
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

static int failures = 0;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);	\
			printf(__VA_ARGS__);				\
			printf("\n");					\
			failures++;					\
		}							\
	} while (0)

/* ------------------------------------------------------------------------ */
/* Building an archive to read back                                          */
/* ------------------------------------------------------------------------ */

#define MAX_MEMBERS 16

typedef struct {
	const char *name;
	const uint8_t *data;		/* Bytes as they appear in the archive */
	uint32_t data_len;
	uint32_t plain_len;		/* Uncompressed length */
	uint32_t crc;
	uint16_t method;
	bool acorn;			/* Emit an Acorn extra field */
	uint32_t load;
	uint32_t exec;
	uint32_t local_offset;		/* Filled in as it is written */
} Member;

static uint32_t crc_table[256];

static void
crc_init(void)
{
	unsigned n, k;

	for (n = 0; n < 256; n++) {
		uint32_t c = n;

		for (k = 0; k < 8; k++) {
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		}
		crc_table[n] = c;
	}
}

static uint32_t
crc_of(const uint8_t *buf, size_t len)
{
	uint32_t c = 0xffffffffu;

	while (len--) {
		c = crc_table[(c ^ *buf++) & 0xff] ^ (c >> 8);
	}
	return c ^ 0xffffffffu;
}

static void
put16(FILE *fp, uint16_t v)
{
	fputc(v & 0xff, fp);
	fputc((v >> 8) & 0xff, fp);
}

static void
put32(FILE *fp, uint32_t v)
{
	put16(fp, (uint16_t) (v & 0xffff));
	put16(fp, (uint16_t) (v >> 16));
}

/** The Acorn extra field: "AC", then a 20-byte ARC0 body. */
static void
put_acorn_extra(FILE *fp, const Member *m)
{
	put16(fp, 0x4341);
	put16(fp, 20);
	fwrite("ARC0", 1, 4, fp);
	put32(fp, m->load);
	put32(fp, m->exec);
	put32(fp, 3);		/* Access attributes */
	put32(fp, 0);
}

static uint16_t
extra_len_of(const Member *m)
{
	return m->acorn ? 24 : 0;
}

static void
write_archive(const char *path, Member *members, int count)
{
	FILE *fp = fopen(path, "wb");
	long cd_offset, cd_end;
	int i;

	if (fp == NULL) {
		printf("FAIL: cannot create %s\n", path);
		exit(1);
	}

	for (i = 0; i < count; i++) {
		Member *m = &members[i];

		m->local_offset = (uint32_t) ftell(fp);
		put32(fp, 0x04034b50);
		put16(fp, 20);			/* Version needed */
		put16(fp, 0);			/* Flags */
		put16(fp, m->method);
		put16(fp, 0);			/* Time */
		put16(fp, 0);			/* Date */
		put32(fp, m->crc);
		put32(fp, m->data_len);
		put32(fp, m->plain_len);
		put16(fp, (uint16_t) strlen(m->name));
		put16(fp, extra_len_of(m));
		fwrite(m->name, 1, strlen(m->name), fp);
		if (m->acorn) {
			put_acorn_extra(fp, m);
		}
		if (m->data_len > 0) {
			fwrite(m->data, 1, m->data_len, fp);
		}
	}

	cd_offset = ftell(fp);
	for (i = 0; i < count; i++) {
		const Member *m = &members[i];

		put32(fp, 0x02014b50);
		put16(fp, 20);			/* Version made by */
		put16(fp, 20);			/* Version needed */
		put16(fp, 0);			/* Flags */
		put16(fp, m->method);
		put16(fp, 0);
		put16(fp, 0);
		put32(fp, m->crc);
		put32(fp, m->data_len);
		put32(fp, m->plain_len);
		put16(fp, (uint16_t) strlen(m->name));
		put16(fp, extra_len_of(m));
		put16(fp, 0);			/* Comment length */
		put16(fp, 0);			/* Disk number */
		put16(fp, 0);			/* Internal attributes */
		put32(fp, 0);			/* External attributes */
		put32(fp, m->local_offset);
		fwrite(m->name, 1, strlen(m->name), fp);
		if (m->acorn) {
			put_acorn_extra(fp, m);
		}
	}
	cd_end = ftell(fp);

	put32(fp, 0x06054b50);
	put16(fp, 0);
	put16(fp, 0);
	put16(fp, (uint16_t) count);
	put16(fp, (uint16_t) count);
	put32(fp, (uint32_t) (cd_end - cd_offset));
	put32(fp, (uint32_t) cd_offset);
	put16(fp, 0);

	fclose(fp);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static bool
file_contains(const char *path, const void *expect, size_t len)
{
	uint8_t buf[512];
	size_t got;
	FILE *fp = fopen(path, "rb");

	if (fp == NULL) {
		return false;
	}
	got = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);

	return got == len && memcmp(buf, expect, len) == 0;
}

static bool
file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/*
 * A real deflate stream, so the Huffman path is exercised and not just the
 * stored one. Produced from the plain text below by a standard deflater.
 */
static const uint8_t deflated[] = {
	0x0b, 0xf2, 0x0c, 0x76, 0x56, 0xf0, 0x0f, 0x56, 0x08, 0xa2, 0x11, 0xed,
	0x98, 0x9c, 0x5f, 0x94, 0xa7, 0x10, 0x94, 0x59, 0x9c, 0xac, 0x10, 0xe0,
	0xcc, 0x05, 0x00
};

static const char deflated_plain[] =
	"RISC OS RISC OS RISC OS RISC OS RISC OS RISC OS RISC OS RISC OS "
	"RISC OS RISC OS RISC OS RISC OS Acorn Risc PC\n";

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

/**
 * A representative archive: the RISC OS metadata, a nested directory, both
 * compression methods, and the leafname spellings HostFS expects back.
 */
static void
test_extract_tree(void)
{
	static const char obey[] = "| Obey file\n";
	static const char text[] = "plain text\n";
	static const char data[] = "\x01\x02\x03\x04";
	Member members[] = {
		/* A directory, flagged by the trailing separator. */
		{ "Disc/", NULL, 0, 0, 0, UNZIP_METHOD_STORE,
		  true, 0xffffff5c, 0, 0 },
		/* Obey (&FEB): the suffix must appear. */
		{ "Disc/!Boot", (const uint8_t *) obey, sizeof(obey) - 1,
		  sizeof(obey) - 1, 0, UNZIP_METHOD_STORE,
		  true, 0xffffeb52, 0, 0 },
		/* Text (&FFF) is HostFS's default, so it carries no suffix. */
		{ "Disc/ReadMe", (const uint8_t *) text, sizeof(text) - 1,
		  sizeof(text) - 1, 0, UNZIP_METHOD_STORE,
		  true, 0xffffff47, 0, 0 },
		/* No filetype, so the addresses themselves are the metadata. */
		{ "Disc/Sub/Absolute", (const uint8_t *) data, 4, 4, 0,
		  UNZIP_METHOD_STORE, true, 0x00008000, 0x00008000, 0 },
		/* Deflated, to exercise the Huffman decoder. */
		{ "Disc/Sub/Squashed", deflated, sizeof(deflated),
		  sizeof(deflated_plain) - 1, 0, UNZIP_METHOD_DEFLATE,
		  true, 0xfffffb00, 0, 0 },
		/* A name that already ends in something HostFS would read as a
		   filetype. The suffix must be written even though the type is
		   the default, or the file comes back as &ABC. */
		{ "Disc/Odd,abc", (const uint8_t *) text, sizeof(text) - 1,
		  sizeof(text) - 1, 0, UNZIP_METHOD_STORE,
		  true, 0xffffff47, 0, 0 },
	};
	const int count = (int) (sizeof(members) / sizeof(members[0]));
	UnzipExtractOptions options;
	UnzipArchive archive;
	int i, r;

	for (i = 0; i < count; i++) {
		members[i].crc = (members[i].method == UNZIP_METHOD_DEFLATE)
		    ? crc_of((const uint8_t *) deflated_plain,
		             sizeof(deflated_plain) - 1)
		    : crc_of(members[i].data, members[i].data_len);
	}

	write_archive("test_unzip_tree.zip", members, count);

	r = unzip_open(&archive, "test_unzip_tree.zip");
	CHECK(r == UNZIP_OK, "open failed: %s", unzip_error_string(r));
	if (r != UNZIP_OK) {
		return;
	}

	CHECK(unzip_entry_count(&archive) == count, "expected %d entries, got %d",
	      count, unzip_entry_count(&archive));

	/* The extra field must have been read, not skipped over. */
	{
		int idx = unzip_find(&archive, "disc/!boot");	/* case folded */
		const UnzipEntry *e;

		CHECK(idx >= 0, "case-insensitive find failed");
		e = unzip_entry(&archive, idx);
		CHECK(e != NULL && e->has_riscos_info,
		      "Acorn extra field not parsed");
		CHECK(e != NULL && ((e->load_addr >> 8) & 0xfff) == 0xfeb,
		      "wrong filetype decoded");
		CHECK(e != NULL && !e->is_directory, "!Boot read as a directory");
	}
	CHECK(unzip_entry(&archive, 0)->is_directory,
	      "trailing separator not read as a directory");
	CHECK(unzip_entry(&archive, count) == NULL,
	      "out-of-range index not rejected");

	memset(&options, 0, sizeof(options));
	options.strip_components = 1;	/* Drop the wrapping "Disc/" */
	options.apply_filetypes = true;

	r = unzip_extract_all(&archive, "test_unzip_out", &options);
	CHECK(r == count - 1, "expected %d files written, got %d", count - 1, r);

	CHECK(file_contains("test_unzip_out/!Boot,feb", obey, sizeof(obey) - 1),
	      "Obey file missing, mistyped or corrupt");
	CHECK(file_contains("test_unzip_out/ReadMe", text, sizeof(text) - 1),
	      "text file should carry no suffix");
	CHECK(!file_exists("test_unzip_out/ReadMe,fff"),
	      "default filetype should not be spelled out");
	CHECK(file_contains("test_unzip_out/Sub/Absolute,8000-8000", data, 4),
	      "load-exec form missing or wrong");
	CHECK(file_contains("test_unzip_out/Sub/Squashed,ffb", deflated_plain,
	                    sizeof(deflated_plain) - 1),
	      "deflated member did not round-trip");
	CHECK(file_exists("test_unzip_out/Odd,abc,fff"),
	      "ambiguous leafname needs its filetype spelled out");

	unzip_close(&archive);
	CHECK(archive.entries == NULL && archive.fp == NULL,
	      "close did not release the archive");
}

/** A member must not be able to write outside the destination directory. */
static void
test_rejects_escaping_members(void)
{
	static const char payload[] = "no";
	static const char *const escapes[] = {
		"../escaped",
		"a/../../escaped",
		"/absolute",
		"sub\\escaped",		/* Windows separator smuggled in */
	};
	size_t i;

	for (i = 0; i < sizeof(escapes) / sizeof(escapes[0]); i++) {
		Member m = { escapes[i], (const uint8_t *) payload,
		             sizeof(payload) - 1, sizeof(payload) - 1, 0,
		             UNZIP_METHOD_STORE, false, 0, 0, 0 };
		UnzipArchive archive;
		int r;

		m.crc = crc_of(m.data, m.data_len);
		write_archive("test_unzip_evil.zip", &m, 1);

		r = unzip_open(&archive, "test_unzip_evil.zip");
		CHECK(r == UNZIP_OK, "evil archive should still parse");
		if (r != UNZIP_OK) {
			continue;
		}

		r = unzip_extract_all(&archive, "test_unzip_out", NULL);
		CHECK(r == UNZIP_ERR_FORMAT, "'%s' was not refused (got %d)",
		      escapes[i], r);
		unzip_close(&archive);
	}

	CHECK(!file_exists("escaped"), "a member escaped the destination");
	CHECK(!file_exists("../escaped"), "a member escaped the destination");
}

/** Corrupt data must be caught by the checksum rather than written out. */
static void
test_detects_corruption(void)
{
	static const char payload[] = "content";
	Member m = { "file", (const uint8_t *) payload, sizeof(payload) - 1,
	             sizeof(payload) - 1, 0xdeadbeef, UNZIP_METHOD_STORE,
	             false, 0, 0, 0 };
	UnzipArchive archive;
	uint8_t buf[64];
	int r;

	write_archive("test_unzip_bad.zip", &m, 1);

	r = unzip_open(&archive, "test_unzip_bad.zip");
	CHECK(r == UNZIP_OK, "archive should parse");
	if (r != UNZIP_OK) {
		return;
	}

	r = unzip_extract_to_memory(&archive, unzip_entry(&archive, 0), buf,
	                            sizeof(buf));
	CHECK(r == UNZIP_ERR_CRC, "bad checksum not reported (got %d)", r);
	unzip_close(&archive);
}

/** Something that is not an archive at all should be turned away politely. */
static void
test_rejects_non_archive(void)
{
	UnzipArchive archive;
	FILE *fp = fopen("test_unzip_not.zip", "wb");
	int r;

	if (fp != NULL) {
		fwrite("not a zip file at all", 1, 21, fp);
		fclose(fp);
	}

	CHECK(!unzip_is_zip_file("test_unzip_not.zip"),
	      "plain file reported as an archive");
	CHECK(!unzip_is_zip_file("test_unzip_does_not_exist.zip"),
	      "missing file reported as an archive");

	r = unzip_open(&archive, "test_unzip_not.zip");
	CHECK(r == UNZIP_ERR_FORMAT, "non-archive not rejected (got %d)", r);

	r = unzip_open(&archive, "test_unzip_does_not_exist.zip");
	CHECK(r == UNZIP_ERR_OPEN, "missing file not reported (got %d)", r);
}

int
main(void)
{
	crc_init();

	test_extract_tree();
	test_rejects_escaping_members();
	test_detects_corruption();
	test_rejects_non_archive();

	if (failures != 0) {
		printf("test_unzip: %d failure(s)\n", failures);
		return 1;
	}

	printf("test_unzip: all checks passed\n");
	return 0;
}
