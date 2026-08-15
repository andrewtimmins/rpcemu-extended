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

#include "machine_shortcut.h"

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/textfile.h>
#include <wx/utils.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>	/* IShellLink, IPersistFile */
#endif

/*
 * A shortcut that opens one machine, without the manager.
 *
 * Written where the user says, and a snapshot of how things are now: it names
 * the binary and the machine by their current paths, so moving either means
 * making the shortcut again.
 *
 * wxWidgets has nothing for this - a shortcut is whatever the desktop it is
 * dropped on understands - so there is a writer per platform.
 */
#ifdef _WIN32
/*
 * A .lnk, through the shell's own COM interfaces. The arguments are a separate
 * field rather than part of the path, which is why this cannot be a text file.
 */
bool WriteShortcut(const wxString &path, const wxString &exe,
                          const wxString &args, const wxString &working_dir,
                          const wxString &description)
{
	IShellLinkW *link = nullptr;
	bool ok = false;

	/* Apartment threaded, and tolerant of somebody having already done it:
	   RPC_E_CHANGED_MODE means the process is initialised the other way, which
	   is still usable here. */
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool uninit = SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;

	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
	        IID_IShellLinkW, (void **) &link))) {
		IPersistFile *file = nullptr;

		link->SetPath(exe.wc_str());
		link->SetArguments(args.wc_str());
		link->SetWorkingDirectory(working_dir.wc_str());
		link->SetDescription(description.wc_str());
		/* The emulator's own icon, from the binary itself. */
		link->SetIconLocation(exe.wc_str(), 0);

		if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void **) &file))) {
			ok = SUCCEEDED(file->Save(path.wc_str(), TRUE));
			file->Release();
		}
		link->Release();
	}

	if (uninit) {
		CoUninitialize();
	}
	return ok;
}
#elif defined(__WXOSX__)
/*
 * A .command file, run in Terminal: macOS has no shortcut that carries
 * arguments. Through "open" rather than the binary inside the bundle, which
 * would bypass the icon and the dock; -n for a new instance, without which the
 * arguments are dropped; --args last, as it must be.
 */
bool WriteShortcut(const wxString &path, const wxString &exe,
                          const wxString &args, const wxString &working_dir,
                          const wxString &description)
{
	wxFFile file(path, "wb");

	if (!file.IsOpened()) {
		return false;
	}

	/* Contents/MacOS/<binary> back up to the .app itself. */
	wxFileName bundle(exe);

	bundle.RemoveLastDir();
	bundle.RemoveLastDir();

	const wxString app = bundle.GetPath();
	wxString text;

	text << "#!/bin/sh\n"
	     << "# " << description << "\n"
	     << "open -n -a \"" << app << "\" --args " << args << "\n";

	if (!file.Write(text) || !file.Close()) {
		return false;
	}

	wxFileName(path).SetPermissions(wxPOSIX_USER_READ | wxPOSIX_USER_WRITE |
	    wxPOSIX_USER_EXECUTE | wxPOSIX_GROUP_READ | wxPOSIX_GROUP_EXECUTE |
	    wxPOSIX_OTHERS_READ | wxPOSIX_OTHERS_EXECUTE);
	return true;
}
#else
/*
 * A .desktop file, as packaging/rpcemu.desktop is. Executable, because a
 * launcher that is not is shown as a text file to be opened rather than run.
 */
bool WriteShortcut(const wxString &path, const wxString &exe,
                          const wxString &args, const wxString &working_dir,
                          const wxString &description)
{
	wxFFile file(path, "wb");

	if (!file.IsOpened()) {
		return false;
	}

	wxString text;

	text << "[Desktop Entry]\n"
	     << "Type=Application\n"
	     << "Name=" << description << "\n"
	     << "Comment=Risc PC and A7000 emulator\n"
	     << "Exec=\"" << exe << "\" " << args << "\n"
	     << "Path=" << working_dir << "\n"
	     << "Icon=rpcemu\n"
	     << "Terminal=false\n"
	     << "Categories=Game;Emulator;\n"
	     << "Keywords=RISC OS;Acorn;RiscPC;\n";

	if (!file.Write(text) || !file.Close()) {
		return false;
	}

	/* 0755. A .desktop file on the desktop is only offered as a launcher when
	   it can be executed; without this the file manager shows it as text. */
	wxFileName(path).SetPermissions(wxPOSIX_USER_READ | wxPOSIX_USER_WRITE |
	    wxPOSIX_USER_EXECUTE | wxPOSIX_GROUP_READ | wxPOSIX_GROUP_EXECUTE |
	    wxPOSIX_OTHERS_READ | wxPOSIX_OTHERS_EXECUTE);
	return true;
}
#endif
