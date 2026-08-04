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

#ifndef DATA_DIR_CHOICE_H
#define DATA_DIR_CHOICE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WHERE THE DATA DIRECTORY COMES FROM, as a decision separated from the act of
 * looking around the filesystem.
 *
 * RPCEmu used to work this out inline and silently, which is issue #67: it
 * created a folder in the user's home directory without asking and gave no way
 * to put it anywhere else short of setting an environment variable. On macOS
 * that is the wrong place by convention as well, since data of this kind belongs
 * under ~/Library/Application Support.
 *
 * The fix is to ask, once. But WHEN to ask is a precedence question with eight
 * inputs and several ways to get it embarrassingly wrong - prompting somebody
 * who has been running RPCEmu for a year, or prompting a headless container, or
 * prompting a developer whose data sits next to the binary on purpose. So the
 * decision lives here as a pure function over stated inputs, the caller does the
 * filesystem probing and the asking, and tests/test_data_dir.c can walk the
 * whole table without a filesystem or a window.
 */

/** Which input won, which is also why. */
typedef enum {
	/** --datadir on the command line. Beats everything. */
	DATA_DIR_FROM_CLI,
	/** RPCEMU_DATADIR. Above the remembered choice on purpose, so a script
	 *  or a CI job is never at the mercy of what somebody once clicked. */
	DATA_DIR_FROM_ENV,
	/** RPCEMU_RESOURCE_DIR: read-only payload there, user data in its usual
	 *  place. */
	DATA_DIR_FROM_ENV_RESOURCE,
	/** What the user chose last time, from the platform's preference store. */
	DATA_DIR_FROM_STORED,
	/** configs/ sits beside the binary: the portable download, and a
	 *  developer's build tree. Self-contained, so never second-guessed. */
	DATA_DIR_FROM_PORTABLE,
	/** configs/ sits in the current directory. Reported separately from the
	 *  one above only so the log says which, since telling them apart is the
	 *  first thing anyone asks about a wrong-folder report. */
	DATA_DIR_FROM_CWD,
	/** configs/ is in the install prefix: payload there, user data elsewhere. */
	DATA_DIR_FROM_INSTALL,
	/** A macOS .app bundle: payload in Contents/Resources, user data outside. */
	DATA_DIR_FROM_BUNDLE,
	/** The default location already exists and is set up. Somebody who has
	 *  been using RPCEmu since before it could ask, and must not be asked
	 *  now: an upgrade that interrogates you about your own data is worse
	 *  than the problem being fixed. */
	DATA_DIR_FROM_EXISTING_DEFAULT,
	/** Nothing to go on, and there is a GUI to ask with. First run. */
	DATA_DIR_ASK,
	/** Nothing to go on and no way to ask: headless, --pkg-list, a container.
	 *  Take the default silently, exactly as before. */
	DATA_DIR_DEFAULT_UNASKED
} DataDirSource;

typedef struct {
	/** --datadir, or NULL/empty if not given. */
	const char *cli_datadir;
	/** RPCEMU_DATADIR, or NULL/empty. */
	const char *env_datadir;
	/** RPCEMU_RESOURCE_DIR, or NULL/empty. */
	const char *env_resource_dir;
	/** The remembered choice, or NULL/empty if never made. */
	const char *stored_datadir;

	/** configs/ found beside the executable. */
	int configs_beside_binary;
	/** configs/ found in the current working directory. */
	int configs_in_cwd;
	/** configs/ found in the compiled-in install prefix. */
	int configs_in_install_dir;
	/** configs/ found in a macOS bundle's Contents/Resources. */
	int configs_in_bundle;
	/** The default user location exists AND looks set up (has configs/). */
	int default_location_ready;

	/** There is a GUI and an event loop, so a dialogue is possible. */
	int can_ask;
} DataDirInputs;

typedef struct {
	DataDirSource source;
	/** Put the first-run dialogue up. Only ever set for DATA_DIR_ASK. */
	int should_ask;
	/** Write the outcome to the preference store, so it is not worked out
	 *  again next time. Not set for inputs that are already explicit every
	 *  run (the command line, the environment) or for a portable layout,
	 *  which must keep following the binary rather than being pinned. */
	int should_remember;
} DataDirDecision;

/*
 * WHERE THE READ-ONLY PAYLOAD IS, which is a separate question and was got wrong.
 *
 * gfxroms, poduleroms, usbroms and netroms are all loaded from the RESOURCE
 * directory, not the data directory - see gfxcard.c, podulerom.c, usbcard.c and
 * network.c. Once the data directory became something the user could choose, the
 * resource directory was pointed at it as well, and none of that payload is
 * there: the graphics card lost its driver, networking lost EtherRPCEm, and
 * poduleroms lost hostfs,ffa, which is HostFS itself. The machine came up with
 * no HostFS at all.
 *
 * So the payload is looked for where payload actually lives, independently of
 * wherever the user keeps their machines, and only falls back to the data
 * directory when nothing can be found. Decided here, as a pure function, for the
 * same reason as the rules above: it is a precedence chain, and the way it fails
 * is silent.
 */
typedef enum {
	RESOURCE_DIR_BUNDLE,		/**< macOS .app Contents/Resources */
	RESOURCE_DIR_BESIDE_BINARY,	/**< Next to the executable */
	RESOURCE_DIR_CWD,		/**< The current directory */
	RESOURCE_DIR_INSTALL,		/**< The compiled-in install prefix */
	RESOURCE_DIR_USR_SHARE,		/**< /usr/share/rpcemu */
	RESOURCE_DIR_SAME_AS_DATA	/**< Nothing found: last resort */
} ResourceDirSource;

typedef struct {
	int payload_in_bundle;
	int payload_beside_binary;
	int payload_in_cwd;
	int payload_in_install;
	int payload_in_usr_share;
} ResourceDirInputs;

/**
 * Decide where the read-only payload is.
 *
 * "Payload" means a directory carrying the files the emulator loads rather than
 * the user's own data: `poduleroms/` or `configs/`. Checking for `configs/`
 * alone is not enough, and that is exactly how this was missed - a user who
 * moves their machines out of a portable folder leaves the payload behind, so
 * the folder still has poduleroms but no longer has configs.
 */
ResourceDirSource data_dir_resource_decide(const ResourceDirInputs *inputs);

/** Human-readable name, for the log. Never NULL. */
const char *data_dir_resource_source_name(ResourceDirSource source);

/**
 * Decide where the data directory comes from.
 *
 * Total over its inputs: every combination yields exactly one source, so there
 * is no "cannot happen" branch for a caller to trip over.
 */
DataDirDecision data_dir_decide(const DataDirInputs *inputs);

/** Human-readable name for a source, for the log. Never NULL. */
const char *data_dir_source_name(DataDirSource source);

#ifdef __cplusplus
}
#endif

#endif /* DATA_DIR_CHOICE_H */
