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

/* Choosing a machine over VNC when there is no GUI. See headless_selector.cpp. */

#ifndef HEADLESS_SELECTOR_H
#define HEADLESS_SELECTOR_H

#include <string>
#include <vector>

/**
 * Show the machine list over VNC and wait for a choice.
 *
 * Blocks until the user picks one or gives up. The VNC server is started on the
 * app settings' port and stopped again before returning, so the machine that
 * follows can bind the same port.
 *
 * @param names Machine names to offer, in the order to show them.
 * @return the chosen name, or an empty string if the user cancelled or the server
 *         could not be started.
 */
extern std::string HeadlessChooseMachine(const std::vector<std::string> &names);

#endif /* HEADLESS_SELECTOR_H */
