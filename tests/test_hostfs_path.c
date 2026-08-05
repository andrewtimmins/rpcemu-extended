/*
 * Where a machine's HostFS drive lives.
 *
 * Discussion #77 asked for this to be configurable so that several machines can
 * share one folder. The feature is small; the CONVENTION is the part worth
 * testing, because it is what decides whether a configuration survives having
 * its data folder moved:
 *
 *   empty     the default, <machine directory>/hostfs, and nothing is stored
 *   relative  resolved under the machine directory, so it moves with it
 *   absolute  taken as given, and the only form that can go stale
 *
 * That matters because the emulator has already been bitten by the other
 * approach: debug_socket stored a resolved ABSOLUTE path, so moving a data
 * folder left it pointing at a directory that no longer existed and the debug
 * socket could not bind. HostFS failing the same way is worse - it is the drive
 * the guest boots from.
 *
 * Pure, with no filesystem access, so the Windows and macOS forms are all
 * checked from whichever platform happens to be running.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostfs_path.h"

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
expect(const char *what, const char *configured, const char *machine_dir,
       const char *expected)
{
	char out[1024];
	char label[256];
	const int ok = hostfs_path_resolve(configured, machine_dir, out, sizeof(out));

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
	if (strcmp(out, expected) != 0) {
		snprintf(label + strlen(label), sizeof(label) - strlen(label),
		    " (got %.120s)", out);
		check(label, 0);
		return;
	}
	check(label, 1);
}

/* The default: what every existing machine has, and what must not change. */
static void
test_default(void)
{
	puts("Empty means the default, exactly as before:");

	expect("empty gives <machine dir>/hostfs", "", "/data/machines/Box/",
	    "/data/machines/Box/hostfs");
	expect("a missing separator is added", "", "/data/machines/Box",
	    "/data/machines/Box/hostfs");
	expect("NULL is the same as empty", NULL, "/data/machines/Box/",
	    "/data/machines/Box/hostfs");
	/* rpcemu_get_machine_datadir() returns a trailing separator, so this is the
	   form that actually occurs. Joining without checking would give a double
	   slash, or worse "Boxhostfs", which is a different directory that HostFS
	   would silently create. */
	expect("no double separator", "", "/data/machines/Box//",
	    "/data/machines/Box/hostfs");
}

/* Relative: the form that survives a move, and so the one to prefer. */
static void
test_relative(void)
{
	puts("A relative path resolves under the machine, so it moves with it:");

	expect("a leaf", "shared-disc", "/data/machines/Box/",
	    "/data/machines/Box/shared-disc");
	expect("a subdirectory", "discs/work", "/data/machines/Box/",
	    "/data/machines/Box/discs/work");
	expect("backslashes are normalised", "discs\\work", "/data/machines/Box/",
	    "/data/machines/Box/discs/work");

	/* The point of the convention: the same setting under a different data
	   folder gives a different, correct answer. Nothing has gone stale. */
	{
		char a[1024], b[1024];

		hostfs_path_resolve("shared", "/old/machines/Box/", a, sizeof(a));
		hostfs_path_resolve("shared", "/new/place/machines/Box/", b, sizeof(b));
		check("the same relative setting follows a moved data folder",
		    strcmp(a, "/old/machines/Box/shared") == 0 &&
		    strcmp(b, "/new/place/machines/Box/shared") == 0);
	}
}

/*
 * Absolute: what discussion #77 actually wants, since a folder shared between
 * machines cannot live under any one of them.
 */
static void
test_absolute(void)
{
	puts("An absolute path is taken as given, which is what sharing needs:");

	expect("unix", "/srv/riscos/shared", "/data/machines/Box/",
	    "/srv/riscos/shared");
	expect("a trailing separator is dropped", "/srv/riscos/shared/",
	    "/data/machines/Box/", "/srv/riscos/shared");
	expect("windows drive letter", "D:\\RiscOS\\Shared", "/data/machines/Box/",
	    "D:/RiscOS/Shared");
	expect("windows forward slashes", "D:/RiscOS/Shared",
	    "/data/machines/Box/", "D:/RiscOS/Shared");
	expect("a UNC path", "\\\\nas\\riscos", "/data/machines/Box/",
	    "//nas/riscos");
	expect("an explicit ./ is absolute, not a leaf", "./here",
	    "/data/machines/Box/", "./here");

	/* A drive root keeps its slash: "C:" alone means that drive's current
	   directory on Windows, which is not the same place as "C:/". */
	expect("a drive root keeps its separator", "C:\\", "/data/machines/Box/",
	    "C:/");
	expect("the filesystem root keeps its separator", "/",
	    "/data/machines/Box/", "/");

	check("unix paths are absolute", hostfs_path_is_absolute("/x") != 0);
	check("drive letters are absolute", hostfs_path_is_absolute("C:\\x") != 0);
	check("lowercase drive letters too", hostfs_path_is_absolute("c:/x") != 0);
	check("a bare leaf is not absolute", hostfs_path_is_absolute("hostfs") == 0);
	check("empty is not absolute", hostfs_path_is_absolute("") == 0);
	check("NULL is not absolute", hostfs_path_is_absolute(NULL) == 0);
}

/*
 * Too long to fit must be refused, not truncated. A truncated path names a
 * DIFFERENT directory, and since the resolved root is created on startup, the
 * guest's files would be written into a folder nobody chose.
 */
static void
test_truncation_is_refused(void)
{
	char out[64];
	char big[1024];
	char longdir[900];

	puts("Too long to fit is refused, never truncated:");

	check("a path longer than the buffer is refused",
	    hostfs_path_resolve("a-rather-long-directory-name-that-will-not-fit",
	        "/some/quite/long/machine/directory/", out, sizeof(out)) == 0);
	check("and the output is emptied rather than left half-written",
	    out[0] == '\0');

	memset(longdir, 'x', sizeof(longdir) - 1);
	longdir[0] = '/';
	longdir[sizeof(longdir) - 1] = '\0';
	check("a very long machine directory with the default is refused",
	    hostfs_path_resolve("", longdir, out, sizeof(out)) == 0);

	check("a zero-length buffer is refused",
	    hostfs_path_resolve("", "/data/", big, 0) == 0);
	check("a NULL buffer is refused",
	    hostfs_path_resolve("", "/data/", NULL, sizeof(big)) == 0);
}

/*
 * Recognising that two machines point at one folder. Used to warn about the
 * hazard: HostFS holds open handles and RISC OS caches directory contents, so
 * two guests writing the same tree at once can lose work.
 */
static void
test_same_root(void)
{
	puts("Spotting two machines sharing one folder:");

	check("identical paths", hostfs_path_same_root("/srv/shared", "/srv/shared"));
	check("a trailing separator does not hide it",
	    hostfs_path_same_root("/srv/shared", "/srv/shared/"));
	check("separator style does not hide it",
	    hostfs_path_same_root("\\srv\\shared", "/srv/shared"));
	check("different folders are different",
	    hostfs_path_same_root("/srv/shared", "/srv/other") == 0);
	/* A prefix is not a match: /srv/shared2 is not inside /srv/shared. */
	check("a shared prefix is not a match",
	    hostfs_path_same_root("/srv/shared", "/srv/shared2") == 0);
	/* Case is significant on purpose. Treating distinct directories as one
	   would suppress a warning that should be given. */
	check("case is significant",
	    hostfs_path_same_root("/srv/Shared", "/srv/shared") == 0);
	check("empty matches nothing", hostfs_path_same_root("", "/srv/shared") == 0);
	check("NULL matches nothing",
	    hostfs_path_same_root(NULL, "/srv/shared") == 0);

	/* The case the warning is for: two machines, both with an absolute path. */
	{
		char a[1024], b[1024];

		hostfs_path_resolve("/srv/riscos/shared", "/data/machines/One/",
		    a, sizeof(a));
		hostfs_path_resolve("/srv/riscos/shared/", "/data/machines/Two/",
		    b, sizeof(b));
		check("two machines pointed at one folder are recognised",
		    hostfs_path_same_root(a, b) != 0);
	}

	/* And the case it must NOT fire for: two machines both on the default. */
	{
		char a[1024], b[1024];

		hostfs_path_resolve("", "/data/machines/One/", a, sizeof(a));
		hostfs_path_resolve("", "/data/machines/Two/", b, sizeof(b));
		check("two machines on the default do not share a folder",
		    hostfs_path_same_root(a, b) == 0);
	}
}

/*
 * A hand-edited Windows path, after wxFileConfig has unescaped it.
 *
 * ★ WHERE THIS CAME FROM. Testing the Windows build under Wine, a config file
 * written by hand with
 *
 *     hostfs_path=C:\foo
 *
 * produced the log line "HostFS: root is 'C:oo'". wx escapes backslashes when it
 * writes a value and unescapes them when it reads one, so a file the emulator
 * wrote is fine and a hand-edited one is not. What comes back depends on the
 * letter, and these are the measured results, from reading a hand-written file
 * with wxFileConfig:
 *
 *     C:\temp -> "C:<09>emp"    C:\foo -> "C:oo"
 *     C:\new  -> "C:<0a>ew"     C:\bar -> "C:ar"
 *     C:\run  -> "C:<0d>un"
 *
 * The left column names directories Windows will not create, so HostFS served a
 * folder that was not there and the guest's disc looked empty with nothing to
 * connect it to the file that had been edited. Those are refused.
 *
 * ★ THE RIGHT COLUMN IS NOT DETECTABLE, and this test says so out loud rather
 * than leaving a reader to assume the whole class is handled. "C:oo" is a
 * well-formed drive-relative path and nothing here can tell it from one somebody
 * meant.
 */
static void
test_control_characters_refused(void)
{
	char out[1024];

	puts("A setting with control characters in it is refused:");

	/* The three escapes wx actually translates. */
	check("a tab, from an unescaped \\t in C:\\temp, is refused",
	    hostfs_path_resolve("C:\temp", "/data/machines/Box/", out,
	        sizeof(out)) == 0);
	check("and nothing is left in the buffer to be used by mistake",
	    out[0] == '\0');
	check("a newline, from \\n in C:\\new, is refused",
	    hostfs_path_resolve("C:\new", "/data/machines/Box/", out,
	        sizeof(out)) == 0);
	check("a carriage return, from \\r in C:\\run, is refused",
	    hostfs_path_resolve("C:\run", "/data/machines/Box/", out,
	        sizeof(out)) == 0);
	/* Not one wx produces, but it is in the same class and free to cover. */
	check("a form feed is refused too",
	    hostfs_path_resolve("C:\f" "oo", "/data/machines/Box/", out,
	        sizeof(out)) == 0);

	puts("What cannot be caught is recorded rather than implied:");
	/* C:\foo loses the backslash AND the f. The result is indistinguishable
	   from a path somebody meant to type, so it is used - deliberately, and
	   documented in docs/hostfs.md, which says to use forward slashes by hand. */
	check("C:\\foo arriving as 'C:oo' is used as given, undetectable",
	    hostfs_path_resolve("C:oo", "/data/machines/Box/", out,
	        sizeof(out)) != 0);
	check("and it resolves to exactly that, not to the default",
	    strcmp(out, "C:oo") == 0);

	/* The boundary, so the check cannot creep upwards into real names. A space
	   is not a control character and is legal in a folder name everywhere. */
	check("a space is still allowed",
	    hostfs_path_resolve("C:\\my folder", "/data/machines/Box/", out,
	        sizeof(out)) != 0);
	check("and resolves to the folder that was asked for",
	    strcmp(out, "C:/my folder") == 0);
}

int
main(void)
{
	test_default();
	test_relative();
	test_absolute();
	test_truncation_is_refused();
	test_control_characters_refused();
	test_same_root();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall HostFS path checks passed");
	return EXIT_SUCCESS;
}
