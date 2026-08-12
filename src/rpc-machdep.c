/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025-2026 Andy Timmins

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

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "rpcemu.h"
#include "rpcemu-win.h"

#define rpcemu_mkdir_one(path) mkdir(path, 0777)

static char datadir[512] = "./";
static char resourcedir[512] = "./";
static char machinedir[1024] = "";
static char logpath[1024] = "";

static void
normalize_dir(char *path, size_t path_size)
{
	size_t len;

	if (path_size == 0 || path[0] == '\0') {
		return;
	}

	len = strlen(path);
	if (path[len - 1] == '/') {
		return;
	}

	if (len + 1 < path_size) {
		path[len] = '/';
		path[len + 1] = '\0';
	}
}

static void
set_dir(char *dest, size_t dest_size, const char *path)
{
	if (!dest || dest_size == 0 || !path) {
		return;
	}

	snprintf(dest, dest_size, "%s", path);
	normalize_dir(dest, dest_size);
}

static void
mkdir_recursive(const char *path)
{
	char tmp[1024];
	char *p;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	while (len > 0 && tmp[len - 1] == '/') {
		tmp[len - 1] = '\0';
		len--;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			rpcemu_mkdir_one(tmp);
			*p = '/';
		}
	}
	rpcemu_mkdir_one(tmp);
}

void
rpcemu_ensure_dir(const char *path)
{
	if (path != NULL && path[0] != '\0') {
		mkdir_recursive(path);
	}
}

static void
ensure_machine_dirs(void)
{
	char hostfs_path[1024];
	char shared_path[512];

	if (machinedir[0] != '\0') {
		mkdir_recursive(machinedir);
		snprintf(hostfs_path, sizeof(hostfs_path), "%shostfs", machinedir);
		mkdir_recursive(hostfs_path);
	}

	snprintf(shared_path, sizeof(shared_path), "%sshared", rpcemu_get_datadir());
	mkdir_recursive(shared_path);
}

const char *
rpcemu_get_datadir(void)
{
	return datadir;
}

void
rpcemu_set_datadir(const char *path)
{
	set_dir(datadir, sizeof(datadir), path ? path : "./");
	machinedir[0] = '\0';
	logpath[0] = '\0';
}

const char *
rpcemu_get_resourcedir(void)
{
	return resourcedir;
}

void
rpcemu_set_resourcedir(const char *path)
{
	set_dir(resourcedir, sizeof(resourcedir), path ? path : "./");
}

void
rpcemu_machine_datadir_for(char *out, size_t size, const char *machine_name)
{
	if (out == NULL || size == 0) {
		return;
	}
	snprintf(out, size, "%smachines/%s/", rpcemu_get_datadir(),
	         (machine_name && machine_name[0] != '\0') ? machine_name : "Default");
}

void
rpcemu_set_machine_datadir(const char *machine_name)
{
	rpcemu_machine_datadir_for(machinedir, sizeof(machinedir), machine_name);
	logpath[0] = '\0';

	ensure_machine_dirs();

	/*
	 * Close the log so the next line reopens it in the machine's own directory.
	 *
	 * Clearing logpath above is not enough on its own: rpclog() opens the file
	 * on its FIRST call and keeps the handle, and the first call happens while
	 * the paths are still being worked out - before any machine is known. So
	 * without this the location is decided before there is a machine to decide
	 * it, and every process writes the data directory's log, which rpclog()
	 * opens with "wt". Several machines running at once then truncate one
	 * another's logs and interleave lines mid-write.
	 *
	 * The handful of lines written before this point stay in the data
	 * directory's log, which is where they belong: they are about finding the
	 * data directory, not about a machine.
	 */
	rpclog_close();
}

const char *
rpcemu_get_machine_datadir(void)
{
	if (machinedir[0] == '\0') {
		rpcemu_set_machine_datadir("Default");
	}
	return machinedir;
}

/*
 * Where this process writes its log.
 *
 * A machine's log belongs in that machine's own directory; the emulator-wide
 * one - before a machine has been chosen - belongs in the data directory.
 *
 * ★ This is what rpcemu_set_machine_datadir() clearing logpath was always for:
 * it is called when a machine is chosen, so the next call here picks the new
 * location up. Reading datadir here regardless defeated it, and every process
 * therefore wrote the same datadir/rpclog.txt.
 *
 * Deliberately does NOT call rpcemu_get_machine_datadir(), which would create
 * the directories for a machine called "Default" as a side effect of asking a
 * question about logging.
 */
const char *
rpcemu_get_log_path(void)
{
	if (logpath[0] == '\0') {
		const char *dir = machinedir[0] != '\0' ? machinedir
		                                        : rpcemu_get_datadir();

		snprintf(logpath, sizeof(logpath), "%srpclog.txt", dir);
	}

	return logpath;
}
