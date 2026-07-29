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
 * md5.c - MD5 (RFC 1321), for checking downloaded packages.
 *
 * See md5.h. Written from the algorithm as specified: the four rounds with
 * their per-round functions, the sine-derived constants, and the length
 * appended as a little-endian bit count.
 */

#include <stdio.h>
#include <string.h>

#include "md5.h"

/* Per-round mixing functions, as named in the specification. */
#define F(x, y, z)	(((x) & (y)) | (~(x) & (z)))
#define G(x, y, z)	(((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z)	((x) ^ (y) ^ (z))
#define I(x, y, z)	((y) ^ ((x) | ~(z)))

#define ROTL(x, n)	(((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, t, s) \
	do { \
		(a) += f((b), (c), (d)) + (x) + (t); \
		(a) = ROTL((a), (s)); \
		(a) += (b); \
	} while (0)

static uint32_t
read_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void
write_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v & 0xff);
	p[1] = (uint8_t) ((v >> 8) & 0xff);
	p[2] = (uint8_t) ((v >> 16) & 0xff);
	p[3] = (uint8_t) ((v >> 24) & 0xff);
}

static void
md5_transform(uint32_t state[4], const uint8_t block[64])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t x[16];
	int i;

	for (i = 0; i < 16; i++) {
		x[i] = read_le32(block + i * 4);
	}

	/* Round 1 */
	STEP(F, a, b, c, d, x[ 0], 0xd76aa478,  7);
	STEP(F, d, a, b, c, x[ 1], 0xe8c7b756, 12);
	STEP(F, c, d, a, b, x[ 2], 0x242070db, 17);
	STEP(F, b, c, d, a, x[ 3], 0xc1bdceee, 22);
	STEP(F, a, b, c, d, x[ 4], 0xf57c0faf,  7);
	STEP(F, d, a, b, c, x[ 5], 0x4787c62a, 12);
	STEP(F, c, d, a, b, x[ 6], 0xa8304613, 17);
	STEP(F, b, c, d, a, x[ 7], 0xfd469501, 22);
	STEP(F, a, b, c, d, x[ 8], 0x698098d8,  7);
	STEP(F, d, a, b, c, x[ 9], 0x8b44f7af, 12);
	STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17);
	STEP(F, b, c, d, a, x[11], 0x895cd7be, 22);
	STEP(F, a, b, c, d, x[12], 0x6b901122,  7);
	STEP(F, d, a, b, c, x[13], 0xfd987193, 12);
	STEP(F, c, d, a, b, x[14], 0xa679438e, 17);
	STEP(F, b, c, d, a, x[15], 0x49b40821, 22);

	/* Round 2 */
	STEP(G, a, b, c, d, x[ 1], 0xf61e2562,  5);
	STEP(G, d, a, b, c, x[ 6], 0xc040b340,  9);
	STEP(G, c, d, a, b, x[11], 0x265e5a51, 14);
	STEP(G, b, c, d, a, x[ 0], 0xe9b6c7aa, 20);
	STEP(G, a, b, c, d, x[ 5], 0xd62f105d,  5);
	STEP(G, d, a, b, c, x[10], 0x02441453,  9);
	STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14);
	STEP(G, b, c, d, a, x[ 4], 0xe7d3fbc8, 20);
	STEP(G, a, b, c, d, x[ 9], 0x21e1cde6,  5);
	STEP(G, d, a, b, c, x[14], 0xc33707d6,  9);
	STEP(G, c, d, a, b, x[ 3], 0xf4d50d87, 14);
	STEP(G, b, c, d, a, x[ 8], 0x455a14ed, 20);
	STEP(G, a, b, c, d, x[13], 0xa9e3e905,  5);
	STEP(G, d, a, b, c, x[ 2], 0xfcefa3f8,  9);
	STEP(G, c, d, a, b, x[ 7], 0x676f02d9, 14);
	STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

	/* Round 3 */
	STEP(H, a, b, c, d, x[ 5], 0xfffa3942,  4);
	STEP(H, d, a, b, c, x[ 8], 0x8771f681, 11);
	STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16);
	STEP(H, b, c, d, a, x[14], 0xfde5380c, 23);
	STEP(H, a, b, c, d, x[ 1], 0xa4beea44,  4);
	STEP(H, d, a, b, c, x[ 4], 0x4bdecfa9, 11);
	STEP(H, c, d, a, b, x[ 7], 0xf6bb4b60, 16);
	STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23);
	STEP(H, a, b, c, d, x[13], 0x289b7ec6,  4);
	STEP(H, d, a, b, c, x[ 0], 0xeaa127fa, 11);
	STEP(H, c, d, a, b, x[ 3], 0xd4ef3085, 16);
	STEP(H, b, c, d, a, x[ 6], 0x04881d05, 23);
	STEP(H, a, b, c, d, x[ 9], 0xd9d4d039,  4);
	STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11);
	STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16);
	STEP(H, b, c, d, a, x[ 2], 0xc4ac5665, 23);

	/* Round 4 */
	STEP(I, a, b, c, d, x[ 0], 0xf4292244,  6);
	STEP(I, d, a, b, c, x[ 7], 0x432aff97, 10);
	STEP(I, c, d, a, b, x[14], 0xab9423a7, 15);
	STEP(I, b, c, d, a, x[ 5], 0xfc93a039, 21);
	STEP(I, a, b, c, d, x[12], 0x655b59c3,  6);
	STEP(I, d, a, b, c, x[ 3], 0x8f0ccc92, 10);
	STEP(I, c, d, a, b, x[10], 0xffeff47d, 15);
	STEP(I, b, c, d, a, x[ 1], 0x85845dd1, 21);
	STEP(I, a, b, c, d, x[ 8], 0x6fa87e4f,  6);
	STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10);
	STEP(I, c, d, a, b, x[ 6], 0xa3014314, 15);
	STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21);
	STEP(I, a, b, c, d, x[ 4], 0xf7537e82,  6);
	STEP(I, d, a, b, c, x[11], 0xbd3af235, 10);
	STEP(I, c, d, a, b, x[ 2], 0x2ad7d2bb, 15);
	STEP(I, b, c, d, a, x[ 9], 0xeb86d391, 21);

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

void
md5_init(MD5Context *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
	ctx->count[0] = 0;
	ctx->count[1] = 0;
}

void
md5_update(MD5Context *ctx, const uint8_t *data, size_t len)
{
	size_t index = (ctx->count[0] >> 3) & 0x3f;
	size_t partial;
	size_t i;

	/* Bit count, 64-bit across two words. */
	ctx->count[0] += (uint32_t) (len << 3);
	if (ctx->count[0] < (uint32_t) (len << 3)) {
		ctx->count[1]++;
	}
	ctx->count[1] += (uint32_t) (len >> 29);

	partial = 64 - index;
	if (len >= partial) {
		memcpy(ctx->buffer + index, data, partial);
		md5_transform(ctx->state, ctx->buffer);

		for (i = partial; i + 63 < len; i += 64) {
			md5_transform(ctx->state, data + i);
		}
		index = 0;
	} else {
		i = 0;
	}

	memcpy(ctx->buffer + index, data + i, len - i);
}

void
md5_final(MD5Context *ctx, uint8_t digest[16])
{
	uint8_t bits[8];
	size_t index = (ctx->count[0] >> 3) & 0x3f;
	size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
	static const uint8_t padding[64] = { 0x80 };
	int i;

	write_le32(bits, ctx->count[0]);
	write_le32(bits + 4, ctx->count[1]);

	md5_update(ctx, padding, pad_len);
	md5_update(ctx, bits, 8);

	for (i = 0; i < 4; i++) {
		write_le32(digest + i * 4, ctx->state[i]);
	}
}

int
md5_file_hex(const char *path, char *hex_out)
{
	MD5Context ctx;
	uint8_t digest[16];
	uint8_t buffer[65536];
	FILE *f;
	size_t got;
	int i;

	f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}

	md5_init(&ctx);
	while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) {
		md5_update(&ctx, buffer, got);
	}
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	md5_final(&ctx, digest);
	for (i = 0; i < 16; i++) {
		sprintf(hex_out + i * 2, "%02x", digest[i]);
	}
	hex_out[32] = '\0';

	return 0;
}
