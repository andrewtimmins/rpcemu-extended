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

#include "input_helpers.h"

extern "C" {
#include "keyboard.h"
}

static unsigned scancode_from_wx_key_code(int key_code)
{
	switch (key_code) {
	case WXK_ESCAPE:
		return 0x09;
	case WXK_BACK:
		return 0x16;
	case WXK_TAB:
		return 0x17;
	case WXK_RETURN:
	case WXK_NUMPAD_ENTER:
		return 0x24;
	case WXK_SPACE:
		return 0x41;
	case WXK_CAPITAL:
		return 0x42;
	case WXK_SHIFT:
		return 0x32;
	case WXK_CONTROL:
		return 0x25;
#ifdef __WXOSX__
	case WXK_RAW_CONTROL:
		/* On macOS wxWidgets maps the Command key to WXK_CONTROL and the
		   physical Control key to WXK_RAW_CONTROL. RISC OS uses Control (e.g.
		   Ctrl-C to copy), so send both to the guest's Control key. */
		return 0x25;
#endif
	case WXK_ALT:
		return 0x40;
	case WXK_DELETE:
		return 0x77;
	case WXK_INSERT:
		return 0x76;
	case WXK_HOME:
		return 0x6e;
	case WXK_END:
		return 0x73;
	case WXK_PAGEUP:
		return 0x70;
	case WXK_PAGEDOWN:
		return 0x75;
	case WXK_LEFT:
		return 0x71;
	case WXK_UP:
		return 0x6f;
	case WXK_RIGHT:
		return 0x72;
	case WXK_DOWN:
		return 0x74;
	case WXK_F1:
		return 0x43;
	case WXK_F2:
		return 0x44;
	case WXK_F3:
		return 0x45;
	case WXK_F4:
		return 0x46;
	case WXK_F5:
		return 0x47;
	case WXK_F6:
		return 0x48;
	case WXK_F7:
		return 0x49;
	case WXK_F8:
		return 0x4a;
	case WXK_F9:
		return 0x4b;
	case WXK_F10:
		return 0x4c;
	case WXK_F11:
		return 0x5f;
	case WXK_F12:
		return 0x60;
	case WXK_NUMPAD0:
		return 0x5a;
	case WXK_NUMPAD1:
		return 0x57;
	case WXK_NUMPAD2:
		return 0x58;
	case WXK_NUMPAD3:
		return 0x59;
	case WXK_NUMPAD4:
		return 0x53;
	case WXK_NUMPAD5:
		return 0x54;
	case WXK_NUMPAD6:
		return 0x55;
	case WXK_NUMPAD7:
		return 0x4f;
	case WXK_NUMPAD8:
		return 0x50;
	case WXK_NUMPAD9:
		return 0x51;
	case WXK_NUMPAD_DIVIDE:
		return 0x6a;
	case WXK_NUMPAD_MULTIPLY:
		return 0x3f;
	case WXK_NUMPAD_SUBTRACT:
		return 0x52;
	case WXK_NUMPAD_ADD:
		return 0x56;
	case WXK_NUMPAD_DECIMAL:
		return 0x5b;
	case '-':
		return 0x14;
	case '=':
		return 0x15;
	case '[':
		return 0x22;
	case ']':
		return 0x23;
	case ';':
		return 0x2f;
	case '\'':
		return 0x30;
	case '`':
		return 0x31;
	case '\\':
		return 0x5e;
	case '#':
		return 0x33;
	case ',':
		return 0x3b;
	case '.':
		return 0x3c;
	case '/':
		return 0x3d;
	default:
		break;
	}

	if (key_code >= 'A' && key_code <= 'Z') {
		// X11 hardware keycodes for A-Z (keyboard_map_key() expects these and
		// maps them to PS/2). K and L are 0x2d/0x2e; earlier these were wrongly
		// set to the PS/2 output values 0x42/0x4b (= Caps Lock / F9), so typing
		// 'k' toggled Caps Lock. Only exercised on non-X11 (e.g. wxMSW).
		static const unsigned letter_scancodes[] = {
			0x26, 0x38, 0x36, 0x28, 0x1a, 0x29, 0x2a, 0x2b, 0x1f, 0x2c,
			0x2d, 0x2e, 0x3a, 0x39, 0x20, 0x21, 0x18, 0x1b, 0x27, 0x1c,
			0x1e, 0x37, 0x19, 0x35, 0x1d, 0x34,
		};
		return letter_scancodes[key_code - 'A'];
	}
	if (key_code >= 'a' && key_code <= 'z') {
		return scancode_from_wx_key_code(key_code - ('a' - 'A'));
	}
	if (key_code >= '0' && key_code <= '9') {
		static const unsigned digit_scancodes[] = {
			0x13, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
		};
		return digit_scancodes[key_code - '0'];
	}

	return 0;
}

unsigned InputNativeScancodeFromKeyEvent(const wxKeyEvent &event)
{
	// keyboard_map_key() is keyed by X11 hardware keycodes, so the raw-keycode
	// paths are only meaningful under wxGTK/X11. On wxMSW the raw values are
	// Windows scancodes/VK codes that the X11 table can't map (and GetRawKeyFlags
	// is non-zero, which would wrongly short-circuit the portable fallback), so
	// fall straight through to the wx-keycode mapping there.
#if defined(__WXGTK__)
	const unsigned hardware = static_cast<unsigned>(event.GetRawKeyFlags());
	if (hardware != 0) {
		return hardware;
	}

	const unsigned raw = static_cast<unsigned>(event.GetRawKeyCode());
	if (raw != 0 && keyboard_map_key(raw) != nullptr) {
		return raw;
	}
#endif

	int keycode = event.GetKeyCode();

	/* A letter typed with Control held arrives as a control character (1-26)
	   rather than the letter code on the wx-keycode platforms (notably wxOSX,
	   where Ctrl-C gives 3), which would otherwise map to no scancode and the
	   guest would never see the letter. Remap it back to the letter so the
	   combination reaches RISC OS. Backspace, Tab and Return share codes 8, 9
	   and 13, so leave those as their own keys. */
	if ((event.GetModifiers() & wxMOD_RAW_CONTROL) &&
	    keycode >= 1 && keycode <= 26 &&
	    keycode != WXK_BACK && keycode != WXK_TAB && keycode != WXK_RETURN) {
		keycode = 'a' + keycode - 1;
	}

	return scancode_from_wx_key_code(keycode);
}

bool InputIsReleaseMouseCaptureKey(const wxKeyEvent &event)
{
	/* Only Ctrl+End releases the mouse / exits full-screen, so that Esc itself
	 * passes through to RISC OS as a normal key. */
	const int key_code = event.GetKeyCode();
	if ((key_code == WXK_END || key_code == WXK_NUMPAD_END) &&
	    (event.GetModifiers() & wxMOD_CONTROL)) {
		return true;
	}

	return false;
}
