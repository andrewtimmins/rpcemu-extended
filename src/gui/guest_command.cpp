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
 * guest_command.cpp - the interface's side of the round trip.
 *
 * Request out through the emulator's command queue, reply back through the bridge
 * as a CallAfter on this thread, so the two ends never touch each other's data.
 * The token is what pairs them up, and what lets a reply that arrives after the
 * caller gave up be dropped instead of written into a dead structure.
 */

#include <map>
#include <memory>

#include <wx/evtloop.h>
#include <wx/timer.h>

#include "emulator_host.h"
#include "guest_command.h"

extern "C" {
#include "hostcmd.h"
#include "rpcemu.h"
}

namespace {

/* One entry per caller waiting for a reply. A caller that times out removes its
   own entry, so a late reply finds nothing and is discarded. */
struct Waiting {
	bool finished = false;
	unsigned rc = 0;
	std::string output;
	bool ok = false;
};

std::map<unsigned, Waiting *> g_waiting;
unsigned g_next_token = 1;

}  /* namespace */

void GuestCommandDeliver(unsigned token, unsigned rc, const std::string &output,
                         bool ok)
{
	const auto it = g_waiting.find(token);

	if (it == g_waiting.end()) {
		/* Nobody is waiting: the caller timed out and went away. Not an error,
		   and there is nothing useful to do with the output. */
		return;
	}

	it->second->rc = rc;
	it->second->output = output;
	it->second->ok = ok;
	it->second->finished = true;
}

bool GuestCommandAvailable()
{
	return emulator_host_instance() != nullptr && hostcmd_internal_ready() &&
	       !hostcmd_internal_busy();
}

GuestCommandResult GuestCommandRun(const wxString &command,
                                   const RiscosFetchLoopFactory &make_loop,
                                   int timeout_ms)
{
	GuestCommandResult result;
	EmulatorHost *host = emulator_host_instance();
	Waiting waiting;
	const unsigned token = g_next_token++;

	if (host == nullptr) {
		result.error = "No machine is running.";
		return result;
	}
	if (!hostcmd_internal_ready()) {
		/* Either the support module is not in this machine's expansion ROM, or
		   the channel is switched off. Both are worth saying plainly: the
		   feature that needed this is not broken, it is unavailable. */
		result.error = "This machine's RPCEmu support module has not started, so "
		               "the emulator cannot ask RISC OS to do anything. Check "
		               "that HostCmd is enabled for this machine.";
		return result;
	}

	g_waiting[token] = &waiting;
	host->RunGuestCommand(std::string(command.utf8_str()), token);

	/* Wait by running a nested loop, as the downloads do: the emulator keeps
	   running, the window keeps painting, and the reply arrives as a CallAfter
	   on this thread. A timer ends the wait whatever happens, so a guest that
	   never answers costs a few seconds rather than the session. */
	{
		std::unique_ptr<wxEventLoopBase> loop(make_loop());
		wxTimer deadline;
		bool expired = false;

		deadline.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
			expired = true;
		});
		deadline.StartOnce(timeout_ms);

		while (!waiting.finished && !expired) {
			if (loop != nullptr) {
				loop->YieldFor(wxEVT_CATEGORY_ALL);
			}
			wxMilliSleep(2);
		}
	}

	g_waiting.erase(token);

	if (!waiting.finished) {
		/* Let the channel go, on the thread that owns it, or a guest that
		   never answers would block every later command. */
		host->AbandonGuestCommand();
		result.timed_out = true;
		result.error = wxString::Format(
		    "RISC OS did not finish '%s' within %d seconds.", command,
		    timeout_ms / 1000);
		return result;
	}
	if (!waiting.ok) {
		result.error = wxString::Format(
		    "RISC OS could not be asked to run '%s'; the command channel was "
		    "busy.", command);
		return result;
	}

	result.ran = true;
	result.rc = static_cast<int>(waiting.rc);
	result.output = wxString::FromUTF8(waiting.output.c_str(),
	                                   waiting.output.size());
	if (result.output.empty() && !waiting.output.empty()) {
		/* RISC OS text is Latin-1, and a byte above 127 makes it invalid
		   UTF-8; fall back rather than losing the output. */
		result.output = wxString(waiting.output.c_str(), wxConvISO8859_1,
		                         waiting.output.size());
	}

	rpclog("guest command: '%s' returned %d, %zu byte(s) of output\n",
	       static_cast<const char *>(command.utf8_str()), result.rc,
	       waiting.output.size());

	return result;
}
