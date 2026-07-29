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
                                   const RiscosFetchLoopFactory &make_loop)
{
	PackageActionResult result;
	const wxString temp = wxFileName::CreateTempFileName("rpcemu-pkg");
	UnzipArchive archive;
	std::vector<wxString> manifest;		/* RISC OS paths, the standard form */
	std::vector<wxString> host_written;	/* exactly what was written */
	wxString control, copyright;

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

	/* --- unpack --- */
	if (unzip_open(&archive, temp.utf8_str()) != UNZIP_OK) {
		wxRemoveFile(temp);
		result.message = wxString::Format(
		    "%s is not a readable zip archive.", record.name);
		return result;
	}

	reporter.Stage(wxString::Format("Installing %s", record.name));

	const int count = unzip_entry_count(&archive);

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
			const wxString leaf = stored.AfterLast('/');

			if (leaf.IsSameAs("Control", false)) {
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

	return result;
}

PackageActionResult PackageRemove(const wxString &package_name,
                                  const wxString &hostfs_dir)
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

	{
		const wxString info = InfoDir(hostfs_dir, package_name);

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
