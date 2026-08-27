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

#include <wx/bitmap.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/frame.h>
#include <wx/icon.h>
#include <wx/image.h>

extern "C" {
#include "rpcemu.h"
}

wxString
AppLogoPath()
{
	const wxChar sep = wxFileName::GetPathSeparator();
	const wxString from_data =
	    wxString::FromUTF8(rpcemu_get_resourcedir()) + "resources" + sep + "rpcemu.png";

	if (wxFileExists(from_data)) {
		return from_data;
	}

	/*
	 * ★ The logo is part of the application, not part of anybody's data, so
	 * where the resource directory has none this looks in the application's own.
	 *
	 * The two are usually the same folder and this changes nothing. They are not
	 * the same when a data directory is named on the command line and holds a
	 * payload of its own: the resource directory becomes that folder (see
	 * ResourceDirForGivenDataDir), which has poduleroms/ and roms/ but no
	 * resources/rpcemu.png, because nothing ever puts one there. A machine
	 * started from a shortcut is exactly that case - the shortcut passes
	 * --datadir - so its About window showed the drawn placeholder while the
	 * Manager beside it, started with no --datadir, showed the logo.
	 *
	 * GetResourcesDir() is Contents/Resources inside a bundle and the
	 * executable's own directory anywhere else, which is where the payload sits
	 * in an ordinary install on all three platforms.
	 */
	const wxString app_resources =
	    wxFileName::DirName(wxStandardPaths::Get().GetResourcesDir()).GetFullPath();
	const wxString from_app = app_resources + "resources" + sep + "rpcemu.png";

	if (wxFileExists(from_app)) {
		return from_app;
	}

	/* Neither: answer the same thing as before, so a caller reporting what it
	   could not find names the place it is meant to be. */
	return from_data;
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
