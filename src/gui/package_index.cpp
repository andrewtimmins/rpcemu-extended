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
 * package_index.cpp - reading the RISC OS package catalogue.
 *
 * The format is the RISC OS Packaging Project's, which is Debian's with RISC OS
 * fields added, and is specified in that project's policy manual. Only the
 * reading of it is here.
 */

#include <algorithm>

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>

#include "config_paths.h"
#include "http_transfer.h"
#include "package_index.h"
#include "package_sources.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

/* One file per source, plus a stamp, under the data directory. */
const char *const kCacheDirLeaf = "pkgcache";

/* Refetched when older than this. Long enough that browsing costs nothing,
   short enough that a new package shows up the same day. */
const long kCacheMaxAgeSeconds = 6 * 60 * 60;

wxString CacheDir()
{
	/* Beside the machines and ROMs, in the writable data directory: the
	   catalogue is per-installation state, not a shipped resource. The core
	   returns this with a trailing separator already. */
	return wxString::FromUTF8(rpcemu_get_datadir()) + kCacheDirLeaf;
}

wxString CachePath(const wxString &source_name)
{
	return CacheDir() + wxFileName::GetPathSeparator() + source_name;
}

/* Field names are case-insensitive per the policy manual, so they are folded
   before being stored and looked up. */
wxString FoldFieldName(const wxString &name)
{
	return name.Lower();
}

/**
 * Compare two package version strings.
 *
 * Not a full implementation of the policy manual's version ordering, which has
 * epochs and upstream/package parts: this splits on the non-alphanumerics that
 * separate the components ("1-1-1", "0.9.8-1") and compares numerically where
 * both sides are numbers. It only has to decide which of two entries for the
 * same package is newer, and every version seen in the live indexes is of that
 * shape. Returns <0, 0 or >0.
 */
int CompareVersions(const wxString &a, const wxString &b)
{
	wxStringTokenizer ta(a, ".-+:~"), tb(b, ".-+:~");

	while (ta.HasMoreTokens() || tb.HasMoreTokens()) {
		const wxString pa = ta.HasMoreTokens() ? ta.GetNextToken() : "0";
		const wxString pb = tb.HasMoreTokens() ? tb.GetNextToken() : "0";
		long na = 0, nb = 0;

		if (pa.ToLong(&na) && pb.ToLong(&nb)) {
			if (na != nb) {
				return na < nb ? -1 : 1;
			}
		} else {
			const int c = pa.Cmp(pb);

			if (c != 0) {
				return c;
			}
		}
	}

	return 0;
}

/**
 * Make a package's URL absolute.
 *
 * ROOL's indexes give absolute URLs, but riscoscommunity.org gives paths
 * beginning with a slash, and the format allows either. A leading slash is
 * resolved against the index's host, anything else against the directory the
 * index sits in.
 */
/*
 * Prefer https where the host is known to serve it.
 *
 * The ROOL indexes give http:// addresses for every package, which macOS will not
 * fetch, so a download there failed even once the catalogue had been read. The
 * same host serves all 194 of them over https.
 */
wxString PreferHttps(const wxString &url)
{
	if (url.StartsWith("http://packages.riscosopen.org/")) {
		return "https://" + url.Mid(7);
	}

	return url;
}

wxString ResolveUrl(const wxString &url, const wxString &index_url)
{
	if (url.empty() || url.Contains("://")) {
		return PreferHttps(url);
	}

	/* Split the index URL into scheme+host and path. */
	const int scheme_end = index_url.Find("://");

	if (scheme_end == wxNOT_FOUND) {
		return url;
	}

	const int host_start = scheme_end + 3;
	const int slash = index_url.find('/', host_start);
	const wxString root = (slash == static_cast<int>(wxString::npos))
	    ? index_url : index_url.Left(slash);

	if (url[0] == '/') {
		return PreferHttps(root + url);
	}

	const wxString dir = index_url.BeforeLast('/');

	return PreferHttps((dir.empty() ? root : dir) + "/" + url);
}

}  /* namespace */

wxString PackageRecord::Field(const wxString &field_name) const
{
	const auto it = fields.find(FoldFieldName(field_name));

	return it == fields.end() ? wxString() : it->second;
}

bool PackageRecord::RunsHere() const
{
	const wxString env = environment.Lower();

	/* An absent Environment is treated as portable, which is how the older
	   records in the third-party index read: 31 of them say nothing, and they
	   are BASIC programs and documents. */
	return env.empty() || env == "any" || env == "arm" || env == "arm32";
}

std::vector<PackageSource> PackageIndexSources()
{
	/*
	 * The list used to live here. It now comes from a file the user owns, so
	 * a repository can be added or turned off without a new release; see
	 * package_sources.cpp, which also holds the shipped defaults and the
	 * reason each of them is or is not in the list.
	 *
	 * Only the enabled ones are returned, so every caller reads "the sources
	 * to fetch" and none of them has to remember to check the flag.
	 *
	 * One thing worth keeping in mind when adding one: https, not http, and
	 * it matters rather than being tidiness. macOS refuses cleartext HTTP by
	 * default (App Transport Security), so over http a source fetches nothing
	 * there while working everywhere else. That was reported from a Mac as
	 * "there are only three items listed".
	 */
	std::vector<PackageSource> enabled;

	for (const auto &source : PackageSourcesLoad()) {
		if (source.enabled) {
			enabled.push_back(source);
		}
	}

	return enabled;
}

int PackageIndexParse(const wxString &text, const wxString &source_name,
                      const wxString &index_url,
                      std::vector<PackageRecord> &out)
{
	PackageRecord record;
	wxString last_field;
	int count = 0;
	bool in_record = false;

	/* Records end at a blank line or the end of the file. A line beginning
	   with whitespace continues the field before it. */
	wxStringTokenizer lines(text, "\n", wxTOKEN_RET_EMPTY_ALL);

	const auto finish = [&]() {
		if (in_record && !record.name.empty()) {
			record.fields["source"] = source_name;
			out.push_back(record);
			count++;
		}
		record = PackageRecord();
		last_field.clear();
		in_record = false;
	};

	while (lines.HasMoreTokens()) {
		wxString line = lines.GetNextToken();

		if (!line.empty() && line.Last() == '\r') {
			line.RemoveLast();
		}

		if (line.Trim().empty()) {
			finish();
			continue;
		}

		if (line[0] == ' ' || line[0] == '\t') {
			/* Continuation. A lone full stop stands for a blank line,
			   which is how the format writes one inside a value. */
			if (last_field.empty()) {
				continue;
			}
			wxString more = line;

			more.Trim(false);
			if (more == ".") {
				more = wxEmptyString;
			}
			if (last_field == "description") {
				record.description_long +=
				    (record.description_long.empty() ? "" : "\n") + more;
			}
			record.fields[last_field] += "\n" + more;
			continue;
		}

		const int colon = line.Find(':');

		if (colon == wxNOT_FOUND) {
			continue;	/* not a field; ignore rather than fail */
		}

		const wxString name = FoldFieldName(line.Left(colon).Trim());
		wxString value = line.Mid(colon + 1);

		value.Trim(false).Trim(true);
		in_record = true;
		last_field = name;
		record.fields[name] = value;

		if (name == "package") {
			record.name = value;
		} else if (name == "version") {
			record.version = value;
		} else if (name == "section") {
			record.section = value;
		} else if (name == "description") {
			record.description = value;
		} else if (name == "url") {
			record.url = ResolveUrl(value, index_url);
		} else if (name == "md5sum") {
			record.md5 = value;
		} else if (name == "environment") {
			record.environment = value;
		} else if (name == "licence" || name == "license") {
			record.licence = value;
		} else if (name == "depends") {
			record.depends = value;
		} else if (name == "recommends") {
			record.recommends = value;
		} else if (name == "osdepends") {
			record.osdepends = value;
		} else if (name == "components") {
			record.components = value;
		} else if (name == "homepage") {
			record.homepage = value;
		} else if (name == "maintainer") {
			record.maintainer = value;
		} else if (name == "size") {
			long long size = 0;

			if (value.ToLongLong(&size)) {
				record.size = size;
			}
		}
	}

	finish();
	return count;
}

/**
 * Keep only the newest version of each package.
 *
 * A package can appear in more than one source, and a source can list several
 * versions. The newest wins, and where two sources offer the same version the
 * first source listed wins, which is why rool comes before thirdparty.
 */
static void KeepNewest(std::vector<PackageRecord> &packages)
{
	std::map<wxString, size_t> best;	/* lower-cased name -> index */
	std::vector<PackageRecord> kept;

	for (const auto &pkg : packages) {
		const wxString key = pkg.name.Lower();
		const auto it = best.find(key);

		if (it == best.end()) {
			best[key] = kept.size();
			kept.push_back(pkg);
		} else if (CompareVersions(pkg.version, kept[it->second].version) > 0) {
			kept[it->second] = pkg;
		}
	}

	packages.swap(kept);
}

bool PackageIndexLoadCached(std::vector<PackageRecord> &out)
{
	const std::vector<PackageSource> sources = PackageIndexSources();
	bool any = false;

	out.clear();
	for (const auto &source : sources) {
		const wxString path = CachePath(source.name);
		wxFFile file;

		if (!wxFileExists(path) || !file.Open(path, "rb")) {
			continue;
		}

		wxString text;

		if (file.ReadAll(&text, wxConvISO8859_1)) {
			PackageIndexParse(text, source.name, source.url, out);
			any = true;
		}
	}

	KeepNewest(out);
	return any;
}

long PackageIndexCacheAge()
{
	const std::vector<PackageSource> sources = PackageIndexSources();
	long oldest = -1;

	for (const auto &source : sources) {
		const wxString path = CachePath(source.name);

		if (!wxFileExists(path)) {
			return -1;	/* incomplete: treat as no cache */
		}

		const wxDateTime modified = wxFileName(path).GetModificationTime();
		const long age = static_cast<long>(
		    (wxDateTime::Now() - modified).GetSeconds().ToLong());

		oldest = std::max(oldest, age);
	}

	return oldest;
}

PackageIndexResult PackageIndexRefresh(std::vector<PackageRecord> &out,
                                       RiscosFetchReporter &reporter,
                                       const RiscosFetchLoopFactory &make_loop,
                                       bool force)
{
	PackageIndexResult result;
	const std::vector<PackageSource> sources = PackageIndexSources();
	const long age = PackageIndexCacheAge();

	if (!force && age >= 0 && age < kCacheMaxAgeSeconds) {
		if (PackageIndexLoadCached(out)) {
			result.ok = true;
			result.sources_read = static_cast<int>(sources.size());
			result.packages = static_cast<int>(out.size());
			return result;
		}
	}

	if (!wxDirExists(CacheDir()) && !wxFileName::Mkdir(CacheDir(), 0755,
	        wxPATH_MKDIR_FULL)) {
		result.message = "The package cache directory could not be created.";
		return result;
	}

	out.clear();
	for (const auto &source : sources) {
		Transfer transfer(reporter, wxString::Format("Fetching the %s package list",
		                                             source.name), make_loop);

		if (!transfer.ToMemory(source.url)) {
			if (transfer.WasCancelled()) {
				result.cancelled = true;
				return result;
			}
			/* One source failing should not cost the others: report it
			   and carry on, so a catalogue is still shown. */
			rpclog("packages: %s could not be fetched: %s\n",
			       static_cast<const char *>(source.name.utf8_str()),
			       static_cast<const char *>(transfer.Error().utf8_str()));
			result.message = transfer.Error();
			continue;
		}

		const int read = PackageIndexParse(transfer.Body(), source.name,
		                                   source.url, out);

		result.sources_read++;
		rpclog("packages: %s holds %d package(s)\n",
		       static_cast<const char *>(source.name.utf8_str()), read);

		/* Cached only once parsed, so a truncated download cannot become
		   the cache. */
		wxFFile file;

		if (file.Open(CachePath(source.name), "wb")) {
			file.Write(transfer.Body(), wxConvISO8859_1);
			file.Close();
		}
	}

	KeepNewest(out);
	result.packages = static_cast<int>(out.size());
	result.ok = result.sources_read > 0;
	if (!result.ok && result.message.empty()) {
		result.message = "No package lists could be fetched.";
	}

	return result;
}
