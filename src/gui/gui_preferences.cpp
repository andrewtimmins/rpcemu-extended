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

#include "display_acceleration.h"
#include "gui_preferences.h"

#include "display_acceleration.h"

#include <algorithm>
#include <sstream>

#include <wx/config.h>

extern "C" {
#include "data_dir_store.h"
}
#include <wx/tokenzr.h>

namespace {

wxConfig *
OpenPreferences()
{
	return new wxConfig("RPCEmu", "RPCEmu");
}

std::vector<std::string>
ReadRecentList(const char *key, int max_entries)
{
	std::vector<std::string> entries;
	wxConfig *config = OpenPreferences();

	wxString value;
	if (config->Read(key, &value, wxEmptyString) && !value.empty()) {
		wxStringTokenizer tok(value, "\n", wxTOKEN_STRTOK);
		while (tok.HasMoreTokens() && static_cast<int>(entries.size()) < max_entries) {
			const wxString token = tok.GetNextToken();
			if (!token.empty()) {
				entries.emplace_back(token.utf8_string());
			}
		}
	}

	delete config;
	return entries;
}

void
WriteRecentList(const char *key, const std::vector<std::string> &entries)
{
	wxConfig *config = OpenPreferences();

	std::ostringstream joined;
	for (size_t i = 0; i < entries.size(); i++) {
		if (i > 0) {
			joined << '\n';
		}
		joined << entries[i];
	}

	config->Write(key, wxString::FromUTF8(joined.str().c_str()));
	config->Flush();

	delete config;
}

void
AddRecentEntry(const char *key, const std::string &value, int max_entries)
{
	if (value.empty()) {
		return;
	}

	std::vector<std::string> entries = ReadRecentList(key, max_entries);
	entries.erase(std::remove(entries.begin(), entries.end(), value), entries.end());
	entries.insert(entries.begin(), value);

	if (static_cast<int>(entries.size()) > max_entries) {
		entries.resize(static_cast<size_t>(max_entries));
	}

	WriteRecentList(key, entries);
}

} // namespace

/*
 * The data directory is the one preference NOT kept in wxConfig, and
 * data_dir_store.h explains why at length: it has to be readable from the
 * no-GUI entry points, which run before wxWidgets is started, and wxConfig
 * there either asserts or drags GTK in and complains about DISPLAY. These are
 * thin wrappers so callers need not care which store is which.
 */
std::string
GetDataDir()
{
	char buf[1024];

	return data_dir_store_read(buf, sizeof(buf)) ? std::string(buf)
	                                             : std::string();
}

void
SetDataDir(const std::string &path)
{
	(void) data_dir_store_write(path.c_str());
}

void
ClearDataDir()
{
	(void) data_dir_store_clear();
}

/*
 * Hardware acceleration for drawing a machine's screen, in its own window and
 * in the Manager alike.
 *
 * Stored as a plain 0/1 under "HardwareAcceleration"; absent means on, which is
 * what a preferences file written before this existed looks like.
 */
bool
GetHardwareAcceleration()
{
	wxConfig *config = OpenPreferences();
	bool enabled = true;

	config->Read("HardwareAcceleration", &enabled, true);
	delete config;
	return enabled;
}

void
SetHardwareAcceleration(bool enabled)
{
	wxConfig *config = OpenPreferences();

	config->Write("HardwareAcceleration", enabled);
	config->Flush();
	delete config;
}

/*
 * Whether the Manager opens with its toolbar and machine list hidden.
 *
 * Off when absent, so a preferences file written before this existed opens the
 * window the way it always did.
 */
bool
GetMinimalUi()
{
	wxConfig *config = OpenPreferences();
	bool minimal = false;

	config->Read("MinimalUi", &minimal, false);
	delete config;
	return minimal;
}

void
SetMinimalUi(bool minimal)
{
	wxConfig *config = OpenPreferences();

	config->Write("MinimalUi", minimal);
	config->Flush();
	delete config;
}

/* Whether the Manager looks for a newer release. On when absent. */
bool
GetCheckForUpdates()
{
	wxConfig *config = OpenPreferences();
	bool check = true;

	config->Read("CheckForUpdates", &check, true);
	delete config;
	return check;
}

void
SetCheckForUpdates(bool check)
{
	wxConfig *config = OpenPreferences();

	config->Write("CheckForUpdates", check);
	config->Flush();
	delete config;
}

/* When it last looked, as a Unix timestamp. Zero means never, which is what a
   preferences file written before this existed looks like. */
long long
GetLastUpdateCheck()
{
	wxConfig *config = OpenPreferences();
	wxString value;
	long long when = 0;

	if (config->Read("LastUpdateCheck", &value, wxEmptyString)) {
		value.ToLongLong(&when);
	}
	delete config;
	return when;
}

void
SetLastUpdateCheck(long long when)
{
	wxConfig *config = OpenPreferences();

	config->Write("LastUpdateCheck", wxString::Format("%lld", when));
	config->Flush();
	delete config;
}

/* Whether stopping a machine asks first. On when absent. */
bool
GetWarnOnStop()
{
	wxConfig *config = OpenPreferences();
	bool warn = true;

	config->Read("WarnOnStop", &warn, true);
	delete config;
	return warn;
}

void
SetWarnOnStop(bool warn)
{
	wxConfig *config = OpenPreferences();

	config->Write("WarnOnStop", warn);
	config->Flush();
	delete config;
}

/* Whether closing with machines running says so. On when absent. */
bool
GetWarnOnExit()
{
	wxConfig *config = OpenPreferences();
	bool warn = true;

	config->Read("WarnOnExit", &warn, true);
	delete config;
	return warn;
}

void
SetWarnOnExit(bool warn)
{
	wxConfig *config = OpenPreferences();

	config->Write("WarnOnExit", warn);
	config->Flush();
	delete config;
}

/* The command line's answer for this session, or none. See
   display_acceleration.h for why the precedence is spelled out separately. */
static int g_acceleration_override = DISPLAY_ACCELERATION_NO_OVERRIDE;

void
SetHardwareAccelerationOverride(int state)
{
	g_acceleration_override = state;
}

bool
HardwareAccelerationWanted()
{
	return display_acceleration_decide(g_acceleration_override,
	    GetHardwareAcceleration() ? 1 : 0) != 0;
}

std::string
GetDefaultMachine()
{
	wxConfig *config = OpenPreferences();
	wxString value;

	config->Read("DefaultMachine", &value, wxEmptyString);
	delete config;
	return value.utf8_string();
}

void
SetDefaultMachine(const std::string &machine_name)
{
	wxConfig *config = OpenPreferences();

	config->Write("DefaultMachine", wxString::FromUTF8(machine_name));
	config->Flush();
	delete config;
}

void
ClearDefaultMachine()
{
	wxConfig *config = OpenPreferences();

	config->DeleteEntry("DefaultMachine");
	config->Flush();
	delete config;
}

std::vector<std::string>
GetRecentMachines()
{
	return ReadRecentList("recentMachines", MaxRecentMachines);
}

void
AddRecentMachine(const std::string &machine_name)
{
	AddRecentEntry("recentMachines", machine_name, MaxRecentMachines);
}

void
ClearRecentMachines()
{
	WriteRecentList("recentMachines", {});
}

std::vector<std::string>
GetRecentFloppies()
{
	return ReadRecentList("recentFloppies", MaxRecentFloppies);
}

void
AddRecentFloppy(const std::string &path)
{
	AddRecentEntry("recentFloppies", path, MaxRecentFloppies);
}

void
ClearRecentFloppies()
{
	WriteRecentList("recentFloppies", {});
}

std::vector<std::string>
GetRecentCDROMs()
{
	return ReadRecentList("recentCDROMs", MaxRecentCDROMs);
}

void
AddRecentCDROM(const std::string &path)
{
	AddRecentEntry("recentCDROMs", path, MaxRecentCDROMs);
}

void
ClearRecentCDROMs()
{
	WriteRecentList("recentCDROMs", {});
}
