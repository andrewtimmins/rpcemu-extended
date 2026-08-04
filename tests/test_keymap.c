/*
 * Physical keys to RISC OS keys, for all three platforms at once.
 *
 * WHY THIS TEST CAN EXIST AT ALL. The two things under test are pure lookup
 * tables over integers, with no wxWidgets, no window and no platform calls
 * anywhere near them. That is deliberate: it means the Windows table and the
 * macOS table are both checkable from a Linux runner, and the keyboard stops
 * being the one input path that could only be tested by a person sitting at
 * three different machines. Before this there was no keyboard test of any kind.
 *
 * WHAT WENT WRONG WITHOUT IT. Windows and macOS used to identify a key by the
 * CHARACTER the host layout produced and look that up in a table written for a
 * UK keyboard. Three reported faults came out of that one decision:
 *
 *   #88  A German keyboard. Umlauts dead, because no table entry types 'u'
 *        with an umlaut. z and y swapped "the wrong way around", because
 *        Windows had already applied German and RISC OS then applied it again.
 *   #91  The third mouse button on macOS, because Command and Control are
 *        different POSITIONS producing the same character, and a character
 *        cannot tell them apart.
 *   #70  Left and right Shift were reported to the guest as the same key, so
 *        changing hands mid-typing released Shift in the guest: the second
 *        Shift's press was swallowed as a duplicate of a key already held, and
 *        the first Shift's release then cancelled a modifier still physically
 *        down. Hence the distinctness checks below being load-bearing rather
 *        than tidiness.
 *
 * So the cases below are not decoration. They assert the properties that were
 * actually violated: that a position maps to the same RISC OS key whatever the
 * host layout thinks it types, that keys differing only by position stay
 * distinct, and that nothing in either table maps to a key keyboard_x.c cannot
 * deliver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "keymap_platform.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* Windows Set 1 scancodes, by position rather than by what they type. */
#define WSC_ESCAPE	0x01
#define WSC_1		0x02
#define WSC_MINUS	0x0c
#define WSC_BACKSPACE	0x0e
#define WSC_TAB		0x0f
#define WSC_Q		0x10
#define WSC_Y		0x15	/* types 'z' on a German keyboard */
#define WSC_LEFTBRACKET	0x1a	/* types 'u' umlaut on a German keyboard */
#define WSC_RETURN	0x1c
#define WSC_LEFTCTRL	0x1d
#define WSC_A		0x1e
#define WSC_SEMICOLON	0x27	/* types 'o' umlaut on a German keyboard */
#define WSC_QUOTE	0x28	/* types 'a' umlaut on a German keyboard */
#define WSC_HASH	0x2b	/* international, # on UK */
#define WSC_LEFTSHIFT	0x2a
#define WSC_Z		0x2c	/* types 'y' on a German keyboard */
#define WSC_SLASH	0x35
#define WSC_RIGHTSHIFT	0x36
#define WSC_LEFTALT	0x38
#define WSC_SPACE	0x39
#define WSC_CAPSLOCK	0x3a
#define WSC_F1		0x3b
#define WSC_NUMLOCK	0x45
#define WSC_KP_7	0x47
#define WSC_SYSREQ	0x54
#define WSC_UNASSIGNED	0x55
#define WSC_BACKSLASH	0x56	/* international, <>| on German */
#define WSC_F11		0x57
#define WSC_F12		0x58

/* macOS virtual keycodes, likewise. Names match Apple's kVK_ constants, which
   keymap_platform.c asserts these numbers against when built on macOS. */
#define MVK_A		0x00
#define MVK_Z		0x06
#define MVK_ISO_SECTION	0x0a
#define MVK_Q		0x0c
#define MVK_Y		0x10
#define MVK_1		0x12
#define MVK_5		0x17
#define MVK_6		0x16
#define MVK_RETURN	0x24
#define MVK_TAB		0x30
#define MVK_SPACE	0x31
#define MVK_BACKSPACE	0x33
#define MVK_ESCAPE	0x35
#define MVK_RIGHTCOMMAND 0x36
#define MVK_COMMAND	0x37
#define MVK_SHIFT	0x38
#define MVK_CAPSLOCK	0x39
#define MVK_OPTION	0x3a
#define MVK_CONTROL	0x3b
#define MVK_RIGHTSHIFT	0x3c
#define MVK_RIGHTOPTION	0x3d
#define MVK_RIGHTCONTROL 0x3e
#define MVK_FUNCTION	0x3f
#define MVK_KEYPAD_EQUALS 0x51
#define MVK_F1		0x7a
#define MVK_LEFTARROW	0x7b
#define MVK_UPARROW	0x7e

/* X11 keycodes, the shared currency both tables produce. */
#define X11_ESCAPE	0x09
#define X11_1		0x0a
#define X11_MINUS	0x14
#define X11_BACKSPACE	0x16
#define X11_TAB		0x17
#define X11_Q		0x18
#define X11_Y		0x1d
#define X11_LEFTBRACKET	0x22
#define X11_RETURN	0x24
#define X11_LEFTCTRL	0x25
#define X11_A		0x26
#define X11_SEMICOLON	0x2f
#define X11_QUOTE	0x30
#define X11_LEFTSHIFT	0x32
#define X11_HASH	0x33
#define X11_Z		0x34
#define X11_SLASH	0x3d
#define X11_RIGHTSHIFT	0x3e
#define X11_LEFTALT	0x40
#define X11_SPACE	0x41
#define X11_CAPSLOCK	0x42
#define X11_F1		0x43
#define X11_NUMLOCK	0x4d
#define X11_KP_7	0x4f
#define X11_BACKSLASH	0x5e
#define X11_F11		0x5f
#define X11_F12		0x60
#define X11_KP_ENTER	0x68
#define X11_RIGHTCTRL	0x69
#define X11_KP_DIVIDE	0x6a
#define X11_RIGHTALT	0x6c
#define X11_UP		0x6f
#define X11_LEFT	0x71
#define X11_DELETE	0x77
#define X11_LEFTWIN	0x85
#define X11_RIGHTWIN	0x86
#define X11_MENU	0x87

/* PS/2 Set 2 make codes, so the chain can be followed all the way to the wire. */
#define PS2_A		0x1c
#define PS2_Q		0x15
#define PS2_Y		0x35
#define PS2_Z		0x1a
#define PS2_LEFTSHIFT	0x12
#define PS2_RIGHTSHIFT	0x59

static void
win(const char *what, uint32_t scancode, int extended, uint32_t expected)
{
	char label[128];
	const uint32_t got = keymap_x11_from_windows_scancode(scancode, extended);

	snprintf(label, sizeof(label), "windows %s0x%02x -> x11 0x%02x",
	    extended ? "e0 " : "", scancode, expected);
	if (got != expected) {
		snprintf(label + strlen(label), sizeof(label) - strlen(label),
		    " (got 0x%02x)", got);
	}
	check(label, got == expected);
	(void) what;
}

static void
mac(const char *what, uint32_t keycode, uint32_t expected)
{
	char label[128];
	const uint32_t got = keymap_x11_from_macos_keycode(keycode);

	snprintf(label, sizeof(label), "macos 0x%02x (%s) -> x11 0x%02x",
	    keycode, what, expected);
	if (got != expected) {
		snprintf(label + strlen(label), sizeof(label) - strlen(label),
		    " (got 0x%02x)", got);
	}
	check(label, got == expected);
}

/** First PS/2 make code for an X11 keycode, or 0 when it maps to nothing. */
static uint8_t
ps2_of(uint32_t x11)
{
	const uint8_t *codes = keyboard_map_key(x11);

	return codes != NULL ? codes[0] : 0;
}

/*
 * The contract keymap_platform.c documents: a non-zero return is always a
 * keycode keyboard_map_key() knows. If that fails, a key silently reaches the
 * guest as nothing at all, which is exactly the class of bug being fixed, so it
 * is checked exhaustively rather than by sampling.
 */
static void
test_no_dead_ends(void)
{
	uint32_t sc, vk;
	int windows_mapped = 0, macos_mapped = 0;
	int dead = 0;

	puts("Every mapped position resolves to a real RISC OS key:");

	for (sc = 1; sc <= KEYMAP_WINDOWS_SCANCODE_MAX; sc++) {
		const uint32_t x11 = keymap_x11_from_windows_scancode(sc, 0);

		if (x11 == 0) {
			continue;
		}
		windows_mapped++;
		if (ps2_of(x11) == 0) {
			printf("    windows 0x%02x -> x11 0x%02x -> nothing\n", sc, x11);
			dead++;
		}
	}
	/* The E0 block is sparse, so sweep the whole byte rather than a range. */
	for (sc = 0; sc <= 0xff; sc++) {
		const uint32_t x11 = keymap_x11_from_windows_scancode(sc, 1);

		if (x11 == 0) {
			continue;
		}
		windows_mapped++;
		if (ps2_of(x11) == 0) {
			printf("    windows e0 0x%02x -> x11 0x%02x -> nothing\n", sc, x11);
			dead++;
		}
	}
	for (vk = 0; vk <= 0xff; vk++) {
		const uint32_t x11 = keymap_x11_from_macos_keycode(vk);

		if (x11 == 0) {
			continue;
		}
		macos_mapped++;
		if (ps2_of(x11) == 0) {
			printf("    macos 0x%02x -> x11 0x%02x -> nothing\n", vk, x11);
			dead++;
		}
	}

	check("no windows or macos position maps to an unknown key", dead == 0);

	/* A table that mapped almost nothing would pass the check above without
	   being any use, so assert it is actually populated. A PC keyboard has
	   about 100 keys and both tables should be within reach of that. */
	printf("    windows positions mapped: %d, macos: %d\n",
	    windows_mapped, macos_mapped);
	check("windows table covers a whole keyboard", windows_mapped >= 100);
	check("macos table covers a whole keyboard", macos_mapped >= 100);
}

/*
 * Two physical keys must never become one RISC OS key. This is the property
 * whose absence made issue #91 impossible to fix at the character level, and it
 * is why the Command policy lives in input_helpers.cpp rather than in the table.
 */
static void
test_no_collisions(void)
{
	int seen[0x100];
	uint32_t sc, vk;
	int collisions = 0;

	puts("Distinct positions stay distinct:");

	memset(seen, 0, sizeof(seen));
	for (sc = 1; sc <= KEYMAP_WINDOWS_SCANCODE_MAX; sc++) {
		const uint32_t x11 = keymap_x11_from_windows_scancode(sc, 0);

		if (x11 != 0 && seen[x11]++) {
			printf("    windows x11 0x%02x reached twice\n", x11);
			collisions++;
		}
	}
	for (sc = 0; sc <= 0xff; sc++) {
		const uint32_t x11 = keymap_x11_from_windows_scancode(sc, 1);

		if (x11 != 0 && seen[x11]++) {
			printf("    windows e0 x11 0x%02x reached twice\n", x11);
			collisions++;
		}
	}
	check("no two windows positions share a RISC OS key", collisions == 0);

	memset(seen, 0, sizeof(seen));
	collisions = 0;
	for (vk = 0; vk <= 0xff; vk++) {
		const uint32_t x11 = keymap_x11_from_macos_keycode(vk);

		if (x11 != 0 && seen[x11]++) {
			printf("    macos x11 0x%02x reached twice\n", x11);
			collisions++;
		}
	}
	check("no two macos positions share a RISC OS key", collisions == 0);
}

/*
 * Issue #88. The point is not that these particular keys work, it is that the
 * answer does not depend on what the host layout says the key types. A German
 * keyboard puts z where a UK one puts y; both are scancode 0x15 and both must
 * arrive as RISC OS's Y key, leaving RISC OS to apply its own layout ONCE.
 */
static void
test_layout_independence(void)
{
	puts("Issue #88 - a position means the same thing on any layout:");

	win("Y position", WSC_Y, 0, X11_Y);
	win("Z position", WSC_Z, 0, X11_Z);
	check("windows Y position reaches the guest's Y",
	    ps2_of(keymap_x11_from_windows_scancode(WSC_Y, 0)) == PS2_Y);
	check("windows Z position reaches the guest's Z",
	    ps2_of(keymap_x11_from_windows_scancode(WSC_Z, 0)) == PS2_Z);

	/* The keys a German layout puts umlauts on. Each used to be dead, because
	   the character it produced was in no table. */
	win("umlaut u position", WSC_LEFTBRACKET, 0, X11_LEFTBRACKET);
	win("umlaut o position", WSC_SEMICOLON, 0, X11_SEMICOLON);
	win("umlaut a position", WSC_QUOTE, 0, X11_QUOTE);

	/* The two keys that exist only on some layouts, and are the ones a German
	   keyboard uses for <>| and #. */
	win("international <>| beside left shift", WSC_BACKSLASH, 0, X11_BACKSLASH);
	win("international # beside return", WSC_HASH, 0, X11_HASH);

	/* AltGr, which a German keyboard cannot manage without, and which reached
	   nothing at all before: right Alt was reported as left Alt. */
	win("AltGr", 0x38, 1, X11_RIGHTALT);
	check("AltGr is not left Alt",
	    keymap_x11_from_windows_scancode(0x38, 1) !=
	    keymap_x11_from_windows_scancode(WSC_LEFTALT, 0));
	check("AltGr reaches the guest", ps2_of(X11_RIGHTALT) != 0);

	mac("Y position", MVK_Y, X11_Y);
	mac("Z position", MVK_Z, X11_Z);
	mac("ISO extra key", MVK_ISO_SECTION, X11_BACKSLASH);
	mac("right Option is AltGr", MVK_RIGHTOPTION, X11_RIGHTALT);
}

/*
 * Issues #91 and #70. Keys that differ only by which side of the keyboard they
 * are on had been collapsed onto one another.
 */
static void
test_left_and_right_are_distinct(void)
{
	puts("Issues #91 and #70 - left and right are not the same key:");

	win("left shift", WSC_LEFTSHIFT, 0, X11_LEFTSHIFT);
	win("right shift", WSC_RIGHTSHIFT, 0, X11_RIGHTSHIFT);
	check("windows shifts differ",
	    keymap_x11_from_windows_scancode(WSC_LEFTSHIFT, 0) !=
	    keymap_x11_from_windows_scancode(WSC_RIGHTSHIFT, 0));
	check("the guest sees two different shifts",
	    ps2_of(X11_LEFTSHIFT) == PS2_LEFTSHIFT &&
	    ps2_of(X11_RIGHTSHIFT) == PS2_RIGHTSHIFT);

	win("left ctrl", WSC_LEFTCTRL, 0, X11_LEFTCTRL);
	win("right ctrl", 0x1d, 1, X11_RIGHTCTRL);
	check("windows ctrls differ",
	    keymap_x11_from_windows_scancode(WSC_LEFTCTRL, 0) !=
	    keymap_x11_from_windows_scancode(0x1d, 1));

	mac("left shift", MVK_SHIFT, X11_LEFTSHIFT);
	mac("right shift", MVK_RIGHTSHIFT, X11_RIGHTSHIFT);
	mac("left control", MVK_CONTROL, X11_LEFTCTRL);
	mac("right control", MVK_RIGHTCONTROL, X11_RIGHTCTRL);

	/* The heart of issue #91: wxWidgets reports Command as WXK_CONTROL, so at
	   the character level Command and Control are one key and neither can be
	   spared for the third mouse button. By position they are four keys. */
	mac("left command", MVK_COMMAND, X11_LEFTWIN);
	mac("right command", MVK_RIGHTCOMMAND, X11_RIGHTWIN);
	check("command is distinguishable from control",
	    keymap_x11_from_macos_keycode(MVK_COMMAND) !=
	    keymap_x11_from_macos_keycode(MVK_CONTROL));
	check("the two commands are distinguishable from each other",
	    keymap_x11_from_macos_keycode(MVK_COMMAND) !=
	    keymap_x11_from_macos_keycode(MVK_RIGHTCOMMAND));
	/* input_helpers.cpp turns right Command into this. If the menu key stopped
	   reaching the guest, the third mouse button would go with it. */
	check("the menu key the third mouse button needs is deliverable",
	    ps2_of(X11_MENU) != 0);
}

/*
 * Known answers, end to end. Deliberately spelled out per platform rather than
 * generated, so that a table edited wrongly on one platform cannot be excused by
 * a matching edit to the other.
 */
static void
test_known_answers(void)
{
	puts("Known answers, position to PS/2:");

	win("escape", WSC_ESCAPE, 0, X11_ESCAPE);
	win("1", WSC_1, 0, X11_1);
	win("minus", WSC_MINUS, 0, X11_MINUS);
	win("backspace", WSC_BACKSPACE, 0, X11_BACKSPACE);
	win("tab", WSC_TAB, 0, X11_TAB);
	win("Q", WSC_Q, 0, X11_Q);
	win("return", WSC_RETURN, 0, X11_RETURN);
	win("A", WSC_A, 0, X11_A);
	win("slash", WSC_SLASH, 0, X11_SLASH);
	win("space", WSC_SPACE, 0, X11_SPACE);
	win("caps lock", WSC_CAPSLOCK, 0, X11_CAPSLOCK);
	win("F1", WSC_F1, 0, X11_F1);
	win("F11", WSC_F11, 0, X11_F11);
	win("F12", WSC_F12, 0, X11_F12);
	win("num lock", WSC_NUMLOCK, 0, X11_NUMLOCK);
	win("keypad 7", WSC_KP_7, 0, X11_KP_7);
	win("keypad enter", 0x1c, 1, X11_KP_ENTER);
	win("keypad divide", 0x35, 1, X11_KP_DIVIDE);
	win("up arrow", 0x48, 1, X11_UP);
	win("left arrow", 0x4b, 1, X11_LEFT);
	win("delete", 0x53, 1, X11_DELETE);
	win("menu", 0x5d, 1, X11_MENU);

	mac("A", MVK_A, X11_A);
	mac("Q", MVK_Q, X11_Q);
	mac("1", MVK_1, X11_1);
	/* 5 and 6 are the wrong way round in ADB numbering, which is the sort of
	   thing a hand-written table gets wrong in exactly one direction. */
	mac("5", MVK_5, 0x0e);
	mac("6", MVK_6, 0x0f);
	mac("return", MVK_RETURN, X11_RETURN);
	mac("tab", MVK_TAB, X11_TAB);
	mac("space", MVK_SPACE, X11_SPACE);
	mac("backspace", MVK_BACKSPACE, X11_BACKSPACE);
	mac("escape", MVK_ESCAPE, X11_ESCAPE);
	mac("caps lock", MVK_CAPSLOCK, X11_CAPSLOCK);
	mac("left option is left alt", MVK_OPTION, X11_LEFTALT);
	mac("F1", MVK_F1, X11_F1);
	mac("left arrow", MVK_LEFTARROW, X11_LEFT);
	mac("up arrow", MVK_UPARROW, X11_UP);

	/* Both platforms must agree about where a key is, since both feed the same
	   guest. A test per platform would let the two drift apart. */
	check("windows and macos agree on A",
	    keymap_x11_from_windows_scancode(WSC_A, 0) ==
	    keymap_x11_from_macos_keycode(MVK_A));
	check("windows and macos agree on Q",
	    keymap_x11_from_windows_scancode(WSC_Q, 0) ==
	    keymap_x11_from_macos_keycode(MVK_Q));
	check("windows and macos agree on Y",
	    keymap_x11_from_windows_scancode(WSC_Y, 0) ==
	    keymap_x11_from_macos_keycode(MVK_Y));
	check("A reaches PS/2 0x1c from both",
	    ps2_of(keymap_x11_from_windows_scancode(WSC_A, 0)) == PS2_A &&
	    ps2_of(keymap_x11_from_macos_keycode(MVK_A)) == PS2_A);
	check("Q reaches PS/2 0x15 from both",
	    ps2_of(keymap_x11_from_windows_scancode(WSC_Q, 0)) == PS2_Q &&
	    ps2_of(keymap_x11_from_macos_keycode(MVK_Q)) == PS2_Q);
}

/*
 * Keys with no RISC OS equivalent, and inputs that are simply not keys. These
 * must answer zero rather than something plausible: the caller reads zero as
 * "let the character fallback try", and a wrong non-zero answer would press a
 * key nobody touched.
 */
static void
test_refusals(void)
{
	puts("Positions with no RISC OS equivalent refuse to guess:");

	win("scancode 0 is not a key", 0, 0, 0);
	win("SysReq", WSC_SYSREQ, 0, 0);
	win("unassigned", WSC_UNASSIGNED, 0, 0);
	win("past the end of the block", KEYMAP_WINDOWS_SCANCODE_MAX + 1, 0, 0);
	win("0xff", 0xff, 0, 0);
	/* An unprefixed scancode is not an E0 one: 0x1e is A, but E0 1E is nothing. */
	win("A is not an extended key", WSC_A, 1, 0);

	mac("fn has no PC equivalent", MVK_FUNCTION, 0);
	mac("keypad equals has no PC equivalent", MVK_KEYPAD_EQUALS, 0);
	mac("0xff", 0xff, 0);
}

/*
 * Unpacking the Windows message parameter. Worth its own cases because the
 * failure mode is silent: a shift that is off by four still produces a
 * scancode, just the wrong one, and the result is a key that does something
 * unrelated rather than an obvious crash.
 *
 * The lParam values are built the way Windows builds them - repeat count in the
 * low half, scancode at bit 16, extended at bit 24, and the transition and
 * previous-state bits set as they would be on a real release - so the masking
 * has to ignore everything it should ignore.
 */
static void
test_windows_lparam(void)
{
	puts("Unpacking the Windows WM_KEY* lParam:");

	/* A press of A: repeat count 1, scancode 0x1e, nothing else set. */
	check("plain press of A",
	    keymap_x11_from_windows_lparam(0x001e0001u) == X11_A);

	/* The same key released: bits 30 and 31 set. Must not change the answer. */
	check("release of A ignores the transition bits",
	    keymap_x11_from_windows_lparam(0xc01e0001u) == X11_A);

	/* Held down: a large repeat count in the low half must be masked off. */
	check("autorepeat count is ignored",
	    keymap_x11_from_windows_lparam(0x001effffu) == X11_A);

	/* Right Ctrl: same scancode as left Ctrl, extended bit set. This pair is
	   the reason the flag cannot be ignored. */
	check("left ctrl without the extended bit",
	    keymap_x11_from_windows_lparam(0x001d0001u) == X11_LEFTCTRL);
	check("right ctrl with the extended bit",
	    keymap_x11_from_windows_lparam(0x011d0001u) == X11_RIGHTCTRL);

	/* AltGr, the key issue #88 needs and the reason bit 24 is read at all. */
	check("AltGr with the extended bit",
	    keymap_x11_from_windows_lparam(0x01380001u) == X11_RIGHTALT);
	check("left alt without it",
	    keymap_x11_from_windows_lparam(0x00380001u) == X11_LEFTALT);

	/* The context-code bit (29) is set whenever Alt is held, so it rides along
	   with every AltGr combination and must not disturb the lookup. */
	check("the Alt context bit is ignored",
	    keymap_x11_from_windows_lparam(0x21380001u) == X11_RIGHTALT);

	/* Bits above the extended flag must not leak into the scancode. Note 0xff
	   in the top byte would ALSO set bit 24 and legitimately give 0, so the
	   reserved and state bits are set here without it - which is the only way
	   this case tests the masking rather than the extended flag again. */
	check("bits above the extended flag are ignored",
	    keymap_x11_from_windows_lparam(0xfe1e0001u) == X11_A);
	check("setting bit 24 does change the answer",
	    keymap_x11_from_windows_lparam(0x011e0001u) == 0);
	check("an empty lparam is not a key",
	    keymap_x11_from_windows_lparam(0) == 0);
}

int
main(void)
{
	test_no_dead_ends();
	test_no_collisions();
	test_layout_independence();
	test_left_and_right_are_distinct();
	test_known_answers();
	test_windows_lparam();
	test_refusals();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall keymap checks passed");
	return EXIT_SUCCESS;
}
