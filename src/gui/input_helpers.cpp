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

#include <stdlib.h>

#include "keymap_platform.h"

extern "C" {
#include "keyboard.h"
#include "rpcemu.h"
}

/*
 * X11 keycodes this file needs by name. The full set lives in keymap_platform.c;
 * these are the few the macOS policy below has to reason about.
 */
enum {
	X11_LEFTCTRL = 0x25,
	X11_LEFTWIN = 0x85,
	X11_RIGHTWIN = 0x86,
	X11_MENU = 0x87
};

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

/*
 * The key's physical position as an X11 keycode, or 0 if this platform will not
 * say. Every platform has some way of reporting position; what differs is where
 * wxWidgets puts it.
 */
static unsigned scancode_from_physical_key(const wxKeyEvent &event)
{
#if defined(__WXGTK__)
	/* X11 hands us a hardware keycode already, which is what the rest of
	   RPCEmu is keyed by, so there is nothing to translate. */
	const unsigned hardware = static_cast<unsigned>(event.GetRawKeyFlags());
	if (hardware != 0) {
		return hardware;
	}

	const unsigned raw = static_cast<unsigned>(event.GetRawKeyCode());
	if (raw != 0 && keyboard_map_key(raw) != nullptr) {
		return raw;
	}
	return 0;
#elif defined(__WXMSW__)
	/* GetRawKeyFlags() is the WM_KEY* lParam (wxWidgets src/msw/window.cpp),
	   which carries the OEM scancode and so describes a position. The unpacking
	   lives in keymap_platform.c so that tests/test_keymap.c can check it. */
	return keymap_x11_from_windows_lparam(
	    static_cast<unsigned>(event.GetRawKeyFlags()));
#elif defined(__WXOSX__)
	/* GetRawKeyCode() is [NSEvent keyCode] (wxWidgets src/osx/cocoa/window.mm),
	   an ADB keycode, which is a position. */
	return keymap_x11_from_macos_keycode(
	    static_cast<unsigned>(event.GetRawKeyCode()));
#else
	(void) event;
	return 0;
#endif
}

/*
 * What RPCEmu does with the two keys a Mac has and a RiscPC does not.
 *
 * Left Command becomes Ctrl. That is long-standing deliberate behaviour, not an
 * accident of the old character mapping: RISC OS uses Ctrl for what macOS uses
 * Command for, so Cmd+C copying is what a Mac user expects and issue #35 was
 * about making those combinations work.
 *
 * Right Command becomes the menu key, which RPCEmu already offers as the third
 * mouse button. A Mac keyboard has no menu key, so before this there was no way
 * to reach a RISC OS menu button from the keyboard at all (issue #91). Left and
 * right Command are only distinguishable now that positions are being read.
 */
static unsigned apply_macos_key_policy(unsigned scan_code)
{
#if defined(__WXOSX__)
	if (scan_code == X11_LEFTWIN) {
		return X11_LEFTCTRL;
	}
	if (scan_code == X11_RIGHTWIN) {
		return X11_MENU;
	}
#endif
	return scan_code;
}

unsigned InputNativeScancodeFromKeyEvent(const wxKeyEvent &event)
{
	const unsigned physical = scancode_from_physical_key(event);

	if (physical != 0) {
		return apply_macos_key_policy(physical);
	}

	/*
	 * Nothing said where the key was, so fall back to mapping the character it
	 * produced. This is the path that cannot cope with a non-UK layout (see
	 * keymap_platform.h), kept only because a key we failed to place is better
	 * sent to the guest approximately than dropped on the floor. Anything
	 * arriving here on Windows or macOS is a gap in the position tables and
	 * shows up in the RPCEMU_KEYBOARD_DEBUG log as such.
	 */
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
	/* Alt+Enter, as DOSBox uses. Alt and Option are the same physical key, so
	   the shortcut is the same on every platform. */
	const int key_code = event.GetKeyCode();
	return (key_code == WXK_RETURN || key_code == WXK_NUMPAD_ENTER) &&
	       (event.GetModifiers() & wxMOD_ALT) != 0;
}

unsigned InputKeyIdentityFromKeyEvent(const wxKeyEvent &event)
{
	/* The position, where the platform gave one, since that is exactly "which
	   key" and is taken before any policy folds two keys onto one scancode. */
	const unsigned physical = scancode_from_physical_key(event);

	if (physical != 0) {
		return physical;
	}

	/* No position available, so the character path is in use and the wx keycode
	   is the best identity going. Tagged above the range of any X11 keycode so a
	   fallback key can never be mistaken for a physical one, which would let a
	   release cancel the wrong key. */
	return 0x10000u | static_cast<unsigned>(event.GetKeyCode() & 0xffff);
}

bool InputIsThirdMouseButtonKey(const wxKeyEvent &event)
{
	const int key_code = event.GetKeyCode();

	if (key_code == WXK_MENU || key_code == WXK_WINDOWS_MENU) {
		return true;
	}

	/* On macOS the test above can never pass - wxWidgets does not report
	   WXK_MENU, because the keyboard has no such key - so right Command stands
	   in for it. Asked of the position, since Command is indistinguishable from
	   Control at the character level on that platform (issue #91). */
	return apply_macos_key_policy(scancode_from_physical_key(event)) == X11_MENU;
}

bool InputNeedsSyntheticRelease(const wxKeyEvent &event, unsigned scan_code)
{
#if defined(__WXOSX__)
	/*
	 * NSEvent modifier flags, which is what GetRawKeyFlags() carries here. Read
	 * from the raw flags rather than asked of wxKeyEvent, because wxWidgets folds
	 * Command and Control together on this platform - ControlDown() is true for
	 * either - and telling them apart is the whole point.
	 */
	const unsigned NS_COMMAND = 0x00100000u;
	const unsigned flags = static_cast<unsigned>(event.GetRawKeyFlags());
	const unsigned raw = static_cast<unsigned>(event.GetRawKeyCode());

	/*
	 * The modifier keys, by ADB keycode. These arrive as flagsChanged rather
	 * than as key events, and macOS does report their releases, so they are the
	 * keys this must NOT touch: synthesising a release for a modifier would drop
	 * it while it was still physically held.
	 */
	switch (raw) {
	case 0x36:	/* right Command */
	case 0x37:	/* left Command  */
	case 0x38:	/* left Shift    */
	case 0x3a:	/* left Option   */
	case 0x3b:	/* left Control  */
	case 0x3c:	/* right Shift   */
	case 0x3d:	/* right Option  */
	case 0x3e:	/* right Control */
		return false;
	default:
		break;
	}

	/*
	 * Caps Lock, which macOS reports as a press each time and never as a
	 * release. One press therefore left the guest believing the key was held for
	 * the rest of the session, and held_keys then suppressed every later press as
	 * a key already down, so RISC OS latched Caps Lock on while the host's lamp
	 * carried on toggling and came to mean the opposite of what it said
	 * (issue #148).
	 *
	 * A real keyboard sends a make and a break for one tap, and RISC OS toggles
	 * its own state from that. The caller supplies the interval between them.
	 */
	if (raw == 0x39) {	/* kVK_CapsLock */
		return true;
	}

	/*
	 * A key pressed while Command is held, whose keyUp macOS keeps to itself.
	 * The press arrives, the release never does, and the guest is left with the
	 * key down: RISC OS starts repeating it, and nothing stops it because no
	 * release is ever coming (issue #149, "Cmd-L gives repeating Ls").
	 *
	 * Command is a shortcut modifier on a Mac, so one action per press is what is
	 * wanted anyway - and it is the only behaviour available, the alternative
	 * being a key held down for ever.
	 */
	if ((flags & NS_COMMAND) != 0u) {
		return true;
	}

	(void) scan_code;
	return false;
#else
	(void) event;
	(void) scan_code;
	return false;
#endif
}

void InputLogKeyEvent(const wxKeyEvent &event, unsigned scan_code, bool key_down)
{
	static int enabled = -1;

	if (enabled < 0) {
		enabled = getenv("RPCEMU_KEYBOARD_DEBUG") != nullptr;
	}
	if (!enabled) {
		return;
	}

	const unsigned physical = scancode_from_physical_key(event);
	const uint8_t *mapped = scan_code != 0 ? keyboard_map_key(scan_code) : nullptr;

	/* "position" is what the platform said, "scancode" is what RPCEmu will send
	   after any policy is applied, and "ps2" is what the guest actually receives.
	   A position of 0 means the physical tables did not place the key and the
	   character fallback was used, which is the shape of issues #88 and #91. */
	rpclog("Keyboard: %s raw=0x%08x flags=0x%08x wxkey=%d position=0x%02x "
	       "scancode=0x%02x ps2=%s\n",
	       key_down ? "down" : "up  ",
	       (unsigned) event.GetRawKeyCode(),
	       (unsigned) event.GetRawKeyFlags(),
	       event.GetKeyCode(),
	       physical,
	       scan_code,
	       mapped != nullptr ? "yes" : "DROPPED");
}
