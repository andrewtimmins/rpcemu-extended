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

#include <set>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>

#include "package_sources.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

const char *const kSourcesLeaf = "pkgsources";

/* Written above the records when the file is created, and kept when it is
   rewritten, because the point of the file is that somebody can edit it. */
const char *const kFileHeader =
    "# RPCEmu package sources.\n"
    "#\n"
    "# One record per repository, separated by a blank line. Lines beginning\n"
    "# with # are ignored. Delete this file to get the shipped list back.\n"
    "#\n"
    "#   Name         short name, letters, digits, - and _ only. It is used as\n"
    "#                a filename for that source's cached index.\n"
    "#   URL          the index file itself, http or https.\n"
    "#   Description  what it is, shown in the Sources window.\n"
    "#   Enabled      no to keep a source without reading it.\n"
    "#\n"
    "# The index must be in the RISC OS Packaging Project's format: control\n"
    "# records of \"Field: value\" lines, one per package.\n"
    "\n";

wxString Trim(const wxString &s)
{
	wxString out = s;

	out.Trim(true);
	out.Trim(false);

	return out;
}

/* One record's worth of accumulated fields, turned into a source. */
bool FinishRecord(PackageSource &record, bool have_any,
                  std::vector<PackageSource> &out,
                  std::set<wxString> &seen,
                  std::vector<wxString> *warnings, int record_line)
{
	const auto warn = [&](const wxString &text) {
		if (warnings != nullptr) {
			warnings->push_back(wxString::Format("line %d: %s",
			                                     record_line, text));
		}
	};

	if (!have_any) {
		return false;
	}

	wxString why;

	if (record.name.empty() && record.url.empty()) {
		return false;
	}
	if (!PackageSourceNameValid(record.name, &why)) {
		warn(wxString::Format("source name \"%s\" ignored, %s",
		                      record.name, why));
		return false;
	}
	if (!PackageSourceUrlValid(record.url, &why)) {
		warn(wxString::Format("source \"%s\" ignored, %s", record.name, why));
		return false;
	}

	/* Two sources with one name would share a cache file and shadow each
	   other in ways that would be very hard to explain. */
	const wxString key = record.name.Lower();

	if (seen.count(key) != 0) {
		warn(wxString::Format("source \"%s\" ignored, a source of that name "
		                      "has already been read", record.name));
		return false;
	}

	seen.insert(key);
	out.push_back(record);

	return true;
}

} /* namespace */

std::vector<PackageSource> PackageSourcesDefaults()
{
	return {
		{ "rool",
		  "https://packages.riscosopen.org/packages/pkg/rool",
		  "Applications from the RISC OS disc image, built nightly", true },
		{ "thirdparty",
		  "https://packages.riscosopen.org/packages/pkg/thirdparty",
		  "Programs and libraries from other authors, for any ARM machine", true },
		/* The RISC OS Community's own repository. Small, but it is where
		   some newer work appears first. Only the released index: there is
		   a testing one beside it, which is unstable by design. */
		{ "community",
		  "https://riscoscommunity.org/packages/rpkg/arm32/released",
		  "Released packages from the RISC OS Community repository", true },
		/* The Archimedes Software Preservation Project: commercial games of
		   the period, packaged to run on a machine like this one. Most need
		   ADFFS, which is in the same index and is pulled in as a
		   dependency. A handful are 26-bit only and are filtered out for
		   this machine, as any index's unsuitable entries are. */
		{ "jaspp",
		  "https://www.jaspp.org.uk/packages/release",
		  "Games from the Archimedes Software Preservation Project (JASPP)", true },
	};
}

wxString PackageSourcesPath()
{
	/* Beside the machines, ROMs and the catalogue cache: this is
	   per-installation state, not a shipped resource. The core returns the
	   data directory with a trailing separator already. */
	return wxString::FromUTF8(rpcemu_get_datadir()) + kSourcesLeaf;
}

bool PackageSourceNameValid(const wxString &name, wxString *why)
{
	const auto fail = [&](const wxString &text) {
		if (why != nullptr) {
			*why = text;
		}
		return false;
	};

	if (name.empty()) {
		return fail("it has no name");
	}
	if (name.length() > 32) {
		return fail("the name is longer than 32 characters");
	}

	for (const wxUniChar c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		                (c >= '0' && c <= '9') || c == '-' || c == '_';

		if (!ok) {
			return fail(wxString::Format(
			    "the name may only hold letters, digits, - and _ "
			    "(it becomes a filename), and \"%c\" is not one of those",
			    static_cast<char>(c.GetValue() < 128 ? c.GetValue() : '?')));
		}
	}

	if (why != nullptr) {
		why->clear();
	}

	return true;
}

bool PackageSourceUrlValid(const wxString &url, wxString *why)
{
	const auto fail = [&](const wxString &text) {
		if (why != nullptr) {
			*why = text;
		}
		return false;
	};

	if (url.empty()) {
		return fail("it has no URL");
	}
	if (!url.StartsWith("http://") && !url.StartsWith("https://")) {
		return fail("the URL must begin with http:// or https://");
	}
	if (url.find(' ') != wxString::npos) {
		return fail("the URL contains a space");
	}

	if (why != nullptr) {
		why->clear();
	}

	return true;
}

std::vector<PackageSource> PackageSourcesLoad(std::vector<wxString> *warnings)
{
	const wxString path = PackageSourcesPath();

	if (!wxFileName::FileExists(path)) {
		/* First run. Write the shipped list out so the file exists to be
		   edited; if that cannot be done it is not worth failing over,
		   the defaults still work for this session. */
		const std::vector<PackageSource> defaults = PackageSourcesDefaults();
		wxString error;

		PackageSourcesSave(defaults, &error);

		return defaults;
	}

	wxTextFile file;

	if (!file.Open(path)) {
		if (warnings != nullptr) {
			warnings->push_back(wxString::Format(
			    "could not read %s, using the shipped list", path));
		}
		return PackageSourcesDefaults();
	}

	std::vector<PackageSource> out;
	std::set<wxString> seen;
	PackageSource record;
	bool have_any = false;
	int record_line = 1;

	for (size_t i = 0; i < file.GetLineCount(); i++) {
		const wxString raw = file[i];
		const wxString line = Trim(raw);

		if (line.StartsWith("#")) {
			continue;
		}
		if (line.empty()) {
			FinishRecord(record, have_any, out, seen, warnings, record_line);
			record = PackageSource();
			have_any = false;
			record_line = static_cast<int>(i) + 2;
			continue;
		}

		const int colon = line.Find(':');

		if (colon == wxNOT_FOUND) {
			if (warnings != nullptr) {
				warnings->push_back(wxString::Format(
				    "line %d: \"%s\" is not \"Name: value\", ignored",
				    static_cast<int>(i) + 1, line));
			}
			continue;
		}

		if (!have_any) {
			record_line = static_cast<int>(i) + 1;
		}
		have_any = true;

		const wxString field = Trim(line.Left(colon)).Lower();
		const wxString value = Trim(line.Mid(colon + 1));

		if (field == "name") {
			record.name = value;
		} else if (field == "url") {
			record.url = value;
		} else if (field == "description") {
			record.description = value;
		} else if (field == "enabled") {
			const wxString v = value.Lower();

			record.enabled = !(v == "no" || v == "false" || v == "0" ||
			                   v == "off");
		}
		/* Anything else is somebody else's field, not a parse failure. */
	}

	FinishRecord(record, have_any, out, seen, warnings, record_line);
	file.Close();

	if (out.empty()) {
		/* An empty or hopeless file leaves nothing to fetch, which looks
		   from the catalogue like every repository has gone down. Say so
		   and carry on with the shipped list. */
		if (warnings != nullptr) {
			warnings->push_back(
			    "no usable sources in the file, using the shipped list");
		}
		return PackageSourcesDefaults();
	}

	return out;
}

bool PackageSourcesSave(const std::vector<PackageSource> &sources, wxString *error)
{
	const wxString path = PackageSourcesPath();
	wxString text = wxString::FromUTF8(kFileHeader);

	for (const auto &source : sources) {
		text += wxString::Format("Name: %s\n", source.name);
		text += wxString::Format("URL: %s\n", source.url);
		if (!source.description.empty()) {
			text += wxString::Format("Description: %s\n", source.description);
		}
		text += wxString::Format("Enabled: %s\n\n",
		                         source.enabled ? "yes" : "no");
	}

	/* Write beside the target and rename, so an interrupted write cannot
	   leave a half a file where the sources should be. */
	const wxString temp = path + ".new";
	wxFile out;

	if (!out.Create(temp, true) || !out.Write(text, wxConvUTF8)) {
		if (error != nullptr) {
			*error = wxString::Format("could not write %s", temp);
		}
		return false;
	}

	out.Close();

	if (wxFileName::FileExists(path) && !wxRemoveFile(path)) {
		if (error != nullptr) {
			*error = wxString::Format("could not replace %s", path);
		}
		return false;
	}
	if (!wxRenameFile(temp, path)) {
		if (error != nullptr) {
			*error = wxString::Format("could not rename %s to %s", temp, path);
		}
		return false;
	}

	if (error != nullptr) {
		error->clear();
	}

	return true;
}
