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
 * held_keys.c - which keys the guest should believe are held.
 *
 * The rationale, and the bug that prompted it, are in held_keys.h. Deliberately
 * plain C with no wxWidgets and no emulator state, so tests/test_held_keys.c can
 * exercise the ordering rules without a window or a machine. A linear scan over
 * at most 64 entries, a few times per key press, is not worth a smarter
 * structure.
 */

#include "held_keys.h"

/** Index of @key_id, or -1. */
static int
find_key(const HeldKeys *held, unsigned key_id)
{
	size_t i;

	for (i = 0; i < held->count; i++) {
		if (held->keys[i].key_id == key_id) {
			return (int) i;
		}
	}
	return -1;
}

/** Is @scan_code held by any physical key other than the one at @skip? */
static int
scancode_held_elsewhere(const HeldKeys *held, unsigned scan_code, int skip)
{
	size_t i;

	for (i = 0; i < held->count; i++) {
		if ((int) i != skip && held->keys[i].scan_code == scan_code) {
			return 1;
		}
	}
	return 0;
}

void
held_keys_reset(HeldKeys *held)
{
	held->count = 0;
}

size_t
held_keys_count(const HeldKeys *held)
{
	return held->count;
}

int
held_keys_press(HeldKeys *held, unsigned key_id, unsigned scan_code)
{
	int tell_guest;

	/* This exact key is already down, so this is a repeat the platform sent
	   without an intervening release. Recording it twice would need two
	   releases to undo and the key would stick. */
	if (find_key(held, key_id) >= 0) {
		return 0;
	}

	/* Full. Refusing to record it is what keeps press and release symmetrical:
	   a key that was never recorded will not be found on release either, so it
	   cannot release somebody else's scancode. The guest is not told, so the
	   worst case is a key that does nothing rather than one stuck down. */
	if (held->count >= HELD_KEYS_MAX) {
		return 0;
	}

	tell_guest = !scancode_held_elsewhere(held, scan_code, -1);

	held->keys[held->count].key_id = key_id;
	held->keys[held->count].scan_code = scan_code;
	held->count++;

	return tell_guest;
}

int
held_keys_release(HeldKeys *held, unsigned key_id, unsigned *scan_code_out)
{
	const int index = find_key(held, key_id);
	unsigned scan_code;
	int tell_guest;

	/* Not down as far as we know. Could be a release for a press that arrived
	   before the window had focus, or one we refused for want of room. */
	if (index < 0) {
		return 0;
	}

	scan_code = held->keys[index].scan_code;

	/* Asked before removal, skipping this entry, so "is anybody else still
	   holding this scancode" is answered about the other keys only. */
	tell_guest = !scancode_held_elsewhere(held, scan_code, index);

	held->keys[index] = held->keys[held->count - 1];
	held->count--;

	if (scan_code_out != NULL) {
		*scan_code_out = scan_code;
	}
	return tell_guest;
}

size_t
held_keys_release_all(HeldKeys *held, unsigned *out, size_t max)
{
	size_t written = 0;
	size_t i, j;

	for (i = 0; i < held->count; i++) {
		const unsigned scan_code = held->keys[i].scan_code;
		int seen = 0;

		/* Each scancode once, however many physical keys were holding it. */
		for (j = 0; j < written; j++) {
			if (out[j] == scan_code) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			continue;
		}
		if (written >= max) {
			break;
		}
		out[written++] = scan_code;
	}

	held->count = 0;
	return written;
}
