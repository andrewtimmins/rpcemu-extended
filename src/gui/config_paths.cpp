#include "config_paths.h"

#include <cstdio>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/filefn.h>

extern "C" {
#include "rpcemu.h"
}

void ConfigFileUseGeneralGroup(wxFileConfig &settings)
{
	settings.SetPath("/General");
}

wxString ConfigPathsDataDir()
{
	return wxString::FromUTF8(rpcemu_get_datadir());
}

wxString ConfigPathsResourceDir()
{
	return wxString::FromUTF8(rpcemu_get_resourcedir());
}

bool ConfigPathsEnsureDataLayout()
{
	const wxString data_dir = ConfigPathsDataDir();
	if (data_dir.empty()) {
		return false;
	}

	const bool ok_configs = wxDir::Make(ConfigPathsConfigsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	const bool ok_machines = wxDir::Make(ConfigPathsMachinesDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	const bool ok_roms = wxDir::Make(ConfigPathsRomsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	const wxString shared_dir = data_dir + wxFileName::GetPathSeparator() + "shared";
	const bool ok_shared = wxDir::Make(shared_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	return ok_configs && ok_machines && ok_roms && ok_shared;
}

wxString ConfigPathsConfigsDir()
{
	return wxFileName(ConfigPathsDataDir(), "configs").GetFullPath();
}

wxString ConfigPathsMachinesDir()
{
	return wxFileName(ConfigPathsDataDir(), "machines").GetFullPath();
}

wxString ConfigPathsRomsDir()
{
	return wxFileName(ConfigPathsDataDir(), "roms").GetFullPath();
}

wxString ConfigPathsAbsoluteConfigPath(const wxString &path)
{
	if (path.empty()) {
		return wxEmptyString;
	}

	wxFileName fn(path);
	if (fn.IsAbsolute()) {
		return fn.GetFullPath();
	}

	return ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + fn.GetFullName();
}

wxString ConfigPathsSanitizeName(const wxString &name)
{
	wxString sanitized = name;
	sanitized.Trim(true).Trim(false);
	sanitized.Replace("<", "_");
	sanitized.Replace(">", "_");
	sanitized.Replace(":", "_");
	sanitized.Replace("\"", "_");
	sanitized.Replace("/", "_");
	sanitized.Replace("\\", "_");
	sanitized.Replace("|", "_");
	sanitized.Replace("?", "_");
	sanitized.Replace("*", "_");
	return sanitized;
}

bool ConfigPathsIsNameUnique(const wxString &name)
{
	const wxString sanitized = ConfigPathsSanitizeName(name);
	const wxString config_path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + sanitized + ".cfg";
	const wxString machine_path = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + sanitized;
	return !wxFileExists(config_path) && !wxDirExists(machine_path);
}

bool ConfigPathsCreateMachineDirectory(const wxString &machine_name)
{
	const wxChar sep = wxFileName::GetPathSeparator();
	const wxString machine_dir = ConfigPathsMachinesDir() + sep + machine_name;
	if (!wxDir::Make(machine_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}

	/* Seed the new machine from the bundled "default" template so it boots with a
	 * working CMOS and HostFS straight away. The template lives in the resource
	 * directory (the portable tree, or <install>/default for packaged builds).
	 * Fall back to an empty HostFS if no template is present. */
	const wxString default_dir = ConfigPathsResourceDir() + "default";
	const wxString default_hostfs = default_dir + sep + "hostfs";
	const wxString machine_hostfs = machine_dir + sep + "hostfs";

	bool hostfs_ok;
	if (wxDirExists(default_hostfs)) {
		hostfs_ok = ConfigPathsCopyDirectory(default_hostfs, machine_hostfs);
	} else {
		hostfs_ok = wxDir::Make(machine_hostfs, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	}
	if (!hostfs_ok) {
		return false;
	}

	const wxString default_cmos = default_dir + sep + "cmos.ram";
	const wxString machine_cmos = machine_dir + sep + "cmos.ram";
	if (wxFileExists(default_cmos) && !wxFileExists(machine_cmos)) {
		wxCopyFile(default_cmos, machine_cmos, true);
	}

	return true;
}

bool ConfigPathsCopyDirectory(const wxString &src, const wxString &dst)
{
	if (!wxDirExists(src)) {
		return false;
	}

	wxFileName src_root;
	src_root.AssignDir(src); /* treat src as a directory, not a file */
	src_root.Normalize(wxPATH_NORM_ALL);
	const wxString src_prefix = src_root.GetPathWithSep();

	if (!wxDir::Make(dst, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}

	wxArrayString files;
	wxDir::GetAllFiles(src, &files, wxEmptyString, wxDIR_FILES);

	for (const auto &full_src : files) {
		if (wxDirExists(full_src)) {
			continue;
		}

		wxString rel = full_src;
		if (rel.StartsWith(src_prefix)) {
			rel = rel.Mid(src_prefix.Length());
		}
		const wxString full_dst = dst + wxFileName::GetPathSeparator() + rel;
		wxFileName dst_parent(full_dst);
		if (!dst_parent.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
			return false;
		}
		if (!wxCopyFile(full_src, full_dst, true)) {
			return false;
		}
	}
	return true;
}

wxString ConfigPathsRenameMachine(const wxString &old_name, const wxString &new_name, const wxString &config_path)
{
	const wxString sanitized = ConfigPathsSanitizeName(new_name);
	const wxString new_config = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + sanitized + ".cfg";
	const wxString old_machine = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + old_name;
	const wxString new_machine = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + sanitized;

	if (wxDirExists(old_machine) && old_machine != new_machine) {
		std::rename(old_machine.utf8_str().data(), new_machine.utf8_str().data());
	}

	if (config_path != new_config && wxFileExists(config_path)) {
		std::rename(config_path.utf8_str().data(), new_config.utf8_str().data());
	}

	return new_config;
}

wxString ConfigPathsSnapshotForConfig(const wxString &config_path)
{
	// The machine's suspend snapshot lives in its data directory
	// (machines/<name>/suspend.state), beside its cmos.ram. The machine
	// directory is keyed by the config's "name" field (matching
	// rpcemu_set_machine_datadir), falling back to the config filename.
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path, wxEmptyString,
	                      wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);
	wxString name;
	settings.Read("name", &name, wxEmptyString);
	if (name.empty()) {
		name = wxFileName(config_path).GetName();
	}

	const wxString sep = wxFileName::GetPathSeparator();
	return ConfigPathsMachinesDir() + sep + name + sep + "suspend.state";
}
