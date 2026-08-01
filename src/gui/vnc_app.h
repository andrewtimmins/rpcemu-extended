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
 * vnc_app - one VNC server for the life of the process.
 *
 * The server used to be owned by whatever was running a machine: MainFrame in the
 * GUI, a local unique_ptr in headless. That made it impossible for anything to be
 * on the display before a machine existed, and it meant a client was disconnected
 * whenever a machine started or stopped.
 *
 * It is now owned here, started once from the app settings, and pointed at a
 * machine as machines come and go. Between machines it stays up with nothing
 * attached, so a client can be shown something the emulator draws itself - the
 * machine selector today, and whatever overlays come later - without the
 * connection dropping.
 *
 * That the settings are the emulator's rather than a machine's is what makes this
 * possible at all; see src/app_settings.c.
 */

#ifndef VNC_APP_H
#define VNC_APP_H

#include <string>

class EmulatorHost;
class VncServer;

/**
 * The process-wide server, created on first use.
 *
 * Never null. Not started until VncAppStart() is called, and a stopped server
 * ignores everything, so callers do not need to care which.
 */
extern VncServer &VncAppServer();

/**
 * Start listening, using the app settings for the port and password.
 *
 * @param force Start even when the settings say VNC is disabled, which is what
 *              --headless wants: without a window there is no other way in.
 * @return true if listening, or already listening.
 */
extern bool VncAppStart(bool force);

/** Stop listening and forget any attached machine. */
extern void VncAppStop();

/** Is the server listening? */
extern bool VncAppRunning();

/**
 * Point the server at a machine, or at nothing when it goes away.
 *
 * Detaching leaves a connected client able to look but not type, which is the
 * correct state between machines.
 */
extern void VncAppAttach(EmulatorHost *host);
extern void VncAppDetach();

/** The port actually being listened on, or 0 if not running. */
extern int VncAppPort();

#endif /* VNC_APP_H */
