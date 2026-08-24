#include <algorithm>
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

#include "config_paths.h"

#include <cstdio>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/filefn.h>

extern "C" {
#include "rpcemu.h"
#include "support_files.h"
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

	/* Seed the new machine from the "default" template so it boots with a
	 * working CMOS and HostFS straight away. The template is one of the files
	 * this build carries and extracts into the data directory, so it is asked
	 * for the same way the guest modules are - see support_files.h. Fall back to
	 * an empty HostFS if no template is present. */
	const wxString default_dir =
	    wxString::FromUTF8(support_root_for("default")) + "default";
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

/*
 * Copy a directory tree, one directory at a time.
 *
 * Everything here is a leaf name taken from the directory listing, joined to a
 * known parent. Nothing works out a relative path by trimming a prefix off an
 * absolute one, which is what went wrong before: on Windows the prefix and the
 * enumerated path did not match character for character, the trim left the
 * source's full path in place, and the copy tried to create
 * "<machine>\hostfs\C:\..." - reported as issue #39, where creating a machine
 * failed with an illegal-syntax error and no machine appeared.
 *
 * Subdirectories are walked explicitly rather than asking for a recursive file
 * list, so an empty directory in the source is a directory in the copy.
 */
static bool CopyTreeInto(const wxString &src, const wxString &dst,
                         const ConfigPathsCopyProgress &progress)
{
	const wxChar sep = wxFileName::GetPathSeparator();

	if (!wxDirExists(dst) && !wxDir::Make(dst, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}

	wxString name;

	{
		wxDir dir(src);
		if (!dir.IsOpened()) {
			return false;
		}
		bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES | wxDIR_HIDDEN);
		while (more) {
			if (!wxCopyFile(src + sep + name, dst + sep + name, true)) {
				return false;
			}
			if (progress) {
				progress(name);
			}
			more = dir.GetNext(&name);
		}
	}

	{
		wxDir dir(src);
		if (!dir.IsOpened()) {
			return false;
		}
		bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS | wxDIR_HIDDEN);
		while (more) {
			if (!CopyTreeInto(src + sep + name, dst + sep + name,
			                  progress)) {
				return false;
			}
			more = dir.GetNext(&name);
		}
	}

	return true;
}

bool ConfigPathsCopyDirectory(const wxString &src, const wxString &dst,
                              const ConfigPathsCopyProgress &progress)
{
	if (!wxDirExists(src)) {
		return false;
	}
	return CopyTreeInto(src, dst, progress);
}

/*
 * Make a copied configuration belong to the machine it has become.
 *
 * A clone used to have only its `name` rewritten, and everything else in the
 * file was written for the machine it came from. Some of that names a resource
 * only one machine may have: the same control socket, so a tool reaches
 * whichever machine bound it last; the same capture file, so two machines write
 * one pcap; the same HostFS or hard disc, so two guests write one filesystem. A
 * machine whose socket fields were empty was fine only by luck, because empty
 * already means "under my own directory".
 *
 * @param config_path  The copied configuration file, rewritten in place
 * @param new_name     The clone's name
 * @param source_dir   The machine directory the copy came from
 * @param clone_dir    The machine directory the copy is going to
 */
void ConfigPathsPrepareClonedConfig(const wxString &config_path,
                                    const wxString &new_name,
                                    const wxString &source_dir,
                                    const wxString &clone_dir)
{
	/* Cleared rather than rewritten: empty already means this machine's own
	   directory, so there is nothing better to put here. */
	static const char *const own_channel[] = {
		"hostcmd_socket", "debug_socket", "netcap_socket"
	};
	/* Things the machine writes to. Two machines appending to one file is not a
	   sensible default whoever chose the path. */
	static const char *const own_output[] = {
		"network_capture", "serial_com1_log", "serial_com2_log",
		"parallel_log", "printer_output_path"
	};
	/* Paths that may point inside the machine's own directory, which the copy
	   has just reproduced under the new name. */
	static const char *const own_path[] = {
		"hostfs_path", "hd4_path", "hd5_path", "cdrom_iso"
	};

	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path, wxEmptyString,
	                      wxCONFIG_USE_RELATIVE_PATH);

	ConfigFileUseGeneralGroup(settings);
	settings.Write("name", new_name);

	for (const char *key : own_channel) {
		if (settings.HasEntry(key)) {
			settings.Write(key, wxEmptyString);
		}
	}
	for (const char *key : own_output) {
		if (settings.HasEntry(key)) {
			settings.Write(key, wxEmptyString);
		}
	}

	/*
	 * A path inside the source machine's directory is re-pointed at the same
	 * place inside the clone's, since the files are being copied there. A path
	 * anywhere else is deliberate and left alone: somebody who pointed two
	 * machines at one disc image meant it.
	 */
	for (const char *key : own_path) {
		wxString value;

		if (!settings.Read(key, &value) || value.empty()) {
			continue;
		}
		if (!source_dir.empty() && value.StartsWith(source_dir)) {
			settings.Write(key, clone_dir + value.Mid(source_dir.length()));
		}
	}

	settings.Flush();
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

std::vector<std::string> ConfigPathsMachineNames()
{
	wxArrayString files;
	std::vector<std::string> names;

	/* The same source the Manager window and the VNC selector both list
	   from, so none of them can disagree about which machines exist. */
	wxDir::GetAllFiles(ConfigPathsConfigsDir(), &files, "*.cfg", wxDIR_FILES);
	for (const wxString &file : files) {
		const wxFileName fn(file);

		names.push_back(std::string(fn.GetName().utf8_str()));
	}
	std::sort(names.begin(), names.end());
	return names;
}
