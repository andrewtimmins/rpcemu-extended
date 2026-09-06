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

#include <memory>
#include <vector>

#include "main_frame.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/richmsgdlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>
#include <wx/weakref.h>

#include "about_dialog.h"
#include "config_selector_dialog.h"
#include "display_options.h"
#include "config_paths.h"
#include "gui_preferences.h"
#include "input_helpers.h"
#include "machine_edit_dialog.h"
#include "support_bundle.h"
#include "machine_inspector_window.h"
#include "nat_list_dialog.h"
#include "guest_command.h"
#include "http_transfer.h"	/* RPCEMU_HAVE_HTTP, HttpUnavailableMessage */
#include "package_dialog.h"
#include "usb_dialog.h"
#include "parallel_dialog.h"
#include "serial_dialog.h"
#include "toolbar_icons.h"

#ifdef RPCEMU_VNC
#include "vnc_dialog.h"
#endif

#ifdef RPCEMU_VNC
#include "vnc_app.h"
#include "vnc_server.h"
#endif

extern "C" {
#include "rpcemu.h"
#include "display_mode.h"
#include "edid.h"
#include "hostclipboard.h"
#include "machine_lock.h"
}

namespace {

wxString WindowTitleFor(const wxString &machine_name)
{
	return wxString::Format("RPCEmu Extended - %s",
	    machine_name.empty() ? wxString("(unnamed machine)") : machine_name);
}

struct DiscTypeFileMap {
	const char *display_name;
	const char *extension;
	const char *blank_filename;
};

const DiscTypeFileMap kDiscTypeFileMaps[] = {
    {"ADFS E 800k Disc Image (*.adf)", ".adf", "blank-e-800.adf"},
    {"ADFS F 1600k Disc Image (*.adf)", ".adf", "blank-f-1600.adf"},
    {"ADFS L 640k Disc Image (*.adl)", ".adl", "blank-l-640.adl"},
    {"DOS 720k Disc Image (*.img)", ".img", "blank-pc-720.img"},
    {"DOS 1440k Disc Image (*.img)", ".img", "blank-pc-1440.img"},
};


bool HostResetQuestion(wxWindow *parent)
{
	return wxMessageBox(
	           "This will reset RPCEmu Extended!\n\nOkay to continue?",
	           "RPCEmu Extended",
	           wxOK | wxCANCEL | wxICON_WARNING,
	           parent) == wxOK;
}

bool SameString(const char *a, const char *b)
{
	if (a == nullptr || b == nullptr) {
		return a == b;
	}
	return strcmp(a, b) == 0;
}

/* Release the strings config_deep_copy() duplicated into a scratch Config. */
void FreeConfigCopy(Config *cfg)
{
	free(cfg->username);
	free(cfg->ipaddress);
	free(cfg->macaddress);
	free(cfg->bridgename);
	free(cfg->network_capture);
	cfg->username = nullptr;
	cfg->ipaddress = nullptr;
	cfg->macaddress = nullptr;
	cfg->bridgename = nullptr;
	cfg->network_capture = nullptr;
}

/*
 * Did the machine editor change something the running machine cannot pick up?
 *
 * The dialog's Options tab holds the settings that are either already live or
 * read when the machine next starts, so nothing there needs a restart. The
 * System, Network, IDE Drives and Podules tabs are the other way round: they
 * describe the hardware, which is built when the machine starts and by the
 * reset the editor does not perform. The ROM is the furthest out of reach,
 * being read only by loadroms() at start-up.
 *
 * Written as "everything except the Options fields" rather than as a list of
 * the fields that matter, so a setting added to one of the hardware tabs later
 * is treated as needing a restart without anyone having to remember to add it
 * here. Being asked to restart unnecessarily is a smaller problem than a
 * change that silently does not apply.
 */
bool MachineNeedsRestart(const Config *before, const Config *after)
{
	Config a;
	Config b;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	config_deep_copy(&a, before);
	config_deep_copy(&b, after);

	/* Flatten every Options-tab setting so it cannot register as a change. */
	a.show_fullscreen_message = b.show_fullscreen_message = 0;
	a.display_scaling = b.display_scaling = 0;
	a.screen_size_x = b.screen_size_x = 0;
	a.screen_size_y = b.screen_size_y = 0;
	a.soundenabled = b.soundenabled = 0;
	a.cdromenabled = b.cdromenabled = 0;
	a.mousetwobutton = b.mousetwobutton = 0;
	a.cpu_idle = b.cpu_idle = 0;
	a.suspend_on_exit = b.suspend_on_exit = 0;
	a.vnc_enabled = b.vnc_enabled = 0;
	a.clipboard_enabled = b.clipboard_enabled = 0;
	/* A rename is not something to restart for, and offering to would be
	   worse than useless: the data directory is deliberately left under the
	   old name while the machine is running, so a restart would load the
	   configuration under the new one and point HostFS at a directory that
	   does not exist. The rename has already said so in its own message. */
	memset(a.name, 0, sizeof(a.name));
	memset(b.name, 0, sizeof(b.name));

	/* Compared by value: the memcmp below only sees the pointers, which always
	   differ between two copies. */
	bool changed = !SameString(a.username, b.username) ||
	               !SameString(a.ipaddress, b.ipaddress) ||
	               !SameString(a.macaddress, b.macaddress) ||
	               !SameString(a.bridgename, b.bridgename) ||
	               !SameString(a.network_capture, b.network_capture);

	if (!changed) {
		/* Take the pointers out of the picture for the structure comparison,
		   keeping hold of them so they can still be freed. */
		Config a_cmp = a;
		Config b_cmp = b;
		a_cmp.username = b_cmp.username = nullptr;
		a_cmp.ipaddress = b_cmp.ipaddress = nullptr;
		a_cmp.macaddress = b_cmp.macaddress = nullptr;
		a_cmp.bridgename = b_cmp.bridgename = nullptr;
		a_cmp.network_capture = b_cmp.network_capture = nullptr;
		changed = memcmp(&a_cmp, &b_cmp, sizeof(Config)) != 0;
	}

	FreeConfigCopy(&a);
	FreeConfigCopy(&b);
	return changed;
}

} // namespace

/*
 * RPCEMU_TEST_DISPLAY_TIMING: where the time goes when the guest changes mode.
 *
 * Issue #220 reported mode switching as slow, and slower the larger the mode,
 * on Windows. It does not reproduce on macOS - measured flat at every size from
 * 800x600 to 1920x1440 - so the numbers have to come from the machine that has
 * the problem. This prints them: what the guest asked for, how long the window
 * waited before following, how long each step of the resize took, and whether
 * the GPU is drawing at all, since the software path pays a full-frame blit
 * that the GPU path does not.
 *
 * Read once. Off, this costs one comparison per mode change.
 */
static bool
display_timing_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		const char *env = getenv("RPCEMU_TEST_DISPLAY_TIMING");

		wanted = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
	}
	return wanted != 0;
}

/** Milliseconds since the first call, for the diagnostic above. */
static long
display_timing_ms(void)
{
	static const wxLongLong start = wxGetLocalTimeMillis();

	return (long) (wxGetLocalTimeMillis() - start).GetValue();
}

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
	EVT_CLOSE(MainFrame::OnClose)
	EVT_ACTIVATE(MainFrame::OnActivate)
	EVT_LEFT_DOWN(MainFrame::OnLeftDown)
	EVT_TIMER(ID_TIMER_MIPS, MainFrame::OnMipsTimer)
	EVT_TIMER(ID_TIMER_VIDEO, MainFrame::OnVideoTimer)
	EVT_TIMER(ID_TIMER_FDC_LED, MainFrame::OnFdcLedTimer)
	EVT_TIMER(ID_TIMER_IDE_LED, MainFrame::OnIdeLedTimer)
	EVT_TIMER(ID_TIMER_HOSTFS_LED, MainFrame::OnHostfsLedTimer)
	EVT_TIMER(ID_TIMER_NETWORK_LED, MainFrame::OnNetworkLedTimer)
	EVT_TIMER(ID_TIMER_CLIPBOARD, MainFrame::OnClipboardTimer)
	EVT_TIMER(ID_TIMER_SYNTHETIC_RELEASE, MainFrame::OnSyntheticReleaseTimer)
	EVT_TIMER(ID_TIMER_MODE_VERIFY, MainFrame::OnModeVerifyTimer)
	EVT_TIMER(ID_TIMER_GUEST_RESIZE, MainFrame::OnGuestResizeTimer)
	EVT_TIMER(ID_TIMER_TEST_CLOSE, MainFrame::OnTestCloseTimer)
	EVT_TIMER(ID_TIMER_TEST_FULLSCREEN, MainFrame::OnTestFullscreenTimer)
	EVT_TIMER(ID_TIMER_TEST_INSPECTOR, MainFrame::OnTestInspectorTimer)
	EVT_TIMER(ID_TIMER_TEST_PODULES, MainFrame::OnTestPodulesTimer)
	EVT_DISPLAY_CHANGED(MainFrame::OnDisplayChanged)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
	: wxFrame(nullptr, wxID_ANY,
	          WindowTitleFor(wxString::FromUTF8(config.name)),
	          wxDefaultPosition, wxSize(800, 600)),
	  mips_timer_(this, ID_TIMER_MIPS),
	  video_timer_(this, ID_TIMER_VIDEO),
	  fdc_led_timer_(this, ID_TIMER_FDC_LED),
	  ide_led_timer_(this, ID_TIMER_IDE_LED),
	  hostfs_led_timer_(this, ID_TIMER_HOSTFS_LED),
	  network_led_timer_(this, ID_TIMER_NETWORK_LED),
	  clipboard_timer_(this, ID_TIMER_CLIPBOARD)
	, synthetic_release_timer_(this, ID_TIMER_SYNTHETIC_RELEASE)
	, mode_verify_timer_(this, ID_TIMER_MODE_VERIFY)
	, guest_resize_timer_(this, ID_TIMER_GUEST_RESIZE)
	, test_close_timer_(this, ID_TIMER_TEST_CLOSE)
	, test_fullscreen_timer_(this, ID_TIMER_TEST_FULLSCREEN)
	, test_inspector_timer_(this, ID_TIMER_TEST_INSPECTOR)
	, test_podules_timer_(this, ID_TIMER_TEST_PODULES)
{
	config_deep_copy(&config_copy_, &config);
	pconfig_copy = &config_copy_;
	model_copy_ = machine.model;

	// Window / taskbar / Alt-Tab icon (both platforms). Shipped in
	// <resourcedir>/resources/rpcemu.png. On Windows the .exe file icon comes
	// from the compiled-in .ico (see cmake/FindWxWidgets.cmake).
	{
		const wxString icon_path = wxString::FromUTF8(rpcemu_get_resourcedir()) +
		    "resources" + wxFileName::GetPathSeparator() + "rpcemu.png";
		wxImage icon_image;
		if (wxFileExists(icon_path) &&
		    icon_image.LoadFile(icon_path, wxBITMAP_TYPE_PNG)) {
			wxIcon icon;
			icon.CopyFromBitmap(wxBitmap(icon_image));
			SetIcon(icon);
		}
	}

	emulator_ = std::make_unique<EmulatorHost>(this);

#ifdef RPCEMU_VNC
	/*
	 * The server belongs to the process, not to this window: it may already be
	 * running and showing the machine selector to a client, and stopping and
	 * restarting it here would drop that connection for no reason. Attach the
	 * machine to it instead, and start it if it is not up yet - which is the case
	 * when VNC was switched on for a machine rather than app-wide.
	 */
	vnc_server_ = &VncAppServer();
	if (!VncAppRunning() && config_copy_.vnc_enabled) {
		VncAppStart(false);
	}
	VncAppAttach(emulator_.get());
#endif

	/*
	 * Black behind the panel, not the platform's window grey.
	 *
	 * Two things show this through at startup: the panel begins at its default
	 * size before the guest has announced a mode, so the frame is briefly larger
	 * than it, and a GL canvas clears to black until its first texture arrives.
	 * Against a grey window both read as a fault - a small black box adrift in a
	 * pale border - where against black they are simply a screen that has not
	 * lit up yet, which is what they are. It is also the right colour for the
	 * letterbox bars either side of a scaled display.
	 */
	SetBackgroundColour(*wxBLACK);

	panel_ = new EmulatorPanel(this, *emulator_);
	panel_->Bind(wxEVT_KEY_DOWN, &MainFrame::OnKeyDown, this);
	panel_->Bind(wxEVT_KEY_UP, &MainFrame::OnKeyUp, this);
	auto *sizer = new wxBoxSizer(wxVERTICAL);
	/* The panel must fill the frame's client area so that scaled modes
	   (fit-to-window, full-screen, integer scaling) can use the whole window;
	   in the fixed 1:1 mode the panel's own size hints keep the frame shrink-
	   wrapped to the guest resolution, so expanding it here is harmless there. */
	sizer->Add(panel_, 1, wxEXPAND);
	SetSizer(sizer);

	BuildMenus();
	BuildToolBar();
	BuildStatusBar();

	mips_timer_.Start(1000);
	/* Watch the host clipboard. wxWidgets has no "it changed" notification, so
	   it has to be looked at now and then; twice a second is well below noticing
	   and costs nothing measurable.
	   It runs whether or not sharing is on, and the handler returns immediately
	   when it is off. Starting and stopping it alongside the setting instead
	   means every path that can change the setting has to remember to do it, and
	   the one that did not (switching to a machine that has sharing on) left the
	   host end asleep with no sign of why. */
	clipboard_timer_.Start(500);

	window_active_ = true;
	UpdateMachineStatus();
}

MainFrame::~MainFrame()
{
#ifdef RPCEMU_VNC
	if (vnc_server_) {
		/* Detached, not stopped: the server outlives this window, and a client
		   should stay connected across a machine going away. */
		VncAppDetach();
	}
#endif
	if (machine_inspector_window_ != nullptr) {
		machine_inspector_window_->Destroy();
		machine_inspector_window_ = nullptr;
	}
	ShutdownEmulator();
}

void MainFrame::UpdateMachineStatus()
{
	wxString status = wxString::Format("Machine: %s", wxString::FromUTF8(config_copy_.name));
	if (!config_copy_.mousehackon) {
		if (mouse_captured) {
			status += " - Press Alt+Enter to release mouse";
		} else {
			status += " - Click to capture mouse";
		}
	} else {
		status += " - Mouse follows host pointer";
	}
	SetStatusText(status, STATUS_MACHINE);
}

wxString MainFrame::BlankDiscResourcePath(const wxString &filename) const
{
	return wxFileName(wxString::FromUTF8(rpcemu_get_resourcedir()), "resources/" + filename).GetFullPath();
}

wxString MainFrame::ConfigPathForMachine(const wxString &machine_name) const
{
	return ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + machine_name + ".cfg";
}

void MainFrame::StartEmulator()
{
	config_deep_copy(&config_copy_, &config);
	model_copy_ = machine.model;
	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}
	/*
	 * A machine that has never been given a screen size gets one now, chosen for
	 * this display. Stored, so it is a plain resolution from here on rather than
	 * a policy that re-decides itself behind the user's back.
	 */
	if (config_copy_.screen_size_x == 0 || config_copy_.screen_size_y == 0) {
		unsigned w = 0, h = 0;

		if (rpcemu_default_screen_size(&w, &h)) {
			config_copy_.screen_size_x = w;
			config_copy_.screen_size_y = h;
			config.screen_size_x = w;
			config.screen_size_y = h;
			rpclog("Display: no screen size configured, chose %ux%u\n", w, h);
		}
	}

	/*
	 * The window is sized here, once, from the configuration - and then left
	 * alone. It is NOT sized from the guest: RISC OS passes through two or three
	 * modes on the way up, and on a machine with the graphics card the display is
	 * handed over part way through as well, so a window that followed the guest
	 * jumped through three sizes and positions before settling. See
	 * ApplyDisplayModeToPanel() and NoteGuestFrame().
	 */
	ApplyDisplayModeToPanel();
	guest_size_seen_ = wxSize(0, 0);

	/* Whatever this machine's monitor definition declares, it is this machine's
	   and nothing learned about the last one applies. */
	display_mode_clear_unavailable();

	/* The size has to be asked for, or the guest never hears about it: the
	   support module polls, and until something bumps the generation there is
	   nothing for it to act on. Not reported if refused - the machine is still
	   starting, and the mode it boots into comes from the EDID anyway. */
	RequestGuestMode(config_copy_.screen_size_x, config_copy_.screen_size_y,
	                 false);
	SyncSettingsMenuChecks();
	SyncCdromMenuChecks();
	UpdateMachineStatus();

	if (const char *after = getenv("RPCEMU_TEST_FULLSCREEN_AFTER")) {
		const long seconds = strtol(after, nullptr, 10);

		if (seconds > 0) {
			test_fullscreen_timer_.StartOnce((int) (seconds * 1000));
		}
	}

	if (const char *after = getenv("RPCEMU_TEST_CLOSE_AFTER")) {
		const long seconds = strtol(after, nullptr, 10);

		if (seconds > 0) {
			rpclog("MainFrame: RPCEMU_TEST_CLOSE_AFTER=%ld\n", seconds);
			test_close_timer_.StartOnce((int) (seconds * 1000));
		}
	}

	if (const char *after = getenv("RPCEMU_TEST_INSPECTOR_AFTER")) {
		const long seconds = strtol(after, nullptr, 10);

		if (seconds > 0) {
			rpclog("MainFrame: RPCEMU_TEST_INSPECTOR_AFTER=%ld\n", seconds);
			test_inspector_timer_.StartOnce((int) (seconds * 1000));
		}
	}

	if (const char *after = getenv("RPCEMU_TEST_PODULES_AFTER")) {
		const long seconds = strtol(after, nullptr, 10);

		if (seconds > 0) {
			rpclog("MainFrame: RPCEMU_TEST_PODULES_AFTER=%ld\n", seconds);
			test_podules_timer_.StartOnce((int) (seconds * 1000));
		}
	}

	AddRecentMachine(config_copy_.name);

	/* The boot's run of mode changes starts here; see NoteGuestFrame(). */
	machine_started_ms_ = wxGetLocalTimeMillis();

	if (emulator_) {
		emulator_->Start();
	}

	/* Deferred, because going full screen resizes and re-lays-out the frame,
	   and this runs before the event loop has shown it: doing it here leaves
	   the panel sized for a window that was never displayed. */
	if (config_copy_.start_fullscreen) {
		CallAfter([this] {
			if (!full_screen_) {
				EnterFullScreen();
			}
		});
	}

	UpdateDebuggerActionStates();
}

void MainFrame::OnScreenshot(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Save Screenshot", wxEmptyString, "screenshot.png",
	                 "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (panel_ == nullptr || !panel_->SaveScreenshot(dlg.GetPath())) {
		wxMessageBox("Error saving screenshot", "RPCEmu Extended", wxOK | wxICON_WARNING, this);
	}
}

void MainFrame::OnReset(wxCommandEvent &)
{
	if (!HostResetQuestion(this)) {
		return;
	}
	if (emulator_) {
		/* A reset boots again, so the window goes back to waiting for the run
		   of mode changes to finish rather than following each one. */
		machine_started_ms_ = wxGetLocalTimeMillis();
		emulator_->Reset();
	}
}

/* Restart the running machine, picking up its configuration file and ROMs as
   they are now. Does nothing if no machine is running, where there is nothing
   to restart and the settings that would be applied are read at startup
   anyway. */
void MainFrame::RestartMachine()
{
	if (emulator_ == nullptr || !emulator_->IsRunning()) {
		return;
	}
	/* Boots again, so the window waits out the boot's modes once more rather
	   than following each of them; see NoteGuestFrame(). */
	machine_started_ms_ = wxGetLocalTimeMillis();
	emulator_->Restart();
}

void MainFrame::OnSaveState(wxCommandEvent &)
{
	const wxFileName snapshot(ConfigPathsSnapshotForConfig(
	    ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path()))));

	wxFileDialog dlg(this, "Save Machine State", snapshot.GetPath(), snapshot.GetFullName(),
	                 "RPCEmu machine state (*.state)|*.state|All files (*)|*",
	                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (emulator_) {
		if (!emulator_->SaveState(dlg.GetPath().utf8_str().data())) {
			wxMessageBox("Failed to save the machine state.", "RPCEmu Extended",
			             wxOK | wxICON_WARNING, this);
		}
	}
}

void MainFrame::OnLoadState(wxCommandEvent &)
{
	const wxFileName snapshot(ConfigPathsSnapshotForConfig(
	    ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path()))));

	wxFileDialog dlg(this, "Load Machine State", snapshot.GetPath(), wxEmptyString,
	                 "RPCEmu machine state (*.state)|*.state|All files (*)|*",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (emulator_) {
		std::string error;
		if (!emulator_->LoadState(dlg.GetPath().utf8_str().data(), &error)) {
			wxMessageBox(error.empty() ? wxString("Failed to load the machine state.")
			                           : wxString::FromUTF8(error.c_str()),
			             "RPCEmu Extended", wxOK | wxICON_WARNING, this);
		}
	}
}

void MainFrame::OnSuspend(wxCommandEvent &)
{
	/* Suspend is an explicit "save state and exit": flag it so OnClose writes
	   the snapshot even when the "Suspend on exit" setting is off, then close. */
	suspend_on_exit_requested_ = true;
	Close(true);
}

void MainFrame::OnSuspendOnExit(wxCommandEvent &event)
{
	const int on = event.IsChecked() ? 1 : 0;
	config_copy_.suspend_on_exit = on;
	config.suspend_on_exit = on;	/* what config_save() persists */
	if (suspend_on_exit_menu_item_ != nullptr) {
		suspend_on_exit_menu_item_->Check(on != 0);
	}
}

void MainFrame::OnRecentMachine(wxCommandEvent &event)
{
	const int index = event.GetId() - ID_MENU_RECENT_MACHINE_0;
	if (index < 0 || index >= MaxRecentMachines) {
		return;
	}

	const std::vector<std::string> recent = GetRecentMachines();
	if (index >= static_cast<int>(recent.size())) {
		return;
	}

	const wxString machine_name = wxString::FromUTF8(recent[static_cast<size_t>(index)]);
	const wxString config_path = ConfigPathForMachine(machine_name);
	if (!wxFileExists(config_path)) {
		wxMessageBox(wxString::Format("The configuration for '%s' no longer exists.", machine_name),
		             "Machine Not Found", wxOK | wxICON_WARNING, this);
		return;
	}

	const int ret = wxMessageBox(
	    wxString::Format(
	        "Are you sure you want to switch to '%s'?\n\n"
	        "Any unsaved data in the current machine will be lost.",
	        machine_name),
	    "Switch Machine",
	    wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
	    this);
	if (ret != wxYES) {
		return;
	}

	AddRecentMachine(machine_name.utf8_str().data());
	UpdateRecentMachinesMenu();
	SetTitle(WindowTitleFor(machine_name));

	if (emulator_) {
		/* The machine being switched to boots from cold, so the window waits
		   out its run of mode changes rather than following each one; see
		   NoteGuestFrame(). */
		machine_started_ms_ = wxGetLocalTimeMillis();
		emulator_->SwitchMachine(config_path.utf8_str().data());
	}
}

void MainFrame::OnClearRecentMachines(wxCommandEvent &)
{
	ClearRecentMachines();
	UpdateRecentMachinesMenu();
}

void MainFrame::LoadDisc(int drive)
{
	wxFileDialog dlg(this, "Open Disc Image", wxEmptyString, wxEmptyString,
	                 "All disc images (*.adf;*.adl;*.hfe;*.img)|*.adf;*.adl;*.hfe;*.img|"
	                 "ADFS D/E/F Disc Image (*.adf)|*.adf|"
	                 "ADFS L Disc Image (*.adl)|*.adl|"
	                 "DOS Disc Image (*.img)|*.img|"
	                 "HFE Disc Image (*.hfe)|*.hfe",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString path = dlg.GetPath();
	AddRecentFloppy(path.utf8_str().data());
	UpdateRecentFloppiesMenu();

	if (emulator_) {
		emulator_->LoadDisc(drive, path.utf8_str().data());
	}
}

void MainFrame::OnLoadDisc0(wxCommandEvent &) { LoadDisc(0); }
void MainFrame::OnLoadDisc1(wxCommandEvent &) { LoadDisc(1); }

void MainFrame::OnEjectDisc0(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->EjectDisc(0);
	}
}

void MainFrame::OnEjectDisc1(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->EjectDisc(1);
	}
}

void MainFrame::CreateDisc(int drive)
{
	const wxString filter =
	    "ADFS E 800k Disc Image (*.adf)|*.adf|"
	    "ADFS F 1600k Disc Image (*.adf)|*.adf|"
	    "ADFS L 640k Disc Image (*.adl)|*.adl|"
	    "DOS 720k Disc Image (*.img)|*.img|"
	    "DOS 1440k Disc Image (*.img)|*.img";

	wxFileDialog dlg(this, "Create Blank Disc Image", wxEmptyString, wxEmptyString, filter,
	                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const int filter_index = dlg.GetFilterIndex();
	if (filter_index < 0 || filter_index >= static_cast<int>(WXSIZEOF(kDiscTypeFileMaps))) {
		return;
	}
	const DiscTypeFileMap *disc_type = &kDiscTypeFileMaps[static_cast<size_t>(filter_index)];

	wxString file_name = dlg.GetPath();
	const wxString extension = disc_type->extension;
	if (!file_name.Lower().EndsWith(extension)) {
		file_name += extension;
		if (wxFileExists(file_name)) {
			wxLogError("Not overwriting existing file '%s'", file_name);
			return;
		}
	}

	if (wxFileExists(file_name) && !wxRemoveFile(file_name)) {
		wxLogError("Failed to remove existing file '%s' before overwriting", file_name);
		return;
	}

	const wxString blank_src = BlankDiscResourcePath(disc_type->blank_filename);
	if (!wxFileExists(blank_src) || !wxCopyFile(blank_src, file_name, true)) {
		wxLogError("Failed to create blank image file '%s'", file_name);
		return;
	}

	if (emulator_) {
		emulator_->LoadDisc(drive, file_name.utf8_str().data());
	}
}

void MainFrame::OnCreateDisc0(wxCommandEvent &) { CreateDisc(0); }
void MainFrame::OnCreateDisc1(wxCommandEvent &) { CreateDisc(1); }

void MainFrame::OnRecentFloppy(wxCommandEvent &event)
{
	const int index = event.GetId() - ID_MENU_RECENT_FLOPPY_0;
	if (index < 0 || index >= MaxRecentFloppies) {
		return;
	}

	const std::vector<std::string> recent = GetRecentFloppies();
	if (index >= static_cast<int>(recent.size())) {
		return;
	}

	const wxString path = wxString::FromUTF8(recent[static_cast<size_t>(index)]);
	if (!wxFileExists(path)) {
		wxMessageBox(wxString::Format("The disc image '%s' no longer exists.", path),
		             "File Not Found", wxOK | wxICON_WARNING, this);
		return;
	}

	AddRecentFloppy(path.utf8_str().data());
	UpdateRecentFloppiesMenu();
	if (emulator_) {
		emulator_->LoadDisc(0, path.utf8_str().data());
	}
}

void MainFrame::OnClearRecentFloppies(wxCommandEvent &)
{
	ClearRecentFloppies();
	UpdateRecentFloppiesMenu();
}

void MainFrame::OnCdromDisabled(wxCommandEvent &)
{
	if (config_copy_.cdromenabled && !HostResetQuestion(this)) {
		SyncCdromMenuChecks();
		return;
	}

	if (emulator_) {
		emulator_->CdromDisabled();
	}
	config_copy_.cdromenabled = 0;
	CdromMenuSelectionUpdate(ID_MENU_CDROM_DISABLED);
}

void MainFrame::OnCdromEmpty(wxCommandEvent &)
{
	if (!config_copy_.cdromenabled && !HostResetQuestion(this)) {
		SyncCdromMenuChecks();
		return;
	}

	if (emulator_) {
		emulator_->CdromEmpty();
	}
	config_copy_.cdromenabled = 1;
	config_copy_.cdromtype = 0;
	CdromMenuSelectionUpdate(ID_MENU_CDROM_EMPTY);
}

void MainFrame::OnCdromIso(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Open ISO Image", wxEmptyString, wxEmptyString,
	                 "ISO CD-ROM Image (*.iso)|*.iso|All Files (*.*)|*.*",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dlg.ShowModal() != wxID_OK) {
		SyncCdromMenuChecks();
		return;
	}

	if (!config_copy_.cdromenabled && !HostResetQuestion(this)) {
		SyncCdromMenuChecks();
		return;
	}

	const wxString path = dlg.GetPath();
	AddRecentCDROM(path.utf8_str().data());
	UpdateRecentCdromsMenu();

	if (emulator_) {
		emulator_->CdromLoadIso(path.utf8_str().data());
	}
	config_copy_.cdromenabled = 1;
	config_copy_.cdromtype = 2;
	CdromMenuSelectionUpdate(ID_MENU_CDROM_ISO);
}

void MainFrame::OnCdromIoctl(wxCommandEvent &)
{
	if (!config_copy_.cdromenabled && !HostResetQuestion(this)) {
		SyncCdromMenuChecks();
		return;
	}

	if (emulator_) {
		emulator_->CdromIoctl();
	}
	config_copy_.cdromenabled = 1;
	config_copy_.cdromtype = 1;
	CdromMenuSelectionUpdate(ID_MENU_CDROM_IOCTL);
}

void MainFrame::OnRecentCdrom(wxCommandEvent &event)
{
	const int index = event.GetId() - ID_MENU_RECENT_CDROM_0;
	if (index < 0 || index >= MaxRecentCDROMs) {
		return;
	}

	const std::vector<std::string> recent = GetRecentCDROMs();
	if (index >= static_cast<int>(recent.size())) {
		return;
	}

	const wxString path = wxString::FromUTF8(recent[static_cast<size_t>(index)]);
	if (!wxFileExists(path)) {
		wxMessageBox(wxString::Format("The CD-ROM image '%s' no longer exists.", path),
		             "File Not Found", wxOK | wxICON_WARNING, this);
		return;
	}

	if (!config_copy_.cdromenabled && !HostResetQuestion(this)) {
		return;
	}

	AddRecentCDROM(path.utf8_str().data());
	UpdateRecentCdromsMenu();

	if (emulator_) {
		emulator_->CdromLoadIso(path.utf8_str().data());
	}
	config_copy_.cdromenabled = 1;
	config_copy_.cdromtype = 2;
	CdromMenuSelectionUpdate(ID_MENU_CDROM_ISO);
}

void MainFrame::OnClearRecentCdroms(wxCommandEvent &)
{
	ClearRecentCDROMs();
	UpdateRecentCdromsMenu();
}

void MainFrame::OnMachine(wxCommandEvent &) { EditMachineConfiguration(); }

void MainFrame::OnNatList(wxCommandEvent &)
{
#ifdef RPCEMU_NETWORKING
	NatListDialog dlg(this, emulator_.get());

	dlg.ShowRules();
#else
	/* Networking (and the NAT list) is not compiled in - nothing to do.
	   (The handler's wxCommandEvent parameter is unnamed, so nothing to void.) */
#endif
}

void MainFrame::OnMute(wxCommandEvent &event)
{
	const bool muted = event.IsChecked();
	plt_sound_set_muted(muted ? 1 : 0);
	if (mute_menu_item_ != nullptr) {
		mute_menu_item_->Check(muted);
	}
	if (tb_mute_tool_ != nullptr && tool_bar_ != nullptr) {
		tool_bar_->ToggleTool(ID_MENU_MUTE, muted);
		tool_bar_->SetToolNormalBitmap(ID_MENU_MUTE, ToolbarIconMute(muted));
	}
}

void MainFrame::EnterFullScreen()
{
	if (full_screen_) {
		return;
	}

	if (config_copy_.show_fullscreen_message) {
		/* One dialogue with a checkbox rather than two. Asking "do not show
		   this again?" and answering Yes/No made the reader work out a double
		   negative, and the first dialogue named the wrong button anyway: it
		   said to answer No, while the code acted on Yes. A ticked checkbox
		   means what it says. */
		wxRichMessageDialog dlg(this,
		                        "This window will now be switched to full-screen mode.\n\n"
		                        "To leave full-screen mode press Alt+Enter.",
		                        "RPCEmu Extended - Full-screen mode",
		                        wxOK | wxCANCEL | wxICON_INFORMATION);
		dlg.SetOKCancelLabels("OK", "Cancel");
		dlg.ShowCheckBox("Do not show this message again");

		if (dlg.ShowModal() != wxID_OK) {
			if (fullscreen_menu_item_ != nullptr) {
				fullscreen_menu_item_->Check(false);
			}
			return;
		}

		if (dlg.IsCheckBoxChecked()) {
			if (emulator_) {
				emulator_->ShowFullscreenMessageOff();
			}
			config_copy_.show_fullscreen_message = 0;
		}
	}

	if (panel_ != nullptr) {
		panel_->SetFullScreen(true);
	}

	/*
	 * ★ THE MENU BAR AND THE TOOL BAR ARE NOT OURS TO HIDE.
	 *
	 * ShowFullScreen(wxFULLSCREEN_ALL) hides and restores both, on GTK and on
	 * MSW, from the flags it is given. This used to hide them here as well, so
	 * one piece of state had two owners, and on Windows the second owner does
	 * not merely duplicate the first - it changes what it does. wxMSW's
	 * ShowFullScreen reads IsShown() on the way in and, for a bar that is
	 * already hidden, drops the flag "to prevent it from being restored later"
	 * (src/msw/frame.cpp). Hiding the tool bar first therefore told wx not to
	 * bring it back, and only the matching Show(true) on the way out covered
	 * for it.
	 *
	 * Issue #206 is the menu bar not coming back on Windows after a round trip.
	 * Leaving both bars to ShowFullScreen means there is one owner again and
	 * nothing to get out of step, whichever half was failing.
	 *
	 * The status bar below is the genuine exception and stays.
	 */

	/*
	 * ★ Read the bars BEFORE touching anything.
	 *
	 * Without this the log has only the state after each transition, and the
	 * two readings cannot answer the question that matters: was the menu bar
	 * detached BY going full screen, or was it never attached on this machine
	 * at all? "native menu GONE" afterwards means nothing without a "native
	 * menu attached" beforehand to compare it with.
	 *
	 * That gap is why the CI run on a Windows runner - the first time a window
	 * has ever come up in CI - could not settle issue #219 on its own. See
	 * docs/testing.md.
	 */
	LogBarState("before entering");

	/* Removed rather than hidden. Hiding it leaves the space it occupied
	   reserved - measurably so: the frame's client height does not change -
	   and on macOS that shows as an empty strip along the bottom of the
	   screen. BuildStatusBar() puts it back on the way out. And because it is
	   gone rather than hidden, wxMSW finds no status bar to reason about and
	   the flag interaction described above does not arise for it. */
	if (wxStatusBar *bar = GetStatusBar()) {
		SetStatusBar(nullptr);
		bar->Destroy();
	}
	/* ★ ShowFullScreen FIRST, then the flag - see the long note in
	   ExitFullScreen(). IsFullScreen() is overridden to answer with
	   full_screen_, and wx's own ShowFullScreen() returns immediately if it
	   already agrees with the state being asked for. */
	ShowFullScreen(true, wxFULLSCREEN_ALL);
	full_screen_ = true;
	LogBarState("entered");

	/*
	 * ★ Ask for the layout again, having changed what the frame contains.
	 *
	 * Taking the status bar away and hiding the bars changes how much room the
	 * panel should have, and ShowFullScreen() changes it again. GTK and Windows
	 * recompute that themselves; wxOSX does not always, and the panel is then
	 * left at the height it had before, which shows as a strip along the bottom
	 * of the screen exactly as deep as the status bar that used to be there -
	 * the same symptom the removal above was meant to cure, arriving by a
	 * different route.
	 *
	 * Layout() first for the sizer, then SendSizeEvent() so the panel recomputes
	 * its own scaling from the size it has actually been given.
	 */
	Layout();
	SendSizeEvent();

	/* A static guest desktop sends no fresh video update after the transition,
	   so force a full repaint once the resize has been processed - otherwise the
	   panel can be left blank until something on the guest happens to redraw. */
	if (panel_ != nullptr) {
		panel_->CallAfter([this] {
			if (panel_ != nullptr) {
				panel_->ForceRedraw();
			}
		});
	}

	/* Full-screen is just a full-screen window: leave the mouse mode alone so
	   follow-mouse (which works when scaled) keeps working here too, rather than
	   forcing the capture/relative path. */
	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}

	/*
	 * ★ Give the panel the keyboard back.
	 *
	 * Alt+Enter is the only way out of full screen: the menu bar and the tool bar
	 * are both hidden, so there is no Settings > Full Screen to click. And it is
	 * not a menu accelerator - it is handled in the frame's own key handler - so
	 * it only arrives if the panel has the focus.
	 *
	 * Hiding the menu bar and the tool bar destroys or detaches the windows the
	 * focus may have been sitting on, and ShowFullScreen() re-parents and resizes
	 * what is left. Nothing here put the focus back afterwards, so if the platform
	 * did not restore it by itself, no key reached the handler and full screen
	 * became a room with no door - which is what leaving full screen doing nothing
	 * on Windows looks like from the outside.
	 *
	 * Asked for on the way in rather than relied upon: the frame is mid-transition
	 * here, so it is deferred until the layout has settled.
	 */
	if (panel_ != nullptr) {
		panel_->CallAfter([this] {
			if (panel_ != nullptr && full_screen_) {
				panel_->FocusPanel();
			}
		});
	}

	if (fullscreen_menu_item_ != nullptr) {
		fullscreen_menu_item_->Check(true);
	}
}

void MainFrame::ExitFullScreen()
{
	if (!full_screen_) {
		return;
	}

	/* The other half of the pair; see the note in EnterFullScreen(). Taken
	   while still full screen, so the four readings across a round trip are
	   before/after entering and before/after leaving. */
	LogBarState("before leaving");

	if (panel_ != nullptr) {
		panel_->SetFullScreen(false);
	}

	/*
	 * ★ full_screen_ IS STILL SET HERE, AND MUST BE.
	 *
	 * IsFullScreen() is overridden in this class to answer with full_screen_,
	 * and it is virtual, so wx's own code asks us. wxFrame::ShowFullScreen()
	 * begins:
	 *
	 *     if ( IsFullScreen() == show )
	 *         return false;
	 *
	 * Clearing the flag before this call therefore told wx we were already out
	 * of full screen, and the whole exit path was skipped: the tool bar never
	 * got its Show(true), the menu was never re-attached with
	 * ::SetMenu(hwnd, m_hMenu), and wxTopLevelWindowMSW never restored
	 * m_fsOldWindowStyle - which is the title bar. On Windows the window came
	 * back with no title bar, no menu bar and no tool bar, and stayed that way
	 * for the rest of the session, because nothing else ever puts them back.
	 *
	 * That is issue #219, and #206 before it. The reporter's log named it: the
	 * native menu was "attached" before entering and "GONE" on all three later
	 * readings, while wx still believed the menu bar was shown. The status bar
	 * was the one thing that came back, and only because ExitFullScreen()
	 * rebuilds that itself a few lines below.
	 *
	 * It also explains the earlier report against 1.1.14 that leaving full
	 * screen "appeared to do nothing at all" - it did nothing at all - which
	 * was worked around by calling SizeWindowToGuest() below rather than by
	 * finding this.
	 *
	 * EnterFullScreen() has the mirror of this: it sets full_screen_ AFTER its
	 * ShowFullScreen(true), for the same reason. Neither order is tidiness.
	 */
	ShowFullScreen(false);

	/* Now it can be cleared: wx has finished, and SizeWindowToGuest() below
	   does nothing while it is set. */
	full_screen_ = false;
	if (GetStatusBar() == nullptr) {
		BuildStatusBar();
	}
	LogBarState("left");

	/*
	 * ★ Put the window back explicitly, rather than trusting ShowFullScreen().
	 *
	 * This used to be a bare Layout(), on the grounds that ShowFullScreen(false)
	 * had already restored the size and that fitting would shrink-wrap the frame
	 * to a panel with no minimum. Both halves of that stopped being true.
	 *
	 * The panel now has a minimum in windowed mode - a hard one at actual size,
	 * where the panel IS the guest's screen - so Fit() no longer collapses
	 * anything; and leaving the geometry to ShowFullScreen(false) meant leaving
	 * the window full-screen-sized on Windows, where leaving full screen appeared
	 * to do nothing at all. Reported against 1.1.14.
	 *
	 * Everything else that changes the window's size goes through here, so full
	 * screen is no longer the one path with its own idea of how to get back.
	 */
	SizeWindowToGuest();

	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}
	if (fullscreen_menu_item_ != nullptr) {
		fullscreen_menu_item_->Check(false);
	}
}

/*
 * What the frame's bars are actually doing, in one log line.
 *
 * Issue #206 - the menu bar not coming back after a full-screen round trip on
 * Windows - could not be seen from this end at all: RPCEMU_TEST_FULLSCREEN_AFTER
 * logged the window size at each step and nothing about the bars, so a menu bar
 * that never returned looked exactly like one that did. There is no Windows
 * machine here, so the substitute for watching it is a line the reporter can
 * send back.
 *
 * On Windows the native menu is asked for directly with ::GetMenu(). That is the
 * question that matters there, and it is not the same question as
 * wxMenuBar::IsShown(): the menu bar is an HMENU attached to the frame rather
 * than a window that is shown or hidden, so wx can believe it is showing one
 * that the frame no longer has.
 */
void MainFrame::LogBarState(const char *when) const
{
	const wxMenuBar *menu = GetMenuBar();
	const bool tool_shown = tool_bar_ != nullptr && tool_bar_->IsShown();

#ifdef __WXMSW__
	const bool native_menu = GetHWND() != nullptr &&
	    ::GetMenu((HWND) GetHWND()) != nullptr;

	rpclog("MainFrame: full screen %s - menu bar %s (native menu %s), "
	       "tool bar %s, status bar %s\n", when,
	       menu == nullptr ? "absent" : (menu->IsShown() ? "shown" : "hidden"),
	       native_menu ? "attached" : "GONE",
	       tool_shown ? "shown" : "hidden",
	       GetStatusBar() != nullptr ? "present" : "absent");
#else
	rpclog("MainFrame: full screen %s - menu bar %s, tool bar %s, "
	       "status bar %s\n", when,
	       menu == nullptr ? "absent" : (menu->IsShown() ? "shown" : "hidden"),
	       tool_shown ? "shown" : "hidden",
	       GetStatusBar() != nullptr ? "present" : "absent");
#endif
}

void MainFrame::OnFullscreen(wxCommandEvent &)
{
	if (full_screen_) {
		ExitFullScreen();
	} else {
		EnterFullScreen();
	}
}

/*
 * Show In Window: which of the three drawing rules to use.
 *
 * Note what is NOT here any more. The two old handlers each had to notice that
 * the other setting contradicted theirs, clear it, re-tick its menu item and
 * tell the emulator thread about that as well - a piece of bookkeeping that
 * existed only because two mutually exclusive settings were stored as two
 * independent flags. One value cannot contradict itself, so a radio group needs
 * none of it.
 */
/*
 * Show In Window: actual size, or whole multiples.
 *
 * Note what is not here any more. When these were two mutually exclusive
 * checkboxes, each handler had to notice that the other contradicted it, clear
 * it, re-tick its menu item and tell the emulator thread about that as well -
 * bookkeeping that existed only because one setting was stored as two flags.
 */
void MainFrame::OnDisplayScaling(wxCommandEvent &event)
{
	const int scaling = event.GetId() == ID_MENU_SCALING_MULTIPLES
	    ? DisplayScaling_WholeMultiples : DisplayScaling_ActualSize;

	if (scaling == config_copy_.display_scaling) {
		return;
	}

	config_copy_.display_scaling = scaling;
	if (scaling_menu_items_[scaling] != nullptr) {
		scaling_menu_items_[scaling]->Check(true);
	}
	if (emulator_) {
		emulator_->SetDisplayScaling(scaling);
	}
	ApplyDisplayModeToPanel();
	ForcePanelRedraw();
}

void MainFrame::OnScreenSize(wxCommandEvent &event)
{
	const int id = event.GetId();

	if (id < ID_MENU_SCREEN_FIXED_FIRST || id > ID_MENU_SCREEN_FIXED_LAST) {
		return;
	}

	const size_t index = (size_t) (id - ID_MENU_SCREEN_FIXED_FIRST);

	if (index >= fixed_mode_items_.size()) {
		return;		/* Stale id from a menu that has since been rebuilt */
	}

	const unsigned want_x = fixed_mode_items_[index].first;
	const unsigned want_y = fixed_mode_items_[index].second;

	/*
	 * Nothing to do only when the desktop is ALREADY this size, which is not the
	 * same as the configuration naming it. RISC OS changes mode from its own end,
	 * and a refused size leaves the configuration naming one it would not take,
	 * so testing the configuration alone made picking the configured size do
	 * nothing at all - with no way to ask for it again.
	 */
	const wxSize guest = panel_ != nullptr ? panel_->GuestScreenSize()
	                                       : wxSize(0, 0);

	if (guest.x == (int) want_x && guest.y == (int) want_y &&
	    want_x == config_copy_.screen_size_x &&
	    want_y == config_copy_.screen_size_y)
	{
		return;
	}

	config_copy_.screen_size_x = want_x;
	config_copy_.screen_size_y = want_y;
	if (emulator_) {
		emulator_->SetScreenSize(want_x, want_y);
	}

	/* Named explicitly, so a refusal is reported rather than quietly turned
	   into some other size. */
	RequestGuestMode(want_x, want_y, true);
}

/*
 * Whether the window's size is the user's to choose.
 *
 * Only when whole-multiple scaling is on. At actual size the window is exactly
 * the desktop, because at 1:1 any other size is either a border or a clipped
 * desktop and neither is what anybody wanted.
 */
bool MainFrame::WindowSizeIsFree() const
{
	return config_copy_.display_scaling == DisplayScaling_WholeMultiples;
}

void MainFrame::ApplyDisplayModeToPanel()
{
	if (panel_ == nullptr) {
		return;
	}

	panel_->SetDisplayMode(
	    DisplayOptions::ClampDisplayScaling(config_copy_.display_scaling));
	SizeWindowToGuest();
}

/*
 * Make the window the size of the RISC OS desktop, and centre it.
 *
 * Called when the guest's mode has settled, and when the drawing rule changes -
 * never from a video update. Both halves matter: the size, because a window
 * smaller than the desktop clips it and loses the icon bar off the bottom while a
 * larger one leaves a black border; and the centring, because a window that has
 * just changed size grows around its top-left corner and walks off the
 * bottom-right of the screen.
 */
void MainFrame::SizeWindowToGuest()
{
	if (panel_ == nullptr || full_screen_) {
		return;
	}

	if (!display_timing_wanted()) {
		panel_->SizeToGuest();
		Layout();
		Fit();
		PlaceWindowAfterResize();
		ForcePanelRedraw();
		return;
	}

	/* The same sequence, timed step by step. Kept as a separate copy rather
	   than sprinkling clocks through the live path: this runs once per mode
	   change and clarity is worth more here than the duplication costs. */
	const long t0 = display_timing_ms();

	panel_->SizeToGuest();
	const long t1 = display_timing_ms();
	Layout();
	Fit();
	const long t2 = display_timing_ms();
	PlaceWindowAfterResize();
	const long t3 = display_timing_ms();
	ForcePanelRedraw();

	rpclog("DISPLAY_TIMING %ld resized: SizeToGuest %ldms, Layout+Fit %ldms, "
	       "Centre %ldms, window now %dx%d, drawing on the %s\n",
	       t0, t1 - t0, t2 - t1, t3 - t2, GetSize().x, GetSize().y,
	       panel_->DrawingWithGpu() ? "GPU" : "CPU");
}


void MainFrame::ForcePanelRedraw()
{
	if (panel_ == nullptr) {
		return;
	}
	/* Deferred: a static guest desktop sends no fresh frame after a resize, so
	   without this the panel keeps showing the old geometry. */
	panel_->CallAfter([this] {
		if (panel_ != nullptr) {
			panel_->ForceRedraw();
		}
	});
}

/*
 * The fixed screen sizes on offer, which depend on the machine.
 *
 * Only modes this machine's display memory can hold: offering one it cannot show
 * would put us back where this redesign started, with a control that looks as
 * though it works and then does not - RISC OS answers "not suitable for
 * displaying the desktop" and the user is none the wiser.
 */
void MainFrame::RebuildScreenSizeMenu()
{
	if (screen_size_menu_ == nullptr) {
		return;
	}

	std::vector<std::pair<unsigned, unsigned>> modes;
	DisplayOptions::FixedModes(rpcemu_display_memory(), modes);

	const size_t limit =
	    (size_t) (ID_MENU_SCREEN_FIXED_LAST - ID_MENU_SCREEN_FIXED_FIRST) + 1;
	if (modes.size() > limit) {
		modes.resize(limit);
	}

	/* Rebuilt rather than patched: the list is short, and working out which
	   entries moved is more code than throwing them away. */
	for (size_t i = 0; i < fixed_mode_items_.size(); i++) {
		screen_size_menu_->Delete(
		    (int) (ID_MENU_SCREEN_FIXED_FIRST + (int) i));
	}
	fixed_mode_items_.clear();

	for (size_t i = 0; i < modes.size(); i++) {
		wxMenuItem *item = screen_size_menu_->AppendRadioItem(
		    (int) (ID_MENU_SCREEN_FIXED_FIRST + (int) i),
		    DisplayOptions::ModeLabel(modes[i].first, modes[i].second));

		item->SetHelp(DisplayOptions::ScreenSizeHelp());
		fixed_mode_items_.push_back(modes[i]);
	}

	/*
	 * Tick the size the desktop actually is, falling back to the configured one
	 * until a frame has said otherwise.
	 *
	 * The desktop rather than the configuration, because the two part company
	 * readily: RISC OS can be given a mode from its own end, and a size it
	 * refuses leaves the configuration naming one it would not take. A tick
	 * against either would be pointing at a mode that is not on the screen.
	 *
	 * Nothing ticked is a real answer and is left to happen: the desktop is in a
	 * mode this list does not offer, which is what a guest that has gone its own
	 * way looks like, and claiming the nearest entry would be a lie.
	 */
	const wxSize shown = panel_ != nullptr ? panel_->GuestScreenSize()
	                                       : wxSize(0, 0);
	const unsigned tick_x = shown.x > 0 ? (unsigned) shown.x
	                                    : config_copy_.screen_size_x;
	const unsigned tick_y = shown.y > 0 ? (unsigned) shown.y
	                                    : config_copy_.screen_size_y;

	for (size_t i = 0; i < fixed_mode_items_.size(); i++) {
		if (fixed_mode_items_[i].first == tick_x &&
		    fixed_mode_items_[i].second == tick_y)
		{
			screen_size_menu_->Check(
			    (int) (ID_MENU_SCREEN_FIXED_FIRST + (int) i), true);
			return;
		}
	}
}

/*
 * The window was resized, and the guest is following it.
 *
 * Debounced, because a drag fires this continuously and each mode change reflows
 * every window on the RISC OS desktop. Publishing every intermediate width would
 * have the guest working its way through modes it is about to leave, which looks
 * and feels like a fault. Only the size the drag settles on is sent.
 */

/*
 * A frame arrived from the guest.
 *
 * Deliberately does almost nothing. The window's size comes from the
 * configuration and not from the guest, so a mode change - and RISC OS makes
 * several while it boots - is simply drawn, centred, inside the window that is
 * already there. Nothing is resized and nothing is moved.
 *
 * The one thing worth noticing is the desktop reaching the size that was asked
 * for, which is how a refused mode is told from an accepted one.
 */
/*
 * How long to give the guest to change mode before deciding it will not.
 *
 * The support module polls a few times a second and then queues a callback that
 * runs *WimpMode, and RISC OS itself takes a moment to reflow the desktop. Two
 * seconds is comfortably longer than that and short enough that a refused mode
 * does not feel like a hang.
 */
static const int kModeVerifyMs = 2000;

/*
 * Put the window in the middle of the display, with its title bar reachable.
 *
 * Called after anything that changes the window's size - the machine starting,
 * the screen size being changed, the drawing rule being changed - and at no other
 * time. Nothing here reacts to the guest, which is the point: the window used to
 * be resized and re-centred every time RISC OS changed mode while booting, and
 * watching a window jump around three times before settling is not a startup
 * anybody wants.
 *
 * The clamp matters as much as the centring. A window taller than the work area,
 * centred, has its title bar above the top of the screen where it cannot be
 * grabbed - so the window can then be neither moved nor resized back. Pinned to
 * the top-left of the work area instead, the title bar is always in reach.
 */
void MainFrame::CentreWindowOnScreen()
{
	int index = wxDisplay::GetFromWindow(this);

	if (index == wxNOT_FOUND) {
		index = 0;
	}

	const wxRect work = wxDisplay((unsigned) index).GetClientArea();
	const wxSize size = GetSize();

	if (size.x <= work.width && size.y <= work.height) {
		SetPosition(wxPoint(work.x + (work.width - size.x) / 2,
		                    work.y + (work.height - size.y) / 2));
	} else {
		SetPosition(work.GetTopLeft());
	}
}

/*
 * Keep the window where the user put it, and on the screen.
 *
 * A guest mode change resizes the window, and a window grows around its
 * top-left corner, so without this a bigger mode walks off the bottom-right.
 * Centring solved that and created another problem: every mode change moved
 * the window, so a Reset - which goes through several modes on its way up -
 * took it back to the middle of the screen whatever the user had done with it.
 * Reported by JonAbbott2 on discussion #223.
 *
 * So the corner is kept and only pulled back when the new size would hang off
 * an edge. The first placement still centres, because a window that has never
 * been positioned has no corner worth keeping.
 */
/*
 * Centre the first time, keep the corner afterwards. See KeepWindowOnScreen().
 */
void MainFrame::PlaceWindowAfterResize()
{
	if (!window_placed_) {
		CentreWindowOnScreen();
		window_placed_ = true;
		return;
	}
	KeepWindowOnScreen();
}

void MainFrame::KeepWindowOnScreen()
{
	int index = wxDisplay::GetFromWindow(this);

	if (index == wxNOT_FOUND) {
		index = 0;
	}

	const wxRect work = wxDisplay((unsigned) index).GetClientArea();
	const wxSize size = GetSize();
	const wxPoint was = GetPosition();
	wxPoint now = was;

	if (size.x > work.width || size.y > work.height) {
		/* Bigger than the screen either way: the top-left corner is the only
		   part that can be seen, so put it there. */
		SetPosition(work.GetTopLeft());
		return;
	}

	if (now.x + size.x > work.x + work.width) {
		now.x = work.x + work.width - size.x;
	}
	if (now.y + size.y > work.y + work.height) {
		now.y = work.y + work.height - size.y;
	}
	if (now.x < work.x) {
		now.x = work.x;
	}
	if (now.y < work.y) {
		now.y = work.y;
	}
	if (now != was) {
		SetPosition(now);
	}
}

void MainFrame::RequestGuestMode(unsigned width, unsigned height,
                                 bool explicit_choice)
{
	unsigned fitted_w = 0, fitted_h = 0;

	if (width == 0 || height == 0) {
		return;
	}

	/* Ask the core what this rounds to, so the size being waited for is the one
	   the guest will actually be asked for. */
	if (!rpcemu_guest_size_for(width, height, &fitted_w, &fitted_h)) {
		if (explicit_choice) {
			wxMessageBox(
			    "No screen mode this machine can display is small enough for "
			    "that.\n\nFit more VRAM, or the graphics card, for the larger "
			    "modes.",
			    "Screen Size", wxOK | wxICON_INFORMATION, this);
		}
		return;
	}

	/* A named size is forced through: see rpcemu_request_guest_size_ex(). A
	   window drag is not, so its quantising still holds. */
	rpcemu_request_guest_size_ex(fitted_w, fitted_h, explicit_choice ? 1 : 0);

	/*
	 * Only a size the user named is checked.
	 *
	 * Checking the one made at startup was a bad mistake: the request goes out
	 * before RISC OS has a desktop, so two seconds later the guest is still part
	 * way through booting and is obviously not in the requested mode. That read
	 * as a refusal, struck a perfectly good size off the list, and dragged the
	 * window down to whatever transient boot mode happened to be on screen.
	 *
	 * There is nothing to check at startup anyway. Nobody is waiting on an answer,
	 * and the window follows whatever the guest settles into regardless.
	 */
	if (!explicit_choice) {
		mode_requested_ = wxSize(0, 0);
		return;
	}

	mode_requested_ = wxSize((int) fitted_w, (int) fitted_h);
	mode_verify_timer_.StartOnce(kModeVerifyMs);
}

/*
 * The guest was asked for a mode. Did it take it?
 *
 * A refusal is invisible from here - RISC OS reports "this screen mode is
 * unsuitable for displaying the desktop" on its own screen and the host is told
 * nothing at all - so the only evidence is the desktop still being the size it
 * was. Measured on a machine with the graphics card fitted, seven of the thirteen
 * modes that fit its display memory were refused, because the monitor definition
 * in force was a definition file rather than the synthesised EDID. Nothing on the
 * host can know which modes a given definition declares, so they are learned.
 */
void MainFrame::OnModeVerifyTimer(wxTimerEvent &event)
{
	(void)event;

	/* Only ever reached for a size the user named: RequestGuestMode() starts this
	   timer for nothing else, so there is no "was this explicit" to test. */

	if (panel_ == nullptr || mode_requested_ == wxSize(0, 0)) {
		return;
	}

	const wxSize requested = mode_requested_;
	const wxSize guest = panel_->GuestScreenSize();

	mode_requested_ = wxSize(0, 0);

	if (guest == requested) {
		return;		/* Taken */
	}

	rpclog("Display: the guest refused %dx%d\n", requested.x, requested.y);

	/*
	 * Two quite different faults arrive here and they need opposite advice, so
	 * ask the block the guest is actually reading which one this is.
	 *
	 * If our own EDID does not declare the size, the refusal is ours: the block
	 * was built around the size configured at the time, and the one just picked
	 * is now stored but will not be advertised until the monitor is rebuilt at
	 * the next start. A restart is the cure and the size must stay in the menu,
	 * because it is the size this machine is now configured for.
	 *
	 * If the block DOES declare it and RISC OS still said no, the monitor
	 * definition in force is not ours - the guest's !Boot has loaded a
	 * definition file over the top - and no restart will help. That is the case
	 * this dialogue was originally written for.
	 */
	const uint8_t *in_force = edid_published();
	const bool ours_declares_it =
	    in_force != nullptr &&
	    edid_block_declares(in_force, (unsigned) requested.x,
	                        (unsigned) requested.y) != 0;

	if (!ours_declares_it) {
		rpclog("Display: %dx%d is not declared by the monitor EDID in force; "
		       "stored, and offered again after a restart\n",
		       requested.x, requested.y);

		const wxString needs_restart = wxString::Format(
		    "RISC OS would not switch to %d x %d yet, and is still using "
		    "%d x %d.\n\n"
		    "This machine's monitor was set up when it started, for the "
		    "screen size in force then, and it does not offer this size. "
		    "The setting has been saved.\n\n"
		    "Restart the machine and it will start at %d x %d.",
		    requested.x, requested.y,
		    guest.x > 0 ? guest.x : requested.x,
		    guest.y > 0 ? guest.y : requested.y,
		    requested.x, requested.y);

		wxMessageBox(needs_restart, "Screen Size Needs A Restart",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	/*
	 * Struck off the list.
	 *
	 * RISC OS accepts only the screen modes the monitor definition in force
	 * declares. That definition is usually a monitor definition file the guest's
	 * own !Boot loads - not the EDID this emulator synthesises - and nothing on
	 * the host can read what it contains. Measured on a machine with the graphics
	 * card fitted, seven of the thirteen modes that fit its display memory were
	 * refused, and not the ones anybody would guess: 1920x1200 accepted,
	 * 1920x1080 refused.
	 *
	 * Nothing else needs doing. The window is the size of whatever the desktop
	 * actually is, so a refusal leaves it where it was rather than leaving a
	 * border or a clipped desktop behind.
	 */
	display_mode_mark_unavailable((unsigned) requested.x, (unsigned) requested.y);
	RebuildScreenSizeMenu();

	/*
	 * Said once, plainly, with the cause and the cure.
	 *
	 * The cure is real and worth spelling out: this machine has MonitorType
	 * configured as EDID, and RISC OS is nevertheless using a definition file,
	 * because its !Boot loads one over the top. Remove that and the emulator's
	 * own monitor definition applies, which declares the whole list.
	 *
	 * Note what is NOT offered: a restart. This branch is reached only when our
	 * own EDID DOES declare the size and RISC OS refused it anyway, so the
	 * definition in force is the guest's own and restarting changes nothing -
	 * it would send somebody round a loop that does not arrive. The case where a
	 * restart IS the cure is handled above, before this point.
	 */
	wxMessageBox(
	    wxString::Format(
	        "RISC OS would not switch to %d x %d, and is still using %d x %d.\n\n"
	        "It only accepts screen modes the monitor definition it has loaded "
	        "declares, and that one does not include this size, so it has been "
	        "taken out of the list.\n\n"
	        "To get the full set of sizes, stop RISC OS loading its own monitor "
	        "definition file: in Configure, set the monitor type to Auto or EDID "
	        "rather than a definition file, then restart the machine. The "
	        "emulator's own monitor definition offers every size in the list.",
	        requested.x, requested.y,
	        guest.x > 0 ? guest.x : requested.x,
	        guest.y > 0 ? guest.y : requested.y),
	    "Screen Size Not Available", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::NoteGuestFrame()
{
	/*
	 * How long the guest's screen mode has to hold still before the window is
	 * resized to it.
	 *
	 * TWO DIFFERENT WAITS, because there are two different things going on and
	 * one number cannot serve both.
	 *
	 * While the machine is starting, RISC OS changes mode several times in quick
	 * succession - and the graphics card hands the display over in the middle of
	 * them - so the window has to wait for the run to finish or it marches
	 * through three or four sizes on the way to the desktop. That is what the
	 * half second was for, and it is still right during a boot.
	 *
	 * A mode change afterwards is a deliberate one. It arrives on its own,
	 * whether from the Screen Size menu or from *WimpMode or the Display
	 * manager inside RISC OS, and the whole half second is then dead time the
	 * user sits through: measured here on RISC OS 4.39, the guest adopts a new
	 * mode in about 300ms and the window then waited another 500 before
	 * following, at every size from 800x600 to 1920x1440. Resizing the window
	 * itself takes 2ms. Issue #220.
	 *
	 * The short wait is not zero because a deliberate change can still be seen
	 * as two steps if a frame arrives mid-switch, and 80ms costs nothing that
	 * can be felt.
	 */
	static const int kGuestSettleBootMs = 500;
	static const int kGuestSettleMs = 80;

	/* How long after starting or resetting a machine mode changes are still
	   treated as part of its boot. Generous: the cost of being wrong this way
	   is one extra window resize, and the cost of being wrong the other way is
	   the window marching through the boot's modes. */
	static const long kBootWindowMs = 20000;

	if (panel_ == nullptr) {
		return;
	}

	const wxSize guest = panel_->GuestScreenSize();

	if (guest.x <= 0 || guest.y <= 0 || guest == guest_size_seen_) {
		return;
	}
	guest_size_seen_ = guest;

	const bool booting = machine_started_ms_ == 0 ||
	    (wxGetLocalTimeMillis() - machine_started_ms_).GetValue() < kBootWindowMs;

	const int settle = booting ? kGuestSettleBootMs : kGuestSettleMs;

	if (display_timing_wanted()) {
		rpclog("DISPLAY_TIMING %ld guest mode now %dx%d, waiting %dms (%s)\n",
		       display_timing_ms(), guest.x, guest.y, settle,
		       booting ? "still booting" : "a deliberate change");
	}

	guest_resize_timer_.StartOnce(settle);

	/* The tick follows the desktop, so it has to move when RISC OS changes mode
	   from its own end and not only when the change was asked for here. */
	RebuildScreenSizeMenu();
}

void MainFrame::OnGuestResizeTimer(wxTimerEvent &event)
{
	(void)event;
	SizeWindowToGuest();
}

void MainFrame::OnTestFullscreenTimer(wxTimerEvent &event)
{
	(void)event;

	const wxSize before = GetSize();
	const wxSize guest = panel_ != nullptr ? panel_->GuestScreenSize()
	                                       : wxSize(0, 0);

	switch (test_fullscreen_step_++) {
	case 0:
		rpclog("TEST_FULLSCREEN: window %dx%d, guest %dx%d - entering\n",
		       before.x, before.y, guest.x, guest.y);
		/* Not through the dialogue: that would need answering. */
		config_copy_.show_fullscreen_message = 0;
		EnterFullScreen();
		rpclog("TEST_FULLSCREEN: full screen, window now %dx%d\n",
		       GetSize().x, GetSize().y);
		test_fullscreen_timer_.StartOnce(5000);
		break;

	case 1:
		rpclog("TEST_FULLSCREEN: window %dx%d - leaving\n", before.x, before.y);
		ExitFullScreen();
		rpclog("TEST_FULLSCREEN: left, window now %dx%d, guest %dx%d\n",
		       GetSize().x, GetSize().y, guest.x, guest.y);
		break;

	default:
		break;
	}
}

/*
 * The Podules tab, measured rather than described (issue #254).
 *
 * The dialog is built but never shown: everything being checked is decided in
 * its constructor, and a modal dialog with nobody to close it would hang the
 * run. It reports each slot to the machine's rpclog.txt, and then the same
 * again with the graphics card switched on, because the whole point of the fix
 * is that the built-in cards move when the machine's configuration changes.
 */
void MainFrame::OnTestPodulesTimer(wxTimerEvent &event)
{
	(void) event;

	const wxString config_path =
	    ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path()));

	MachineEditDialog dlg(this, config_path,
	                      emulator_ == nullptr || !emulator_->IsRunning(),
	                      emulator_ != nullptr && emulator_->IsRunning());

	dlg.LogPoduleRows("as configured");
	dlg.TestSetGfxCard(true);
	dlg.LogPoduleRows("with the graphics card switched on");
	dlg.TestSetGfxCard(false);
	dlg.LogPoduleRows("and switched off again");

	rpclog("TEST_PODULES: done\n");
	dlg.Destroy();
}

void MainFrame::OnTestInspectorTimer(wxTimerEvent &event)
{
	(void)event;

	switch (test_inspector_step_++) {
	case 0: {
		/*
		 * Something for the controls to disagree with. Set through the emulator
		 * the way the debug socket would, so this is the machine's own state and
		 * not something the window was told.
		 */
		DebugTraceConfig cfg{};

		cfg.trap_data_abort = 1;
		cfg.swi_trace_enabled = 1;
		cfg.swi_filter_max = 0xffffffffu;
		emulator_->SetDebugTraceConfig(cfg);
		rpclog("TEST_INSPECTOR: machine set to trap data aborts and log SWIs\n");
		test_inspector_timer_.StartOnce(2000);
		break;
	}

	case 1: {
		wxCommandEvent open(wxEVT_MENU);

		OnMachineInspector(open);
		rpclog("TEST_INSPECTOR: opened\n");
		test_inspector_timer_.StartOnce(2000);
		break;
	}

	case 2:
		if (machine_inspector_window_ != nullptr) {
			machine_inspector_window_->LogTraceControlsAgainstMachine("first open");
			/* Somewhere distinctive to be found again after the reopen, which
			   is how "remember where the window was" gets checked at all. */
			test_inspector_position_ =
			    machine_inspector_window_->GetPosition() + wxPoint(37, 53);
			machine_inspector_window_->SetPosition(test_inspector_position_);
			rpclog("TEST_INSPECTOR: moved to %d,%d before closing\n",
			    test_inspector_position_.x, test_inspector_position_.y);
			machine_inspector_window_->Close(true);
			rpclog("TEST_INSPECTOR: closed\n");
		} else {
			rpclog("TEST_INSPECTOR: no inspector window to close\n");
		}
		test_inspector_timer_.StartOnce(2000);
		break;

	case 3: {
		/* Destroy() is deferred, so the window from step 2 has gone by now and
		   this really is a new one - which is the case #221 is about. */
		wxCommandEvent open(wxEVT_MENU);

		OnMachineInspector(open);
		rpclog("TEST_INSPECTOR: reopened\n");
		test_inspector_timer_.StartOnce(2000);
		break;
	}

	case 4:
		if (machine_inspector_window_ != nullptr) {
			machine_inspector_window_->LogTraceControlsAgainstMachine("reopened");
			/* Issue #258: the memory view read physical addresses while every
			   other address in the window meant a virtual one. Needs a booted
			   machine, because it needs real page tables. */
			machine_inspector_window_->LogMemoryViewAgainstMachine("reopened");
			/* Discussion #257: the FPA's registers, which the debugger could
			   not show at all until they were plumbed through the snapshot. */
			machine_inspector_window_->LogFpRegistersAgainstMachine("reopened");
			machine_inspector_window_->LogMemoryWordsAgainstBytes("reopened");
			machine_inspector_window_->LogDisassemblyFollowsHalt("reopened");
			machine_inspector_window_->LogEditsAgainstMachine("reopened");
			{
				const wxPoint at = machine_inspector_window_->GetPosition();

				rpclog("TEST_INSPECTOR: reopened at %d,%d, was moved to %d,%d%s\n",
				    at.x, at.y, test_inspector_position_.x,
				    test_inspector_position_.y,
				    at == test_inspector_position_ ? "" : "  <- DID NOT COME BACK");
			}
		} else {
			rpclog("TEST_INSPECTOR: no inspector window on reopen\n");
		}
		test_inspector_timer_.StartOnce(2000);
		break;

	case 5:
		/*
		 * Auto-step and the session file. Both are window behaviour - a timer
		 * and two file dialogs - so this is the only place they meet a real
		 * machine. Asked for in discussion #223.
		 */
		if (machine_inspector_window_ != nullptr) {
			const wxString path = wxFileName::CreateTempFileName("rpcdbg");

			machine_inspector_window_->TestAutoStep(20, 25);
			rpclog("TEST_INSPECTOR: session round trip %s\n",
			    machine_inspector_window_->TestSessionRoundTrip(path)
			        ? "ok" : "FAILED");
			wxRemoveFile(path);
		} else {
			rpclog("TEST_INSPECTOR: no inspector window for the session test\n");
		}
		rpclog("TEST_INSPECTOR: done\n");
		break;

	default:
		break;
	}
}

void MainFrame::OnTestCloseTimer(wxTimerEvent &event)
{
	(void)event;
	rpclog("MainFrame: RPCEMU_TEST_CLOSE_AFTER - requesting close\n");
	Close(false);	/* false: may be vetoed, exactly like the close button */
}


void MainFrame::OnCpuIdle(wxCommandEvent &event)
{
	if (!HostResetQuestion(this)) {
		if (cpu_idle_menu_item_ != nullptr) {
			cpu_idle_menu_item_->Check(config_copy_.cpu_idle != 0);
		}
		return;
	}

	if (emulator_) {
		emulator_->CpuIdle();
	}
	config_copy_.cpu_idle ^= 1;
	if (cpu_idle_menu_item_ != nullptr) {
		cpu_idle_menu_item_->Check(config_copy_.cpu_idle != 0);
	}
	(void)event;
}

void MainFrame::OnMouseHack(wxCommandEvent &event)
{
	if (emulator_) {
		emulator_->MouseHack();
	}
	config_copy_.mousehackon ^= 1;
	if (mouse_hack_menu_item_ != nullptr) {
		mouse_hack_menu_item_->Check(config_copy_.mousehackon != 0);
	}
	if (config_copy_.mousehackon) {
		mouse_captured = 0;
	}
	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}
	UpdateMachineStatus();
	(void)event;
}

void MainFrame::OnMouseTwobutton(wxCommandEvent &event)
{
	if (emulator_) {
		emulator_->MouseTwobutton();
	}
	config_copy_.mousetwobutton ^= 1;
	if (mouse_twobutton_menu_item_ != nullptr) {
		mouse_twobutton_menu_item_->Check(config_copy_.mousetwobutton != 0);
	}
	(void)event;
}

/*
 * Settings -> Share Clipboard with RISC OS.
 */
wxString MainFrame::CurrentMachineBaseName() const
{
	return wxFileName(wxString::FromUTF8(config_get_path())).GetName();
}

/**
 * Make this the machine RPCEmu opens on startup, or stop it being so.
 *
 * The same setting as the machine selector's Set as Default, reachable from
 * inside a running machine - which matters, because once a machine opens
 * automatically the selector is not shown, and this is the way to turn it off
 * without knowing that holding Shift brings the selector back.
 */
void MainFrame::OnDefaultMachine(wxCommandEvent &)
{
	const wxString name = CurrentMachineBaseName();
	const bool was_default = wxString::FromUTF8(GetDefaultMachine()) == name;

	if (was_default || name.empty()) {
		ClearDefaultMachine();
	} else {
		SetDefaultMachine(name.utf8_string());
	}
	if (default_machine_menu_item_ != nullptr) {
		default_machine_menu_item_->Check(!was_default && !name.empty());
	}
}

void MainFrame::OnSharedClipboard(wxCommandEvent &event)
{
	if (emulator_) {
		emulator_->SetClipboardEnabled();
	}
	config_copy_.clipboard_enabled ^= 1;
	if (shared_clipboard_menu_item_ != nullptr) {
		shared_clipboard_menu_item_->Check(config_copy_.clipboard_enabled != 0);
	}
	if (config_copy_.clipboard_enabled) {
		/* Take whatever is on the host clipboard now, rather than waiting for
		   it to change. */
		clipboard_last_seen_.clear();
	}
	(void) event;
}

/*
 * Encode a bitmap as a PNG in memory, which is the form the guest is given: its
 * clipboard module asks for PNG, and RISC OS applications read it.
 */
static std::string EncodeBitmapAsPng(const wxBitmap &bitmap)
{
	wxImage image = bitmap.ConvertToImage();
	wxMemoryOutputStream out;

	if (!image.IsOk() || !image.SaveFile(out, wxBITMAP_TYPE_PNG)) {
		return std::string();
	}

	const wxStreamBuffer *buffer = out.GetOutputStreamBuffer();
	return std::string(static_cast<const char *>(buffer->GetBufferStart()),
	                   buffer->GetIntPosition());
}

/*
 * Look at the host clipboard, and if what is on it has changed since we last
 * passed it on, hand it to the emulator thread for the guest.
 *
 * Text is preferred over an image when both are offered, because an application
 * that copies text often puts a rendered picture of it alongside, and text is
 * what was meant. The image is compared as the encoded PNG rather than by asking
 * the clipboard whether it has changed, which it cannot tell us.
 */
void MainFrame::OnClipboardTimer(wxTimerEvent &)
{
	if (!config_copy_.clipboard_enabled || emulator_ == nullptr) {
		return;
	}
	if (!wxTheClipboard->Open()) {
		return;		/* another application has it; try again next time */
	}

	wxString text;
	wxBitmap bitmap;
	if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
		wxTextDataObject data;
		if (wxTheClipboard->GetData(data)) {
			text = data.GetText();
		}
	} else if (wxTheClipboard->IsSupported(wxDF_BITMAP)) {
		wxBitmapDataObject data;
		if (wxTheClipboard->GetData(data)) {
			bitmap = data.GetBitmap();
		}
	}
	wxTheClipboard->Close();

	if (!text.empty()) {
		if (text == clipboard_last_seen_) {
			return;
		}
		clipboard_last_seen_ = text;
		clipboard_image_last_seen_.clear();
		emulator_->HostClipboardChanged(CLIPBOARD_TYPE_TEXT,
		                                std::string(text.utf8_str()));
		return;
	}

	if (bitmap.IsOk()) {
		const std::string png = EncodeBitmapAsPng(bitmap);

		if (png.empty() || png == clipboard_image_last_seen_) {
			return;
		}
		clipboard_image_last_seen_ = png;
		clipboard_last_seen_.clear();
		emulator_->HostClipboardChanged(CLIPBOARD_TYPE_PNG, png);
	}
}

/*
 * The guest has copied something. Called from the emulator thread, so the
 * clipboard itself is touched on the GUI thread.
 */
void MainFrame::PostSetHostClipboard(const std::string &utf8)
{
	const wxString text = wxString::FromUTF8(utf8.c_str(), utf8.size());

	CallAfter([this, text]() {
		if (!wxTheClipboard->Open()) {
			return;
		}
		wxTheClipboard->SetData(new wxTextDataObject(text));
		wxTheClipboard->Close();
		/* Ours now: do not read it straight back and send it to the guest
		   again. */
		clipboard_last_seen_ = text;
		clipboard_image_last_seen_.clear();
	});
}

/*
 * The guest has copied an image. It arrives as the encoded file - a PNG or a
 * JPEG - which is decoded here into a bitmap, because that is the form host
 * applications take from the clipboard.
 *
 * Called from the emulator thread, so the clipboard itself is touched on the GUI
 * thread.
 */
void MainFrame::PostSetHostClipboardImage(int file_type, const std::string &bytes)
{
	const wxBitmapType type = (file_type == CLIPBOARD_TYPE_JPEG)
	                          ? wxBITMAP_TYPE_JPEG : wxBITMAP_TYPE_PNG;
	const std::string data = bytes;

	CallAfter([this, data, type]() {
		wxMemoryInputStream in(data.data(), data.size());
		wxImage image;

		if (!image.LoadFile(in, type)) {
			rpclog("Clipboard: the guest's image could not be decoded\n");
			return;
		}
		if (!wxTheClipboard->Open()) {
			return;
		}
		wxTheClipboard->SetData(new wxBitmapDataObject(wxBitmap(image)));
		wxTheClipboard->Close();

		/* Ours now: do not read it straight back and send it to the guest
		   again. Compared as a PNG, so re-encode it the way the timer will. */
		clipboard_last_seen_.clear();
		clipboard_image_last_seen_ = EncodeBitmapAsPng(wxBitmap(image));
	});
}

void MainFrame::OnDebugRun(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->DebuggerResume();
	}
	CallAfter([this]() { UpdateDebuggerActionStates(); });
}

void MainFrame::OnDebugPause(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->DebuggerPause();
	}
	CallAfter([this]() { UpdateDebuggerActionStates(); });
}

void MainFrame::OnDebugStep(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->DebuggerStep();
	}
	CallAfter([this]() { UpdateDebuggerActionStates(); });
}

void MainFrame::OnDebugStep5(wxCommandEvent &)
{
	if (emulator_) {
		emulator_->DebuggerStepN(5);
	}
	CallAfter([this]() { UpdateDebuggerActionStates(); });
}

void MainFrame::OnMachineInspector(wxCommandEvent &)
{
	if (machine_inspector_window_ == nullptr) {
		machine_inspector_window_ = new MachineInspectorWindow(this, *emulator_);
		machine_inspector_window_->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent &) {
			machine_inspector_window_ = nullptr;
		});
	}
	machine_inspector_window_->ShowAndRaise();
}

void MainFrame::OnOnlineManual(wxCommandEvent &)
{
	wxLaunchDefaultBrowser(URL_MANUAL);
}

void MainFrame::OnVisitWebsite(wxCommandEvent &)
{
	wxLaunchDefaultBrowser(URL_WEBSITE);
}

void MainFrame::OnReportIssue(wxCommandEvent &)
{
	wxLaunchDefaultBrowser(URL_ISSUES);
}

void MainFrame::OnSupportBundle(wxCommandEvent &)
{
	wxString screenshot;

	if (panel_ != nullptr) {
		const wxString temp = wxFileName::CreateTempFileName("rpcemu-screen");

		if (!temp.empty() && panel_->SaveScreenshot(temp)) {
			screenshot = temp;
		} else if (!temp.empty()) {
			wxRemoveFile(temp);
		}
	}

	if (!screenshot.empty() &&
	    wxMessageBox("Include a screenshot of the RISC OS screen?\n\n"
	                 "It shows what the machine was displaying just now, and "
	                 "anyone reading the report will see it.",
	                 "RPCEmu Extended - Support Files",
	                 wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
		wxRemoveFile(screenshot);
		screenshot.clear();
	}

	wxFileDialog dlg(this, "Save Support Files",
	    wxStandardPaths::Get().GetDocumentsDir(), SupportBundleSuggestedName(),
	    "Zip archives (*.zip)|*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dlg.ShowModal() != wxID_OK) {
		if (!screenshot.empty()) {
			wxRemoveFile(screenshot);
		}
		return;
	}

	const SupportBundleResult result = SupportBundleWrite(dlg.GetPath(), screenshot);

	if (!screenshot.empty()) {
		wxRemoveFile(screenshot);
	}

	if (!result.ok) {
		wxMessageBox(result.message, "RPCEmu Extended - Support Files",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	/* Listed rather than summarised: this is going to be attached to a public
	   issue, and what was put in it should not have to be taken on trust. */
	wxString detail;

	for (const SupportBundleMember &member : result.members) {
		detail += wxString::Format("    %s\n", member.name);
	}

	wxMessageBox(
	    wxString::Format("Saved to:\n%s\n\nIt contains:\n%s\n"
	                     "The password and any home folder paths have been "
	                     "taken out of the log and the settings.",
	        dlg.GetPath(), detail),
	    "RPCEmu Extended - Support Files", wxOK | wxICON_INFORMATION, this);
}

namespace {

/* "v1.1.12" -> 1, 1, 12. Anything unparsable leaves the fields at zero, which
   compares as older than any real release and so offers the update. */
void SplitVersion(const wxString &text, long out[3])
{
	wxString rest = text;

	out[0] = out[1] = out[2] = 0;
	if (rest.StartsWith("v")) {
		rest = rest.Mid(1);
	}
	for (int i = 0; i < 3 && !rest.empty(); i++) {
		wxString field = rest.BeforeFirst('.');

		field.ToLong(&out[i]);
		rest = rest.AfterFirst('.');
	}
}

/* Numerically, so 1.1.9 is older than 1.1.10 rather than sorting after it. */
bool IsNewerVersion(const wxString &candidate, const wxString &current)
{
	long a[3], b[3];

	SplitVersion(candidate, a);
	SplitVersion(current, b);
	for (int i = 0; i < 3; i++) {
		if (a[i] != b[i]) {
			return a[i] > b[i];
		}
	}
	return false;
}

/* The one field wanted out of the release JSON. The response has several
   html_url members - the release, the uploader, others - so the tag is taken
   and the page built from it rather than trusting their order. */
wxString TagNameFromReleaseJson(const wxString &json)
{
	const wxString key = "\"tag_name\"";
	const int at = json.Find(key);

	if (at == wxNOT_FOUND) {
		return wxString();
	}

	wxString rest = json.Mid(static_cast<size_t>(at) + key.length());
	rest = rest.AfterFirst(':').Strip(wxString::both);
	if (!rest.StartsWith("\"")) {
		return wxString();
	}
	return rest.Mid(1).BeforeFirst('"');
}

} // namespace

void MainFrame::OnCheckUpdate(wxCommandEvent &)
{
	const wxString current = VERSION;

	/* A development build is ahead of the newest release, not behind it, so
	   there is nothing useful to compare against. */
	if (current.Contains("-")) {
		if (wxMessageBox(
		        wxString::Format(
		            "This is a development build (%s), not a release.\n\n"
		            "Open the releases page?", current),
		        "RPCEmu Extended - Check for Update",
		        wxOK | wxCANCEL | wxICON_INFORMATION, this) == wxOK) {
			wxLaunchDefaultBrowser(URL_RELEASES);
		}
		return;
	}

	if (!RPCEMU_HAVE_HTTP) {
		wxMessageBox(HttpUnavailableMessage(), "RPCEmu Extended - Check for Update",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	wxString body, error;
	{
		RiscosFetchProgressReporter reporter(this);
		Transfer transfer(reporter, "Checking for a newer release...", {});

		if (!transfer.ToMemory(URL_LATEST_RELEASE_API)) {
			if (transfer.WasCancelled()) {
				return;
			}
			error = transfer.Error();
		} else {
			body = transfer.Body();
		}
	}

	if (!error.empty()) {
		wxMessageBox(wxString::Format("Could not check for an update:\n\n%s", error),
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_ERROR, this);
		return;
	}

	const wxString tag = TagNameFromReleaseJson(body);
	if (tag.empty()) {
		wxMessageBox("The reply from GitHub did not name a release.",
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_ERROR, this);
		return;
	}

	if (!IsNewerVersion(tag, current)) {
		wxMessageBox(wxString::Format("You are running the latest version (%s).", current),
		             "RPCEmu Extended - Check for Update", wxOK | wxICON_INFORMATION, this);
		return;
	}

	if (wxMessageBox(
	        wxString::Format(
	            "RPCEmu Extended %s is available. You are running %s.\n\n"
	            "Open the release page to read the notes and download it?",
	            tag, current),
	        "RPCEmu Extended - Check for Update",
	        wxOK | wxCANCEL | wxICON_INFORMATION, this) == wxOK) {
		wxLaunchDefaultBrowser(wxString(URL_RELEASE_TAG) + tag);
	}
}

/* RISC OS is not ours, and this is where it comes from. */
void MainFrame::OnAboutRiscos(wxCommandEvent &)
{
	wxLaunchDefaultBrowser(URL_RISCOSOPEN);
}

void MainFrame::OnAbout(wxCommandEvent &)
{
	if (panel_ != nullptr) {
		panel_->ReleaseMouseCapture();
	}

	AboutDialog dlg(this);
	dlg.ShowModal();
}

#ifdef RPCEMU_VNC
void MainFrame::OnVnc(wxCommandEvent &)
{
	if (!vnc_server_) {
		return;
	}
	VncDialog dlg(this, vnc_server_, wxString::FromUTF8(config_copy_.vnc_password), &config_copy_);
	if (dlg.ShowModal() == wxID_OK) {
		config.vnc_enabled = config_copy_.vnc_enabled;
		config.vnc_port = config_copy_.vnc_port;
		strncpy(config.vnc_password, config_copy_.vnc_password, sizeof(config.vnc_password) - 1);
		config.vnc_password[sizeof(config.vnc_password) - 1] = '\0';
		strncpy(config.vnc_password_readonly, config_copy_.vnc_password_readonly,
		    sizeof(config.vnc_password_readonly) - 1);
		config.vnc_password_readonly[
		    sizeof(config.vnc_password_readonly) - 1] = '\0';
		config_save(&config_copy_);
	}
}
#endif

void MainFrame::OnPackages(wxCommandEvent &)
{
	/* Said here rather than letting the dialogue open and fail on its first
	   fetch: the answer is the same whatever the user then clicks. */
	if (!RPCEMU_HAVE_HTTP) {
		wxMessageBox(HttpUnavailableMessage(), "Package Manager unavailable",
		    wxOK | wxICON_INFORMATION, this);
		return;
	}

	PackageDialog dialog(this);

	dialog.ShowModal();
}

void MainFrame::OnUsb(wxCommandEvent &)
{
	UsbDialog dialog(this);

	dialog.ShowModal();
}

void MainFrame::OnSerial(wxCommandEvent &)
{
	SerialDialog dlg(this);
	dlg.ShowModal();
}

void MainFrame::OnParallel(wxCommandEvent &)
{
	ParallelDialog dlg(this);
	dlg.ShowModal();
}

void MainFrame::EditMachineConfiguration()
{
	const wxString old_config_path = ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path()));
	const wxString old_name = wxString::FromUTF8(config_copy_.name);

	/* The dialog writes straight into the live configuration, so a copy has to
	   be taken now to have anything to compare against afterwards. */
	Config old_config;
	memset(&old_config, 0, sizeof(old_config));
	config_deep_copy(&old_config, &config);

	MachineEditDialog dlg(this, old_config_path,
	                      emulator_ == nullptr || !emulator_->IsRunning(),
	                      emulator_ != nullptr && emulator_->IsRunning());
	if (dlg.ShowModal() != wxID_OK) {
		FreeConfigCopy(&old_config);
		return;
	}

	wxString config_path = old_config_path;
	if (dlg.WasRenamed()) {
		if (emulator_ && emulator_->IsRunning()) {
			wxMessageBox(
			    "The machine was renamed in the configuration file, but the data directory "
			    "was not renamed while the emulator is running.\n\n"
			    "Restart the emulator to use the new machine name.",
			    "Machine Renamed",
			    wxOK | wxICON_INFORMATION,
			    this);
		} else {
			config_path = ConfigPathsRenameMachine(old_name, dlg.GetNewName(), old_config_path);
			config_set_path(config_path.utf8_str().data());
		}
	}

	config_sync_machine_edit_to_copy(&config_copy_, &config);
	model_copy_ = machine.model;
	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}
	/* The display choices take effect now, not at the next restart: they are the
	   same settings the Settings menu applies immediately, and it would be a
	   strange dialog that made you restart for one and not the other. */
	ApplyDisplayModeToPanel();
	guest_size_seen_ = wxSize(0, 0);
	RequestGuestMode(config_copy_.screen_size_x, config_copy_.screen_size_y,
	                 true);
	ForcePanelRedraw();
	SyncSettingsMenuChecks();
	SyncCdromMenuChecks();
	UpdateMachineStatus();

	/* Ask only when the machine's hardware description changed and there is a
	   machine running to apply it to. Changing something on the Options tab -
	   the two-button mouse, the sound - needs nothing, and a dialog offering to
	   throw the machine away over it would be noise. */
	const bool needs_restart = MachineNeedsRestart(&old_config, &config);
	FreeConfigCopy(&old_config);

	if (!needs_restart || emulator_ == nullptr || !emulator_->IsRunning()) {
		return;
	}

	wxMessageDialog restart_dlg(
	    this,
	    "The machine's configuration has changed, which only takes effect "
	    "when it restarts.\n\n"
	    "Restarting will lose any unsaved data in the running machine.",
	    "Machine Configuration",
	    /* Continue is the default: a stray Return should not throw away
	       whatever is running. */
	    wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
	restart_dlg.SetYesNoLabels("Restart", "Continue Without Restarting");
	if (restart_dlg.ShowModal() == wxID_YES) {
		RestartMachine();
	}
}

void MainFrame::OnVideoTimer(wxTimerEvent &)
{
	// Display updates are delivered via PostVideoUpdate on the GUI thread.
}

void MainFrame::OnMipsTimer(wxTimerEvent &)
{
	const unsigned count = static_cast<unsigned>(instruction_count.exchange(0));

	const double mips = static_cast<double>(count) * 65536.0 / 1000000.0;
	mips_total_instructions_ += static_cast<uint64_t>(count) << 16;
	mips_seconds_++;

	const double average =
	    static_cast<double>(mips_total_instructions_) /
	    (static_cast<double>(mips_seconds_) * 1000000.0);

	perf.mips = static_cast<float>(mips);

	const int fdc_ops = fdc_activity.exchange(0);
	const int ide_ops = ide_activity.exchange(0);
	const int hostfs_ops = hostfs_activity.exchange(0);
	const int network_ops = network_activity.exchange(0);

	/* Say when the machine is idle rather than leaving a figure near zero to
	   look like a fault. Under "Reduce CPU usage" RISC OS hands its idle time
	   back through Portable_Idle, so a machine sitting at the desktop really
	   is executing almost nothing: the low number is the truth, and without
	   this it reads as the emulator having ground to a halt. */
	{
		const unsigned long idle_now = idle_ticks;
		const bool idling = (idle_now - last_idle_ticks_) > 100;

		last_idle_ticks_ = idle_now;
		SetStatusText(idling
		    ? wxString::Format("MIPS: %.1f (idle)", mips)
		    : wxString::Format("MIPS: %.1f", mips), STATUS_MIPS);
	}
	SetStatusText(wxString::Format("Avg: %.1f", average), STATUS_AVG_MIPS);

	if (fdc_ops > 0) {
		SetStatusText(wxString(L'\u25cf'), STATUS_FDC_LED);
		fdc_led_timer_.StartOnce(200);
	}
	if (ide_ops > 0) {
		SetStatusText(wxString(L'\u25cf'), STATUS_IDE_LED);
		ide_led_timer_.StartOnce(200);
	}
	if (hostfs_ops > 0) {
		SetStatusText(wxString(L'\u25cf'), STATUS_HOSTFS_LED);
		hostfs_led_timer_.StartOnce(200);
	}
	if (network_ops > 0) {
		SetStatusText(wxString(L'\u25cf'), STATUS_NET_LED);
		network_led_timer_.StartOnce(200);
	}

	UpdateMachineStatus();
}

void MainFrame::SetStatusText(const wxString &text, int number)
{
	if (GetStatusBar() == nullptr) {
		return;
	}
	wxFrame::SetStatusText(text, number);
}

void MainFrame::OnFdcLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_FDC_LED); }
void MainFrame::OnIdeLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_IDE_LED); }
void MainFrame::OnHostfsLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_HOSTFS_LED); }
void MainFrame::OnNetworkLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_NET_LED); }

void MainFrame::ReleaseHeldKeys()
{
	unsigned scan_codes[HELD_KEYS_MAX];
	const size_t n = held_keys_release_all(&held_keys_, scan_codes,
	    sizeof(scan_codes) / sizeof(scan_codes[0]));

	for (size_t i = 0; i < n; i++) {
		if (emulator_) {
			emulator_->KeyRelease(scan_codes[i]);
		}
	}
}

/*
 * Held keys are tracked per PHYSICAL key rather than per scancode, so the guest
 * hears about a key when the first physical key mapping to it goes down and
 * again when the last one comes up. Tracking scancodes alone meant that letting
 * go of one Shift released the modifier while the other was still held, which is
 * issue #70. See held_keys.h.
 */
void MainFrame::NativeKeyPress(unsigned key_id, unsigned scan_code)
{
	if (held_keys_press(&held_keys_, key_id, scan_code) && emulator_) {
		emulator_->KeyPress(scan_code);
	}
}

/*
 * Hold a made-up release back for the length of a keypress.
 *
 * The delay is the whole point. RISC OS is given a make, an interval, and a
 * break, which is what a finger on a real keyboard produces; sending the break
 * in the same event as the make leaves no interval at all and the guest acts on
 * neither. 60ms is comfortably longer than a keyboard poll and far shorter than
 * anybody can press a key twice.
 *
 * One timer for however many keys are waiting: they were all pressed within one
 * event loop turn, so they can all be let go together.
 */
void MainFrame::QueueSyntheticRelease(unsigned key_id)
{
	static const int kSyntheticReleaseMs = 60;

	synthetic_release_pending_.push_back(key_id);
	if (!synthetic_release_timer_.IsRunning()) {
		synthetic_release_timer_.StartOnce(kSyntheticReleaseMs);
	}
}

void MainFrame::OnSyntheticReleaseTimer(wxTimerEvent & /*event*/)
{
	/* Swapped out first: NativeKeyRelease() must not be walking the list if
	   anything it calls should ever add to it. */
	std::vector<unsigned> pending;

	pending.swap(synthetic_release_pending_);
	for (const unsigned key_id : pending) {
		NativeKeyRelease(key_id);
	}
}

void MainFrame::NativeKeyRelease(unsigned key_id)
{
	unsigned scan_code = 0;

	/* The scancode comes from what was recorded at press time, not from this
	   event. They can differ: a modifier can change what the same physical key
	   reports between its press and its release, and releasing a scancode that
	   was never pressed would leave the real one held. */
	if (held_keys_release(&held_keys_, key_id, &scan_code) && emulator_) {
		emulator_->KeyRelease(scan_code);
	}
}

void MainFrame::ProcessEmulatorKeyEvent(wxKeyEvent &event, bool key_down)
{
	/*
	 * menu_open_ is raised on wxEVT_MENU_OPEN and cleared on wxEVT_MENU_CLOSE,
	 * but that close event is not delivered reliably on every platform (on
	 * wxOSX it can go missing, which used to leave the flag stuck true and
	 * swallow all keyboard input - issue #29). A key event only reaches the
	 * emulator panel once no menu is holding the keyboard, so its arrival is
	 * itself proof that any open menu has since closed: clear the flag and
	 * carry on rather than dropping the key.
	 */
	menu_open_ = false;

	/* No keyboard shortcuts are intercepted here: menu/toolbar actions are
	 * mouse-driven so that every key (function keys like F12, Ctrl combos, etc.)
	 * passes straight through to RISC OS. The only exception is the mouse-capture
	 * / full-screen release key (Alt+Enter), handled below. */

	const int key_code = event.GetKeyCode();
	const unsigned scan_code = InputNativeScancodeFromKeyEvent(event);

	InputLogKeyEvent(event, scan_code, key_down);

	/*
	 * Both have to be unusable before the key is given up on. Testing the wx
	 * keycode alone used to be enough, and was wrong: a key the host layout puts
	 * somewhere wxWidgets has no name for reports WXK_NONE, so the keys that most
	 * needed the physical-position lookup were thrown away before reaching it.
	 * That is the German umlauts in issue #88.
	 */
	if (scan_code == 0 && (key_code == WXK_NONE || key_code == 0)) {
		event.Skip();
		return;
	}

	if (key_down && InputIsReleaseMouseCaptureKey(event)) {
		if (full_screen_) {
			ExitFullScreen();
			return;
		}
		if (panel_ != nullptr && !config_copy_.mousehackon && mouse_captured) {
			panel_->ReleaseMouseCapture();
			UpdateMachineStatus();
			return;
		}
		/* Nothing to escape from, so the guest gets the key. */
	}

	if (InputIsThirdMouseButtonKey(event)) {
		if (emulator_) {
			if (key_down) {
				emulator_->MousePress(4);
			} else {
				emulator_->MouseRelease(4);
			}
		}
		return;
	}

	if (key_down && event.IsAutoRepeat()) {
		return;
	}

	if (scan_code == 0) {
		event.Skip();
		return;
	}

	if (key_down) {
		const unsigned key_id = InputKeyIdentityFromKeyEvent(event);

		NativeKeyPress(key_id, scan_code);

		/*
		 * Some presses are never going to be answered by a release, so one is
		 * made up - but AFTER AN INTERVAL, not in the same breath as the press.
		 *
		 * Releasing immediately was tried and the guest sees nothing at all:
		 * there is then no interval in which the key was held, and both codes go
		 * into the PS/2 queue back to back. A real keyboard tap is a make, the
		 * time a finger takes, and a break, which is what RISC OS has always
		 * been given and demonstrably copes with. So that is what this sends.
		 *
		 * Without it the guest is left holding a key nothing will ever lift:
		 * RISC OS repeats it, and held_keys goes on suppressing further presses
		 * of it as a key already down. Caps Lock latching while the host lamp
		 * carried on toggling, and Cmd-L typing Ls for ever, were both this.
		 * See InputNeedsSyntheticRelease().
		 */
		if (InputNeedsSyntheticRelease(event, scan_code)) {
			QueueSyntheticRelease(key_id);
		}
	} else {
		NativeKeyRelease(InputKeyIdentityFromKeyEvent(event));
	}
	event.StopPropagation();
}

void MainFrame::OnKeyDown(wxKeyEvent &event) { ProcessEmulatorKeyEvent(event, true); }
void MainFrame::OnKeyUp(wxKeyEvent &event) { ProcessEmulatorKeyEvent(event, false); }

void MainFrame::OnActivate(wxActivateEvent &event)
{
	window_active_ = event.GetActive();
	if (!event.GetActive()) {
		ReleaseHeldKeys();
	} else if (panel_ != nullptr) {
		panel_->FocusPanel();
	}
	/* wxFrame's handler is what puts this window's menu bar up on macOS. */
	event.Skip();
}


/* The host's displays changed: a monitor was attached or removed, or its
   resolution was altered. Republish the geometry so the guest support module can
   follow it, using the display this window is now on.

   No debounce is needed here: this event fires on an actual display change, not
   while a window is being dragged, and rpcemu_set_host_display() ignores a
   value that has not changed, so a spurious event costs nothing. */
void MainFrame::OnDisplayChanged(wxDisplayChangedEvent &event)
{
	int index = wxDisplay::GetFromWindow(this);

	if (index == wxNOT_FOUND) {
		index = 0;
	}

	const wxDisplay display((unsigned) index);
	const wxRect geom = display.GetGeometry();
	const wxRect work = display.GetClientArea();

	if (geom.width > 0 && geom.height > 0) {
		const wxVideoMode mode = display.GetCurrentMode();

		rpcemu_set_host_display((unsigned) geom.width, (unsigned) geom.height,
		                        mode.refresh > 0 ? (unsigned) mode.refresh : 0,
		                        (unsigned) std::max(work.width, 0),
		                        (unsigned) std::max(work.height, 0));
	}

	/* The window may have moved to a display of a different size, so put it back
	   in the middle of whichever one it is on now. The guest's screen size is a
	   configured resolution and does not change with the monitor. */
	CentreWindowOnScreen();

	event.Skip();
}

/*
 * A key held when a menu is pulled down has to be released, because the menu's
 * own tracking loop swallows the key-up and the guest would be left holding the
 * key for ever. Where that release belongs is not the same on every platform.
 *
 * wxOSX raises wxEVT_MENU_OPEN from NSMenu's menuNeedsUpdate:, which AppKit
 * calls whenever it validates the menu bar and not only when a menu is opened.
 * Those passes are frequent, and one of them lands between a modifier going
 * down and the key it modifies arriving: the guest was told the modifier had
 * come back up, so the first shifted keystroke of a session came out unshifted
 * and stayed that way until the modifier was pressed again. That is issue #183,
 * and it was the same reason the menu_open_ flag used to stick.
 *
 * menuDidClose:, which raises wxEVT_MENU_CLOSE, is sent only for a menu that
 * really opened. By then the swallowed key-ups have already been missed, so
 * macOS does the release there instead and the validation passes are ignored.
 */
void MainFrame::OnMenuOpen(wxMenuEvent &)
{
#ifndef __WXOSX__
	ReleaseHeldKeys();
#endif
	menu_open_ = true;
}

void MainFrame::OnMenuClose(wxMenuEvent &)
{
#ifdef __WXOSX__
	ReleaseHeldKeys();
#endif
	menu_open_ = false;
	if (panel_ != nullptr) {
		panel_->FocusPanel();
	}
}

void MainFrame::OnLeftDown(wxMouseEvent &event)
{
	menu_open_ = false;
	if (panel_ != nullptr) {
		panel_->FocusPanel();
	}
	event.Skip();
}

bool MainFrame::IsGuiThread() const { return wxIsMainThread(); }

void MainFrame::PostVideoUpdate(VideoUpdate update)
{
	if (wxIsMainThread()) {
		if (panel_ != nullptr) {
			panel_->ApplyVideoUpdate(update);
			NoteGuestFrame();
		}
		return;
	}

	// Called from the VIDC worker thread. update.buffer points into emulator
	// memory that the worker reuses as soon as this returns (and frees on
	// shutdown), so the raw pointer cannot be handed to a deferred GUI callback.
	//
	// The previous design blocked this thread on a CallAfter handshake until
	// the GUI thread had consumed the frame. That coupling was racy on Windows:
	// if the event loop was not yet servicing CallAfter, the worker blocked
	// (holding video_mutex) on a callback that could not run, leaving a window
	// with menus but a permanently blank display. Instead, copy the frame into
	// a heap buffer owned by the posted work and return immediately - the VIDC
	// thread never waits on the GUI thread, so the display can never stall.
	if (quited) {
		return;
	}

	const size_t npixels = (size_t) update.xsize * (size_t) update.ysize;
	if (update.buffer == nullptr || npixels == 0) {
		return;
	}

	auto pixels = std::make_shared<std::vector<uint32_t>>(
	    update.buffer, update.buffer + npixels);
	VideoUpdate copy = update;
	copy.buffer = pixels->data();

	CallAfter([this, copy, pixels]() {
		(void) pixels; // keeps copy.buffer alive until the frame is applied
		if (panel_ != nullptr) {
			panel_->ApplyVideoUpdate(copy);
			NoteGuestFrame();
		}
	});
}

void MainFrame::PostMoveHostMouse(const MouseMoveUpdate &update)
{
	CallAfter([this, update]() {
		if (panel_ != nullptr) {
			panel_->HandleMoveHostMouse(update);
		}
	});
}

void MainFrame::PostError(const std::string &message)
{
	CallAfter([this, message]() { ShowError(message); });
}

void MainFrame::PostFatal(const std::string &message)
{
	// Runs on the thread that raised the fatal error (often the emulator
	// thread, which then spins and can no longer service commands). Record it
	// immediately so a concurrent window close doesn't try to save state.
	fatal_occurred_ = true;
	CallAfter([this, message]() { ShowFatal(message); });
}

void MainFrame::ShowError(const std::string &message)
{
	wxMessageBox(wxString::FromUTF8(message), "RPCEmu Extended Error", wxOK | wxICON_WARNING, this);
}

void MainFrame::ShowFatal(const std::string &message)
{
	fatal_occurred_ = true;
	wxMessageBox(wxString::FromUTF8(message), "RPCEmu Extended Fatal Error", wxOK | wxICON_ERROR, this);

	// The machine has failed unrecoverably, so terminate the process rather
	// than attempting a clean shutdown. A normal shutdown joins the emulator
	// thread, but the thread that raised the error is spinning forever inside
	// fatal() and would deadlock that join. And a startup failure (e.g. a
	// missing ROM) runs on the GUI thread before the event loop has started,
	// so falling back through fatal()'s wait loop would hang the whole app
	// after the user clicks OK. Flush logs, then exit hard (no destructors,
	// which would themselves try to join the spinning thread).
	fflush(nullptr);
	std::_Exit(EXIT_FAILURE);
}

void MainFrame::PostDebuggerStateChanged()
{
	CallAfter([this]() {
		UpdateDebuggerActionStates();

		/*
		 * ★ And tell the inspector, which used to hear nothing from here.
		 *
		 * Its own refresh is a half-second timer. That is fine for watching a
		 * running machine and hopeless for stepping: the snapshot it takes
		 * straight after a step still says "running", because the step has not
		 * finished, so Step and Run grey out and nothing re-reads until the
		 * next tick. Measured by the reporter of discussion #223 as a
		 * consistent half second per step with Step unclickable in between,
		 * which is exactly this and not the cost of drawing the window.
		 *
		 * The emulator thread raises this when the debugger actually starts or
		 * stops, so the window now follows the machine rather than the clock.
		 */
		if (machine_inspector_window_ != nullptr) {
			machine_inspector_window_->RefreshFromStateChange();
		}
	});
}

/* Handed straight to whoever is waiting for it (see guest_command.cpp), which is
   why this does nothing with the window. */
void MainFrame::PostGuestCommandResult(unsigned token, unsigned rc,
                                       const std::string &output, bool ok)
{
	CallAfter([token, rc, output, ok]() {
		GuestCommandDeliver(token, rc, output, ok);
	});
}

void MainFrame::PostMachineSwitched(const std::string &machine_name)
{
	CallAfter([this, machine_name]() {
		SetTitle(WindowTitleFor(wxString::FromUTF8(machine_name.c_str())));
		config_deep_copy(&config_copy_, &config);
		model_copy_ = machine.model;
		if (panel_ != nullptr) {
			panel_->UpdateMouseCursor();
		}
		ApplyDisplayModeToPanel();
		SyncSettingsMenuChecks();
		SyncCdromMenuChecks();
		UpdateMachineStatus();
		UpdateRecentMachinesMenu();
	});
}

void MainFrame::PostQuit()
{
	/*
	 * Called from the emulator thread; hop to the GUI thread and deal with the
	 * guest having powered itself off - the desktop's Shutdown, via OS_Reset.
	 *
	 * Through WhenNothingIsModal() rather than acting straight away, for the
	 * reason set out there: every dialogue this window opens is a stack object
	 * parented to it, destroying the window from inside a dialogue's own event
	 * loop aborts the process, and this request can arrive while anything at
	 * all is on screen.
	 */
	CallAfter([this]() { WhenNothingIsModal([this]() { GuestPoweredOff(); }); });
}

/*
 * File > Exit, which on macOS is RPCEmu > Quit (Cmd+Q).
 *
 * Close(false), not Close(true). The forced form cannot be declined, so quitting
 * this way skipped the "are you sure" entirely and took the machine down without
 * asking - the close button asked and the Quit menu did not, for the same action.
 */
void MainFrame::OnExit(wxCommandEvent &) { Close(false); }

/*
 * Closing the window: confirm, then offer the machine list.
 *
 * Two things were missing. Closing a running machine took it down with no
 * question asked, which for an emulator is the same shape of mistake as closing
 * an unsaved document - RISC OS is running, its discs are mounted, and there may
 * be work in it. And quitting was the only thing closing could do, so somebody
 * who wanted a different machine had to quit and start the application again,
 * even though switching machines in place is something RPCEmu already does.
 *
 * Both dialogues are deliberately plain two-button ones, and both are run OUTSIDE
 * the close event. The first attempt at this was a single three-button alert with
 * custom labels, raised from inside the close handler, and on macOS it never ran
 * its modal loop at all: ShowModal() returned wxID_YES straight away, so the
 * machine was shut down as though the user had picked "Quit" from a dialogue they
 * were never shown. Vetoing first and asking afterwards keeps the questions out
 * of a handler that is in the middle of tearing the window down.
 */
bool MainFrame::ConfirmCloseOrSwitch()
{
	if (close_confirmed_) {
		return true;	/* Already been through this; this is the real close */
	}

	/*
	 * One question at a time.
	 *
	 * A single click on the close button can produce more than one close event -
	 * the window's own, and the application's check for whether it should quit
	 * now that its last window is going - and each of those was queueing another
	 * question. The result was the confirmation appearing a second time on top of
	 * the machine selector, with two answers wanted for one click.
	 */
	if (close_question_pending_) {
		return false;
	}
	close_question_pending_ = true;

	/* Deferred, so the dialogues run with the close event finished and the frame
	   in a settled state rather than half way through being destroyed. */
	CallAfter([this] { AskAboutClosing(); });
	return false;
}

/*
 * Are you sure you wish to shut down this machine?
 *
 * Yes: the window goes, and the machine selector comes back so another machine
 * can be started. No: nothing happens.
 *
 * Runs from a CallAfter(), with the close event already declined, and NOT from
 * inside the close handler. That is not tidiness, it is the bug this had: the
 * first version raised a three-button alert with custom labels from inside the
 * close handler, and on macOS it never ran its modal loop - ShowModal() returned
 * wxID_YES straight away, so the machine was shut down as though the user had
 * picked "Quit" from a dialogue they were never shown.
 */
void MainFrame::AskAboutClosing()
{
	/*
	 * ★ The close may already have been settled while this sat in the queue.
	 *
	 * A close that cannot be vetoed - which is what macOS sends when the
	 * application is asked to quit - is obeyed by OnClose() whatever the answer
	 * here would have been, and it runs the whole teardown, Destroy() included.
	 * Clearing close_question_pending_ there does NOT unqueue this call: it is
	 * already a pending event, and only the flag was dropped.
	 *
	 * So this used to run afterwards, on a frame waiting to be deleted: it hid
	 * the window and opened the machine selector, and ShowModal()'s nested event
	 * loop runs idle processing, which is exactly where wxWidgets deletes
	 * pending windows. The frame was freed with this function and
	 * SwitchToChosenMachine() still on the stack underneath it, and the next
	 * free aborted inside libmalloc - "RPCEmu quit unexpectedly" on every quit,
	 * issue #184.
	 *
	 * Checked first, before anything is hidden or shown, because by this point
	 * there is nothing left to decide.
	 */
	if (shutting_down_ || close_confirmed_) {
		rpclog("MainFrame: close already settled, so nothing is asked\n");
		close_question_pending_ = false;
		return;
	}

	const bool running = emulator_ != nullptr && emulator_->IsRunning();

	/* Cleared on every path out of here, so a later close can ask again. */
	struct ClearPending {
		bool *flag;
		~ClearPending() { *flag = false; }
	} clear_pending{ &close_question_pending_ };

	if (running) {
		const int answer = wxMessageBox(
		    "Are you sure you wish to shut down this machine?",
		    wxString::Format("Shut Down %s", MachineDisplayName()),
		    wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);

		/* wxMessageBox answers with the STYLE flags - wxYES, wxNO - and not with
		   wxID_YES and friends, which is what wxMessageDialog::ShowModal()
		   returns. Mixing the two families up is an easy way to write a
		   confirmation that silently always takes the same branch. */
		rpclog("MainFrame: shutdown question answered %d (yes=%d)\n",
		       answer, wxYES);

		if (answer != wxYES) {
			return;
		}

		if (shutting_down_) {
			rpclog("MainFrame: the close was forced while the question was up\n");
			return;
		}
	}

	if (StopMachineAndOfferList()) {
		return;
	}

	/* The list was closed without choosing, so there is no machine to go to and
	   the close stands. Flagged so it is not questioned a second time, and the
	   guard is dropped here rather than by the destructor above, because Close()
	   runs the close handler before this function returns. */
	close_confirmed_ = true;
	close_question_pending_ = false;
	Close(true);
}

bool MainFrame::StopMachineAndOfferList()
{
	/*
	 * ★ Execution stops BEFORE anything is shown.
	 *
	 * The window used to be hidden with the emulator still running, so a
	 * machine the user had just agreed to shut down went on executing behind
	 * the machine list: a host core at full tilt, and - which is how it was
	 * noticed - a game's music still playing out of a machine that was
	 * supposedly closed. Issue #222.
	 *
	 * Suspended rather than stopped, because if another machine is chosen it is
	 * this same emulator thread that switches to it; Stop() would leave nothing
	 * to do the switching. See EmulatorHost::SetExecutionSuspended().
	 */
	if (emulator_ != nullptr) {
		emulator_->SetExecutionSuspended(true);
	}

	/*
	 * The window goes next, then the list.
	 *
	 * Hidden rather than destroyed, because it has to be able to come back.
	 * Destroying it would take the emulator thread with it, and starting a
	 * machine from nothing is the application's own startup path rather than
	 * something a frame can do to itself - so a machine chosen from the list would
	 * need the whole process restarted. Hidden, the same window reappears with the
	 * new machine in it, which is what the user sees either way.
	 */
	Hide();

	/*
	 * The same hazard once more, one level in: a forced close arriving while the
	 * selector is open tears the frame down inside its modal loop. Nothing below
	 * may touch this frame if that has happened, so its liveness is checked
	 * rather than assumed. wxWeakRef goes empty when the window is destroyed.
	 */
	wxWeakRef<MainFrame> self(this);
	const bool switching = SwitchToChosenMachine();

	if (!self) {
		rpclog("MainFrame: the frame went while the machine list was open\n");
		return true;	/* nothing left here to close */
	}

	if (switching) {
		/* The switch is a command on the emulator thread, and commands are
		   drained whether or not execution is suspended, so this can be lifted
		   now: the new machine runs as soon as the switch has been done. */
		if (emulator_ != nullptr) {
			emulator_->SetExecutionSuspended(false);
		}
		Show();
		Raise();
		return true;
	}

	return false;
}

void MainFrame::GuestPoweredOff()
{
	rpclog("MainFrame: the guest powered the machine off\n");

	/* Already on the way down for some other reason, so there is nothing to
	   offer a machine list to. */
	if (shutting_down_ || close_confirmed_) {
		rpclog("MainFrame: already closing, so the power-off is ignored\n");
		return;
	}

	/*
	 * No confirmation. RISC OS has just been through its own shutdown, which
	 * asked about unsaved work and dismounted the discs, and asking "are you
	 * sure you wish to shut down this machine" after that is asking twice about
	 * one decision.
	 */
	if (StopMachineAndOfferList()) {
		return;
	}

	close_confirmed_ = true;
	close_question_pending_ = false;
	Close(true);
}

/*
 * Show the machine list and start whatever is chosen in place of this machine.
 *
 * The same dialogue the application opens with, so "another machine" means the
 * same thing in both places. Resume and Load State come with it, since they are
 * properties of the machine being chosen rather than of how it was reached -
 * though a state file can only be applied by starting into it, which switching
 * does not do, so those are declined with an explanation rather than silently
 * ignored.
 *
 * @return true if a machine was asked for, false if the list was cancelled
 */
bool MainFrame::SwitchToChosenMachine()
{
	ConfigSelectorDialog selector(this);

	if (selector.ShowModal() != wxID_OK) {
		return false;
	}

	const wxString config_path =
	    ConfigPathsAbsoluteConfigPath(selector.GetSelectedConfigPath());

	if (!wxFileExists(config_path)) {
		/* Shown before the window comes back, so it is not hidden behind it. */
		wxMessageBox("That machine's configuration could not be found.",
		             "Machine Not Found", wxOK | wxICON_WARNING, this);
		return true;	/* Asked for something: do not fall through to quitting */
	}

	if (selector.ShouldResume() || !selector.GetStateFileToLoad().IsEmpty()) {
		wxMessageBox(
		    "Switching machines starts the machine from cold, so a saved state "
		    "cannot be resumed this way.\n\n"
		    "Quit and start RPCEmu again to resume it.",
		    "Resume Not Available", wxOK | wxICON_INFORMATION, this);
		return true;
	}

	const wxString machine_name = wxFileName(config_path).GetName();

	AddRecentMachine(machine_name.utf8_str().data());
	UpdateRecentMachinesMenu();
	SetTitle(WindowTitleFor(machine_name));

	/* Stops the machine that is running and starts this one in its place, which
	   is the same path the recent-machines menu uses. */
	if (emulator_) {
		/* Boots from cold, as above. */
		machine_started_ms_ = wxGetLocalTimeMillis();
		emulator_->SwitchMachine(config_path.utf8_str().data());
	}
	return true;
}

/** This machine's name for a dialogue title, or the application's. */
wxString MainFrame::MachineDisplayName() const
{
	const wxString name = CurrentMachineBaseName();

	return name.IsEmpty() ? wxString("RPCEmu") : name;
}

void MainFrame::OnClose(wxCloseEvent &event)
{
	if (shutting_down_) {
		event.Skip();
		return;
	}

	/* One line per close. Kept because it is what found two separate faults in
	   this path: a dialogue that answered itself, and a bundle that did not
	   contain the code being tested. */
	rpclog("MainFrame: close requested (vetoable=%d signal=%d fatal=%d confirmed=%d)\n",
	       event.CanVeto() ? 1 : 0, closing_for_signal_ ? 1 : 0,
	       EmulatorFatalOccurred() ? 1 : 0, close_confirmed_ ? 1 : 0);

	/*
	 * A forced close - a signal, or the session ending - is not a request that
	 * may be declined, so it is not put to the user.
	 *
	 * CanVeto() is deliberately NOT part of the test any more. A close that
	 * cannot be vetoed still has to be obeyed, but it must not be the reason the
	 * question is skipped: one click on the close button can raise more than one
	 * close event, and if a non-vetoable one arrives first then testing CanVeto()
	 * first means the machine is taken down without anything being asked. Ask
	 * first, and then veto only if we are allowed to.
	 */
	if (!closing_for_signal_ && !EmulatorFatalOccurred() &&
	    !ConfirmCloseOrSwitch())
	{
		if (event.CanVeto()) {
			event.Veto();
			return;
		}
		/* Cannot decline it, so the machine is going down regardless. Drop the
		   question that was queued, since there is nothing left for it to
		   decide. */
		rpclog("MainFrame: close cannot be vetoed - going down without asking\n");
		close_question_pending_ = false;
		close_confirmed_ = true;
	}

	// If a fatal error has been raised, the emulator thread is spinning forever
	// inside fatal() and can never be joined or asked to save state. ShowFatal()
	// normally terminates the process directly, but guard here too: run no
	// teardown (destructors would try to join the spinning thread and hang) and
	// exit immediately.
	if (EmulatorFatalOccurred()) {
		fflush(nullptr);
		std::_Exit(EXIT_FAILURE);
	}

	// Store the machine state on exit so the next launch can Resume it (the
	// machine selector offers it). This must run while the emulator thread is
	// still alive, before ShutdownEmulator() stops it.
	//
	// Only done when the user opted in - either File->Suspend (an explicit
	// "save and exit", flagged here) or the "Suspend on exit" setting. A plain
	// Quit shuts down cleanly and leaves no snapshot.
	//
	// Skip it regardless if the emulator never started running (e.g. a fatal
	// error like a missing ROM during startup) or if a fatal error has since
	// occurred: the emulator thread cannot service a SaveState command in those
	// cases, so attempting it would block forever, and its state is not worth
	// saving.
	if (emulator_ && emulator_->IsRunning() && !fatal_occurred_ &&
	    !closing_for_signal_ &&
	    (config.suspend_on_exit || suspend_on_exit_requested_)) {
		const wxString snapshot = ConfigPathsSnapshotForConfig(
		    ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path())));
		if (!emulator_->SaveState(snapshot.utf8_str().data())) {
			rpclog("MainFrame: failed to save machine state on exit\n");
		}
	}

	ShutdownEmulator();
	shutting_down_ = true;
	Destroy();
}

int EndOpenModalDialogs(bool cancel)
{
	int open = 0;

	/* The iterator is tested directly rather than against nullptr. wxWidgets
	   defines wxWindowList::compatibility_iterator two different ways: with
	   wxUSE_STL it is a class offering operator bool() and no comparison with
	   nullptr_t, and without it a wrapper that converts to a node pointer. Only
	   the second form compiles against nullptr, so a build with the STL
	   containers enabled - which is what Homebrew's wx gives on macOS - failed
	   with "invalid operands to binary expression". Truth-testing works in
	   both. Reported by Septercius, issue #30. */
	for (auto node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext()) {
		auto *dialog = dynamic_cast<wxDialog *>(node->GetData());

		if (dialog == nullptr || !dialog->IsModal()) {
			continue;
		}
		open++;
		if (cancel) {
			dialog->EndModal(wxID_CANCEL);
		}
	}
	return open;
}

void MainFrame::WhenNothingIsModal(std::function<void()> action)
{
	/* Asked once. A dialogue does not leave its loop when it is told to, it
	   leaves when control next returns to it, so telling it again on every
	   turn of this would be noise. */
	if (!close_asked_modals_) {
		close_asked_modals_ = EndOpenModalDialogs(true) > 0;
		if (close_asked_modals_) {
			rpclog("MainFrame: waiting for an open dialogue to close first\n");
		}
	}

	if (EndOpenModalDialogs(false) > 0) {
		/* Still up, so this window must not be destroyed yet: doing it from
		   inside that dialogue's nested loop is the abort this exists to
		   avoid. Queued, so the loop gets its turn. */
		CallAfter([this, action]() { WhenNothingIsModal(action); });
		return;
	}

	/* Dropped before the action runs, not after: the guest powering the machine
	   off no longer ends the session, so a later close has to be able to ask
	   the dialogues of that session to end as well. Left latched, that close
	   would wait for ever for dialogues nothing had asked to go. */
	close_asked_modals_ = false;
	action();
}

void MainFrame::CloseWhenNothingIsModal()
{
	/* Close() rather than Destroy(), so the machine is taken down by the same
	   OnClose() the window's own close button uses - the snapshot being the
	   only part a signal skips. Forced, because neither a signal nor the
	   Manager's Stop is a request the window may decline. */
	WhenNothingIsModal([this]() { Close(true); });
}

void MainFrame::CloseForSignal()
{
	if (shutting_down_) {
		return;
	}

	closing_for_signal_ = true;
	CloseWhenNothingIsModal();
}

void MainFrame::ResetForSignal()
{
	/* Nothing to reset once the window is on its way out, whatever the
	   emulator still looks like. */
	EmulatorResetForSignal(shutting_down_ ? nullptr : emulator_.get());
}

void MainFrame::ShutdownEmulator()
{
	mips_timer_.Stop();
	video_timer_.Stop();
	if (emulator_) {
		emulator_->RequestExit();
		emulator_->Stop();
		emulator_->Join();
		emulator_.reset();
	}
	machine_lock_release();
}
