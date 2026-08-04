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

int
hostfs_path_resolve(const char *configured, const char *machine_dir,
                    char *out, size_t len)
{
	int written;

	if (out == NULL || len == 0) {
		return 0;
	}
	out[0] = '\0';

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
