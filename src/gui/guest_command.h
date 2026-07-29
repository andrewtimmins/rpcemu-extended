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
 * guest_command.h - run a RISC OS command from the interface and read the reply.
 *
 * The emulator can ask the running machine things: whether a module is present,
 * running a package's install script, telling the Filer to re-read a directory.
 * The channel is HostCmd, the same one rpcemu-run uses, reached internally.
 *
 * Only the emulator thread may go near hostcmd, so a request goes through the
 * emulator's command queue and the reply comes back as a bridge event. This wraps
 * that round trip up as a call that returns an answer.
 */

#ifndef GUEST_COMMAND_H
#define GUEST_COMMAND_H

#include <string>

#include <wx/string.h>

#include "riscos_fetch.h"	/* RiscosFetchLoopFactory */

/** What a command did. */
struct GuestCommandResult {
	bool ran = false;	/* it reached the guest and finished */
	bool timed_out = false;
	int rc = 0;		/* RISC OS's return code; 0 is success */
	wxString output;	/* everything it printed */
	wxString error;		/* why it did not run, for the user */

	/** True if it ran and RISC OS reported success. */
	bool Succeeded() const { return ran && rc == 0; }
};

/**
 * Run @command in the guest and wait for it.
 *
 * Waits by running a nested event loop, as the downloads do, so the interface
 * stays alive and the emulator keeps running. Never waits indefinitely: a guest
 * that is wedged, or has no support module, times out.
 *
 * @param command    A RISC OS command line, without a leading '*'.
 * @param make_loop  Where the nested loop comes from (CreateMainLoop()).
 * @param timeout_ms How long to wait. The default suits a * command; give a
 *                   package's install script longer.
 */
GuestCommandResult GuestCommandRun(const wxString &command,
                                   const RiscosFetchLoopFactory &make_loop,
                                   int timeout_ms = 5000);

/**
 * True if the guest could run a command now: a machine is up, its support module
 * has announced itself, and the channel is not already busy.
 *
 * Worth asking before offering something that depends on it, so a menu item can
 * explain itself rather than failing later.
 */
bool GuestCommandAvailable();

/** Called by the bridge when a reply arrives. Not for general use. */
void GuestCommandDeliver(unsigned token, unsigned rc, const std::string &output,
                         bool ok);

#endif /* GUEST_COMMAND_H */
