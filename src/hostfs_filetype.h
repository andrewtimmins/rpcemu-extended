/*
  RPCEmu - An Acorn system emulator

  Guessing a RISC OS filetype from a host filename extension.

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 */

#ifndef HOSTFS_FILETYPE_H
#define HOSTFS_FILETYPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Guess a RISC OS filetype from a host leafname's extension.
 *
 * Only for files that have not been given a type by any other means: a
 * ",xxx" suffix or a load-exec pair always wins, because those were written
 * by RISC OS and say what the file actually is.
 *
 * @param leafname Host leafname (no directory part needed, but harmless)
 * @param type     Filled in with the RISC OS filetype if one is recognised
 * @return         1 if the extension is recognised, 0 if not
 */
int hostfs_filetype_from_leafname(const char *leafname, uint32_t *type);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
