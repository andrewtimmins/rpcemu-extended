/*
 * Which keys the guest believes are held, and the ordering rules that decide it.
 *
 * WHY THIS IS A TEST RATHER THAN A COMMENT. This is the regression test for
 * issue #70, where holding Shift and typing produced
 *
 *     QWERTYUIOPASDfghjklzxcvbnm
 *
 * on macOS. The cause was that held keys were tracked as a set of SCANCODES,
 * and several physical keys can map to one scancode. wxWidgets reports both
 * Shift keys identically, so changing hands part way through:
 *
 *     press right Shift    -> guest told Shift down
 *     press left Shift     -> same scancode, already held, nothing sent
 *     release right Shift  -> guest told Shift UP, though left Shift is down
 *
 * and every letter after that arrived unshifted. Tracking by physical key
 * instead fixes it, and this file pins the behaviour that makes it work: first
 * key down tells the guest, last key up tells the guest, and nothing in between.
 *
 * The bug class outlives the specific fault. macOS deliberately maps both
 * Command and Control onto the guest's Ctrl so that Cmd+C copies, so two
 * physical keys sharing one scancode is a permanent, intended state of affairs
 * rather than an accident that has been tidied away. Hence testing the rule and
 * not just the Shift case.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "held_keys.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* Physical keys, as X11 keycodes, which is what the real identities are. */
#define KEY_LSHIFT	0x32
#define KEY_RSHIFT	0x3e
#define KEY_LCTRL	0x25
#define KEY_LCOMMAND	0x85
#define KEY_A		0x26
#define KEY_B		0x38

/* Scancodes sent to the guest. Left and right Shift are different keys to RISC
   OS; Command and Control are deliberately the same one. */
#define SC_LSHIFT	0x32
#define SC_RSHIFT	0x3e
#define SC_CTRL		0x25
#define SC_A		0x26
#define SC_B		0x38

/*
 * Issue #70 itself, as the sequence that produced it. Note this passes even
 * with the old scancode-set behaviour, because RISC OS now receives two
 * DIFFERENT scancodes for the two Shift keys - so it is the case below that
 * carries the real weight.
 */
static void
test_shift_handover_distinct_scancodes(void)
{
	HeldKeys h;
	unsigned sc = 0;

	puts("Issue #70, changing Shift hands mid-word:");
	held_keys_reset(&h);

	check("right Shift down tells the guest",
	    held_keys_press(&h, KEY_RSHIFT, SC_RSHIFT) != 0);
	check("left Shift down also tells the guest, being a different key",
	    held_keys_press(&h, KEY_LSHIFT, SC_LSHIFT) != 0);
	check("releasing right Shift releases only right Shift",
	    held_keys_release(&h, KEY_RSHIFT, &sc) != 0 && sc == SC_RSHIFT);
	check("left Shift is still held", held_keys_count(&h) == 1);
	check("releasing left Shift releases left Shift",
	    held_keys_release(&h, KEY_LSHIFT, &sc) != 0 && sc == SC_LSHIFT);
	check("nothing left held", held_keys_count(&h) == 0);
}

/*
 * ★ THE ONE THAT MATTERS. Two physical keys deliberately mapped to ONE
 * scancode, which is macOS's Command and Control. This is the exact shape of
 * #70, and the only difference from the case above is that the two keys share a
 * scancode - which is what the old code could not survive.
 */
static void
test_two_keys_one_scancode(void)
{
	HeldKeys h;
	unsigned sc = 0;

	puts("Two physical keys sharing one guest key (macOS Command and Control):");
	held_keys_reset(&h);

	check("Control down tells the guest to hold Ctrl",
	    held_keys_press(&h, KEY_LCTRL, SC_CTRL) != 0);
	/* The old bug: this was swallowed, and then the release below let go of a
	   modifier the user was still holding. */
	check("Command down does NOT repeat the press",
	    held_keys_press(&h, KEY_LCOMMAND, SC_CTRL) == 0);
	check("both are recorded", held_keys_count(&h) == 2);

	check("releasing Control does NOT release Ctrl, Command still holds it",
	    held_keys_release(&h, KEY_LCTRL, &sc) == 0);
	check("Command alone is still held", held_keys_count(&h) == 1);

	check("releasing Command finally releases Ctrl",
	    held_keys_release(&h, KEY_LCOMMAND, &sc) != 0 && sc == SC_CTRL);
	check("nothing left held", held_keys_count(&h) == 0);

	/* And the other order, since a user may let go in either. */
	held_keys_reset(&h);
	check("reverse order: Command first, then Control",
	    held_keys_press(&h, KEY_LCOMMAND, SC_CTRL) != 0 &&
	    held_keys_press(&h, KEY_LCTRL, SC_CTRL) == 0);
	check("releasing Command does not release Ctrl",
	    held_keys_release(&h, KEY_LCOMMAND, &sc) == 0);
	check("releasing Control does",
	    held_keys_release(&h, KEY_LCTRL, &sc) != 0 && sc == SC_CTRL);
}

/*
 * Why this is keyed by physical key and not simply counted per scancode. A
 * reference count would be fooled by a platform repeating a press, ending up
 * needing two releases and leaving the key stuck down in the guest.
 */
static void
test_repeat_press_of_one_key(void)
{
	HeldKeys h;
	unsigned sc = 0;

	puts("A repeated press of the SAME key is not a second holder:");
	held_keys_reset(&h);

	check("first press tells the guest",
	    held_keys_press(&h, KEY_A, SC_A) != 0);
	check("second press of the same key is ignored",
	    held_keys_press(&h, KEY_A, SC_A) == 0);
	check("recorded once, not twice", held_keys_count(&h) == 1);
	check("ONE release is enough to release it",
	    held_keys_release(&h, KEY_A, &sc) != 0 && sc == SC_A);
	check("nothing left held", held_keys_count(&h) == 0);
}

/*
 * The scancode is remembered from press time rather than taken from the release
 * event. Those can differ, because a modifier can change what a platform reports
 * for the same physical key between its press and its release, and releasing a
 * scancode that was never pressed would leave the real one held forever.
 */
static void
test_release_uses_the_recorded_scancode(void)
{
	HeldKeys h;
	unsigned sc = 0;

	puts("A release uses the scancode from when the key went down:");
	held_keys_reset(&h);

	held_keys_press(&h, KEY_A, SC_A);
	check("release reports the scancode that was pressed",
	    held_keys_release(&h, KEY_A, &sc) != 0 && sc == SC_A);

	held_keys_reset(&h);
	check("a release for a key that is not down does nothing",
	    held_keys_release(&h, KEY_B, &sc) == 0);
	check("and does not invent a held key", held_keys_count(&h) == 0);
}

/* Losing focus with keys down must not leave the guest holding them. */
static void
test_release_all(void)
{
	HeldKeys h;
	unsigned out[HELD_KEYS_MAX];
	size_t n;

	puts("Releasing everything, for when the host stops telling us:");
	held_keys_reset(&h);

	held_keys_press(&h, KEY_LSHIFT, SC_LSHIFT);
	held_keys_press(&h, KEY_A, SC_A);
	held_keys_press(&h, KEY_LCTRL, SC_CTRL);
	held_keys_press(&h, KEY_LCOMMAND, SC_CTRL);	/* shares SC_CTRL */

	n = held_keys_release_all(&h, out, HELD_KEYS_MAX);

	/* Four keys down but only three distinct guest keys, and Ctrl must be
	   released once rather than twice. */
	check("three distinct scancodes for four held keys", n == 3);
	check("nothing is left held afterwards", held_keys_count(&h) == 0);
	{
		size_t i, ctrl = 0, shift = 0, a = 0;

		for (i = 0; i < n; i++) {
			ctrl += out[i] == SC_CTRL;
			shift += out[i] == SC_LSHIFT;
			a += out[i] == SC_A;
		}
		check("Ctrl released exactly once", ctrl == 1);
		check("Shift released exactly once", shift == 1);
		check("the letter released exactly once", a == 1);
	}

	/* Doing it twice must be harmless. ReleaseHeldKeys() is called from more
	   than one place and the second call has nothing to do. */
	n = held_keys_release_all(&h, out, HELD_KEYS_MAX);
	check("releasing everything again releases nothing", n == 0);
}

/*
 * Running out of room. The important property is not the limit but that press
 * and release stay symmetrical across it: a key that was refused must not be
 * found on release, or it would release a scancode somebody else is holding.
 */
static void
test_capacity(void)
{
	HeldKeys h;
	unsigned sc = 0;
	unsigned out[4];
	unsigned i;
	int refused = 0;

	puts("More keys at once than can be recorded:");
	held_keys_reset(&h);

	/* Distinct keys and distinct scancodes, so every one would otherwise be
	   reported as a fresh press. */
	for (i = 0; i < HELD_KEYS_MAX; i++) {
		if (!held_keys_press(&h, 0x1000 + i, 0x2000 + i)) {
			refused++;
		}
	}
	check("everything up to the limit was accepted", refused == 0);
	check("full", held_keys_count(&h) == HELD_KEYS_MAX);

	check("one more is refused rather than overflowing",
	    held_keys_press(&h, 0x9999, 0x8888) == 0);
	check("and is not recorded", held_keys_count(&h) == HELD_KEYS_MAX);
	check("so its release does nothing",
	    held_keys_release(&h, 0x9999, &sc) == 0);
	check("leaving the real keys alone", held_keys_count(&h) == HELD_KEYS_MAX);

	/* A caller with a small output buffer must not be written past. */
	{
		const size_t n = held_keys_release_all(&h, out, 4);

		check("release_all honours the buffer it was given", n == 4);
		check("and still empties the set", held_keys_count(&h) == 0);
	}
}

/* An ordinary burst of typing, in case a rule above is right in isolation and
   wrong in sequence. */
static void
test_ordinary_typing(void)
{
	HeldKeys h;
	unsigned sc = 0;
	int presses = 0, releases = 0;
	int i;

	puts("Ordinary typing with a modifier held throughout:");
	held_keys_reset(&h);

	presses += held_keys_press(&h, KEY_LSHIFT, SC_LSHIFT) != 0;
	for (i = 0; i < 20; i++) {
		presses += held_keys_press(&h, KEY_A, SC_A) != 0;
		releases += held_keys_release(&h, KEY_A, &sc) != 0;
	}
	releases += held_keys_release(&h, KEY_LSHIFT, &sc) != 0;

	check("every letter reached the guest", presses == 21);
	check("every letter was released", releases == 21);
	check("and the modifier survived all of it", held_keys_count(&h) == 0);
}

int
main(void)
{
	test_shift_handover_distinct_scancodes();
	test_two_keys_one_scancode();
	test_repeat_press_of_one_key();
	test_release_uses_the_recorded_scancode();
	test_release_all();
	test_capacity();
	test_ordinary_typing();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall held-key checks passed");
	return EXIT_SUCCESS;
}
