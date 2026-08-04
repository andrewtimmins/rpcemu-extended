/*
 * The one-line file that records where the data directory is.
 *
 * WHY IT IS A FILE OF OUR OWN rather than wxConfig, which is where every other
 * preference lives: it has to be readable from the entry points that run before
 * wxWidgets is started, because a headless run initialises no GUI toolkit at
 * all. Both alternatives were tried and measured:
 *
 *   - Reading wxConfig before wxEntry() asserts "create wxApp before calling
 *     this" and answers nothing.
 *   - Starting wxWidgets just for the read initialises GTK and prints
 *     "Unable to initialize GTK+, is DISPLAY set properly?" to stderr on
 *     exactly the headless server this feature exists to serve, and then fails
 *     anyway, so the choice was silently ignored.
 *
 * The path rules are a pure function over the environment values that decide
 * them, so ALL THREE PLATFORMS' RULES ARE CHECKED FROM WHICHEVER ONE IS RUNNING.
 * The read and write cases use a real file under a temporary HOME, which is why
 * this test is given a working directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "data_dir_store.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
expect_path(const char *what, DataDirStorePlatform platform, const char *home,
            const char *xdg, const char *appdata, const char *expected)
{
	char buf[1024];
	char label[256];
	const int ok = data_dir_store_path_for(platform, home, xdg, appdata,
	    buf, sizeof(buf));

	snprintf(label, sizeof(label), "%s", what);
	if (expected == NULL) {
		check(label, !ok);
		return;
	}
	if (!ok) {
		snprintf(label + strlen(label), sizeof(label) - strlen(label),
		    " (refused)");
		check(label, 0);
		return;
	}
	if (strcmp(buf, expected) != 0) {
		snprintf(label + strlen(label), sizeof(label) - strlen(label),
		    " (got %.120s)", buf);
		check(label, 0);
		return;
	}
	check(label, 1);
}

/* Each platform's own convention for where an application's settings go. */
static void
test_path_rules(void)
{
	puts("Where the file goes, per platform:");

	expect_path("unix, XDG_CONFIG_HOME set", DATA_DIR_STORE_UNIX,
	    "/home/andy", "/home/andy/.cfg", NULL, "/home/andy/.cfg/rpcemu/datadir");
	expect_path("unix, no XDG so ~/.config", DATA_DIR_STORE_UNIX,
	    "/home/andy", NULL, NULL, "/home/andy/.config/rpcemu/datadir");
	expect_path("unix, empty XDG is not a value", DATA_DIR_STORE_UNIX,
	    "/home/andy", "", NULL, "/home/andy/.config/rpcemu/datadir");

	expect_path("macos uses Library/Preferences", DATA_DIR_STORE_MACOS,
	    "/Users/andy", NULL, NULL,
	    "/Users/andy/Library/Preferences/RPCEmu/datadir");
	/* XDG is a Unix idea and must not leak into the macOS rule. */
	expect_path("macos ignores XDG_CONFIG_HOME", DATA_DIR_STORE_MACOS,
	    "/Users/andy", "/Users/andy/.cfg", NULL,
	    "/Users/andy/Library/Preferences/RPCEmu/datadir");

	expect_path("windows uses APPDATA, with backslashes",
	    DATA_DIR_STORE_WINDOWS, NULL, NULL, "C:\\Users\\Andy\\AppData\\Roaming",
	    "C:\\Users\\Andy\\AppData\\Roaming\\RPCEmu\\datadir");
	/* Windows has no HOME to fall back on, so this is the one case that must
	   refuse rather than invent a path. */
	expect_path("windows without APPDATA refuses", DATA_DIR_STORE_WINDOWS,
	    "C:\\Users\\Andy", NULL, NULL, NULL);

	expect_path("unix without a home refuses", DATA_DIR_STORE_UNIX,
	    NULL, NULL, NULL, NULL);
	expect_path("macos without a home refuses", DATA_DIR_STORE_MACOS,
	    NULL, NULL, NULL, NULL);
}

/*
 * A path that will not fit must be refused, not truncated. A truncated path is a
 * preference silently written to, and read from, the wrong place.
 */
static void
test_truncation_is_refused(void)
{
	char small[8];
	char big[1024];
	char longhome[1100];

	puts("A path that does not fit is refused, not cut short:");

	check("a tiny buffer is refused",
	    data_dir_store_path_for(DATA_DIR_STORE_UNIX, "/home/andy", NULL, NULL,
	        small, sizeof(small)) == 0);
	check("a zero-length buffer is refused",
	    data_dir_store_path_for(DATA_DIR_STORE_UNIX, "/home/andy", NULL, NULL,
	        big, 0) == 0);
	check("a NULL buffer is refused",
	    data_dir_store_path_for(DATA_DIR_STORE_UNIX, "/home/andy", NULL, NULL,
	        NULL, sizeof(big)) == 0);

	/* Long enough to genuinely overflow the internal 1024-byte join. An earlier
	   version of this case used 900 characters, which FITS, and the check failed
	   because the expectation was wrong rather than the code - worth recording,
	   since "the test caught something" and "the test is right" are not the same
	   claim. */
	memset(longhome, 'x', sizeof(longhome) - 1);
	longhome[0] = '/';
	longhome[sizeof(longhome) - 1] = '\0';
	check("an absurdly long home directory is refused rather than truncated",
	    data_dir_store_path_for(DATA_DIR_STORE_UNIX, longhome, NULL, NULL,
	        big, sizeof(big)) == 0);
}

/* Reading and writing for real, under a temporary HOME. */
static void
test_read_write(void)
{
	char buf[1024];
	char path[1024];

	puts("Recording and reading back:");

	/* Nothing recorded yet: the state that makes a first run recognisable. */
	check("nothing recorded reads as nothing",
	    data_dir_store_read(buf, sizeof(buf)) == 0);

	check("writing succeeds, creating the directory it needs",
	    data_dir_store_write("/tmp/some/data/dir") != 0);
	check("the file is where the rules say",
	    data_dir_store_path(path, sizeof(path)) != 0);

	memset(buf, 0, sizeof(buf));
	check("it reads back exactly",
	    data_dir_store_read(buf, sizeof(buf)) != 0 &&
	    strcmp(buf, "/tmp/some/data/dir") == 0);

	/* Overwriting, since Settings can be used more than once. */
	check("it can be changed", data_dir_store_write("/tmp/other") != 0);
	memset(buf, 0, sizeof(buf));
	check("and reads back as the new value",
	    data_dir_store_read(buf, sizeof(buf)) != 0 &&
	    strcmp(buf, "/tmp/other") == 0);

	check("clearing succeeds", data_dir_store_clear() != 0);
	check("and it reads as nothing again",
	    data_dir_store_read(buf, sizeof(buf)) == 0);
	check("clearing when there is nothing is not an error",
	    data_dir_store_clear() != 0);

	/* An empty path is not a choice. */
	check("writing an empty path is refused", data_dir_store_write("") == 0);
	check("writing NULL is refused", data_dir_store_write(NULL) == 0);
}

/*
 * What a hand-edited or damaged file does. This is a plain text file in the
 * user's config directory, so somebody will edit it, and a stray newline or
 * trailing space must not become part of the path.
 */
static void
test_awkward_file_contents(void)
{
	char path[1024];
	char buf[1024];
	FILE *f;

	puts("A hand-edited or damaged file:");

	if (!data_dir_store_path(path, sizeof(path))) {
		check("could not work out the store path", 0);
		return;
	}

	/* Trailing whitespace and a CRLF ending, as a Windows editor would leave. */
	f = fopen(path, "w");
	if (f == NULL) {
		check("could not write the test file", 0);
		return;
	}
	fprintf(f, "/tmp/edited   \r\n");
	fclose(f);

	memset(buf, 0, sizeof(buf));
	check("trailing whitespace and CRLF are trimmed",
	    data_dir_store_read(buf, sizeof(buf)) != 0 &&
	    strcmp(buf, "/tmp/edited") == 0);

	/* Empty file: the same as never having chosen. */
	f = fopen(path, "w");
	fclose(f);
	check("an empty file reads as nothing",
	    data_dir_store_read(buf, sizeof(buf)) == 0);

	/* Only a newline. */
	f = fopen(path, "w");
	fprintf(f, "\n");
	fclose(f);
	check("a blank line reads as nothing",
	    data_dir_store_read(buf, sizeof(buf)) == 0);

	/* A path longer than the caller's buffer must be refused, not truncated:
	   half a path is worse than none, because it exists somewhere. */
	f = fopen(path, "w");
	fprintf(f, "/tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
	fclose(f);
	{
		char small[16];

		check("a value too long for the caller is refused",
		    data_dir_store_read(small, sizeof(small)) == 0);
	}

	(void) data_dir_store_clear();
}

int
main(int argc, char **argv)
{
	/* Given a directory to work in, so the real user's preferences are never
	   touched. HOME decides where the store lives on this platform. */
	if (argc > 1) {
#ifdef _WIN32
		_putenv_s("APPDATA", argv[1]);
		_putenv_s("HOME", argv[1]);
#else
		setenv("HOME", argv[1], 1);
		unsetenv("XDG_CONFIG_HOME");
#endif
	} else {
		puts("usage: test_data_dir_store <writable-dir>");
		return EXIT_FAILURE;
	}

	test_path_rules();
	test_truncation_is_refused();
	test_read_write();
	test_awkward_file_contents();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall data directory store checks passed");
	return EXIT_SUCCESS;
}
