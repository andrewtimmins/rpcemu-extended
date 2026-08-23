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
#include "manager_frame.h"

#include "display_options.h"
#include "settings_labels.h"

#include "window_owner.h"

#include "manager_settings_dialog.h"

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
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/ffile.h>
#include <wx/tokenzr.h>

#include "about_dialog.h"
#include "check_update.h"
#include "support_bundle.h"
#include "config_paths.h"
#include "machine_edit_dialog.h"
#include "new_machine_dialog.h"
#include "machine_ipc.h"	/* IpcRequestType, kStateFullscreenMessage */
#include "main_frame.h"	/* MainFrameMenuId - the ids these menus forward */
#include "toolbar_icons.h"
/* C++ headers, so outside the extern "C" below - putting them inside gives
   their functions C linkage at the call site and the link fails. */
#include "folder_transfer.h"
#include "gui_preferences.h"
#include "machine_shortcut.h"

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

/* The status bar's right-hand field, wide enough for the longest thing that
   goes in it: "MIPS: 123.4 (idle)   FPS: 60.0". */
const int kSpeedField = 230;

/* Status holds "Suspended" and no more; Machine takes the rest of the width. */
const int kStatusColumnWidth = 90;

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
	ID_DISCARD_STATE,
	ID_SHORTCUT,
	ID_MINIMAL_UI,
	ID_MACHINE_LIST,
	ID_DATA_FOLDER,
	ID_POLL_TIMER,
};

/* Not the machine's ids for the same menu, and never sent anywhere: the chosen
   size travels as a width and height. */
enum {
	ID_SCREEN_SIZE_FIRST = wxID_HIGHEST + 600,
	ID_SCREEN_SIZE_LAST = ID_SCREEN_SIZE_FIRST + 63,
};

enum {
	kStatusIconRunning = 0,
	kStatusIconStarting,
	kStatusIconStopped,
	kStatusIconSuspended,
};

/* How long a --managed child gets to publish its control-channel endpoint and
   become attachable. Only process startup happens in that window - the child
   publishes before rpcemu_start(), so ROM loading and booting RISC OS are on
   the far side of it and a machine reads as "Running" throughout them. */
constexpr int kStartupTimeoutMs = 10000;
constexpr int kPollIntervalMs = 200;

/* "1920x1080". False rather than a partial answer: a size that did not parse is
   one the menu must not offer. */
bool ParseScreenSize(const wxString &text, unsigned &width, unsigned &height)
{
	const int x = text.Find('x');
	long w = 0, h = 0;

	if (x == wxNOT_FOUND ||
	    !text.Left(x).ToLong(&w) || !text.Mid(x + 1).ToLong(&h) ||
	    w <= 0 || h <= 0) {
		return false;
	}
	width = (unsigned) w;
	height = (unsigned) h;
	return true;
}

/* "1920x1080,1600x1200,..." */
void ParseScreenModes(const wxString &text,
    std::vector<std::pair<unsigned, unsigned>> &out)
{
	wxStringTokenizer modes(text, ",");

	out.clear();
	while (modes.HasMoreTokens()) {
		unsigned w = 0, h = 0;

		if (ParseScreenSize(modes.GetNextToken(), w, h)) {
			out.emplace_back(w, h);
		}
	}
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
	EVT_MENU(ID_DISCARD_STATE, ManagerFrame::OnDiscardState)
	EVT_MENU(ID_DATA_FOLDER, ManagerFrame::OnDataFolder)
	EVT_MENU(wxID_PREFERENCES, ManagerFrame::OnSettings)
	EVT_MENU(ID_STOP, ManagerFrame::OnStop)
	EVT_MENU(ID_RESET, ManagerFrame::OnReset)
	EVT_MENU(ID_RESTART, ManagerFrame::OnRestart)
	EVT_MENU(ID_SHORTCUT, ManagerFrame::OnCreateShortcut)
	EVT_MENU(ID_MINIMAL_UI, ManagerFrame::OnMinimalUi)
	EVT_MENU(ID_MACHINE_LIST, ManagerFrame::OnMachineListToggle)
	EVT_MENU(wxID_EXIT, ManagerFrame::OnExit)
	EVT_CLOSE(ManagerFrame::OnClose)
	EVT_TIMER(ID_POLL_TIMER, ManagerFrame::OnPollTimer)
wxEND_EVENT_TABLE()

ManagerFrame::ManagerFrame()
	: wxFrame(nullptr, wxID_ANY, "RPCEmu Extended", wxDefaultPosition, wxSize(1100, 750))
	, poll_timer_(this, ID_POLL_TIMER)
{
	SetMinSize(wxSize(760, 520));
	SetFrameIcon(this);

	BuildUi();
	BuildMenus();
	BuildToolBar();
	/* Again, now the toolbar exists: BuildMenus() runs before it and so can
	   only reach the menu items. */
	UpdateMachineMenuState();
	RefreshMachineList();
	DiscoverAlreadyRunningMachines();
	/* After the toolbar and the splitter exist, since it hides both. */
	if (GetMinimalUi()) {
		ApplyMinimalUi(true);
	}
	poll_timer_.Start(kPollIntervalMs);

	/* Queued so the window is up before anything can appear over it. */
	CallAfter([this] { CheckForUpdateInBackground(this); });
}

ManagerFrame::~ManagerFrame()
{
}

wxPanel *ManagerFrame::BuildPlaceholderPage()
{
	auto *placeholder = new wxPanel(display_book_);
	placeholder->SetBackgroundColour(wxColour(32, 32, 36));

	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->AddStretchSpacer();

	const wxString icon_path = AppLogoPath();
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
	/*
	 * Bundles, not a wxImageList.
	 *
	 * Handed an image list, wxGTK draws these circles as clipped squares - at
	 * 12 pixels, at 14, at 16, with a mask and without. SetSmallImages takes
	 * bundles and works the size out for the display it is on, which is both
	 * correct on a HiDPI screen and the only one of the three that draws a
	 * round dot. The order matches the kStatusIcon* constants.
	 */
	{
		wxVector<wxBitmapBundle> status_icons;

		status_icons.push_back(StatusIconRunning());
		status_icons.push_back(StatusIconStarting());
		status_icons.push_back(StatusIconStopped());
		status_icons.push_back(StatusIconSuspended());
		machine_list_->SetSmallImages(status_icons);
	}
	machine_list_->InsertColumn(0, "Machine", wxLIST_FORMAT_LEFT, 170);
	machine_list_->InsertColumn(1, "Status", wxLIST_FORMAT_LEFT, kStatusColumnWidth);
	machine_list_->Bind(wxEVT_SIZE, &ManagerFrame::OnMachineListSize, this);
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

	machines_panel_ = left_panel;
	splitter->SplitVertically(left_panel, display_book_, 300);

	auto *root = new wxBoxSizer(wxVERTICAL);
	root->Add(splitter, 1, wxEXPAND);
	SetSizer(root);

	/*
	 * Three fields: a narrow one the collapse button sits over, the machine
	 * count, and how fast the machine being shown is going. A status bar does
	 * not lay out children itself, so the button is placed by hand over the
	 * first field and moved again whenever the bar is resized.
	 */
	wxStatusBar *status = CreateStatusBar(3);
	const int widths[] = { kCollapseButtonField, -1, kSpeedField };

	status->SetStatusWidths(3, widths);
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

/* Two fixed columns in a control that fills the pane leave the rest of the
   header drawn as an empty third column. Machine takes whatever Status does not,
   so the columns always end where the list does. */
void ManagerFrame::OnMachineListSize(wxSizeEvent &event)
{
	if (machine_list_ != nullptr) {
		const int width = machine_list_->GetClientSize().GetWidth()
		    - kStatusColumnWidth;

		if (width > 0) {
			machine_list_->SetColumnWidth(0, width);
		}
	}
	event.Skip();
}

/* Hide or show the machine list, and turn the button round. Hidden means gone:
   a pane collapsed to the sash's minimum is a stripe down the side of the
   machine, and the button on the status bar is what brings it back. */
void ManagerFrame::SetMachinesPanelCollapsed(bool collapsed)
{
	if (collapsed) {
		if (splitter_->IsSplit()) {
			const int width = splitter_->GetSashPosition();

			if (width > kMachinesPanelCollapsed) {
				machines_panel_width_ = width;
			}
			splitter_->Unsplit(machines_panel_);
		}
	} else {
		if (!splitter_->IsSplit() && machines_panel_ != nullptr) {
			splitter_->SplitVertically(machines_panel_, display_book_,
			    machines_panel_width_);
		}
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

	/* The same state from a second place, so the tick follows the button. */
	if (machine_list_item_ != nullptr) {
		machine_list_item_->Check(!collapsed);
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
		             "screen.", "RPCEmu Extended Manager", wxOK | wxICON_INFORMATION, this);
		SetFullScreenMenuChecked(false);
		return;
	}

	/* The same message a machine's own window shows, and the same setting: the
	   machine reported it, and the machine is what stores it. */
	if (it->second.show_fullscreen_message) {
		wxRichMessageDialog dlg(this,
		    "This window will now be switched to full-screen mode.\n\n"
		    "To leave full-screen mode press Alt+Enter.",
		    "RPCEmu Extended - Full-screen mode",
		    wxOK | wxCANCEL | wxICON_INFORMATION);

		dlg.SetOKCancelLabels("OK", "Cancel");
		dlg.ShowCheckBox("Do not show this message again");

		if (dlg.ShowModal() != wxID_OK) {
			SetFullScreenMenuChecked(false);
			return;
		}

		if (dlg.IsCheckBoxChecked()) {
			IpcRequest request;

			request.type = IpcRequestType::FullscreenMessageOff;
			panel->SendRequest(request);
			it->second.show_fullscreen_message = false;
		}
	}

	collapsed_before_full_screen_ = machines_panel_collapsed_;
	if (!machines_panel_collapsed_) {
		machines_panel_width_ = splitter_->GetSashPosition();
	}
	splitter_->Unsplit(machines_panel_);

	if (tool_bar_ != nullptr) {
		tool_bar_->Hide();
	}
	if (GetStatusBar() != nullptr) {
		GetStatusBar()->Hide();
	}

	full_screen_ = true;
	ShowFullScreen(true, wxFULLSCREEN_ALL);
	RelayoutAroundToolBar();
	panel->SetFocus();
	SetFullScreenMenuChecked(true);
}


/*
 * Put the window back together after showing or hiding the toolbar.
 *
 * The frame owns the toolbar's geometry rather than the sizer, so only a size
 * event recomputes the client area around it - Layout() alone lays out the old
 * area. Nor does either imply a repaint: on Windows the toolbar's band keeps
 * stale pixels until something invalidates it.
 */
void ManagerFrame::RelayoutAroundToolBar()
{
	SendSizeEvent();
	Layout();

	if (tool_bar_ != nullptr && tool_bar_->IsShown()) {
		tool_bar_->Refresh();
		tool_bar_->Update();
	}

	Refresh();
	Update();
	PositionCollapseButton();
}

/* Hide the furniture and leave the machine. */
void ManagerFrame::ApplyMinimalUi(bool minimal)
{
	minimal_ui_ = minimal;

	if (tool_bar_ != nullptr) {
		tool_bar_->Show(!minimal);
	}

	SetMachinesPanelCollapsed(minimal);

	if (minimal_ui_item_ != nullptr) {
		minimal_ui_item_->Check(minimal);
	}

	RelayoutAroundToolBar();
}

void ManagerFrame::OnMinimalUi(wxCommandEvent &event)
{
	ApplyMinimalUi(event.IsChecked());
}

void ManagerFrame::OnMachineListToggle(wxCommandEvent &event)
{
	SetMachinesPanelCollapsed(!event.IsChecked());
}

void ManagerFrame::ExitFullScreen()
{
	if (!full_screen_) {
		return;
	}

	full_screen_ = false;
	ShowFullScreen(false);

	/* Back to what it was, which in a minimal window is still hidden. */
	if (tool_bar_ != nullptr) {
		tool_bar_->Show(!minimal_ui_);
	}
	if (GetStatusBar() != nullptr) {
		GetStatusBar()->Show();
	}
	if (!splitter_->IsSplit() && machines_panel_ != nullptr) {
		splitter_->SplitVertically(machines_panel_, display_book_,
		    machines_panel_width_);
	}
	SetMachinesPanelCollapsed(minimal_ui_ || collapsed_before_full_screen_);
	RelayoutAroundToolBar();
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
	file_menu->AppendSeparator();
	/*
	 * RPCEmu's own settings, not a machine's - so wxID_PREFERENCES, which is
	 * what puts it in the application menu on macOS rather than under File.
	 * The data folder used to be an item here, which read like another machine
	 * command; it lives in that window now.
	 */
	file_menu->Append(wxID_PREFERENCES, "&Settings...\tCtrl+,")
	    ->SetHelp("Where RPCEmu keeps its files, and how it draws a machine");
	file_menu->AppendSeparator();
	file_menu->Append(wxID_EXIT, "E&xit\tCtrl+Q");

	auto *machine_menu = new wxMenu();
	machine_menu->Append(ID_EDIT, "&Edit...");
	machine_menu->Append(ID_CLONE, "&Clone...");
	machine_menu->Append(ID_DELETE, "&Delete");
	machine_menu->AppendSeparator();
	start_item_ = machine_menu->Append(ID_START, "&Start\tCtrl+S");
	resume_item_ = machine_menu->Append(ID_RESUME, "Res&ume");
	resume_item_->SetHelp(
	    "Start this machine from the state it was suspended in");
	discard_state_item_ = machine_menu->Append(ID_DISCARD_STATE,
	    "&Discard Suspended State");
	discard_state_item_->SetHelp(
	    "Throw away the saved state, so this machine starts fresh");
	stop_item_ = machine_menu->Append(ID_STOP, "S&top");
	machine_menu->AppendSeparator();
	reset_item_ = machine_menu->Append(ID_RESET, "&Reset");
	restart_item_ = machine_menu->Append(ID_RESTART, "Re&start");
	machine_menu->AppendSeparator();
	machine_menu->Append(ID_MENU_SUSPEND, "Sus&pend");
	machine_menu->AppendCheckItem(ID_MENU_SUSPEND_ON_EXIT, "Suspend on E&xit");
	machine_menu->Append(ID_MENU_SAVE_STATE, "Save State...");
	machine_menu->Append(ID_MENU_LOAD_STATE, "Load State...");
	machine_menu->AppendSeparator();
	machine_menu->Append(ID_MENU_SCREENSHOT, "Screensh&ot...");
	machine_menu->AppendSeparator();
	shortcut_item_ = machine_menu->Append(ID_SHORTCUT, "Create S&hortcut...");
	shortcut_item_->SetHelp(
	    "A shortcut that opens this machine directly, without the manager");

	auto *view_menu = new wxMenu();

	machine_list_item_ = view_menu->AppendCheckItem(ID_MACHINE_LIST,
	    "Machine &List");
	machine_list_item_->Check(true);
	view_menu->AppendSeparator();
	minimal_ui_item_ = view_menu->AppendCheckItem(ID_MINIMAL_UI,
	    "&Minimal Interface");
	minimal_ui_item_->SetHelp(
	    "Hide the toolbar and the machine list, for this session");

	auto *menu_bar = new wxMenuBar();
	menu_bar->Append(file_menu, "&File");
	menu_bar->Append(machine_menu, "&Machine");
	menu_bar->Append(view_menu, "&View");
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
	/* Radio items, as the machine's own window has them: these are which source
	   the drive is using, not three separate things to do. */
	cdrom_disabled_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_DISABLED,
	    "Disabled");
	cdrom_empty_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_EMPTY, "Empty");
	cdrom_iso_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_ISO, "ISO Image...");
	machine_disc_menu_->AppendSubMenu(cdrom_menu, "&CD-ROM");

	machine_settings_menu_ = new wxMenu();
	machine_settings_menu_->Append(ID_MENU_MACHINE, "Machine Settings...");
	nat_list_item_ = machine_settings_menu_->Append(ID_MENU_NAT_LIST,
	    "NAT Port Forwarding...");
	netcap_item_ = machine_settings_menu_->Append(ID_MENU_NETCAP,
	    "Network Capture...");
	machine_settings_menu_->Append(ID_MENU_VNC, "VNC Server...");
	machine_settings_menu_->Append(ID_MENU_SERIAL, "Serial Port...");
	machine_settings_menu_->Append(ID_MENU_PARALLEL, "Parallel Port...");
	machine_settings_menu_->Append(ID_MENU_USB, "USB Devices...");
	machine_settings_menu_->AppendSeparator();
	machine_settings_menu_->AppendCheckItem(ID_MENU_MUTE,
	    SettingsLabels::MuteSound())->SetHelp(SettingsLabels::MuteSoundHelp());
	machine_settings_menu_->AppendCheckItem(ID_MENU_FULLSCREEN,
	    DisplayOptions::FullScreen())->SetHelp(DisplayOptions::FullScreenHelp());
	/*
	 * How the machine draws its screen, in the submenu the machine's own window
	 * puts it in. The group's name is the question the two answers answer, so
	 * flattening it out here left the Manager with a bare pair of radio items
	 * and nothing saying what they were about.
	 *
	 * The screen SIZE is the other half, and the list is learned rather than
	 * computed - RISC OS accepts only the modes its monitor definition declares,
	 * and which those are is found out as they are refused - so it is filled in
	 * from what the machine reports rather than worked out here. Empty, and
	 * greyed, until it does.
	 */
	{
		auto *screen_menu = new wxMenu;

		screen_size_menu_ = screen_menu;
		screen_size_parent_ = machine_settings_menu_->AppendSubMenu(screen_menu,
		    DisplayOptions::ScreenSizeGroup());

		auto *scaling_menu = new wxMenu;

		scaling_menu->AppendRadioItem(ID_MENU_SCALING_ACTUAL,
		    DisplayOptions::ScalingActualSize())
		    ->SetHelp(DisplayOptions::ScalingActualSizeHelp());
		scaling_menu->AppendRadioItem(ID_MENU_SCALING_MULTIPLES,
		    DisplayOptions::ScalingWholeMultiples())
		    ->SetHelp(DisplayOptions::ScalingWholeMultiplesHelp());
		machine_settings_menu_->AppendSubMenu(scaling_menu,
		    DisplayOptions::ScalingGroup());
	}
	machine_settings_menu_->AppendSeparator();
	machine_settings_menu_->AppendCheckItem(ID_MENU_MOUSE_HACK,
	    SettingsLabels::MouseFollows())
	    ->SetHelp(SettingsLabels::MouseFollowsHelp());
	machine_settings_menu_->AppendCheckItem(ID_MENU_MOUSE_TWOBUTTON,
	    SettingsLabels::TwoButtonMouse())
	    ->SetHelp(SettingsLabels::TwoButtonMouseHelp());
	machine_settings_menu_->AppendCheckItem(ID_MENU_SHARED_CLIPBOARD,
	    SettingsLabels::SharedClipboard())
	    ->SetHelp(SettingsLabels::SharedClipboardHelp());
	machine_settings_menu_->AppendCheckItem(ID_MENU_CPU_IDLE,
	    SettingsLabels::ReduceCpu())->SetHelp(SettingsLabels::ReduceCpuHelp());
	machine_settings_menu_->AppendCheckItem(ID_MENU_DEFAULT_MACHINE,
	    SettingsLabels::DefaultMachine())
	    ->SetHelp(SettingsLabels::DefaultMachineHelp());

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
	machine_debug_menu_->Append(ID_MENU_NETWORK_ANALYSER, "Network Analyser...");

	machine_help_menu_ = new wxMenu();
	machine_help_menu_->Append(ID_MENU_ONLINE_MANUAL, "Online Manual");
	machine_help_menu_->Append(ID_MENU_VISIT_WEBSITE, "Visit Website");
	machine_help_menu_->Append(ID_MENU_REPORT_ISSUE, "Report an Issue");
	machine_help_menu_->Append(ID_MENU_SUPPORT_BUNDLE, "Create Support Bundle...");
	machine_help_menu_->Append(ID_MENU_CHECK_UPDATE, "Check for Updates...");
	machine_help_menu_->AppendSeparator();
	machine_help_menu_->Append(ID_MENU_ABOUT_RISCOS, "About RISC OS");
	machine_help_menu_->Append(wxID_ABOUT, "About RPCEmu");

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

	/* Its own handler rather than the range above: these ids are the Manager's
	   own and mean nothing to the machine, so what travels is the size. */
	Bind(wxEVT_MENU, &ManagerFrame::OnScreenSize, this,
	    ID_SCREEN_SIZE_FIRST, ID_SCREEN_SIZE_LAST);
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

	/* Manager's own: a machine's power state is this window's job, and has no
	   equivalent in a machine that is already running. Reset joins them, being
	   the same kind of thing. */
	tool_bar_->AddTool(ID_START, "Start", ToolbarIconPower(icon_size, true),
	    "Start the selected machine");
	tool_bar_->AddTool(ID_RESUME, "Resume", ToolbarIconResume(icon_size),
	    "Start the selected machine from the state it was suspended in");
	tool_bar_->AddTool(ID_MENU_SUSPEND, "Suspend", ToolbarIconSuspend(icon_size),
	    "Save this machine's state and stop it");
	tool_bar_->AddTool(ID_STOP, "Stop", ToolbarIconPower(icon_size, false),
	    "Stop the selected machine");
	/* ID_RESET, this window's own, rather than the machine menu's
	   ID_MENU_RESET: it is bound and greyed here with Start and Stop. */
	tool_bar_->AddTool(ID_RESET, wxEmptyString, ToolbarIconReset(icon_size),
	    "Reset machine");
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
	/* Plain rather than a check tool: the state arrives over IPC, and the two
	   icons say which it is. */
	tool_bar_->AddTool(ID_MENU_MUTE, wxEmptyString,
	    ToolbarIconMute(false, icon_size), "Toggle sound mute");
	tool_bar_->AddTool(ID_MENU_FULLSCREEN, wxEmptyString, ToolbarIconFullscreen(icon_size),
	    "Toggle full-screen mode (Alt+Enter to leave)");
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
	tool_bar_->AddTool(ID_MENU_NETWORK_ANALYSER, wxEmptyString, ToolbarIconAnalyser(icon_size),
	    "Open Network Analyser");
	tool_bar_->Realize();

	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnStart, this, ID_START);
	tool_bar_->Bind(wxEVT_TOOL, &ManagerFrame::OnResume, this, ID_RESUME);
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

	/* Emptying and refilling the rows makes the list announce selections that
	   the user did not make - a deselect from DeleteAllItems() with the names
	   already gone, which would take the active machine away with it. */
	rebuilding_machine_list_ = true;

	machine_list_->DeleteAllItems();
	machine_names_.clear();

	long index = 0;
	long selected_index = -1;
	size_t running_count = 0;

	for (const std::string &name_utf8 : ConfigPathsMachineNames()) {
		const wxString name = wxString::FromUTF8(name_utf8);
		/* Said in words as well as in colour. The column was left empty for a
		   machine that is not running, so the commonest state was the one the
		   list did not name - and a colour on its own is no use to somebody who
		   cannot tell these two apart.

		   Suspended is told apart from stopped for the same reason: it has a
		   state to resume from, and a list that calls them both "Stopped"
		   makes Start look like the only thing to do with it. */
		const auto it = running_.find(name);
		int image;
		wxString status;

		if (it == running_.end()) {
			const bool suspended = HasSnapshot(name);

			image = suspended ? kStatusIconSuspended : kStatusIconStopped;
			status = suspended ? "Suspended" : "Stopped";
		} else if (it->second.starting) {
			image = kStatusIconStarting;
			status = "Starting Up";
		} else {
			image = kStatusIconRunning;
			status = "Running";
			running_count++;
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

	rebuilding_machine_list_ = false;
	running_count_ = running_count;

	/* The rows the user was pointing at are back, so let the selection mean
	   what it did before: the events during the rebuild were ignored. */
	if (SelectedMachineName() != active_machine_) {
		ShowMachinePanel(SelectedMachineName());
	} else {
		RefreshUiState();
	}
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

	wxString status = wxString::Format(
	    "Current machine: %s   |   %zu machine%s, %zu running",
	    active_machine_.empty() ? wxString("None") : active_machine_,
	    machine_names_.size(), machine_names_.size() == 1 ? "" : "s",
	    running_count_);

	/*
	 * What the mouse is doing, but only when it is doing something the user has
	 * to know about. Follow-mouse needs no explanation and is the usual case, so
	 * saying so on every machine would be noise; a captured pointer very much
	 * does need explaining, since the way out of it is a keystroke.
	 */
	auto it = running_.find(active_machine_);

	if (it != running_.end() && it->second.panel != nullptr &&
	    !it->second.panel->FollowHostMouse()) {
		status += it->second.panel->PointerCaptured()
		    ? "   |   Alt+Enter releases the mouse"
		    : "   |   Click to capture the mouse";
	}

	SetStatusText(status, 1);
	UpdateSpeedStatus();
}

/*
 * The speed of the machine being shown, in the right-hand status bar field:
 * MIPS as the machine itself measures it, and the rate its frames are arriving
 * at. The machine window has its own status bar saying the same thing, but a
 * managed machine's window is never shown, so this is the only place the
 * figures appear.
 *
 * Only the machine being displayed, deliberately. Every running machine
 * reports, and all of them could be listed, but a status bar is one line and
 * the question being answered is "how is the one I am watching doing?". A
 * column in the machine list would be the way to show them all.
 *
 * Called from the poll timer, five times a second, against figures that change
 * once a second - so it compares before it sets, and an unchanged status bar is
 * left alone rather than repainted for nothing.
 */
void ManagerFrame::UpdateSpeedStatus()
{
	auto it = running_.find(active_machine_);
	wxString text;

	if (it != running_.end() && it->second.panel != nullptr &&
	    it->second.panel->IsLive() && it->second.panel->HasPerf()) {
		const RemoteEmulatorPanel *panel = it->second.panel;

		/*
		 * "(idle)" for the same reason the machine's own status bar says it:
		 * a machine sitting at the desktop under "Reduce CPU usage" hands its
		 * time back and really is executing almost nothing, and a MIPS figure
		 * near zero without that word reads as something being wrong.
		 */
		text = wxString::Format("MIPS: %.1f%s   FPS: %.1f",
		    panel->Mips(), panel->GuestIdle() ? " (idle)" : "", panel->Fps());
	}

	if (text != speed_text_) {
		speed_text_ = text;
		SetStatusText(text, 2);
	}
}

wxString ManagerFrame::SelectedMachineName() const
{
	const long sel = machine_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	if (sel < 0 || (size_t) sel >= machine_names_.size()) {
		return wxString();
	}
	return machine_names_[(size_t) sel];
}

/* Only when it actually changes: on MSW setting a tool's bitmap re-realizes the
   whole toolbar, and reports arrive repeatedly saying the same thing. */
void ManagerFrame::SetMuteToolState(bool muted)
{
	if (tool_bar_ == nullptr || muted == mute_tool_muted_) {
		return;
	}
	mute_tool_muted_ = muted;
	tool_bar_->SetToolNormalBitmap(ID_MENU_MUTE, ToolbarIconMute(muted));

	/* The new bitmap is not always drawn on its own. */
	tool_bar_->Refresh();
	tool_bar_->Update();
}

/* Everything that depends on which machine is selected, active or running. The
   three are idempotent, so any path that moves a machine can just call this.
   RefreshMachineList() is not here: rebuilding the rows is the costly part. */
/* The machine's own rules, from UpdateDebuggerActionStates: Run and Step want a
   machine that has stopped, Pause one that has not stopped yet - it stays lit
   through the gap between asking and the emulator thread reaching a hook. */
void ManagerFrame::SetDebugToolState(bool paused, bool pausing)
{
	wxMenuBar *bar = GetMenuBar();

	if (bar != nullptr) {
		bar->Enable(ID_MENU_DEBUG_RUN, paused);
		bar->Enable(ID_MENU_DEBUG_PAUSE, !paused || pausing);
		bar->Enable(ID_MENU_DEBUG_STEP, paused);
		bar->Enable(ID_MENU_DEBUG_STEP5, paused);
	}
	if (tool_bar_ != nullptr) {
		tool_bar_->EnableTool(ID_MENU_DEBUG_RUN, paused);
		tool_bar_->EnableTool(ID_MENU_DEBUG_PAUSE, !paused || pausing);
		tool_bar_->EnableTool(ID_MENU_DEBUG_STEP, paused);
		tool_bar_->Refresh();
	}
}

void ManagerFrame::SetCdromMenuState(int source)
{
	wxMenuItem *item = (source == 2) ? cdrom_iso_item_
	    : (source == 1) ? cdrom_empty_item_ : cdrom_disabled_item_;

	if (item != nullptr) {
		item->Check(true);
	}
}

void ManagerFrame::SetNatMenuState(bool is_nat)
{
	if (nat_list_item_ != nullptr) {
		nat_list_item_->Enable(is_nat);
	}
	/* Capture has nothing to capture without networking, and follows the same
	   rule as the item above it rather than offering something that would do
	   nothing. */
	if (netcap_item_ != nullptr) {
		netcap_item_->Enable(is_nat);
	}
}

/*
 * The screen sizes the machine on show has offered, as a menu.
 *
 * Rebuilt rather than patched, like the machine's own: the list is short, and it
 * changes wholesale when a different machine is shown.
 *
 * The submenu is greyed while the list is empty rather than left to open onto
 * nothing. Empty is not only the no-machine case: a machine reports when asked,
 * so there is a moment after it attaches when it is running and has not said
 * yet. The greying loop in UpdateMachineMenuState() cannot do this one - it
 * disables a submenu by disabling the items inside it, and there are none.
 */
void ManagerFrame::RebuildScreenSizeMenu()
{
	if (screen_size_menu_ == nullptr) {
		return;
	}

	for (size_t i = 0; i < screen_size_items_.size(); i++) {
		screen_size_menu_->Delete((int) (ID_SCREEN_SIZE_FIRST + (int) i));
	}
	screen_size_items_.clear();

	auto it = running_.find(active_machine_);

	if (it == running_.end()) {
		if (screen_size_parent_ != nullptr) {
			screen_size_parent_->Enable(false);
		}
		return;
	}

	const RunningMachine &machine = it->second;
	const size_t limit =
	    (size_t) (ID_SCREEN_SIZE_LAST - ID_SCREEN_SIZE_FIRST) + 1;

	for (size_t i = 0; i < machine.screen_modes.size() && i < limit; i++) {
		const unsigned w = machine.screen_modes[i].first;
		const unsigned h = machine.screen_modes[i].second;
		wxMenuItem *item = screen_size_menu_->AppendRadioItem(
		    (int) (ID_SCREEN_SIZE_FIRST + (int) i),
		    DisplayOptions::ModeLabel(w, h));

		item->SetHelp(DisplayOptions::ScreenSizeHelp());
		screen_size_items_.emplace_back(w, h);

		/* The size the machine reports its desktop to be, which is not always
		   the one asked for: RISC OS can change mode from its own end, and a
		   refused size leaves it on the old one. Nothing ticked means the
		   desktop is in a mode this list does not offer. */
		if (w == machine.screen_size_x && h == machine.screen_size_y) {
			item->Check(true);
		}
	}

	if (screen_size_parent_ != nullptr) {
		screen_size_parent_->Enable(!screen_size_items_.empty());
	}
}

void ManagerFrame::OnScreenSize(wxCommandEvent &event)
{
	const size_t index = (size_t) (event.GetId() - ID_SCREEN_SIZE_FIRST);

	if (index >= screen_size_items_.size()) {
		return;		/* Stale id from a menu that has since been rebuilt */
	}

	auto it = running_.find(active_machine_);

	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}

	IpcRequest request;

	request.type = IpcRequestType::SetScreenSize;
	request.arg1 = (int32_t) screen_size_items_[index].first;
	request.arg2 = (int32_t) screen_size_items_[index].second;
	it->second.panel->SendRequest(request);

	/* The tick is not set here. The machine reports back what it actually
	   settled on, which is not necessarily this - RISC OS refuses sizes its
	   monitor definition does not declare - and showing the asked-for one would
	   claim a change that did not happen. */
}

void ManagerFrame::RefreshUiState()
{
	UpdateStatusText();
	UpdateButtons();
	UpdateMachineMenuState();
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

	/* The same commands as the buttons above, so the same rules: a menu that
	   offers what the button beside it refuses is two answers to one question. */
	if (wxMenuBar *bar = GetMenuBar()) {
		bar->Enable(ID_EDIT, have_selection && !is_running);
		bar->Enable(ID_CLONE, have_selection);
		bar->Enable(ID_DELETE, have_selection && !is_running);
	}

	/* Resume only offers itself when there is something to resume from: a
	   machine that has never been suspended has no snapshot, and a command that
	   silently did nothing would be worse than one that is greyed out. */
	const bool can_resume = have_selection && !is_running && HasSnapshot(name);

	if (tool_bar_ != nullptr) {
		tool_bar_->EnableTool(ID_START, have_selection && !is_running);
		tool_bar_->EnableTool(ID_RESUME, can_resume);
		tool_bar_->EnableTool(ID_STOP, is_running);
		tool_bar_->EnableTool(ID_RESET, is_live);
		tool_bar_->Refresh();	/* see UpdateMachineMenuState */
	}
	if (start_item_ != nullptr) start_item_->Enable(have_selection && !is_running);
	if (resume_item_ != nullptr) {
		resume_item_->Enable(can_resume);
	}
	if (discard_state_item_ != nullptr) {
		discard_state_item_->Enable(can_resume);
	}
	if (stop_item_ != nullptr) stop_item_->Enable(is_running);
	if (reset_item_ != nullptr) reset_item_->Enable(is_live);
	if (restart_item_ != nullptr) restart_item_->Enable(is_live);
	if (shortcut_item_ != nullptr) shortcut_item_->Enable(have_selection);
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
		ApplyStateReport(name, report);
	});

	/* Capture starts and ends with a click or a keystroke inside the panel, which
	   this window never sees, so it has to be told - the status bar is what says
	   how to escape. */
	panel->SetCaptureChangedCallback([this, name]() {
		if (name == active_machine_) {
			UpdateStatusText();
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

	/*
	 * Tell the machine which window its own dialogues should sit above. Without
	 * this they are parented to a frame that is never shown, in another process,
	 * so the window manager opens them behind this one and nothing says they
	 * opened - see window_owner.h. Zero on the platforms with no cross-process
	 * equivalent, which the machine treats as "raise instead".
	 */
	{
		IpcRequest request;
		const uint64_t id = window_native_id(this);

		request.type = IpcRequestType::SetOwnerWindow;
		request.arg1 = (int32_t) (uint32_t) (id & 0xffffffffu);
		request.arg2 = (int32_t) (uint32_t) (id >> 32);
		panel->SendRequest(request);
	}
	it->second.book_page = display_book_->GetPageCount();
	display_book_->AddPage(panel, name, false);

	RefreshMachineList();

	/* Only if it is still the machine the user is pointing at. A machine takes
	   a second or two to come up, and showing it because it was started puts it
	   on screen over whatever was selected while it did. */
	if (name == SelectedMachineName()) {
		ShowMachinePanel(name);
	} else {
		/* Ask now rather than when it is first shown: a machine only reports
		   when asked, so without this its settings are unknown until somebody
		   looks at it, and the toolbar has nothing to be set from. */
		IpcRequest request;
		request.type = IpcRequestType::RequestState;
		panel->SendRequest(request);
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
	 * The child is a fresh process and works its data directory out from
	 * scratch, so without this a Manager started with an explicit --datadir
	 * would list the machines in one folder and launch them against another.
	 *
	 * Passed unconditionally, which it could not be before: --datadir used to
	 * make the resource directory the same folder as well, so handing it to the
	 * child sent it looking for the podule ROMs, default/ and the CMOS template
	 * in the data directory, where in an ordinary install they are not. The test
	 * here was therefore "only when the two are already the same folder", which
	 * meant the data directory silently was NOT passed on in exactly the
	 * arrangement macOS always has - payload in the bundle, data outside it.
	 *
	 * --datadir now names only the data directory and the payload is looked for
	 * on its own merits (DATA_DIR_FROM_CLI in data_paths.cpp), so there is
	 * nothing left to be careful about, and both processes agree on both
	 * directories.
	 */
	const wxString datadir = wxString::FromUTF8(rpcemu_get_datadir());

	if (!datadir.empty()) {
		cmd << " --datadir \"" << datadir << '"';
	}

	/*
	 * The payload is deliberately NOT passed on. The child is the same binary in
	 * the same place, so it derives the same answer, and an explicit
	 * RPCEMU_RESOURCE_DIR reaches it through the inherited environment.
	 */

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
	RefreshUiState();
}

void ManagerFrame::OnPollTimer(wxTimerEvent & /*event*/)
{
	/* Before the early return: with nothing running there is no speed to show
	   and the field has to be cleared of the last machine's. */
	UpdateSpeedStatus();

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

		/* What this machine last said, now, rather than leaving the previous
		   machine's answer up until the reply below arrives. */
		SetMuteToolState(it->second.muted);
		SetDebugToolState(it->second.debug_paused,
		    it->second.debug_pause_requested);
		SetCdromMenuState(it->second.cdrom_source);
		SetNatMenuState(it->second.network_is_nat);

		/* The menus now belong to a different machine, so ask it what its
		   tick-boxes say rather than leaving the previous machine's answers
		   on display. */
		IpcRequest request;
		request.type = IpcRequestType::RequestState;
		it->second.panel->SendRequest(request);
	} else {
		active_machine_.clear();
		display_book_->SetSelection((size_t) placeholder_page_);
		SetMuteToolState(false);
	}
	RefreshUiState();
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

	RefreshUiState();
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
	RefreshUiState();

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
	if (rebuilding_machine_list_) {
		return;
	}

	/* Always, including for a machine that is not running: that is what clears
	   the active one, and it ends with the refresh either way. */
	ShowMachinePanel(SelectedMachineName());
}

void ManagerFrame::OnMachineActivated(wxListEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty()) {
		return;
	}
	StartMachine(name);
}

/* The machine that was clicked: start it, stop it, edit it, or make a shortcut. */
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
	const bool can_resume = !is_running && HasSnapshot(name);
	wxMenu menu;

	menu.Append(ID_START, "Start");
	menu.Append(ID_RESUME, "Resume");
	menu.Append(ID_STOP, "Stop");
	menu.Enable(ID_START, !is_running);
	menu.Enable(ID_RESUME, can_resume);
	menu.Enable(ID_STOP, is_running);
	menu.AppendSeparator();
	menu.Append(ID_DISCARD_STATE, "Discard Suspended State");
	menu.Enable(ID_DISCARD_STATE, can_resume);
	menu.AppendSeparator();
	menu.Append(ID_EDIT, "Edit...");
	/* Greyed while the machine runs, to match the Edit button and the Machine
	   menu: a menu that offers what the button beside it refuses is two
	   answers to one question. OnEdit turns a running machine away in any
	   case, but silently, which reads as the item having done nothing. */
	menu.Enable(ID_EDIT, !is_running);
	menu.Append(ID_SHORTCUT, "Create Shortcut...");

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
	ConfigPathsPrepareClonedConfig(dest_path, sanitized, MachineDirFor(name),
	    MachineDirFor(sanitized));

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
 * Throw the suspended state away, so the machine boots fresh next time.
 *
 * Asked about rather than done: this is the only way to lose a suspended
 * session outright, everything else having left it where it was. The file goes
 * to .bak, which is where resuming puts it too, so the same one undo exists
 * either way.
 */
void ManagerFrame::OnDiscardState(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	const wxString snapshot = SnapshotPathFor(name);

	if (snapshot.empty() || !wxFileExists(snapshot)) {
		return;
	}

	wxRichMessageDialog dlg(this,
	    wxString::Format("Discard the suspended state of %s?", name),
	    "RPCEmu Extended Manager", wxYES_NO | wxNO_DEFAULT | wxICON_EXCLAMATION);

	dlg.SetYesNoLabels("Discard", "Cancel");
	dlg.SetExtendedMessage(
	    "The machine will start fresh instead of carrying on from where\n"
	    "it was suspended. Whatever RISC OS had open at the time is lost.");

	if (dlg.ShowModal() != wxID_YES) {
		return;
	}

	const wxString bak = snapshot + ".bak";

	if (wxFileExists(bak)) {
		wxRemoveFile(bak);
	}
	if (!wxRenameFile(snapshot, bak)) {
		wxMessageBox("Could not discard the suspended state.",
		    "RPCEmu Extended Manager", wxOK | wxICON_ERROR, this);
		return;
	}

	RefreshMachineList();
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
/*
 * RPCEmu's own settings.
 *
 * The data folder button runs OnDataFolder's flow rather than a copy of it -
 * that flow refuses while machines are running, names them, and offers to move
 * the files, and none of that is worth having twice.
 *
 * Hardware acceleration is applied to the machines already on screen rather than
 * at the next start, because a setting that appears to do nothing until you
 * restart reads as a setting that does not work.
 */
void ManagerFrame::OnSettings(wxCommandEvent & /*event*/)
{
	ManagerSettingsDialog dialog(this, [this]() {
		wxCommandEvent unused;

		OnDataFolder(unused);
		return wxString::FromUTF8(rpcemu_get_datadir());
	});

	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	if (dialog.HardwareAccelerationChanged()) {
		for (auto &entry : running_) {
			if (entry.second.panel != nullptr) {
				entry.second.panel->SetHardwareAcceleration(
				    dialog.HardwareAccelerationChosen());
			}
		}
	}
}

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

/* Nothing to lose: a machine set to suspend on exit saves its state, and one
   that is not running has none. */
bool ManagerFrame::WillAskBeforeStopping(const wxString &name) const
{
	auto running = running_.find(name);

	return running != running_.end() && GetWarnOnStop() &&
	    !running->second.suspend_on_exit;
}

wxString ManagerFrame::SnapshotPathFor(const wxString &name) const
{
	if (name.empty()) {
		return wxEmptyString;
	}

	return ConfigPathsSnapshotForConfig(
	    ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() +
	    name + ".cfg");
}

bool ManagerFrame::HasSnapshot(const wxString &name) const
{
	const wxString snapshot = SnapshotPathFor(name);

	return !snapshot.empty() && wxFileExists(snapshot);
}

/*
 * Ask before stopping a machine.
 *
 * Stop is a button on the toolbar, an item on the Machine menu and an item on a
 * machine's context menu, and until now all three shut the machine down on the
 * first click with nothing in between. Next to Start on the same toolbar, that is
 * an easy thing to hit by accident, and what it costs is whatever RISC OS had not
 * written to disc.
 *
 * Deliberately shaped like the warning for closing this window with machines
 * running: the same kind of dialogue, saying what will happen and what the other
 * choice is. Cancel is the default button (wxNO_DEFAULT), so a stray Return or
 * Space after the click does not confirm it - which is the same misfire the
 * dialogue exists to catch.
 *
 * Not asked when the machine is not running: there is nothing to lose, and a
 * question with no consequence teaches people to dismiss questions.
 */
bool ManagerFrame::ConfirmStop(const wxString &name)
{
	if (!WillAskBeforeStopping(name)) {
		return true;
	}

	wxRichMessageDialog dlg(this,
	    wxString::Format("Stop %s?", name),
	    "RPCEmu Extended Manager", wxYES_NO | wxNO_DEFAULT | wxICON_EXCLAMATION);

	dlg.SetYesNoLabels("Stop", "Cancel");
	dlg.SetExtendedMessage(
	    "The machine is asked to shut down, and anything RISC OS has\n"
	    "not written to disc is lost - unless this machine is set to\n"
	    "suspend on exit, in which case its state is saved.\n\n"
	    "A machine left running can be shown again from this window\n"
	    "at any time, including after this window is closed.");

	return dlg.ShowModal() == wxID_YES;
}

void ManagerFrame::OnStop(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}

	/* Queued only when something will be asked: a dialogue opened from inside
	   the click leaves the button drawn pressed, and with the warning turned
	   off there is nothing to open. */
	if (WillAskBeforeStopping(name)) {
		CallAfter([this, name] {
			if (running_.find(name) != running_.end() && ConfirmStop(name)) {
				StopMachine(name);
			}
		});
		return;
	}
	StopMachine(name);
}

void ManagerFrame::OnReset(wxCommandEvent & /*event*/)
{
	/* The selected machine, which is what UpdateButtons greys this from and
	   what Start and Stop beside it act on. */
	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}
	auto it = running_.find(name);
	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}
	IpcRequest request;
	request.type = IpcRequestType::Reset;
	it->second.panel->SendRequest(request);
}


void ManagerFrame::OnRestart(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();	/* see OnReset */

	if (name.empty()) {
		return;
	}
	auto it = running_.find(name);
	if (it == running_.end() || it->second.panel == nullptr) {
		return;
	}
	IpcRequest request;
	request.type = IpcRequestType::Restart;
	it->second.panel->SendRequest(request);
}

void ManagerFrame::OnCreateShortcut(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}

	const wxString exe = wxStandardPaths::Get().GetExecutablePath();
	wxString args = wxString::Format("--machine \"%s\"", name);

	/*
	 * --datadir on the same terms StartMachine uses it: unconditionally, now
	 * that it names the data directory alone and no longer decides where the
	 * read-only payload is looked for. A shortcut is the case that needs it
	 * most - it is started from wherever the desktop happens to be, with none of
	 * this process's context.
	 */
	const wxString datadir = wxString::FromUTF8(rpcemu_get_datadir());

	if (!datadir.empty()) {
		args << " --datadir \"" << datadir << '"';
	}

#ifdef _WIN32
	const wxString extension = ".lnk";
	const wxString wildcard = "Shortcuts (*.lnk)|*.lnk";
#elif defined(__WXOSX__)
	const wxString extension = ".command";
	const wxString wildcard = "Shell commands (*.command)|*.command";
#else
	const wxString extension = ".desktop";
	const wxString wildcard = "Desktop entries (*.desktop)|*.desktop";
#endif

	/* The desktop, which is where a shortcut is usually wanted. Not forced:
	   the applications menu is a folder away, and some people keep neither. */
	wxFileName desktop(wxStandardPaths::Get().GetDocumentsDir(), wxEmptyString);

	desktop.AppendDir("Desktop");

	wxFileDialog dialog(this, "Create Shortcut",
	    desktop.DirExists() ? desktop.GetPath() : wxString(),
	    ConfigPathsSanitizeName(name) + extension,
	    wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	if (!WriteShortcut(dialog.GetPath(), exe, args,
	        wxFileName(exe).GetPath(), name)) {
		wxMessageBox("The shortcut could not be written.",
		    "RPCEmu Extended Manager", wxOK | wxICON_ERROR, this);
		return;
	}

	rpclog("Manager: shortcut for '%s' written to %s\n",
	    name.utf8_str().data(), dialog.GetPath().utf8_str().data());
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
		    "RPCEmu Extended Manager", wxOK | wxICON_ERROR, this);
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
 *
 * Queued so the toolbar has finished with the mouse first: a modal opened from
 * inside a tool's click leaves the button drawn pressed.
 */
void ManagerFrame::ForwardWithFileDialog(int id, const wxString &title,
    const wxString &wildcard, bool save)
{
	CallAfter([this, id, title, wildcard, save] {
		wxFileDialog dialog(this, title, "", "", wildcard,
		    save ? (wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
		         : (wxFD_OPEN | wxFD_FILE_MUST_EXIST));

		if (dialog.ShowModal() != wxID_OK) {
			return;
		}
		SendMenuCommand(id, false, dialog.GetPath());
	});
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
	case ID_MENU_CHECK_UPDATE:
		CheckForUpdate(this);
		return;
	case ID_MENU_SUPPORT_BUNDLE:
		CreateSupportBundle();
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
		             "being shown.", "RPCEmu Extended Manager", wxOK | wxICON_INFORMATION, this);
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
		if (running_.find(active_machine_) == running_.end()) {
			return;
		}

		/* Queued for the same reason as ForwardWithFileDialog. */
		CallAfter([this] {
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
				wxMessageBox("Could not save the screenshot.",
				    "RPCEmu Extended Manager",
				    wxOK | wxICON_WARNING, this);
			}
		});
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
	bool checked = event.IsChecked();

	/*
	 * Only one of the two moved: a menu tick and a toolbar tool are separate
	 * copies of one command, and wx moves whichever was clicked. Left alone,
	 * the next click read the stale one and sent the state that had just been
	 * asked for, so mute inverted itself every other press.
	 */
	if (wxMenuBar *bar = GetMenuBar()) {
		if (wxMenuItem *item = bar->FindItem(id)) {
			if (item->IsCheckable()) {
				/* From a tool the tick is what is being flipped, the tool
				   having no state of its own; from the menu wx has already
				   moved the tick and the event agrees with it. */
				checked = (event.GetEventType() == wxEVT_TOOL)
				    ? !item->IsChecked()
				    : item->IsChecked();
				item->Check(checked);
			}
		}
	}
	/*
	 * The tool itself is deliberately not set here. The machine's state report
	 * is what moves it, and this window asking for one thing while a report
	 * said another left the two racing: whichever landed last won, so the
	 * button could end up showing the opposite of the sound.
	 */
	SendMenuCommand(id, checked);
}

/*
 * Enable the machine menus only while a machine is being shown.
 *
 * The Help items that this window answers itself stay enabled: they work with
 * no machine running, and grey items that would have worked read as a fault.
 */
void ManagerFrame::UpdateMachineMenuState()
{
	/*
	 * The active machine, which ShowMachinePanel sets from the selection and
	 * clears when the selected one is not running. Deliberately not the list's
	 * own selection: RefreshMachineList() empties and refills the rows, and
	 * during that this would read a selection that is briefly not there.
	 */
	const bool have_machine = !active_machine_.empty() &&
	    running_.find(active_machine_) != running_.end();

	wxMenuBar *bar = GetMenuBar();

	if (bar == nullptr) {
		return;
	}

	/* The list belongs to the machine on show, so it follows the switch rather
	   than waiting for that machine's next report. */
	RebuildScreenSizeMenu();

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

	for (wxMenu *menu : { machine_disc_menu_,
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

	/* Forwarded to the machine, but sharing the Machine menu with this
	   window's own commands, which have their own rules - so they are named
	   rather than reached by walking a menu. */
	static const int forwarded_machine_items[] = {
		ID_MENU_SUSPEND, ID_MENU_SUSPEND_ON_EXIT,
		ID_MENU_SAVE_STATE, ID_MENU_LOAD_STATE, ID_MENU_SCREENSHOT,
	};

	for (int id : forwarded_machine_items) {
		bar->Enable(id, have_machine);
	}

	for (int id : always_enabled) {
		bar->Enable(id, true);
	}

	/* The forwarded tools go the same way as their menu items - they are the
	   same commands, so a live tool beside a greyed menu entry would be two
	   answers to the same question. */
	if (tool_bar_ != nullptr) {
		static const int forwarded_tools[] = {
			ID_MENU_SCREENSHOT, ID_MENU_SUSPEND, ID_MENU_LOAD_DISC0,
			ID_MENU_CDROM_ISO,
			ID_MENU_MUTE, ID_MENU_FULLSCREEN,
			ID_MENU_MACHINE, ID_MENU_DEBUG_RUN, ID_MENU_DEBUG_PAUSE,
			ID_MENU_DEBUG_STEP, ID_MENU_MACHINE_INSPECTOR,
			ID_MENU_NETWORK_ANALYSER,
		};

		for (int id : forwarded_tools) {
			tool_bar_->EnableTool(id, have_machine);
		}

		/* Greying a tool does not always redraw it. */
		tool_bar_->Refresh();
	}

	/*
	 * Last, because the walks above have just enabled every forwarded item from
	 * "is there a machine" alone, and the debugger's have a second rule on top
	 * of that: Step on a machine that is running free does not step it, it
	 * quietly stops it.
	 */
	if (have_machine) {
		auto it = running_.find(active_machine_);

		if (it != running_.end()) {
			SetDebugToolState(it->second.debug_paused,
			    it->second.debug_pause_requested);
			SetNatMenuState(it->second.network_is_nat);
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
void ManagerFrame::ApplyStateReport(const wxString &machine, const wxString &report)
{
	/* The menus show the active machine only, but every machine's own settings
	   are recorded as they arrive - full screen asks the machine it is about
	   to show, which is not necessarily the one that just reported. */
	const bool active = (machine == active_machine_);
	wxMenuBar *bar = GetMenuBar();

	wxStringTokenizer pairs(report, " ");

	while (pairs.HasMoreTokens()) {
		const wxString pair = pairs.GetNextToken();
		const int equals = pair.Find('=');

		if (equals == wxNOT_FOUND) {
			continue;
		}

		long id = 0;
		long value = 0;

		if (!pair.Left(equals).ToLong(&id)) {
			continue;
		}

		/* The two whose values are sizes rather than integers, taken before the
		   ToLong below that every other pair needs. */
		if (id == kStateScreenModes || id == kStateScreenSize) {
			auto it = running_.find(machine);

			if (it != running_.end()) {
				const wxString text = pair.Mid(equals + 1);

				if (id == kStateScreenModes) {
					ParseScreenModes(text, it->second.screen_modes);
				} else {
					ParseScreenSize(text, it->second.screen_size_x,
					    it->second.screen_size_y);
				}
				if (machine == active_machine_) {
					RebuildScreenSizeMenu();
				}
			}
			continue;
		}

		if (!pair.Mid(equals + 1).ToLong(&value)) {
			continue;
		}

		/* Config rather than a menu item, and wanted whichever machine sent
		   it: EnterFullScreen asks the machine it is about to show. */
		if (id == kStateFullscreenMessage) {
			auto it = running_.find(machine);

			if (it != running_.end()) {
				it->second.show_fullscreen_message = (value != 0);
			}
			continue;
		}

		/* Recorded for every machine, not only the one being shown: ConfirmStop
		   asks about whichever machine is being stopped. */
		if (id == ID_MENU_SUSPEND_ON_EXIT) {
			auto it = running_.find(machine);

			if (it != running_.end()) {
				it->second.suspend_on_exit = (value != 0);
			}
		}

		/* Likewise: the toolbar is set from these when a machine is shown. */
		if (id == ID_MENU_MUTE || id == kStateDebugPaused ||
		    id == kStateDebugPauseRequested || id == kStateCdromSource ||
		    id == kStateNetworkIsNat) {
			auto it = running_.find(machine);

			if (it != running_.end()) {
				switch (id) {
				case ID_MENU_MUTE:
					it->second.muted = (value != 0);
					break;
				case kStateDebugPaused:
					it->second.debug_paused = (value != 0);
					break;
				case kStateDebugPauseRequested:
					it->second.debug_pause_requested = (value != 0);
					break;
				case kStateCdromSource:
					it->second.cdrom_source = (int) value;
					break;
				case kStateNetworkIsNat:
					it->second.network_is_nat = (value != 0);
					break;
				}
			}
		}

		if (!active) {
			continue;
		}

		wxMenuItem *item = bar != nullptr ? bar->FindItem((int) id) : nullptr;

		if (item != nullptr && item->IsCheckable()) {
			item->Check(value != 0);
		}

		/*
		 * The mouse mode is not only a tick-box here: the panel has to know it
		 * to decide whether the machine is sent the pointer's position or its
		 * movement, and getting that wrong is not a cosmetic fault - a machine
		 * expecting movements asserts on a position. Which is why it is taken
		 * from what the machine says rather than from what this menu shows.
		 */
		if (id == ID_MENU_MOUSE_HACK) {
			auto it = running_.find(active_machine_);

			if (it != running_.end() && it->second.panel != nullptr) {
				it->second.panel->SetFollowHostMouse(value != 0);
			}
			UpdateStatusText();
		}

		/* Mute also has a toolbar tool, which has to agree with its menu
		   item or the toolbar shows the opposite of the truth. */
		if (id == ID_MENU_MUTE) {
			SetMuteToolState(value != 0);
		}
		if (id == kStateCdromSource) {
			SetCdromMenuState((int) value);
		}
		if (id == kStateNetworkIsNat) {
			SetNatMenuState(value != 0);
		}

		/* Both flags travel in the same report, so apply once either has
		   arrived, from what this machine has said so far. */
		if (id == kStateDebugPaused || id == kStateDebugPauseRequested) {
			auto it = running_.find(machine);

			if (it != running_.end()) {
				SetDebugToolState(it->second.debug_paused,
				    it->second.debug_pause_requested);
			}
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

void ManagerFrame::CreateSupportBundle()
{
	wxArrayString choices;

	for (const wxString &name : machine_names_) {
		choices.Add(name);
	}

	if (choices.IsEmpty()) {
		wxMessageBox("There are no machines to collect anything about.",
		    "RPCEmu Extended - Support Files", wxOK | wxICON_INFORMATION, this);
		return;
	}

	wxString machine;

	if (choices.GetCount() == 1) {
		machine = choices[0];
	} else {
		/* Defaulted to the one being shown, which is what somebody looking at
		   a misbehaving machine is most likely to mean. */
		int selected = choices.Index(active_machine_);

		if (selected == wxNOT_FOUND) {
			selected = 0;
		}

		wxSingleChoiceDialog dlg(this, "Which machine is the report about?",
		    "RPCEmu Extended - Support Files", choices);

		dlg.SetSelection(selected);
		if (dlg.ShowModal() != wxID_OK) {
			return;
		}
		machine = dlg.GetStringSelection();
	}

	/* Only a running machine has a screen, and only this window has its
	   pixels - a managed machine's own panel never receives frames. */
	wxString screenshot;
	auto it = running_.find(machine);

	if (it != running_.end() && it->second.panel != nullptr) {
		const wxString temp = wxFileName::CreateTempFileName("rpcemu-screen");

		if (!temp.empty() && it->second.panel->SaveScreenshot(temp)) {
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
	    wxStandardPaths::Get().GetDocumentsDir(),
	    SupportBundleSuggestedName(machine),
	    "Zip archives (*.zip)|*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dlg.ShowModal() != wxID_OK) {
		if (!screenshot.empty()) {
			wxRemoveFile(screenshot);
		}
		return;
	}

	const wxString dir = MachineDirFor(machine);
	const SupportBundleResult result = SupportBundleWrite(dlg.GetPath(), machine,
	    dir, wxFileName(dir, "rpclog.txt").GetFullPath(), screenshot,
	    wxFileName(wxString::FromUTF8(rpcemu_get_datadir()),
	        "rpclog.txt").GetFullPath());

	if (!screenshot.empty()) {
		wxRemoveFile(screenshot);
	}

	if (!result.ok) {
		wxMessageBox(result.message, "RPCEmu Extended - Support Files",
		    wxOK | wxICON_ERROR, this);
		return;
	}

	wxString detail;

	for (const SupportBundleMember &member : result.members) {
		detail += wxString::Format("    %s\n", member.name);
	}

	wxMessageBox(
	    wxString::Format("Saved to:\n%s\n\nIt contains:\n%s\n"
	                     "The VNC password and any home folder paths have been "
	                     "taken out of the log and the settings.",
	        dlg.GetPath(), detail),
	    "RPCEmu Extended - Support Files", wxOK | wxICON_INFORMATION, this);
}

void ManagerFrame::OnExit(wxCommandEvent & /*event*/)
{
	/* Not Close(true): OnClose asks about running machines and has to be able
	   to change its mind, which a forced close does not allow. */
	Close();
}

void ManagerFrame::OnClose(wxCloseEvent &event)
{
	/*
	 * A machine outliving this window is the intended behaviour, but not an
	 * obvious one: nothing on screen afterwards says the machine is still
	 * there. Say so, and offer the other choice.
	 */
	if (!running_.empty() && !closing_after_stop_ && GetWarnOnExit()) {
		const size_t count = running_.size();
		wxRichMessageDialog dlg(this,
		    wxString::Format("%zu machine%s still running.", count,
		        count == 1 ? " is" : "s are"),
		    "RPCEmu Extended Manager", wxOK | wxCANCEL | wxICON_WARNING);

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

			/* Only stay open if something is still stopping: a machine can
			   go while StopAllAndClose() is still running, which queues the
			   close itself, and vetoing that one is not allowed. */
			if (!running_.empty() && event.CanVeto()) {
				event.Veto();
				return;
			}
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
