/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

#include <string.h>

#include "rpcemu.h"

/* These are functions that can be overridden by platforms if need
   be, but currently this version is used by Linux, all the other autoconf
   based builds and Windows. Only Mac OS X GUI version needs to override */

static char datadir[512] = "./";
static char machinedir[1024] = "";
static char logpath[1024] = "";

/**
 * Return the path of the data directory containing all the sub data parts
 * used by the program, eg romload, hostfs etc.
 *
 * @return Pointer to static zero-terminated string of path
 */
const char *
rpcemu_get_datadir(void)
{
	return datadir;
}

/**
 * Set the machine-specific data directory path.
 * This should be called after loading config but before starting emulation.
 *
 * @param machine_name The name of the machine (used as folder name)
 */
void
rpcemu_set_machine_datadir(const char *machine_name)
{
	if (machine_name && machine_name[0] != '\0') {
		snprintf(machinedir, sizeof(machinedir), "%smachines/%s/",
		         rpcemu_get_datadir(), machine_name);
	} else {
		/* Fallback to Default machine */
		snprintf(machinedir, sizeof(machinedir), "%smachines/Default/",
		         rpcemu_get_datadir());
	}
	/* Reset log path so it gets regenerated with new machine dir */
	logpath[0] = '\0';
}

/**
 * Return the path of the machine-specific data directory.
 * This is where cmos.ram, hostfs, hd images, and logs are stored.
 *
 * @return Pointer to static zero-terminated string of path
 */
const char *
rpcemu_get_machine_datadir(void)
{
	/* If not set, use default machine directory */
	if (machinedir[0] == '\0') {
		rpcemu_set_machine_datadir("Default");
	}
	return machinedir;
}

/**
 * Return the full path to the RPCEmu log file.
 *
 * @return Pointer to static zero-terminated string of full path to log file
 */
const char *
rpcemu_get_log_path(void)
{
	if (logpath[0] == '\0') {
		strcpy(logpath, rpcemu_get_datadir());
		strcat(logpath, "rpclog.txt");
	}

	return logpath;
}
