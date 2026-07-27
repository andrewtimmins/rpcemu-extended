/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef GUI_BRIDGE_H
#define GUI_BRIDGE_H

#include <string>

extern "C" {
#include "rpcemu.h"
}

#include "host_types.h"

class GuiBridge {
public:
	virtual ~GuiBridge() = default;

	virtual bool IsGuiThread() const = 0;
	virtual void PostVideoUpdate(VideoUpdate update) = 0;
	virtual void PostError(const std::string &message) = 0;
	virtual void PostFatal(const std::string &message) = 0;
	virtual void PostMoveHostMouse(const MouseMoveUpdate &update) = 0;

	virtual void ShowError(const std::string &message) = 0;
	virtual void ShowFatal(const std::string &message) = 0;

	virtual void PostNatRule(PortForwardRule rule) = 0;
	virtual void PostDebuggerStateChanged() = 0;
	virtual void PostMachineSwitched(const std::string &machine_name) = 0;

	/* The core is asking the application to quit (e.g. guest soft power-off). */
	virtual void PostQuit() = 0;

	/* The guest has put text on the clipboard; put it on the host's. */
	virtual void PostSetHostClipboard(const std::string &utf8) = 0;
};

#endif
