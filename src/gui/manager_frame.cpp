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

#include "manager_frame.h"

#include <wx/artprov.h>
#include <wx/dcmemory.h>
#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/progdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/statbmp.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/filedlg.h>
#include <wx/tokenzr.h>

#include "about_dialog.h"
#include "config_paths.h"
#include "machine_edit_dialog.h"
#include "new_machine_dialog.h"
#include "main_frame.h"	/* MainFrameMenuId - the ids these menus forward */
#include "toolbar_icons.h"
/* C++ headers, so outside the extern "C" below - putting them inside gives
   their functions C linkage at the call site and the link fails. */
#include "folder_transfer.h"
#include "gui_preferences.h"

extern "C" {
#include "machine_lock.h"
#include "rpcemu.h"
}

namespace {

/* Where the sash stops, so a collapsed machine list is still visibly a list
   rather than an edge, and the width of the status bar field the button that
   collapses it sits in. */
const int kMachinesPanelCollapsed = 12;
const int kCollapseButtonField = 28;

enum {
	ID_NEW = wxID_HIGHEST + 500,
	ID_EDIT,
	ID_CLONE,
	ID_DELETE,
	ID_START,
	ID_STOP,
	ID_RESET,
	ID_RESTART,
	ID_RESUME,
	ID_DATA_FOLDER,
	ID_POLL_TIMER,
};

enum {
	kStatusIconRunning = 0,
	kStatusIconStarting,
	kStatusIconStopped,
};

/* How long a --managed child gets to publish its control-channel endpoint and
   become attachable. Only process startup happens in that window - the child
   publishes before rpcemu_start(), so ROM loading and booting RISC OS are on
   the far side of it and a machine reads as "Running" throughout them. */
constexpr int kStartupTimeoutMs = 10000;
constexpr int kPollIntervalMs = 200;

/* A small filled circle for the machine list's status column - cheaper and
   crisper at list-row size than trying to press one of the toolbar's 24x24
   SVGs into service, and it keeps "what's running" glanceable the way
   VirtualBox's own machine list uses a coloured state icon per row.
   Drawn on plain white rather than masked: a masked bitmap picked up a
   visible colour-keyed halo from anti-aliased edge pixels that were close to,
   but not exactly, the mask colour. wxListCtrl rows are white outside
   selection, so the square corners are not noticeable in practice. */
wxBitmap MakeStatusDot(const wxColour &fill)
{
	const int size = 12;
	wxBitmap bmp(size, size);
	wxMemoryDC dc(bmp);

	dc.SetBackground(*wxWHITE_BRUSH);
	dc.Clear();
	dc.SetBrush(wxBrush(fill));
	dc.SetPen(wxPen(fill.ChangeLightness(65)));
	dc.DrawEllipse(0, 0, size, size);
	dc.SelectObject(wxNullBitmap);

	return bmp;
}

} /* namespace */

/*
 * Watches one spawned machine's OS process so the Manager notices it exiting
 * even when the machine's own control channel never gets to say so (a crash,
 * or being killed outside RPCEmu). Deliberately separate from the IPC
 * "Quit"/"Fatal" events, which cover the orderly cases; this covers the rest.
 */
class ManagerChildProcess : public wxProcess {
public:
	ManagerChildProcess(ManagerFrame *owner, wxString machine_name)
		: wxProcess(), owner_(owner), machine_name_(std::move(machine_name))
	{
	}

	void OnTerminate(int pid, int status) override
	{
		if (owner_ != nullptr) {
			owner_->OnChildProcessEnded(machine_name_, pid, status);
		}
		/* wx deletes a Detach()ed process itself once OnTerminate returns;
		   otherwise (the owner is still around) this object is owned by
		   the RunningMachine entry and is cleaned up there. */
	}

	void Forget() { owner_ = nullptr; }

private:
	ManagerFrame *owner_;
	wxString machine_name_;
};

wxBEGIN_EVENT_TABLE(ManagerFrame, wxFrame)
	EVT_LIST_ITEM_SELECTED(wxID_ANY, ManagerFrame::OnMachineSelected)
	EVT_LIST_ITEM_ACTIVATED(wxID_ANY, ManagerFrame::OnMachineActivated)
	EVT_LIST_ITEM_RIGHT_CLICK(wxID_ANY, ManagerFrame::OnMachineRightClick)
	EVT_BUTTON(ID_NEW, ManagerFrame::OnNew)
	EVT_BUTTON(ID_EDIT, ManagerFrame::OnEdit)
	EVT_BUTTON(ID_CLONE, ManagerFrame::OnClone)
	EVT_BUTTON(ID_DELETE, ManagerFrame::OnDelete)
	EVT_MENU(ID_NEW, ManagerFrame::OnNew)
	EVT_MENU(ID_EDIT, ManagerFrame::OnEdit)
	EVT_MENU(ID_CLONE, ManagerFrame::OnClone)
	EVT_MENU(ID_DELETE, ManagerFrame::OnDelete)
	EVT_MENU(ID_START, ManagerFrame::OnStart)
	EVT_MENU(ID_RESUME, ManagerFrame::OnResume)
	EVT_MENU(ID_DATA_FOLDER, ManagerFrame::OnDataFolder)
	EVT_MENU(ID_STOP, ManagerFrame::OnStop)
	EVT_MENU(ID_RESET, ManagerFrame::OnReset)
	EVT_MENU(ID_RESTART, ManagerFrame::OnRestart)
	EVT_MENU(wxID_EXIT, ManagerFrame::OnExit)
	EVT_CLOSE(ManagerFrame::OnClose)
	EVT_TIMER(ID_POLL_TIMER, ManagerFrame::OnPollTimer)
wxEND_EVENT_TABLE()

ManagerFrame::ManagerFrame()
	: wxFrame(nullptr, wxID_ANY, "RPCEmu Extended", wxDefaultPosition, wxSize(1100, 750))
	, poll_timer_(this, ID_POLL_TIMER)
{
	SetMinSize(wxSize(760, 520));

	BuildUi();
	BuildMenus();
	BuildToolBar();
	/* Again, now the toolbar exists: BuildMenus() runs before it and so can
	   only reach the menu items. */
	UpdateMachineMenuState();
	RefreshMachineList();
	DiscoverAlreadyRunningMachines();
	poll_timer_.Start(kPollIntervalMs);
}

ManagerFrame::~ManagerFrame()
{
}

void ManagerFrame::BuildStatusImages()
{
	status_images_ = new wxImageList(12, 12, true);
	status_images_->Add(MakeStatusDot(wxColour(46, 160, 67)));	/* running: green */
	status_images_->Add(MakeStatusDot(wxColour(214, 158, 46)));	/* starting: amber */
	status_images_->Add(MakeStatusDot(wxColour(160, 160, 160)));	/* stopped: grey */
}

wxPanel *ManagerFrame::BuildPlaceholderPage()
{
	auto *placeholder = new wxPanel(display_book_);
	placeholder->SetBackgroundColour(wxColour(32, 32, 36));

	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->AddStretchSpacer();

	const wxString icon_path = wxString::FromUTF8(rpcemu_get_resourcedir()) +
	    "resources" + wxFileName::GetPathSeparator() + "rpcemu.png";
	wxImage icon_image;
	if (wxFileExists(icon_path) && icon_image.LoadFile(icon_path, wxBITMAP_TYPE_PNG)) {
		icon_image.Rescale(64, 64, wxIMAGE_QUALITY_HIGH);
		auto *icon_bitmap = new wxStaticBitmap(placeholder, wxID_ANY, wxBitmap(icon_image));
		sizer->Add(icon_bitmap, 0, wxALIGN_CENTER | wxBOTTOM, 16);
	}

	auto *text = new wxStaticText(placeholder, wxID_ANY,
	    "Select a machine on the left and press Start.");
	text->SetForegroundColour(wxColour(190, 190, 190));
	sizer->Add(text, 0, wxALIGN_CENTER);

	sizer->AddStretchSpacer();
	placeholder->SetSizer(sizer);
	return placeholder;
}

void ManagerFrame::BuildUi()
{
	BuildStatusImages();

	auto *splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	    wxSP_LIVE_UPDATE | wxSP_3DSASH);
	/* As small as the collapsed list, and no smaller: at zero, double-clicking
	   the sash takes the machine list away altogether. */
	splitter->SetMinimumPaneSize(kMachinesPanelCollapsed);
	splitter_ = splitter;

	auto *left_panel = new wxPanel(splitter);
	auto *left_sizer = new wxBoxSizer(wxVERTICAL);

	auto *heading = new wxStaticText(left_panel, wxID_ANY, "Machines");
	wxFont heading_font = heading->GetFont();
	heading_font.SetWeight(wxFONTWEIGHT_BOLD);
	heading_font.SetPointSize(heading_font.GetPointSize() + 1);
	heading->SetFont(heading_font);
	left_sizer->Add(heading, 0, wxALL, 10);

	machine_list_ = new wxListCtrl(left_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	    wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
	machine_list_->AssignImageList(status_images_, wxIMAGE_LIST_SMALL);
	machine_list_->InsertColumn(0, "Machine", wxLIST_FORMAT_LEFT, 170);
	machine_list_->InsertColumn(1, "Status", wxLIST_FORMAT_LEFT, 90);
	left_sizer->Add(machine_list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

	/* A 2x2 grid rather than one row of four: at a sidebar's natural width,
	   four buttons in a row do not fit their own minimum sizes side by side
	   (Delete was being pushed clean off the edge of the panel), and a grid
	   reads better in a narrow column regardless. */
	auto *manage_grid = new wxFlexGridSizer(2, 2, 6, 6);
	manage_grid->AddGrowableCol(0, 1);
	manage_grid->AddGrowableCol(1, 1);
	new_button_ = new wxButton(left_panel, ID_NEW, "New...");
	edit_button_ = new wxButton(left_panel, ID_EDIT, "Edit...");
	clone_button_ = new wxButton(left_panel, ID_CLONE, "Clone...");
	delete_button_ = new wxButton(left_panel, ID_DELETE, "Delete");
	manage_grid->Add(new_button_, 1, wxEXPAND);
	manage_grid->Add(edit_button_, 1, wxEXPAND);
	manage_grid->Add(clone_button_, 1, wxEXPAND);
	manage_grid->Add(delete_button_, 1, wxEXPAND);
	left_sizer->Add(manage_grid, 0, wxEXPAND | wxALL, 10);

	left_panel->SetSizer(left_sizer);

	display_book_ = new wxSimplebook(splitter);
	placeholder_page_ = display_book_->GetPageCount();
	display_book_->AddPage(BuildPlaceholderPage(), "", true);

	splitter->SplitVertically(left_panel, display_book_, 300);

	auto *root = new wxBoxSizer(wxVERTICAL);
	root->Add(splitter, 1, wxEXPAND);
	SetSizer(root);

	/*
	 * Two fields: a narrow one the collapse button sits over, and the machine
	 * count. A status bar does not lay out children itself, so the button is
	 * placed by hand over the first field and moved again whenever the bar is
	 * resized.
	 */
	wxStatusBar *status = CreateStatusBar(2);
	const int widths[] = { kCollapseButtonField, -1 };

	status->SetStatusWidths(2, widths);
	SetStatusBarPane(1);

	collapse_button_ = new wxBitmapButton(status, wxID_ANY,
	    wxArtProvider::GetBitmap(wxART_GO_BACK, wxART_BUTTON, wxSize(16, 16)),
	    wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
	collapse_button_->SetToolTip("Hide the machine list");
	collapse_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
		SetMachinesPanelCollapsed(!machines_panel_collapsed_);
	});
	status->Bind(wxEVT_SIZE, &ManagerFrame::OnStatusBarSize, this);
	PositionCollapseButton();
}

/* Over the first status bar field, which is sized for it. */
void ManagerFrame::PositionCollapseButton()
{
	wxStatusBar *status = GetStatusBar();
	wxRect rect;

	if (collapse_button_ == nullptr || status == nullptr ||
	    !status->GetFieldRect(0, rect)) {
		return;
	}

	wxSize size = collapse_button_->GetBestSize();

	size.y = wxMin(size.y, rect.height);
	collapse_button_->SetSize(rect.x + (rect.width - size.x) / 2,
	    rect.y + (rect.height - size.y) / 2, size.x, size.y);
}

void ManagerFrame::OnStatusBarSize(wxSizeEvent &event)
{
	wxStatusBar *status = GetStatusBar();

	PositionCollapseButton();
	/* Moving the button by hand leaves the fields unpainted. */
	if (status != nullptr) {
		status->Refresh();
	}
	event.Skip();
}

/* Collapse or restore the machine list, and turn the button round. */
void ManagerFrame::SetMachinesPanelCollapsed(bool collapsed)
{
	if (collapsed) {
		const int width = splitter_->GetSashPosition();

		if (width > kMachinesPanelCollapsed) {
			machines_panel_width_ = width;
		}
		splitter_->SetSashPosition(kMachinesPanelCollapsed);
	} else {
		splitter_->SetSashPosition(machines_panel_width_);
	}
	machines_panel_collapsed_ = collapsed;

	if (collapse_button_ != nullptr) {
		collapse_button_->SetBitmap(wxArtProvider::GetBitmap(
		    collapsed ? wxART_GO_FORWARD : wxART_GO_BACK,
		    wxART_BUTTON, wxSize(16, 16)));
		collapse_button_->SetToolTip(collapsed ? "Show the machine list"
		                                       : "Hide the machine list");
	}
}

/*
 * Full screen: the machine being shown, and nothing else.
 *
 * The machine list, the toolbar and the status bar all go, because full screen
 * that keeps its furniture is not full screen. The panel is left holding the
 * whole window, and takes focus so the keyboard reaches the guest.
 */
void ManagerFrame::EnterFullScreen()
{
	if (full_screen_) {
		return;
	}

	auto it = running_.find(active_machine_);
	RemoteEmulatorPanel *panel =
	    it != running_.end() ? it->second.panel : nullptr;

	if (panel == nullptr) {
		wxMessageBox("Start a machine first - there is nothing to show full "
		             "screen.", "RPCEmu", wxOK | wxICON_INFORMATION, this);
		SetFullScreenMenuChecked(false);
		return;
	}

	collapsed_before_full_screen_ = machines_panel_collapsed_;
	SetMachinesPanelCollapsed(true);

	if (tool_bar_ != nullptr) {
		tool_bar_->Hide();
	}
	if (GetStatusBar() != nullptr) {
		GetStatusBar()->Hide();
	}

	full_screen_ = true;
	ShowFullScreen(true, wxFULLSCREEN_ALL);
	Layout();
	panel->SetFocus();
	SetFullScreenMenuChecked(true);
}

void ManagerFrame::ExitFullScreen()
{
	if (!full_screen_) {
		return;
	}

	full_screen_ = false;
	ShowFullScreen(false);

	if (tool_bar_ != nullptr) {
		tool_bar_->Show();
	}
	if (GetStatusBar() != nullptr) {
		GetStatusBar()->Show();
	}
	SetMachinesPanelCollapsed(collapsed_before_full_screen_);
	Layout();
	PositionCollapseButton();
	SetFullScreenMenuChecked(false);

	auto it = running_.find(active_machine_);

	if (it != running_.end() && it->second.panel != nullptr) {
		it->second.panel->SetFocus();
	}
}

/*
 * Keep the tick-box and the toolbar button agreeing with reality.
 *
 * Both are ordinarily set from the machine's own state report, which is no use
 * here: full screen is this window's business now, and the machine has no
 * opinion about it.
 */
void ManagerFrame::SetFullScreenMenuChecked(bool checked)
{
	wxMenuBar *bar = GetMenuBar();

	if (bar != nullptr) {
		wxMenuItem *item = bar->FindItem(ID_MENU_FULLSCREEN);

		if (item != nullptr && item->IsCheckable()) {
			item->Check(checked);
		}
	}
	if (tool_bar_ != nullptr && tool_bar_->GetToolsCount() > 0) {
		/* Added with AddTool rather than AddCheckTool, so there is no state
		   to set - nothing to do here beyond not pretending otherwise. */
	}
}

void ManagerFrame::BuildMenus()
{
	auto *file_menu = new wxMenu();
	file_menu->Append(ID_NEW, "&New Machine...\tCtrl+N");
	file_menu->Append(ID_EDIT, "&Edit Machine...");
	file_menu->Append(ID_CLONE, "&Clone Machine...");
	file_menu->Append(ID_DELETE, "&Delete Machine");
	file_menu->AppendSeparator();
	file_menu->Append(ID_DATA_FOLDER, "Data Folder...")
	    ->SetHelp("Where RPCEmu keeps machines, ROMs and settings");
	file_menu->AppendSeparator();
	file_menu->Append(wxID_EXIT, "E&xit\tCtrl+Q");

	auto *machine_menu = new wxMenu();
	start_item_ = machine_menu->Append(ID_START, "&Start\tCtrl+S");
	resume_item_ = machine_menu->Append(ID_RESUME, "Res&ume");
	resume_item_->SetHelp(
	    "Start this machine from the state it was suspended in");
	stop_item_ = machine_menu->Append(ID_STOP, "S&top");
	machine_menu->AppendSeparator();
	reset_item_ = machine_menu->Append(ID_RESET, "&Reset");
	restart_item_ = machine_menu->Append(ID_RESTART, "Re&start");

	auto *menu_bar = new wxMenuBar();
	menu_bar->Append(file_menu, "&File");
	menu_bar->Append(machine_menu, "&Machine");
	BuildMachineMenus(menu_bar);
	SetMenuBar(menu_bar);
	UpdateMachineMenuState();
}

/*
 * The machine window's own menus, rebuilt on this window.
 *
 * These are the same items, with the same ids, as MainFrame::BuildMenus()
 * creates - which is what lets every one of them be forwarded by id with no
 * per-command code at either end. The wording is kept identical too: somebody
 * who has used the machine window should not have to learn a second vocabulary
 * for the same commands.
 *
 * They are deliberately not built by calling into main_frame_menus.cpp. That
 * function binds handlers, reads a running machine's configuration and updates
 * items from emulator state, none of which exists in this process; the shared
 * thing here is the list of commands, and it is shared through the id enum.
 */
void ManagerFrame::BuildMachineMenus(wxMenuBar *menu_bar)
{
	machine_file_menu_ = new wxMenu();
	machine_file_menu_->Append(ID_MENU_SCREENSHOT, "Screenshot...");
	machine_file_menu_->Append(ID_MENU_SAVE_STATE, "Save State...");
	machine_file_menu_->Append(ID_MENU_LOAD_STATE, "Load State...");
	machine_file_menu_->AppendSeparator();
	machine_file_menu_->Append(ID_MENU_SUSPEND, "Suspend");
	machine_file_menu_->AppendCheckItem(ID_MENU_SUSPEND_ON_EXIT, "Suspend on Exit");

	machine_disc_menu_ = new wxMenu();
	auto *floppy_menu = new wxMenu();
	floppy_menu->Append(ID_MENU_LOAD_DISC0, "Load Drive :0...");
	floppy_menu->Append(ID_MENU_LOAD_DISC1, "Load Drive :1...");
	floppy_menu->AppendSeparator();
	floppy_menu->Append(ID_MENU_EJECT_DISC0, "Eject Drive :0");
	floppy_menu->Append(ID_MENU_EJECT_DISC1, "Eject Drive :1");
	floppy_menu->AppendSeparator();
	floppy_menu->Append(ID_MENU_CREATE_DISC0, "Create Disc in :0...");
	floppy_menu->Append(ID_MENU_CREATE_DISC1, "Create Disc in :1...");
	machine_disc_menu_->AppendSubMenu(floppy_menu, "&Floppy");

	auto *cdrom_menu = new wxMenu();
	cdrom_menu->Append(ID_MENU_CDROM_DISABLED, "Disabled");
	cdrom_menu->Append(ID_MENU_CDROM_EMPTY, "Empty");
	cdrom_menu->Append(ID_MENU_CDROM_ISO, "ISO Image...");
	machine_disc_menu_->AppendSubMenu(cdrom_menu, "&CD-ROM");

	machine_settings_menu_ = new wxMenu();
	machine_settings_menu_->Append(ID_MENU_MACHINE, "Machine Settings...");
	machine_settings_menu_->Append(ID_MENU_NAT_LIST, "NAT Port Forwarding...");
	machine_settings_menu_->Append(ID_MENU_VNC, "VNC Server...");
	machine_settings_menu_->Append(ID_MENU_SERIAL, "Serial Port...");
	machine_settings_menu_->Append(ID_MENU_PARALLEL, "Parallel Port...");
	machine_settings_menu_->Append(ID_MENU_USB, "USB Devices...");
	machine_settings_menu_->AppendSeparator();
	machine_settings_menu_->AppendCheckItem(ID_MENU_MUTE, "Mute Sound");
	machine_settings_menu_->AppendCheckItem(ID_MENU_FULLSCREEN, "Fullscreen");
	machine_settings_menu_->AppendCheckItem(ID_MENU_INTEGER_SCALING, "Integer Scaling");
	machine_settings_menu_->AppendCheckItem(ID_MENU_FIT_TO_WINDOW, "Fit to Window");
	machine_settings_menu_->AppendCheckItem(ID_MENU_FOLLOW_HOST_DISPLAY,
	    "Follow Host Display");
	machine_settings_menu_->AppendSeparator();
	machine_settings_menu_->AppendCheckItem(ID_MENU_MOUSE_TWOBUTTON, "Two-Button Mouse");
	machine_settings_menu_->AppendCheckItem(ID_MENU_SHARED_CLIPBOARD, "Shared Clipboard");
	machine_settings_menu_->AppendCheckItem(ID_MENU_CPU_IDLE, "Reduce CPU Usage");
	machine_settings_menu_->AppendCheckItem(ID_MENU_DEFAULT_MACHINE, "Default Machine");

	machine_tools_menu_ = new wxMenu();
	machine_tools_menu_->Append(ID_MENU_PACKAGES, "Package Manager...");

	machine_debug_menu_ = new wxMenu();
	machine_debug_menu_->Append(ID_MENU_DEBUG_RUN, "Run");
	machine_debug_menu_->Append(ID_MENU_DEBUG_PAUSE, "Pause");
	machine_debug_menu_->AppendSeparator();
	machine_debug_menu_->Append(ID_MENU_DEBUG_STEP, "Step");
	machine_debug_menu_->Append(ID_MENU_DEBUG_STEP5,
	    wxString::FromUTF8("Step \xC3\x97" "5"));
	machine_debug_menu_->AppendSeparator();
	machine_debug_menu_->Append(ID_MENU_MACHINE_INSPECTOR, "Machine Inspector...");

	machine_help_menu_ = new wxMenu();
	machine_help_menu_->Append(ID_MENU_ONLINE_MANUAL, "Online Manual");
	machine_help_menu_->Append(ID_MENU_VISIT_WEBSITE, "Visit Website");
	machine_help_menu_->Append(ID_MENU_REPORT_ISSUE, "Report an Issue");
	machine_help_menu_->Append(ID_MENU_SUPPORT_BUNDLE, "Create Support Bundle...");
	machine_help_menu_->Append(ID_MENU_CHECK_UPDATE, "Check for Updates...");
	machine_help_menu_->AppendSeparator();
	machine_help_menu_->Append(ID_MENU_ABOUT_RISCOS, "About RISC OS");
	machine_help_menu_->Append(wxID_ABOUT, "About RPCEmu");

	menu_bar->Append(machine_file_menu_, "&Media");
	menu_bar->Append(machine_disc_menu_, "&Disc");
	menu_bar->Append(machine_settings_menu_, "&Settings");
	menu_bar->Append(machine_tools_menu_, "&Tools");
	menu_bar->Append(machine_debug_menu_, "De&bug");
	menu_bar->Append(machine_help_menu_, "&Help");

	/*
	 * One binding for the lot. Every id in the range belongs to the machine
	 * window, so there is nothing to decide here beyond which machine to send
	 * it to - which is what makes a command added to that window in future
	 * work here without being mentioned.
	 */
	Bind(wxEVT_MENU, &ManagerFrame::OnMachineMenuCommand, this,
	    ID_MENU_SCREENSHOT, ID_MENU_CHECK_UPDATE);
	Bind(wxEVT_MENU, &ManagerFrame::OnMachineMenuCommand, this, wxID_ABOUT);
}

void ManagerFrame::BuildToolBar()
{
	const wxSize icon_size(24, 24);

	/* New/Edit/Clone/Delete are not here: they already have their own buttons
	   beside the list, where they act on whichever row is selected the same
	   way, and duplicating them added width without adding anything a user
	   could not already do.
	 *
	 * Everything after the machine controls mirrors the machine window's own
	 * toolbar, tool for tool and icon for icon, and forwards by id exactly as
	 * the menus do. A toolbar that offered a third of what the menus did was
	 * the most visible half of "not on parity with the machine window".
	 */
	tool_bar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_NODIVIDER);
	tool_bar_->SetToolBitmapSize(icon_size);

	/* Manager's own: starting and stopping a machine is this window's job,
	   and has no equivalent in a machine that is already running. */
	tool_bar_->AddTool(ID_START, "Start", ToolbarIconPower(icon_size, true),
	    "Start the selected machine");
	tool_bar_->AddTool(ID_STOP, "Stop", ToolbarIconPower(icon_size, false),
	    "Stop the selected machine");
	tool_bar_->AddSeparator();

	/* The machine window's toolbar, in its order. */
	tool_bar_->AddTool(ID_MENU_SCREENSHOT, wxEmptyString, ToolbarIconScreenshot(icon_size),
	    "Save screenshot");
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_LOAD_DISC0, wxEmptyString, ToolbarIconFloppy(icon_size),
	    "Load floppy disc into drive :0");
	tool_bar_->AddTool(ID_MENU_CDROM_ISO, wxEmptyString, ToolbarIconCdrom(icon_size),
	    "Load CD-ROM ISO image");
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_RESET, wxEmptyString, ToolbarIconReset(icon_size),
	    "Reset machine");
	tool_bar_->AddSeparator();
	tool_bar_->AddCheckTool(ID_MENU_MUTE, wxEmptyString,
	    ToolbarIconMute(false, icon_size), wxNullBitmap, "Toggle sound mute");
	tool_bar_->AddTool(ID_MENU_FULLSCREEN, wxEmptyString, ToolbarIconFullscreen(icon_size),
	    "Toggle full-screen mode");
	tool_bar_->AddTool(ID_MENU_MACHINE, wxEmptyString, ToolbarIconConfigure(icon_size),
	    "Edit machine settings");

	tool_bar_->AddStretchableSpace();
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_DEBUG_RUN, wxEmptyString, ToolbarIconDebugRun(icon_size),
	    "Run emulation");
	tool_bar_->AddTool(ID_MENU_DEBUG_PAUSE, wxEmptyString, ToolbarIconDebugPause(icon_size),
	    "Pause emulation");
	tool_bar_->AddTool(ID_MENU_DEBUG_STEP, wxEmptyString, ToolbarIconDebugStep(icon_size),
	    "Single step");
	tool_bar_->AddTool(ID_MENU_MACHINE_INSPECTOR, wxEmptyString, ToolbarIconInspector(icon_size),
	    "Open Machine Inspector");
	tool_bar_->Realize();

	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnStart, this, ID_START);
	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnStop, this, ID_STOP);
	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnReset, this, ID_RESET);

	/* The forwarded tools, over the same id range the menus use, so a tool and
	   its menu item are the same command by construction. */
	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnMachineMenuCommand, this,
	    ID_MENU_SCREENSHOT, ID_MENU_CHECK_UPDATE);
}

wxString ManagerFrame::MachineDirFor(const wxString &name) const
{
	/* Named, not current: this process never loads any machine's
	   configuration, so rpcemu_get_machine_datadir() has nothing to answer. */
	char dir[1024];

	rpcemu_machine_datadir_for(dir, sizeof(dir), name.utf8_str().data());
	return wxString::FromUTF8(dir);
}

void ManagerFrame::RefreshMachineList()
{
	const wxString was_selected = SelectedMachineName();

	machine_list_->DeleteAllItems();
	machine_names_.clear();

	long index = 0;
	long selected_index = -1;
	size_t running_count = 0;

	for (const std::string &name_utf8 : ConfigPathsMachineNames()) {
		const wxString name = wxString::FromUTF8(name_utf8);
		int image = kStatusIconStopped;
		wxString status;

		const auto it = running_.find(name);
		if (it != running_.end()) {
			if (it->second.starting) {
				image = kStatusIconStarting;
				status = "Starting...";
			} else {
				image = kStatusIconRunning;
				status = "Running";
				running_count++;
			}
		}

		machine_names_.push_back(name);
		machine_list_->InsertItem(index, name, image);
		machine_list_->SetItem(index, 1, status);

		if (name == was_selected) {
			selected_index = index;
		}
		index++;
	}

	if (selected_index >= 0) {
		machine_list_->SetItemState(selected_index, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
		    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
	}

	running_count_ = running_count;
	UpdateStatusText();

	UpdateButtons();
}

/*
 * Which machine is being shown, as well as how many there are.
 *
 * Clicking the empty part of the list deselects it without changing which
 * machine is displayed, and that highlight was the only thing saying which.
 */
void ManagerFrame::UpdateStatusText()
{
	SetTitle(active_machine_.empty()
	    ? wxString("RPCEmu Extended")
	    : wxString::Format("RPCEmu Extended - %s", active_machine_));

	SetStatusText(wxString::Format("Current machine: %s   |   %zu machine%s, %zu running",
	    active_machine_.empty() ? wxString("None") : active_machine_,
	    machine_names_.size(), machine_names_.size() == 1 ? "" : "s",
	    running_count_), 1);
}

wxString ManagerFrame::SelectedMachineName() const
{
	const long sel = machine_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	if (sel < 0 || (size_t) sel >= machine_names_.size()) {
		return wxString();
	}
	return machine_names_[(size_t) sel];
}

void ManagerFrame::UpdateButtons()
{
	const wxString name = SelectedMachineName();
	const bool have_selection = !name.empty();
	const bool is_running = have_selection && running_.count(name) != 0;
	const bool is_live = is_running && running_.at(name).panel != nullptr &&
	    running_.at(name).panel->IsLive();

	edit_button_->Enable(have_selection && !is_running);
	clone_button_->Enable(have_selection);
	delete_button_->Enable(have_selection && !is_running);

	if (tool_bar_ != nullptr) {
		tool_bar_->EnableTool(ID_START, have_selection && !is_running);
		tool_bar_->EnableTool(ID_STOP, is_running);
		tool_bar_->EnableTool(ID_RESET, is_live);
	}
	if (start_item_ != nullptr) start_item_->Enable(have_selection && !is_running);
	/* Resume only offers itself when there is something to resume from: a
	   machine that has never been suspended has no snapshot, and an item that
	   silently did nothing would be worse than one that is greyed out. */
	if (resume_item_ != nullptr) {
		bool has_snapshot = false;

		if (have_selection && !is_running) {
			const wxString snapshot = ConfigPathsSnapshotForConfig(
			    ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() +
			    name + ".cfg");

			has_snapshot = !snapshot.empty() && wxFileExists(snapshot);
		}
		resume_item_->Enable(has_snapshot);
	}
	if (stop_item_ != nullptr) stop_item_->Enable(is_running);
	if (reset_item_ != nullptr) reset_item_->Enable(is_live);
	if (restart_item_ != nullptr) restart_item_->Enable(is_live);
}

void ManagerFrame::DiscoverAlreadyRunningMachines()
{
	/* A machine can already be running from a previous Manager (or a plain
	   --machine launch, or a --managed process orphaned by a Manager crash)
	   before this window ever existed. Attempt a connection to each
	   configured machine's advertised endpoint; a stale lock file left by a
	   process that no longer exists simply fails to connect, which
	   RemoteEmulatorPanel already reports via IsLive(). */
	for (const std::string &name_utf8 : ConfigPathsMachineNames()) {
		const wxString name = wxString::FromUTF8(name_utf8);
		const wxString dir = MachineDirFor(name);
		long pid = 0;
		int vnc_port = 0;
		char endpoint[700];

		if (!machine_lock_read_owner(dir.utf8_str().data(), &pid, &vnc_port) || pid == 0) {
			continue;
		}

		/*
		 * ★ The lock FILE is not evidence that a machine is running.
		 *
		 * It is written when the lock is taken and left behind if the holder
		 * is killed or crashes - the lock underneath is an flock the kernel
		 * drops on death, but the file stays exactly as it was. Believing it
		 * meant this window attached to machines that had not existed for
		 * hours, failed, and announced that they had not started: an error
		 * about a machine nobody had asked to run, which read as though the
		 * one they HAD just started was broken.
		 */
		if (!machine_lock_owner_alive(pid)) {
			continue;
		}

		if (!machine_lock_read_ipc_endpoint(dir.utf8_str().data(), endpoint, sizeof(endpoint)) ||
		    endpoint[0] == '\0') {
			continue;	/* running, but not as a --managed child - nothing for us to attach to */
		}

		AttachPanelFor(name, wxString::FromUTF8(MachineIpcNameFor(rpcemu_get_datadir(), name.utf8_str().data(), pid)),
		    wxString::FromUTF8(endpoint), false);
	}
	RefreshMachineList();
}

void ManagerFrame::AttachPanelFor(const wxString &name, const wxString &shared_fb_name,
                                  const wxString &ipc_endpoint, bool newly_started)
{
	auto *panel = new RemoteEmulatorPanel(display_book_, shared_fb_name.utf8_str().data(),
	    ipc_endpoint.utf8_str().data());

	if (!panel->IsLive()) {
		const wxString why = panel->AttachError();

		panel->Destroy();

		/* Kept for the deadline to report, this being the only account of why
		   a machine that never appeared did not. */
		auto failed = running_.find(name);
		if (failed != running_.end()) {
			failed->second.last_attach_error = why;
		}

		/* A machine we started is left for the next poll to try again -
		   giving up on the first attempt lost it from the window for the rest
		   of the session, still running and no longer listed. OnPollTimer's
		   deadline decides when to stop. Discovery has no entry to leave: the
		   lock file it was found by is stale. */
		if (!newly_started) {
			RemoveRunningEntry(name);
		}
		return;
	}

	panel->SetGoneCallback([this, name]() { RemoveRunningEntry(name); });

	/* Only the machine being shown drives the menus; a report from one in the
	   background would set this window's tick-boxes from the wrong machine. */
	/* A machine with no window cannot show its own errors, so it sends them
	   here. Named, because the user is looking at a window that lists several
	   machines and an unattributed error says nothing about which. */
	panel->SetErrorCallback([this, name](const wxString &message) {
		wxMessageBox(name + ": " + message, "RPCEmu Extended Manager",
		    wxOK | wxICON_WARNING, this);
	});

	panel->SetStateCallback([this, name](const wxString &report) {
		if (name == active_machine_) {
			ApplyStateReport(report);
		}
	});

	/* Alt+Enter leaves full screen. Answered false when there is none, so the
	   key goes to the guest as any other would. */
	panel->SetLeaveFullScreenCallback([this]() {
		if (!full_screen_) {
			return false;
		}
		ExitFullScreen();
		return true;
	});

	auto it = running_.find(name);
	if (it == running_.end()) {
		RunningMachine rm;
		it = running_.emplace(name, rm).first;
	}
	it->second.starting = false;
	it->second.panel = panel;
	it->second.book_page = display_book_->GetPageCount();
	display_book_->AddPage(panel, name, false);

	RefreshMachineList();
	if (name == SelectedMachineName() || newly_started) {
		ShowMachinePanel(name);
	}
}

void ManagerFrame::StartMachine(const wxString &name, bool resume)
{
	auto existing = running_.find(name);
	if (existing != running_.end()) {
		ShowMachinePanel(name);
		return;
	}

	const wxString exe = wxStandardPaths::Get().GetExecutablePath();
	wxString cmd;
	cmd << '"' << exe << "\" --managed --machine \"" << name << '"';

	/* Resuming is the child's business: it loads the machine's own snapshot
	   during startup, exactly as it does when started with --resume by hand. */
	if (resume) {
		cmd << " --resume";
	}

	/*
	 * ★ --datadir is passed on ONLY when it cannot do any harm, which is not
	 * always.
	 *
	 * The child is a fresh process and works its data directory out from
	 * scratch, so a Manager started with an explicit --datadir would list the
	 * machines in that folder and then launch them against a different one.
	 * Passing it on fixes that.
	 *
	 * But --datadir does more than name the data directory: it makes the
	 * resource directory the same folder (DATA_DIR_FROM_CLI in
	 * data_paths.cpp). In the ordinary installation those are two different
	 * places - machines and settings under the user's data directory, and
	 * default/, resources/ and the podule ROMs beside the binary. Passing
	 * --datadir there sent the child looking for all of that in the data
	 * directory, where it is not, and the machine failed to find its CMOS
	 * template, its HostFS defaults and its card ROMs.
	 *
	 * The two are only ever the same folder when the directory was given
	 * explicitly, which is exactly when the child needs telling. So that is
	 * the test: same folder, pass it on; different folders, say nothing and
	 * let the child resolve them the way this process did.
	 */
	const wxString datadir = wxString::FromUTF8(rpcemu_get_datadir());
	const wxString resourcedir = wxString::FromUTF8(rpcemu_get_resourcedir());

	if (!datadir.empty() && datadir == resourcedir) {
		cmd << " --datadir \"" << datadir << '"';
	}

	auto *process = new ManagerChildProcess(this, name);
	const long pid = wxExecute(cmd, wxEXEC_ASYNC, process);

	if (pid == 0) {
		wxMessageBox("Could not start '" + name + "'.", "RPCEmu Extended Manager",
		    wxOK | wxICON_ERROR, this);
		delete process;
		return;
	}

	RunningMachine rm;
	rm.pid = pid;
	rm.process = process;
	rm.starting = true;
	rm.start_time_ms = wxGetLocalTimeMillis();
	running_[name] = rm;

	RefreshMachineList();
}

void ManagerFrame::OnPollTimer(wxTimerEvent & /*event*/)
{
	if (running_.empty()) {
		return;
	}

	/* Copy the names first: AttachPanelFor()/RemoveRunningEntry() mutate
	   running_, which would invalidate an iterator over it directly. */
	std::vector<wxString> starting_names;
	for (const auto &entry : running_) {
		if (entry.second.starting) {
			starting_names.push_back(entry.first);
		}
	}

	for (const wxString &name : starting_names) {
		auto it = running_.find(name);
		if (it == running_.end() || !it->second.starting) {
			continue;
		}

		const wxString dir = MachineDirFor(name);
		char endpoint[700];
		long pid = 0;
		int vnc_port = 0;

		/* machine_lock_owner_alive() for the same reason as in
		   DiscoverAlreadyRunningMachines(): a machine starting up may still
		   have the previous run's lock file sitting there, and attaching to
		   the pid it names would reach a process that has gone. */
		if (machine_lock_read_owner(dir.utf8_str().data(), &pid, &vnc_port) && pid != 0 &&
		    machine_lock_owner_alive(pid) &&
		    machine_lock_read_ipc_endpoint(dir.utf8_str().data(), endpoint, sizeof(endpoint)) &&
		    endpoint[0] != '\0') {
			AttachPanelFor(name, wxString::FromUTF8(MachineIpcNameFor(rpcemu_get_datadir(), name.utf8_str().data(), pid)),
			    wxString::FromUTF8(endpoint), true);

			/* Still starting means the attempt failed, so fall through to
			   the deadline rather than trying for ever. */
			it = running_.find(name);
			if (it == running_.end() || !it->second.starting) {
				continue;
			}
		}

		if ((wxGetLocalTimeMillis() - it->second.start_time_ms).ToLong() > kStartupTimeoutMs) {
			const wxString why = it->second.last_attach_error;

			RemoveRunningEntry(name);

			wxRichMessageDialog dlg(this,
			    wxString::Format("'%s' started but could not be displayed.", name),
			    "RPCEmu Extended Manager", wxOK | wxICON_ERROR);

			dlg.SetExtendedMessage(
			    "The machine may still be running. Stopping it and starting it "
			    "again usually clears this.");
			if (!why.empty()) {
				dlg.ShowDetailedText(why);
			}
			dlg.ShowModal();
		}
	}
}

void ManagerFrame::ShowMachinePanel(const wxString &name)
{
	auto it = running_.find(name);

	if (active_machine_ != name && !active_machine_.empty()) {
		auto prev = running_.find(active_machine_);
		if (prev != running_.end() && prev->second.panel != nullptr) {
			prev->second.panel->SetActive(false);
		}
	}

	if (it != running_.end() && it->second.panel != nullptr) {
		active_machine_ = name;
		display_book_->SetSelection((size_t) it->second.book_page);
		it->second.panel->SetActive(true);

		/* The menus now belong to a different machine, so ask it what its
		   tick-boxes say rather than leaving the previous machine's answers
		   on display. */
		IpcRequest request;
		request.type = IpcRequestType::RequestState;
		it->second.panel->SendRequest(request);
	} else {
		active_machine_.clear();
		display_book_->SetSelection((size_t) placeholder_page_);
	}
	UpdateStatusText();
	UpdateButtons();
	UpdateMachineMenuState();
}

void ManagerFrame::StopMachine(const wxString &name)
{
	auto it = running_.find(name);
	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}

	IpcRequest request;
	request.type = IpcRequestType::RequestExit;
	it->second.panel->SendRequest(request);
	/* The rest of the teardown (removing the list entry, the display page)
	   happens when the machine confirms it is gone - either the IPC Quit
	   event RemoteEmulatorPanel reports through its GoneCallback, or the OS
	   process exiting, whichever the machine actually manages given
	   whatever state it is in. Asking twice is harmless. */
}

/* Ask every machine to stop, and close once they have. */
void ManagerFrame::StopAllAndClose()
{
	closing_after_stop_ = true;

	std::vector<wxString> names;

	for (const auto &entry : running_) {
		names.push_back(entry.first);
	}
	for (const wxString &name : names) {
		StopMachine(name);
	}

	UpdateButtons();
}

void ManagerFrame::RemoveRunningEntry(const wxString &name)
{
	auto it = running_.find(name);
	if (it == running_.end()) {
		return;
	}

	if (it->second.process != nullptr) {
		auto *proc = static_cast<ManagerChildProcess *>(it->second.process);
		proc->Forget();
		/* Do not delete: if the OS process has already ended, wx has (or
		   will) call OnTerminate and delete it itself once Detach()ed. If
		   it has not, Detach() below hands ownership to wx to clean up
		   whenever it does. Either way this object must not be freed here
		   as well. */
		proc->Detach();
	}

	if (it->second.panel != nullptr) {
		if (active_machine_ == name) {
			active_machine_.clear();
			display_book_->SetSelection((size_t) placeholder_page_);
		}
		it->second.panel->Destroy();
	}

	running_.erase(it);
	RefreshMachineList();

	/* The last one asked to stop before closing has gone, so the window can
	   follow it. Queued rather than closed here: this runs from a panel
	   callback, and Close() would take that panel down underneath it. */
	if (closing_after_stop_ && running_.empty()) {
		CallAfter([this]() { Close(true); });
	}
}

void ManagerFrame::OnChildProcessEnded(const wxString &machine_name, int /*pid*/, int /*status*/)
{
	RemoveRunningEntry(machine_name);
}

void ManagerFrame::OnMachineSelected(wxListEvent & /*event*/)
{
	UpdateButtons();

	const wxString name = SelectedMachineName();
	if (!name.empty() && running_.count(name) != 0 && running_[name].panel != nullptr) {
		ShowMachinePanel(name);
	}
}

void ManagerFrame::OnMachineActivated(wxListEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty()) {
		return;
	}
	StartMachine(name);
}

/* Start and Stop on the machine that was clicked. */
void ManagerFrame::OnMachineRightClick(wxListEvent &event)
{
	/* Right-clicking does not move the selection by itself, so without this the
	   menu would act on whichever machine happened to be selected rather than
	   the one under the pointer. */
	machine_list_->SetItemState(event.GetIndex(),
	    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
	    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}

	const bool is_running = running_.count(name) != 0;
	wxMenu menu;

	menu.Append(ID_START, "Start");
	menu.Append(ID_STOP, "Stop");
	menu.Enable(ID_START, !is_running);
	menu.Enable(ID_STOP, is_running);

	PopupMenu(&menu);
}

void ManagerFrame::OnNew(wxCommandEvent & /*event*/)
{
	NewMachineDialog dlg(this);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}
	RefreshMachineList();
}

void ManagerFrame::OnEdit(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty() || running_.count(name) != 0) {
		return;
	}

	const wxString path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + name + ".cfg";
	MachineEditDialog dlg(this, path);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}
	if (dlg.WasRenamed()) {
		ConfigPathsRenameMachine(name, dlg.GetNewName(), path);
	}
	RefreshMachineList();
}

void ManagerFrame::OnClone(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty()) {
		return;
	}
	const wxString source_path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + name + ".cfg";

	wxTextEntryDialog dlg(this, "New machine name:", "Clone Machine", name + " (Copy)");
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString sanitized = ConfigPathsSanitizeName(dlg.GetValue());
	if (!ConfigPathsIsNameUnique(sanitized)) {
		wxMessageBox("That machine name already exists.", "Clone Machine", wxOK | wxICON_WARNING, this);
		return;
	}
	if (!ConfigPathsCreateMachineDirectory(sanitized)) {
		wxMessageBox("Could not create the new machine's directory.", "Clone Machine",
		    wxOK | wxICON_ERROR, this);
		return;
	}

	const wxString dest_path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + sanitized + ".cfg";
	wxCopyFile(source_path, dest_path);

	/*
	 * The copy still calls itself by the name of the machine it came from, and
	 * that field is what decides which directory a machine uses
	 * (rpcemu_set_machine_datadir, via config_load). A clone left as copied
	 * therefore ran out of the original's directory: the same cmos.ram, the
	 * same HostFS, the same hard discs, and the lock refusing to start it
	 * because the original already held it - under the original's name.
	 */
	{
		wxFileConfig settings(wxEmptyString, wxEmptyString, dest_path, wxEmptyString,
		                      wxCONFIG_USE_RELATIVE_PATH);

		ConfigFileUseGeneralGroup(settings);
		settings.Write("name", sanitized);
		settings.Flush();
	}

	/* Pulsed rather than counted: the copy walks the tree as it goes, and
	   counting it first would mean walking the whole thing twice to tell
	   somebody a number they are not waiting on. */
	bool copied;
	{
		wxProgressDialog progress("Clone Machine",
		    wxString::Format("Copying '%s'...", name), 100, this,
		    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
		int seen = 0;

		copied = ConfigPathsCopyDirectory(MachineDirFor(name),
		    MachineDirFor(sanitized),
		    [&progress, &seen](const wxString &) {
			    if ((++seen & 0x1f) == 0) {
				    progress.Pulse();
			    }
		    });
	}

	if (!copied) {
		wxRemoveFile(dest_path);
		wxMessageBox("Could not copy the machine's files.", "Clone Machine",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	RefreshMachineList();
}

void ManagerFrame::OnDelete(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty() || running_.count(name) != 0) {
		return;
	}

	if (wxMessageBox(wxString::Format(
	                     "Delete '%s'?\n\nThis removes the configuration and all machine data. "
	                     "This cannot be undone.", name),
	                 "Delete Machine", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
		return;
	}

	wxRemoveFile(ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + name + ".cfg");

	/*
	 * The files go one at a time so there is something to report: a machine's
	 * hard disc is thousands of them, and the window answers nothing while they
	 * are removed. What is left afterwards is the empty directories they were
	 * in, which the recursive remove clears in one quick pass - GetAllFiles()
	 * reports files only, so nothing here would otherwise take them.
	 */
	const wxString dir = MachineDirFor(name);
	if (wxDirExists(dir)) {
		wxArrayString files;

		wxDir::GetAllFiles(dir, &files);

		{
			wxProgressDialog progress("Delete Machine",
			    wxString::Format("Deleting '%s'...", name),
			    static_cast<int>(files.GetCount()), this,
			    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

			for (size_t i = 0; i < files.GetCount(); i++) {
				wxRemoveFile(files[i]);
				/* Every so often rather than every file: the update costs more
				   than the unlink, and a bar moving in steps of one is no more
				   informative. */
				if ((i & 0x1f) == 0) {
					progress.Update(static_cast<int>(i));
				}
			}
		}

		if (!wxFileName::Rmdir(dir, wxPATH_RMDIR_RECURSIVE)) {
			wxMessageBox(wxString::Format(
			                 "'%s' has been removed from the list, but its data "
			                 "could not be deleted from\n\n%s\n\nYou may want to "
			                 "remove that folder yourself.",
			                 name, dir),
			             "Delete Machine", wxOK | wxICON_WARNING, this);
		}
	}

	RefreshMachineList();
}

void ManagerFrame::OnStart(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (!name.empty()) {
		StartMachine(name);
	}
}

/*
 * Start the selected machine from the state it was suspended in.
 *
 * Suspend writes the machine's own snapshot beside it, and without this the
 * only way back in was Load State... and knowing where that file lives. A
 * machine set to suspend on exit would otherwise look as though every session
 * had been lost.
 */
void ManagerFrame::OnResume(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}
	StartMachine(name, true);
}


/*
 * Is any machine in this data folder being used by a running RPCEmu?
 *
 * Asked by trying to take each machine's lock, not by looking for a lock FILE.
 * The file is only a hint - it survives a crash, so its presence would refuse
 * the move for a machine that stopped running last Tuesday. The lock underneath
 * it is an flock, which the kernel drops when the holder dies, so acquiring it
 * is an accurate answer. Anything taken here is given straight back.
 */
static bool AnyMachineInUse(const wxString &datadir)
{
	const wxString machines = datadir + wxFileName::GetPathSeparator() +
	    "machines";

	if (!wxDirExists(machines)) {
		return false;
	}

	wxDir dir(machines);
	if (!dir.IsOpened()) {
		/* Cannot tell. Say yes: refusing to move somebody's files is a
		   nuisance, copying them out from under a live machine is not. */
		return true;
	}

	wxString name;
	bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
	while (more) {
		const wxString machine_dir = machines +
		    wxFileName::GetPathSeparator() + name +
		    wxFileName::GetPathSeparator();

		if (machine_lock_acquire(machine_dir.utf8_str().data(), 0) != 0) {
			return true;
		}
		machine_lock_release();
		more = dir.GetNext(&name);
	}
	return false;
}

/*
 * Point RPCEmu at a different data folder, and offer to take the files along.
 *
 * ★ THE DIFFERENCE FROM THE OLD MACHINE SELECTOR. That dialogue was shown
 * before any machine started, so the only machine that could be writing to
 * these files belonged to somebody else's RPCEmu. This window hosts running
 * machines itself, and its own children hold their locks for as long as they
 * run - so AnyMachineInUse() would report the folder busy and give a reason
 * that reads as though another copy of RPCEmu were at fault. Our own machines
 * are therefore checked first, by name, and asked to be stopped.
 *
 * Beyond that the rules are folder_transfer's: verified before anything is
 * deleted, and the pointer only moved once the files have arrived.
 */
void ManagerFrame::OnDataFolder(wxCommandEvent & /*event*/)
{
	const wxString current = wxString::FromUTF8(rpcemu_get_datadir());

	if (!running_.empty()) {
		wxString names;

		for (const auto &entry : running_) {
			if (!names.empty()) {
				names << ", ";
			}
			names << entry.first;
		}
		wxMessageBox(
		    wxString::Format(
		        "Stop the running machine%s first: %s\n\n"
		        "The data folder holds their discs, and moving those while a "
		        "machine is writing to them would corrupt the disc.",
		        running_.size() == 1 ? "" : "s", names),
		    "RPCEmu Extended - Data Folder", wxOK | wxICON_INFORMATION, this);
		return;
	}

	wxDirDialog dlg(this,
	    "Where should RPCEmu keep its machines, ROMs and settings?",
	    current, wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString chosen = dlg.GetPath();

	if (chosen.empty() ||
	    wxFileName(chosen, "").SameAs(wxFileName(current, ""))) {
		return;
	}

	/*
	 * The offer comes first, because it decides what the confirmation should
	 * say. Telling somebody their machines stay behind and then offering to
	 * move them would be two dialogues arguing with each other.
	 */
	unsigned long long bytes = 0;
	const folder_move_facts facts = FolderTransferGatherFacts(current, chosen,
	    AnyMachineInUse(current) || AnyMachineInUse(chosen), &bytes);
	folder_move_reason why = FOLDER_MOVE_OK;
	const folder_move_offer offer = folder_move_decide(&facts, &why);
	FolderTransferChoice choice = FolderTransferChoice::Manual;

	if (offer != FOLDER_MOVE_OFFER_NOTHING ||
	    (why != FOLDER_MOVE_SAME_PLACE && why != FOLDER_MOVE_SOURCE_EMPTY &&
	     why != FOLDER_MOVE_SOURCE_MISSING)) {
		choice = FolderTransferAsk(this, "data folder", current, chosen, offer,
		    why, bytes);
		if (choice == FolderTransferChoice::Cancel) {
			return;
		}
	}

	if (choice == FolderTransferChoice::Move ||
	    choice == FolderTransferChoice::Copy) {
		/* The log lives in the folder being moved and we are holding it open;
		   Windows will not move an open file. Reopens at the new location on
		   the next message. */
		rpclog_close();

		const FolderTransferResult result = FolderTransferRun(this, current,
		    chosen, choice);

		if (!result.ok) {
			wxMessageBox(result.message, "The files were not moved",
			    wxOK | wxICON_ERROR, this);
			/* Pointer left where it was, so the machines are still found. */
			return;
		}
		SetDataDir(chosen.utf8_string());
		wxMessageBox(result.message + "\n\nRestart RPCEmu for the new data "
		    "folder to take effect.", "RPCEmu Extended - Data Folder",
		    wxOK | wxICON_INFORMATION, this);
		return;
	}

	/* Manual: the old behaviour, and the old warning, which is still the right
	   thing to say when the files are staying where they are. */
	const wxString message = wxString::Format(
	    "RPCEmu will use:\n%s\n\n"
	    "Your existing machines, discs and ROMs are NOT moved. They stay in:\n%s\n\n"
	    "If the new folder is empty, RPCEmu will start with no machines and set up "
	    "a fresh folder. You can point it back at any time, and nothing is deleted "
	    "either way.\n\n"
	    "This takes effect when RPCEmu is restarted. Change the data folder?",
	    chosen, current);

	if (wxMessageBox(message, "RPCEmu Extended - Data Folder",
	                 wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
		return;
	}

	SetDataDir(chosen.utf8_string());

	wxMessageBox("The data folder has been changed.\n\n"
	             "Restart RPCEmu for it to take effect.",
	             "RPCEmu Extended - Data Folder", wxOK | wxICON_INFORMATION, this);
}

void ManagerFrame::OnStop(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (!name.empty()) {
		StopMachine(name);
	}
}

void ManagerFrame::OnReset(wxCommandEvent & /*event*/)
{
	if (active_machine_.empty()) {
		return;
	}
	auto it = running_.find(active_machine_);
	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}
	IpcRequest request;
	request.type = IpcRequestType::Reset;
	it->second.panel->SendRequest(request);
}

void ManagerFrame::OnRestart(wxCommandEvent & /*event*/)
{
	if (active_machine_.empty()) {
		return;
	}
	auto it = running_.find(active_machine_);
	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}
	IpcRequest request;
	request.type = IpcRequestType::Restart;
	it->second.panel->SendRequest(request);
}

/*
 * Send a menu command to the machine currently being shown.
 *
 * @return true if it was sent, false if there was no machine to send it to
 */
bool ManagerFrame::SendMenuCommand(int id, bool checked, const wxString &argument)
{
	if (active_machine_.empty()) {
		return false;
	}

	auto it = running_.find(active_machine_);

	if (it == running_.end() || it->second.panel == nullptr) {
		return false;
	}

	IpcRequest request;
	request.type = IpcRequestType::MenuCommand;
	request.arg1 = id;
	request.arg2 = checked ? 1 : 0;

	const wxScopedCharBuffer utf8 = argument.utf8_str();

	if (utf8.length() >= sizeof(request.path)) {
		wxMessageBox("That path is too long to send to the machine.",
		    "RPCEmu", wxOK | wxICON_ERROR, this);
		return false;
	}
	strncpy(request.path, utf8.data(), sizeof(request.path) - 1);
	request.path[sizeof(request.path) - 1] = '\0';

	it->second.panel->SendRequest(request);
	return true;
}

/*
 * Ask for a file here, and send the machine the answer.
 *
 * The dialogue belongs in this process because this is the one with a window:
 * a managed machine never shows its own, so a file dialogue opened over there
 * would appear detached from anything the user was looking at.
 */
void ManagerFrame::ForwardWithFileDialog(int id, const wxString &title,
    const wxString &wildcard, bool save)
{
	wxFileDialog dialog(this, title, "", "", wildcard,
	    save ? (wxFD_SAVE | wxFD_OVERWRITE_PROMPT) : (wxFD_OPEN | wxFD_FILE_MUST_EXIST));

	if (dialog.ShowModal() != wxID_OK) {
		return;
	}
	SendMenuCommand(id, false, dialog.GetPath());
}

/*
 * A menu command chosen on this window rather than on the machine's.
 *
 * Three kinds arrive here. Some are about the application and are answered
 * without a machine at all. Some need a file, which is asked for here and sent
 * as an argument. The rest are forwarded as they are, and run in the machine's
 * own process against its own state.
 */
void ManagerFrame::OnMachineMenuCommand(wxCommandEvent &event)
{
	const int id = event.GetId();

	/* Answered here: these say nothing about any particular machine, so
	   requiring one to be running before the manual can be opened would be
	   an odd thing to insist on. */
	switch (id) {
	/* The same constants the machine window's handlers use, rather than the
	   same addresses typed again - one of the two would eventually be wrong. */
	case ID_MENU_ONLINE_MANUAL:
		wxLaunchDefaultBrowser(URL_MANUAL);
		return;
	case ID_MENU_VISIT_WEBSITE:
		wxLaunchDefaultBrowser(URL_WEBSITE);
		return;
	case ID_MENU_REPORT_ISSUE:
		wxLaunchDefaultBrowser(URL_ISSUES);
		return;
	case ID_MENU_ABOUT_RISCOS:
		/* Opens RISC OS Open's website. It says nothing about any machine, so
		   requiring one to be running before it would work was simply wrong. */
		wxLaunchDefaultBrowser(URL_RISCOSOPEN);
		return;
	case wxID_ABOUT:
		ShowAboutDialog();
		return;
	default:
		break;
	}

	/*
	 * Answered here rather than forwarded. A managed machine's own window never
	 * receives frames - they go to shared memory for this window to draw - so
	 * asking it to go full screen filled the screen with its empty window.
	 */
	if (id == ID_MENU_FULLSCREEN) {
		if (full_screen_) {
			ExitFullScreen();
		} else {
			EnterFullScreen();
		}
		return;
	}

	if (active_machine_.empty() || running_.find(active_machine_) == running_.end()) {
		wxMessageBox("Start a machine first - this command acts on the machine "
		             "being shown.", "RPCEmu", wxOK | wxICON_INFORMATION, this);
		return;
	}

	/* Needs a file, which is chosen here. */
	switch (id) {
	case ID_MENU_SCREENSHOT: {
		/*
		 * ★ Taken here, not by the machine.
		 *
		 * A managed machine's own panel never receives frames - they are
		 * published into shared memory instead - so asking it to screenshot
		 * saved an empty 640x480 window and reported success. This window has
		 * the pixels it is displaying, so it writes them itself, at the
		 * guest's own resolution.
		 */
		auto it = running_.find(active_machine_);

		if (it == running_.end() || it->second.panel == nullptr) {
			return;
		}

		wxFileDialog dialog(this, "Save Screenshot", "", "screenshot.png",
		    "PNG files (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

		if (dialog.ShowModal() != wxID_OK) {
			return;
		}
		if (!it->second.panel->SaveScreenshot(dialog.GetPath())) {
			wxMessageBox("Could not save the screenshot.", "RPCEmu",
			    wxOK | wxICON_WARNING, this);
		}
		return;
	}
	case ID_MENU_SAVE_STATE:
		ForwardWithFileDialog(id, "Save State",
		    "RPCEmu state files (*.rpcemu)|*.rpcemu", true);
		return;
	case ID_MENU_LOAD_STATE:
		ForwardWithFileDialog(id, "Load State",
		    "RPCEmu state files (*.rpcemu)|*.rpcemu", false);
		return;
	case ID_MENU_LOAD_DISC0:
	case ID_MENU_LOAD_DISC1:
		ForwardWithFileDialog(id, "Load Disc Image",
		    "Disc images (*.adf;*.hfe;*.img)|*.adf;*.hfe;*.img|All files (*.*)|*.*",
		    false);
		return;
	case ID_MENU_CDROM_ISO:
		ForwardWithFileDialog(id, "Open ISO Image",
		    "ISO images (*.iso)|*.iso|All files (*.*)|*.*", false);
		return;
	case ID_MENU_CREATE_DISC0:
	case ID_MENU_CREATE_DISC1:
		ForwardWithFileDialog(id, "Create Disc Image",
		    "ADFS disc images (*.adf)|*.adf", true);
		return;
	default:
		break;
	}

	/* Everything else goes across as it stands. A tick-box has already moved
	   on this window, so its new state travels with it and the machine sets
	   its own copy to match before running the handler. */
	SendMenuCommand(id, event.IsChecked());
}

/*
 * Enable the machine menus only while a machine is being shown.
 *
 * The Help items that this window answers itself stay enabled: they work with
 * no machine running, and grey items that would have worked read as a fault.
 */
void ManagerFrame::UpdateMachineMenuState()
{
	const bool have_machine = !active_machine_.empty() &&
	    running_.find(active_machine_) != running_.end();

	wxMenuBar *bar = GetMenuBar();

	if (bar == nullptr) {
		return;
	}

	/*
	 * Help stays usable with no machine running.
	 *
	 * Four of these are answered by this window and need nothing else. The
	 * other two do need a machine, and are left enabled anyway: clicking them
	 * says which machine they act on and that one has to be running, whereas
	 * greying them out says only that something is wrong, with no way to find
	 * out what. A menu that explains itself beats one that goes quiet.
	 */
	static const int always_enabled[] = {
		ID_MENU_ONLINE_MANUAL, ID_MENU_VISIT_WEBSITE,
		ID_MENU_REPORT_ISSUE, ID_MENU_ABOUT_RISCOS, wxID_ABOUT,
		ID_MENU_SUPPORT_BUNDLE, ID_MENU_CHECK_UPDATE,
	};

	for (wxMenu *menu : { machine_file_menu_, machine_disc_menu_,
	                      machine_settings_menu_, machine_tools_menu_,
	                      machine_debug_menu_, machine_help_menu_ }) {
		if (menu == nullptr) {
			continue;
		}
		for (const wxMenuItem *item : menu->GetMenuItems()) {
			if (item->IsSeparator()) {
				continue;
			}
			if (item->IsSubMenu()) {
				for (const wxMenuItem *sub : item->GetSubMenu()->GetMenuItems()) {
					if (!sub->IsSeparator()) {
						bar->Enable(sub->GetId(), have_machine);
					}
				}
				continue;
			}
			bar->Enable(item->GetId(), have_machine);
		}
	}

	for (int id : always_enabled) {
		bar->Enable(id, true);
	}

	/* The forwarded tools go the same way as their menu items - they are the
	   same commands, so a live tool beside a greyed menu entry would be two
	   answers to the same question. */
	if (tool_bar_ != nullptr) {
		static const int forwarded_tools[] = {
			ID_MENU_SCREENSHOT, ID_MENU_LOAD_DISC0, ID_MENU_CDROM_ISO,
			ID_MENU_RESET, ID_MENU_MUTE, ID_MENU_FULLSCREEN,
			ID_MENU_MACHINE, ID_MENU_DEBUG_RUN, ID_MENU_DEBUG_PAUSE,
			ID_MENU_DEBUG_STEP, ID_MENU_MACHINE_INSPECTOR,
		};

		for (int id : forwarded_tools) {
			tool_bar_->EnableTool(id, have_machine);
		}
	}
}

/*
 * Set this window's tick-boxes from what the machine says they are.
 *
 * Sent by the machine when asked and after every forwarded command. Parsed
 * loosely on purpose: an id this window does not have is skipped rather than
 * treated as an error, so the two processes need not be in lockstep about
 * which items exist.
 */
void ManagerFrame::ApplyStateReport(const wxString &report)
{
	wxMenuBar *bar = GetMenuBar();

	if (bar == nullptr) {
		return;
	}

	wxStringTokenizer pairs(report, " ");

	while (pairs.HasMoreTokens()) {
		const wxString pair = pairs.GetNextToken();
		const int equals = pair.Find('=');

		if (equals == wxNOT_FOUND) {
			continue;
		}

		long id = 0;
		long value = 0;

		if (!pair.Left(equals).ToLong(&id) ||
		    !pair.Mid(equals + 1).ToLong(&value)) {
			continue;
		}

		wxMenuItem *item = bar->FindItem((int) id);

		if (item != nullptr && item->IsCheckable()) {
			item->Check(value != 0);
		}

		/* Mute also has a toolbar tool, which has to agree with its menu
		   item or the toolbar shows the opposite of the truth. */
		if (id == ID_MENU_MUTE && tool_bar_ != nullptr) {
			tool_bar_->ToggleTool(ID_MENU_MUTE, value != 0);
		}
	}
}

/*
 * About, shown by this window.
 *
 * Deliberately not forwarded, and deliberately not a second About box written
 * for the Manager. AboutDialog takes any parent, so this is the same dialogue
 * the machine window shows - which matters because it carries the version and
 * the credits, and a copy would be wrong the first time either changed.
 *
 * It is answered here rather than in a machine because it is about RPCEmu, not
 * about any machine, and so has to work with none running.
 */
void ManagerFrame::ShowAboutDialog()
{
	AboutDialog dlg(this);

	dlg.ShowModal();
}

void ManagerFrame::OnExit(wxCommandEvent & /*event*/)
{
	Close(true);
}

void ManagerFrame::OnClose(wxCloseEvent &event)
{
	/*
	 * A machine outliving this window is the intended behaviour, but not an
	 * obvious one: nothing on screen afterwards says the machine is still
	 * there. Say so, and offer the other choice.
	 */
	if (!running_.empty() && !closing_after_stop_) {
		const size_t count = running_.size();
		wxRichMessageDialog dlg(this,
		    wxString::Format("%zu machine%s still running.", count,
		        count == 1 ? " is" : "s are"),
		    "RPCEmu Extended Manager", wxOK | wxCANCEL | wxICON_QUESTION);

		dlg.SetExtendedMessage(
		    "Closing this window leaves them running in the background.\n\n"
		    "Opening RPCEmu Extended again reconnects to them.");
		dlg.ShowCheckBox("Stop the running machines first", false);

		if (dlg.ShowModal() != wxID_OK) {
			event.Veto();
			return;
		}

		if (dlg.IsCheckBoxChecked()) {
			StopAllAndClose();
			event.Veto();	/* closes itself once they have gone */
			return;
		}
	}

	/* Running machines are independent processes and are left running,
	   exactly as closing VirtualBox's or VMware Workstation's manager
	   window does not stop the VMs it was showing - only Stop, or closing
	   the machine itself, does that. Detach every child so its eventual
	   OnTerminate does not call back into a ManagerFrame that no longer
	   exists. */
	for (auto &entry : running_) {
		if (entry.second.process != nullptr) {
			auto *proc = static_cast<ManagerChildProcess *>(entry.second.process);
			proc->Forget();
			proc->Detach();
		}
		/* Here rather than in the panel's destructor: closing a connection
		   waits for its reader thread, and doing that as the window came down
		   stopped the window coming down at all. */
		if (entry.second.panel != nullptr) {
			entry.second.panel->CloseConnection();
		}
	}
	poll_timer_.Stop();
	event.Skip();
}
