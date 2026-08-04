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

#ifndef DATA_DIR_STORE_H
#define DATA_DIR_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the chosen data directory is recorded.
 *
 * A file of our own, read and written with plain C, and NOT through wxConfig.
 * That is not a preference for reinventing things, it is a requirement:
 *
 *   - The pointer to the data directory cannot live in the data directory, so it
 *     has to go somewhere in the user's config area.
 *   - It has to be readable from the no-GUI entry points - --headless,
 *     --list-machines, --pkg-*, --fetch-riscos - which all run BEFORE wxEntry()
 *     because a headless run deliberately initialises no GUI toolkit.
 *
 * wxConfig cannot do the second. Reading it before wxWidgets is started asserts
 * with "create wxApp before calling this", and starting wxWidgets just for the
 * read pulls in GTK and prints "Unable to initialize GTK+, is DISPLAY set
 * properly?" on exactly the headless server this feature has to work on. Both
 * were measured, not guessed.
 *
 * So: one plain file, one format (a single line holding a path), read the same
 * way from both paths, with nothing to drift.
 */

/** Platforms, so the path rules for all three can be tested from any one. */
typedef enum {
	DATA_DIR_STORE_UNIX,	/**< $XDG_CONFIG_HOME/rpcemu, else $HOME/.config/rpcemu */
	DATA_DIR_STORE_MACOS,	/**< $HOME/Library/Preferences */
	DATA_DIR_STORE_WINDOWS	/**< %APPDATA%\\RPCEmu */
} DataDirStorePlatform;

/**
 * Work out the file's path from the environment values that decide it.
 *
 * Pure: takes the values rather than reading the environment, so every
 * platform's rule can be checked from whichever one the tests run on.
 *
 * @param platform     Which rule to apply.
 * @param home         $HOME, or NULL/empty.
 * @param xdg_config   $XDG_CONFIG_HOME, or NULL/empty. Unix only.
 * @param appdata      %APPDATA%, or NULL/empty. Windows only.
 * @param out          Filled with the path.
 * @param len          Capacity of @out.
 * @return Non-zero on success. Zero when there is nowhere to put it (no home
 *         directory) or @out is too small, in which case @out is untouched.
 */
int data_dir_store_path_for(DataDirStorePlatform platform, const char *home,
                            const char *xdg_config, const char *appdata,
                            char *out, size_t len);

/** The path on this platform, from the real environment. */
int data_dir_store_path(char *out, size_t len);

/**
 * Read the recorded data directory.
 *
 * @return Non-zero if one was recorded and fitted in @out. Zero means "never
 *         chosen", which is what makes a first run recognisable, and is also the
 *         answer when the file is missing, empty or unreadable - all of which
 *         are the same thing as far as the caller is concerned.
 */
int data_dir_store_read(char *out, size_t len);

/** Record @path. Creates the directory it lives in. Non-zero on success. */
int data_dir_store_write(const char *path);

/** Forget the recorded directory. Non-zero on success, or if there was none. */
int data_dir_store_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DATA_DIR_STORE_H */
