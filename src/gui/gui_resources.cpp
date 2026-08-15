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

#include "gui_resources.h"

#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/icon.h>
#include <wx/image.h>

extern "C" {
#include "rpcemu.h"
}

wxString
AppLogoPath()
{
	return wxString::FromUTF8(rpcemu_get_resourcedir()) + "resources" +
	    wxFileName::GetPathSeparator() + "rpcemu.png";
}

/*
 * On Windows the .exe file icon is a different thing, compiled in from the .ico
 * - see cmake/FindWxWidgets.cmake. This is the one the window and the taskbar
 * show.
 */
void
SetFrameIcon(wxFrame *frame)
{
	const wxString path = AppLogoPath();
	wxImage image;

	if (frame == nullptr || !wxFileExists(path) ||
	    !image.LoadFile(path, wxBITMAP_TYPE_PNG)) {
		return;
	}

	wxIcon icon;

	icon.CopyFromBitmap(wxBitmap(image));
	frame->SetIcon(icon);
}
