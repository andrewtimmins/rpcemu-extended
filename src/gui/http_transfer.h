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
 * http_transfer.h - one HTTP GET, waited for.
 *
 * Lifted out of riscos_fetch.cpp when the package manager needed the same
 * thing. Header-only because it is a single small class and both users are in
 * the GUI; there is no third caller to justify a translation unit.
 */

#ifndef HTTP_TRANSFER_H
#define HTTP_TRANSFER_H

#include <map>
#include <memory>

#include <wx/event.h>
#include <wx/eventfilter.h>
#include <wx/evtloop.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/webrequest.h>

#include "riscos_fetch.h"	/* RiscosFetchReporter, RiscosFetchLoopFactory */

extern "C" {
#include "rpcemu.h"		/* VERSION */
}

/**
 * Whether this build can fetch anything over HTTP at all.
 *
 * wxWebRequest is what every download here is built on, and it is not always
 * present: wxWidgets sets wxUSE_WEBREQUEST to 0 when its own configure finds no
 * backend for it, and ships that way on some distributions - Debian 12 bookworm
 * among them, which is what Raspberry Pi OS is built on. The header then exists
 * and declares nothing.
 *
 * Rather than refuse to build, RPCEmu builds without the two features that need
 * it: the package manager and downloading RISC OS. Everything else is
 * unaffected, and each entry point says what is missing and why instead of
 * failing obscurely. Testing wxUSE_WEBREQUEST directly rather than adding a
 * build option of our own means there is nothing for anybody to set wrongly:
 * a tree either has the support or it does not.
 */
#if wxUSE_WEBREQUEST
#define RPCEMU_HAVE_HTTP 1
#else
#define RPCEMU_HAVE_HTTP 0
#endif

/**
 * One sentence to show wherever a download would have been offered.
 *
 * Kept in one place so the dialogue, the menu and the command line all say the
 * same thing, and so it can be corrected once.
 */
inline wxString HttpUnavailableMessage()
{
	return "This build of RPCEmu cannot download anything: the wxWidgets it was "
	       "built against has no wxWebRequest support, which the package "
	       "manager and the RISC OS download both need.\n\n"
	       "Everything else works normally. To get these features, build "
	       "against a wxWidgets with a web request backend - see the "
	       "wxWidgets and wxWebRequest section of COMPILE.md.";
}

/**
 * RISC OS Open asked to be able to identify the emulator's own traffic, so
 * every request carries this. It is not a licence check or a tracker, just a
 * courtesy to somebody hosting the files for us. The package indexes are on the
 * same server, so they carry it too.
 */
inline wxString HttpRequestedByHeader()
{
	return wxString::Format("RPCEmu Spork Edition/%s", VERSION);
}

#if !RPCEMU_HAVE_HTTP

/**
 * A Transfer that cannot transfer.
 *
 * Same surface as the real one below, so every caller compiles and links
 * unchanged; each attempt fails at once with the explanation above. The callers
 * are all written to report Error() to the user, so this arrives as a sentence
 * rather than as silence.
 */
class Transfer {
public:
	Transfer(RiscosFetchReporter &reporter, const wxString &stage,
	         const RiscosFetchLoopFactory &make_loop)
	    : error_(HttpUnavailableMessage())
	{
		(void) reporter;
		(void) stage;
		(void) make_loop;
	}

	bool ToMemory(const wxString &url) { (void) url; return false; }

	bool ToFile(const wxString &url, const wxString &dest_path)
	{
		(void) url;
		(void) dest_path;
		return false;
	}

	const wxString &Body() const { return body_; }
	const wxString &Error() const { return error_; }
	bool WasCancelled() const { return false; }
	wxString Header(const wxString &name) const { (void) name; return wxString(); }

private:
	wxString body_;
	wxString error_;
};

#else

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

		request_.SetHeader("X-Requested-By", HttpRequestedByHeader());
		request_.SetHeader("User-Agent", HttpRequestedByHeader());
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

#endif /* RPCEMU_HAVE_HTTP */

#endif /* HTTP_TRANSFER_H */
