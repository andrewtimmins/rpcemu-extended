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
 * vnc_selector_session - offer the machine list to a VNC client while the GUI is
 * showing its own selector.
 *
 * The GUI's selector is a modal wxWidgets dialogue, so a remote client could see
 * nothing at all until a machine had already been chosen locally. Now the same
 * list is drawn on the VNC display for as long as that dialogue is open, and
 * whichever chooses first wins: a remote Enter ends the local dialogue, and a
 * local choice stops the remote list.
 *
 * Scoped: construct it before showing the dialogue, destroy it after. It attaches
 * to the process-wide server (see vnc_app.h) rather than starting one of its own,
 * so the client that picks a machine keeps its connection into that machine.
 */

#ifndef VNC_SELECTOR_SESSION_H
#define VNC_SELECTOR_SESSION_H

#include <string>
#include <vector>

class wxDialog;

class VncSelectorSession {
public:
	/**
	 * Start serving the list.
	 *
	 * @param names   Machines to offer.
	 * @param dialog  The local dialogue to end if a remote client chooses; may be
	 *                null, in which case a remote choice is simply recorded.
	 */
	VncSelectorSession(const std::vector<std::string> &names, wxDialog *dialog);
	~VncSelectorSession();

	VncSelectorSession(const VncSelectorSession &) = delete;
	VncSelectorSession &operator=(const VncSelectorSession &) = delete;

	/** Did a VNC client choose a machine, and which? Empty if not. */
	std::string chosen() const;

	/** Is anything actually being served? False if VNC is off or would not start. */
	bool active() const { return active_; }

private:
	bool active_ = false;
};

#endif /* VNC_SELECTOR_SESSION_H */
