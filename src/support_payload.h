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
 */

#ifndef SUPPORT_PAYLOAD_H
#define SUPPORT_PAYLOAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The guest-side files this build carries, embedded by CMake at configure time
 * from the payload directories. See the note in src/CMakeLists.txt for why they
 * are in the binary rather than only beside it.
 */
typedef struct {
	const char *path;		/**< Relative, '/' separated */
	const char *hash;		/**< MD5 of the contents, lower-case hex */
	const unsigned char *data;
	size_t size;
} SupportPayloadFile;

extern const SupportPayloadFile support_payload[];
extern const int support_payload_count;

#ifdef __cplusplus
}
#endif

#endif /* SUPPORT_PAYLOAD_H */
