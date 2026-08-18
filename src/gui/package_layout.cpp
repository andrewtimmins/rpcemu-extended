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
 * package_layout.cpp - see package_layout.h for why the location is not ours to
 * choose.
 */

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/tokenzr.h>

#include "package_layout.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

const char *const kPackagesLeaf = "!Packages";
const char *const kDatabaseVersion = "2";

wxString Sep()
{
	return wxString(wxFileName::GetPathSeparator());
}

/** A host path from components that are always '/' separated. */
wxString HostPath(const wxString &base, const wxString &relative)
{
	wxString rel = relative;

	rel.Replace("/", Sep());
	return relative.empty() ? base : base + Sep() + rel;
}

wxString BootResourcesRel()
{
	return "!Boot/Resources";
}

/* wxString::Trim() mutates and returns a reference, so it cannot be called on
   anything const. Trimming a copy keeps that out of the expressions below. */
wxString Trimmed(const wxString &text)
{
	wxString out = text;

	return out.Trim().Trim(false);
}

wxString ReadWholeFile(const wxString &path)
{
	/* Version and Paths are both routinely absent, and a wxFFile that fails to
	   open logs it. In the GUI that log is a dialogue box, so a database without
	   an optional file would put an error in front of the user on the way to
	   working perfectly. */
	wxLogNull quiet;
	wxFFile file(path, "rb");
	wxString text;

	if (!file.IsOpened() || !file.ReadAll(&text)) {
		return wxEmptyString;
	}

	return text;
}

/*
 * Copy what is not already there, and nothing else.
 *
 * Used to lay the template down, which has to work both on a directory that does
 * not exist yet and on one that exists but predates the template - the databases
 * our earlier versions wrote have an Info directory and a Status file and none of
 * the RISC OS side. Never overwriting means Status and Version, which are real
 * data, are safe whichever case it is.
 */
bool CopyTreeMissing(const wxString &src, const wxString &dst)
{
	wxDir dir(src);
	wxString entry;
	bool ok = true;

	if (!dir.IsOpened()) {
		return false;
	}

	if (!wxDirExists(dst) && !wxFileName::Mkdir(dst, 0755, wxPATH_MKDIR_FULL)) {
		return false;
	}

	if (dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES)) {
		do {
			const wxString to = dst + Sep() + entry;

			if (!wxFileExists(to) &&
			    !wxCopyFile(src + Sep() + entry, to, false)) {
				ok = false;
			}
		} while (dir.GetNext(&entry));
	}

	if (dir.GetFirst(&entry, wxEmptyString, wxDIR_DIRS)) {
		do {
			if (!CopyTreeMissing(src + Sep() + entry, dst + Sep() + entry)) {
				ok = false;
			}
		} while (dir.GetNext(&entry));
	}

	return ok;
}

/** Copy a whole subtree, for moving one package's Info across. */
bool CopyTree(const wxString &src, const wxString &dst)
{
	wxDir dir(src);
	wxString entry;
	bool ok = true;

	if (!dir.IsOpened()) {
		return false;
	}

	if (!wxDirExists(dst) && !wxFileName::Mkdir(dst, 0755, wxPATH_MKDIR_FULL)) {
		return false;
	}

	if (dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES)) {
		do {
			if (!wxCopyFile(src + Sep() + entry, dst + Sep() + entry, true)) {
				ok = false;
			}
		} while (dir.GetNext(&entry));
	}

	if (dir.GetFirst(&entry, wxEmptyString, wxDIR_DIRS)) {
		do {
			if (!CopyTree(src + Sep() + entry, dst + Sep() + entry)) {
				ok = false;
			}
		} while (dir.GetNext(&entry));
	}

	return ok;
}

/** Where the bundled template lives in the installation. */
wxString TemplateDir()
{
	return wxString::FromUTF8(rpcemu_get_resourcedir()) +
	    "resources" + Sep() + "ro5" + Sep() + "packages";
}

/*
 * The RISC OS system variables a Paths entry can be written against, as paths
 * relative to the disc's root.
 *
 * These are the boot sequence's own, and every one was checked against a real
 * RISC OS 5 disc rather than assumed. Packages$Dir is the caller's, because it
 * is the thing being located.
 */
std::map<wxString, wxString> SystemVariables(const wxString &packages_rel)
{
	std::map<wxString, wxString> vars;

	vars["Boot$Dir"] = "!Boot";
	vars["BootResources$Dir"] = BootResourcesRel();
	vars["System$Dir"] = BootResourcesRel() + "/!System";
	vars["Choices$Write"] = "!Boot/Choices";
	vars["Boot$ToBeLoaded"] = "!Boot/Choices/Boot/PreDesk";
	vars["Boot$ToBeTasks"] = "!Boot/Choices/Boot/Tasks";
	vars["Packages$Dir"] = packages_rel;

	return vars;
}

/*
 * Expand one Paths value into a path relative to the disc's root.
 *
 * "<Boot$Dir>.^.Apps" becomes "Apps": the components are split on '.', each
 * "<Var>" replaced by its own components, and "^" pops the one before it, which
 * is what RISC OS means by it. An unknown variable makes the whole entry
 * unusable, so it is dropped rather than half expanded into a path that happens
 * to exist.
 */
bool ExpandPathValue(const wxString &value,
                     const std::map<wxString, wxString> &vars,
                     wxString &out)
{
	wxArrayString result;
	wxStringTokenizer parts(value, ".");

	while (parts.HasMoreTokens()) {
		const wxString part = parts.GetNextToken();

		if (part == "^") {
			if (result.IsEmpty()) {
				return false;	/* above the root: nonsense */
			}
			result.RemoveAt(result.GetCount() - 1);
			continue;
		}

		if (part.StartsWith("<") && part.EndsWith(">")) {
			const wxString name = part.Mid(1, part.length() - 2);
			const auto it = vars.find(name);

			if (it == vars.end()) {
				return false;
			}

			wxStringTokenizer expanded(it->second, "/");

			while (expanded.HasMoreTokens()) {
				result.Add(expanded.GetNextToken());
			}
			continue;
		}

		if (!part.empty()) {
			result.Add(part);
		}
	}

	out = wxJoin(result, '/', '\0');
	return true;
}

/* The standard set, used when the database has no Paths file of its own. Kept
   identical to resources/ro5/packages/Paths, which is what a fresh database is
   given, so a database with the file and one without behave the same. */
const char *const kDefaultPaths =
    "Apps = <Boot$Dir>.^.Apps\n"
    "Apps.Admin.!PackMan = <Boot$Dir>.^.Apps.!PackMan\n"
    "Boot = <Boot$Dir>\n"
    "Bootloader = <Boot$Dir>.Loader\n"
    "Diversions = <Boot$Dir>.^.Diversions\n"
    "Documents = <Boot$Dir>.^.Documents\n"
    "Manuals = <Boot$Dir>.^.Manuals\n"
    "Printing = <Boot$Dir>.^.Printing\n"
    "Public = <Boot$Dir>.^.Public\n"
    "Resources = <BootResources$Dir>\n"
    "RiscPkg = <Packages$Dir>.Info.@\n"
    "Sprites = <Packages$Dir>.Sprites\n"
    "SysVars = <Packages$Dir>.SysVars\n"
    "System = <System$Dir>\n"
    "ToBeLoaded = <Boot$ToBeLoaded>\n"
    "ToBeTasks = <Boot$ToBeTasks>\n"
    "Utilities = <Boot$Dir>.^.Utilities\n";

} /* namespace */

PackagesLocation PackagesResolve(const wxString &hostfs_dir)
{
	const wxString resources = HostPath(hostfs_dir, BootResourcesRel());
	const wxString canonical = resources + Sep() + kPackagesLeaf;
	const wxString legacy = hostfs_dir + Sep() + kPackagesLeaf;
	PackagesLocation loc;

	if (wxDirExists(canonical)) {
		loc.dir = canonical;
		loc.layout = PackagesLayout::BootResources;
		loc.existed = true;
		return loc;
	}

	if (wxDirExists(resources)) {
		loc.dir = canonical;
		loc.layout = PackagesLayout::BootResources;
		loc.existed = false;
		return loc;
	}

	loc.dir = legacy;
	loc.layout = PackagesLayout::Legacy;
	loc.existed = wxDirExists(legacy);
	return loc;
}

std::map<wxString, wxString> PackagesReadStatus(const wxString &path)
{
	std::map<wxString, wxString> entries;
	const wxString text = ReadWholeFile(path);
	wxStringTokenizer lines(text, "\n");

	while (lines.HasMoreTokens()) {
		wxString line = lines.GetNextToken();

		line.Replace("\r", wxEmptyString);

		const wxString name = line.BeforeFirst('\t');

		if (!name.empty()) {
			entries[name] = line;
		}
	}

	return entries;
}

namespace {

/*
 * Merge a legacy database into the one RISC OS reads, then get the legacy one
 * out of the way.
 *
 * A package recorded in both is left as the target has it. Its record there was
 * written by whichever manager actually installed the files, and ours would be a
 * guess at a version and a file list we did not put down.
 *
 * The old directory is renamed rather than deleted. If this merge is wrong, the
 * evidence of what was in it is the only way to find out how.
 */
void MergeLegacy(const wxString &legacy, const wxString &target)
{
	const std::map<wxString, wxString> from = PackagesReadStatus(
	    legacy + Sep() + "Status");
	const std::map<wxString, wxString> to = PackagesReadStatus(
	    target + Sep() + "Status");
	int moved = 0;
	int kept = 0;

	wxString appended;

	for (const auto &entry : from) {
		const wxString info_from = legacy + Sep() + "Info" + Sep() + entry.first;
		const wxString info_to = target + Sep() + "Info" + Sep() + entry.first;

		if (to.find(entry.first) != to.end()) {
			kept++;
			continue;
		}

		if (wxDirExists(info_from) && !CopyTree(info_from, info_to)) {
			rpclog("packages: %s could not be copied out of the old database, "
			       "so it is left recorded only there\n",
			       entry.first.utf8_str().data());
			continue;
		}

		appended += entry.second + "\n";
		moved++;
	}

	/*
	 * A real PackMan writes its last Status line WITHOUT a trailing newline, so
	 * appending straight onto it glues the first migrated package to the end of
	 * PackMan's own record and loses both. Found on a real machine, having passed
	 * a test that wrote well formed files.
	 */
	if (!appended.empty()) {
		const wxString path = target + Sep() + "Status";
		wxString existing = ReadWholeFile(path);
		wxFFile status;

		if (!existing.empty() && !existing.EndsWith("\n")) {
			existing += "\n";
		}

		if (!status.Open(path, "wb") || !status.Write(existing + appended)) {
			rpclog("packages: the merged Status file could not be written, so "
			       "%d package(s) are still recorded only in the old "
			       "database\n", moved);
			return;
		}
	}

	{
		wxString retired = legacy + "-migrated";
		int n = 2;

		while (wxDirExists(retired) || wxFileExists(retired)) {
			retired = wxString::Format("%s-migrated%d", legacy, n++);
		}

		if (!wxRenameFile(legacy, retired, false)) {
			rpclog("packages: the old database could not be renamed, so it is "
			       "still at %s and will be merged again\n",
			       legacy.utf8_str().data());
			return;
		}

		rpclog("packages: %d package(s) merged into %s, %d already recorded "
		       "there, old database kept as %s\n",
		       moved, target.utf8_str().data(), kept,
		       retired.utf8_str().data());
	}
}

} /* namespace */

PackagesLocation PackagesPrepare(const wxString &hostfs_dir)
{
	PackagesLocation loc = PackagesResolve(hostfs_dir);
	const wxString legacy = hostfs_dir + Sep() + kPackagesLeaf;

	/*
	 * A database whose format we do not know is not ours to touch. Falling back
	 * to the legacy location keeps our own records somewhere we understand
	 * instead of appending lines to a file whose meaning has changed.
	 */
	if (loc.existed) {
		const wxString version = ReadWholeFile(loc.dir + Sep() + "Version");

		if (!version.empty() && Trimmed(version) != kDatabaseVersion) {
			rpclog("packages: the database at %s is version %s, which this "
			       "build does not know how to write, so it is left alone\n",
			       loc.dir.utf8_str().data(),
			       Trimmed(version).utf8_str().data());
			loc.dir = legacy;
			loc.layout = PackagesLayout::Legacy;
			loc.existed = wxDirExists(legacy);
			return loc;
		}
	}

	/*
	 * Lay the template down: the sprites, the !Boot that publishes Packages$Dir,
	 * and the standard Paths. Without it the directory is a database no RISC OS
	 * tool can find, which is the whole reason for putting it here.
	 */
	{
		const wxString source = TemplateDir();

		if (!wxDirExists(source)) {
			rpclog("packages: no template at %s, so the database is created "
			       "bare\n", source.utf8_str().data());

			if (!wxDirExists(loc.dir)) {
				wxFileName::Mkdir(loc.dir, 0755, wxPATH_MKDIR_FULL);
			}
		} else if (!CopyTreeMissing(source, loc.dir)) {
			rpclog("packages: the template could not be copied to %s in "
			       "full\n", loc.dir.utf8_str().data());
		}
	}

	/* Info is not in the template because git cannot carry an empty directory,
	   and a database without it does not look like one. */
	{
		const wxString info = loc.dir + Sep() + "Info";

		if (!wxDirExists(info)) {
			wxFileName::Mkdir(info, 0755, wxPATH_MKDIR_FULL);
		}
	}

	if (loc.layout == PackagesLayout::BootResources && wxDirExists(legacy)) {
		MergeLegacy(legacy, loc.dir);
	}

	loc.existed = true;
	return loc;
}

PackagesPathMap PackagesReadPaths(const wxString &hostfs_dir,
                                  const wxString &packages_dir)
{
	wxString packages_rel = packages_dir;
	PackagesPathMap map;

	/* Packages$Dir has to be expressed the same way as everything else: as a
	   path relative to the disc's root. */
	if (packages_rel.StartsWith(hostfs_dir)) {
		packages_rel = packages_rel.Mid(hostfs_dir.length());
	}
	packages_rel.Replace(Sep(), "/");
	while (packages_rel.StartsWith("/")) {
		packages_rel = packages_rel.Mid(1);
	}

	const std::map<wxString, wxString> vars = SystemVariables(packages_rel);
	wxString text = ReadWholeFile(packages_dir + Sep() + "Paths");

	if (text.empty()) {
		text = kDefaultPaths;
	}

	wxStringTokenizer lines(text, "\n");

	while (lines.HasMoreTokens()) {
		wxString line = lines.GetNextToken();

		line.Replace("\r", wxEmptyString);

		const wxString name = Trimmed(line.BeforeFirst('='));
		const wxString value = Trimmed(line.AfterFirst('='));
		wxString expanded;

		if (name.empty() || value.empty() || !line.Contains("=")) {
			continue;
		}

		if (ExpandPathValue(value, vars, expanded)) {
			map[name] = expanded;
		}
	}

	return map;
}

wxString PackagesApplyPaths(const PackagesPathMap &paths, const wxString &stored)
{
	wxArrayString parts = wxStringTokenize(stored, "/", wxTOKEN_STRTOK);

	/* Longest logical name first, so "Apps.Admin.!PackMan" is preferred over
	   "Apps" for a member that is inside it. */
	for (size_t take = parts.GetCount(); take > 0; take--) {
		wxArrayString head;

		for (size_t i = 0; i < take; i++) {
			head.Add(parts[i]);
		}

		const auto it = paths.find(wxJoin(head, '.', '\0'));

		if (it == paths.end()) {
			continue;
		}

		wxArrayString tail;

		for (size_t i = take; i < parts.GetCount(); i++) {
			tail.Add(parts[i]);
		}

		if (tail.IsEmpty()) {
			return it->second;
		}

		return it->second + "/" + wxJoin(tail, '/', '\0');
	}

	return stored;
}

wxString PackagesRiscosPath(const wxString &hostfs_dir, const wxString &host_path)
{
	wxString relative = host_path;
	wxArrayString out;

	if (relative.StartsWith(hostfs_dir)) {
		relative = relative.Mid(hostfs_dir.length());
	}

	/*
	 * Component by component, because the two filesystems disagree about which
	 * character is the separator: HostFS carries a RISC OS '.' inside a name as a
	 * '/' and vice versa, so a component has its two exchanged while the
	 * separator between components becomes '.'.
	 */
	for (const wxString &part : wxStringTokenize(relative, Sep(),
	                                             wxTOKEN_STRTOK)) {
		wxString component = part;

		for (size_t i = 0; i < component.length(); i++) {
			if (component[i] == '.') {
				component[i] = '/';
			} else if (component[i] == '/') {
				component[i] = '.';
			}
		}

		out.Add(component);
	}

	return "$." + wxJoin(out, '.', '\0');
}

std::set<wxString> PackagesDiscOwnedDirs(const wxString &hostfs_dir,
                                         const PackagesPathMap &logical)
{
	std::set<wxString> dirs;

	for (const auto &entry : logical) {
		wxString rel = entry.second;

		if (rel.empty()) {
			continue;
		}

		rel.Replace("/", Sep());
		dirs.insert(hostfs_dir + Sep() + rel);
	}

	return dirs;
}
