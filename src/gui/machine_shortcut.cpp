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

#include "gui_resources.h"

extern "C" {
#include "rpcemu.h"
}

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
 * A small application bundle, because macOS has no shortcut that carries
 * arguments and the two things that come closest do not work here: a Finder
 * alias and a symbolic link both point at a file and have nowhere to put
 * "--machine KB5".
 *
 * ★ This used to be a .command file, and that is what put a second window on
 * screen. A .command is an executable shell script that Terminal is registered
 * to open, so double-clicking one starts Terminal, which opens a window, runs
 * the line and leaves the window there. The window belongs to Terminal, not to
 * RPCEmu, and nothing in this program could close it. An application bundle is
 * opened by LaunchServices directly, with no interpreter in front of it and no
 * window of its own, and it can carry an icon - which a .command cannot.
 *
 * The bundle is tiny: a plist, a one-line script and a copy of the icon.
 *
 *   <name>.app/Contents/Info.plist
 *   <name>.app/Contents/MacOS/launch
 *   <name>.app/Contents/Resources/rpcemu.icns
 *
 * It starts the emulator through "open" rather than the binary inside the
 * bundle, which would bypass the icon and the dock; -n for a new instance,
 * which is also what stops the click being swallowed by a machine already
 * running (see RpcemuApp::MacReopenApp()); --args last, as it must be.
 */

/* Anything that would end the string early or be read as markup. The machine
   name reaches the plist from a name the user chose. */
static wxString PlistEscape(const wxString &text)
{
	wxString out;

	for (const wxUniChar c : text) {
		switch ((int) c) {
		case '&': out << "&amp;"; break;
		case '<': out << "&lt;"; break;
		case '>': out << "&gt;"; break;
		default: out << c; break;
		}
	}
	return out;
}

/*
 * A bundle identifier for the shortcut, which must be its own and must NOT be
 * the emulator's: LaunchServices treats every process running a bundle's
 * executable as an instance of that application, and two applications claiming
 * one identifier is exactly the confusion that made the Manager unreachable
 * once already.
 */
static wxString ShortcutBundleId(const wxString &name)
{
	wxString id;

	for (const wxUniChar c : name) {
		if (wxIsalnum(c) || c == '-') {
			id << c;
		} else {
			id << '-';
		}
	}
	if (id.empty()) {
		id = "machine";
	}
	return "com.github.andrewtimmins.rpcemu.shortcut." + id;
}

static bool WriteExecutableFile(const wxString &path, const wxString &text)
{
	wxFFile file(path, "wb");

	if (!file.IsOpened() || !file.Write(text) || !file.Close()) {
		return false;
	}
	return wxFileName(path).SetPermissions(wxPOSIX_USER_READ | wxPOSIX_USER_WRITE |
	    wxPOSIX_USER_EXECUTE | wxPOSIX_GROUP_READ | wxPOSIX_GROUP_EXECUTE |
	    wxPOSIX_OTHERS_READ | wxPOSIX_OTHERS_EXECUTE);
}

bool WriteShortcut(const wxString &path, const wxString &exe,
                          const wxString &args, const wxString &working_dir,
                          const wxString &description)
{
	/* Contents/MacOS/<binary> back up to the .app itself. */
	wxFileName bundle(exe);

	bundle.RemoveLastDir();
	bundle.RemoveLastDir();

	const wxString app = bundle.GetPath();
	const wxString contents = path + "/Contents";
	const wxString macos = contents + "/MacOS";
	const wxString resources = contents + "/Resources";

	/*
	 * Replacing rather than merging. The user has already agreed to overwrite,
	 * and a bundle written over an older one of the same name would otherwise
	 * keep whatever the old one had and this one does not.
	 */
	if (wxFileName::DirExists(path)) {
		if (!wxFileName::Rmdir(path, wxPATH_RMDIR_RECURSIVE)) {
			return false;
		}
	} else if (wxFileExists(path)) {
		if (!wxRemoveFile(path)) {
			return false;
		}
	}

	if (!wxFileName::Mkdir(macos, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL) ||
	    !wxFileName::Mkdir(resources, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		return false;
	}

	/*
	 * The icon, taken from the bundle this process is running out of.
	 *
	 * ★ Not from rpcemu_get_resourcedir(). That is where the guest payload
	 * lives, and --datadir moves it: a Manager started with an explicit data
	 * directory looks for the icon in that folder, does not find it, and writes
	 * a shortcut with no icon at all. The icon is part of the application, not
	 * part of anybody's data, and the bundle is already worked out just above
	 * because the shortcut has to name it anyway.
	 *
	 * The resource directory is still worth a look as a fallback, for a build
	 * run from its own directory rather than from a bundle. A source build that
	 * never ran build-macos.sh has no .icns anywhere, and a shortcut with the
	 * system's blank application icon still works, so finding neither is not a
	 * failure.
	 */
	wxString icns = app + "/Contents/Resources/rpcemu.icns";

	if (!wxFileExists(icns)) {
		icns = wxString::FromUTF8(rpcemu_get_resourcedir()) + "rpcemu.icns";
	}

	const bool have_icon = wxFileExists(icns) &&
	    wxCopyFile(icns, resources + "/rpcemu.icns");

	wxString plist;

	plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	      << "<plist version=\"1.0\">\n<dict>\n"
	      << "\t<key>CFBundleName</key><string>" << PlistEscape(description) << "</string>\n"
	      << "\t<key>CFBundleDisplayName</key><string>" << PlistEscape(description) << "</string>\n"
	      << "\t<key>CFBundleExecutable</key><string>launch</string>\n"
	      << "\t<key>CFBundleIdentifier</key><string>" << ShortcutBundleId(description) << "</string>\n"
	      << "\t<key>CFBundlePackageType</key><string>APPL</string>\n"
	      << "\t<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
	      << "\t<key>CFBundleShortVersionString</key><string>1.0</string>\n"
	      << "\t<key>CFBundleVersion</key><string>1.0</string>\n";
	if (have_icon) {
		plist << "\t<key>CFBundleIconFile</key><string>rpcemu</string>\n";
	}
	/* No dock icon and no menu bar for the launcher itself: it starts the
	   emulator and exits, and a second icon appearing beside the one it started
	   would be the same complaint in a different place. */
	plist << "\t<key>LSUIElement</key><true/>\n"
	      << "\t<key>NSHighResolutionCapable</key><true/>\n"
	      << "</dict>\n</plist>\n";

	wxFFile plist_file(contents + "/Info.plist", "wb");

	if (!plist_file.IsOpened() || !plist_file.Write(plist) || !plist_file.Close()) {
		return false;
	}

	wxString launch;

	launch << "#!/bin/sh\n"
	       << "# " << description << "\n"
	       << "exec open -n -a \"" << app << "\" --args " << args << "\n";

	if (!WriteExecutableFile(macos + "/launch", launch)) {
		return false;
	}

	/* Not required by anything modern, and four bytes to say what this is to
	   anything that still looks. */
	wxFFile pkginfo(contents + "/PkgInfo", "wb");

	if (pkginfo.IsOpened()) {
		pkginfo.Write("APPL????");
		pkginfo.Close();
	}
	(void) working_dir;	/* "open" starts the machine in its own; see above */
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

	/*
	 * The icon by absolute path where this build has one, and by name where it
	 * does not. A bare "rpcemu" is a theme lookup, which finds something only
	 * for a packaged install that put an icon in the theme; for a build run
	 * from its own directory - which is most of the ways this program is used -
	 * it silently finds nothing and the launcher shows the generic icon.
	 */
	const wxString logo = AppLogoPath();
	const wxString icon = wxFileExists(logo) ? logo : wxString("rpcemu");
	wxString text;

	text << "[Desktop Entry]\n"
	     << "Type=Application\n"
	     << "Name=" << description << "\n"
	     << "Comment=Risc PC and A7000 emulator\n"
	     << "Exec=\"" << exe << "\" " << args << "\n"
	     << "Path=" << working_dir << "\n"
	     << "Icon=" << icon << "\n"
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
