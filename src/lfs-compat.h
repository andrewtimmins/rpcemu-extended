/*
  RPCEmu - An Acorn system emulator

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

#ifdef __APPLE__
#include <sys/types.h>
#define off64_t   off_t
#define fopen64   fopen
#define fseeko64  fseeko
#define ftello64  ftello
#endif

#endif /* LFS_COMPAT_H */
