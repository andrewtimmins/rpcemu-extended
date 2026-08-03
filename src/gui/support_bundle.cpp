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
 * support_bundle.cpp - the small files a bug report needs, in one zip.
 *
 * What goes in is deliberately short: the log, the machine's configuration,
 * its CMOS and the metadata beside it. Not the hard disc, and not a saved
 * state - both are large, and a state is a whole RAM image that could hold
 * anything the guest had open.
 *
 * The two text members are redacted on the way in. This is meant to be
 * attached to a public issue, and the log carries the configuration verbatim:
 * settings.cpp logs every key it reads, so the VNC password is in there in
 * plain text along with the reporter's home directory.
 */

#include "support_bundle.h"

#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include "config_paths.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

/*
 * Values that are nobody else's business, by configuration key.
 *
 * Both files are searched for these: the .cfg holds them directly, and the
 * log holds them again because every key read is logged as it is parsed.
 *
 * The network settings are deliberately not here. The MAC address is one
 * RPCEmu invents (06:02:03:04:05:06, last byte per instance) unless somebody
 * types one for the emulated card, and the IP address is the NAT network's
 * own - neither says anything about the host's real hardware, and both are
 * worth having when the report is about networking.
 */
const char *const kSecretKeys[] = {
	"vnc_password",
	"username",
};

/** Replace the value in `key = "value"` or `key=value`, whichever form. */
wxString RedactKeys(wxString text)
{
	for (const char *key : kSecretKeys) {
		/* Quoted, as the log writes it. */
		wxRegEx quoted(wxString::Format("(%s[[:space:]]*=[[:space:]]*\")[^\"]*(\")", key));

		if (quoted.IsValid()) {
			quoted.ReplaceAll(&text, "\\1<redacted>\\2");
		}

		/* Bare, as the configuration file writes it. */
		wxRegEx bare(wxString::Format("(^|\n)(%s[[:space:]]*=)[^\n]*", key));

		if (bare.IsValid()) {
			bare.ReplaceAll(&text, "\\1\\2<redacted>");
		}
	}
	return text;
}

/*
 * Take the reporter's name out of every path in the text.
 *
 * Sixty-eight places log a path of some kind, so this is done by pattern
 * rather than at each of them: what identifies somebody is the home directory
 * the path starts with, and the rest is what makes the line worth reading.
 */
wxString RedactHomePaths(wxString text)
{
	const wxString home = wxFileName::GetHomeDir();

	/* The real one first, so it goes even if it sits somewhere unusual. */
	if (!home.empty() && home != "/" ) {
		text.Replace(home, "<home>");
	}

	static const char *const kPatterns[] = {
		"/home/[^/[:space:]\"']+",
		"/Users/[^/[:space:]\"']+",
		"[A-Za-z]:\\\\Users\\\\[^\\\\[:space:]\"']+",
	};

	for (const char *pattern : kPatterns) {
		wxRegEx re(pattern);

		if (re.IsValid()) {
			re.ReplaceAll(&text, "<home>");
		}
	}
	return text;
}

wxString Redact(const wxString &text)
{
	return RedactHomePaths(RedactKeys(text));
}

/** Whole file as text, or empty when it cannot be read. */
bool ReadTextFile(const wxString &path, wxString *out)
{
	wxFFile file(path, "rb");

	return file.IsOpened() && file.ReadAll(out);
}

/** Add @data to the archive as @name. */
bool AddText(wxZipOutputStream &zip, const wxString &name, const wxString &data,
             std::vector<SupportBundleMember> *members)
{
	const wxScopedCharBuffer utf8 = data.utf8_str();

	if (!zip.PutNextEntry(name)) {
		return false;
	}
	zip.Write(utf8.data(), utf8.length());
	if (!zip.IsOk()) {
		return false;
	}

	members->push_back({ name, (wxFileOffset) utf8.length() });
	return true;
}

/** Copy @path in verbatim. Missing files are skipped rather than fatal. */
bool AddFile(wxZipOutputStream &zip, const wxString &name, const wxString &path,
             std::vector<SupportBundleMember> *members)
{
	if (!wxFileName::FileExists(path)) {
		return true;
	}

	wxFFileInputStream in(path);

	if (!in.IsOk()) {
		return true;
	}
	if (!zip.PutNextEntry(name)) {
		return false;
	}
	zip.Write(in);
	if (!zip.IsOk()) {
		return false;
	}

	members->push_back({ name, in.GetLength() });
	return true;
}

/** Copy @path in with the redactions applied. */
bool AddRedactedFile(wxZipOutputStream &zip, const wxString &name,
                     const wxString &path,
                     std::vector<SupportBundleMember> *members)
{
	wxString text;

	if (!wxFileName::FileExists(path) || !ReadTextFile(path, &text)) {
		return true;
	}
	return AddText(zip, name, Redact(text), members);
}

} // namespace

wxString
SupportBundleSuggestedName()
{
	const wxString machine = wxString::FromUTF8(config.name);
	const wxString stamp = wxDateTime::Now().Format("%Y%m%d");

	if (machine.empty()) {
		return wxString::Format("rpcemu-support-%s.zip", stamp);
	}
	return wxString::Format("rpcemu-support-%s-%s.zip",
	    ConfigPathsSanitizeName(machine), stamp);
}

SupportBundleResult
SupportBundleWrite(const wxString &dest_path, const wxString &screenshot_path)
{
	SupportBundleResult result;
	wxFFileOutputStream out(dest_path);

	if (!out.IsOk()) {
		result.message = "The file could not be created. Check there is room "
		                 "and that the folder can be written to.";
		return result;
	}

	wxZipOutputStream zip(out);
	const wxString machine = wxString::FromUTF8(config.name);
	const wxString machine_dir = wxString::FromUTF8(rpcemu_get_machine_datadir());
	bool ok = true;

	/* Redacted: the log repeats the configuration as it parses it. */
	ok = ok && AddRedactedFile(zip, "rpclog.txt",
	    wxString::FromUTF8(rpcemu_get_log_path()), &result.members);

	if (!machine.empty()) {
		const wxFileName cfg(ConfigPathsConfigsDir(),
		    ConfigPathsSanitizeName(machine) + ".cfg");

		ok = ok && AddRedactedFile(zip, "machine.cfg", cfg.GetFullPath(),
		    &result.members);
	}

	/* Binary, and neither says anything about the person running it. */
	if (!machine_dir.empty()) {
		const wxFileName dir(machine_dir);

		ok = ok && AddFile(zip, "cmos.ram",
		    wxFileName(dir.GetPath(), "cmos.ram").GetFullPath(), &result.members);
		ok = ok && AddFile(zip, "emulator.meta",
		    wxFileName(dir.GetPath(), "emulator.meta").GetFullPath(), &result.members);
	}

	if (!screenshot_path.empty()) {
		ok = ok && AddFile(zip, "screen.png", screenshot_path, &result.members);
	}

	if (!ok || !zip.Close() || !out.Close()) {
		result.message = "The archive could not be written.";
		return result;
	}

	if (result.members.empty()) {
		result.message = "There was nothing to collect: no log and no machine "
		                 "configuration could be found.";
		return result;
	}

	result.ok = true;
	return result;
}
