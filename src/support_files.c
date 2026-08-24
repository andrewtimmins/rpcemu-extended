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
 * support_files.c - keeping the guest-side files in step with the release
 *
 * See support_files.h for what this is for. Nothing here touches the
 * filesystem: whether a file exists is asked of the caller, so the decisions
 * can be tested on their own.
 */

#include <string.h>

#include "support_files.h"

/* The directories a manifest may name. Anything else is refused, so a manifest
   cannot be talked into writing outside the files this manages - which matters
   most on the day one of these arrives over the network. */
static const char *const managed_dirs[] = {
	"poduleroms",
	"netroms",
	"gfxroms",
	"usbroms",
	"podules",
	"default",
};

int
support_path_is_safe(const char *path)
{
	size_t i;
	const char *slash;
	size_t dir_len;

	if (path == NULL || path[0] == '\0') {
		return 0;
	}

	/* Absolute, or a Windows drive or UNC path. */
	if (path[0] == '/' || path[0] == '\\') {
		return 0;
	}
	if (path[1] == ':') {
		return 0;
	}

	/* One separator only, and it is '/'. A backslash would be a separator on
	   Windows and an ordinary character elsewhere, which is exactly the kind of
	   difference that turns into a path escape on one platform and not the
	   other. */
	if (strchr(path, '\\') != NULL) {
		return 0;
	}

	/* No walking upwards, and no empty components. Checked on the whole string
	   rather than per component so that "a/../b" and "..." are both caught. */
	if (strstr(path, "..") != NULL) {
		return 0;
	}
	if (strstr(path, "//") != NULL) {
		return 0;
	}

	slash = strchr(path, '/');
	if (slash == NULL || slash == path) {
		return 0;	/* Must be inside one of the directories below */
	}
	if (slash[1] == '\0') {
		return 0;	/* A directory, not a file */
	}

	dir_len = (size_t) (slash - path);
	for (i = 0; i < sizeof(managed_dirs) / sizeof(managed_dirs[0]); i++) {
		if (strlen(managed_dirs[i]) == dir_len &&
		    strncmp(path, managed_dirs[i], dir_len) == 0)
		{
			return 1;
		}
	}

	return 0;
}

/* Copy a line's worth of text, NUL terminated, refusing anything too long
   rather than storing a truncated path that would name a different file. */
static int
copy_field(char *dest, size_t dest_size, const char *src, size_t len)
{
	if (len >= dest_size) {
		return 0;
	}
	memcpy(dest, src, len);
	dest[len] = '\0';
	return 1;
}

int
support_manifest_parse(const char *text, SupportFile *out, int max)
{
	const char *p = text;
	int count = 0;
	int seen_version = 0;

	if (text == NULL) {
		return -1;
	}

	while (*p != '\0') {
		const char *line = p;
		const char *end;
		const char *sep;
		size_t len;

		/* One line, however it ends. */
		end = strchr(line, '\n');
		if (end == NULL) {
			end = line + strlen(line);
			p = end;
		} else {
			p = end + 1;
		}
		if (end > line && end[-1] == '\r') {
			end--;
		}
		len = (size_t) (end - line);

		/* Blank, or a comment. */
		if (len == 0 || line[0] == '#') {
			continue;
		}

		/* Header lines. Only the version is required; an algorithm this build
		   does not know is not an error here, because the hashes are compared
		   as text and never recomputed - a manifest written by a later release
		   with a different algorithm still says correctly which files changed. */
		if (len > 8 && strncmp(line, "version ", 8) == 0) {
			seen_version = 1;
			continue;
		}
		if (len > 10 && strncmp(line, "algorithm ", 10) == 0) {
			continue;
		}

		/* "<hash>  <path>" - two spaces, and the path is the rest of the line
		   so that a name containing spaces survives. */
		sep = strstr(line, "  ");
		if (sep == NULL || sep >= end) {
			continue;	/* Not an entry; ignore rather than refuse the file */
		}

		if (count < max && out != NULL) {
			SupportFile *e = &out[count];
			const char *path = sep + 2;

			if (!copy_field(e->hash, sizeof(e->hash), line,
			                (size_t) (sep - line)))
			{
				continue;
			}
			if (path >= end) {
				continue;
			}
			if (!copy_field(e->path, sizeof(e->path), path,
			                (size_t) (end - path)))
			{
				continue;
			}
		}
		count++;
	}

	return seen_version ? count : -1;
}

/* The hash the data directory recorded for this path, or NULL if it recorded
   nothing about it. */
static const char *
recorded_hash(const SupportFile *recorded, int recorded_count, const char *path)
{
	int i;

	for (i = 0; i < recorded_count; i++) {
		if (strcmp(recorded[i].path, path) == 0) {
			return recorded[i].hash;
		}
	}

	return NULL;
}

int
support_files_plan(const SupportFile *shipped, int shipped_count,
                   const SupportFile *recorded, int recorded_count,
                   int (*exists)(const char *path, void *ctx), void *ctx,
                   const SupportFile **out, int max)
{
	int i;
	int count = 0;

	if (shipped == NULL) {
		return 0;
	}

	for (i = 0; i < shipped_count; i++) {
		const SupportFile *s = &shipped[i];
		const char *had;
		int copy;

		/* A manifest naming somewhere it should not is not acted on at all.
		   Skipped rather than failing the whole run: one bad line should not
		   stop the other files being brought up to date. */
		if (!support_path_is_safe(s->path)) {
			continue;
		}

		had = recorded_hash(recorded, recorded_count, s->path);

		if (had == NULL) {
			copy = 1;		/* Never seeded, or newly added to the release */
		} else if (strcmp(had, s->hash) != 0) {
			copy = 1;		/* The release changed it */
		} else if (exists != NULL && !exists(s->path, ctx)) {
			copy = 1;		/* Recorded, unchanged, and gone */
		} else {
			copy = 0;
		}

		if (copy) {
			if (count < max && out != NULL) {
				out[count] = s;
			}
			count++;
		}
	}

	return count;
}
