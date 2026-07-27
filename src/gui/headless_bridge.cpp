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

#include "headless_bridge.h"

#include <cstdio>
#include <cstdlib>

#include "rpcemu.h"

/*
 * Video is delivered to VNC clients straight from rpcemu_video_update(); the
 * bridge has nothing to draw. Likewise there is no host cursor to move, no NAT
 * dialog to update, no debugger UI, and no window title to retitle.
 */
void HeadlessBridge::PostVideoUpdate(VideoUpdate /*update*/)
{
}

void HeadlessBridge::PostMoveHostMouse(const MouseMoveUpdate & /*update*/)
{
}

void HeadlessBridge::PostNatRule(PortForwardRule /*rule*/)
{
}

void HeadlessBridge::PostDebuggerStateChanged()
{
}

void HeadlessBridge::PostMachineSwitched(const std::string & /*machine_name*/)
{
}

/*
 * The guest asked to power off. There is no window to close; drop the emulator
 * out of its run loop so the headless process exits cleanly.
 */
void HeadlessBridge::PostQuit()
{
	quited = 1;
}

/*
 * A headless machine has no host clipboard to put anything on, so this is where
 * guest copies go: nowhere.
 */
void HeadlessBridge::PostSetHostClipboard(const std::string &utf8)
{
	(void) utf8;
}

/*
 * error()/fatal() in emulator_host.cpp already print the message to stderr
 * before calling the bridge, so the non-fatal hooks need only avoid crashing.
 */
void HeadlessBridge::PostError(const std::string & /*message*/)
{
}

void HeadlessBridge::ShowError(const std::string & /*message*/)
{
}

/*
 * fatal() invokes one of these and then spins forever expecting the GUI to put
 * up a dialog. In headless mode there is no UI, so terminate the process
 * instead of hanging.
 */
void HeadlessBridge::PostFatal(const std::string & /*message*/)
{
	fflush(stdout);
	fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void HeadlessBridge::ShowFatal(const std::string &message)
{
	PostFatal(message);
}
