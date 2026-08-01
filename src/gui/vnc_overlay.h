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
 * vnc_overlay - a control menu for whoever is at the far end of a VNC connection.
 *
 * A local user has menus, a toolbar and a keyboard shortcut for everything. A VNC
 * user had the screen and nothing else: no way to reset a wedged machine, no way
 * to do anything but type at the guest. That is a poor deal for a session someone
 * may be running entirely remotely, and it applies to a desktop session someone
 * has walked away from and reconnected to, not only to headless.
 *
 * So: a menu the client can call up over the running machine. It is drawn by the
 * emulator into the VNC framebuffer, so nothing about the local window changes and
 * a local user does not see it.
 *
 * Ctrl+Alt+Shift+M opens and closes it. Three modifiers because the guest must
 * never lose a key combination it wanted: RISC OS uses Alt heavily, and anything
 * shorter risks taking something real away from it. While the menu is closed every
 * key is passed straight through, and only that one combination is watched for.
 */

#ifndef VNC_OVERLAY_H
#define VNC_OVERLAY_H

class EmulatorHost;

/**
 * Start watching for the shortcut, acting on @p host when something is chosen.
 *
 * Installs the server's keysym hook, so call it after a machine is attached.
 * Passing nullptr stops the overlay and restores plain pass-through.
 */
extern void VncOverlayAttach(EmulatorHost *host);

/** Is the menu currently on screen? */
extern bool VncOverlayVisible();

#endif /* VNC_OVERLAY_H */
