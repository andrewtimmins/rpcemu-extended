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
 */

/*
 * test_support_files.c - deciding which shipped files to bring over
 *
 * The decision is the whole of this feature: copy too little and somebody runs
 * a new emulator with old guest modules, which is the fault it exists to fix;
 * copy too much and somebody's own module is overwritten without being asked.
 * Both are silent, so both are tested here.
 */

#include <stdio.h>
#include <string.h>

#include "support_files.h"

static int failures;

static void
check(const char *what, int got, int want)
{
	if (got != want) {
		printf("  %-52s FAIL (got %d, wanted %d)\n", what, got, want);
		failures++;
		return;
	}
	printf("  %-52s ok\n", what);
}

/* --- the manifest ---------------------------------------------------------*/

static void
test_parse(void)
{
	static const char good[] =
	    "# RPCEmu support files\n"
	    "version 1\n"
	    "algorithm md5\n"
	    "\n"
	    "d41d8cd98f00b204e9800998ecf8427e  poduleroms/hostfs,ffa\n"
	    "0123456789abcdef0123456789abcdef  netroms/EtherRPCEm,ffa\n";
	SupportFile e[8];
	int n;

	printf("reading a manifest\n");

	n = support_manifest_parse(good, e, 8);
	check("two entries, comments and blank lines ignored", n, 2);
	check("first hash read", strcmp(e[0].hash, "d41d8cd98f00b204e9800998ecf8427e") == 0, 1);
	check("first path read", strcmp(e[0].path, "poduleroms/hostfs,ffa") == 0, 1);
	check("second path read", strcmp(e[1].path, "netroms/EtherRPCEm,ffa") == 0, 1);

	/* Without a version line it is not a manifest, and saying so matters: a
	   truncated or unrelated file must not read as "the release ships
	   nothing", which would look exactly like "nothing to do". */
	check("no version line is refused, not read as empty",
	      support_manifest_parse("abc  poduleroms/x\n", e, 8), -1);
	check("empty text is refused", support_manifest_parse("", e, 8), -1);
	check("NULL is refused", support_manifest_parse(NULL, e, 8), -1);

	/* CRLF, because the manifest is generated on three platforms and read on
	   three platforms, and one of them will get this wrong eventually. */
	n = support_manifest_parse("version 1\r\nabc  poduleroms/x,ffa\r\n", e, 8);
	check("CRLF line endings", n, 1);
	check("no carriage return left on the path",
	      (n == 1 && strcmp(e[0].path, "poduleroms/x,ffa") == 0), 1);

	/* RISC OS names carry commas and can carry spaces, so the path is the rest
	   of the line rather than the next whitespace-separated word. */
	n = support_manifest_parse("version 1\nabc  podules/Acorn SCSI,ffa\n", e, 8);
	check("a path containing a space survives",
	      (n == 1 && strcmp(e[0].path, "podules/Acorn SCSI,ffa") == 0), 1);

	/* Counting without storing, so a caller can size its array. */
	check("counts past the buffer it was given",
	      support_manifest_parse(good, e, 1), 2);
	check("counts with no buffer at all",
	      support_manifest_parse(good, NULL, 0), 2);
}

/* --- what may be written --------------------------------------------------*/

static void
test_paths(void)
{
	printf("\npaths a manifest may name\n");

	check("a managed directory", support_path_is_safe("poduleroms/x,ffa"), 1);
	check("another one", support_path_is_safe("default/cmos.ram"), 1);

	check("absolute is refused", support_path_is_safe("/etc/passwd"), 0);
	check("a drive letter is refused", support_path_is_safe("C:/x"), 0);
	check("walking up is refused", support_path_is_safe("poduleroms/../../x"), 0);
	check("a bare .. is refused", support_path_is_safe("../x"), 0);
	check("a backslash is refused", support_path_is_safe("poduleroms\\x"), 0);
	check("an empty component is refused", support_path_is_safe("poduleroms//x"), 0);
	check("an unmanaged directory is refused", support_path_is_safe("configs/rpc.cfg"), 0);
	check("machines are refused", support_path_is_safe("machines/mine/cmos.ram"), 0);
	check("the user's ROMs are refused", support_path_is_safe("roms/ROM530"), 0);
	check("a bare filename is refused", support_path_is_safe("hostfs,ffa"), 0);
	check("a directory is refused", support_path_is_safe("poduleroms/"), 0);
	check("empty is refused", support_path_is_safe(""), 0);
	check("NULL is refused", support_path_is_safe(NULL), 0);
}

/* --- the plan -------------------------------------------------------------*/

static SupportFile
entry(const char *hash, const char *path)
{
	SupportFile e;

	memset(&e, 0, sizeof(e));
	snprintf(e.hash, sizeof(e.hash), "%s", hash);
	snprintf(e.path, sizeof(e.path), "%s", path);
	return e;
}

/* Says everything is present, for the cases where existence is not the point. */
static int
all_present(const char *path, void *ctx)
{
	(void) path;
	(void) ctx;
	return 1;
}

/* Says one named file is missing. */
static int
missing_one(const char *path, void *ctx)
{
	return strcmp(path, (const char *) ctx) != 0;
}

static void
test_plan(void)
{
	SupportFile shipped[3];
	SupportFile recorded[3];
	const SupportFile *plan[3];
	int n;

	shipped[0] = entry("aaa", "poduleroms/one,ffa");
	shipped[1] = entry("bbb", "netroms/two,ffa");
	shipped[2] = entry("ccc", "gfxroms/three,ffa");

	printf("\nwhat gets copied\n");

	/* Nothing recorded: a first run, so everything comes over. */
	n = support_files_plan(shipped, 3, NULL, 0, all_present, NULL, plan, 3);
	check("first run copies everything", n, 3);

	/* Everything recorded and unchanged: the common case, and it must do
	   nothing at all - this runs at every startup. */
	recorded[0] = entry("aaa", "poduleroms/one,ffa");
	recorded[1] = entry("bbb", "netroms/two,ffa");
	recorded[2] = entry("ccc", "gfxroms/three,ffa");
	n = support_files_plan(shipped, 3, recorded, 3, all_present, NULL, plan, 3);
	check("an up-to-date data directory copies nothing", n, 0);

	/* One changed by the release. */
	recorded[1] = entry("old", "netroms/two,ffa");
	n = support_files_plan(shipped, 3, recorded, 3, all_present, NULL, plan, 3);
	check("one changed file is copied", n, 1);
	check("and it is the changed one",
	      (n == 1 && strcmp(plan[0]->path, "netroms/two,ffa") == 0), 1);

	/* A file the release has added since. */
	recorded[0] = entry("aaa", "poduleroms/one,ffa");
	recorded[1] = entry("bbb", "netroms/two,ffa");
	n = support_files_plan(shipped, 3, recorded, 2, all_present, NULL, plan, 3);
	check("a newly shipped file is copied", n, 1);
	check("and it is the new one",
	      (n == 1 && strcmp(plan[0]->path, "gfxroms/three,ffa") == 0), 1);

	/* Recorded, unchanged, but deleted from the data directory. A missing file
	   is a broken installation rather than a preference, so it comes back. */
	recorded[2] = entry("ccc", "gfxroms/three,ffa");
	n = support_files_plan(shipped, 3, recorded, 3, missing_one,
	                       (void *) "gfxroms/three,ffa", plan, 3);
	check("a deleted file is restored", n, 1);

	/* The one that protects the user. Their edit is left alone while the
	   release has not touched that file - the recorded hash still matches what
	   was shipped, whatever the bytes on disc now say, because this never
	   hashes the data directory. */
	n = support_files_plan(shipped, 3, recorded, 3, all_present, NULL, plan, 3);
	check("an edited file is left alone until the release changes it", n, 0);

	/* A manifest naming somewhere it should not: skipped, and the rest of the
	   run still happens. */
	shipped[1] = entry("bbb", "../../etc/passwd");
	n = support_files_plan(shipped, 3, NULL, 0, all_present, NULL, plan, 3);
	check("an unsafe entry is skipped, the others still copied", n, 2);

	/* Counting past the caller's array, so it can size one. */
	shipped[1] = entry("bbb", "netroms/two,ffa");
	n = support_files_plan(shipped, 3, NULL, 0, all_present, NULL, plan, 1);
	check("counts past the array it was given", n, 3);
	n = support_files_plan(shipped, 3, NULL, 0, all_present, NULL, NULL, 0);
	check("counts with no array at all", n, 3);
}

int
main(void)
{
	test_parse();
	test_paths();
	test_plan();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
