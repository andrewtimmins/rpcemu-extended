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
 * md5.h - MD5, for checking downloaded packages against the checksum their
 * index gives.
 *
 * A straight implementation of RFC 1321 from the algorithm as published, in the
 * same spirit as the CRC-32 and DEFLATE in unzip.c: no dependency to add for one
 * checksum, and it builds unchanged on all three platforms.
 *
 * This is a checksum, not a security measure. MD5 has been broken for collision
 * resistance for years, and it is used here only because it is what the package
 * indexes publish. It catches a truncated or corrupted download, which is what
 * it is for.
 */

#ifndef MD5_H
#define MD5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t state[4];
	uint32_t count[2];	/* bit count, low then high */
	uint8_t buffer[64];
} MD5Context;

void md5_init(MD5Context *ctx);
void md5_update(MD5Context *ctx, const uint8_t *data, size_t len);
void md5_final(MD5Context *ctx, uint8_t digest[16]);

/**
 * MD5 of a whole file, as 32 lower-case hex digits.
 *
 * @param path    File to read.
 * @param hex_out Buffer of at least 33 bytes.
 * @return 0 on success, -1 if the file could not be read.
 */
int md5_file_hex(const char *path, char *hex_out);

#ifdef __cplusplus
}
#endif

#endif /* MD5_H */
