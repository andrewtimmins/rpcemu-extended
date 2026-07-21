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

#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/icon.h>
#include <wx/image.h>

#include "about_dialog.h"
#include "config_paths.h"
#include "gui_preferences.h"
#include "input_helpers.h"
#include "machine_edit_dialog.h"
#include "machine_inspector_window.h"
#include "nat_list_dialog.h"
#include "parallel_dialog.h"
#include "serial_dialog.h"
#include "toolbar_icons.h"

#ifdef RPCEMU_VNC
#include "vnc_dialog.h"
#endif

#ifdef RPCEMU_VNC
#include "vnc_server.h"
#endif

extern "C" {
#include "rpcemu.h"
}

namespace {

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
	           "This will reset RPCEmu!\n\nOkay to continue?",
	           "RPCEmu",
	           wxOK | wxCANCEL | wxICON_WARNING,
	           parent) == wxOK;
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
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
	: wxFrame(nullptr, wxID_ANY,
	          wxString::Format("RPCEmu v%s", wxString(VERSION)),
	          wxDefaultPosition, wxSize(800, 600)),
	  mips_timer_(this, ID_TIMER_MIPS),
	  video_timer_(this, ID_TIMER_VIDEO),
	  fdc_led_timer_(this, ID_TIMER_FDC_LED),
	  ide_led_timer_(this, ID_TIMER_IDE_LED),
	  hostfs_led_timer_(this, ID_TIMER_HOSTFS_LED),
	  network_led_timer_(this, ID_TIMER_NETWORK_LED)
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
	nat_list_dialog_ = std::make_unique<NatListDialog>(this, emulator_.get());

#ifdef RPCEMU_VNC
	vnc_server_ = std::make_unique<VncServer>(emulator_.get());
	g_vnc_server = vnc_server_.get();
	if (config_copy_.vnc_enabled) {
		vnc_server_->start(config_copy_.vnc_port, std::string(config_copy_.vnc_password));
	}
#endif

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

	window_active_ = true;
	UpdateMachineStatus();
}

MainFrame::~MainFrame()
{
#ifdef RPCEMU_VNC
	if (vnc_server_) {
		vnc_server_->stop();
	}
	g_vnc_server = nullptr;
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
			status += " - Press Ctrl+End to release mouse";
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
		panel_->SetIntegerScaling(config_copy_.integer_scaling != 0);
		panel_->SetFitToWindow(config_copy_.fit_to_window != 0);
	}
	if (config_copy_.fit_to_window) {
		CallAfter([this] { ApplyFitToWindowSize(); });
	}
	SyncSettingsMenuChecks();
	SyncCdromMenuChecks();
	UpdateMachineStatus();

	AddRecentMachine(config_copy_.name);

	if (emulator_) {
		emulator_->Start();
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
		wxMessageBox("Error saving screenshot", "RPCEmu", wxOK | wxICON_WARNING, this);
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
			wxMessageBox("Failed to save the machine state.", "RPCEmu",
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
			             "RPCEmu", wxOK | wxICON_WARNING, this);
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
	SetTitle(wxString::Format("RPCEmu - %s", machine_name));

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
	if (nat_list_dialog_) {
		nat_list_dialog_->ShowModal();
	}
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
		wxMessageDialog dlg(this,
		                    "This window will now be switched to full-screen mode.\n\n"
		                    "To leave full-screen mode press Ctrl+End.\n\n"
		                    "Select 'No' on the next prompt to hide this message in future.",
		                    "RPCEmu - Full-screen mode",
		                    wxOK | wxCANCEL | wxICON_INFORMATION);
		dlg.SetOKCancelLabels("OK", "Cancel");

		if (dlg.ShowModal() != wxID_OK) {
			if (fullscreen_menu_item_ != nullptr) {
				fullscreen_menu_item_->Check(false);
			}
			return;
		}

		if (wxMessageBox("Do not show the full-screen message again?",
		                  "RPCEmu - Full-screen mode",
		                  wxYES_NO | wxICON_QUESTION,
		                  this) == wxYES) {
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
	ShowFullScreen(true, wxFULLSCREEN_ALL);
	full_screen_ = true;

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
		fullscreen_menu_item_->Check(false);
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
	Layout();
	Fit();
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

void MainFrame::OnIntegerScaling(wxCommandEvent &event)
{
	config_copy_.integer_scaling = event.IsChecked() ? 1 : 0;
	if (integer_scaling_menu_item_ != nullptr) {
		integer_scaling_menu_item_->Check(config_copy_.integer_scaling != 0);
	}
	/* Integer scaling and fit-to-window are alternative scaling modes; turning
	   one on turns the other off. */
	if (config_copy_.integer_scaling && config_copy_.fit_to_window) {
		config_copy_.fit_to_window = 0;
		if (fit_to_window_menu_item_ != nullptr) {
			fit_to_window_menu_item_->Check(false);
		}
		if (emulator_) {
			emulator_->FitToWindow();
		}
	}
	if (panel_ != nullptr) {
		panel_->SetFitToWindow(config_copy_.fit_to_window != 0);
		panel_->SetIntegerScaling(config_copy_.integer_scaling != 0);
	}
	if (emulator_) {
		emulator_->IntegerScaling();
	}
}

void MainFrame::OnFitToWindow(wxCommandEvent &event)
{
	config_copy_.fit_to_window = event.IsChecked() ? 1 : 0;
	if (fit_to_window_menu_item_ != nullptr) {
		fit_to_window_menu_item_->Check(config_copy_.fit_to_window != 0);
	}
	/* Mutually exclusive with integer scaling. */
	if (config_copy_.fit_to_window && config_copy_.integer_scaling) {
		config_copy_.integer_scaling = 0;
		if (integer_scaling_menu_item_ != nullptr) {
			integer_scaling_menu_item_->Check(false);
		}
		if (emulator_) {
			emulator_->IntegerScaling();
		}
	}
	if (panel_ != nullptr) {
		panel_->SetIntegerScaling(config_copy_.integer_scaling != 0);
		panel_->SetFitToWindow(config_copy_.fit_to_window != 0);
	}
	if (emulator_) {
		emulator_->FitToWindow();
	}

	/* Give the now freely-resizable window a comfortable starting size, then
	   force a repaint - a static guest desktop sends no fresh frame to trigger
	   one after the resize. */
	if (config_copy_.fit_to_window) {
		ApplyFitToWindowSize();
	}
	if (panel_ != nullptr) {
		panel_->CallAfter([this] {
			if (panel_ != nullptr) {
				panel_->ForceRedraw();
			}
		});
	}
}

/* Size the window to a comfortable default for fit-to-window mode: no larger
   than 80% of the display, and no smaller than a usable floor, while leaving it
   freely resizable by the user afterwards. */
void MainFrame::ApplyFitToWindowSize()
{
	if (!config_copy_.fit_to_window) {
		return;
	}

	const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this)).GetClientArea();
	const int cap_w = std::max(area.width * 4 / 5, 800);
	const int cap_h = std::max(area.height * 4 / 5, 600);
	const wxSize cur = GetSize();
	const int w = std::clamp(cur.x, 800, cap_w);
	const int h = std::clamp(cur.y, 600, cap_h);

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
	VncDialog dlg(this, vnc_server_.get(), wxString::FromUTF8(config_copy_.vnc_password), &config_copy_);
	if (dlg.ShowModal() == wxID_OK) {
		config.vnc_enabled = config_copy_.vnc_enabled;
		config.vnc_port = config_copy_.vnc_port;
		strncpy(config.vnc_password, config_copy_.vnc_password, sizeof(config.vnc_password) - 1);
		config.vnc_password[sizeof(config.vnc_password) - 1] = '\0';
		config_save(&config_copy_);
	}
}
#endif

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

	MachineEditDialog dlg(this, old_config_path,
	                      emulator_ == nullptr || !emulator_->IsRunning(),
	                      emulator_ != nullptr && emulator_->IsRunning());
	if (dlg.ShowModal() != wxID_OK) {
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
	wxMessageBox(
	    "Machine configuration saved.\n\nRestart the emulator for changes to take full effect.",
	    "Machine Configuration",
	    wxOK | wxICON_INFORMATION,
	    this);
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

	SetStatusText(wxString::Format("MIPS: %.1f", mips), STATUS_MIPS);
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

void MainFrame::OnFdcLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_FDC_LED); }
void MainFrame::OnIdeLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_IDE_LED); }
void MainFrame::OnHostfsLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_HOSTFS_LED); }
void MainFrame::OnNetworkLedTimer(wxTimerEvent &) { SetStatusText(wxString(L'\u25cb'), STATUS_NET_LED); }

void MainFrame::ReleaseHeldKeys()
{
	for (auto it = held_keys_.rbegin(); it != held_keys_.rend(); ++it) {
		if (emulator_) {
			emulator_->KeyRelease(*it);
		}
	}
	held_keys_.clear();
}

void MainFrame::NativeKeyPress(unsigned scan_code)
{
	const auto found = std::find(held_keys_.begin(), held_keys_.end(), scan_code);
	if (found != held_keys_.end()) {
		return;
	}

	held_keys_.push_back(scan_code);
	if (emulator_) {
		emulator_->KeyPress(scan_code);
	}
}

void MainFrame::NativeKeyRelease(unsigned scan_code)
{
	const auto found = std::find(held_keys_.begin(), held_keys_.end(), scan_code);
	if (found == held_keys_.end()) {
		return;
	}

	held_keys_.remove(scan_code);
	if (emulator_) {
		emulator_->KeyRelease(scan_code);
	}
}

void MainFrame::ProcessEmulatorKeyEvent(wxKeyEvent &event, bool key_down)
{
	if (menu_open_) {
		return;
	}

	/* No keyboard shortcuts are intercepted here: menu/toolbar actions are
	 * mouse-driven so that every key (function keys like F12, Ctrl combos, etc.)
	 * passes straight through to RISC OS. The only exception is the mouse-capture
	 * / full-screen release key (Ctrl+End), handled below. */

	const int key_code = event.GetKeyCode();
	if (key_code == WXK_NONE || key_code == 0) {
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
		}
		return;
	}

	if (key_code == WXK_MENU) {
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

	const unsigned scan_code = InputNativeScancodeFromKeyEvent(event);
	if (scan_code == 0) {
		event.Skip();
		return;
	}

	if (key_down) {
		NativeKeyPress(scan_code);
	} else {
		NativeKeyRelease(scan_code);
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
	wxMessageBox(wxString::FromUTF8(message), "RPCEmu Error", wxOK | wxICON_WARNING, this);
}

void MainFrame::ShowFatal(const std::string &message)
{
	fatal_occurred_ = true;
	wxMessageBox(wxString::FromUTF8(message), "RPCEmu Fatal Error", wxOK | wxICON_ERROR, this);

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

void MainFrame::PostNatRule(PortForwardRule rule)
{
	CallAfter([rule]() { NatListDialogNotifyRule(rule); });
}

void MainFrame::PostDebuggerStateChanged()
{
	CallAfter([this]() { UpdateDebuggerActionStates(); });
}

void MainFrame::PostMachineSwitched(const std::string &machine_name)
{
	CallAfter([this, machine_name]() {
		SetTitle(wxString::Format("RPCEmu - %s", wxString::FromUTF8(machine_name.c_str())));
		config_deep_copy(&config_copy_, &config);
		model_copy_ = machine.model;
		if (panel_ != nullptr) {
			panel_->UpdateMouseCursor();
			panel_->SetIntegerScaling(config_copy_.integer_scaling != 0);
		}
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
}
