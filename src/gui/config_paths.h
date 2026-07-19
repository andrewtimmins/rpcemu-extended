#ifndef CONFIG_PATHS_H
#define CONFIG_PATHS_H

#include <wx/fileconf.h>
#include <wx/string.h>

wxString ConfigPathsConfigsDir();
wxString ConfigPathsMachinesDir();
wxString ConfigPathsRomsDir();
wxString ConfigPathsResourceDir();
bool ConfigPathsEnsureDataLayout();
wxString ConfigPathsAbsoluteConfigPath(const wxString &path);
wxString ConfigPathsSnapshotForConfig(const wxString &config_path);

wxString ConfigPathsSanitizeName(const wxString &name);
bool ConfigPathsIsNameUnique(const wxString &name);
bool ConfigPathsCreateMachineDirectory(const wxString &machine_name);
bool ConfigPathsCopyDirectory(const wxString &src, const wxString &dst);
wxString ConfigPathsRenameMachine(const wxString &old_name, const wxString &new_name, const wxString &config_path);

// QSettings-style machine configs store keys under [General]; wxFileConfig needs an explicit group.
void ConfigFileUseGeneralGroup(wxFileConfig &settings);

#endif
