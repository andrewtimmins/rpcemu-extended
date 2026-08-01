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

/* See app_settings.h for what belongs here and why. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_settings.h"

#define APP_SETTINGS_FILE	"rpcemu.cfg"
#define MAX_LINE		1024

/** Join the data directory and the file name, tolerating a missing separator. */
const char *
app_settings_path(const char *datadir)
{
	static char path[1024];
	size_t len;

	if (datadir == NULL || datadir[0] == '\0') {
		datadir = "./";
	}
	len = strlen(datadir);
	if (len > 0 && (datadir[len - 1] == '/' || datadir[len - 1] == '\\')) {
		snprintf(path, sizeof(path), "%s%s", datadir, APP_SETTINGS_FILE);
	} else {
		snprintf(path, sizeof(path), "%s/%s", datadir, APP_SETTINGS_FILE);
	}
	return path;
}

/** Trim leading and trailing blanks in place, returning the first character. */
static char *
trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	end = s + strlen(s);
	while (end > s) {
		const char c = end[-1];

		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
			break;
		}
		end--;
	}
	*end = '\0';
	return s;
}

/**
 * Split a line into key and value.
 *
 * @return 1 if the line held a key, 0 if it was blank or a comment.
 */
static int
split(char *line, char **key, char **value)
{
	char *eq;

	line = trim(line);
	if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
		return 0;
	}
	eq = strchr(line, '=');
	if (eq == NULL) {
		return 0;
	}
	*eq = '\0';
	*key = trim(line);
	*value = trim(eq + 1);
	return (*key)[0] != '\0';
}

/** Copy a string into a fixed field, always terminating. */
static void
set_str(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src);
}

static int
apply(Config *cfg, const char *key, const char *value)
{
	if (strcmp(key, "vnc_enabled") == 0) {
		cfg->vnc_enabled = (atoi(value) != 0);
		return 1;
	}
	if (strcmp(key, "vnc_port") == 0) {
		const int port = atoi(value);

		/* A port outside the usable range would be rejected at bind time with a
		   less helpful message, so ignore it here and leave the previous value. */
		if (port > 0 && port < 65536) {
			cfg->vnc_port = port;
			return 1;
		}
		rpclog("app_settings: ignoring vnc_port=%s, not a usable port\n", value);
		return 0;
	}
	if (strcmp(key, "vnc_password") == 0) {
		set_str(cfg->vnc_password, sizeof(cfg->vnc_password), value);
		return 1;
	}
	if (strcmp(key, "hostcmd_enabled") == 0) {
		cfg->hostcmd_enabled = (atoi(value) != 0);
		return 1;
	}
	if (strcmp(key, "hostcmd_socket") == 0) {
		set_str(cfg->hostcmd_socket, sizeof(cfg->hostcmd_socket), value);
		return 1;
	}

	/* An unknown key is left alone rather than treated as an error: a settings
	   file written by a later version should not stop this one starting. */
	return 0;
}

int
app_settings_load(const char *datadir, Config *cfg)
{
	const char *path = app_settings_path(datadir);
	char line[MAX_LINE];
	FILE *f;
	int applied = 0;

	f = fopen(path, "r");
	if (f == NULL) {
		/* No file is the normal state of a fresh installation. */
		return (errno == ENOENT) ? 0 : -1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *key, *value;

		if (split(line, &key, &value)) {
			applied += apply(cfg, key, value);
		}
	}

	if (ferror(f)) {
		fclose(f);
		rpclog("app_settings: error reading %s\n", path);
		return -1;
	}
	fclose(f);
	return applied;
}

int
app_settings_has(const char *datadir, const char *key)
{
	const char *path = app_settings_path(datadir);
	char line[MAX_LINE];
	FILE *f;
	int found = 0;

	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *k, *v;

		if (split(line, &k, &v) && strcmp(k, key) == 0) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int
app_settings_save(const char *datadir, const Config *cfg)
{
	const char *path = app_settings_path(datadir);
	char tmp[1024];
	FILE *f;

	/* Written and renamed, so an interrupted save cannot leave a file that fails
	   to parse and takes the emulator's access channels down with it. */
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	f = fopen(tmp, "w");
	if (f == NULL) {
		rpclog("app_settings: cannot write %s: %s\n", tmp, strerror(errno));
		return -1;
	}

	fprintf(f,
	    "# RPCEmu settings that belong to the emulator rather than to a machine.\n"
	    "#\n"
	    "# These are the ways in to the running process, so they are the same\n"
	    "# whichever machine is loaded, and they apply before one is chosen. Edit\n"
	    "# by hand if you like; the format is key=value and # starts a comment.\n"
	    "#\n"
	    "# The command line overrides anything here.\n"
	    "\n"
	    "vnc_enabled=%d\n"
	    "vnc_port=%d\n"
	    "vnc_password=%s\n"
	    "hostcmd_enabled=%d\n"
	    "hostcmd_socket=%s\n",
	    cfg->vnc_enabled ? 1 : 0,
	    cfg->vnc_port,
	    cfg->vnc_password,
	    cfg->hostcmd_enabled ? 1 : 0,
	    cfg->hostcmd_socket);

	if (fflush(f) != 0 || ferror(f)) {
		fclose(f);
		remove(tmp);
		rpclog("app_settings: error writing %s\n", tmp);
		return -1;
	}
	fclose(f);

	/* Windows will not rename onto an existing file. */
	remove(path);
	if (rename(tmp, path) != 0) {
		rpclog("app_settings: cannot rename %s to %s: %s\n", tmp, path,
		    strerror(errno));
		remove(tmp);
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Command-line overrides
 * ------------------------------------------------------------------------- */

/*
 * Held separately from the Config so that "not given" is distinguishable from
 * "given as the same value the file already had". Only what was actually asked
 * for on the command line is applied.
 */
static struct {
	int vnc_port;
	int vnc_enabled;
	int relay_enabled;
	char hostcmd_socket[512];
	char debug_socket[512];
	unsigned have_vnc_port : 1;
	unsigned have_vnc_enabled : 1;
	unsigned have_hostcmd_socket : 1;
	unsigned have_debug_socket : 1;
	unsigned have_relay : 1;
} overrides;

void
app_settings_override_vnc_port(int port)
{
	overrides.vnc_port = port;
	overrides.have_vnc_port = 1;
}

void
app_settings_override_vnc_enabled(int enabled)
{
	overrides.vnc_enabled = enabled ? 1 : 0;
	overrides.have_vnc_enabled = 1;
}

void
app_settings_override_hostcmd_socket(const char *spec)
{
	set_str(overrides.hostcmd_socket, sizeof(overrides.hostcmd_socket),
	    spec ? spec : "");
	overrides.have_hostcmd_socket = 1;
}

void
app_settings_override_debug_socket(const char *spec)
{
	set_str(overrides.debug_socket, sizeof(overrides.debug_socket),
	    spec ? spec : "");
	overrides.have_debug_socket = 1;
}

void
app_settings_override_relay(int enabled)
{
	overrides.relay_enabled = enabled ? 1 : 0;
	overrides.have_relay = 1;
}

int
app_settings_relay_enabled(void)
{
	return overrides.have_relay ? overrides.relay_enabled : 1;
}

void
app_settings_apply_overrides(Config *cfg)
{
	if (overrides.have_vnc_port) {
		cfg->vnc_port = overrides.vnc_port;
	}
	if (overrides.have_vnc_enabled) {
		cfg->vnc_enabled = overrides.vnc_enabled;
	}
	if (overrides.have_hostcmd_socket) {
		set_str(cfg->hostcmd_socket, sizeof(cfg->hostcmd_socket),
		    overrides.hostcmd_socket);
	}
	if (overrides.have_debug_socket) {
		set_str(cfg->debug_socket, sizeof(cfg->debug_socket),
		    overrides.debug_socket);
	}
}
