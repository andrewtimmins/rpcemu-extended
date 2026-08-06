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
 * help_actions.h - the Help menu items that need no machine.
 *
 * On macOS there are two menu bars: the machine window's, and the one the
 * application owns for when no window is open. Both offer these, so they live
 * here rather than on the frame and there is one implementation of each.
 *
 * @parent is the window to hang a dialogue off, or null when there is none.
 */

#ifndef HELP_ACTIONS_H
#define HELP_ACTIONS_H

class wxWindow;

void HelpShowAbout(wxWindow *parent);
void HelpShowAboutRiscos(wxWindow *parent);
void HelpOpenOnlineManual(wxWindow *parent);
void HelpOpenWebsite(wxWindow *parent);
void HelpReportIssue(wxWindow *parent);
void HelpCheckForUpdate(wxWindow *parent);

#endif /* HELP_ACTIONS_H */
