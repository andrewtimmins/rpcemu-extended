/*
  RPCEmu - An Acorn system emulator

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

/*
  Large-file (>2 GiB) stdio compatibility.

  The disc-image code (ide.c, cdrom-iso.c) uses the glibc large-file-support
  aliases fopen64()/fseeko64()/ftello64()/off64_t. glibc (Linux) provides them
  when _LARGEFILE64_SOURCE is defined, and MinGW-w64 (Windows) provides them
  too. macOS/BSD have no *64 aliases at all: there off_t is already 64-bit and
  the plain fopen()/fseeko()/ftello() are large-file-capable, so map the *64
  names onto them.

  Include this AFTER <stdio.h> (so fopen/fseeko/ftello are declared).
*/
#ifndef LFS_COMPAT_H
#define LFS_COMPAT_H

#if defined(__APPLE__) || defined(__OpenBSD__)
#include <sys/types.h>
#define off64_t   off_t
#define fopen64   fopen
#define fseeko64  fseeko
#define ftello64  ftello
#endif

#endif /* LFS_COMPAT_H */
