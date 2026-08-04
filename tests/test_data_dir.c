/*
 * Where the data directory comes from, and above all WHEN THE USER IS ASKED.
 *
 * Issue #67: RPCEmu created a folder in the user's home directory without
 * asking and offered no way to move it. Asking on first run fixes that, and
 * introduces a worse bug if the conditions are wrong, because every one of
 * these is a real user having their day interrupted:
 *
 *   - somebody who has run RPCEmu for a year, asked about data they already have
 *   - a developer whose configs/ sit next to the binary on purpose
 *   - a headless container, blocked on a dialogue nobody can see
 *   - a CI job silently redirected by a preference a person once clicked
 *
 * None of that is visible from reading a precedence chain, so the chain is a
 * pure function over stated inputs and this walks it. Nothing here touches a
 * filesystem or a window, so it runs on all three platforms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_dir_choice.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/** Nothing set at all: a first run on a clean machine, with a GUI. */
static DataDirInputs
blank(void)
{
	DataDirInputs in;

	memset(&in, 0, sizeof(in));
	in.can_ask = 1;
	return in;
}

static DataDirDecision
decide(const DataDirInputs *in)
{
	return data_dir_decide(in);
}

/*
 * The order things win in. Each case sets everything below it as well, so it is
 * testing that the higher one WINS rather than merely that it works alone.
 */
static void
test_precedence(void)
{
	DataDirInputs in;

	puts("Precedence, strongest first:");

	/* --datadir over absolutely everything. */
	in = blank();
	in.cli_datadir = "/tmp/cli";
	in.env_datadir = "/tmp/env";
	in.stored_datadir = "/tmp/stored";
	in.configs_beside_binary = 1;
	in.configs_in_bundle = 1;
	in.configs_in_install_dir = 1;
	in.default_location_ready = 1;
	check("--datadir beats everything", decide(&in).source == DATA_DIR_FROM_CLI);

	/* The environment over the remembered choice. The important one: a
	   scripted or CI run must not be at the mercy of a past click. */
	in.cli_datadir = NULL;
	check("RPCEMU_DATADIR beats the remembered choice",
	    decide(&in).source == DATA_DIR_FROM_ENV);

	in.env_datadir = NULL;
	check("the remembered choice beats anything found by looking",
	    decide(&in).source == DATA_DIR_FROM_STORED);

	in.stored_datadir = NULL;
	check("a layout beside the binary beats a bundle and the default",
	    decide(&in).source == DATA_DIR_FROM_PORTABLE);

	in.configs_beside_binary = 0;
	check("a bundle beats the install prefix",
	    decide(&in).source == DATA_DIR_FROM_BUNDLE);

	in.configs_in_bundle = 0;
	check("the install prefix beats the existing default",
	    decide(&in).source == DATA_DIR_FROM_PORTABLE);

	in.configs_in_install_dir = 0;
	check("an existing default location is used rather than asked about",
	    decide(&in).source == DATA_DIR_FROM_EXISTING_DEFAULT);

	in.default_location_ready = 0;
	check("with nothing at all, the user is asked",
	    decide(&in).source == DATA_DIR_ASK);
}

/*
 * ★ THE CASES THAT MUST NOT PRODUCE A DIALOGUE. Every one of these is somebody
 * being interrupted for no reason, which is how a fix for #67 turns into a
 * complaint about #67's fix.
 */
static void
test_who_is_never_asked(void)
{
	DataDirInputs in;

	puts("Nobody is asked unless there is genuinely nothing to go on:");

	in = blank();
	in.default_location_ready = 1;
	check("an existing user upgrading is NOT asked", decide(&in).should_ask == 0);
	check("and their location is remembered, so it is not inferred again",
	    decide(&in).should_remember == 1);

	in = blank();
	in.configs_beside_binary = 1;
	check("a portable or in-tree layout is NOT asked",
	    decide(&in).should_ask == 0);

	in = blank();
	in.configs_in_cwd = 1;
	check("running from a source tree is NOT asked",
	    decide(&in).should_ask == 0);

	in = blank();
	in.configs_in_install_dir = 1;
	check("an installed copy is NOT asked", decide(&in).should_ask == 0);

	in = blank();
	in.configs_in_bundle = 1;
	check("a macOS bundle is NOT asked", decide(&in).should_ask == 0);

	in = blank();
	in.stored_datadir = "/somewhere";
	check("somebody who has already chosen is NOT asked again",
	    decide(&in).should_ask == 0);

	in = blank();
	in.env_datadir = "/somewhere";
	check("an environment variable is NOT asked about",
	    decide(&in).should_ask == 0);

	in = blank();
	in.cli_datadir = "/somewhere";
	check("--datadir is NOT asked about", decide(&in).should_ask == 0);

	/* Headless: the dialogue is impossible, so the old behaviour stands. */
	in = blank();
	in.can_ask = 0;
	check("with no GUI, the default is taken silently",
	    decide(&in).source == DATA_DIR_DEFAULT_UNASKED);
	check("and no dialogue is attempted", decide(&in).should_ask == 0);

	/* Only ever one situation produces a dialogue. */
	in = blank();
	check("the ONLY case that asks is a first run with a GUI",
	    decide(&in).source == DATA_DIR_ASK && decide(&in).should_ask == 1);
}

/*
 * What gets written back. Remembering the wrong thing is quieter than failing to
 * remember, and worse: it changes where a later run looks.
 */
static void
test_what_is_remembered(void)
{
	DataDirInputs in;

	puts("What is written back to the preference store:");

	in = blank();
	check("a first-run answer is remembered", decide(&in).should_remember == 1);

	in = blank();
	in.default_location_ready = 1;
	check("an existing default is recorded once", decide(&in).should_remember == 1);

	/* Supplied every run, so recording them would let one odd scripted run
	   become the default for every interactive one afterwards. */
	in = blank();
	in.cli_datadir = "/tmp/x";
	check("--datadir is NOT remembered", decide(&in).should_remember == 0);

	in = blank();
	in.env_datadir = "/tmp/x";
	check("RPCEMU_DATADIR is NOT remembered", decide(&in).should_remember == 0);

	/* Pinning this would break the next copy unpacked somewhere else. */
	in = blank();
	in.configs_beside_binary = 1;
	check("a portable layout is NOT remembered", decide(&in).should_remember == 0);

	in = blank();
	in.configs_in_bundle = 1;
	check("a bundle is NOT remembered", decide(&in).should_remember == 0);

	in = blank();
	in.stored_datadir = "/already";
	check("an existing stored choice is not rewritten",
	    decide(&in).should_remember == 0);

	/* The subtle one. A headless first run must leave the question open, or
	   the first interactive run inherits a choice nobody made. */
	in = blank();
	in.can_ask = 0;
	check("a headless first run does NOT remember anything",
	    decide(&in).should_remember == 0);
}

/* An empty string is not a value. Environment variables in particular arrive
   set-but-empty all the time, and treating that as a path would send the data
   directory to "". */
static void
test_empty_is_not_set(void)
{
	DataDirInputs in;

	puts("Empty strings are absent, not paths:");

	in = blank();
	in.cli_datadir = "";
	in.env_datadir = "";
	in.env_resource_dir = "";
	in.stored_datadir = "";
	check("empty values are ignored entirely",
	    decide(&in).source == DATA_DIR_ASK);

	in = blank();
	in.env_datadir = "";
	in.default_location_ready = 1;
	check("an empty variable does not shadow the existing default",
	    decide(&in).source == DATA_DIR_FROM_EXISTING_DEFAULT);
}

/* RPCEMU_RESOURCE_DIR is the read-only-payload case: it says where the payload
   is, not where user data goes, so it sits below anything that settles both. */
static void
test_resource_dir(void)
{
	DataDirInputs in;

	puts("RPCEMU_RESOURCE_DIR, which settles only the payload:");

	in = blank();
	in.env_resource_dir = "/opt/rpcemu";
	check("it is used when nothing else applies",
	    decide(&in).source == DATA_DIR_FROM_ENV_RESOURCE);
	check("and does not trigger a dialogue", decide(&in).should_ask == 0);

	in.configs_beside_binary = 1;
	check("a layout beside the binary still wins",
	    decide(&in).source == DATA_DIR_FROM_PORTABLE);

	in = blank();
	in.env_resource_dir = "/opt/rpcemu";
	in.default_location_ready = 1;
	check("it outranks the existing default, being explicit",
	    decide(&in).source == DATA_DIR_FROM_ENV_RESOURCE);
}

/*
 * Every combination, checked for the properties that must hold no matter what.
 * A precedence chain is exactly the kind of code where a case nobody thought of
 * falls through to something unintended, so this stops relying on the cases
 * above being imaginative enough.
 */
static void
test_exhaustive_invariants(void)
{
	unsigned mask;
	int asked_without_gui = 0;
	int asked_with_something_to_go_on = 0;
	int remembered_an_explicit_input = 0;
	int no_source = 0;
	int ask_count = 0;

	puts("Every combination of inputs (512), for the invariants:");

	for (mask = 0; mask < 512u; mask++) {
		DataDirInputs in;
		DataDirDecision d;
		int have_something;

		memset(&in, 0, sizeof(in));
		in.cli_datadir          = (mask & 1u)   ? "/a" : NULL;
		in.env_datadir          = (mask & 2u)   ? "/b" : NULL;
		in.env_resource_dir     = (mask & 4u)   ? "/c" : NULL;
		in.stored_datadir       = (mask & 8u)   ? "/d" : NULL;
		in.configs_beside_binary = (mask & 16u)  ? 1 : 0;
		in.configs_in_cwd        = (mask & 32u)  ? 1 : 0;
		in.configs_in_install_dir = (mask & 64u) ? 1 : 0;
		in.configs_in_bundle     = (mask & 128u) ? 1 : 0;
		in.default_location_ready = (mask & 256u) ? 1 : 0;
		/* can_ask is swept separately below so both halves see every mask. */
		in.can_ask = 1;

		d = decide(&in);

		have_something = in.cli_datadir || in.env_datadir ||
		    in.env_resource_dir || in.stored_datadir ||
		    in.configs_beside_binary || in.configs_in_cwd ||
		    in.configs_in_install_dir || in.configs_in_bundle ||
		    in.default_location_ready;

		if (d.should_ask && have_something) {
			asked_with_something_to_go_on++;
		}
		if (d.should_ask && d.source != DATA_DIR_ASK) {
			no_source++;
		}
		if (d.source == DATA_DIR_ASK) {
			ask_count++;
		}
		if (d.should_remember &&
		    (in.cli_datadir != NULL || in.env_datadir != NULL)) {
			remembered_an_explicit_input++;
		}

		/* Now the same inputs with no GUI. */
		in.can_ask = 0;
		d = decide(&in);
		if (d.should_ask) {
			asked_without_gui++;
		}
	}

	check("no dialogue is ever raised without a GUI", asked_without_gui == 0);
	check("no dialogue when there was something to go on",
	    asked_with_something_to_go_on == 0);
	check("should_ask is only ever set alongside DATA_DIR_ASK", no_source == 0);
	check("exactly one of the 512 combinations asks", ask_count == 1);
	check("the command line and environment are never remembered",
	    remembered_an_explicit_input == 0);
}

/* Names are logged, so a missing one would read as a bug report saying
   "unknown". */
static void
test_source_names(void)
{
	static const DataDirSource all[] = {
		DATA_DIR_FROM_CLI, DATA_DIR_FROM_ENV, DATA_DIR_FROM_ENV_RESOURCE,
		DATA_DIR_FROM_STORED, DATA_DIR_FROM_PORTABLE, DATA_DIR_FROM_BUNDLE,
		DATA_DIR_FROM_EXISTING_DEFAULT, DATA_DIR_ASK,
		DATA_DIR_DEFAULT_UNASKED
	};
	size_t i;
	int unnamed = 0;

	puts("Every source has a name for the log:");
	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const char *name = data_dir_source_name(all[i]);

		if (name == NULL || name[0] == '\0' ||
		    strcmp(name, "unknown") == 0) {
			unnamed++;
		}
	}
	check("all nine sources are named", unnamed == 0);
}

int
main(void)
{
	test_precedence();
	test_who_is_never_asked();
	test_what_is_remembered();
	test_empty_is_not_set();
	test_resource_dir();
	test_exhaustive_invariants();
	test_source_names();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall data directory checks passed");
	return EXIT_SUCCESS;
}
