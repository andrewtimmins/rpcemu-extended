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

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * hostfs_path.c - resolving a machine's HostFS root.
 *
 * The convention and the reasoning are in hostfs_path.h.
 */

#include "hostfs_path.h"

#include <stdio.h>
#include <string.h>

int
hostfs_path_is_absolute(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return 0;
	}
	/* Unix, and the UNC/rooted forms Windows accepts. */
	if (path[0] == '/' || path[0] == '\\') {
		return 1;
	}
	/* An explicit "here", which is a deliberate statement and not a leaf. */
	if (strncmp(path, "./", 2) == 0 || strncmp(path, ".\\", 2) == 0) {
		return 1;
	}
	/* A drive letter. Understood on every platform so that a configuration
	   brought over from Windows is not quietly resolved under the machine
	   directory instead. */
	if (((path[0] >= 'A' && path[0] <= 'Z') ||
	     (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
		return 1;
	}
	return 0;
}

/*
 * Does a separator need inserting after @dir?
 *
 * rpcemu_get_machine_datadir() returns a path that already ends in one, but
 * callers in tests and elsewhere may not, and joining without checking gives
 * either "dir//hostfs" or "dirhostfs" - the second of which is a different
 * directory that will be silently created.
 */
static int
needs_separator(const char *dir)
{
	size_t len;

	if (dir == NULL || dir[0] == '\0') {
		return 0;
	}
	len = strlen(dir);
	return dir[len - 1] != '/' && dir[len - 1] != '\\';
}

/*
 * Forward slashes throughout, no repeated separators, no trailing separator.
 *
 * Collapsing repeats is not only tidiness. hostfs_path_same_root() compares
 * these strings to decide whether two machines share a folder, so
 * "/data/Box//hostfs" and "/data/Box/hostfs" comparing unequal would suppress
 * the warning for two machines that really are pointed at the same place.
 *
 * A LEADING PAIR IS PRESERVED, because "//nas/share" is a UNC path and means
 * something quite different from "/nas/share".
 */
static void
tidy(char *path)
{
	size_t len;
	size_t out;
	size_t i;

	for (len = 0; path[len] != '\0'; len++) {
		if (path[len] == '\\') {
			path[len] = '/';
		}
	}

	/* Keep the first two characters as they are, so a UNC prefix survives. */
	out = len > 2 ? 2 : len;
	for (i = out; i < len; i++) {
		if (path[i] == '/' && path[out - 1] == '/') {
			continue;
		}
		path[out++] = path[i];
	}
	path[out] = '\0';
	len = out;
	/* A lone "/" is the root and must keep its slash. A drive root such as
	   "C:/" keeps it too, since "C:" alone means something different on
	   Windows: the current directory of that drive. */
	while (len > 1 && path[len - 1] == '/') {
		if (len == 3 && path[1] == ':') {
			break;
		}
		path[--len] = '\0';
	}
}

/*
 * Is this setting unusable as a path?
 *
 * ★ WHY A CONTROL CHARACTER CAN EVEN GET IN HERE. wxFileConfig escapes
 * backslashes when it writes a value and unescapes them when it reads one, so a
 * config file the emulator wrote round-trips fine. A config file edited BY HAND
 * on Windows does not, and what comes back depends on the letter that follows the
 * backslash. Measured, not guessed (see tests/test_hostfs_path.c):
 *
 *     hostfs_path=C:\temp   ->  "C:<09>emp"   \t became a tab
 *     hostfs_path=C:\new    ->  "C:<0a>ew"    \n became a newline
 *     hostfs_path=C:\run    ->  "C:<0d>un"    \r became a carriage return
 *     hostfs_path=C:\foo    ->  "C:oo"        unknown escape, BOTH characters gone
 *     hostfs_path=C:\bar    ->  "C:ar"        likewise
 *
 * The first three name directories Windows will not create, so HostFS served a
 * folder that was not there and the guest came up with a hard disc that appeared
 * empty - with nothing anywhere to connect it to the file that was edited. Those
 * are refused here, so the caller falls back to the machine's own folder and logs
 * that it did. A boot from the wrong folder with a line in the log beats a boot
 * from nowhere in silence.
 *
 * ★ THE LAST TWO CANNOT BE CAUGHT, and pretending otherwise would be worse than
 * saying so. "C:oo" is a perfectly well-formed drive-relative path; nothing here
 * can tell it from one somebody meant to type. docs/hostfs.md says so, and says
 * to use forward slashes when editing by hand, which need no escaping at all.
 *
 * Found while testing the Windows build under Wine, by writing a config by hand
 * and watching the log say C:oo.
 */
static int
has_control_char(const char *path)
{
	size_t i;

	if (path == NULL) {
		return 0;
	}
	for (i = 0; path[i] != '\0'; i++) {
		const unsigned char c = (unsigned char) path[i];

		/* Below space only. Anything at or above it may be part of a perfectly
		   good name in some encoding, and this is not the place to police that. */
		if (c < 0x20 || c == 0x7f) {
			return 1;
		}
	}
	return 0;
}

int
hostfs_path_resolve(const char *configured, const char *machine_dir,
                    char *out, size_t len)
{
	int written;

	if (out == NULL || len == 0) {
		return 0;
	}
	out[0] = '\0';

	if (has_control_char(configured)) {
		return 0;
	}

	if (configured != NULL && hostfs_path_is_absolute(configured)) {
		written = snprintf(out, len, "%s", configured);
	} else if (configured != NULL && configured[0] != '\0') {
		/* Relative: under the machine's own directory, so it travels with the
		   machine when the data folder moves. */
		written = snprintf(out, len, "%s%s%s",
		    machine_dir != NULL ? machine_dir : "",
		    needs_separator(machine_dir) ? "/" : "",
		    configured);
	} else {
		/* The default. Nothing is stored in the configuration for this case. */
		written = snprintf(out, len, "%s%shostfs",
		    machine_dir != NULL ? machine_dir : "",
		    needs_separator(machine_dir) ? "/" : "");
	}

	if (written < 0 || (size_t) written >= len) {
		/* Emptied rather than left truncated. A truncated path names a
		   different directory, and for HostFS that means the guest's files
		   going somewhere nobody chose. */
		out[0] = '\0';
		return 0;
	}

	tidy(out);
	return 1;
}

int
hostfs_path_parent(const char *path, char *out, size_t len)
{
	char tidied[1024];
	size_t i;
	size_t cut = 0;
	int found = 0;

	if (out == NULL || len == 0) {
		return 0;
	}
	out[0] = '\0';

	if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(tidied)) {
		return 0;
	}
	snprintf(tidied, sizeof(tidied), "%s", path);
	tidy(tidied);

	for (i = 0; tidied[i] != '\0'; i++) {
		if (tidied[i] == '/') {
			cut = i;
			found = 1;
		}
	}
	if (!found) {
		/* A bare leaf has no parent to speak of: "hostfs" says nothing about
		   where it sits. The caller wants a directory it can test for, and "" is
		   not one, so say so rather than answering "." and having the caller test
		   the working directory by accident. */
		return 0;
	}
	if (cut == 0) {
		/* "/foo": the parent is the root itself, and its slash is the whole of
		   it. */
		if (len < 2) {
			return 0;
		}
		out[0] = '/';
		out[1] = '\0';
		return 1;
	}
	if (cut + 1 > len) {
		return 0;
	}
	memcpy(out, tidied, cut);
	out[cut] = '\0';
	/* "C:/foo" leaves "C:", which on Windows means the current directory of that
	   drive rather than its root - not what a caller testing for a directory
	   wants. Give it the root. */
	if (cut == 2 && out[1] == ':') {
		if (len < 4) {
			out[0] = '\0';
			return 0;
		}
		out[2] = '/';
		out[3] = '\0';
	}
	return 1;
}

int
hostfs_path_same_root(const char *a, const char *b)
{
	char ta[1024];
	char tb[1024];

	if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
		return 0;
	}
	if (strlen(a) >= sizeof(ta) || strlen(b) >= sizeof(tb)) {
		return 0;
	}

	snprintf(ta, sizeof(ta), "%s", a);
	snprintf(tb, sizeof(tb), "%s", b);
	tidy(ta);
	tidy(tb);

	return strcmp(ta, tb) == 0;
}
