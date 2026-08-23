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

#include "about_dialog.h"
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
	a.screen_size = b.screen_size = 0;
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
	EVT_TIMER(ID_TIMER_MATCH_WINDOW, MainFrame::OnMatchWindowTimer)
	EVT_DISPLAY_CHANGED(MainFrame::OnDisplayChanged)
	EVT_SIZE(MainFrame::OnFrameSize)
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
	, match_window_timer_(this, ID_TIMER_MATCH_WINDOW)
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
	ApplyDisplayModeToPanel();
	/* Nothing has been seen from the guest yet, so there is no size to record.
	   The first frame gives one, and OnMatchWindowTimer() then sizes the window
	   to it. */
	match_window_guest_size_ = wxSize(0, 0);
	/*
	 * A window that is not locked to the desktop needs an opening size, or it
	 * keeps whatever the layout left it at.
	 *
	 * Not for match-the-window, though: there the opening size is the guest's
	 * desktop, which does not exist yet. OnMatchWindowTimer() sizes the window
	 * once the guest has settled on a mode - it boots through two or three.
	 */
	if (WindowSizeIsFree() &&
	    config_copy_.screen_size != ScreenSize_MatchWindow)
	{
		CallAfter([this] { ApplyFreeWindowSize(); });
	}
	/* A fixed screen size has to be published, or the guest never hears about
	   it: the support module polls, and until something bumps the generation
	   there is nothing for it to act on. */
	if (config_copy_.screen_size == ScreenSize_Fixed) {
		rpcemu_request_guest_size(config_copy_.screen_size_x,
		                          config_copy_.screen_size_y);
	}
	SyncSettingsMenuChecks();
	SyncCdromMenuChecks();
	UpdateMachineStatus();

	AddRecentMachine(config_copy_.name);

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
	if (GetMenuBar() != nullptr) {
		GetMenuBar()->Show(false);
	}
	if (tool_bar_ != nullptr) {
		tool_bar_->Show(false);
	}
	/* Removed rather than hidden. Hiding it leaves the space it occupied
	   reserved - measurably so: the frame's client height does not change -
	   and on macOS that shows as an empty strip along the bottom of the
	   screen. BuildStatusBar() puts it back on the way out. */
	if (wxStatusBar *bar = GetStatusBar()) {
		SetStatusBar(nullptr);
		bar->Destroy();
	}
	ShowFullScreen(true, wxFULLSCREEN_ALL);
	full_screen_ = true;

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
	if (fullscreen_menu_item_ != nullptr) {
		fullscreen_menu_item_->Check(true);
	}
}

void MainFrame::ExitFullScreen()
{
	if (!full_screen_) {
		return;
	}

	if (panel_ != nullptr) {
		panel_->SetFullScreen(false);
	}
	ShowFullScreen(false);
	if (GetMenuBar() != nullptr) {
		GetMenuBar()->Show(true);
	}
	if (tool_bar_ != nullptr) {
		tool_bar_->Show(true);
	}
	if (GetStatusBar() == nullptr) {
		BuildStatusBar();
	}
	/* No Fit(): ShowFullScreen(false) has already restored the size, and
	   fitting would shrink-wrap the frame to a panel that has no minimum. */
	Layout();
	full_screen_ = false;

	/* Force a full repaint once the windowed layout has settled (see the note
	   in EnterFullScreen). */
	if (panel_ != nullptr) {
		panel_->CallAfter([this] {
			if (panel_ != nullptr) {
				panel_->ForceRedraw();
			}
		});
	}

	if (panel_ != nullptr) {
		panel_->UpdateMouseCursor();
	}
	if (fullscreen_menu_item_ != nullptr) {
		fullscreen_menu_item_->Check(false);
	}
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
void MainFrame::OnDisplayScaling(wxCommandEvent &event)
{
	int scaling = DisplayScaling_ActualSize;

	switch (event.GetId()) {
	case ID_MENU_SCALING_MULTIPLES:
		scaling = DisplayScaling_WholeMultiples;
		break;
	case ID_MENU_SCALING_FIT:
		scaling = DisplayScaling_ScaleToFit;
		break;
	default:
		break;
	}

	if (scaling == config_copy_.display_scaling) {
		return;
	}

	const bool was_free = WindowSizeIsFree();

	config_copy_.display_scaling = scaling;
	if (scaling_menu_items_[scaling] != nullptr) {
		scaling_menu_items_[scaling]->Check(true);
	}
	if (emulator_) {
		emulator_->SetDisplayScaling(scaling);
	}
	ApplyDisplayModeToPanel();

	/* A window that has just stopped being locked to the desktop keeps whatever
	   size the lock left it at, which for a 640x480 mode is a postage stamp.
	   Give it something usable to start from. */
	if (!was_free && WindowSizeIsFree()) {
		ApplyFreeWindowSize();
	}
	ForcePanelRedraw();
}

/*
 * RISC OS Screen Size: where the size of the guest's desktop comes from.
 */
void MainFrame::OnScreenSize(wxCommandEvent &event)
{
	const int id = event.GetId();
	int mode = ScreenSize_Automatic;
	unsigned fixed_x = config_copy_.screen_size_x;
	unsigned fixed_y = config_copy_.screen_size_y;

	if (id == ID_MENU_SCREEN_MATCH_WINDOW) {
		mode = ScreenSize_MatchWindow;
	} else if (id >= ID_MENU_SCREEN_FIXED_FIRST &&
	           id <= ID_MENU_SCREEN_FIXED_LAST)
	{
		const size_t index = (size_t) (id - ID_MENU_SCREEN_FIXED_FIRST);

		if (index >= fixed_mode_items_.size()) {
			return;		/* Stale id from a menu that has since been rebuilt */
		}
		mode = ScreenSize_Fixed;
		fixed_x = fixed_mode_items_[index].first;
		fixed_y = fixed_mode_items_[index].second;
	}

	if (mode == config_copy_.screen_size &&
	    fixed_x == config_copy_.screen_size_x &&
	    fixed_y == config_copy_.screen_size_y)
	{
		return;
	}

	const bool was_free = WindowSizeIsFree();

	config_copy_.screen_size = mode;
	config_copy_.screen_size_x = fixed_x;
	config_copy_.screen_size_y = fixed_y;
	if (emulator_) {
		emulator_->SetScreenSize(mode, fixed_x, fixed_y);
	}
	ApplyDisplayModeToPanel();

	if (!was_free && WindowSizeIsFree()) {
		ApplyFreeWindowSize();
	}

	/* Match-the-window has to say what the window currently is, or nothing
	   happens until the user next drags an edge. Chosen from the menu the window
	   is already up and sized, so unlike at startup there is nothing to wait
	   for. */
	if (mode == ScreenSize_MatchWindow) {
		/*
		 * Seeded with the size the desktop is NOW, not zero.
		 *
		 * Zero reads as "the guest has changed mode", and the timer answers that
		 * by sizing the window to the desktop. So the first drag after switching
		 * this on had the window snapped straight back to where it started and
		 * nothing was ever published: dragging appeared to do nothing at all.
		 * Recording the current size means a drag is correctly read as the
		 * window having moved while the guest stood still.
		 */
		if (panel_ != nullptr) {
			match_window_guest_size_ = panel_->GuestScreenSize();
		}
		PublishWindowSizeToGuest();
	}

	/* Deliberately NOT SyncSettingsMenuChecks() here: that rebuilds the fixed-
	   size entries, and one of those is the item currently dispatching this
	   event. The click has already moved the radio tick, and the list itself
	   only changes with the machine's display memory, which a click on it
	   cannot alter. */
	ForcePanelRedraw();
}

bool MainFrame::WindowSizeIsFree() const
{
	return config_copy_.display_scaling != DisplayScaling_ActualSize ||
	       config_copy_.screen_size == ScreenSize_MatchWindow;
}

void MainFrame::ApplyDisplayModeToPanel()
{
	if (panel_ == nullptr) {
		return;
	}
	panel_->SetDisplayMode(
	    DisplayOptions::ClampDisplayScaling(config_copy_.display_scaling),
	    WindowSizeIsFree());
	Layout();
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

		item->SetHelp(DisplayOptions::ScreenSizeFixedHelp());
		fixed_mode_items_.push_back(modes[i]);
	}

	/* Tick whichever entry the configuration names, and fall back to automatic
	   when it names one this machine can no longer show - VRAM reduced, or the
	   graphics card taken out. */
	const int mode = DisplayOptions::ClampScreenSize(config_copy_.screen_size);
	if (mode == ScreenSize_Fixed) {
		for (size_t i = 0; i < fixed_mode_items_.size(); i++) {
			if (fixed_mode_items_[i].first == config_copy_.screen_size_x &&
			    fixed_mode_items_[i].second == config_copy_.screen_size_y)
			{
				screen_size_menu_->Check(
				    (int) (ID_MENU_SCREEN_FIXED_FIRST + (int) i), true);
				return;
			}
		}
		config_copy_.screen_size = ScreenSize_Automatic;
	}

	if (screen_size_menu_items_[config_copy_.screen_size] != nullptr) {
		screen_size_menu_items_[config_copy_.screen_size]->Check(true);
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
 * The window was resized. Only of interest when the guest is following it.
 *
 * Every resize comes through here, including the ones the emulator itself causes
 * by locking the window to the guest's desktop; PublishWindowSizeToGuest()
 * returns at once unless the screen size is actually following the window, so
 * those cost nothing.
 */
void MainFrame::OnFrameSize(wxSizeEvent &event)
{
	PublishWindowSizeToGuest();
	event.Skip();
}

/*
 * A frame arrived from the guest.
 *
 * Only the desktop's size is of interest here, and only when it changes. RISC OS
 * changes mode several times while it boots, so a change is not by itself a
 * moment to act on; it restarts the settle timer, and OnMatchWindowTimer() acts
 * on whatever the size has turned out to be.
 */
/*
 * How long to wait for a resize drag, or a run of guest mode changes, to stop.
 */
static const int kMatchWindowSettleMs = 400;

void MainFrame::NoteGuestFrame()
{
	if (panel_ == nullptr) {
		return;
	}

	const wxSize guest = panel_->GuestScreenSize();

	if (guest.x <= 0 || guest.y <= 0) {
		return;
	}

	if (config_copy_.screen_size != ScreenSize_MatchWindow ||
	    guest == match_window_guest_size_)
	{
		return;
	}
	match_window_timer_.StartOnce(kMatchWindowSettleMs);
}

/*
 * The window was resized, and the guest is following it.
 *
 * Debounced, because a drag fires this continuously and each mode change reflows
 * every window on the RISC OS desktop. Publishing every intermediate width would
 * have the guest working its way through modes it is about to leave, which looks
 * and feels like a fault. Only the size things settle at is acted on.
 */
void MainFrame::PublishWindowSizeToGuest()
{
	if (config_copy_.screen_size != ScreenSize_MatchWindow) {
		return;
	}
	match_window_timer_.StartOnce(kMatchWindowSettleMs);
}

/*
 * Things have gone quiet. Work out which way the size is meant to travel.
 */
void MainFrame::OnMatchWindowTimer(wxTimerEvent &event)
{
	(void)event;

	if (config_copy_.screen_size != ScreenSize_MatchWindow || panel_ == nullptr) {
		return;
	}

	const wxSize guest = panel_->GuestScreenSize();

	if (guest.x <= 0 || guest.y <= 0) {
		return;		/* No desktop yet: nothing to reconcile with */
	}

	if (guest != match_window_guest_size_) {
		/*
		 * The guest changed mode for its own reasons - it boots through two or
		 * three - so follow it with the window rather than answering back with a
		 * size of our own. That would be a conversation neither side started.
		 *
		 * Only at actual size, where the window and the desktop are meant to be
		 * the same size and any difference is a black border or a clipped
		 * desktop. The other two drawing rules are built to fill whatever window
		 * they are given, so snapping one to the guest would take a size away
		 * from the user for no gain.
		 */
		match_window_guest_size_ = guest;
		if (config_copy_.display_scaling == DisplayScaling_ActualSize) {
			SnapWindowToGuest();
		}
		/* Round again: the resize above, or a mode change still in flight, may
		   leave the two out of step. */
		match_window_timer_.StartOnce(kMatchWindowSettleMs);
		return;
	}

	/* The guest is where it was, so it is the window that moved: the user
	   dragged an edge. Ask the guest for a mode that fits it. */
	const wxSize client = panel_->GetClientSize();

	if (client.x > 0 && client.y > 0) {
		rpcemu_request_guest_size((unsigned) client.x, (unsigned) client.y);
	}
}

void MainFrame::ApplyFreeWindowSize()
{
	if (!WindowSizeIsFree()) {
		return;
	}

	/* An opening size, not a limit: four fifths of the work area is a window
	   somebody can see the edges of and drag from, which is what a freshly
	   unlocked window wants. Nothing stops it being dragged larger afterwards. */
	const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
	const wxSize cur = GetSize();
	const int w = std::clamp(cur.x, 800, std::max(area.width * 4 / 5, 800));
	const int h = std::clamp(cur.y, 600, std::max(area.height * 4 / 5, 600));

	if (w != cur.x || h != cur.y) {
		SetSize(wxSize(w, h));
	}
	/* Force the sizer to re-lay-out so the (now unconstrained) panel expands to
	   fill the client area, even if the frame size did not actually change. */
	Layout();
	if (panel_ != nullptr) {
		panel_->ForceRedraw();
	}
}

/*
 * Fit the window to the desktop the guest has just moved to.
 *
 * Only for match-the-window at actual size, where the two are meant to be the
 * same size and any difference is a black border or a clipped desktop. The user
 * drags an edge, the guest picks the largest standard mode that fits, and the
 * window then closes the gap between the two - which is what makes the option
 * feel like the window snapping to real screen modes rather than leaving a
 * ragged margin.
 *
 * Bounded by the work area and NOT by the four-fifths opening size above: that
 * one is about giving a new window somewhere sensible to start, and applying it
 * here would cap the RISC OS desktop at four fifths of the display however large
 * the user made the window.
 */
void MainFrame::SnapWindowToGuest()
{
	if (panel_ == nullptr) {
		return;
	}

	const wxSize guest = panel_->GuestScreenSize();

	if (guest.x <= 0 || guest.y <= 0) {
		return;
	}

	/* The frame is larger than the panel by the menu bar, tool bar and status
	   bar, so ask for the panel's size plus that difference rather than trying
	   to enumerate the chrome. */
	const wxSize cur = GetSize();
	const wxSize panel_size = panel_->GetSize();
	const int chrome_w = std::max(cur.x - panel_size.x, 0);
	const int chrome_h = std::max(cur.y - panel_size.y, 0);
	const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
	const int w = std::min(guest.x + chrome_w, area.width);
	const int h = std::min(guest.y + chrome_h, area.height);

	if (w != cur.x || h != cur.y) {
		SetSize(wxSize(w, h));
	}
	Layout();
	panel_->ForceRedraw();
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

	/* A monitor change moves the window between displays and can change how much
	   room it has, so a guest that is following the window may need a new mode. */
	PublishWindowSizeToGuest();

	event.Skip();
}

void MainFrame::OnMenuOpen(wxMenuEvent &)
{
	ReleaseHeldKeys();
	menu_open_ = true;
}

void MainFrame::OnMenuClose(wxMenuEvent &)
{
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
	CallAfter([this]() { UpdateDebuggerActionStates(); });
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
	/* Called from the emulator thread; hop to the GUI thread and close the
	   window, which runs the normal shutdown (stop + join the emu threads). */
	CallAfter([this]() { Close(true); });
}

void MainFrame::OnExit(wxCommandEvent &) { Close(true); }

void MainFrame::OnClose(wxCloseEvent &event)
{
	if (shutting_down_) {
		event.Skip();
		return;
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

void MainFrame::CloseForSignal()
{
	if (shutting_down_) {
		return;
	}

	closing_for_signal_ = true;

	/* Close() rather than Destroy(), so the machine is taken down by the same
	   OnClose() the window's own close button uses - the snapshot being the
	   only part it skips. Forced, because a signal is not a request the window
	   may decline. */
	Close(true);
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
