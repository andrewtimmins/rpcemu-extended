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

#ifndef KEYMAP_PLATFORM_H
#define KEYMAP_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical key position -> the X11 hardware keycode the rest of RPCEmu calls a
 * "native scancode" and feeds to keyboard_map_key().
 *
 * WHY THIS EXISTS. A native scancode identifies a key by WHERE IT IS, not by
 * what it types. keyboard_map_key() turns that position into PS/2 Set 2 codes
 * and RISC OS applies its own keyboard layout on top. Under wxGTK the position
 * arrives for free, because an X11 keycode is what X11 hands us. On Windows and
 * macOS it does not: wxKeyEvent::GetKeyCode() has already been through the
 * host's layout, so it describes a CHARACTER.
 *
 * Mapping that character back onto a position can only work if the host layout
 * matches whatever layout the table was written against, and ours was written
 * against UK. On any other layout it fails three ways at once, all of them
 * reported: keys whose character is not in the table go dead (issue #88,
 * umlauts), keys that moved get the position of whatever they now type so RISC
 * OS translates a second time (issue #88, z and y swapping "the wrong way
 * around"), and keys that differ only by position rather than by character
 * cannot be told apart at all (issue #91, Command against Control on macOS;
 * also left against right Shift and Ctrl everywhere).
 *
 * So both platforms are given their own position table instead. Neither is
 * conditionally compiled: they are plain data, which means tests/test_keymap.c
 * can check all three platforms' mappings from whichever one it happens to be
 * running on. See docs/keyboard.md.
 */

/**
 * Windows OEM (PS/2 Set 1) scancode -> X11 keycode.
 *
 * @param scancode Set 1 scancode, i.e. bits 16-23 of the WM_KEY* lParam that
 *                 wxWidgets exposes as wxKeyEvent::GetRawKeyFlags().
 * @param extended Non-zero when bit 24 of that lParam is set, marking the key
 *                 as one of the E0-prefixed duplicates (keypad Enter, right
 *                 Ctrl, the arrow cluster, AltGr and so on).
 * @return X11 keycode, or 0 when the key has no RISC OS equivalent.
 */
uint32_t keymap_x11_from_windows_scancode(uint32_t scancode, int extended);

/**
 * Windows WM_KEY* lParam -> X11 keycode.
 *
 * Unpacks the message parameter and looks the result up, so that the shifts and
 * masks live somewhere a test can reach rather than inline in the wxWidgets
 * event handler. Bits 16-23 hold the scancode and bit 24 the extended flag;
 * getting either wrong yields a plausible but wrong key, which is not the kind
 * of mistake that announces itself.
 *
 * @param lparam Value of wxKeyEvent::GetRawKeyFlags() under wxMSW.
 * @return X11 keycode, or 0 when the key has no RISC OS equivalent.
 */
uint32_t keymap_x11_from_windows_lparam(uint32_t lparam);

/**
 * macOS virtual keycode -> X11 keycode.
 *
 * @param keycode The value of [NSEvent keyCode], which wxWidgets exposes as
 *                wxKeyEvent::GetRawKeyCode(). These are the original ADB
 *                keycodes and describe a position, not a character.
 * @return X11 keycode, or 0 when the key has no RISC OS equivalent.
 */
uint32_t keymap_x11_from_macos_keycode(uint32_t keycode);

/** Highest Windows Set 1 scancode the non-extended arithmetic covers. */
#define KEYMAP_WINDOWS_SCANCODE_MAX 0x58

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_PLATFORM_H */
