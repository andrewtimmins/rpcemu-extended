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
 * unzip.c - reading PKZIP archives that carry RISC OS file metadata.
 *
 * Stored and deflated members only, which is everything the archives we care
 * about use. The DEFLATE decoder is a straight implementation of RFC 1951
 * (fixed and dynamic Huffman, plus stored blocks) so that nothing here needs
 * zlib or a host archiving library on any of the three platforms we ship.
 *
 * The RISC OS part is the "Acorn" extra field, header ID 0x4341, whose body
 * is the tag "ARC0" followed by the load address, execution address and
 * access attributes. The filetype lives in the load address. Every member of
 * ROOL's HardDisc4 carries one, which is what makes it possible to unpack the
 * archive onto a machine's HostFS and have the result boot.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rpcemu.h"
#include "rpcemu-win.h"
#include "unzip.h"

#define ZIP_LOCAL_HEADER_SIG	0x04034b50u
#define ZIP_CENTRAL_HEADER_SIG	0x02014b50u
#define ZIP_END_CENTRAL_SIG	0x06054b50u

#define ZIP_CENTRAL_HEADER_LEN	46
#define ZIP_LOCAL_HEADER_LEN	30
#define ZIP_EOCD_LEN		22

/* The Acorn extra field: "AC" as a little-endian header ID, body tagged ARC0. */
#define ZIP_EXTRA_ACORN		0x4341u
#define ZIP_EXTRA_ACORN_MIN	16

/*
 * The filetype HostFS leaves implicit. path_construct() in hostfs.c omits the
 * ",xxx" suffix for this one type, so a tree we unpack matches a tree HostFS
 * would have written itself.
 */
#define HOSTFS_DEFAULT_FILE_TYPE	0xfffu

/* A central directory larger than this is not something we want to swallow. */
#define UNZIP_MAX_CENTRAL_DIR	(64u * 1024u * 1024u)

/* ------------------------------------------------------------------------ */
/* CRC-32                                                                    */
/* ------------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void
init_crc32_table(void)
{
	unsigned n, k;

	if (crc32_table_ready) {
		return;
	}

	for (n = 0; n < 256; n++) {
		uint32_t c = n;

		for (k = 0; k < 8; k++) {
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		}
		crc32_table[n] = c;
	}
	crc32_table_ready = true;
}

static uint32_t
update_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
	uint32_t c = crc ^ 0xffffffffu;

	while (len--) {
		c = crc32_table[(c ^ *buf++) & 0xff] ^ (c >> 8);
	}

	return c ^ 0xffffffffu;
}

/* ------------------------------------------------------------------------ */
/* Little-endian readers                                                     */
/* ------------------------------------------------------------------------ */

static uint16_t
read_le16(const uint8_t *p)
{
	return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t
read_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* ------------------------------------------------------------------------ */
/* DEFLATE (RFC 1951)                                                        */
/* ------------------------------------------------------------------------ */

#define DEFLATE_MAX_BITS	15
#define DEFLATE_MAX_LITLEN	288
#define DEFLATE_MAX_DIST	32
#define DEFLATE_MAX_CODELEN	19

static const uint8_t deflate_fixed_litlen[288] = {
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
	9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
	9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
	9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8
};

static const uint8_t deflate_fixed_dist[32] = {
	5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};

static const uint8_t deflate_codelen_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const unsigned deflate_len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const unsigned deflate_len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const unsigned deflate_dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577
};

static const unsigned deflate_dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

typedef struct {
	uint16_t symbol;
	uint8_t bits;
} HuffmanEntry;

typedef struct {
	HuffmanEntry *table;
	int max_bits;
	int size;
} HuffmanTable;

typedef struct {
	const uint8_t *data;
	size_t data_len;
	size_t pos;
	uint32_t bits;
	int nbits;
} Bitstream;

static void
bs_init(Bitstream *bs, const uint8_t *data, size_t len)
{
	bs->data = data;
	bs->data_len = len;
	bs->pos = 0;
	bs->bits = 0;
	bs->nbits = 0;
}

static int
bs_ensure_bits(Bitstream *bs, int n)
{
	while (bs->nbits < n) {
		if (bs->pos >= bs->data_len) {
			return -1;
		}
		bs->bits |= (uint32_t) bs->data[bs->pos++] << bs->nbits;
		bs->nbits += 8;
	}
	return 0;
}

static uint32_t
bs_peek_bits(const Bitstream *bs, int n)
{
	return bs->bits & ((1u << n) - 1);
}

static void
bs_drop_bits(Bitstream *bs, int n)
{
	bs->bits >>= n;
	bs->nbits -= n;
}

static int
bs_read_bits(Bitstream *bs, int n)
{
	uint32_t val;

	if (bs_ensure_bits(bs, n) < 0) {
		return -1;
	}

	val = bs_peek_bits(bs, n);
	bs_drop_bits(bs, n);
	return (int) val;
}

/**
 * Build a flat, bit-reversed lookup table from a set of canonical Huffman
 * code lengths, so decoding is one indexed read of max_bits.
 */
static int
huffman_build(HuffmanTable *ht, const uint8_t *lengths, int count)
{
	int bl_count[DEFLATE_MAX_BITS + 1];
	int next_code[DEFLATE_MAX_BITS + 1];
	int code, bits, i;
	int max_bits = 0;
	int table_size;

	memset(bl_count, 0, sizeof(bl_count));
	for (i = 0; i < count; i++) {
		if (lengths[i] > 0) {
			bl_count[lengths[i]]++;
			if (lengths[i] > max_bits) {
				max_bits = lengths[i];
			}
		}
	}

	if (max_bits == 0) {
		ht->table = NULL;
		ht->max_bits = 0;
		ht->size = 0;
		return 0;
	}

	code = 0;
	bl_count[0] = 0;
	for (bits = 1; bits <= max_bits; bits++) {
		code = (code + bl_count[bits - 1]) << 1;
		next_code[bits] = code;
	}

	table_size = 1 << max_bits;
	ht->table = calloc((size_t) table_size, sizeof(HuffmanEntry));
	if (ht->table == NULL) {
		return -1;
	}

	ht->max_bits = max_bits;
	ht->size = table_size;

	for (i = 0; i < count; i++) {
		int len = lengths[i];
		int fill_count, rev, j;

		if (len == 0) {
			continue;
		}

		code = next_code[len]++;

		rev = 0;
		for (bits = 0; bits < len; bits++) {
			rev = (rev << 1) | ((code >> bits) & 1);
		}

		/* Every longer index sharing this prefix decodes to it. */
		fill_count = 1 << (max_bits - len);
		for (j = 0; j < fill_count; j++) {
			int idx = rev | (j << len);

			ht->table[idx].symbol = (uint16_t) i;
			ht->table[idx].bits = (uint8_t) len;
		}
	}

	return 0;
}

static void
huffman_free(HuffmanTable *ht)
{
	free(ht->table);
	ht->table = NULL;
	ht->max_bits = 0;
	ht->size = 0;
}

static int
huffman_decode(const HuffmanTable *ht, Bitstream *bs)
{
	const HuffmanEntry *entry;
	uint32_t idx;

	if (ht->table == NULL || ht->max_bits == 0) {
		return -1;
	}

	/* Short of a full window at the end of the stream, decode on what is
	   left: the buffer pads with zeros and a valid stream has already
	   delivered its end-of-block code by then. */
	if (bs_ensure_bits(bs, ht->max_bits) < 0 && bs->nbits == 0) {
		return -1;
	}

	idx = bs_peek_bits(bs, ht->max_bits);
	entry = &ht->table[idx];

	if (entry->bits == 0) {
		return -1;
	}

	bs_drop_bits(bs, entry->bits);
	return entry->symbol;
}

static int
inflate_block(Bitstream *bs, uint8_t *out, size_t *out_pos, size_t out_size,
              const HuffmanTable *ht_litlen, const HuffmanTable *ht_dist)
{
	size_t pos = *out_pos;

	for (;;) {
		int sym = huffman_decode(ht_litlen, bs);

		if (sym < 0) {
			return UNZIP_ERR_INFLATE;
		}

		if (sym < 256) {
			if (pos >= out_size) {
				return UNZIP_ERR_INFLATE;
			}
			out[pos++] = (uint8_t) sym;
			continue;
		}

		if (sym == 256) {
			break;
		}

		{
			int length_code = sym - 257;
			int dist_code, extra, bits, i;
			unsigned length, distance;
			size_t src;

			if (length_code >= 29) {
				return UNZIP_ERR_INFLATE;
			}

			length = deflate_len_base[length_code];
			extra = (int) deflate_len_extra[length_code];
			if (extra > 0) {
				bits = bs_read_bits(bs, extra);
				if (bits < 0) {
					return UNZIP_ERR_INFLATE;
				}
				length += (unsigned) bits;
			}

			dist_code = huffman_decode(ht_dist, bs);
			if (dist_code < 0 || dist_code >= 30) {
				return UNZIP_ERR_INFLATE;
			}

			distance = deflate_dist_base[dist_code];
			extra = (int) deflate_dist_extra[dist_code];
			if (extra > 0) {
				bits = bs_read_bits(bs, extra);
				if (bits < 0) {
					return UNZIP_ERR_INFLATE;
				}
				distance += (unsigned) bits;
			}

			if ((size_t) distance > pos ||
			    pos + length > out_size) {
				return UNZIP_ERR_INFLATE;
			}

			/* Byte at a time: the ranges may overlap, and that
			   overlap is how DEFLATE encodes a run. */
			src = pos - distance;
			for (i = 0; i < (int) length; i++) {
				out[pos++] = out[src++];
			}
		}
	}

	*out_pos = pos;
	return UNZIP_OK;
}

static int
inflate_stream(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size,
               size_t *out_len)
{
	Bitstream bs;
	HuffmanTable ht_litlen;
	HuffmanTable ht_dist;
	size_t out_pos = 0;
	int bfinal;
	int result = UNZIP_OK;

	bs_init(&bs, in, in_len);
	memset(&ht_litlen, 0, sizeof(ht_litlen));
	memset(&ht_dist, 0, sizeof(ht_dist));

	do {
		int btype;

		bfinal = bs_read_bits(&bs, 1);
		btype = bs_read_bits(&bs, 2);
		if (bfinal < 0 || btype < 0) {
			result = UNZIP_ERR_INFLATE;
			break;
		}

		if (btype == 0) {
			/* Stored: byte-align, then a length and its complement. */
			unsigned len, nlen;

			bs.bits = 0;
			bs.nbits = 0;

			if (bs.pos + 4 > bs.data_len) {
				result = UNZIP_ERR_INFLATE;
				break;
			}

			len = read_le16(bs.data + bs.pos);
			nlen = read_le16(bs.data + bs.pos + 2);
			bs.pos += 4;

			if ((len ^ nlen) != 0xffff ||
			    bs.pos + len > bs.data_len ||
			    out_pos + len > out_size) {
				result = UNZIP_ERR_INFLATE;
				break;
			}

			memcpy(out + out_pos, bs.data + bs.pos, len);
			out_pos += len;
			bs.pos += len;
		} else if (btype == 1) {
			huffman_free(&ht_litlen);
			huffman_free(&ht_dist);

			if (huffman_build(&ht_litlen, deflate_fixed_litlen, 288) < 0 ||
			    huffman_build(&ht_dist, deflate_fixed_dist, 32) < 0) {
				result = UNZIP_ERR_MEMORY;
				break;
			}

			result = inflate_block(&bs, out, &out_pos, out_size,
			                       &ht_litlen, &ht_dist);
			if (result != UNZIP_OK) {
				break;
			}
		} else if (btype == 2) {
			uint8_t codelen_lengths[DEFLATE_MAX_CODELEN];
			uint8_t lengths[DEFLATE_MAX_LITLEN + DEFLATE_MAX_DIST];
			HuffmanTable ht_codelen;
			int hlit, hdist, hclen, i, n;

			hlit = bs_read_bits(&bs, 5);
			hdist = bs_read_bits(&bs, 5);
			hclen = bs_read_bits(&bs, 4);

			if (hlit < 0 || hdist < 0 || hclen < 0) {
				result = UNZIP_ERR_INFLATE;
				break;
			}

			hlit += 257;
			hdist += 1;
			hclen += 4;

			memset(codelen_lengths, 0, sizeof(codelen_lengths));
			for (i = 0; i < hclen; i++) {
				int cbits = bs_read_bits(&bs, 3);

				if (cbits < 0) {
					result = UNZIP_ERR_INFLATE;
					break;
				}
				codelen_lengths[deflate_codelen_order[i]] =
				    (uint8_t) cbits;
			}
			if (result != UNZIP_OK) {
				break;
			}

			memset(&ht_codelen, 0, sizeof(ht_codelen));
			if (huffman_build(&ht_codelen, codelen_lengths,
			                  DEFLATE_MAX_CODELEN) < 0) {
				result = UNZIP_ERR_MEMORY;
				break;
			}

			/* The code lengths are themselves Huffman coded, with
			   three run-length symbols above the literal 0-15. */
			n = hlit + hdist;
			i = 0;
			while (i < n) {
				int sym = huffman_decode(&ht_codelen, &bs);
				int repeat;

				if (sym < 0) {
					result = UNZIP_ERR_INFLATE;
					break;
				}

				if (sym < 16) {
					lengths[i++] = (uint8_t) sym;
				} else if (sym == 16) {
					uint8_t prev;

					repeat = bs_read_bits(&bs, 2);
					if (repeat < 0 || i == 0) {
						result = UNZIP_ERR_INFLATE;
						break;
					}
					prev = lengths[i - 1];
					for (repeat += 3; repeat > 0 && i < n; repeat--) {
						lengths[i++] = prev;
					}
				} else if (sym == 17) {
					repeat = bs_read_bits(&bs, 3);
					if (repeat < 0) {
						result = UNZIP_ERR_INFLATE;
						break;
					}
					for (repeat += 3; repeat > 0 && i < n; repeat--) {
						lengths[i++] = 0;
					}
				} else if (sym == 18) {
					repeat = bs_read_bits(&bs, 7);
					if (repeat < 0) {
						result = UNZIP_ERR_INFLATE;
						break;
					}
					for (repeat += 11; repeat > 0 && i < n; repeat--) {
						lengths[i++] = 0;
					}
				} else {
					result = UNZIP_ERR_INFLATE;
					break;
				}
			}

			huffman_free(&ht_codelen);
			if (result != UNZIP_OK) {
				break;
			}

			huffman_free(&ht_litlen);
			huffman_free(&ht_dist);

			if (huffman_build(&ht_litlen, lengths, hlit) < 0 ||
			    huffman_build(&ht_dist, lengths + hlit, hdist) < 0) {
				result = UNZIP_ERR_MEMORY;
				break;
			}

			result = inflate_block(&bs, out, &out_pos, out_size,
			                       &ht_litlen, &ht_dist);
			if (result != UNZIP_OK) {
				break;
			}
		} else {
			result = UNZIP_ERR_INFLATE;
			break;
		}
	} while (!bfinal);

	huffman_free(&ht_litlen);
	huffman_free(&ht_dist);

	*out_len = out_pos;
	return result;
}

/* ------------------------------------------------------------------------ */
/* Central directory                                                         */
/* ------------------------------------------------------------------------ */

/**
 * Pull the RISC OS load and execution addresses out of a member's extra
 * fields, if it has an Acorn one.
 */
static void
parse_acorn_extra(UnzipEntry *entry, const uint8_t *extra, size_t extra_len)
{
	size_t pos = 0;

	while (pos + 4 <= extra_len) {
		uint16_t header_id = read_le16(extra + pos);
		uint16_t data_size = read_le16(extra + pos + 2);
		const uint8_t *body = extra + pos + 4;

		if (pos + 4 + data_size > extra_len) {
			break;	/* Truncated; ignore the remainder */
		}

		if (header_id == ZIP_EXTRA_ACORN &&
		    data_size >= ZIP_EXTRA_ACORN_MIN &&
		    memcmp(body, "ARC0", 4) == 0) {
			entry->load_addr = read_le32(body + 4);
			entry->exec_addr = read_le32(body + 8);
			entry->attributes = read_le32(body + 12);
			entry->has_riscos_info = true;
			return;
		}

		pos += 4u + data_size;
	}
}

/**
 * Read the whole central directory in one pass, into an entry array and a
 * pooled block of names.
 *
 * Reading it once matters: the obvious alternative, seeking back and walking
 * from the start for each member, is quadratic, and HardDisc4 alone has 2289
 * of them.
 */
static int
read_central_directory(UnzipArchive *archive, uint32_t cd_offset,
                       uint32_t cd_size, int entry_count)
{
	uint8_t *cd;
	char *pool;
	size_t pos = 0;
	int i;

	if (cd_size == 0 || cd_size > UNZIP_MAX_CENTRAL_DIR || entry_count <= 0) {
		return UNZIP_ERR_FORMAT;
	}

	cd = malloc(cd_size);
	if (cd == NULL) {
		return UNZIP_ERR_MEMORY;
	}

	if (fseek(archive->fp, (long) cd_offset, SEEK_SET) != 0 ||
	    fread(cd, 1, cd_size, archive->fp) != cd_size) {
		free(cd);
		return UNZIP_ERR_FORMAT;
	}

	archive->entries = calloc((size_t) entry_count, sizeof(UnzipEntry));
	/* Names are a subset of the directory, so it bounds the pool. */
	archive->name_pool = malloc((size_t) cd_size + (size_t) entry_count + 1);
	if (archive->entries == NULL || archive->name_pool == NULL) {
		free(cd);
		return UNZIP_ERR_MEMORY;
	}
	pool = archive->name_pool;

	for (i = 0; i < entry_count; i++) {
		UnzipEntry *entry = &archive->entries[i];
		size_t name_len, extra_len, comment_len;
		const uint8_t *record = cd + pos;

		if (pos + ZIP_CENTRAL_HEADER_LEN > cd_size ||
		    read_le32(record) != ZIP_CENTRAL_HEADER_SIG) {
			free(cd);
			return UNZIP_ERR_FORMAT;
		}

		name_len = read_le16(record + 28);
		extra_len = read_le16(record + 30);
		comment_len = read_le16(record + 32);

		if (pos + ZIP_CENTRAL_HEADER_LEN + name_len + extra_len +
		    comment_len > cd_size) {
			free(cd);
			return UNZIP_ERR_FORMAT;
		}

		/* Refuse an over-long name rather than truncating it: a
		   truncated name is a different, possibly colliding, file. */
		if (name_len == 0 || name_len > UNZIP_MAX_NAME) {
			rpclog("unzip: rejecting member with %u byte name\n",
			       (unsigned) name_len);
			free(cd);
			return UNZIP_ERR_FORMAT;
		}

		entry->method = read_le16(record + 10);
		entry->crc32 = read_le32(record + 16);
		entry->compressed_size = read_le32(record + 20);
		entry->uncompressed_size = read_le32(record + 24);
		entry->local_header_offset = read_le32(record + 42);

		memcpy(pool, record + ZIP_CENTRAL_HEADER_LEN, name_len);
		pool[name_len] = '\0';
		entry->name = pool;
		pool += name_len + 1;

		/* A trailing separator is the format's way of saying this
		   member is a directory. Its size being zero is not: an empty
		   file has that too, and we want to create those. */
		entry->is_directory = (entry->name[name_len - 1] == '/');

		parse_acorn_extra(entry,
		                  record + ZIP_CENTRAL_HEADER_LEN + name_len,
		                  extra_len);

		pos += ZIP_CENTRAL_HEADER_LEN + name_len + extra_len + comment_len;
	}

	archive->entry_count = entry_count;
	free(cd);
	return UNZIP_OK;
}

int
unzip_open(UnzipArchive *archive, const char *path)
{
	uint8_t eocd[ZIP_EOCD_LEN];
	uint8_t sig[4];
	long file_size, search_pos, floor_pos;
	uint32_t cd_offset, cd_size;
	int entry_count;
	int result;
	bool found = false;

	if (archive == NULL || path == NULL) {
		return UNZIP_ERR_OPEN;
	}

	init_crc32_table();
	memset(archive, 0, sizeof(*archive));

	archive->fp = fopen(path, "rb");
	if (archive->fp == NULL) {
		rpclog("unzip: cannot open '%s'\n", path);
		return UNZIP_ERR_OPEN;
	}

	if (fread(sig, 1, 4, archive->fp) != 4 ||
	    read_le32(sig) != ZIP_LOCAL_HEADER_SIG) {
		rpclog("unzip: '%s' is not a zip archive\n", path);
		unzip_close(archive);
		return UNZIP_ERR_FORMAT;
	}

	if (fseek(archive->fp, 0, SEEK_END) != 0) {
		unzip_close(archive);
		return UNZIP_ERR_FORMAT;
	}
	file_size = ftell(archive->fp);
	if (file_size < ZIP_EOCD_LEN) {
		unzip_close(archive);
		return UNZIP_ERR_FORMAT;
	}

	/* The end-of-central-directory record sits last, but a trailing
	   comment of up to 64KB may follow it, so scan back for it. */
	floor_pos = file_size - 65536 - ZIP_EOCD_LEN;
	if (floor_pos < 0) {
		floor_pos = 0;
	}
	for (search_pos = file_size - ZIP_EOCD_LEN; search_pos >= floor_pos;
	     search_pos--) {
		if (fseek(archive->fp, search_pos, SEEK_SET) == 0 &&
		    fread(sig, 1, 4, archive->fp) == 4 &&
		    read_le32(sig) == ZIP_END_CENTRAL_SIG) {
			found = true;
			break;
		}
	}

	if (!found) {
		rpclog("unzip: no central directory in '%s'\n", path);
		unzip_close(archive);
		return UNZIP_ERR_FORMAT;
	}

	if (fseek(archive->fp, search_pos, SEEK_SET) != 0 ||
	    fread(eocd, 1, ZIP_EOCD_LEN, archive->fp) != ZIP_EOCD_LEN) {
		unzip_close(archive);
		return UNZIP_ERR_FORMAT;
	}

	entry_count = read_le16(eocd + 10);
	cd_size = read_le32(eocd + 12);
	cd_offset = read_le32(eocd + 16);

	/* 0xffff in any of these means the real values live in a Zip64
	   record. Nothing we fetch is anywhere near that large. */
	if (entry_count == 0xffff || cd_size == 0xffffffffu ||
	    cd_offset == 0xffffffffu) {
		rpclog("unzip: '%s' needs Zip64, which is not supported\n", path);
		unzip_close(archive);
		return UNZIP_ERR_UNSUPPORTED;
	}

	result = read_central_directory(archive, cd_offset, cd_size, entry_count);
	if (result != UNZIP_OK) {
		unzip_close(archive);
		return result;
	}

	rpclog("unzip: opened '%s', %d entries\n", path, archive->entry_count);
	return UNZIP_OK;
}

void
unzip_close(UnzipArchive *archive)
{
	if (archive == NULL) {
		return;
	}

	if (archive->fp != NULL) {
		fclose(archive->fp);
	}
	free(archive->entries);
	free(archive->name_pool);
	memset(archive, 0, sizeof(*archive));
}

int
unzip_entry_count(const UnzipArchive *archive)
{
	return archive != NULL ? archive->entry_count : 0;
}

const UnzipEntry *
unzip_entry(const UnzipArchive *archive, int index)
{
	if (archive == NULL || index < 0 || index >= archive->entry_count) {
		return NULL;
	}
	return &archive->entries[index];
}

static int
ascii_casecmp(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		char ca = *a++;
		char cb = *b++;

		if (ca >= 'A' && ca <= 'Z') {
			ca += 32;
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb += 32;
		}
		if (ca != cb) {
			return ca - cb;
		}
	}
	return (unsigned char) *a - (unsigned char) *b;
}

int
unzip_find(const UnzipArchive *archive, const char *name)
{
	int i;

	if (archive == NULL || name == NULL) {
		return -1;
	}

	for (i = 0; i < archive->entry_count; i++) {
		if (ascii_casecmp(archive->entries[i].name, name) == 0) {
			return i;
		}
	}

	return -1;
}

/* ------------------------------------------------------------------------ */
/* Extraction                                                                */
/* ------------------------------------------------------------------------ */

int
unzip_extract_to_memory(UnzipArchive *archive, const UnzipEntry *entry,
                        uint8_t *buffer, size_t buffer_size)
{
	uint8_t local_hdr[ZIP_LOCAL_HEADER_LEN];
	size_t name_len, extra_len;
	uint32_t crc;

	if (archive == NULL || entry == NULL || archive->fp == NULL ||
	    (buffer == NULL && entry->uncompressed_size != 0)) {
		return UNZIP_ERR_FORMAT;
	}

	if (buffer_size < entry->uncompressed_size) {
		return UNZIP_ERR_MEMORY;
	}

	/* The sizes come from the archive, so they are the attacker's to
	   choose. Check them before allocating anything from them. */
	if (entry->compressed_size > UNZIP_MAX_ENTRY_SIZE ||
	    entry->uncompressed_size > UNZIP_MAX_ENTRY_SIZE) {
		rpclog("unzip: '%s' too large (%u compressed, %u uncompressed)\n",
		       entry->name, (unsigned) entry->compressed_size,
		       (unsigned) entry->uncompressed_size);
		return UNZIP_ERR_TOOBIG;
	}

	if (entry->uncompressed_size == 0) {
		return UNZIP_OK;
	}

	if (fseek(archive->fp, (long) entry->local_header_offset, SEEK_SET) != 0 ||
	    fread(local_hdr, 1, ZIP_LOCAL_HEADER_LEN, archive->fp) !=
	    ZIP_LOCAL_HEADER_LEN ||
	    read_le32(local_hdr) != ZIP_LOCAL_HEADER_SIG) {
		return UNZIP_ERR_FORMAT;
	}

	/* The local header repeats the name and carries its own extra field,
	   which need not be the same length as the central one. */
	name_len = read_le16(local_hdr + 26);
	extra_len = read_le16(local_hdr + 28);
	if (fseek(archive->fp, (long) (name_len + extra_len), SEEK_CUR) != 0) {
		return UNZIP_ERR_FORMAT;
	}

	if (entry->method == UNZIP_METHOD_STORE) {
		if (fread(buffer, 1, entry->uncompressed_size, archive->fp) !=
		    entry->uncompressed_size) {
			return UNZIP_ERR_INFLATE;
		}
	} else if (entry->method == UNZIP_METHOD_DEFLATE) {
		uint8_t *compressed;
		size_t out_len = 0;
		int result;

		compressed = malloc(entry->compressed_size);
		if (compressed == NULL) {
			return UNZIP_ERR_MEMORY;
		}

		if (fread(compressed, 1, entry->compressed_size, archive->fp) !=
		    entry->compressed_size) {
			free(compressed);
			return UNZIP_ERR_INFLATE;
		}

		result = inflate_stream(compressed, entry->compressed_size,
		                        buffer, buffer_size, &out_len);
		free(compressed);

		if (result != UNZIP_OK) {
			return result;
		}

		if (out_len != entry->uncompressed_size) {
			rpclog("unzip: '%s' inflated to %u bytes, expected %u\n",
			       entry->name, (unsigned) out_len,
			       (unsigned) entry->uncompressed_size);
			return UNZIP_ERR_INFLATE;
		}
	} else {
		rpclog("unzip: '%s' uses unsupported method %u\n",
		       entry->name, (unsigned) entry->method);
		return UNZIP_ERR_UNSUPPORTED;
	}

	crc = update_crc32(0, buffer, entry->uncompressed_size);
	if (crc != entry->crc32) {
		rpclog("unzip: '%s' checksum %08x, expected %08x\n",
		       entry->name, (unsigned) crc, (unsigned) entry->crc32);
		return UNZIP_ERR_CRC;
	}

	return UNZIP_OK;
}

int
unzip_extract_to_file(UnzipArchive *archive, const UnzipEntry *entry,
                      const char *dest_path)
{
	uint8_t *buffer = NULL;
	FILE *out;
	int result;

	if (archive == NULL || entry == NULL || dest_path == NULL) {
		return UNZIP_ERR_FORMAT;
	}

	if (entry->uncompressed_size > 0) {
		buffer = malloc(entry->uncompressed_size);
		if (buffer == NULL) {
			return UNZIP_ERR_MEMORY;
		}

		result = unzip_extract_to_memory(archive, entry, buffer,
		                                 entry->uncompressed_size);
		if (result != UNZIP_OK) {
			free(buffer);
			return result;
		}
	}

	out = fopen(dest_path, "wb");
	if (out == NULL) {
		rpclog("unzip: cannot create '%s'\n", dest_path);
		free(buffer);
		return UNZIP_ERR_WRITE;
	}

	if (entry->uncompressed_size > 0 &&
	    fwrite(buffer, 1, entry->uncompressed_size, out) !=
	    entry->uncompressed_size) {
		fclose(out);
		free(buffer);
		return UNZIP_ERR_WRITE;
	}

	free(buffer);

	if (fclose(out) != 0) {
		return UNZIP_ERR_WRITE;
	}

	return UNZIP_OK;
}

/* ------------------------------------------------------------------------ */
/* Writing a tree out for HostFS                                             */
/* ------------------------------------------------------------------------ */

static bool
is_hex_digit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

/**
 * Would HostFS read a filetype off the end of this leafname by itself?
 *
 * path_construct() in hostfs.c omits the suffix for the default (text) type,
 * which is fine until the name genuinely ends in something like ",abc" and
 * HostFS reads that as a filetype. In that one case we write the suffix out
 * explicitly so the file keeps the type the archive gave it.
 */
static bool
leaf_has_suffix_lookalike(const char *leaf)
{
	const char *comma = strrchr(leaf, ',');

	if (comma == NULL) {
		return false;
	}

	if (strlen(comma + 1) == 3 && is_hex_digit(comma[1]) &&
	    is_hex_digit(comma[2]) && is_hex_digit(comma[3])) {
		return true;
	}

	/* The load-exec form, ",llllllll-eeeeeeee". */
	return strchr(comma + 1, '-') != NULL;
}

bool
unzip_hostfs_leafname(const UnzipEntry *entry, const char *leaf, char *out,
                      size_t out_size)
{
	int written;

	if (entry == NULL || leaf == NULL || out == NULL || out_size == 0) {
		return false;
	}

	if (!entry->has_riscos_info || entry->is_directory) {
		written = snprintf(out, out_size, "%s", leaf);
	} else if ((entry->load_addr & 0xfff00000u) == 0xfff00000u) {
		unsigned filetype = (entry->load_addr >> 8) & 0xfff;

		if (filetype == HOSTFS_DEFAULT_FILE_TYPE &&
		    !leaf_has_suffix_lookalike(leaf)) {
			written = snprintf(out, out_size, "%s", leaf);
		} else {
			written = snprintf(out, out_size, "%s,%03x", leaf,
			                   filetype);
		}
	} else {
		/* No filetype, so the addresses themselves are the metadata.
		   Same spelling hostfs.c uses, so it reads back unchanged. */
		written = snprintf(out, out_size, "%s,%x-%x", leaf,
		                   (unsigned) entry->load_addr,
		                   (unsigned) entry->exec_addr);
	}

	return written > 0 && (size_t) written < out_size;
}

static void
mkdir_recursive(const char *path)
{
	char tmp[UNZIP_MAX_NAME + 512];
	char *p;
	size_t len;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int) sizeof(tmp)) {
		return;
	}

	len = strlen(tmp);
	while (len > 0 && tmp[len - 1] == '/') {
		tmp[--len] = '\0';
	}

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

/**
 * Reject a member name that would write outside the destination.
 *
 * These archives are fetched over the network, so a member is free to call
 * itself "../../.." or start at the root. Neither is legitimate here.
 */
static bool
name_is_safe(const char *name)
{
	const char *p = name;

	if (name[0] == '/' || name[0] == '\\') {
		return false;
	}

	/* A drive or volume prefix, which Windows would honour. */
	if (name[0] != '\0' && name[1] == ':') {
		return false;
	}

	for (;;) {
		const char *slash = strchr(p, '/');
		size_t len = (slash != NULL) ? (size_t) (slash - p) : strlen(p);

		if (len == 2 && p[0] == '.' && p[1] == '.') {
			return false;
		}
		if (memchr(p, '\\', len) != NULL) {
			return false;
		}

		if (slash == NULL) {
			break;
		}
		p = slash + 1;
	}

	return true;
}

/**
 * Drop @strip leading components, returning what is left of @name, or NULL
 * if it does not have that many.
 */
static const char *
strip_leading_components(const char *name, int strip)
{
	const char *p = name;
	int i;

	for (i = 0; i < strip; i++) {
		const char *slash = strchr(p, '/');

		if (slash == NULL) {
			return NULL;
		}
		p = slash + 1;
	}

	return (*p != '\0') ? p : NULL;
}

int
unzip_extract_all(UnzipArchive *archive, const char *dest_dir,
                  const UnzipExtractOptions *options)
{
	UnzipExtractOptions defaults;
	char dest_path[UNZIP_MAX_NAME + 512];
	int count = 0;
	int i;

	if (archive == NULL || dest_dir == NULL) {
		return UNZIP_ERR_FORMAT;
	}

	if (options == NULL) {
		memset(&defaults, 0, sizeof(defaults));
		options = &defaults;
	}

	mkdir_recursive(dest_dir);

	for (i = 0; i < archive->entry_count; i++) {
		const UnzipEntry *entry = &archive->entries[i];
		const char *relative;
		const char *leaf;
		char typed_leaf[UNZIP_MAX_NAME + 32];
		int result;

		if (!name_is_safe(entry->name)) {
			rpclog("unzip: refusing unsafe member '%s'\n",
			       entry->name);
			return UNZIP_ERR_FORMAT;
		}

		relative = strip_leading_components(entry->name,
		                                    options->strip_components);
		if (relative == NULL) {
			continue;	/* Wholly consumed by the strip */
		}

		if (entry->is_directory) {
			if (snprintf(dest_path, sizeof(dest_path), "%s/%s",
			             dest_dir, relative) >= (int) sizeof(dest_path)) {
				return UNZIP_ERR_WRITE;
			}
			mkdir_recursive(dest_path);
			continue;
		}

		/* Create the parent before the file: an archive is not
		   obliged to list directories at all. */
		leaf = strrchr(relative, '/');
		if (leaf != NULL) {
			if (snprintf(dest_path, sizeof(dest_path), "%s/%.*s",
			             dest_dir, (int) (leaf - relative),
			             relative) >= (int) sizeof(dest_path)) {
				return UNZIP_ERR_WRITE;
			}
			mkdir_recursive(dest_path);
			leaf++;
		} else {
			leaf = relative;
		}

		if (options->apply_filetypes) {
			if (!unzip_hostfs_leafname(entry, leaf, typed_leaf,
			                           sizeof(typed_leaf))) {
				return UNZIP_ERR_WRITE;
			}
		} else if (snprintf(typed_leaf, sizeof(typed_leaf), "%s", leaf) >=
		           (int) sizeof(typed_leaf)) {
			return UNZIP_ERR_WRITE;
		}

		if (leaf == relative) {
			result = snprintf(dest_path, sizeof(dest_path), "%s/%s",
			                  dest_dir, typed_leaf);
		} else {
			result = snprintf(dest_path, sizeof(dest_path), "%s/%.*s/%s",
			                  dest_dir, (int) (leaf - relative - 1),
			                  relative, typed_leaf);
		}
		if (result >= (int) sizeof(dest_path)) {
			return UNZIP_ERR_WRITE;
		}

		result = unzip_extract_to_file(archive, entry, dest_path);
		if (result != UNZIP_OK) {
			rpclog("unzip: failed on '%s': %s\n", entry->name,
			       unzip_error_string(result));
			return result;
		}

		count++;

		if (options->progress != NULL &&
		    !options->progress(count, archive->entry_count, entry->name,
		                       options->progress_opaque)) {
			return UNZIP_ERR_CANCELLED;
		}
	}

	return count;
}

/* ------------------------------------------------------------------------ */
/* Odds and ends                                                             */
/* ------------------------------------------------------------------------ */

uint32_t
unzip_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
	init_crc32_table();
	return update_crc32(crc, data, len);
}

bool
unzip_is_zip_file(const char *path)
{
	uint8_t sig[4];
	FILE *fp;
	bool is_zip;

	if (path == NULL) {
		return false;
	}

	fp = fopen(path, "rb");
	if (fp == NULL) {
		return false;
	}

	is_zip = (fread(sig, 1, 4, fp) == 4) &&
	         (read_le32(sig) == ZIP_LOCAL_HEADER_SIG);

	fclose(fp);
	return is_zip;
}

const char *
unzip_error_string(int error)
{
	switch (error) {
	case UNZIP_OK:			return "no error";
	case UNZIP_ERR_OPEN:		return "cannot open archive";
	case UNZIP_ERR_FORMAT:		return "archive is not a valid zip";
	case UNZIP_ERR_CRC:		return "archive is corrupt (checksum failed)";
	case UNZIP_ERR_UNSUPPORTED:	return "archive uses an unsupported feature";
	case UNZIP_ERR_WRITE:		return "cannot write extracted file";
	case UNZIP_ERR_MEMORY:		return "out of memory";
	case UNZIP_ERR_INFLATE:		return "compressed data is corrupt";
	case UNZIP_ERR_TOOBIG:		return "archive member is too large";
	case UNZIP_ERR_CANCELLED:	return "cancelled";
	default:			return "unknown error";
	}
}
