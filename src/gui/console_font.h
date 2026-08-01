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

/* An 8x16 bitmap font for text the emulator draws itself. See console_font.c
   for where it came from and why it is compiled in. */

#ifndef CONSOLE_FONT_H
#define CONSOLE_FONT_H

#include <stdint.h>

#define CONSOLE_FONT_WIDTH   8
#define CONSOLE_FONT_HEIGHT  16
#define CONSOLE_FONT_FIRST   32   /**< first character in the table (space) */
#define CONSOLE_FONT_LAST    126  /**< last character in the table (tilde) */
#define CONSOLE_FONT_GLYPHS  (CONSOLE_FONT_LAST - CONSOLE_FONT_FIRST + 1)

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t console_font_8x16[CONSOLE_FONT_GLYPHS][CONSOLE_FONT_HEIGHT];

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_FONT_H */
