/*
 * Names for guest addresses.
 *
 * WHY THIS EXISTS. The value of a symbol table is entirely that the name
 * beside an address can be trusted. A backtrace reading "main+0x24" is acted
 * on; the person goes and reads main. If that attribution is wrong they read
 * the wrong function, and nothing about the output tells them so - it is a
 * plausible answer to the question they asked, which is the worst kind of
 * wrong a debugger can be.
 *
 * Almost all of that risk lives in one decision: WHAT NOT TO NAME. A table
 * covers the addresses its author knew about, and the debugger will be asked
 * about many others - ROM, module workspace, wherever a wild branch landed.
 * The naive implementation returns the nearest symbol at or below the address,
 * which cheerfully labels an address in the RISC OS kernel as "main+0x3f21c".
 * So the cases below lean hard on the boundaries: below the first symbol,
 * between symbols, at the exact end of a symbol, and past the end of the last
 * one.
 *
 * The other half is loading. A file that is not a symbol file must be refused
 * outright rather than yielding whichever handful of its lines happened to
 * parse, because a table that is silently 3 entries instead of 3000 produces
 * exactly the mis-attribution described above.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugsym.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/** Where the temporary symbol files are written; set from argv[1]. */
static const char *work_dir = ".";

static char path_buf[1024];

/**
 * Write a symbol file and return its path.
 */
static const char *
write_file(const char *name, const char *contents)
{
	FILE *f;

	snprintf(path_buf, sizeof(path_buf), "%s/%s", work_dir, name);
	f = fopen(path_buf, "w");
	if (f == NULL) {
		printf("FAIL: cannot write %s\n", path_buf);
		failures++;
		return path_buf;
	}
	fputs(contents, f);
	fclose(f);
	return path_buf;
}

/**
 * The address must resolve to this name and offset.
 */
static void
lookup_is(uint32_t address, const char *name, uint32_t offset)
{
	uint32_t got_offset = 0xdeadbeef;
	const char *got = debugsym_lookup(address, &got_offset);
	char what[160];

	snprintf(what, sizeof(what), "0x%X is %s+0x%X", address, name, offset);
	check(what, got != NULL && strcmp(got, name) == 0 && got_offset == offset);
	if (got == NULL) {
		printf("      got nothing\n");
	} else if (strcmp(got, name) != 0 || got_offset != offset) {
		printf("      got %s+0x%X\n", got, got_offset);
	}
}

/**
 * The address must not be attributed to anything.
 */
static void
lookup_none(uint32_t address)
{
	const char *got = debugsym_lookup(address, NULL);
	char what[160];

	snprintf(what, sizeof(what), "0x%X is not named", address);
	check(what, got == NULL);
	if (got != NULL) {
		printf("      wrongly attributed to %s\n", got);
	}
}

/* ---- tests --------------------------------------------------------------- */

static void
test_loading(void)
{
	uint32_t count = 0;
	const char *error = NULL;

	printf("Loading\n");

	check("loading a good file succeeds",
	      debugsym_load_file(write_file("good.sym",
	          "00008000 main\n"
	          "00008100 parse\n"
	          "00008200 emit\n"), &count, &error));
	check("... reporting how many were loaded", count == 3);
	check("... and the table says the same", debugsym_count() == 3);

	/* Every spelling of an address people will actually use */
	check("prefixes and comments are handled",
	      debugsym_load_file(write_file("mixed.sym",
	          "# a comment line\n"
	          "\n"
	          "  00008000   main  \n"
	          "&8100 parse\n"
	          "0x8200 emit\n"
	          "0X8300 finish ; trailing comment\n"), &count, NULL));
	check("... loading every symbol", count == 4);
	lookup_is(0x8100, "parse", 0);
	lookup_is(0x8300, "finish", 0);

	/* Names may contain spaces - RISC OS module names do */
	check("names with spaces survive",
	      debugsym_load_file(write_file("spaces.sym",
	          "00008000 Shared C Library\n"), &count, NULL));
	lookup_is(0x8000, "Shared C Library", 0);

	/* Order in the file must not matter */
	check("an unsorted file is sorted on load",
	      debugsym_load_file(write_file("unsorted.sym",
	          "00008200 emit\n"
	          "00008000 main\n"
	          "00008100 parse\n"), &count, NULL));
	lookup_is(0x8000, "main", 0);
	lookup_is(0x8100, "parse", 0);
	lookup_is(0x8200, "emit", 0);
}

static void
test_bad_files(void)
{
	uint32_t count = 99;
	const char *error = NULL;

	printf("Files that are not symbol files\n");

	/* The dangerous outcome is a partial load: three symbols out of a file
	   that was never a symbol table, silently mis-attributing everything */
	check("a file of prose is refused",
	      !debugsym_load_file(write_file("prose.txt",
	          "This is not a symbol file at all.\n"
	          "It is a README.\n"), &count, &error));
	check("... with a reason given", error != NULL);
	check("... and nothing left loaded", debugsym_count() == 0);

	check("a file that starts well and then goes wrong is refused",
	      !debugsym_load_file(write_file("partial.sym",
	          "00008000 main\n"
	          "00008100 parse\n"
	          "and then some prose\n"), &count, NULL));
	check("... leaving nothing loaded, not a partial table",
	      debugsym_count() == 0);

	check("an address with no name is refused",
	      !debugsym_load_file(write_file("noname.sym", "00008000\n"), &count, NULL));

	check("an empty file is refused",
	      !debugsym_load_file(write_file("empty.sym", ""), &count, NULL));

	check("a file of only comments is refused",
	      !debugsym_load_file(write_file("comments.sym", "# nothing here\n"),
	          &count, NULL));

	check("a missing file is refused",
	      !debugsym_load_file("/nonexistent/nowhere.sym", &count, &error));
	check("... with a reason given", error != NULL);

	check("a null path is refused", !debugsym_load_file(NULL, &count, NULL));
}

static void
test_attribution(void)
{
	printf("What gets named, and what does not\n");

	debugsym_load_file(write_file("range.sym",
	    "00008000 main\n"
	    "00008040 parse\n"
	    "00009000 emit\n"), NULL, NULL);

	/* Inside a symbol */
	lookup_is(0x8000, "main", 0);
	lookup_is(0x8004, "main", 4);
	lookup_is(0x803c, "main", 0x3c);

	/* A symbol ends exactly where the next begins */
	lookup_is(0x8040, "parse", 0);
	lookup_is(0x8fff, "parse", 0xfbf);
	lookup_is(0x9000, "emit", 0);

	/* Below the first symbol is nobody's */
	lookup_none(0x7fff);
	lookup_none(0);

	/* And past the last one. This is the case that matters: without a
	   bound, an address in ROM or in the kernel would be reported as
	   emit+something, which reads as an answer and is not one. */
	lookup_none(0x9000 + 0x10000);
	lookup_none(0x03800000);	/* ROM */
	lookup_none(0xffffffff);

	/* Two symbols at one address must not confuse the search */
	debugsym_load_file(write_file("dup.sym",
	    "00008000 first\n"
	    "00008000 second\n"
	    "00008100 later\n"), NULL, NULL);
	check("a duplicated address resolves to one of them",
	      debugsym_lookup(0x8000, NULL) != NULL);
	lookup_is(0x8100, "later", 0);

	/* A single-symbol table still bounds itself */
	debugsym_load_file(write_file("one.sym", "00008000 only\n"), NULL, NULL);
	lookup_is(0x8000, "only", 0);
	lookup_none(0x7fff);
	lookup_none(0x8000 + 0x10000);
}

static void
test_resolve(void)
{
	uint32_t address = 0;

	printf("Names to addresses\n");

	debugsym_load_file(write_file("resolve.sym",
	    "00008000 main\n"
	    "00008100 parse\n"), NULL, NULL);

	check("a name resolves", debugsym_resolve("main", &address));
	check("... to its address", address == 0x8000);
	check("another name resolves", debugsym_resolve("parse", &address));
	check("... to its address", address == 0x8100);

	check("an unknown name does not resolve",
	      !debugsym_resolve("nosuchthing", &address));
	check("matching is case sensitive", !debugsym_resolve("MAIN", &address));
	check("a null name does not resolve", !debugsym_resolve(NULL, &address));
	check("an empty name does not resolve", !debugsym_resolve("", &address));
}

static void
test_empty_table(void)
{
	uint32_t address = 0;

	printf("With nothing loaded\n");

	debugsym_clear();

	check("the count is zero", debugsym_count() == 0);
	check("nothing is named", debugsym_lookup(0x8000, NULL) == NULL);
	check("nothing resolves", !debugsym_resolve("main", &address));

	/* Clearing twice must be harmless */
	debugsym_clear();
	check("clearing an empty table is harmless", debugsym_count() == 0);
}

int
main(int argc, char *argv[])
{
	/* Required rather than defaulting to ".": this test writes the files it
	   then loads, and a default of the working directory means running it by
	   hand from a source tree scatters a dozen .sym files through it. */
	if (argc < 2) {
		fprintf(stderr, "usage: test_debugsym <writable-directory>\n");
		return 2;
	}
	work_dir = argv[1];

	printf("Guest symbol table\n\n");

	test_loading();
	test_bad_files();
	test_attribution();
	test_resolve();
	test_empty_table();

	debugsym_clear();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
