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
 * data_dir_store.c - the one-line file recording where the data directory is.
 *
 * The reasoning, and why this is not wxConfig, is in data_dir_store.h.
 */

#include "data_dir_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define MKDIR(path) mkdir((path), 0700)
#define PATH_SEP '/'
#endif

#define STORE_LEAF "datadir"
#define STORE_PATH_MAX 1024

/** Is this a usable value rather than absent or empty? */
static int
given(const char *value)
{
	return value != NULL && value[0] != '\0';
}

/**
 * Join and write into @out, reporting whether it fitted.
 *
 * snprintf's return value is what it WOULD have written, so a path one byte too
 * long is a silent truncation unless it is checked - and a truncated path is a
 * preference written to the wrong place.
 */
static int
join(char *out, size_t len, const char *fmt, const char *a, const char *b)
{
	const int n = b != NULL ? snprintf(out, len, fmt, a, b)
	                        : snprintf(out, len, fmt, a);

	return n > 0 && (size_t) n < len;
}

int
data_dir_store_path_for(DataDirStorePlatform platform, const char *home,
                        const char *xdg_config, const char *appdata,
                        char *out, size_t len)
{
	char dir[STORE_PATH_MAX];

	if (out == NULL || len == 0) {
		return 0;
	}

	switch (platform) {
	case DATA_DIR_STORE_WINDOWS:
		/* %APPDATA% is where a Windows application's own settings belong. */
		if (!given(appdata)) {
			return 0;
		}
		if (!join(dir, sizeof(dir), "%s\\RPCEmu", appdata, NULL)) {
			return 0;
		}
		break;

	case DATA_DIR_STORE_MACOS:
		/* Where a Mac keeps preferences, which is what this is - as opposed to
		   the data itself, which is the thing being pointed at. */
		if (!given(home)) {
			return 0;
		}
		if (!join(dir, sizeof(dir), "%s/Library/Preferences/RPCEmu", home, NULL)) {
			return 0;
		}
		break;

	case DATA_DIR_STORE_UNIX:
	default:
		/* XDG first, since somebody who has set it means it. */
		if (given(xdg_config)) {
			if (!join(dir, sizeof(dir), "%s/rpcemu", xdg_config, NULL)) {
				return 0;
			}
		} else if (given(home)) {
			if (!join(dir, sizeof(dir), "%s/.config/rpcemu", home, NULL)) {
				return 0;
			}
		} else {
			return 0;
		}
		break;
	}

	{
		const char sep = platform == DATA_DIR_STORE_WINDOWS ? '\\' : '/';
		const int n = snprintf(out, len, "%s%c%s", dir, sep, STORE_LEAF);

		return n > 0 && (size_t) n < len;
	}
}

int
data_dir_store_path(char *out, size_t len)
{
#ifdef _WIN32
	const DataDirStorePlatform platform = DATA_DIR_STORE_WINDOWS;
#elif defined(__APPLE__)
	const DataDirStorePlatform platform = DATA_DIR_STORE_MACOS;
#else
	const DataDirStorePlatform platform = DATA_DIR_STORE_UNIX;
#endif

	return data_dir_store_path_for(platform, getenv("HOME"),
	    getenv("XDG_CONFIG_HOME"), getenv("APPDATA"), out, len);
}

/** Create every directory along @path's parent, ignoring ones already there. */
static int
make_parent_dirs(const char *path)
{
	char buf[STORE_PATH_MAX];
	size_t i;

	if (snprintf(buf, sizeof(buf), "%s", path) < 0 ||
	    strlen(path) >= sizeof(buf)) {
		return 0;
	}

	/* Start at 1: a leading separator is the root, not a directory to make. */
	for (i = 1; buf[i] != '\0'; i++) {
		if (buf[i] != '/' && buf[i] != '\\') {
			continue;
		}
		buf[i] = '\0';
		if (MKDIR(buf) != 0 && errno != EEXIST) {
			return 0;
		}
		buf[i] = PATH_SEP;
	}
	return 1;
}

int
data_dir_store_read(char *out, size_t len)
{
	char path[STORE_PATH_MAX];
	char line[STORE_PATH_MAX];
	FILE *f;
	size_t n;

	if (out == NULL || len == 0 || !data_dir_store_path(path, sizeof(path))) {
		return 0;
	}

	f = fopen(path, "r");
	if (f == NULL) {
		/* Missing is the normal state before anything has been chosen. */
		return 0;
	}
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return 0;
	}
	fclose(f);

	/* One line, so trim whatever ended it. Also trims trailing whitespace,
	   which a hand-edited file is likely to have. */
	n = strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
	                 line[n - 1] == ' ' || line[n - 1] == '\t')) {
		line[--n] = '\0';
	}

	if (n == 0 || n >= len) {
		return 0;
	}
	memcpy(out, line, n + 1);
	return 1;
}

int
data_dir_store_write(const char *path)
{
	char store[STORE_PATH_MAX];
	char tmp[STORE_PATH_MAX];
	FILE *f;

	if (!given(path) || !data_dir_store_path(store, sizeof(store))) {
		return 0;
	}
	if (!make_parent_dirs(store)) {
		return 0;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", store) < 0) {
		return 0;
	}

	/* Written and renamed, so an interrupted write cannot leave a half-path
	   that sends the next run somewhere that does not exist. The same reasoning
	   as app_settings_save(). */
	f = fopen(tmp, "w");
	if (f == NULL) {
		return 0;
	}
	if (fprintf(f, "%s\n", path) < 0) {
		fclose(f);
		remove(tmp);
		return 0;
	}
	if (fclose(f) != 0) {
		remove(tmp);
		return 0;
	}

	/* Windows rename() will not replace an existing file. */
	remove(store);
	if (rename(tmp, store) != 0) {
		remove(tmp);
		return 0;
	}
	return 1;
}

int
data_dir_store_clear(void)
{
	char store[STORE_PATH_MAX];

	if (!data_dir_store_path(store, sizeof(store))) {
		return 0;
	}
	if (remove(store) != 0 && errno != ENOENT) {
		return 0;
	}
	return 1;
}
