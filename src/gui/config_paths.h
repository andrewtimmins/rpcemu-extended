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

#ifndef CONFIG_PATHS_H
#define CONFIG_PATHS_H

#include <functional>
#include <string>
#include <vector>

#include <wx/fileconf.h>
#include <wx/string.h>

wxString ConfigPathsConfigsDir();

/**
 * Machine names, from the configs directory.
 *
 * The same list the selector dialogue shows, so the dialogue and the VNC selector
 * cannot disagree about what exists. Names only, without the .cfg.
 */
std::vector<std::string> ConfigPathsMachineNames();
wxString ConfigPathsMachinesDir();
wxString ConfigPathsRomsDir();
wxString ConfigPathsResourceDir();
bool ConfigPathsEnsureDataLayout();
wxString ConfigPathsAbsoluteConfigPath(const wxString &path);
wxString ConfigPathsSnapshotForConfig(const wxString &config_path);

wxString ConfigPathsSanitizeName(const wxString &name);
bool ConfigPathsIsNameUnique(const wxString &name);
bool ConfigPathsCreateMachineDirectory(const wxString &machine_name);
/*
 * Called as each file is copied, for a caller that wants to say so. A machine's
 * hard disc is thousands of files, and the window answers nothing while they
 * are copied. Optional: the callers with no window to update pass nothing.
 */
using ConfigPathsCopyProgress = std::function<void(const wxString &file)>;

bool ConfigPathsCopyDirectory(const wxString &src, const wxString &dst,
                              const ConfigPathsCopyProgress &progress = nullptr);
/**
 * Make a copied configuration belong to the machine it has become: clear the
 * fields naming a resource only one machine may have, and re-point any path
 * that lay inside the machine directory the copy came from.
 */
void ConfigPathsPrepareClonedConfig(const wxString &config_path,
                                    const wxString &new_name,
                                    const wxString &source_dir,
                                    const wxString &clone_dir);

wxString ConfigPathsRenameMachine(const wxString &old_name, const wxString &new_name, const wxString &config_path);

// QSettings-style machine configs store keys under [General]; wxFileConfig needs an explicit group.
void ConfigFileUseGeneralGroup(wxFileConfig &settings);

#endif
