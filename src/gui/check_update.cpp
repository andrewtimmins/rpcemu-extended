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

#include "check_update.h"

#include <cstdlib>

#include <wx/datetime.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/utils.h>

#include "gui_preferences.h"
#include "http_transfer.h"	/* RPCEMU_HAVE_HTTP, HttpUnavailableMessage */
#include "riscos_fetch.h"	/* RiscosFetchProgressReporter */

extern "C" {
#include "rpcemu.h"
}

namespace {

/* "v1.1.12" -> 1, 1, 12. Anything unparsable leaves the fields at zero, which
   compares as older than any real release and so offers the update. */
void SplitVersion(const wxString &text, long out[3])
{
	wxString rest = text;

	out[0] = out[1] = out[2] = 0;
	if (rest.StartsWith("v")) {
		rest = rest.Mid(1);
	}
	for (int i = 0; i < 3 && !rest.empty(); i++) {
		wxString field = rest.BeforeFirst('.');

		field.ToLong(&out[i]);
		rest = rest.AfterFirst('.');
	}
}

/* Numerically, so 1.1.9 is older than 1.1.10 rather than sorting after it. */
bool IsNewerVersion(const wxString &candidate, const wxString &current)
{
	long a[3], b[3];

	SplitVersion(candidate, a);
	SplitVersion(current, b);
	for (int i = 0; i < 3; i++) {
		if (a[i] != b[i]) {
			return a[i] > b[i];
		}
	}
	return false;
}

/* The one field wanted out of the release JSON. The response has several
   html_url members - the release, the uploader, others - so the tag is taken
   and the page built from it rather than trusting their order. */
wxString TagNameFromReleaseJson(const wxString &json)
{
	const wxString key = "\"tag_name\"";
	const int at = json.Find(key);

	if (at == wxNOT_FOUND) {
		return wxString();
	}

	wxString rest = json.Mid(static_cast<size_t>(at) + key.length());
	rest = rest.AfterFirst(':').Strip(wxString::both);
	if (!rest.StartsWith("\"")) {
		return wxString();
	}
	return rest.Mid(1).BeforeFirst('"');
}
/* Nothing on screen: the background check is not allowed to interrupt, and it
   has nothing to offer a Cancel button either. */
class SilentReporter : public RiscosFetchReporter {
public:
	bool Stage(const wxString &) override { return true; }
	bool Progress(long long, long long) override { return true; }
};

} // namespace

void CheckForUpdateInBackground(wxWindow *parent)
{
	const bool testing = getenv("RPCEMU_UPDATE_TEST") != nullptr;
	const wxString current = testing ? wxString("1.0.0") : wxString(VERSION);

	if (!GetCheckForUpdates() || !RPCEMU_HAVE_HTTP) {
		return;
	}
	if (!testing && current.Contains("-")) {
		return;
	}

	const long long now = wxDateTime::Now().GetTicks();
	const long long last = GetLastUpdateCheck();
	const long long a_day = 24 * 60 * 60;

	/* A stored time in the future is deliberate - see the postponement below -
	   but cap it, so a clock that jumped years ahead and back does not silence
	   this for ever. */
	if (last > now + 7 * a_day) {
		SetLastUpdateCheck(0);
	} else if (last > now) {
		return;		/* postponed, and still is */
	} else if (!testing && last != 0 && now - last < a_day) {
		return;
	}

	/* Recorded before asking rather than after: a GitHub that cannot be reached
	   should not mean trying again at every launch. */
	SetLastUpdateCheck(now);

	wxString body;
	{
		SilentReporter reporter;
		Transfer transfer(reporter, wxString(), {});

		if (!transfer.ToMemory(URL_LATEST_RELEASE_API)) {
			return;
		}
		body = transfer.Body();
	}

	const wxString tag = TagNameFromReleaseJson(body);

	if (tag.empty() || !IsNewerVersion(tag, current)) {
		return;
	}

	wxRichMessageDialog dlg(parent,
	    wxString::Format("RPCEmu Extended %s is available.", tag),
	    "RPCEmu Extended - Update Available",
	    wxOK | wxCANCEL | wxICON_INFORMATION);

	dlg.SetExtendedMessage(wxString::Format(
	    "You are running v%s.\n\n"
	    "Open the release page to read the notes and download it?", current));
	dlg.SetOKCancelLabels("Open Release Page", "Remind Me in a Week");

	if (dlg.ShowModal() == wxID_OK) {
		wxLaunchDefaultBrowser(wxString(URL_RELEASE_TAG) + tag);
		return;
	}

	/* Dated into the future rather than kept as a second preference: the gate
	   above finds it has not been long enough, six more times. */
	SetLastUpdateCheck(now + 6 * a_day);
}

void CheckForUpdate(wxWindow *parent)
{
	const wxString current = VERSION;

	/* A development build is ahead of the newest release, not behind it, so
	   there is nothing useful to compare against. */
	if (current.Contains("-")) {
		if (wxMessageBox(
		        wxString::Format(
		            "This is a development build (%s), not a release.\n\n"
		            "Open the releases page?", current),
		        "RPCEmu Extended - Check for Update",
		        wxOK | wxCANCEL | wxICON_INFORMATION, parent) == wxOK) {
			wxLaunchDefaultBrowser(URL_RELEASES);
		}
		return;
	}

	if (!RPCEMU_HAVE_HTTP) {
		wxMessageBox(HttpUnavailableMessage(), "RPCEmu Extended - Check for Update",
		             wxOK | wxICON_INFORMATION, parent);
		return;
	}

	wxString body, error;
	{
		RiscosFetchProgressReporter reporter(parent);
		Transfer transfer(reporter, "Checking for a newer release...", {});

		if (!transfer.ToMemory(URL_LATEST_RELEASE_API)) {
			if (transfer.WasCancelled()) {
				return;
			}
			error = transfer.Error();
		} else {
			body = transfer.Body();
		}
	}

	if (!error.empty()) {
		wxMessageBox(wxString::Format("Could not check for an update:\n\n%s", error),
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_ERROR, parent);
		return;
	}

	const wxString tag = TagNameFromReleaseJson(body);
	if (tag.empty()) {
		wxMessageBox("The reply from GitHub did not name a release.",
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_ERROR, parent);
		return;
	}

	if (!IsNewerVersion(tag, current)) {
		wxMessageBox(wxString::Format("You are running the latest version (v%s).", current),
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_INFORMATION, parent);
		return;
	}

	if (wxMessageBox(
	        wxString::Format(
	            "RPCEmu Extended %s is available. You are running v%s.\n\n"
	            "Open the release page to read the notes and download it?",
	            tag, current),
	        "RPCEmu Extended - Check for Update",
	        wxOK | wxCANCEL | wxICON_INFORMATION, parent) == wxOK) {
		wxLaunchDefaultBrowser(wxString(URL_RELEASE_TAG) + tag);
	}
}
