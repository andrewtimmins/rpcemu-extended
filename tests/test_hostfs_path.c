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

int
main(void)
{
	test_default();
	test_relative();
	test_absolute();
	test_truncation_is_refused();
	test_same_root();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall HostFS path checks passed");
	return EXIT_SUCCESS;
}
