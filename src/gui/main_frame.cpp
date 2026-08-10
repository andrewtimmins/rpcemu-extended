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

#include "machine_ipc.h"

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
	a.integer_scaling = b.integer_scaling = 0;
	a.fit_to_window = b.fit_to_window = 0;
	a.follow_host_display = b.follow_host_display = 0;
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

void MainFrame::EnableManagedMode()
{
	managed_mode_ = true;

	const std::string machine_dir = rpcemu_get_machine_datadir();

	shared_fb_ = std::make_unique<SharedFramebuffer>();
	if (!shared_fb_->CreateNew(MachineIpcNameFor(machine_dir))) {
		rpclog("MainFrame: could not create the shared framebuffer; "
		       "this machine will not be visible to the Manager\n");
	}

	ipc_server_ = std::make_unique<MachineIpcServer>();

#ifdef _WIN32
	const std::string control_endpoint;	/* --managed always gets an OS-assigned TCP port on Windows */
#else
	const std::string control_endpoint = machine_dir + "manager.sock";
#endif

	if (ipc_server_->Start(control_endpoint,
	        [this](const IpcRequest &request) { HandleIpcRequest(request); })) {
		machine_lock_set_ipc_endpoint(ipc_server_->BoundEndpoint().c_str());
	} else {
		rpclog("MainFrame: could not start the machine control channel; "
		       "this machine will not be controllable from the Manager\n");
	}
}

void MainFrame::MirrorToSharedFramebuffer(const VideoUpdate &update)
{
	if (!shared_fb_ || update.buffer == nullptr || update.xsize <= 0 || update.ysize <= 0) {
		return;
	}

	/* Copies synchronously before returning, so - unlike the wxImage path
	   below - this is safe to call directly from the VIDC worker thread with
	   no heap copy or CallAfter hop: update.buffer is only valid until this
	   call returns, and by the time it does, the frame is already in shared
	   memory. */
	shared_fb_->Publish(update.buffer, update.xsize, update.ysize);

	if (ipc_server_) {
		IpcEvent event;
		event.type = IpcEventType::FrameReady;
		ipc_server_->SendEvent(event);
	}
}

/*
 * Dispatch a request that arrived over the machine control channel.
 *
 * Runs on MachineIpcServer's own thread, never the GUI thread - which is why
 * this goes straight to EmulatorHost's public methods rather than through the
 * menu handlers (OnReset() and friends) that call them on the window's
 * behalf: those methods are themselves just PostCommand() onto a mutex-
 * protected queue the emulator thread drains, already safe to call from a
 * foreign thread. The VNC server's keyboard/pointer callbacks call the same
 * methods the same way today.
 */
void MainFrame::HandleIpcRequest(const IpcRequest &request)
{
	if (!emulator_) {
		return;
	}

	switch (request.type) {
	case IpcRequestType::KeyPress:
		emulator_->KeyPress((unsigned) request.arg1);
		break;
	case IpcRequestType::KeyRelease:
		emulator_->KeyRelease((unsigned) request.arg1);
		break;
	case IpcRequestType::MouseMove:
		emulator_->MouseMove(request.arg1, request.arg2);
		break;
	case IpcRequestType::MouseMoveRelative:
		emulator_->MouseMoveRelative(request.arg1, request.arg2);
		break;
	case IpcRequestType::MousePress:
		emulator_->MousePress(request.arg1);
		break;
	case IpcRequestType::MouseRelease:
		emulator_->MouseRelease(request.arg1);
		break;
	case IpcRequestType::MouseWheel:
		emulator_->MouseWheel(request.arg1);
		break;
	case IpcRequestType::Reset:
		emulator_->Reset();
		break;
	case IpcRequestType::Restart:
		emulator_->Restart();
		break;
	case IpcRequestType::RequestExit:
		/* Close() touches wx window state, so it has to run on the GUI
		   thread; everything above is a plain EmulatorHost call and needs
		   no hop. */
		CallAfter([this]() { Close(true); });
		break;
	case IpcRequestType::LoadDisc0:
		emulator_->LoadDisc(0, request.path);
		break;
	case IpcRequestType::LoadDisc1:
		emulator_->LoadDisc(1, request.path);
		break;
	case IpcRequestType::EjectDisc0:
		emulator_->EjectDisc(0);
		break;
	case IpcRequestType::EjectDisc1:
		emulator_->EjectDisc(1);
		break;
	case IpcRequestType::CdromDisabled:
		emulator_->CdromDisabled();
		break;
	case IpcRequestType::CdromEmpty:
		emulator_->CdromEmpty();
		break;
	case IpcRequestType::CdromLoadIso:
		emulator_->CdromLoadIso(request.path);
		break;
	case IpcRequestType::RequestKeyFrame:
		/* Nothing to do: the shared framebuffer always holds the most
		   recently published frame regardless of who has asked for it, so a
		   Manager tab that has just switched to this machine can read it
		   immediately without waiting for the guest to draw something new. */
		break;

	case IpcRequestType::MenuCommand:
		/* arg2 is the tick-box state for a checkable item, and the chosen
		   file-type for Create Disc; no command needs both. */
		DispatchMenuCommand(request.arg1, request.arg2 != 0,
		    wxString::FromUTF8(request.path), request.arg2);
		break;

	case IpcRequestType::RequestState:
		/* Reads menu items, so it belongs on the GUI thread like the rest
		   of the wx object graph. */
		CallAfter([this] { ReportMenuState(); });
		break;
	}
}

/*
 * The file a command should act on.
 *
 * Normally this asks, exactly as it always has. When the command was forwarded
 * from the Manager, the file has already been chosen there and travels with the
 * request, so there is nothing to ask.
 *
 * ★ That is not a shortcut, it is the only thing that can work.
 *
 * A managed machine's window is never shown. A wxFileDialog opened on it would
 * be modal to a window that is not on screen: on some platforms it appears with
 * no owner and no taskbar entry, on others behind the Manager, and in every case
 * the user is looking at a Manager that has silently stopped responding to the
 * menu they just used. The dialogue therefore belongs to the process with a
 * window, and only the answer crosses.
 *
 * @param title    Dialogue title, when one is shown
 * @param wildcard Dialogue file filter
 * @param save     Whether this is a save rather than an open
 * @param path     Where the chosen file is written
 * @return         true if there is a file to act on
 */
bool MainFrame::AskForFile(const wxString &title, const wxString &wildcard,
    bool save, wxString *path, const wxString &default_dir,
    const wxString &default_file)
{
	if (!pending_menu_argument_.empty()) {
		*path = pending_menu_argument_;
		return true;
	}

	wxFileDialog dlg(this, title, default_dir, default_file, wildcard,
	    save ? (wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
	         : (wxFD_OPEN | wxFD_FILE_MUST_EXIST));

	if (dlg.ShowModal() != wxID_OK) {
		return false;
	}
	*path = dlg.GetPath();
	return true;
}

/*
 * Turn a menu id that arrived from the Manager back into a menu event on this
 * window.
 *
 * The point of doing it this way round is that nothing below has to know which
 * command it is. ProcessWindowEvent() runs the same handler the menu item is
 * bound to, so every command the machine window has is available to the Manager,
 * including ones added after this was written.
 *
 * ★ CallAfter, because this arrives on MachineIpcServer's thread.
 *
 * Unlike the input and disc requests above - which go to EmulatorHost methods
 * that are explicitly safe to call from a foreign thread - a menu handler is
 * ordinary GUI code. It reads menu items, opens dialogues and touches the window
 * hierarchy, none of which may be done from anywhere but the GUI thread.
 */
void MainFrame::DispatchMenuCommand(int id, bool checked, const wxString &argument,
    int filter)
{
	CallAfter([this, id, checked, argument, filter] {
		/*
		 * A tick-box handler asks the event whether it is now ticked, and
		 * for a menu event wx answers from the item rather than the event,
		 * so the item has to be set before the event is sent. The Manager
		 * has already moved its own copy, and sends where it ended up.
		 */
		wxMenuBar *bar = GetMenuBar();
		wxMenu *owner = nullptr;
		wxMenuItem *item = (bar != nullptr) ? bar->FindItem(id, &owner) : nullptr;

		if (item != nullptr && item->IsCheckable()) {
			item->Check(checked);
		}

		if (!argument.empty()) {
			pending_menu_argument_ = argument;
			pending_menu_filter_ = filter;
		}

		wxCommandEvent event(wxEVT_MENU, id);
		event.SetInt(checked ? 1 : 0);
		event.SetEventObject(this);

		/*
		 * ★ Sent to the menu that owns the item, not to this window.
		 *
		 * BindMenuItem() binds each command on the wxMenu it was appended to,
		 * not on the frame - only wxID_ABOUT, wxID_EXIT and wxID_PREFERENCES
		 * are bound here, because macOS moves those out of our menus. A real
		 * click reaches the menu's handler because the event starts there.
		 * An event handed to ProcessWindowEvent() starts at the frame, and a
		 * wxMenu is not in that chain, so it went nowhere: every forwarded
		 * command was accepted, reported as sent, and did nothing.
		 *
		 * The frame is still tried afterwards, for the three ids bound there
		 * and for anything bound that way in future.
		 */
		bool handled = false;

		if (owner != nullptr) {
			handled = owner->ProcessEvent(event);
		}
		if (!handled) {
			handled = ProcessWindowEvent(event);
		}
		if (!handled) {
			rpclog("MainFrame: forwarded menu command %d had no handler\n", id);
		}

		pending_menu_argument_.clear();
		pending_menu_filter_ = 0;

		/* The command may well have moved a tick-box - muting sound, say -
		   so tell the Manager where things stand rather than leaving its
		   copy to drift. */
		ReportMenuState();
	});
}

/*
 * Tell the Manager what this machine's tick-box menu items currently say.
 *
 * Sent when asked, and again after every forwarded command, since a command can
 * change an item other than the one that was clicked.
 */
void MainFrame::ReportMenuState()
{
	if (!ipc_server_) {
		return;
	}

	static const int checkable[] = {
		ID_MENU_MUTE,
		ID_MENU_FULLSCREEN,
		ID_MENU_INTEGER_SCALING,
		ID_MENU_FIT_TO_WINDOW,
		ID_MENU_FOLLOW_HOST_DISPLAY,
		ID_MENU_SUSPEND_ON_EXIT,
		ID_MENU_CPU_IDLE,
		ID_MENU_MOUSE_TWOBUTTON,
		ID_MENU_SHARED_CLIPBOARD,
		ID_MENU_DEFAULT_MACHINE,
	};

	wxMenuBar *bar = GetMenuBar();

	if (bar == nullptr) {
		return;
	}

	wxString report;

	for (int id : checkable) {
		wxMenuItem *item = bar->FindItem(id);

		if (item == nullptr || !item->IsCheckable()) {
			continue;
		}
		if (!report.empty()) {
			report += " ";
		}
		report += wxString::Format("%d=%d", id, item->IsChecked() ? 1 : 0);
	}

	IpcEvent event;
	event.type = IpcEventType::StateReport;
	const wxScopedCharBuffer utf8 = report.utf8_str();

	/* Truncation would only cost the Manager a tick-box it cannot see the
	   state of, but the list is far shorter than the field, so it does not
	   happen in practice. */
	strncpy(event.path, utf8.data(), sizeof(event.path) - 1);
	event.path[sizeof(event.path) - 1] = '\0';
	ipc_server_->SendEvent(event);
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
	wxString path;

	if (!AskForFile("Save Screenshot", "PNG (*.png)|*.png", true, &path)) {
		return;
	}

	if (panel_ == nullptr || !panel_->SaveScreenshot(path)) {
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

	wxString path;

	if (!AskForFile("Save Machine State",
	        "RPCEmu machine state (*.state)|*.state|All files (*)|*", true, &path,
	        snapshot.GetPath(), snapshot.GetFullName())) {
		return;
	}

	if (emulator_) {
		if (!emulator_->SaveState(path.utf8_str().data())) {
			wxMessageBox("Failed to save the machine state.", "RPCEmu Extended",
			             wxOK | wxICON_WARNING, this);
		}
	}
}

void MainFrame::OnLoadState(wxCommandEvent &)
{
	const wxFileName snapshot(ConfigPathsSnapshotForConfig(
	    ConfigPathsAbsoluteConfigPath(wxString::FromUTF8(config_get_path()))));

	wxString path;

	if (!AskForFile("Load Machine State",
	        "RPCEmu machine state (*.state)|*.state|All files (*)|*", false, &path,
	        snapshot.GetPath())) {
		return;
	}

	if (emulator_) {
		std::string error;
		if (!emulator_->LoadState(path.utf8_str().data(), &error)) {
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
	wxString path;

	if (!AskForFile("Open Disc Image",
	        "All disc images (*.adf;*.adl;*.hfe;*.img)|*.adf;*.adl;*.hfe;*.img|"
	        "ADFS D/E/F Disc Image (*.adf)|*.adf|"
	        "ADFS L Disc Image (*.adl)|*.adl|"
	        "DOS Disc Image (*.img)|*.img|"
	        "HFE Disc Image (*.hfe)|*.hfe", false, &path)) {
		return;
	}

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

	wxString chosen_path;
	int filter_index = 0;

	/*
	 * ★ This one needs more than a path.
	 *
	 * Which disc is created comes from the dialogue's selected filter, not
	 * from the file name, so a forwarded command has to carry that choice as
	 * well - it arrives in pending_menu_filter_. Sending only the path would
	 * silently create an ADFS F image whatever the user picked, which is the
	 * kind of wrong that is not noticed until the guest cannot read the disc.
	 */
	if (!pending_menu_argument_.empty()) {
		chosen_path = pending_menu_argument_;
		filter_index = pending_menu_filter_;
	} else {
		wxFileDialog dlg(this, "Create Blank Disc Image", wxEmptyString,
		    wxEmptyString, filter, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

		if (dlg.ShowModal() != wxID_OK) {
			return;
		}
		chosen_path = dlg.GetPath();
		filter_index = dlg.GetFilterIndex();
	}

	if (filter_index < 0 || filter_index >= static_cast<int>(WXSIZEOF(kDiscTypeFileMaps))) {
		return;
	}
	const DiscTypeFileMap *disc_type = &kDiscTypeFileMaps[static_cast<size_t>(filter_index)];

	wxString file_name = chosen_path;
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
	wxString iso_path;

	if (!AskForFile("Open ISO Image",
	        "ISO CD-ROM Image (*.iso)|*.iso|All Files (*.*)|*.*", false, &iso_path)) {
		SyncCdromMenuChecks();
		return;
	}

	if (!config_copy_.cdromenabled && !HostResetQuestion(this)) {
		SyncCdromMenuChecks();
		return;
	}

	const wxString path = iso_path;
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
		Layout();
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
		Layout();
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
		NativeKeyPress(InputKeyIdentityFromKeyEvent(event), scan_code);
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

/* Let the guest change screen mode to match the host display.
 *
 * Not mutually exclusive with the scaling options, because they answer the same
 * question differently rather than incompatibly: these two keep the guest's mode
 * and scale the picture, this changes the guest's mode. The combination worth
 * having is full-screen plus this, which gives a crisp desktop at the monitor's
 * own resolution with no scaling at all.
 *
 * Takes effect from the next display change: switching it on does not retune the
 * mode the desktop is already in, which also means ticking it never reflows
 * anyone's windows by surprise. */
void MainFrame::OnFollowHostDisplay(wxCommandEvent &event)
{
	config_copy_.follow_host_display = event.IsChecked() ? 1 : 0;
	if (follow_host_display_menu_item_ != nullptr) {
		follow_host_display_menu_item_->Check(config_copy_.follow_host_display != 0);
	}
	if (emulator_) {
		emulator_->FollowHostDisplay();
	}
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

	if (geom.width > 0 && geom.height > 0) {
		const wxVideoMode mode = display.GetCurrentMode();

		rpcemu_set_host_display((unsigned) geom.width, (unsigned) geom.height,
		                        mode.refresh > 0 ? (unsigned) mode.refresh : 0);
	}

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
	if (managed_mode_) {
		/* No window is shown, so there is nothing for panel_ to paint: send
		   the frame to the Manager instead of building a wxBitmap nobody
		   will ever see. Safe from any thread - see MirrorToSharedFramebuffer. */
		MirrorToSharedFramebuffer(update);
		return;
	}

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
	/*
	 * ★ A managed machine must never move the host pointer.
	 *
	 * This exists so the guest can keep the host cursor with the RISC OS
	 * pointer, and it works by converting guest coordinates through this
	 * window and calling WarpPointer(). Under the Manager this window is
	 * never shown, so it has no position on screen: ScreenToClient() cannot
	 * answer (wx says so, repeatedly, in the log) and the warp lands at
	 * whatever an unmapped window's origin comes out as - the corner of the
	 * host display. Moving the mouse anywhere near the Manager threw the
	 * cursor into that corner, over and over.
	 *
	 * The Manager owns the pointer for a machine it is showing: its panel
	 * takes real mouse events and sends them over, and nothing needs to warp
	 * the host cursor for that. So this is dropped here, exactly as
	 * PostVideoUpdate drops painting into a window nobody can see.
	 */
	if (managed_mode_) {
		return;
	}

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
	/*
	 * ★ A managed machine has no window to put this over.
	 *
	 * wxMessageBox on a frame that is never shown is modal to something the
	 * user cannot see: on some platforms it appears with no owner, on others
	 * behind the Manager, and either way the machine stops until somebody
	 * finds it. A NAT rule that could not bind its host port - the ordinary
	 * way two machines collide - reported itself exactly there, so the
	 * forwarding silently did not work and the explanation was invisible.
	 *
	 * Sent to the Manager instead, which has the window.
	 */
	if (managed_mode_ && ipc_server_) {
		IpcEvent event;

		/* Unprefixed: the Manager knows which machine this arrived from
		   and says so, which it can do and this cannot. */
		event.type = IpcEventType::Error;
		strncpy(event.path, message.c_str(), sizeof(event.path) - 1);
		event.path[sizeof(event.path) - 1] = '\0';
		ipc_server_->SendEvent(event);
		return;
	}

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
