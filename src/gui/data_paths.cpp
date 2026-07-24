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

#include "data_paths.h"

#include "config_paths.h"

#include <cstdlib>
#include <string>

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

extern "C" {
#include "rpcemu.h"
}

#ifndef RPCEMU_INSTALL_DATADIR
#define RPCEMU_INSTALL_DATADIR "/usr/share/rpcemu"
#endif

static wxString NormalizeDirPath(const wxString &path)
{
	if (path.empty()) {
		return wxEmptyString;
	}

	wxFileName fn;
	fn.AssignDir(path);
	fn.Normalize(wxPATH_NORM_ALL);
	return fn.GetPathWithSep();
}

static bool HasConfigsDir(const wxString &base)
{
	return wxDirExists(NormalizeDirPath(base) + "configs");
}

/* Writable per-user data lives in a visible ~/RPCEmu folder (machines, configs,
   ROMs, hostfs, logs). */
static wxString UserDataRoot()
{
	/* wxGetHomeDir() is cross-platform: $HOME on Unix, the user profile
	   directory (%USERPROFILE%) on Windows. */
	wxString base = wxGetHomeDir();
	if (base.empty()) {
		base = ".";
	}
	return NormalizeDirPath(base + wxFileName::GetPathSeparator() + "RPCEmu");
}

/* Location used before the move to ~/RPCEmu (XDG ~/.local/share/rpcemu), kept
   only so existing data can be migrated once. */
static wxString LegacyUserDataRoot()
{
	const char *xdg = getenv("XDG_DATA_HOME");
	wxString base;
	if (xdg && xdg[0] != '\0') {
		base = wxString::FromUTF8(xdg);
	} else {
		const char *home = getenv("HOME");
		base = (home && home[0] != '\0')
		    ? (wxString::FromUTF8(home) + wxFileName::GetPathSeparator() + ".local" +
		       wxFileName::GetPathSeparator() + "share")
		    : wxString(".local/share");
	}
	return NormalizeDirPath(base + wxFileName::GetPathSeparator() + "rpcemu");
}

/* One-time move of a pre-existing ~/.local/share/rpcemu to ~/RPCEmu. Only runs
   when the new location does not yet exist, so it never clobbers user data. */
static void MigrateLegacyUserData(const wxString &user_dir)
{
	const wxString legacy = LegacyUserDataRoot();
	if (user_dir == legacy) {
		return; /* nothing to do (shouldn't happen) */
	}
	if (wxDirExists(user_dir) || !wxDirExists(legacy)) {
		return;
	}
	/* Strip trailing separators for rename(). */
	wxFileName from(user_dir), to(user_dir);
	from.AssignDir(legacy);
	to.AssignDir(user_dir);
	const wxString from_path = from.GetPath();
	const wxString to_path = to.GetPath();
	if (wxRenameFile(from_path, to_path, false)) {
		wxLogMessage("RPCEmu: migrated existing data from %s to %s", from_path, to_path);
	} else {
		wxLogWarning("RPCEmu: could not migrate %s to %s; a fresh data folder "
		             "will be created.", from_path, to_path);
	}
}

static std::string DirPathForCore(const wxString &dir)
{
	return std::string(NormalizeDirPath(dir).utf8_str().data());
}

static void SeedFileIfMissing(const wxString &src, const wxString &dst)
{
	if (!wxFileExists(src) || wxFileExists(dst)) {
		return;
	}
	wxCopyFile(src, dst, false);
}

/* Copy every file under src into dst, but only those not already present, so
 * user-added files are never clobbered on subsequent launches. */
static void SeedDirIfMissing(const wxString &src, const wxString &dst)
{
	if (!wxDirExists(src)) {
		return;
	}
	wxDir::Make(dst, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

	wxFileName src_root;
	src_root.AssignDir(src); /* treat src as a directory, not a file */
	src_root.Normalize(wxPATH_NORM_ALL);
	const wxString src_prefix = src_root.GetPathWithSep();

	wxArrayString files;
	wxDir::GetAllFiles(src, &files, wxEmptyString, wxDIR_FILES);
	for (const auto &full_src : files) {
		wxString rel = full_src;
		if (rel.StartsWith(src_prefix)) {
			rel = rel.Mid(src_prefix.Length());
		}
		const wxString full_dst = dst + wxFileName::GetPathSeparator() + rel;
		if (wxFileExists(full_dst)) {
			continue;
		}
		wxFileName(full_dst).Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
		wxCopyFile(full_src, full_dst, false);
	}
}

static bool SeedUserDataDir(const wxString &resource_dir, const wxString &user_dir)
{
	const wxString resource = NormalizeDirPath(resource_dir);
	const wxString user = NormalizeDirPath(user_dir);
	if (user.empty()) {
		return false;
	}

	if (!wxDir::Make(user, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}

	if (resource.empty() || resource == user) {
		return ConfigPathsEnsureDataLayout();
	}

	const wxString user_configs = user + "configs";
	const wxString resource_configs = resource + "configs";
	if (wxDirExists(resource_configs)) {
		if (!wxDir::Make(user_configs, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
			return false;
		}
		if (!wxFileExists(user_configs + wxFileName::GetPathSeparator() + "Default.cfg")) {
			if (!ConfigPathsCopyDirectory(resource_configs, user_configs)) {
				return false;
			}
		}
	}

	const wxString user_machine = user + "machines" + wxFileName::GetPathSeparator() + "Default";
	const wxString resource_machine = resource + "machines" + wxFileName::GetPathSeparator() + "Default";
	if (wxDirExists(resource_machine) && !wxDirExists(user_machine)) {
		if (!ConfigPathsCopyDirectory(resource_machine, user_machine)) {
			return false;
		}
	}

	const wxString user_roms = user + "roms";
	if (!wxDir::Make(user_roms, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}
	/* Seed the whole bundled roms/ directory (ROM images + roms.txt), not just the
	 * index, so a packaged install can boot out of the box. */
	SeedDirIfMissing(resource + "roms", user_roms);

	const wxString user_netroms = user + "netroms";
	const wxString resource_netroms = resource + "netroms";
	if (wxDirExists(resource_netroms)) {
		if (!wxDir::Make(user_netroms, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
			return false;
		}
		SeedFileIfMissing(resource_netroms + wxFileName::GetPathSeparator() + "EtherRPCEm,ffa",
		                  user_netroms + wxFileName::GetPathSeparator() + "EtherRPCEm,ffa");
	}

	return ConfigPathsEnsureDataLayout();
}

static wxString EnvVar(const char *name)
{
	const char *value = getenv(name);
	if (!value || value[0] == '\0') {
		return wxEmptyString;
	}
	return wxString::FromUTF8(value);
}

void InitRpcemuPaths()
{
	wxString resource_dir;
	wxString user_dir;

	const wxString env_datadir = EnvVar("RPCEMU_DATADIR");
	if (!env_datadir.empty()) {
		user_dir = NormalizeDirPath(env_datadir);
		resource_dir = user_dir;
	} else {
		const wxString env_resource = EnvVar("RPCEMU_RESOURCE_DIR");
		const wxString exe_dir = NormalizeDirPath(wxPathOnly(wxStandardPaths::Get().GetExecutablePath()));
		const wxString cwd = NormalizeDirPath(wxGetCwd());
		const wxString install_dir = NormalizeDirPath(wxString(RPCEMU_INSTALL_DATADIR, wxConvUTF8));

		if (!env_resource.empty()) {
			resource_dir = NormalizeDirPath(env_resource);
			user_dir = UserDataRoot();
#ifdef __WXOSX__
		} else if (HasConfigsDir(NormalizeDirPath(wxStandardPaths::Get().GetResourcesDir()))) {
			/* Inside a .app bundle the read-only payload lives in
			   Contents/Resources and writable data is seeded into ~/RPCEmu
			   on first run. GetResourcesDir() falls back to the executable's
			   own directory for an unbundled binary, so the HasConfigsDir()
			   guard keeps the plain-folder layout working too. */
			resource_dir = NormalizeDirPath(wxStandardPaths::Get().GetResourcesDir());
			user_dir = UserDataRoot();
#endif
		} else if (HasConfigsDir(exe_dir)) {
			resource_dir = exe_dir;
			user_dir = exe_dir;
		} else if (HasConfigsDir(cwd)) {
			resource_dir = cwd;
			user_dir = cwd;
		} else if (HasConfigsDir(install_dir)) {
			resource_dir = install_dir;
			user_dir = UserDataRoot();
		} else if (wxDirExists("/usr/share/rpcemu/configs")) {
			resource_dir = "/usr/share/rpcemu/";
			user_dir = UserDataRoot();
		} else {
			resource_dir = cwd.empty() ? "./" : cwd;
			user_dir = resource_dir;
		}
	}

	if (user_dir.empty()) {
		user_dir = UserDataRoot();
	}
	if (resource_dir.empty()) {
		resource_dir = user_dir;
	}

	/* Move a pre-existing ~/.local/share/rpcemu to ~/RPCEmu (only when the user
	   dir is the default home location and the new folder doesn't exist yet). */
	if (user_dir == UserDataRoot()) {
		MigrateLegacyUserData(user_dir);
	}

	rpcemu_set_datadir(DirPathForCore(user_dir).c_str());
	rpcemu_set_resourcedir(DirPathForCore(resource_dir).c_str());

	if (!SeedUserDataDir(resource_dir, user_dir)) {
		wxLogWarning("RPCEmu could not fully prepare the user data directory:\n%s",
		             user_dir);
	}
}
