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
 * app_settings - settings that belong to the emulator, not to a machine.
 *
 * Everything configurable used to live in a machine's own file, which was fine
 * while every setting described an emulated Risc PC. The ways *into* the running
 * process are not that: the VNC server and the HostCmd socket are how a person
 * or a script reaches the emulator, no more a property of the guest than the
 * window is. Two things make that concrete:
 *
 *   - Headless mode already ignores the machine's vnc_enabled and starts the
 *     server anyway, which is a workaround for a setting in the wrong place.
 *   - A machine selector shown over VNC has to be listening *before* any machine
 *     is chosen, so it cannot read a machine's configuration at all.
 *
 * So these five keys move here, to $DATADIR/rpcemu.cfg. Deliberately not into
 * configs/, because everything there is enumerated as a machine and an app
 * settings file would appear in the selector as a machine called "rpcemu".
 *
 * Written in plain C with a small key=value parser rather than through wx, so
 * that the emulator core can read it, a unit test can exercise it without a
 * toolkit, and the file stays something a person can edit over ssh.
 *
 * Precedence, highest first:
 *
 *   1. the command line             at present only --headless, which forces the
 *                                   VNC server on for the session; per-setting
 *                                   flags such as --vnc-port are not implemented
 *   2. $DATADIR/rpcemu.cfg          this file
 *   3. configs/<machine>.cfg        legacy; read, reported, never written back
 *   4. the built-in default
 *
 * Step 3 is what stops an existing installation changing behaviour on upgrade.
 */

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include "rpcemu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Overlay the app settings file onto a Config.
 *
 * Only keys actually present in the file are applied, so a machine value that
 * came from the legacy location survives when the file does not mention it. That
 * is the whole of the migration story: the caller loads the machine file first,
 * calls this, and whatever the user has set app-wide wins.
 *
 * A missing file is not an error - it is what a fresh installation looks like.
 *
 * @param datadir Data directory, with or without a trailing separator.
 * @param cfg     Config to overlay onto.
 * @return the number of keys applied, or -1 if the file exists but could not be
 *         read, which is worth telling the user about because their settings are
 *         being silently ignored.
 */
extern int app_settings_load(const char *datadir, Config *cfg);

/**
 * Write the app settings file.
 *
 * Writes to a temporary file and renames, so an interrupted save cannot leave a
 * half-written settings file that fails to parse on the next run.
 *
 * @return 0 on success, -1 on failure.
 */
extern int app_settings_save(const char *datadir, const Config *cfg);

/**
 * Does the app settings file mention this key?
 *
 * For the GUI, so a control can say whether it is showing an app-wide value or a
 * legacy per-machine one that is about to become app-wide.
 */
extern int app_settings_has(const char *datadir, const char *key);

/*
 * Command-line overrides, which sit above the settings file.
 *
 * These exist because the settings file is per installation and these values need
 * to be per *instance*: several emulators sharing one data directory each need
 * their own VNC port and their own control sockets, or the second one to start
 * cannot bind and the third fights the second. Set them while parsing arguments,
 * before any config is loaded.
 *
 * Deliberately no --vnc-password: a password on a command line is visible to
 * anyone who can run ps. It stays in the settings file.
 */
extern void app_settings_override_vnc_port(int port);
extern void app_settings_override_vnc_enabled(int enabled);
extern void app_settings_override_hostcmd_socket(const char *spec);
extern void app_settings_override_debug_socket(const char *spec);
extern void app_settings_override_relay(int enabled);

/** Apply whatever overrides were set. Call after app_settings_load(). */
extern void app_settings_apply_overrides(Config *cfg);

/** Has the relay been turned off for this instance? */
extern int app_settings_relay_enabled(void);

/** Path of the settings file, for messages. Returns a pointer to a static buffer. */
extern const char *app_settings_path(const char *datadir);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H */
