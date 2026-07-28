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
 * riscos_fetch.cpp - fetching RISC OS from RISC OS Open.
 *
 * Three steps: work out what to download, download it, then put it where the
 * emulator will find it. Only the last of those touches anything the user
 * already has, and it is written to one side and moved into place at the end,
 * so cancelling or losing the network leaves the installation as it was.
 *
 * Transfers go through wxWebRequest, which uses the platform's own HTTP stack
 * (WinHTTP on Windows, NSURLSession on macOS, libcurl elsewhere), so there is
 * no new dependency and TLS is the host's business rather than ours.
 */

#include <map>
#include <memory>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dir.h>
#include <wx/evtloop.h>
#include <wx/filefn.h>
#include <wx/font.h>
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/webrequest.h>

#include "apache_licence.h"
#include "config_paths.h"
#include "riscos_fetch.h"

extern "C" {
#include "rpcemu.h"
#include "unzip.h"
}

namespace {

/*
 * Where things come from.
 *
 * The two ROM builds are found by the anchor names on the downloads page
 * rather than by their filenames, because the filenames carry the version and
 * so change with every release, whereas the anchors have stayed put. The
 * filenames are kept as a fallback for the day the page is restructured.
 */
const char *const kDownloadsPage =
    "https://www.riscosopen.org/content/downloads/riscpc";

const char *const kStableAnchor = "iomd_5_30_stable__softload_";
const char *const kStableFallback =
    "https://www.riscosopen.org/zipfiles/platform/riscpc/IOMD-Soft.5.30.zip";

const char *const kNightlyAnchor = "beta_iomd_softload";
const char *const kNightlyFallback =
    "https://www.riscosopen.org/zipfiles/platform/riscpc/IOMDHALDev.5.31.zip";

const char *const kHardDiscUrl =
    "https://www.riscosopen.org/zipfiles/platform/common/HardDisc4.zip";

const char *const kSiteRoot = "https://www.riscosopen.org";

/** Where the ROM image sits inside either softload archive. */
const char *const kRomMember = "soft/!Boot/Resources/SoftLoad/riscos";

/**
 * HardDisc4 wraps its tree in one directory, which should not appear on the
 * machine's HostFS.
 */
const int kHardDiscStrip = 1;

/**
 * RISC OS Open put a copy of the Apache licence at the root of HardDisc4.zip,
 * alongside the wrapper directory. Stripping a component would drop it, and
 * Apache 2.0 section 4(a) asks that the licence travel with the work, so it is
 * lifted out separately and written to the root of the machine's disc.
 */
const char *const kDiscLicenceMember = "LICENSE";

/* Rough sizes, for warning the user before they commit to the download. */
const long long kRomApproxBytes = 2200000LL;
const long long kDiscApproxBytes = 13300000LL;

/**
 * RISC OS Open asked to be able to identify the emulator's own traffic, so
 * every request carries this. It is not a licence check or a tracker, just a
 * courtesy to somebody hosting the files for us.
 */
wxString RequestedByHeader()
{
	return wxString::Format("RPCEmu Spork Edition/%s", VERSION);
}

/* --------------------------------------------------------------------- */
/* Transfers                                                              */
/* --------------------------------------------------------------------- */

/**
 * One HTTP GET, run to completion against a nested event loop.
 *
 * wxWebRequest is asynchronous, which is what keeps the interface alive during
 * a download, but every caller here wants to wait for the result. Running a
 * nested loop gives that without a worker thread, and leaves the progress
 * dialog able to repaint and its Cancel button able to be pressed.
 */
class Transfer : public wxEvtHandler {
public:
	Transfer(RiscosFetchReporter &reporter, const wxString &stage,
	         const RiscosFetchLoopFactory &make_loop)
	    : reporter_(reporter), stage_(stage), make_loop_(make_loop) { }

	/** Fetch @url into memory. Response text lands in Body(). */
	bool ToMemory(const wxString &url)
	{
		return Run(url, wxWebRequest::Storage_Memory, wxEmptyString);
	}

	/** Fetch @url and leave it at @dest_path. */
	bool ToFile(const wxString &url, const wxString &dest_path)
	{
		return Run(url, wxWebRequest::Storage_File, dest_path);
	}

	const wxString &Body() const { return body_; }
	const wxString &Error() const { return error_; }
	bool WasCancelled() const { return cancelled_; }

	/** Value of a response header, or empty. */
	wxString Header(const wxString &name) const
	{
		return headers_.count(name) ? headers_.at(name) : wxString();
	}

private:
	bool Run(const wxString &url, wxWebRequest::Storage storage,
	         const wxString &dest_path)
	{
		/* The loop comes from the caller rather than being a plain
		   wxEventLoop, because in a GUI build that name means the
		   toolkit's loop, which --fetch-riscos cannot use: it runs
		   with no display connection. */
		std::unique_ptr<wxEventLoopBase> loop(make_loop_
		    ? make_loop_() : new wxEventLoop);
		wxEventLoopActivator activate(loop.get());
		wxTimer ticker(this);

		ok_ = false;
		cancelled_ = false;
		error_.clear();
		body_.clear();
		dest_path_ = dest_path;

		request_ = wxWebSession::GetDefault().CreateRequest(this, url);
		if (!request_.IsOk()) {
			error_ = "The system's HTTP support is unavailable.";
			return false;
		}

		request_.SetHeader("X-Requested-By", RequestedByHeader());
		request_.SetHeader("User-Agent", RequestedByHeader());
		request_.SetStorage(storage);

		Bind(wxEVT_WEBREQUEST_STATE, &Transfer::OnState, this);
		Bind(wxEVT_TIMER, &Transfer::OnTick, this);

		if (!reporter_.Stage(stage_)) {
			cancelled_ = true;
			return false;
		}

		request_.Start();
		ticker.Start(150);
		loop->Run();
		ticker.Stop();

		return ok_;
	}

	/* Progress is polled rather than pushed: wxWebRequest reports byte
	   counts on demand but has no per-chunk event when it is storing the
	   response for us. */
	void OnTick(wxTimerEvent &)
	{
		if (!reporter_.Progress(request_.GetBytesReceived(),
		                        request_.GetBytesExpectedToReceive())) {
			cancelled_ = true;
			request_.Cancel();
		}
	}

	void OnState(wxWebRequestEvent &event)
	{
		switch (event.GetState()) {
		case wxWebRequest::State_Completed: {
			const wxWebResponse &response = event.GetResponse();

			if (response.GetStatus() != 200) {
				error_ = wxString::Format(
				    "The server answered %d (%s).",
				    response.GetStatus(),
				    response.GetStatusText());
				break;
			}

			CollectHeaders(response);

			if (dest_path_.empty()) {
				body_ = response.AsString();
				ok_ = true;
			} else {
				/* With file storage the body is in a temporary
				   file that goes away with the response, so it
				   has to be taken now. */
				ok_ = wxCopyFile(response.GetDataFile(),
				                 dest_path_, true);
				if (!ok_) {
					error_ = "Could not write the downloaded file to disc.";
				}
			}
			break;
		}

		case wxWebRequest::State_Failed:
			error_ = event.GetErrorDescription();
			if (error_.empty()) {
				error_ = "The download failed.";
			}
			break;

		case wxWebRequest::State_Cancelled:
			cancelled_ = true;
			break;

		case wxWebRequest::State_Unauthorized:
			error_ = "The server refused the request.";
			break;

		default:
			return;		/* Still going; keep the loop running */
		}

		wxEventLoopBase::GetActive()->ScheduleExit();
	}

	void CollectHeaders(const wxWebResponse &response)
	{
		static const char *const wanted[] = { "Last-Modified", "Content-Length" };

		for (const char *name : wanted) {
			const wxString value = response.GetHeader(name);

			if (!value.empty()) {
				headers_[name] = value;
			}
		}
	}

	RiscosFetchReporter &reporter_;
	wxString stage_;
	RiscosFetchLoopFactory make_loop_;
	wxWebRequest request_;
	wxString dest_path_;
	wxString body_;
	wxString error_;
	std::map<wxString, wxString> headers_;
	bool ok_ = false;
	bool cancelled_ = false;
};

/* --------------------------------------------------------------------- */
/* Working out what to download                                           */
/* --------------------------------------------------------------------- */

/**
 * Find the target of the link with a given anchor name on the downloads page.
 *
 * The markup is <a name="anchor" href="/path">, and only that one shape is
 * accepted: a looser search risks picking up an unrelated link and quietly
 * installing the wrong thing, where failing here just falls back to the
 * known URL.
 */
wxString FindAnchorTarget(const wxString &page, const wxString &anchor)
{
	const wxString needle = wxString::Format("<a name=\"%s\" href=\"", anchor);
	const int start = page.Find(needle);

	if (start == wxNOT_FOUND) {
		return wxEmptyString;
	}

	const size_t from = static_cast<size_t>(start) + needle.length();
	const int end = page.find('"', from);

	if (end == wxNOT_FOUND) {
		return wxEmptyString;
	}

	wxString target = page.Mid(from, static_cast<size_t>(end) - from);

	if (target.StartsWith("/")) {
		target = wxString(kSiteRoot) + target;
	}

	/* The links carry a "?<timestamp>" cache-buster, so the check has to be
	   against the path and not the whole target. The query string is kept:
	   it is what a browser would send, and it is harmless. */
	return target.BeforeFirst('?').EndsWith(".zip") ? target : wxString();
}

/** Resolve the ROM archive URL, preferring the page, falling back to the file. */
wxString ResolveRomUrl(RiscosRelease release, RiscosFetchReporter &reporter,
                       const RiscosFetchLoopFactory &make_loop)
{
	const bool nightly = (release == RiscosRelease::Nightly);
	const char *anchor = nightly ? kNightlyAnchor : kStableAnchor;
	const char *fallback = nightly ? kNightlyFallback : kStableFallback;
	Transfer page(reporter, "Looking up the current download...", make_loop);

	if (page.ToMemory(kDownloadsPage)) {
		const wxString found = FindAnchorTarget(page.Body(), anchor);

		if (!found.empty()) {
			return found;
		}
		rpclog("riscos_fetch: anchor '%s' not on the downloads page, "
		       "using the known URL\n", anchor);
	} else if (page.WasCancelled()) {
		return wxEmptyString;
	}

	return wxString::FromUTF8(fallback);
}

/**
 * Pull "5.30" out of a URL such as ".../IOMD-Soft.5.30.zip".
 *
 * The archive filename states the version plainly. The ROM image also
 * contains version strings, but it holds a whole table of them for every
 * release it knows about, so reading it there gets the wrong answer.
 */
wxString VersionFromUrl(const wxString &url)
{
	const wxString leaf = url.AfterLast('/');

	for (size_t i = 0; i + 2 < leaf.length(); i++) {
		if (wxIsdigit(leaf[i]) && leaf[i + 1] == '.' &&
		    wxIsdigit(leaf[i + 2])) {
			size_t end = i + 2;

			while (end + 1 < leaf.length() && wxIsdigit(leaf[end + 1])) {
				end++;
			}
			return leaf.Mid(i, end - i + 1);
		}
	}

	return wxEmptyString;
}

/** "Sun, 19 Jul 2026 06:38:49 GMT" to "20260719", for naming a nightly. */
wxString DateStampFromHeader(const wxString &last_modified)
{
	wxDateTime when;

	if (last_modified.empty() || !when.ParseRfc822Date(last_modified)) {
		return wxEmptyString;
	}

	return when.Format("%Y%m%d");
}

/* --------------------------------------------------------------------- */
/* Putting it in place                                                    */
/* --------------------------------------------------------------------- */

/** A name not already taken, by appending -2, -3 and so on. */
wxString UniqueName(const wxString &base,
                    bool (*is_taken)(const wxString &))
{
	if (!is_taken(base)) {
		return base;
	}

	for (int n = 2; n < 100; n++) {
		const wxString candidate = wxString::Format("%s-%d", base, n);

		if (!is_taken(candidate)) {
			return candidate;
		}
	}

	return wxEmptyString;
}

bool RomNameTaken(const wxString &name)
{
	const wxString path = ConfigPathsRomsDir() +
	    wxFileName::GetPathSeparator() + name;

	return wxFileExists(path) || wxDirExists(path);
}

bool MachineNameTaken(const wxString &name)
{
	return !ConfigPathsIsNameUnique(name);
}

/**
 * Has anything been put on this HostFS beyond what a new machine is seeded
 * with?
 *
 * A machine's HostFS is the user's disc, and the rule is that a download never
 * overwrites one. But a machine created moments ago is not empty either: it
 * carries the seed copied from default/hostfs. Comparing against that seed
 * distinguishes "untouched" from "in use", so a brand new machine can have its
 * disc populated while an established one is left alone.
 */
bool HostfsIsPristine(const wxString &hostfs_dir)
{
	const wxString seed_dir = ConfigPathsResourceDir() +
	    wxFileName::GetPathSeparator() + "default" +
	    wxFileName::GetPathSeparator() + "hostfs";
	wxArrayString seeded;
	wxString name;

	if (!wxDirExists(hostfs_dir)) {
		return true;
	}

	if (wxDirExists(seed_dir)) {
		wxDir seed(seed_dir);

		if (seed.IsOpened() && seed.GetFirst(&name)) {
			do {
				seeded.Add(name);
			} while (seed.GetNext(&name));
		}
	}

	wxDir dir(hostfs_dir);
	if (!dir.IsOpened()) {
		return false;
	}

	if (!dir.GetFirst(&name)) {
		return true;		/* Empty */
	}

	do {
		if (seeded.Index(name) == wxNOT_FOUND) {
			return false;
		}
	} while (dir.GetNext(&name));

	return true;
}

/**
 * Is the file at @path this exact content, by size and CRC-32?
 *
 * The CRC is the one the archive records for the member, so this answers
 * "have I already unpacked this?" without unpacking it again.
 */
bool FileMatchesCrc(const wxString &path, uint32_t crc, uint32_t size)
{
	uint8_t buffer[64 * 1024];
	uint32_t running = 0;
	size_t got;
	FILE *fp;

	if (!wxFileExists(path) || wxFileName::GetSize(path) != wxULongLong(size)) {
		return false;
	}

	fp = fopen(path.utf8_str(), "rb");
	if (fp == NULL) {
		return false;
	}

	while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		running = unzip_crc32(running, buffer, got);
	}
	fclose(fp);

	return running == crc;
}

/** Whole-tree delete, used only on directories this code created or seeded. */
bool RemoveTree(const wxString &path)
{
	if (!wxDirExists(path)) {
		return true;
	}
	return wxFileName::Rmdir(path, wxPATH_RMDIR_RECURSIVE);
}

/** Progress adaptor so unpacking reports on the same interface as downloading. */
struct UnpackContext {
	RiscosFetchReporter *reporter;
	bool cancelled;
};

bool UnpackProgress(int done, int total, const char *name, void *opaque)
{
	UnpackContext *ctx = static_cast<UnpackContext *>(opaque);

	(void) name;

	if (!ctx->reporter->Progress(done, total)) {
		ctx->cancelled = true;
		return false;
	}
	return true;
}

/**
 * Extract the single ROM image from a softload archive.
 *
 * @param dest_dir  Where ROMs live
 * @param base_name Name to give it, if it is not already there
 * @param out_name  Name it ended up under
 */
bool InstallRom(const wxString &archive_path, const wxString &dest_dir,
                const wxString &base_name, wxString &out_name, wxString &error)
{
	UnzipArchive archive;
	const UnzipEntry *entry;
	const wxString sep = wxFileName::GetPathSeparator();
	int index;
	int r;

	r = unzip_open(&archive, archive_path.utf8_str());
	if (r != UNZIP_OK) {
		error = wxString::Format("The downloaded ROM archive could not be "
		                         "read: %s.", unzip_error_string(r));
		return false;
	}

	index = unzip_find(&archive, kRomMember);
	if (index < 0) {
		unzip_close(&archive);
		error = "The downloaded archive does not contain a ROM image "
		        "where one was expected.";
		return false;
	}
	entry = unzip_entry(&archive, index);

	/*
	 * Already have this exact image? Then use it where it is.
	 *
	 * Fetching twice is ordinary - choosing a ROM for a second machine, or
	 * adding a disc to one that already has its ROM - and each time would
	 * otherwise leave another numbered copy of the same four megabytes. The
	 * archive states the image's checksum, so identity is cheap to establish
	 * and does not depend on trusting the name.
	 */
	if (FileMatchesCrc(dest_dir + sep + base_name, entry->crc32,
	                   entry->uncompressed_size)) {
		rpclog("riscos_fetch: '%s' is already present and identical\n",
		       static_cast<const char *>(base_name.utf8_str()));
		unzip_close(&archive);
		out_name = base_name;
		return true;
	}

	out_name = UniqueName(base_name, RomNameTaken);
	if (out_name.empty()) {
		unzip_close(&archive);
		error = "Too many ROM images of that name already exist.";
		return false;
	}

	r = unzip_extract_to_file(&archive, entry, (dest_dir + sep + out_name).utf8_str());
	unzip_close(&archive);

	if (r != UNZIP_OK) {
		error = wxString::Format("The ROM image could not be unpacked: %s.",
		                         unzip_error_string(r));
		wxRemoveFile(dest_dir + sep + out_name);
		out_name.clear();
		return false;
	}

	return true;
}

/** Unpack HardDisc4 into a directory of its own, filetypes and all. */
int InstallDisc(const wxString &archive_path, const wxString &dest_dir,
                RiscosFetchReporter &reporter, wxString &error, bool &cancelled)
{
	UnzipExtractOptions options;
	UnpackContext context;
	UnzipArchive archive;
	int r;

	r = unzip_open(&archive, archive_path.utf8_str());
	if (r != UNZIP_OK) {
		error = wxString::Format("The downloaded hard disc archive could "
		                         "not be read: %s.", unzip_error_string(r));
		return -1;
	}

	context.reporter = &reporter;
	context.cancelled = false;

	memset(&options, 0, sizeof(options));
	options.strip_components = kHardDiscStrip;
	options.apply_filetypes = true;
	options.progress = UnpackProgress;
	options.progress_opaque = &context;

	r = unzip_extract_all(&archive, dest_dir.utf8_str(), &options);

	/*
	 * Their licence file, which the component strip above would otherwise
	 * discard. A courtesy rather than a requirement of the download, so a
	 * failure here is logged and does not sink the whole install.
	 */
	if (r >= 0) {
		const int index = unzip_find(&archive, kDiscLicenceMember);

		if (index < 0) {
			rpclog("riscos fetch: no '%s' in the hard disc archive\n",
			       kDiscLicenceMember);
		} else {
			const wxString target = dest_dir +
			    wxFileName::GetPathSeparator() + kDiscLicenceMember;
			const int lr = unzip_extract_to_file(&archive,
			    unzip_entry(&archive, index), target.utf8_str());

			if (lr != UNZIP_OK) {
				rpclog("riscos fetch: could not write %s: %s\n",
				       static_cast<const char *>(target.utf8_str()),
				       unzip_error_string(lr));
			}
		}
	}

	unzip_close(&archive);

	if (r < 0) {
		if (context.cancelled || r == UNZIP_ERR_CANCELLED) {
			cancelled = true;
		} else {
			error = wxString::Format("The hard disc could not be "
			                         "unpacked: %s.",
			                         unzip_error_string(r));
		}
		return -1;
	}

	return r;
}

/** Write the configuration for a machine built around a fetched ROM. */
bool WriteMachineConfig(const wxString &config_path, const wxString &name,
                        const wxString &rom_name)
{
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path,
	                      wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);

	ConfigFileUseGeneralGroup(settings);
	settings.Write("name", name);
	settings.Write("rom_dir", rom_name);
	settings.Write("model", "RPCSA");
	settings.Write("mem_size", "256");
	settings.Write("vram_size", "8");
	settings.Write("sound_enabled", 1L);
	settings.Write("refresh_rate", 60L);
	settings.Write("cdrom_enabled", 1L);
	settings.Write("network_type", "nat");
	settings.Write("cpu_idle", 0L);
	settings.Write("show_fullscreen_message", 1L);

	return settings.Flush();
}

}  /* namespace */

/* --------------------------------------------------------------------- */
/* Public interface                                                       */
/* --------------------------------------------------------------------- */

bool RiscosFetchHaveRom()
{
	const wxString roms_dir = ConfigPathsRomsDir();
	wxString name;

	if (!wxDirExists(roms_dir)) {
		return false;
	}

	wxDir dir(roms_dir);
	if (!dir.IsOpened() || !dir.GetFirst(&name)) {
		return false;
	}

	do {
		/* Anything that is not obviously a stray dotfile counts: this
		   only decides whether to offer the download, and the ROM
		   loader itself reports a genuinely unusable image far better
		   than a guess from the filename could. */
		if (!name.StartsWith(".")) {
			return true;
		}
	} while (dir.GetNext(&name));

	return false;
}

/*
 * The four paragraphs of the acknowledgement, kept separate because the
 * dialogue puts two of them above the licence box and two below, while the
 * command line prints them in order. Written once so the two cannot drift.
 */
namespace {

const char *const kLicenceOwnership =
    "RISC OS is copyright RISC OS Developments Ltd, and is developed and "
    "maintained by RISC OS Open Ltd. It is licensed under the Apache License, "
    "Version 2.0.";

const char *const kLicenceThanks =
    "Thank you to everyone at both companies, and to everyone who contributes "
    "to RISC OS, for the work they put in. RPCEmu emulates the hardware; the "
    "operating system is theirs.";

/*
 * Kept even though the licence itself is on screen in full: the ROM and disc
 * are licensed *primarily* under Apache 2.0, and HardDisc4 carries third-party
 * material that is not. Showing only the Apache text would overstate it.
 */
const char *const kLicenceThirdParty =
    "Some applications, logos and other material in these downloads come from "
    "third parties under their own licensing terms.";

const char *const kLicenceDonations =
    "RISC OS Open fund this work themselves, through donations.";

}  /* namespace */

/*
 * The same acknowledgement as plain text, for the command line. The licence
 * runs to 202 lines, which is not something to print into a terminal on every
 * invocation, so this gives the address of it instead.
 */
wxString RiscosFetchLicenceText()
{
	return wxString::Format(
	    "%s\n\n%s\n\n%s\n\n%s\n\n"
	    "Full licence:  %s\n"
	    "Other terms:   %s\n"
	    "Donations:     %s",
	    kLicenceOwnership, kLicenceThanks, kLicenceThirdParty,
	    kLicenceDonations, URL_APACHE_LICENCE, URL_ROOL_LICENCES,
	    URL_ROOL_DONATE);
}

bool RiscosFetchConfirmLicence(wxWindow *parent)
{
	/* Not "Downloading RISC OS": nothing is downloading yet, and that is the
	   progress window's title. */
	wxDialog dlg(parent, wxID_ANY, "Download RISC OS", wxDefaultPosition,
	             wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

	auto *heading = new wxStaticText(&dlg, wxID_ANY,
	    "Thank you to RISC OS Open and RISC OS Developments");
	wxFont bold = heading->GetFont();
	bold.SetWeight(wxFONTWEIGHT_BOLD);
	heading->SetFont(bold);

	/*
	 * The licence in full, read-only and unwrapped: it arrives already wrapped
	 * to the width its author chose (77 columns at the widest), so wrapping it
	 * again would ruin the numbered clauses and the appendix. The box is sized
	 * from the font actually in use rather than a pixel count, so 80 columns
	 * fit on every platform.
	 */
	auto *licence = new wxTextCtrl(&dlg, wxID_ANY, apache_licence_text,
	                              wxDefaultPosition, wxDefaultSize,
	                              wxTE_MULTILINE | wxTE_READONLY |
	                              wxTE_DONTWRAP | wxBORDER_THEME);
	/* wxSYS_ANSI_FIXED_FONT is not fixed-width on GTK, so ask for a teletype
	   family at the interface font's size instead. */
	licence->SetFont(wxFont(wxFontInfo(dlg.GetFont().GetPointSize())
	                            .Family(wxFONTFAMILY_TELETYPE)));

	const int column = licence->GetTextExtent("M").GetWidth();
	const int line = licence->GetCharHeight();
	const int wrap = column * 80 +
	    wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, &dlg) + 8;

	/* The few spare pixels stop the fourteenth line being sliced in half. */
	licence->SetMinSize(wxSize(wrap, line * 14 + 8));
	licence->SetInsertionPoint(0);	/* open at the top, not the end */

	/* Wrapped to the licence box, so every block of text lines up. */
	auto *ownership = new wxStaticText(&dlg, wxID_ANY, kLicenceOwnership);
	ownership->Wrap(wrap);
	auto *thanks = new wxStaticText(&dlg, wxID_ANY, kLicenceThanks);
	thanks->Wrap(wrap);
	auto *third_party = new wxStaticText(&dlg, wxID_ANY, kLicenceThirdParty);
	third_party->Wrap(wrap);
	auto *donations = new wxStaticText(&dlg, wxID_ANY, kLicenceDonations);
	donations->Wrap(wrap);

	auto *licences = new wxButton(&dlg, wxID_ANY, "Read the full terms");
	auto *donate = new wxButton(&dlg, wxID_ANY, "Donate to RISC OS Open");
	auto *agree = new wxButton(&dlg, wxID_OK, "I agree, download RISC OS");
	auto *cancel = new wxButton(&dlg, wxID_CANCEL, "Cancel");

	agree->SetDefault();
	licences->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
		wxLaunchDefaultBrowser(URL_ROOL_LICENCES);
	});
	donate->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
		wxLaunchDefaultBrowser(URL_ROOL_DONATE);
	});

	auto *links = new wxBoxSizer(wxHORIZONTAL);
	links->Add(licences, 0, wxRIGHT, 8);
	links->Add(donate, 0);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->AddStretchSpacer();
	buttons->Add(agree, 0, wxRIGHT, 8);
	buttons->Add(cancel, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, 12);
	main->Add(ownership, 0, wxLEFT | wxRIGHT | wxTOP, 12);
	main->Add(thanks, 0, wxLEFT | wxRIGHT | wxTOP, 12);
	/* The box is the only thing that should take up the slack when the
	   dialogue is resized. */
	main->Add(licence, 1, wxEXPAND | wxALL, 12);
	main->Add(third_party, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(donations, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(links, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
	dlg.SetSizerAndFit(main);
	dlg.CentreOnParent();

	return dlg.ShowModal() == wxID_OK;
}

RiscosFetchProgressReporter::RiscosFetchProgressReporter(wxWindow *parent)
    : dialog_("Setting up RISC OS", "Starting...", 1000, parent,
              wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_SMOOTH |
              wxPD_ELAPSED_TIME)
{
}

bool RiscosFetchProgressReporter::Stage(const wxString &description)
{
	stage_ = description;
	return dialog_.Update(0, description);
}

bool RiscosFetchProgressReporter::Progress(long long done, long long total)
{
	/* A total of zero means the server did not say how big the transfer is,
	   so there is nothing honest to draw: pulse rather than invent a
	   proportion. */
	if (total <= 0) {
		return dialog_.Pulse(stage_);
	}

	const int permille = static_cast<int>((done * 1000) / total);

	return dialog_.Update(permille < 1000 ? permille : 999, stage_);
}

bool RiscosFetchMachineDiscIsEmpty(const wxString &machine_name)
{
	if (machine_name.empty()) {
		return false;
	}

	return HostfsIsPristine(ConfigPathsMachinesDir() +
	                        wxFileName::GetPathSeparator() + machine_name +
	                        wxFileName::GetPathSeparator() + "hostfs");
}

long long RiscosFetchApproximateSize(const RiscosFetchRequest &request)
{
	return (request.include_rom ? kRomApproxBytes : 0) +
	       (request.include_disc ? kDiscApproxBytes : 0);
}

RiscosFetchOutcome RiscosFetchPerform(const RiscosFetchRequest &request,
                                      RiscosFetchReporter &reporter,
                                      RiscosFetchLoopFactory make_loop)
{
	RiscosFetchOutcome outcome;
	const wxString sep = wxFileName::GetPathSeparator();
	const wxString temp_dir = wxFileName::GetTempDir() + sep + "rpcemu-fetch";
	const wxString rom_zip = temp_dir + sep + "rom.zip";
	const wxString disc_zip = temp_dir + sep + "harddisc4.zip";
	wxString rom_url, rom_name, machine_name, config_path, staged_hostfs;
	wxString version, datestamp;

	if (!request.include_rom && !request.include_disc) {
		outcome.message = "Nothing was asked for.";
		return outcome;
	}

	if (!ConfigPathsEnsureDataLayout()) {
		outcome.message = "The RPCEmu data directory could not be created.";
		return outcome;
	}

	RemoveTree(temp_dir);
	if (!wxFileName::Mkdir(temp_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		outcome.message = "A temporary directory for the download could "
		                  "not be created.";
		return outcome;
	}

	/* --- Work out what to fetch --- */
	if (request.include_rom) {
		rom_url = ResolveRomUrl(request.release, reporter, make_loop);
		if (rom_url.empty()) {
			outcome.cancelled = true;
			RemoveTree(temp_dir);
			return outcome;
		}

		version = VersionFromUrl(rom_url);
		if (version.empty()) {
			version = (request.release == RiscosRelease::Nightly) ? "5.31" : "5.30";
		}
	}
	/* Fetching the disc on its own leaves this empty, which is what should
	   happen: the version here describes a ROM that is being downloaded, and
	   none is. The disc does not need it - its own boot sequence reads the
	   version from the running machine and sets itself up accordingly. */
	outcome.version = version;

	/* --- The ROM --- */
	if (request.include_rom) {
		Transfer transfer(reporter,
		                  wxString::Format("Fetching RISC OS %s...", version),
		                  make_loop);

		if (!transfer.ToFile(rom_url, rom_zip)) {
			outcome.cancelled = transfer.WasCancelled();
			outcome.message = transfer.Error();
			RemoveTree(temp_dir);
			return outcome;
		}
		datestamp = DateStampFromHeader(transfer.Header("Last-Modified"));
	}

	/* --- The hard disc --- */
	if (request.include_disc) {
		Transfer transfer(reporter, "Fetching the hard disc...", make_loop);

		if (!transfer.ToFile(kHardDiscUrl, disc_zip)) {
			outcome.cancelled = transfer.WasCancelled();
			outcome.message = transfer.Error();
			RemoveTree(temp_dir);
			return outcome;
		}
	}

	/* --- Install the ROM ---
	   Named for its version, and for a nightly also for the day it was
	   built, so successive nightlies sit alongside each other in the ROM
	   list rather than replacing one another. */
	if (request.include_rom) {
		wxString base = "ROM" + version;

		base.Replace(".", "");
		if (request.release == RiscosRelease::Nightly && !datestamp.empty()) {
			base += "-" + datestamp;
		}

		if (!reporter.Stage("Unpacking the ROM...") ||
		    !InstallRom(rom_zip, ConfigPathsRomsDir(), base, rom_name,
		                outcome.message)) {
			outcome.cancelled = outcome.message.empty();
			RemoveTree(temp_dir);
			return outcome;
		}
		outcome.rom_name = rom_name;
	}

	/* --- The machine ---
	   Either a new one built around the download, or an existing one the disc
	   is going onto. With neither, the ROM was the whole job. */
	if (request.create_machine) {
		machine_name = request.machine_name.empty()
		    ? wxString::Format("RISC OS %s", version)
		    : request.machine_name;
		machine_name = ConfigPathsSanitizeName(machine_name);
		machine_name = UniqueName(machine_name, MachineNameTaken);
		if (machine_name.empty()) {
			outcome.message = "Too many machines of that name already exist.";
			RemoveTree(temp_dir);
			return outcome;
		}

		config_path = ConfigPathsConfigsDir() + sep + machine_name + ".cfg";
		if (!WriteMachineConfig(config_path, machine_name, rom_name) ||
		    !ConfigPathsCreateMachineDirectory(machine_name)) {
			wxRemoveFile(config_path);
			outcome.message = "The new machine could not be created.";
			RemoveTree(temp_dir);
			return outcome;
		}

		outcome.machine_name = machine_name;
		outcome.config_path = config_path;
	} else if (request.include_disc) {
		machine_name = request.machine_name;
		if (machine_name.empty() ||
		    !wxDirExists(ConfigPathsMachinesDir() + sep + machine_name)) {
			outcome.message = "There is no machine to put the hard disc on.";
			RemoveTree(temp_dir);
			return outcome;
		}
		outcome.machine_name = machine_name;
	} else {
		RemoveTree(temp_dir);
		outcome.ok = true;
		return outcome;
	}

	/* --- The disc onto the machine ---
	   Unpacked beside the HostFS and moved into place only once it is
	   complete, so a cancelled unpack cannot leave a half-populated !Boot,
	   which would look like a broken machine rather than an absent one. */
	if (request.include_disc) {
		const wxString machine_dir = ConfigPathsMachinesDir() + sep +
		    machine_name;
		const wxString hostfs = machine_dir + sep + "hostfs";
		bool cancelled = false;
		int files;

		staged_hostfs = machine_dir + sep + "hostfs.incoming";
		RemoveTree(staged_hostfs);

		if (!reporter.Stage("Unpacking the hard disc...")) {
			outcome.cancelled = true;
			RemoveTree(temp_dir);
			return outcome;
		}

		files = InstallDisc(disc_zip, staged_hostfs, reporter,
		                    outcome.message, cancelled);
		if (files < 0) {
			RemoveTree(staged_hostfs);
			RemoveTree(temp_dir);
			outcome.cancelled = cancelled;
			return outcome;
		}

		/* Checked here as well as before the option was offered: the
		   download takes time, and a machine that was empty when it
		   started may not be by the time it finishes. A disc is never
		   worth overwriting on a guess. */
		if (!HostfsIsPristine(hostfs)) {
			RemoveTree(staged_hostfs);
			RemoveTree(temp_dir);
			outcome.message = wxString::Format(
			    "'%s' already has files on its hard disc, so it was "
			    "left alone. The ROM was installed.", machine_name);
			return outcome;
		}

		RemoveTree(hostfs);
		if (!wxRenameFile(staged_hostfs, hostfs)) {
			RemoveTree(staged_hostfs);
			RemoveTree(temp_dir);
			outcome.message = "The hard disc could not be moved into "
			                  "place.";
			return outcome;
		}

		outcome.disc_files = files;
	}

	RemoveTree(temp_dir);
	outcome.ok = true;
	return outcome;
}
