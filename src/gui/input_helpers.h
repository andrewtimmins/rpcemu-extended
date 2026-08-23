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

#ifndef INPUT_HELPERS_H
#define INPUT_HELPERS_H

#include <wx/event.h>

extern "C" {
#include "keyboard.h"
}

/**
 * The X11 keycode ("native scancode") for the key this event came from.
 *
 * Derived from the key's PHYSICAL POSITION wherever the platform will say what
 * that was, so that the host's keyboard layout is not applied on top of the one
 * RISC OS is already applying. See keymap_platform.h for why that matters and
 * which issues it fixes.
 *
 * @return X11 keycode, or 0 when the key has no RISC OS equivalent.
 */
unsigned InputNativeScancodeFromKeyEvent(const wxKeyEvent &event);

/**
 * An identifier for the PHYSICAL key this event came from, distinct for every
 * key on the keyboard.
 *
 * Needed because a scancode is not a key: several physical keys can map to one
 * RISC OS key, deliberately so on macOS, and tracking what is held by scancode
 * alone means a hand-over releases a modifier that is still down (issue #70).
 * See held_keys.h.
 *
 * Not a scancode and not comparable with one. It is only ever used to tell one
 * held key from another, so its numeric value carries no meaning beyond being
 * stable for a given key and unique between keys.
 */
unsigned InputKeyIdentityFromKeyEvent(const wxKeyEvent &event);

bool InputIsReleaseMouseCaptureKey(const wxKeyEvent &event);

/**
 * Is this the key that stands in for the third (menu) mouse button?
 *
 * The menu key everywhere it exists, and additionally right Command on macOS,
 * where there is no menu key at all and wxWidgets never reports WXK_MENU
 * (issue #91).
 */
bool InputIsThirdMouseButtonKey(const wxKeyEvent &event);

/**
 * Will the host never send a release for this press, so one has to be made up?
 *
 * macOS has two of these, and a key whose release never arrives is not a key
 * that misbehaves once - it is held for ever, because nothing else will ever say
 * otherwise. Both were reported as separate faults (issues #148 and #149) and
 * are the same thing underneath.
 *
 * Always false off macOS, where a press is answered by a release.
 *
 * @param event     The key event as it arrived.
 * @param scan_code What InputNativeScancodeFromKeyEvent() returned for it.
 */
bool InputNeedsSyntheticRelease(const wxKeyEvent &event, unsigned scan_code);


/**
 * Log one key event to rpclog.txt when RPCEMU_KEYBOARD_DEBUG is set in the
 * environment.
 *
 * Off by default and free when off. This is how a keyboard fault on a platform
 * the developer cannot run gets diagnosed: it prints what the host reported
 * (raw code, raw flags, wx keycode) beside what RPCEmu made of it, so a reporter
 * can be asked for a log rather than for adjectives.
 *
 * @param event     The key event as it arrived.
 * @param scan_code What InputNativeScancodeFromKeyEvent() returned for it.
 * @param key_down  True for a press, false for a release.
 */
void InputLogKeyEvent(const wxKeyEvent &event, unsigned scan_code, bool key_down);

#endif
