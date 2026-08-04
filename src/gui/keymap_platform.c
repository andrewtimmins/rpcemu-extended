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
 * keymap_platform.c - Windows and macOS physical keys as X11 keycodes.
 *
 * The rationale is in keymap_platform.h. The contract worth stating here is
 * that a NON-ZERO RETURN IS ALWAYS A KEYCODE keyboard_map_key() KNOWS. Callers
 * are entitled to treat zero as "this key has no RISC OS equivalent" and a
 * non-zero value as something that will reach the guest, and
 * tests/test_keymap.c enforces exactly that by resolving every entry here
 * through keyboard_map_key().
 */

#include "keymap_platform.h"

#include <stddef.h>

/*
 * Apple's own constants, used to check the numbers below rather than to build
 * them. The table has to hold literals so that it compiles and can be tested
 * everywhere, which leaves the risk that a literal is simply wrong; asserting
 * each one against <Carbon/HIToolbox/Events.h> turns that into a macOS build
 * failure instead of a key that does the wrong thing on somebody's Mac.
 *
 * __has_include keeps it optional: where the header is absent the table is
 * still built, merely unchecked, so this can never be the reason a port fails
 * to compile.
 */
#if defined(__APPLE__) && defined(__has_include)
#  if __has_include(<Carbon/HIToolbox/Events.h>)
#    include <Carbon/HIToolbox/Events.h>
#    define KEYMAP_CHECK_AGAINST_APPLE_HEADERS 1
#  endif
#endif

/* X11 keycodes, named so the tables below read as positions. These are the
   values keyboard_x.c is keyed by; the comments there name the same keys. */
enum {
	X11_ESCAPE = 0x09,
	X11_1 = 0x0a, X11_2 = 0x0b, X11_3 = 0x0c, X11_4 = 0x0d, X11_5 = 0x0e,
	X11_6 = 0x0f, X11_7 = 0x10, X11_8 = 0x11, X11_9 = 0x12, X11_0 = 0x13,
	X11_MINUS = 0x14, X11_EQUAL = 0x15, X11_BACKSPACE = 0x16,
	X11_TAB = 0x17,
	X11_Q = 0x18, X11_W = 0x19, X11_E = 0x1a, X11_R = 0x1b, X11_T = 0x1c,
	X11_Y = 0x1d, X11_U = 0x1e, X11_I = 0x1f, X11_O = 0x20, X11_P = 0x21,
	X11_LEFTBRACKET = 0x22, X11_RIGHTBRACKET = 0x23, X11_RETURN = 0x24,
	X11_LEFTCTRL = 0x25,
	X11_A = 0x26, X11_S = 0x27, X11_D = 0x28, X11_F = 0x29, X11_G = 0x2a,
	X11_H = 0x2b, X11_J = 0x2c, X11_K = 0x2d, X11_L = 0x2e,
	X11_SEMICOLON = 0x2f, X11_QUOTE = 0x30, X11_GRAVE = 0x31,
	X11_LEFTSHIFT = 0x32, X11_HASH = 0x33,
	X11_Z = 0x34, X11_X = 0x35, X11_C = 0x36, X11_V = 0x37, X11_B = 0x38,
	X11_N = 0x39, X11_M = 0x3a,
	X11_COMMA = 0x3b, X11_PERIOD = 0x3c, X11_SLASH = 0x3d,
	X11_RIGHTSHIFT = 0x3e, X11_KP_MULTIPLY = 0x3f,
	X11_LEFTALT = 0x40, X11_SPACE = 0x41, X11_CAPSLOCK = 0x42,
	X11_F1 = 0x43, X11_F2 = 0x44, X11_F3 = 0x45, X11_F4 = 0x46,
	X11_F5 = 0x47, X11_F6 = 0x48, X11_F7 = 0x49, X11_F8 = 0x4a,
	X11_F9 = 0x4b, X11_F10 = 0x4c,
	X11_NUMLOCK = 0x4d, X11_SCROLLLOCK = 0x4e,
	X11_KP_7 = 0x4f, X11_KP_8 = 0x50, X11_KP_9 = 0x51, X11_KP_MINUS = 0x52,
	X11_KP_4 = 0x53, X11_KP_5 = 0x54, X11_KP_6 = 0x55, X11_KP_PLUS = 0x56,
	X11_KP_1 = 0x57, X11_KP_2 = 0x58, X11_KP_3 = 0x59, X11_KP_0 = 0x5a,
	X11_KP_DECIMAL = 0x5b,
	X11_BACKSLASH = 0x5e, X11_F11 = 0x5f, X11_F12 = 0x60,
	X11_KP_ENTER = 0x68, X11_RIGHTCTRL = 0x69, X11_KP_DIVIDE = 0x6a,
	X11_RIGHTALT = 0x6c,
	X11_HOME = 0x6e, X11_UP = 0x6f, X11_PAGEUP = 0x70, X11_LEFT = 0x71,
	X11_RIGHT = 0x72, X11_END = 0x73, X11_DOWN = 0x74, X11_PAGEDOWN = 0x75,
	X11_INSERT = 0x76, X11_DELETE = 0x77,
	X11_BREAK = 0x7f,
	X11_LEFTWIN = 0x85, X11_RIGHTWIN = 0x86, X11_MENU = 0x87
};

/*
 * Windows, the non-extended block: X11 keycode = Set 1 scancode + 8.
 *
 * That is not a coincidence worth hiding behind a hand-typed table. Linux
 * builds its evdev keycodes from the same AT scancode numbering with the same
 * offset of 8, so every key from Escape (0x01 -> 0x09) through F12
 * (0x58 -> 0x60) lines up, the two international keys included: 0x2b is the
 * `#`/`\` key next to Return and 0x56 is the `\`/`<>|` key beside left Shift,
 * which are exactly the two keyboard_x.c marks "International only". A table
 * would be ninety chances to mistype a number that arithmetic gets right.
 *
 * Two scancodes inside the range have no X11 keycode: 0x54 is SysReq
 * (Alt+PrintScreen) and 0x55 is unassigned, which is the gap at 0x5c/0x5d in
 * keyboard_x.c. They are refused so that the "non-zero means mappable"
 * contract holds.
 */
#define WINDOWS_SCANCODE_TO_X11_OFFSET 8
#define WINDOWS_SCANCODE_SYSREQ 0x54
#define WINDOWS_SCANCODE_UNASSIGNED 0x55

/* Windows, the E0-prefixed block. Positions that duplicate a key in the block
   above and so cannot be told apart by scancode alone. */
static const struct {
	uint32_t scancode;
	uint32_t x11;
} windows_extended[] = {
	{ 0x1c, X11_KP_ENTER },
	{ 0x1d, X11_RIGHTCTRL },
	{ 0x35, X11_KP_DIVIDE },
	/* AltGr. A German keyboard needs it for @ \ | ~ and the like, and until
	   now it reached nothing: right Alt was being reported to the guest as
	   left Alt on Windows and macOS, and on X11 it was dropped outright
	   because keyboard_x.c had no 0x6c. See issue #88. */
	{ 0x38, X11_RIGHTALT },
	/* Ctrl+Pause arrives as E0 46 rather than as the E1 sequence. */
	{ 0x46, X11_BREAK },
	{ 0x47, X11_HOME },
	{ 0x48, X11_UP },
	{ 0x49, X11_PAGEUP },
	{ 0x4b, X11_LEFT },
	{ 0x4d, X11_RIGHT },
	{ 0x4f, X11_END },
	{ 0x50, X11_DOWN },
	{ 0x51, X11_PAGEDOWN },
	{ 0x52, X11_INSERT },
	{ 0x53, X11_DELETE },
	{ 0x5b, X11_LEFTWIN },
	{ 0x5c, X11_RIGHTWIN },
	{ 0x5d, X11_MENU },
};

/*
 * macOS virtual keycodes. Sparse and in no useful order, because ADB numbered
 * the keys by where they sat on the 1984 keyboard's matrix rather than by
 * anything a layout cares about - which is precisely why they are the right
 * thing to read. Every entry is checked against Apple's own kVK_ constant
 * below when building on macOS.
 *
 * Deliberately absent: kVK_Function (0x3f) and kVK_ANSI_KeypadEquals (0x51),
 * which no PC keyboard and therefore no RISC OS has, and kVK_F13 (0x69), whose
 * PC equivalent is Print Screen and which keyboard_x.c does not map either.
 */
static const struct {
	uint32_t keycode;
	uint32_t x11;
} macos_keys[] = {
	{ 0x00, X11_A },
	{ 0x01, X11_S },
	{ 0x02, X11_D },
	{ 0x03, X11_F },
	{ 0x04, X11_H },
	{ 0x05, X11_G },
	{ 0x06, X11_Z },
	{ 0x07, X11_X },
	{ 0x08, X11_C },
	{ 0x09, X11_V },
	/* The extra key an ISO keyboard has and an ANSI one does not. On a UK Mac
	   it is the section key; on a PC it is the one beside left Shift. */
	{ 0x0a, X11_BACKSLASH },
	{ 0x0b, X11_B },
	{ 0x0c, X11_Q },
	{ 0x0d, X11_W },
	{ 0x0e, X11_E },
	{ 0x0f, X11_R },
	{ 0x10, X11_Y },
	{ 0x11, X11_T },
	{ 0x12, X11_1 },
	{ 0x13, X11_2 },
	{ 0x14, X11_3 },
	{ 0x15, X11_4 },
	{ 0x16, X11_6 },	/* 6 and 5 really are the other way round */
	{ 0x17, X11_5 },
	{ 0x18, X11_EQUAL },
	{ 0x19, X11_9 },
	{ 0x1a, X11_7 },
	{ 0x1b, X11_MINUS },
	{ 0x1c, X11_8 },
	{ 0x1d, X11_0 },
	{ 0x1e, X11_RIGHTBRACKET },
	{ 0x1f, X11_O },
	{ 0x20, X11_U },
	{ 0x21, X11_LEFTBRACKET },
	{ 0x22, X11_I },
	{ 0x23, X11_P },
	{ 0x24, X11_RETURN },
	{ 0x25, X11_L },
	{ 0x26, X11_J },
	{ 0x27, X11_QUOTE },
	{ 0x28, X11_K },
	{ 0x29, X11_SEMICOLON },
	{ 0x2a, X11_HASH },
	{ 0x2b, X11_COMMA },
	{ 0x2c, X11_SLASH },
	{ 0x2d, X11_N },
	{ 0x2e, X11_M },
	{ 0x2f, X11_PERIOD },
	{ 0x30, X11_TAB },
	{ 0x31, X11_SPACE },
	{ 0x32, X11_GRAVE },
	{ 0x33, X11_BACKSPACE },	/* kVK_Delete is the one above Return */
	{ 0x35, X11_ESCAPE },
	/* Both Command keys map to their honest PC equivalent, the Windows keys.
	   What RPCEmu then DOES with them is a policy decision and lives in
	   input_helpers.cpp: left Command becomes Ctrl so that Cmd+C still copies
	   in RISC OS, and right Command becomes the third mouse button because a
	   Mac has no menu key (issue #91). Keeping that out of this table is what
	   lets the table stay one-to-one and be checked for collisions. */
	{ 0x36, X11_RIGHTWIN },
	{ 0x37, X11_LEFTWIN },
	{ 0x38, X11_LEFTSHIFT },
	{ 0x39, X11_CAPSLOCK },
	{ 0x3a, X11_LEFTALT },
	{ 0x3b, X11_LEFTCTRL },
	{ 0x3c, X11_RIGHTSHIFT },
	{ 0x3d, X11_RIGHTALT },
	{ 0x3e, X11_RIGHTCTRL },
	{ 0x41, X11_KP_DECIMAL },
	{ 0x43, X11_KP_MULTIPLY },
	{ 0x45, X11_KP_PLUS },
	{ 0x47, X11_NUMLOCK },		/* kVK_ANSI_KeypadClear */
	{ 0x4b, X11_KP_DIVIDE },
	{ 0x4c, X11_KP_ENTER },
	{ 0x4e, X11_KP_MINUS },
	{ 0x52, X11_KP_0 },
	{ 0x53, X11_KP_1 },
	{ 0x54, X11_KP_2 },
	{ 0x55, X11_KP_3 },
	{ 0x56, X11_KP_4 },
	{ 0x57, X11_KP_5 },
	{ 0x58, X11_KP_6 },
	{ 0x59, X11_KP_7 },
	{ 0x5b, X11_KP_8 },
	{ 0x5c, X11_KP_9 },
	{ 0x60, X11_F5 },
	{ 0x61, X11_F6 },
	{ 0x62, X11_F7 },
	{ 0x63, X11_F3 },
	{ 0x64, X11_F8 },
	{ 0x65, X11_F9 },
	{ 0x67, X11_F11 },
	{ 0x6b, X11_SCROLLLOCK },	/* kVK_F14 */
	{ 0x6d, X11_F10 },
	{ 0x6f, X11_F12 },
	{ 0x71, X11_BREAK },		/* kVK_F15 */
	{ 0x72, X11_INSERT },		/* kVK_Help */
	{ 0x73, X11_HOME },
	{ 0x74, X11_PAGEUP },
	{ 0x75, X11_DELETE },		/* kVK_ForwardDelete */
	{ 0x76, X11_F4 },
	{ 0x77, X11_END },
	{ 0x78, X11_F2 },
	{ 0x79, X11_PAGEDOWN },
	{ 0x7a, X11_F1 },
	{ 0x7b, X11_LEFT },
	{ 0x7c, X11_RIGHT },
	{ 0x7d, X11_DOWN },
	{ 0x7e, X11_UP },
};

#ifdef KEYMAP_CHECK_AGAINST_APPLE_HEADERS
/* Every literal in macos_keys[] against the constant it is meant to be. A
   mistyped number becomes a compile error on macOS rather than a wrong key.
   kVK_RightCommand is left out on purpose: unlike the rest it was only added to
   the SDK in 10.12, so asserting it would make the check itself a portability
   problem. */
#define KEYMAP_ASSERT_KVK(literal, apple) \
	_Static_assert((literal) == (apple), "macOS keycode " #apple " is not " #literal)

KEYMAP_ASSERT_KVK(0x00, kVK_ANSI_A);
KEYMAP_ASSERT_KVK(0x01, kVK_ANSI_S);
KEYMAP_ASSERT_KVK(0x02, kVK_ANSI_D);
KEYMAP_ASSERT_KVK(0x03, kVK_ANSI_F);
KEYMAP_ASSERT_KVK(0x04, kVK_ANSI_H);
KEYMAP_ASSERT_KVK(0x05, kVK_ANSI_G);
KEYMAP_ASSERT_KVK(0x06, kVK_ANSI_Z);
KEYMAP_ASSERT_KVK(0x07, kVK_ANSI_X);
KEYMAP_ASSERT_KVK(0x08, kVK_ANSI_C);
KEYMAP_ASSERT_KVK(0x09, kVK_ANSI_V);
KEYMAP_ASSERT_KVK(0x0a, kVK_ISO_Section);
KEYMAP_ASSERT_KVK(0x0b, kVK_ANSI_B);
KEYMAP_ASSERT_KVK(0x0c, kVK_ANSI_Q);
KEYMAP_ASSERT_KVK(0x0d, kVK_ANSI_W);
KEYMAP_ASSERT_KVK(0x0e, kVK_ANSI_E);
KEYMAP_ASSERT_KVK(0x0f, kVK_ANSI_R);
KEYMAP_ASSERT_KVK(0x10, kVK_ANSI_Y);
KEYMAP_ASSERT_KVK(0x11, kVK_ANSI_T);
KEYMAP_ASSERT_KVK(0x12, kVK_ANSI_1);
KEYMAP_ASSERT_KVK(0x13, kVK_ANSI_2);
KEYMAP_ASSERT_KVK(0x14, kVK_ANSI_3);
KEYMAP_ASSERT_KVK(0x15, kVK_ANSI_4);
KEYMAP_ASSERT_KVK(0x16, kVK_ANSI_6);
KEYMAP_ASSERT_KVK(0x17, kVK_ANSI_5);
KEYMAP_ASSERT_KVK(0x18, kVK_ANSI_Equal);
KEYMAP_ASSERT_KVK(0x19, kVK_ANSI_9);
KEYMAP_ASSERT_KVK(0x1a, kVK_ANSI_7);
KEYMAP_ASSERT_KVK(0x1b, kVK_ANSI_Minus);
KEYMAP_ASSERT_KVK(0x1c, kVK_ANSI_8);
KEYMAP_ASSERT_KVK(0x1d, kVK_ANSI_0);
KEYMAP_ASSERT_KVK(0x1e, kVK_ANSI_RightBracket);
KEYMAP_ASSERT_KVK(0x1f, kVK_ANSI_O);
KEYMAP_ASSERT_KVK(0x20, kVK_ANSI_U);
KEYMAP_ASSERT_KVK(0x21, kVK_ANSI_LeftBracket);
KEYMAP_ASSERT_KVK(0x22, kVK_ANSI_I);
KEYMAP_ASSERT_KVK(0x23, kVK_ANSI_P);
KEYMAP_ASSERT_KVK(0x24, kVK_Return);
KEYMAP_ASSERT_KVK(0x25, kVK_ANSI_L);
KEYMAP_ASSERT_KVK(0x26, kVK_ANSI_J);
KEYMAP_ASSERT_KVK(0x27, kVK_ANSI_Quote);
KEYMAP_ASSERT_KVK(0x28, kVK_ANSI_K);
KEYMAP_ASSERT_KVK(0x29, kVK_ANSI_Semicolon);
KEYMAP_ASSERT_KVK(0x2a, kVK_ANSI_Backslash);
KEYMAP_ASSERT_KVK(0x2b, kVK_ANSI_Comma);
KEYMAP_ASSERT_KVK(0x2c, kVK_ANSI_Slash);
KEYMAP_ASSERT_KVK(0x2d, kVK_ANSI_N);
KEYMAP_ASSERT_KVK(0x2e, kVK_ANSI_M);
KEYMAP_ASSERT_KVK(0x2f, kVK_ANSI_Period);
KEYMAP_ASSERT_KVK(0x30, kVK_Tab);
KEYMAP_ASSERT_KVK(0x31, kVK_Space);
KEYMAP_ASSERT_KVK(0x32, kVK_ANSI_Grave);
KEYMAP_ASSERT_KVK(0x33, kVK_Delete);
KEYMAP_ASSERT_KVK(0x35, kVK_Escape);
KEYMAP_ASSERT_KVK(0x37, kVK_Command);
KEYMAP_ASSERT_KVK(0x38, kVK_Shift);
KEYMAP_ASSERT_KVK(0x39, kVK_CapsLock);
KEYMAP_ASSERT_KVK(0x3a, kVK_Option);
KEYMAP_ASSERT_KVK(0x3b, kVK_Control);
KEYMAP_ASSERT_KVK(0x3c, kVK_RightShift);
KEYMAP_ASSERT_KVK(0x3d, kVK_RightOption);
KEYMAP_ASSERT_KVK(0x3e, kVK_RightControl);
KEYMAP_ASSERT_KVK(0x41, kVK_ANSI_KeypadDecimal);
KEYMAP_ASSERT_KVK(0x43, kVK_ANSI_KeypadMultiply);
KEYMAP_ASSERT_KVK(0x45, kVK_ANSI_KeypadPlus);
KEYMAP_ASSERT_KVK(0x47, kVK_ANSI_KeypadClear);
KEYMAP_ASSERT_KVK(0x4b, kVK_ANSI_KeypadDivide);
KEYMAP_ASSERT_KVK(0x4c, kVK_ANSI_KeypadEnter);
KEYMAP_ASSERT_KVK(0x4e, kVK_ANSI_KeypadMinus);
KEYMAP_ASSERT_KVK(0x52, kVK_ANSI_Keypad0);
KEYMAP_ASSERT_KVK(0x53, kVK_ANSI_Keypad1);
KEYMAP_ASSERT_KVK(0x54, kVK_ANSI_Keypad2);
KEYMAP_ASSERT_KVK(0x55, kVK_ANSI_Keypad3);
KEYMAP_ASSERT_KVK(0x56, kVK_ANSI_Keypad4);
KEYMAP_ASSERT_KVK(0x57, kVK_ANSI_Keypad5);
KEYMAP_ASSERT_KVK(0x58, kVK_ANSI_Keypad6);
KEYMAP_ASSERT_KVK(0x59, kVK_ANSI_Keypad7);
KEYMAP_ASSERT_KVK(0x5b, kVK_ANSI_Keypad8);
KEYMAP_ASSERT_KVK(0x5c, kVK_ANSI_Keypad9);
KEYMAP_ASSERT_KVK(0x60, kVK_F5);
KEYMAP_ASSERT_KVK(0x61, kVK_F6);
KEYMAP_ASSERT_KVK(0x62, kVK_F7);
KEYMAP_ASSERT_KVK(0x63, kVK_F3);
KEYMAP_ASSERT_KVK(0x64, kVK_F8);
KEYMAP_ASSERT_KVK(0x65, kVK_F9);
KEYMAP_ASSERT_KVK(0x67, kVK_F11);
KEYMAP_ASSERT_KVK(0x6b, kVK_F14);
KEYMAP_ASSERT_KVK(0x6d, kVK_F10);
KEYMAP_ASSERT_KVK(0x6f, kVK_F12);
KEYMAP_ASSERT_KVK(0x71, kVK_F15);
KEYMAP_ASSERT_KVK(0x72, kVK_Help);
KEYMAP_ASSERT_KVK(0x73, kVK_Home);
KEYMAP_ASSERT_KVK(0x74, kVK_PageUp);
KEYMAP_ASSERT_KVK(0x75, kVK_ForwardDelete);
KEYMAP_ASSERT_KVK(0x76, kVK_F4);
KEYMAP_ASSERT_KVK(0x77, kVK_End);
KEYMAP_ASSERT_KVK(0x78, kVK_F2);
KEYMAP_ASSERT_KVK(0x79, kVK_PageDown);
KEYMAP_ASSERT_KVK(0x7a, kVK_F1);
KEYMAP_ASSERT_KVK(0x7b, kVK_LeftArrow);
KEYMAP_ASSERT_KVK(0x7c, kVK_RightArrow);
KEYMAP_ASSERT_KVK(0x7d, kVK_DownArrow);
KEYMAP_ASSERT_KVK(0x7e, kVK_UpArrow);
#endif /* KEYMAP_CHECK_AGAINST_APPLE_HEADERS */

uint32_t
keymap_x11_from_windows_scancode(uint32_t scancode, int extended)
{
	size_t i;

	if (extended) {
		for (i = 0; i < sizeof(windows_extended) / sizeof(windows_extended[0]); i++) {
			if (windows_extended[i].scancode == scancode) {
				return windows_extended[i].x11;
			}
		}
		return 0;
	}

	if (scancode == 0 || scancode > KEYMAP_WINDOWS_SCANCODE_MAX) {
		return 0;
	}
	if (scancode == WINDOWS_SCANCODE_SYSREQ ||
	    scancode == WINDOWS_SCANCODE_UNASSIGNED) {
		return 0;
	}
	return scancode + WINDOWS_SCANCODE_TO_X11_OFFSET;
}

/* Layout of the WM_KEY* lParam, as documented for WM_KEYDOWN. */
#define WINDOWS_LPARAM_SCANCODE_SHIFT 16
#define WINDOWS_LPARAM_SCANCODE_MASK 0xff
#define WINDOWS_LPARAM_EXTENDED_BIT (1u << 24)

uint32_t
keymap_x11_from_windows_lparam(uint32_t lparam)
{
	const uint32_t scancode =
	    (lparam >> WINDOWS_LPARAM_SCANCODE_SHIFT) & WINDOWS_LPARAM_SCANCODE_MASK;
	const int extended = (lparam & WINDOWS_LPARAM_EXTENDED_BIT) != 0;

	return keymap_x11_from_windows_scancode(scancode, extended);
}

uint32_t
keymap_x11_from_macos_keycode(uint32_t keycode)
{
	size_t i;

	for (i = 0; i < sizeof(macos_keys) / sizeof(macos_keys[0]); i++) {
		if (macos_keys[i].keycode == keycode) {
			return macos_keys[i].x11;
		}
	}
	return 0;
}
