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

#ifndef HELD_KEYS_H
#define HELD_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which keys the host is holding down, and therefore which keys RISC OS should
 * believe are held.
 *
 * WHY THIS IS NOT JUST A SET OF SCANCODES. Several physical keys can legitimately
 * map to one RISC OS key. Sometimes because the host has more keys than a RiscPC
 * (on macOS both Command and Control are sent as Ctrl, so that Cmd+C copies),
 * and historically because a mapping could not tell two keys apart at all.
 *
 * Track scancodes alone and a hand-over releases a key that is still held:
 *
 *   press left Shift    -> guest told Shift down
 *   press right Shift   -> same scancode, already held, nothing sent
 *   release left Shift  -> guest told Shift UP, though right Shift is still down
 *
 * Every letter after that arrives unshifted. That is issue #70, where changing
 * hands part way through a word silently dropped the modifier.
 *
 * So each entry is keyed by the PHYSICAL key, and the guest is told about a
 * scancode when the FIRST physical key mapping to it goes down and again when
 * the LAST one comes up. Keying by physical key also keeps the duplicate-press
 * guard honest: a platform that repeats a press for one key is still ignored,
 * which a reference count on the scancode alone would get wrong by leaving the
 * key stuck down.
 */

/** More than any keyboard can hold at once, rollover and modifiers included. */
#define HELD_KEYS_MAX 64

typedef struct {
	unsigned key_id;	/**< The physical key, unique per key */
	unsigned scan_code;	/**< What it is being sent to the guest as */
} HeldKey;

typedef struct {
	HeldKey keys[HELD_KEYS_MAX];
	size_t count;
} HeldKeys;

/** Forget everything, without telling the guest. */
void held_keys_reset(HeldKeys *held);

/**
 * Record a press.
 *
 * @param key_id    The physical key, distinct for every key on the keyboard.
 * @param scan_code What the guest should be told, after any platform policy.
 * @return Non-zero if the guest should now be sent a press for @scan_code, i.e.
 *         this is the first physical key mapping to it. Zero if the guest
 *         already believes that key is held, or if there is no room to record
 *         another key.
 */
int held_keys_press(HeldKeys *held, unsigned key_id, unsigned scan_code);

/**
 * Record a release.
 *
 * @return Non-zero if the guest should now be sent a release for the scancode,
 *         i.e. no other physical key holding it remains. Zero if this key was
 *         not down, or if another key is still holding the same scancode. The
 *         scancode to release is the one recorded at press time, which is
 *         returned through @scan_code_out.
 */
int held_keys_release(HeldKeys *held, unsigned key_id, unsigned *scan_code_out);

/**
 * Everything that is down, as the set of scancodes to release, each once.
 *
 * For the cases where the host stops telling us about keys at all, such as the
 * window losing focus, so the guest is not left with a key held forever.
 * Empties the set.
 *
 * @param out Filled with the distinct scancodes to release.
 * @param max Capacity of @out.
 * @return How many were written.
 */
size_t held_keys_release_all(HeldKeys *held, unsigned *out, size_t max);

/** How many physical keys are currently recorded as down. */
size_t held_keys_count(const HeldKeys *held);

#ifdef __cplusplus
}
#endif

#endif /* HELD_KEYS_H */
