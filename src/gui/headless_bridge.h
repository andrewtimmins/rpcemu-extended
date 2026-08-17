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

#ifndef HEADLESS_BRIDGE_H
#define HEADLESS_BRIDGE_H

#include "gui_bridge.h"

/*
 * GuiBridge implementation for --headless mode.
 *
 * There is no wxWidgets window and no GUI event loop. Video reaches clients
 * through the built-in VNC server, which is fed directly from
 * rpcemu_video_update() (independently of the bridge), so the video and mouse
 * callbacks here are deliberate no-ops. Errors are reported on stderr; a fatal
 * error terminates the process (there is no dialog to show and nobody to
 * dismiss it).
 */
class HeadlessBridge : public GuiBridge {
public:
	bool IsGuiThread() const override { return false; }

	void PostVideoUpdate(VideoUpdate update) override;
	void PostError(const std::string &message) override;
	void PostFatal(const std::string &message) override;
	void PostMoveHostMouse(const MouseMoveUpdate &update) override;
	void PostPointerShape(const PointerShape &shape) override;

	void ShowError(const std::string &message) override;
	void ShowFatal(const std::string &message) override;

	void PostDebuggerStateChanged() override;
	void PostMachineSwitched(const std::string &machine_name) override;
	void PostGuestCommandResult(unsigned token, unsigned rc,
	                            const std::string &output, bool ok) override;
	void PostQuit() override;
	void PostSetHostClipboard(const std::string &utf8) override;
	void PostSetHostClipboardImage(int file_type, const std::string &bytes) override;
};

#endif
