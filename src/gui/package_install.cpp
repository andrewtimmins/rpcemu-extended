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
 * package_install.cpp - installing and removing RISC OS packages.
 *
 * See package_install.h for the database's shape and why it is kept in the
 * project's own format.
 *
 * Members are extracted one at a time rather than through unzip_extract_all(),
 * for two reasons: the RiscPkg metadata directory has to be diverted into the
 * database instead of onto the disc, and the manifest wants writing as the
 * files go down rather than reconstructed afterwards.
 */

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/tokenzr.h>

#include "http_transfer.h"
#include "package_install.h"

extern "C" {
#include "md5.h"
#include "rpcemu.h"
#include "unzip.h"
}

namespace {

/* The database, on the machine's disc. */
const char *const kPackagesDirLeaf = "!Packages";
const char *const kDatabaseVersion = "2";

/* Metadata inside a package, which belongs in the database rather than on the
   disc alongside the program. */
const char *const kMetadataDir = "RiscPkg";

wxString Sep()
{
	return wxString(wxFileName::GetPathSeparator());
}

wxString PackagesDir(const wxString &hostfs_dir)
{
	return hostfs_dir + Sep() + kPackagesDirLeaf;
}

wxString InfoDir(const wxString &hostfs_dir, const wxString &package_name)
{
	return PackagesDir(hostfs_dir) + Sep() + "Info" + Sep() + package_name;
}

wxString StatusPath(const wxString &hostfs_dir)
{
	return PackagesDir(hostfs_dir) + Sep() + "Status";
}

bool WriteWholeFile(const wxString &path, const wxString &text)
{
	wxFFile file;

	if (!file.Open(path, "wb")) {
		return false;
	}
	/* Latin-1: RISC OS text is a byte per character, and the control records
	   these hold came in that way. */
	return file.Write(text, wxConvISO8859_1) && file.Close();
}

bool ReadWholeFile(const wxString &path, wxString &text)
{
	wxFFile file;

	return wxFileExists(path) && file.Open(path, "rb") &&
	       file.ReadAll(&text, wxConvISO8859_1);
}

/**
 * Exchange '.' and '/'.
 *
 * RISC OS uses '.' as its directory separator, so a file whose name contains a
 * dot on another system carries a '/' there instead, and archives swap the two
 * when they store it. The swap is its own inverse, which is what makes a path
 * survive the round trip: "Help/HTML/help.css" as stored is
 * "Help.HTML.help/css" to RISC OS, and back again.
 *
 * Treating every dot as a separator, as this first did, turns "help.css" into a
 * directory called "help" holding a file called "css", and then removal cannot
 * find what it installed.
 */
wxString SwapDotsAndSlashes(const wxString &path)
{
	wxString out = path;

	for (size_t i = 0; i < out.length(); i++) {
		if (out[i] == '.') {
			out[i] = '/';
		} else if (out[i] == '/') {
			out[i] = '.';
		}
	}

	return out;
}

/*
 * A member's path on the host, and the same path as RISC OS names it.
 *
 * Members are stored '/' separated, which is the separator HostFS uses for a
 * RISC OS '.', so the host path is the member name with the platform's separator
 * and the filetype suffix HostFS wants. The manifest records the RISC OS form,
 * dot separated, because that is what the database is read by on the guest.
 */
bool MemberPaths(const UnzipEntry *entry, const wxString &dest_dir,
                 wxString &host_path, wxString &riscos_path)
{
	const wxString stored = wxString::FromUTF8(entry->name);
	wxString dir_part = stored.BeforeLast('/');
	const wxString leaf = stored.AfterLast('/');
	char suffixed[UNZIP_MAX_NAME + 32];

	if (!unzip_hostfs_leafname(entry, leaf.utf8_str(), suffixed,
	        sizeof(suffixed))) {
		return false;
	}

	wxString rel = dir_part.empty() ? wxString()
	    : dir_part + "/";

	rel += wxString::FromUTF8(suffixed);
	rel.Replace("/", Sep());
	host_path = dest_dir + Sep() + rel;

	/* The RISC OS form is the stored name with the two separators exchanged;
	   the ",xxx" is HostFS's way of carrying a filetype on a host filesystem
	   and is not part of the RISC OS name. */
	riscos_path = SwapDotsAndSlashes(stored);

	return true;
}

/*
 * Our own record of exactly which host files a package wrote.
 *
 * Files, the standard manifest, holds RISC OS paths, which is what the format
 * requires and what makes the database readable by other package tools. But a
 * RISC OS path does not say what HostFS called the file: the ",xxx" filetype
 * suffix is added on the way down. Deriving it back by globbing "leaf*" is how
 * removal came to delete a neighbour that merely shared a prefix.
 *
 * So the exact host-relative paths are kept beside it, in a file of our own that
 * other tools have no reason to read. Removal uses these and unlinks precisely
 * what was written.
 */
const char *const kHostManifest = "HostFiles";

/*
 * A package's triggers, kept in the database so removal can run them too - by
 * then the zip is long gone.
 *
 * Stored under the machine's own !Packages, which means they are on the guest's
 * disc and can be run there. The path RISC OS sees is
 * "$.!Packages.Info.<Package>.Triggers".
 */
const char *const kTriggersDir = "Triggers";

/* The four points the policy manual defines. */
const char *const kTriggerPreInstall = "PreInstall";
const char *const kTriggerPostInstall = "PostInstall";
const char *const kTriggerPreRemove = "PreRemove";
const char *const kTriggerPostRemove = "PostRemove";

/* Everything a package installed, from the database. */
std::vector<wxString> ReadManifestFile(const wxString &hostfs_dir,
                                       const wxString &package_name,
                                       const wxString &leaf)
{
	std::vector<wxString> files;
	wxString text;

	if (!ReadWholeFile(InfoDir(hostfs_dir, package_name) + Sep() + leaf,
	        text)) {
		return files;
	}

	wxStringTokenizer lines(text, "\n", wxTOKEN_STRTOK);

	while (lines.HasMoreTokens()) {
		wxString line = lines.GetNextToken();

		line.Trim().Trim(false);
		if (!line.empty()) {
			files.push_back(line);
		}
	}

	return files;
}

std::vector<wxString> ReadManifest(const wxString &hostfs_dir,
                                   const wxString &package_name)
{
	return ReadManifestFile(hostfs_dir, package_name, "Files");
}

/*
 * Status is one tab-separated line per package: name, version, state, then two
 * further fields RiscPkg writes. Only the first three are interpreted here; the
 * rest of a line is preserved when rewriting so nothing another manager put
 * there is lost.
 */
std::map<wxString, wxString> ReadStatusLines(const wxString &hostfs_dir)
{
	std::map<wxString, wxString> lines;	/* package -> whole line */
	wxString text;

	if (!ReadWholeFile(StatusPath(hostfs_dir), text)) {
		return lines;
	}

	wxStringTokenizer tok(text, "\n", wxTOKEN_STRTOK);

	while (tok.HasMoreTokens()) {
		const wxString line = tok.GetNextToken();
		const wxString name = line.BeforeFirst('\t');

		if (!name.empty()) {
			lines[name] = line;
		}
	}

	return lines;
}

bool WriteStatusLines(const wxString &hostfs_dir,
                      const std::map<wxString, wxString> &lines)
{
	wxString text;

	for (const auto &entry : lines) {
		text += entry.second + "\n";
	}

	return WriteWholeFile(StatusPath(hostfs_dir), text);
}

/** Create the database if the machine has not got one yet. */
bool EnsureDatabase(const wxString &hostfs_dir)
{
	const wxString dir = PackagesDir(hostfs_dir);

	if (!wxDirExists(dir) &&
	    !wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
		return false;
	}
	if (!wxDirExists(dir + Sep() + "Info") &&
	    !wxFileName::Mkdir(dir + Sep() + "Info", 0755, wxPATH_MKDIR_FULL)) {
		return false;
	}
	if (!wxFileExists(dir + Sep() + "Version")) {
		WriteWholeFile(dir + Sep() + "Version", kDatabaseVersion);
	}
	if (!wxFileExists(StatusPath(hostfs_dir))) {
		WriteWholeFile(StatusPath(hostfs_dir), wxEmptyString);
	}

	return true;
}

/** Read a member into a string, for the metadata files. */
bool MemberToString(UnzipArchive *archive, const UnzipEntry *entry,
                    wxString &out)
{
	std::vector<uint8_t> buffer(entry->uncompressed_size + 1);

	if (entry->uncompressed_size == 0) {
		out.clear();
		return true;
	}
	if (unzip_extract_to_memory(archive, entry, buffer.data(),
	        buffer.size() - 1) != UNZIP_OK) {
		return false;
	}
	buffer[entry->uncompressed_size] = '\0';
	out = wxString(reinterpret_cast<const char *>(buffer.data()),
	               wxConvISO8859_1);

	return true;
}

/*
 * The host path of a file RISC OS calls @leaf, in @dir.
 *
 * HostFS adds a ",xxx" filetype suffix, so a trigger RISC OS knows as
 * "PreRemove" is "PreRemove,feb" on the host. Matched on the part before the
 * comma rather than as a prefix, so "PreRemoveOld" is not mistaken for it.
 * Returns an empty string if there is no such file.
 */
wxString FindHostFile(const wxString &dir, const wxString &leaf)
{
	if (wxFileExists(dir + Sep() + leaf)) {
		return dir + Sep() + leaf;	/* no suffix, e.g. type &FFF */
	}
	if (!wxDirExists(dir)) {
		return wxEmptyString;
	}

	wxDir handle(dir);
	wxString entry;
	bool more = handle.GetFirst(&entry, wxEmptyString, wxDIR_FILES);

	while (more) {
		if (entry.BeforeFirst(',') == leaf) {
			return dir + Sep() + entry;
		}
		more = handle.GetNext(&entry);
	}

	return wxEmptyString;
}

/*
 * Run one of a package's triggers, if it has that one.
 *
 * Information reaches a trigger through PkgTrigger$ variables and comes back the
 * same way: it must set PkgTrigger$ReturnCode to 0 or 1, and may set
 * PkgTrigger$ReturnText to say why it failed. The policy manual asks for a
 * background TaskWindow with a 256K WimpSlot, which is what TaskWindow's -wimpslot
 * and -quit give.
 *
 * @param action   What is happening: install, remove, upgrade, or one of the
 *                 abort- forms when unwinding a failure.
 * @param failure  Set to what went wrong, empty if the trigger passed or was
 *                 absent.
 * @return false only if the trigger ran and failed, or could not be run at all.
 *         A package with no such trigger is a pass.
 */
bool RunTrigger(PackageTriggerRunner *triggers, const wxString &hostfs_dir,
                const wxString &package_name, const char *which,
                const wxString &action, const wxString &old_version,
                const wxString &new_version, wxString &failure)
{
	failure.clear();
	if (triggers == nullptr) {
		return true;	/* nobody to run them; not the package's fault */
	}

	const wxString host_path = FindHostFile(
	    InfoDir(hostfs_dir, package_name) + Sep() + kTriggersDir,
	    wxString::FromUTF8(which));

	if (host_path.empty()) {
		return true;	/* most packages have none */
	}

	/* The same place, as RISC OS names it. */
	const wxString riscos_dir = wxString::Format(
	    "$.!Packages.Info.%s.%s", package_name, kTriggersDir);

	/*
	 * Set the variables the trigger reads.
	 *
	 * One command each: OS_CLI runs a single command line, so the "|M" that
	 * separates commands inside an Obey file does not work here - batching them
	 * that way set the first variable to the text of all the others, which cost
	 * an hour to see.
	 */
	{
		const wxString settings[] = {
			wxString::Format("Set PkgTrigger$Action %s", action),
			wxString::Format("Set PkgTrigger$Abort %s",
			    action.StartsWith("abort-") ? "1" : "0"),
			wxString::Format("Set PkgTrigger$OldVersion %s",
			    old_version.empty() ? wxString("\"\"") : old_version),
			wxString::Format("Set PkgTrigger$NewVersion %s",
			    new_version.empty() ? wxString("\"\"") : new_version),
			wxString::Format("Set PkgTrigger$Dir %s", riscos_dir),
			/* Starts at 1 so a trigger that dies without setting it counts as
			   a failure rather than passing by omission. */
			wxString("Set PkgTrigger$ReturnCode 1"),
			wxString("Unset PkgTrigger$ReturnText"),
		};
		wxString output;

		for (const auto &setting : settings) {
			if (!triggers->Run(setting, output)) {
				failure = wxString::Format(
				    "%s's %s trigger could not be set up on this machine.",
				    package_name, wxString::FromUTF8(which));
				return false;
			}
		}
	}

	wxString output;

	/* ReturnCode starts at 1, so a trigger that dies without setting it counts as
	   a failure rather than passing by omission. */
	if (!triggers->Run(wxString::Format(
	        "TaskWindow \"%s.%s\" -wimpslot 256K -quit -name \"Package trigger\"",
	        riscos_dir, wxString::FromUTF8(which)), output)) {
		failure = wxString::Format("%s's %s trigger could not be run.",
		                           package_name, wxString::FromUTF8(which));
		return false;
	}

	{
		wxString code;

		if (!triggers->Run("Show PkgTrigger$ReturnCode", code)) {
			failure = wxString::Format(
			    "%s's %s trigger gave no answer.", package_name,
			    wxString::FromUTF8(which));
			return false;
		}

		/* "PkgTrigger$ReturnCode : 0" */
		const wxString value = code.AfterLast(':').Trim().Trim(false);

		if (value != "0") {
			wxString text;

			triggers->Run("Show PkgTrigger$ReturnText", text);
			text = text.AfterFirst(':').Trim().Trim(false);
			failure = wxString::Format("%s's %s trigger failed%s",
			    package_name, wxString::FromUTF8(which),
			    text.empty() ? wxString(".")
			                 : wxString::Format(": %s", text));
			return false;
		}
	}

	rpclog("packages: %s ran %s\n",
	       static_cast<const char *>(package_name.utf8_str()), which);
	return true;
}

/* Remove a directory if it is empty, and its parents likewise, stopping at the
   machine's disc. Leaves anything that still holds a file. */
void PruneEmptyDirs(wxString dir, const wxString &stop_at)
{
	while (dir.length() > stop_at.length() && dir.StartsWith(stop_at)) {
		if (!wxDirExists(dir)) {
			dir = dir.BeforeLast(wxFileName::GetPathSeparator());
			continue;
		}

		wxDir handle(dir);
		wxString name;

		if (!handle.IsOpened() ||
		    handle.GetFirst(&name, wxEmptyString, wxDIR_FILES | wxDIR_DIRS)) {
			return;		/* not empty */
		}

		handle.Close();
		if (!wxRmdir(dir)) {
			return;
		}
		dir = dir.BeforeLast(wxFileName::GetPathSeparator());
	}
}

}  /* namespace */

PackageInstalledMap PackageInstalledList(const wxString &hostfs_dir)
{
	PackageInstalledMap installed;

	for (const auto &entry : ReadStatusLines(hostfs_dir)) {
		wxStringTokenizer fields(entry.second, "\t", wxTOKEN_RET_EMPTY_ALL);
		const wxString name = fields.HasMoreTokens() ? fields.GetNextToken()
		                                            : wxString();
		const wxString version = fields.HasMoreTokens() ? fields.GetNextToken()
		                                               : wxString();
		const wxString state = fields.HasMoreTokens() ? fields.GetNextToken()
		                                             : wxString();

		/* "installed" is the state that means the files are on the disc.
		   A half-installed or removed record is not something to report as
		   installed. */
		if (!name.empty() && state.Lower() == "installed") {
			installed[name] = version;
		}
	}

	return installed;
}

PackageActionResult PackageInstall(const PackageRecord &record,
                                   const wxString &hostfs_dir,
                                   RiscosFetchReporter &reporter,
                                   const RiscosFetchLoopFactory &make_loop,
                                   PackageTriggerRunner *triggers)
{
	PackageActionResult result;
	const wxString temp = wxFileName::CreateTempFileName("rpcemu-pkg");
	UnzipArchive archive;
	std::vector<wxString> manifest;		/* RISC OS paths, the standard form */
	std::vector<wxString> host_written;	/* exactly what was written */
	wxString control, copyright;
	bool have_triggers = false;

	if (record.url.empty()) {
		result.message = wxString::Format(
		    "%s has no download address in the catalogue.", record.name);
		return result;
	}
	if (!wxDirExists(hostfs_dir)) {
		result.message = "This machine has no hard disc to install onto.";
		return result;
	}

	/* --- fetch --- */
	{
		Transfer transfer(reporter,
		    wxString::Format("Fetching %s %s", record.name, record.version),
		    make_loop);

		if (!transfer.ToFile(record.url, temp)) {
			wxRemoveFile(temp);
			if (transfer.WasCancelled()) {
				result.cancelled = true;
				return result;
			}
			/* A dead entry in an index looks exactly like this, and there
			   is at least one live example, so say which address failed
			   rather than only that something did. */
			result.message = wxString::Format(
			    "%s could not be downloaded from %s: %s",
			    record.name, record.url, transfer.Error());
			return result;
		}
	}

	/* --- check it is what the index promised --- */
	if (!record.md5.empty()) {
		char hex[33] = "";

		reporter.Stage("Checking the download");
		if (md5_file_hex(temp.utf8_str(), hex) != 0) {
			wxRemoveFile(temp);
			result.message = "The downloaded package could not be read.";
			return result;
		}
		if (record.md5.Lower() != wxString::FromUTF8(hex)) {
			wxRemoveFile(temp);
			result.message = wxString::Format(
			    "%s does not match the checksum its index gives, so it has "
			    "not been installed. The download may be damaged, or the "
			    "index out of date.", record.name);
			return result;
		}
	}

	if (!EnsureDatabase(hostfs_dir)) {
		wxRemoveFile(temp);
		result.message = "The package database could not be created on this "
		                 "machine's disc.";
		return result;
	}

	/*
	 * A reinstall or upgrade of something already here runs that copy's
	 * PreRemove first, and its triggers are already on the disc from last time.
	 */
	{
		const PackageInstalledMap already = PackageInstalledList(hostfs_dir);
		const auto it = already.find(record.name);

		if (it != already.end()) {
			wxString failure;

			if (!RunTrigger(triggers, hostfs_dir, record.name,
			        kTriggerPreRemove, "upgrade", it->second, record.version,
			        failure)) {
				wxRemoveFile(temp);
				result.message = failure + "\n\nNothing has been changed.";
				return result;
			}
		}
	}

	/* --- unpack --- */
	if (unzip_open(&archive, temp.utf8_str()) != UNZIP_OK) {
		wxRemoveFile(temp);
		result.message = wxString::Format(
		    "%s is not a readable zip archive.", record.name);
		return result;
	}

	reporter.Stage(wxString::Format("Installing %s", record.name));

	const int count = unzip_entry_count(&archive);

	/*
	 * Triggers first, in a pass of their own.
	 *
	 * PreInstall has to run before any of the package's files are written, and it
	 * lives inside the zip, so it must be on the guest's disc before the main
	 * pass starts. Cheap: a package has at most a handful of these and most have
	 * none.
	 */
	for (int i = 0; i < count; i++) {
		const UnzipEntry *entry = unzip_entry(&archive, i);
		const wxString stored = wxString::FromUTF8(entry->name);
		const wxString prefix = wxString(kMetadataDir) + "/" + kTriggersDir + "/";

		if (entry->is_directory || !stored.StartsWith(prefix)) {
			continue;
		}

		const wxString dir = InfoDir(hostfs_dir, record.name) + Sep() +
		    kTriggersDir;
		char suffixed[UNZIP_MAX_NAME + 32];

		if (!wxDirExists(dir) &&
		    !wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
			continue;
		}
		if (unzip_hostfs_leafname(entry, stored.AfterLast('/').utf8_str(),
		        suffixed, sizeof(suffixed)) &&
		    unzip_extract_to_file(&archive, entry,
		        (dir + Sep() + wxString::FromUTF8(suffixed)).utf8_str()) ==
		        UNZIP_OK) {
			have_triggers = true;
		}
	}

	if (have_triggers) {
		wxString failure;

		/* A PreInstall that fails means the package is not installed at all. */
		if (!RunTrigger(triggers, hostfs_dir, record.name, kTriggerPreInstall,
		        "install", wxEmptyString, record.version, failure)) {
			unzip_close(&archive);
			wxRemoveFile(temp);
			result.message = failure + "\n\nNothing has been installed.";
			return result;
		}
	}

	for (int i = 0; i < count; i++) {
		const UnzipEntry *entry = unzip_entry(&archive, i);
		const wxString stored = wxString::FromUTF8(entry->name);
		wxString host_path, riscos_path;

		if (!reporter.Progress(i, count)) {
			result.cancelled = true;
			break;
		}

		if (entry->is_directory) {
			continue;	/* created as their files are written */
		}

		/* Metadata goes to the database, not onto the disc. */
		if (stored.StartsWith(wxString(kMetadataDir) + "/")) {
			const wxString rest = stored.Mid(strlen(kMetadataDir) + 1);
			const wxString leaf = stored.AfterLast('/');

			if (rest.StartsWith(wxString(kTriggersDir) + "/")) {
				/* Triggers are kept, because removal needs to run them long
				   after the zip has gone. They go under the machine's own
				   !Packages, so they are on the guest's disc where they can
				   actually be run. */
				const wxString dir = InfoDir(hostfs_dir, record.name) + Sep() +
				    kTriggersDir;
				char suffixed[UNZIP_MAX_NAME + 32];

				if (!wxDirExists(dir)) {
					wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL);
				}
				if (unzip_hostfs_leafname(entry, leaf.utf8_str(), suffixed,
				        sizeof(suffixed))) {
					unzip_extract_to_file(&archive, entry,
					    (dir + Sep() + wxString::FromUTF8(suffixed)).utf8_str());
					have_triggers = true;
				}
			} else if (leaf.IsSameAs("Control", false)) {
				MemberToString(&archive, entry, control);
			} else if (leaf.IsSameAs("Copyright", false)) {
				MemberToString(&archive, entry, copyright);
			}
			continue;
		}

		if (!MemberPaths(entry, hostfs_dir, host_path, riscos_path)) {
			continue;
		}

		{
			const wxString dir = host_path.BeforeLast(
			    wxFileName::GetPathSeparator());

			if (!dir.empty() && !wxDirExists(dir) &&
			    !wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
				result.message = wxString::Format(
				    "%s could not be created on the disc.", dir);
				break;
			}
		}

		if (unzip_extract_to_file(&archive, entry, host_path.utf8_str()) !=
		    UNZIP_OK) {
			result.message = wxString::Format(
			    "%s could not be written to the disc.", riscos_path);
			break;
		}

		manifest.push_back(riscos_path);
		/* Relative, so the machine can be moved or renamed later. */
		host_written.push_back(host_path.Mid(hostfs_dir.length() + 1));
		result.files++;
	}

	unzip_close(&archive);
	wxRemoveFile(temp);

	if (result.cancelled || !result.message.empty()) {
		/* Roll back what went down, so a failure part way through does not
		   leave a package the database does not know about. The exact paths
		   are to hand, so nothing has to be guessed at. */
		for (const auto &rel : host_written) {
			wxRemoveFile(hostfs_dir + Sep() + rel);
		}
		if (result.message.empty()) {
			result.message = "The installation was cancelled.";
		}
		return result;
	}

	/* --- record it --- */
	{
		const wxString info = InfoDir(hostfs_dir, record.name);
		wxString files_text;

		if (!wxDirExists(info) &&
		    !wxFileName::Mkdir(info, 0755, wxPATH_MKDIR_FULL)) {
			result.message = "The package could not be recorded in the "
			                 "database, so it has not been installed.";
			return result;
		}

		for (const auto &path : manifest) {
			files_text += path + "\n";
		}
		WriteWholeFile(info + Sep() + "Files", files_text);

		{
			wxString host_text;

			for (const auto &rel : host_written) {
				host_text += rel + "\n";
			}
			WriteWholeFile(info + Sep() + kHostManifest, host_text);
		}

		/* Prefer the package's own control record; fall back to the
		   catalogue's, which is the same information. */
		WriteWholeFile(info + Sep() + "Control",
		    control.empty() ? record.Field("package") : control);
		if (!copyright.empty()) {
			WriteWholeFile(info + Sep() + "Copyright", copyright);
		}

		std::map<wxString, wxString> status = ReadStatusLines(hostfs_dir);

		status[record.name] = wxString::Format("%s\t%s\tinstalled\t\t",
		                                       record.name, record.version);
		WriteStatusLines(hostfs_dir, status);
	}

	rpclog("packages: installed %s %s (%d file(s))\n",
	       static_cast<const char *>(record.name.utf8_str()),
	       static_cast<const char *>(record.version.utf8_str()), result.files);
	result.ok = true;

	/*
	 * PostInstall runs with the package in place and recorded. The manual is
	 * explicit that a failure here leaves the package installed and warns, which
	 * is right: the files are there and the database says so, and rolling back at
	 * this point would be a bigger lie than a warning.
	 */
	if (have_triggers) {
		wxString failure;

		if (!RunTrigger(triggers, hostfs_dir, record.name, kTriggerPostInstall,
		        "install", wxEmptyString, record.version, failure)) {
			result.message = failure + "\n\n" + record.name +
			    " is installed, but that step did not complete.";
		}
	}

	return result;
}

PackageActionResult PackageRemove(const wxString &package_name,
                                  const wxString &hostfs_dir,
                                  PackageTriggerRunner *triggers)
{
	PackageActionResult result;
	std::map<wxString, wxString> status = ReadStatusLines(hostfs_dir);
	std::vector<wxString> host_files = ReadManifestFile(hostfs_dir, package_name,
	                                                    kHostManifest);
	const std::vector<wxString> riscos_files = ReadManifest(hostfs_dir,
	                                                        package_name);
	std::vector<wxString> left_behind;

	if (status.find(package_name) == status.end()) {
		result.message = wxString::Format(
		    "%s is not installed on this machine.", package_name);
		return result;
	}

	/*
	 * Packages installed before the host manifest existed have only the RISC OS
	 * one, so derive the host paths from it: swap the separators back, then
	 * match the leafname exactly or with a HostFS filetype suffix. Deliberately
	 * not a "leaf*" pattern, which would also match a neighbour whose name
	 * merely begins the same way.
	 */
	if (host_files.empty()) {
		for (const auto &riscos_path : riscos_files) {
			const wxString rel = SwapDotsAndSlashes(riscos_path);
			const wxString host = hostfs_dir + Sep() + rel;

			if (wxFileExists(host)) {
				host_files.push_back(rel);
				continue;
			}

			/* Look for "leaf,xxx" in the file's own directory. */
			const wxString dir = host.BeforeLast(wxFileName::GetPathSeparator());
			const wxString leaf = host.AfterLast(wxFileName::GetPathSeparator());
			bool found = false;

			if (wxDirExists(dir)) {
				wxDir handle(dir);
				wxString entry;
				bool more = handle.GetFirst(&entry, wxEmptyString, wxDIR_FILES);

				while (more) {
					if (entry.BeforeFirst(',') == leaf) {
						host_files.push_back(
						    rel.BeforeLast(wxFileName::GetPathSeparator()) +
						    Sep() + entry);
						found = true;
						break;
					}
					more = handle.GetNext(&entry);
				}
			}

			if (!found) {
				left_behind.push_back(riscos_path);
			}
		}
	}

	if (host_files.empty() && riscos_files.empty()) {
		result.message = wxString::Format(
		    "The database has no record of what %s installed, so nothing has "
		    "been deleted. Remove it by hand if you are sure.", package_name);
		return result;
	}

	/* PreRemove can refuse: a failure here means the package is not removed. */
	{
		const PackageInstalledMap installed = PackageInstalledList(hostfs_dir);
		const auto it = installed.find(package_name);
		wxString failure;

		if (!RunTrigger(triggers, hostfs_dir, package_name, kTriggerPreRemove,
		        "remove", (it != installed.end()) ? it->second : wxString(),
		        wxEmptyString, failure)) {
			result.message = failure + "\n\nNothing has been deleted.";
			return result;
		}
	}

	/* wxWidgets logs a message of its own for a file that is not there, which
	   is not news: whether each one is present is what is being established. */
	{
		wxLogNull silence;

		for (const auto &rel : host_files) {
			const wxString host = hostfs_dir + Sep() + rel;

			if (wxFileExists(host)) {
				if (wxRemoveFile(host)) {
					result.files++;
				} else {
					left_behind.push_back(rel);
				}
			} else {
				left_behind.push_back(rel);
			}

			PruneEmptyDirs(host.BeforeLast(wxFileName::GetPathSeparator()),
			               hostfs_dir);
		}
	}

	/*
	 * A file that is not there is already in the state removal wants, so missing
	 * ones are worth mentioning and no more: somebody may have tidied one by
	 * hand, or an earlier attempt may have got part way, and refusing would
	 * leave them unable to remove the package at all.
	 *
	 * What must not pass is leaving files on the disc while saying they are
	 * gone, which is how the separator bug behaved. So the test is the direct
	 * one: are there still files where the missing ones should have been? If so,
	 * something is wrong with the paths and the database is left alone.
	 */
	if (!left_behind.empty()) {
		wxLogNull silence;
		wxString witness;

		for (const auto &rel : left_behind) {
			const wxString dir = (hostfs_dir + Sep() + rel).BeforeLast(
			    wxFileName::GetPathSeparator());

			if (!wxDirExists(dir)) {
				continue;	/* pruned: genuinely gone */
			}

			wxDir handle(dir);
			wxString entry;

			if (handle.IsOpened() &&
			    handle.GetFirst(&entry, wxEmptyString, wxDIR_FILES)) {
				witness = dir.Mid(hostfs_dir.length());
				witness.Trim(false);
				while (!witness.empty() &&
				       witness[0] == wxFileName::GetPathSeparator()) {
					witness = witness.Mid(1);
				}
				witness = witness.empty() ? entry
				                          : witness + Sep() + entry;
				break;
			}
		}

		if (!witness.empty()) {
			result.message = wxString::Format(
			    "%d of %s's files were deleted, but %d could not be found while "
			    "files remain on the disc where they should be, such as %s. "
			    "Nothing more has been changed and it is still recorded as "
			    "installed.", result.files, package_name,
			    static_cast<int>(left_behind.size()), witness);
			rpclog("packages: %s: %d recorded file(s) not found, disc not "
			       "clear (%s)\n",
			       static_cast<const char *>(package_name.utf8_str()),
			       static_cast<int>(left_behind.size()),
			       static_cast<const char *>(witness.utf8_str()));
			return result;
		}
	}

	/*
	 * PostRemove runs while its own file is still on the disc, and before the
	 * database entry goes. As with PostInstall, a failure warns rather than
	 * undoing the removal: the files have gone either way.
	 */
	{
		wxString failure;

		if (!RunTrigger(triggers, hostfs_dir, package_name, kTriggerPostRemove,
		        "remove", wxEmptyString, wxEmptyString, failure)) {
			result.message = failure;
		}
	}

	{
		const wxString info = InfoDir(hostfs_dir, package_name);

		/* The triggers go last, having been needed up to this point. */
		{
			const wxString dir = info + Sep() + kTriggersDir;

			if (wxDirExists(dir)) {
				wxString found = wxFindFirstFile(dir + Sep() + "*", wxFILE);

				while (!found.empty()) {
					wxRemoveFile(found);
					found = wxFindNextFile();
				}
				wxRmdir(dir);
			}
		}

		for (const char *leaf : { "Files", kHostManifest, "Control",
		                          "Copyright" }) {
			const wxString path = info + Sep() + leaf;

			if (wxFileExists(path)) {
				wxRemoveFile(path);
			}
		}
		if (wxDirExists(info)) {
			wxRmdir(info);
		}
	}

	status.erase(package_name);
	WriteStatusLines(hostfs_dir, status);

	rpclog("packages: removed %s (%d file(s))\n",
	       static_cast<const char *>(package_name.utf8_str()), result.files);
	result.ok = true;
	if (!left_behind.empty()) {
		result.message = wxString::Format(
		    "%s has been removed. %d of its files were already gone.",
		    package_name, static_cast<int>(left_behind.size()));
	}

	return result;
}

std::vector<PackageRecord> PackageResolveDepends(
    const PackageRecord &record,
    const std::vector<PackageRecord> &catalogue,
    const PackageInstalledMap &installed,
    std::vector<wxString> &missing)
{
	std::vector<PackageRecord> order;
	std::vector<wxString> queue;
	std::map<wxString, bool> seen;

	queue.push_back(record.name);
	seen[record.name.Lower()] = true;

	/* Breadth first, and the package itself is not included: the caller has
	   it. Version relations in a Depends field ("Foo (>= 1.2)") are read as
	   the name only, since the catalogue holds one version of each anyway. */
	for (size_t i = 0; i < queue.size(); i++) {
		const PackageRecord *current = nullptr;

		for (const auto &candidate : catalogue) {
			if (candidate.name.Lower() == queue[i].Lower()) {
				current = &candidate;
				break;
			}
		}
		if (current == nullptr) {
			continue;
		}

		wxStringTokenizer deps(current->depends, ",", wxTOKEN_STRTOK);

		while (deps.HasMoreTokens()) {
			wxString dep = deps.GetNextToken();

			dep = dep.BeforeFirst('(');
			dep.Trim().Trim(false);
			if (dep.empty() || seen.count(dep.Lower())) {
				continue;
			}
			seen[dep.Lower()] = true;

			if (installed.find(dep) != installed.end()) {
				continue;	/* already there */
			}

			const PackageRecord *found = nullptr;

			for (const auto &candidate : catalogue) {
				if (candidate.name.Lower() == dep.Lower()) {
					found = &candidate;
					break;
				}
			}

			if (found == nullptr) {
				missing.push_back(dep);
			} else {
				order.push_back(*found);
				queue.push_back(dep);
			}
		}
	}

	return order;
}
