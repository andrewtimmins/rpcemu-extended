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

#include <wx/wx.h>
#include <wx/display.h>

#include <cstdio>
#include <cstring>

#include "data_paths.h"
#include "config_paths.h"
#include "config_selector_dialog.h"
#include "main_frame.h"
#include "headless_main.h"

extern "C" {
#include "rpcemu.h"
#include "savestate.h"
}

class RpcemuApp : public wxApp {
public:
	bool OnInit() override;
};

/*
 * No wxIMPLEMENT_APP here: we provide our own main() so that headless mode can
 * run without ever constructing a wxApp (and therefore without gtk_init() and a
 * display connection). The GUI path is reached via wxEntry() below.
 */
wxIMPLEMENT_APP_NO_MAIN(RpcemuApp);

bool RpcemuApp::OnInit()
{
	if (!wxApp::OnInit()) {
		return false;
	}

	// Register image handlers (PNG etc.). wxGTK pulls these in implicitly, but
	// wxMSW does not; without this, PNG bitmaps (toolbar icons, app icon) fail
	// to load.
	wxInitAllImageHandlers();

	InitRpcemuPaths();

	ConfigSelectorDialog selector(nullptr);
	if (selector.ShowModal() != wxID_OK) {
		return false;
	}

	const wxString config_path = selector.GetSelectedConfigPath();
	const bool resume_requested = selector.ShouldResume();
	const wxString state_file = selector.GetStateFileToLoad();
	// The machine's own snapshot is "consumed" (renamed to .bak) on resume;
	// a state file the user opened explicitly via Load State is left in place.
	const wxString own_snapshot = ConfigPathsSnapshotForConfig(
	    ConfigPathsAbsoluteConfigPath(config_path));
	config_set_path(ConfigPathsAbsoluteConfigPath(config_path).utf8_str().data());
	rpcemu_prestart();

	auto *frame = new MainFrame();
	frame->Show(true);
	SetTopWindow(frame);

	// Tell the core the size of the display the window opened on, before
	// rpcemu_start() loads the ROMs: the synthesised monitor EDID is patched in
	// during loadroms(), and without this it falls back to a fixed 1920x1080 and
	// the guest's "Auto" monitor detection learns the wrong native mode.
	//
	// The monitor's full geometry is used rather than its client area, because
	// that is what an EDID's preferred timing means - the display's native mode,
	// not the space left over beside a taskbar. rom_patch.c clamps it.
	{
		// Show() is asynchronous on some platforms, so the frame may not be
		// mapped yet and GetFromWindow() can answer wxNOT_FOUND. Fall back to
		// the primary display rather than constructing an invalid wxDisplay.
		int index = wxDisplay::GetFromWindow(frame);

		if (index == wxNOT_FOUND) {
			index = 0;
		}

		const wxDisplay display((unsigned) index);
		const wxRect geom = display.GetGeometry();

		if (geom.width > 0 && geom.height > 0) {
			const wxVideoMode mode = display.GetCurrentMode();

			rpcemu_set_host_display((unsigned) geom.width, (unsigned) geom.height,
			                        mode.refresh > 0 ? (unsigned) mode.refresh : 0);
			rpclog("main: host display %dx%d (display %d of %u)\n",
			       geom.width, geom.height, index, wxDisplay::GetCount());
		}
	}

	rpcemu_start();

	// If the user chose Resume in the machine selector, load this machine's
	// snapshot. This runs before the emulator thread starts, so state_load()
	// operates single-threaded. The snapshot is renamed to .bak on success so
	// it is "consumed" (the session is now live) yet recoverable; a Restart /
	// Start leaves the snapshot untouched.
	if (resume_requested) {
		const std::string state_utf8 = state_file.utf8_str().data();
		char errbuf[256];

		if (state_check(state_utf8.c_str(), errbuf, sizeof(errbuf)) == 0 &&
		    state_load(state_utf8.c_str()) == 0) {
			/* Only the machine's own snapshot is consumed to .bak; a file
			   opened explicitly via Load State is left where it is. */
			if (state_file == own_snapshot) {
				const wxString bak = own_snapshot + ".bak";
				if (wxFileExists(bak)) {
					wxRemoveFile(bak);
				}
				wxRenameFile(own_snapshot, bak);
			}
		} else {
			rpclog("main: could not load state '%s': %s\n",
			       state_utf8.c_str(), errbuf);
			wxMessageBox(
			    wxString::Format("Could not load the machine state:\n%s\n\n"
			                     "Performing a normal boot instead.",
			                     wxString::FromUTF8(errbuf)),
			    "RPCEmu", wxOK | wxICON_WARNING, frame);
		}
	}

	// Start the emulator (which spawns the CPU and VIDC worker threads) only
	// once the GUI event loop is actually running. If it starts here - before
	// OnInit returns and the loop begins pumping - the VIDC thread can post its
	// first frame while no event loop is servicing CallAfter. PostVideoUpdate()
	// then blocks that thread (holding video_mutex) on a CallAfter that cannot
	// run, deadlocking startup. Deferring via CallAfter guarantees the loop is
	// live first. (Symptom: window opens with menus but no toolbar/display and
	// "Not Responding"; timing-dependent, seen on Windows.)
	frame->CallAfter([frame]() { frame->StartEmulator(); });
	return true;
}

int main(int argc, char **argv)
{
	bool headless = false;
	bool list_machines = false;
	bool show_help = false;
	const char *machine_name = nullptr;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--headless") == 0) {
			headless = true;
		} else if (strcmp(arg, "--list-machines") == 0) {
			list_machines = true;
		} else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			show_help = true;
		} else if (strcmp(arg, "--machine") == 0) {
			if (i + 1 < argc) {
				machine_name = argv[++i];
			} else {
				fprintf(stderr, "error: --machine requires a machine name.\n");
				return 2;
			}
		} else if (strncmp(arg, "--machine=", 10) == 0) {
			machine_name = arg + 10;
		}
		/* Any other argument (e.g. --verbose) is left for the GUI wxApp. */
	}

	/* These paths run entirely without wxWidgets, so they never initialise GTK
	   and work on a system with no display present. */
	if (show_help) {
		HeadlessPrintUsage(argv[0]);
		return 0;
	}
	if (list_machines) {
		return HeadlessListMachines();
	}
	if (headless) {
		return RunHeadless(machine_name);
	}
	if (machine_name != nullptr) {
		fprintf(stderr, "error: --machine is only valid together with --headless.\n");
		return 2;
	}

	/* Normal graphical launch. */
	return wxEntry(argc, argv);
}
