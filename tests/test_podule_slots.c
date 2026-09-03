/*
 * Which expansion-card slots the machine fills itself.
 *
 * WHY THIS EXISTS. Issue #254. The answer to "what is in slot 1" was written
 * down in five places and no two of them agreed: the machine editor's slot rows
 * said the USB card was in slot 0 and the support card in slot 1, its own
 * footnote said slot 0 was Support and the network card was built in, an
 * unreachable branch beside it said slot 1 was Ethernet, podule_config.h said
 * what the slot rows said, and podules.md said what the footnote said. The
 * machine did none of those: it fits Support, USB, then the graphics card and
 * the network card if they are enabled, because that is the order resetrpc()
 * calls them in and addpodule() hands out the lowest free slot.
 *
 * The consequence was not cosmetic. A podule configured into a slot a built-in
 * card had taken was dropped, with one line in rpclog.txt and nothing in the
 * interface, which is how a CC Lark configured into slot 2 came to be missing
 * from a machine with networking on.
 *
 * There is one statement of the rule now, in podules.c, and both the emulator
 * and the machine editor ask it. This pins that rule, the names it gives, and
 * the two properties the fix depends on: the built-in cards are contiguous from
 * slot 0, and the first free slot moves up as cards are enabled.
 */

#include <stdio.h>
#include <string.h>

#include "podules.h"
#include "podule_config.h"
#include "network.h"
#include "podulerom.h"
#include "usbcard.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_mask(const char *what, int gfx, int net, uint32_t expected)
{
	const uint32_t got = podules_builtin_slot_mask(gfx, net);
	char buf[128];

	snprintf(buf, sizeof(buf), "%s: slots %02x", what, (unsigned) expected);
	if (got != expected) {
		printf("  %-66s FAIL (got %02x)\n", buf, (unsigned) got);
		failures++;
		return;
	}
	check(buf, 1);
}

/*
 * The four machines a user can configure.
 *
 * Support and USB are in every one of them; the other two stack up from slot 2
 * in the order the reset installs them.
 */
static void
test_the_mask(void)
{
	printf("Which slots the machine takes\n");

	check_mask("neither card", 0, 0, 0x03);
	check_mask("graphics card only", 1, 0, 0x07);
	check_mask("networking only", 0, 1, 0x07);
	check_mask("both", 1, 1, 0x0f);
}

/*
 * The first slot a user podule can go in. This is the number the machine editor
 * offers and the one issue #254 got wrong: it was a constant 2 whatever the
 * machine was.
 */
static void
test_the_first_user_slot(void)
{
	printf("The first slot left for a user podule\n");

	check("neither card: slot 2", podules_first_user_slot(0, 0) == 2);
	check("graphics card only: slot 3", podules_first_user_slot(1, 0) == 3);
	check("networking only: slot 3", podules_first_user_slot(0, 1) == 3);
	check("both: slot 4", podules_first_user_slot(1, 1) == 4);
	check("never below the fixed pair",
	      podules_first_user_slot(0, 0) >= PODULE_CONFIG_FIRST_USER_SLOT);
}

/*
 * A built-in card's slot is named by the card, not by a second copy of its name
 * kept in the interface: these are the description chunks RISC OS reads out of
 * each card's ROM for *Podules, which is what the reporter compared against.
 */
static void
test_the_names(void)
{
	const char *name;

	printf("What each built-in slot is called\n");

	name = podules_builtin_slot_name(PODULE_SLOT_SUPPORT, 0, 0);
	check("slot 0 is the support card",
	      name != NULL && strcmp(name, "RPCEmu Support") == 0);
	check("and it is the card's own description string",
	      name == podulerom_description);

	name = podules_builtin_slot_name(PODULE_SLOT_USB, 0, 0);
	check("slot 1 is the USB card",
	      name != NULL && strcmp(name, "RPCEmu USB") == 0);
	check("and it is the card's own description string",
	      name == usbcard_description);

	name = podules_builtin_slot_name(2, 0, 1);
	check("slot 2 is Ethernet with networking on and no graphics card",
	      name != NULL && strcmp(name, "RPCEmu Ethernet") == 0);
	check("and it is the card's own description string",
	      name == network_description);

	name = podules_builtin_slot_name(2, 1, 1);
	check("slot 2 is the graphics card when that is on too",
	      name != NULL && strcmp(name, "RPCEmu Graphics") == 0);
	check("which puts Ethernet in slot 3",
	      podules_builtin_slot_name(3, 1, 1) != NULL &&
	      strcmp(podules_builtin_slot_name(3, 1, 1), "RPCEmu Ethernet") == 0);

	check("slot 2 is free when neither is on",
	      podules_builtin_slot_name(2, 0, 0) == NULL);
	check("slot 3 is free with one of them on",
	      podules_builtin_slot_name(3, 0, 1) == NULL);
	check("the top slot is never built in",
	      podules_builtin_slot_name(PODULE_CONFIG_SLOTS - 1, 1, 1) == NULL);
}

/*
 * The property the machine editor relies on to lock a contiguous run of rows,
 * and the emulator to move a displaced card up rather than search: every
 * built-in card is below every free slot, on all four configurations.
 */
static void
test_the_builtins_are_contiguous(void)
{
	int gfx, net;

	printf("The built-in cards leave no gaps\n");

	for (gfx = 0; gfx <= 1; gfx++) {
		for (net = 0; net <= 1; net++) {
			const uint32_t mask = podules_builtin_slot_mask(gfx, net);
			const int first = podules_first_user_slot(gfx, net);
			char buf[128];
			int i, gap = 0;

			for (i = first; i < PODULE_CONFIG_SLOTS; i++) {
				if (mask & (1u << i)) {
					gap = 1;
				}
			}

			snprintf(buf, sizeof(buf),
			         "gfx=%d net=%d: nothing built in above slot %d",
			         gfx, net, first);
			check(buf, !gap);

			snprintf(buf, sizeof(buf),
			         "gfx=%d net=%d: the name and the mask agree", gfx, net);
			gap = 0;
			for (i = 0; i < PODULE_CONFIG_SLOTS; i++) {
				const int named =
				    podules_builtin_slot_name(i, gfx, net) != NULL;
				if (named != ((mask & (1u << i)) != 0)) {
					gap = 1;
				}
			}
			check(buf, !gap);
		}
	}
}

int
main(void)
{
	printf("Built-in expansion-card slots (issue #254)\n\n");

	test_the_mask();
	test_the_first_user_slot();
	test_the_names();
	test_the_builtins_are_contiguous();

	printf("\n%s\n", failures ? "FAILED" : "All checks passed");
	return failures ? 1 : 0;
}
