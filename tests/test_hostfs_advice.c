/*
 * What the machine editor says about a chosen HostFS folder.
 *
 * ★ THIS TEST EXISTS BECAUSE THE DECISION WAS WRONG IN A WAY THAT LOOKED RIGHT.
 * The editor warned "there is no !Boot here" when the chosen folder existed and
 * was empty, and in a separate branch said "does not exist yet, it will be
 * created" when it did not. Both branches read sensibly. Together they left the
 * one case that ALWAYS needs the warning - a folder about to be created, which is
 * necessarily empty - as the only case that never got it.
 *
 * David Ramsden hit exactly that: a new machine, HostFS pointed at C:\foo, accept
 * the restart, and a machine that will not reach its desktop with nothing on
 * screen to say why. Reproduced here on Linux and under Wine on the Windows
 * build, on RISC OS 5.31 (a bare grey screen with no icon bar) and 5.30 (the
 * supervisor prompt). Nothing was broken and nothing was lost - an empty folder
 * is an empty hard disc, and RISC OS does not announce a missing !Boot.
 *
 * The decision is therefore kept out of the dialog, where a wx build is needed to
 * exercise it, and checked here on every platform instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostfs_advice.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* The regression. Everything else here is the surrounding behaviour. */
static void
test_folder_that_does_not_exist_yet(void)
{
	puts("A folder that does not exist yet gets BOTH warnings:");

	check("it says the folder will be created",
	    (hostfs_advice(0, 0) & HOSTFS_ADVICE_WILL_CREATE) != 0);
	check("and it says there will be no !Boot - the case that was missed",
	    (hostfs_advice(0, 0) & HOSTFS_ADVICE_NO_BOOT) != 0);

	/*
	 * A caller claiming a !Boot in a folder that does not exist is contradicting
	 * itself. The answer is decided here rather than taken on trust, because the
	 * dialog works it out from two separate filesystem calls and a wrong answer
	 * would silence the warning again.
	 */
	check("a claimed !Boot in a folder that is not there is not believed",
	    (hostfs_advice(0, 1) & HOSTFS_ADVICE_NO_BOOT) != 0);
}

static void
test_folder_that_exists(void)
{
	puts("A folder that exists is judged on what is in it:");

	check("empty: warned about the missing !Boot",
	    hostfs_advice(1, 0) == HOSTFS_ADVICE_NO_BOOT);
	check("empty: not told it will be created, because it is already there",
	    (hostfs_advice(1, 0) & HOSTFS_ADVICE_WILL_CREATE) == 0);
	check("with a !Boot: nothing to say at all",
	    hostfs_advice(1, 1) == 0);
}

/*
 * The wording is checked for the two things that would actually mislead: naming
 * only one of the two symptoms, and claiming the machine is broken when it is
 * not. Not checked word for word - that would fail on every edit and teach us to
 * stop reading it.
 */
static void
test_wording(void)
{
	const char *no_boot = hostfs_advice_text(HOSTFS_ADVICE_NO_BOOT);
	const char *create = hostfs_advice_text(HOSTFS_ADVICE_WILL_CREATE);

	puts("The wording covers both symptoms and does not overstate:");

	check("there is wording for the missing !Boot", no_boot != NULL);
	check("and for the folder being created", create != NULL);

	if (no_boot != NULL) {
		/* An earlier version promised the supervisor prompt, so on 5.31 - the
		   version ROOL are shipping - it described something the user would
		   never see. Both, or the warning misleads half its readers. */
		check("it mentions the grey screen (what 5.31 does)",
		    strstr(no_boot, "grey") != NULL);
		check("it mentions the supervisor prompt (what 5.30 does)",
		    strstr(no_boot, "supervisor") != NULL);
		check("and it says nothing is lost, because nothing is",
		    strstr(no_boot, "Nothing is lost") != NULL);
	}
	if (create != NULL) {
		check("the create wording says the folder will be empty",
		    strstr(create, "empty") != NULL);
	}

	check("a bit that does not exist gets no invented text",
	    hostfs_advice_text(0x80u) == NULL);
	check("nor does zero", hostfs_advice_text(0) == NULL);
	/* A combination is a caller mistake: it would have to pick an order and
	   punctuation, which is the caller's job. */
	check("nor a combination of bits",
	    hostfs_advice_text(HOSTFS_ADVICE_WILL_CREATE |
	        HOSTFS_ADVICE_NO_BOOT) == NULL);
}

int
main(void)
{
	test_folder_that_does_not_exist_yet();
	test_folder_that_exists();
	test_wording();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall HostFS advice checks passed");
	return EXIT_SUCCESS;
}
