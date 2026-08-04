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

#ifndef DATA_DIR_DIALOG_H
#define DATA_DIR_DIALOG_H

#include <wx/string.h>

class wxWindow;

/**
 * Ask, on a first run, where RPCEmu should keep its data.
 *
 * Answers the complaint in issue #67: a folder appeared in the user's home
 * directory without their being told and with no way to move it. The field is
 * prefilled with @suggested, which is where it would have gone anyway, so
 * agreeing is one click and behaves exactly as every previous version did.
 *
 * @param parent    Parent window, or nullptr. There is no main window yet when
 *                  this runs, so nullptr is the normal case.
 * @param suggested The default location, offered rather than imposed.
 * @param chosen    Set to the directory the user settled on.
 * @return true if a directory was chosen, false to carry on with @suggested.
 */
bool AskForDataDir(wxWindow *parent, const wxString &suggested, wxString *chosen);

#endif /* DATA_DIR_DIALOG_H */
