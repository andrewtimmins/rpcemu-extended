/*
  RPCEmu - An Acorn system emulator

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

#ifndef GUI_PREFERENCES_H
#define GUI_PREFERENCES_H

#include <string>
#include <vector>

static const int MaxRecentMachines = 5;
static const int MaxRecentFloppies = 10;
static const int MaxRecentCDROMs = 10;

/* The machine RPCEmu opens without asking. Empty when there is none, in which
   case the machine selector is shown as usual. Kept in the host's own
   preferences rather than in a machine's configuration file, because it is a
   statement about which machine to pick, not a setting of any one of them. */
/*
 * Where the user's data directory is.
 *
 * This one preference cannot live with the others in rpcemu.cfg, for the obvious
 * reason: rpcemu.cfg is IN the data directory. So it goes to the platform's own
 * preference store, which is where wxConfig already puts the entries below - the
 * registry on Windows, ~/Library/Preferences on macOS, a dotfile on Linux. That
 * is also what issue #67's reporter does in their own port, using Apple's
 * defaults system.
 *
 * Empty means never chosen, which is what makes a first run recognisable. See
 * data_dir_choice.h for what is then done about it.
 */
std::string GetDataDir();
void SetDataDir(const std::string &path);
void ClearDataDir();

/*
 * Whether a machine shown in the Manager is drawn through the platform's
 * accelerated display path - Direct2D on Windows, an OpenGL texture on Linux and
 * macOS. See display_acceleration.h for what the one setting means on three
 * platforms, and Settings... in the Manager, which is where the user sets it.
 *
 * A host preference rather than a per-machine or per-data-folder one: it is a
 * statement about this computer's display, and the same machines may well be run
 * on another computer whose answer differs.
 *
 * On by default. Every accelerated path falls back to the software one by itself
 * when it cannot start, so defaulting to off would only slow down the users who
 * never find the setting.
 */
bool GetHardwareAcceleration();
void SetHardwareAcceleration(bool enabled);

/* Set from the command line (--no-gl), for this session only, and beats the
   stored preference. */
void SetHardwareAccelerationOverride(int state);

/* The answer to act on: the override if there is one, the preference if not. */
bool HardwareAccelerationWanted();

/* Whether the Manager opens with its toolbar and machine list hidden. The View
   menu turns it off and on for a session without writing it back. */
bool GetMinimalUi();
void SetMinimalUi(bool minimal);

/* Whether the Manager looks for a newer release when it opens, and when it last
   did. The time is stored as a Unix timestamp, 0 meaning never. */
bool GetCheckForUpdates();
void SetCheckForUpdates(bool check);
long long GetLastUpdateCheck();
void SetLastUpdateCheck(long long when);

/* Whether stopping a machine, and closing with machines running, ask first. */
bool GetWarnOnStop();
void SetWarnOnStop(bool warn);
bool GetWarnOnExit();
void SetWarnOnExit(bool warn);

/*
 * When the Community Network's terms were accepted, as a Unix time, or 0 if
 * they never were.
 *
 * Kept here rather than per machine because it is the person who agrees, not
 * the machine: somebody who has read it once should not be asked again for
 * every machine they own. The time rather than a flag so the answer says when,
 * which is the point of recording an agreement at all.
 */
long long GetCommunityNetworkAccepted();
void SetCommunityNetworkAccepted(long long when);

std::string GetDefaultMachine();
void SetDefaultMachine(const std::string &machine_name);
void ClearDefaultMachine();

std::vector<std::string> GetRecentMachines();
void AddRecentMachine(const std::string &machine_name);
void ClearRecentMachines();

std::vector<std::string> GetRecentFloppies();
void AddRecentFloppy(const std::string &path);
void ClearRecentFloppies();

std::vector<std::string> GetRecentCDROMs();
void AddRecentCDROM(const std::string &path);
void ClearRecentCDROMs();


/*
 * Whether a machine's screen is drawn through the platform's accelerated path
 * (an OpenGL texture) rather than rescaled on the CPU for every frame.
 *
 * On by default. The accelerated path falls back to the software one by itself
 * when it cannot start, so defaulting to off would only slow down the users who
 * never find the setting.
 */
bool GetHardwareAcceleration();
void SetHardwareAcceleration(bool enabled);

/* Set from the command line (--no-gl), for this session only, and beats the
   stored preference. */
void SetHardwareAccelerationOverride(int state);

/* The answer to act on: the override if there is one, the preference if not. */
bool HardwareAccelerationWanted();

#endif
