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
#include "data_dir_choice.h"
#include "gui_preferences.h"

#include <cstring>

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

/*
 * Does this directory carry the read-only payload?
 *
 * poduleroms/ as well as configs/, because those come apart. Somebody who moves
 * their machines out of a portable folder into one of their own takes configs/
 * with them and leaves the payload behind - so the folder still has poduleroms,
 * and testing only for configs/ declared it empty and sent the resource
 * directory to the new folder, where hostfs,ffa is not. That is how a machine
 * ends up with no HostFS.
 */
static bool HasPayloadDir(const wxString &base)
{
	const wxString dir = NormalizeDirPath(base);

	return wxDirExists(dir + "poduleroms") || wxDirExists(dir + "configs");
}

/* Where the payload is, independently of where the user keeps their data. */
static ResourceDirSource g_resource_source = RESOURCE_DIR_SAME_AS_DATA;

static wxString FindResourceDir(const wxString &bundle_dir, const wxString &exe_dir,
                                const wxString &cwd, const wxString &install_dir,
                                const wxString &user_dir)
{
	ResourceDirInputs inputs;

	memset(&inputs, 0, sizeof(inputs));
	inputs.payload_in_bundle = (!bundle_dir.empty() && HasPayloadDir(bundle_dir)) ? 1 : 0;
	inputs.payload_beside_binary = HasPayloadDir(exe_dir) ? 1 : 0;
	inputs.payload_in_cwd = HasPayloadDir(cwd) ? 1 : 0;
	inputs.payload_in_install = HasPayloadDir(install_dir) ? 1 : 0;
	inputs.payload_in_usr_share = wxDirExists("/usr/share/rpcemu/poduleroms") ? 1 : 0;

	const ResourceDirSource source = data_dir_resource_decide(&inputs);

	/* Recorded rather than logged here. This runs before rpcemu_set_datadir(),
	   and the FIRST rpclog() call decides where the log file lives for the whole
	   run - so logging now would put the entire log in whatever directory
	   happened to be current, which is how it ended up beside the binary instead
	   of in the data directory. */
	g_resource_source = source;

	switch (source) {
	case RESOURCE_DIR_BUNDLE:		return bundle_dir;
	case RESOURCE_DIR_BESIDE_BINARY:	return exe_dir;
	case RESOURCE_DIR_CWD:			return cwd;
	case RESOURCE_DIR_INSTALL:		return install_dir;
	case RESOURCE_DIR_USR_SHARE:		return "/usr/share/rpcemu/";
	case RESOURCE_DIR_SAME_AS_DATA:		break;
	}
	return user_dir;
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

wxString RpcemuDefaultDataDir()
{
	return UserDataRoot();
}

#ifdef __WXOSX__
wxString RpcemuMacApplicationSupportDataDir()
{
	/* What a Mac expects for data of this kind, and what issue #67 asked for.
	   Offered in the first-run dialogue rather than made the default, so that
	   nobody's existing ~/RPCEmu moves under them. */
	return NormalizeDirPath(wxStandardPaths::Get().GetUserLocalDataDir());
}
#endif

void InitRpcemuPaths(const wxString &cli_datadir, DataDirPrompt prompt)
{
	wxString resource_dir;
	wxString user_dir;

	const wxString env_datadir = EnvVar("RPCEMU_DATADIR");
	const wxString env_resource = EnvVar("RPCEMU_RESOURCE_DIR");
	const wxString exe_dir = NormalizeDirPath(wxPathOnly(wxStandardPaths::Get().GetExecutablePath()));
	const wxString cwd = NormalizeDirPath(wxGetCwd());
	const wxString install_dir = NormalizeDirPath(wxString(RPCEMU_INSTALL_DATADIR, wxConvUTF8));
	const wxString stored = wxString::FromUTF8(GetDataDir());
#ifdef __WXOSX__
	const wxString bundle_dir = NormalizeDirPath(wxStandardPaths::Get().GetResourcesDir());
#else
	const wxString bundle_dir = wxEmptyString;
#endif

	/*
	 * The decision is made from stated inputs by data_dir_decide(), so that the
	 * precedence rules - and in particular which situations do and do not put a
	 * dialogue in somebody's way - can be tested without a filesystem. All this
	 * function does is gather the facts and carry out the answer.
	 */
	/* Named, not temporaries. DataDirInputs holds borrowed pointers, so a
	   `foo.utf8_str().data()` would dangle the moment the full expression ended
	   and the decision would be made from freed memory. */
	const std::string cli_utf8 = cli_datadir.utf8_string();
	const std::string env_utf8 = env_datadir.utf8_string();
	const std::string env_res_utf8 = env_resource.utf8_string();
	const std::string stored_utf8 = stored.utf8_string();

	DataDirInputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.cli_datadir = cli_utf8.c_str();
	inputs.env_datadir = env_utf8.c_str();
	inputs.env_resource_dir = env_res_utf8.c_str();
	inputs.stored_datadir = stored_utf8.empty() ? nullptr : stored_utf8.c_str();
	inputs.configs_beside_binary = HasConfigsDir(exe_dir) ? 1 : 0;
	inputs.configs_in_cwd = HasConfigsDir(cwd) ? 1 : 0;
	inputs.configs_in_install_dir =
	    (HasConfigsDir(install_dir) || wxDirExists("/usr/share/rpcemu/configs")) ? 1 : 0;
	inputs.configs_in_bundle = (!bundle_dir.empty() && HasConfigsDir(bundle_dir)) ? 1 : 0;
	inputs.default_location_ready = HasConfigsDir(UserDataRoot()) ? 1 : 0;
	inputs.can_ask = prompt != nullptr ? 1 : 0;

	const DataDirDecision decision = data_dir_decide(&inputs);

	switch (decision.source) {
	case DATA_DIR_FROM_CLI:
		user_dir = NormalizeDirPath(cli_datadir);
		resource_dir = user_dir;
		break;

	case DATA_DIR_FROM_ENV:
		user_dir = NormalizeDirPath(env_datadir);
		resource_dir = user_dir;
		break;

	case DATA_DIR_FROM_STORED:
		/* Only the user's data was ever chosen, so the payload still has to be
		   found by looking - and looked for as PAYLOAD, not as configs/. */
		user_dir = NormalizeDirPath(stored);
		resource_dir = FindResourceDir(bundle_dir, exe_dir, cwd, install_dir, user_dir);
		break;

	case DATA_DIR_FROM_ENV_RESOURCE:
		resource_dir = NormalizeDirPath(env_resource);
		user_dir = UserDataRoot();
		break;

	case DATA_DIR_FROM_BUNDLE:
		/* Inside a .app the read-only payload lives in Contents/Resources and
		   writable data goes outside it. GetResourcesDir() falls back to the
		   executable's own directory for an unbundled binary, which is why the
		   input was guarded by HasConfigsDir(). */
		resource_dir = bundle_dir;
		user_dir = UserDataRoot();
		break;

	case DATA_DIR_FROM_PORTABLE:
		resource_dir = exe_dir;
		user_dir = exe_dir;
		break;

	case DATA_DIR_FROM_CWD:
		resource_dir = cwd;
		user_dir = cwd;
		break;

	case DATA_DIR_FROM_INSTALL:
		resource_dir = HasConfigsDir(install_dir) ? install_dir
		                                         : wxString("/usr/share/rpcemu/");
		user_dir = UserDataRoot();
		break;

	case DATA_DIR_FROM_EXISTING_DEFAULT:
		user_dir = UserDataRoot();
		resource_dir = FindResourceDir(bundle_dir, exe_dir, cwd, install_dir, user_dir);
		break;

	case DATA_DIR_ASK: {
		/* First run with a GUI. The suggestion is where it would have gone
		   anyway, so agreeing costs one click and behaves exactly as before. */
		wxString chosen;

		if (prompt(UserDataRoot(), &chosen) && !chosen.empty()) {
			user_dir = NormalizeDirPath(chosen);
		} else {
			user_dir = UserDataRoot();
		}
		resource_dir = FindResourceDir(bundle_dir, exe_dir, cwd, install_dir, user_dir);
		break;
	}

	case DATA_DIR_DEFAULT_UNASKED:
		user_dir = UserDataRoot();
		resource_dir = cwd.empty() ? "./" : cwd;
		break;
	}

	if (decision.should_remember && !user_dir.empty()) {
		SetDataDir(user_dir.utf8_string());
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

	/* After the paths are set, not before: rpclog() writes to a file under the
	   data directory, so logging the decision any earlier put the one line that
	   says WHICH directory was chosen into the log of a different one. */
	rpclog("Paths: data directory from %s: %s\n",
	       data_dir_source_name(decision.source),
	       user_dir.utf8_str().data());
	rpclog("Paths: resource directory from %s: %s\n",
	       data_dir_resource_source_name(g_resource_source),
	       resource_dir.utf8_str().data());

	if (!SeedUserDataDir(resource_dir, user_dir)) {
		wxLogWarning("RPCEmu could not fully prepare the user data directory:\n%s",
		             user_dir);
	}
}
