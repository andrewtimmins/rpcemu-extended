#include "main_frame.h"

#include <wx/artprov.h>
#include <wx/filename.h>

#include "gui_preferences.h"
#include "toolbar_icons.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

const wchar_t kLedOn = L'\u25cf';
const wchar_t kLedOff = L'\u25cb';

wxString LedText(bool active)
{
	return active ? wxString(kLedOn) : wxString(kLedOff);
}

void BindMenuItem(wxMenu *menu, int id, MainFrame *frame, void (MainFrame::*handler)(wxCommandEvent &))
{
	menu->Bind(wxEVT_MENU, handler, frame, id);
}

/*
 * Rebuild a "recent items" submenu from scratch: one enabled entry per recent
 * item (using fixed per-slot IDs so the existing id-based bindings still fire),
 * or a single disabled placeholder when the list is empty, followed by a
 * separator and the Clear command. Rebuilding avoids leaving empty-label menu
 * items around (which wxWidgets asserts on) and never shows stale entries.
 */
void PopulateRecentMenu(wxMenu *menu, const std::vector<std::string> &recent,
                        int max_items, int base_id, int clear_id,
                        const wxString &clear_label, const wxString &empty_label,
                        bool use_basename)
{
	while (menu->GetMenuItemCount() > 0) {
		menu->Delete(menu->FindItemByPosition(0));
	}

	int num_recent = static_cast<int>(recent.size());
	if (num_recent > max_items) {
		num_recent = max_items;
	}

	if (num_recent == 0) {
		wxMenuItem *placeholder = menu->Append(wxID_ANY, empty_label);
		placeholder->Enable(false);
	} else {
		for (int i = 0; i < num_recent; ++i) {
			const wxString entry = wxString::FromUTF8(recent[static_cast<size_t>(i)]);
			const wxString text = use_basename ? wxFileName(entry).GetFullName() : entry;
			const wxString accel = (i < 9) ? wxString::Format("&%d", i + 1) : wxString("&0");
			menu->Append(base_id + i, wxString::Format("%s %s", accel, text));
		}
	}

	menu->AppendSeparator();
	menu->Append(clear_id, clear_label);
}

} // namespace

void MainFrame::BindMenuOpenClose(wxMenu *menu)
{
	menu->Bind(wxEVT_MENU_OPEN, &MainFrame::OnMenuOpen, this);
	menu->Bind(wxEVT_MENU_CLOSE, &MainFrame::OnMenuClose, this);

	for (size_t i = 0; i < menu->GetMenuItemCount(); ++i) {
		if (wxMenuItem *item = menu->FindItemByPosition(i)) {
			if (wxMenu *submenu = item->GetSubMenu()) {
				BindMenuOpenClose(submenu);
			}
		}
	}
}

void MainFrame::BindAllMenuOpenCloseHandlers()
{
	if (wxMenuBar *bar = GetMenuBar()) {
		for (size_t i = 0; i < bar->GetMenuCount(); ++i) {
			BindMenuOpenClose(bar->GetMenu(i));
		}
	}
}

void MainFrame::BuildMenus()
{
	auto *file_menu = new wxMenu;
	file_menu->Append(ID_MENU_SCREENSHOT, "Take Screenshot...");
	file_menu->Append(ID_MENU_SAVE_STATE, "Save State...");
	file_menu->Append(ID_MENU_LOAD_STATE, "Load State...");
	file_menu->AppendSeparator();

	auto *recent_machines_menu = new wxMenu;
	recent_machines_menu_ = recent_machines_menu;
	/* Contents are filled in by UpdateRecentMachinesMenu(); see below. */
	file_menu->AppendSubMenu(recent_machines_menu, "Recent Machines");

	file_menu->AppendSeparator();
	file_menu->Append(ID_MENU_RESET, "&Reset");
	file_menu->AppendSeparator();
	file_menu->Append(ID_MENU_SUSPEND, "Suspend");
	file_menu->Append(wxID_EXIT, "E&xit");

	auto *disc_menu = new wxMenu;
	auto *floppy_menu = new wxMenu;
	floppy_menu->Append(ID_MENU_LOAD_DISC0, "Load Drive :0...");
	floppy_menu->Append(ID_MENU_LOAD_DISC1, "Load Drive :1...");
	floppy_menu->AppendSeparator();
	floppy_menu->Append(ID_MENU_EJECT_DISC0, "Eject Drive :0");
	floppy_menu->Append(ID_MENU_EJECT_DISC1, "Eject Drive :1");
	floppy_menu->AppendSeparator();
	floppy_menu->Append(ID_MENU_CREATE_DISC0, "Create Blank Drive :0...");
	floppy_menu->Append(ID_MENU_CREATE_DISC1, "Create Blank Drive :1...");
	floppy_menu->AppendSeparator();

	auto *recent_floppies_menu = new wxMenu;
	recent_floppies_menu_ = recent_floppies_menu;
	/* Contents are filled in by UpdateRecentFloppiesMenu(); see below. */
	floppy_menu->AppendSubMenu(recent_floppies_menu, "Recent Images");
	disc_menu->AppendSubMenu(floppy_menu, "Floppy");

	auto *cdrom_menu = new wxMenu;
	cdrom_disabled_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_DISABLED, "Disabled");
	cdrom_empty_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_EMPTY, "Empty");
	cdrom_iso_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_ISO, "ISO Image...");
	cdrom_ioctl_item_ = cdrom_menu->AppendRadioItem(ID_MENU_CDROM_IOCTL, "Host CD/DVD Drive");
	cdrom_menu->AppendSeparator();

	auto *recent_cdroms_menu = new wxMenu;
	recent_cdroms_menu_ = recent_cdroms_menu;
	/* Contents are filled in by UpdateRecentCdromsMenu(); see below. */
	cdrom_menu->AppendSubMenu(recent_cdroms_menu, "Recent Images");
	disc_menu->AppendSubMenu(cdrom_menu, "CD-ROM");

	auto *settings_menu = new wxMenu;
	settings_menu->Append(ID_MENU_MACHINE, "&Machine...");
#ifdef RPCEMU_NETWORKING
	nat_list_item_ = settings_menu->Append(ID_MENU_NAT_LIST, "NAT Port Forwarding Rules...");
#endif
	settings_menu->AppendSeparator();
	mute_menu_item_ = settings_menu->AppendCheckItem(ID_MENU_MUTE, "Mute Sound");
	settings_menu->AppendSeparator();
	fullscreen_menu_item_ = settings_menu->AppendCheckItem(ID_MENU_FULLSCREEN, "Full-screen Mode");
	integer_scaling_menu_item_ =
	    settings_menu->AppendCheckItem(ID_MENU_INTEGER_SCALING, "Pixel Perfect");
	settings_menu->AppendSeparator();
#ifdef RPCEMU_VNC
	settings_menu->Append(ID_MENU_VNC, "&VNC Server...");
#endif
	settings_menu->AppendSeparator();
	settings_menu->Append(ID_MENU_SERIAL, "Serial...");
	settings_menu->Append(ID_MENU_PARALLEL, "Parallel...");
	settings_menu->AppendSeparator();
	cpu_idle_menu_item_ = settings_menu->AppendCheckItem(ID_MENU_CPU_IDLE, "Reduce CPU Usage");
	settings_menu->AppendSeparator();

	auto *mouse_menu = new wxMenu;
	mouse_hack_menu_item_ =
	    mouse_menu->AppendCheckItem(ID_MENU_MOUSE_HACK, "Follow Host Mouse");
	mouse_menu->AppendSeparator();
	mouse_twobutton_menu_item_ =
	    mouse_menu->AppendCheckItem(ID_MENU_MOUSE_TWOBUTTON, "Two-button Mouse Mode");
	settings_menu->AppendSubMenu(mouse_menu, "Mouse");

	auto *debug_menu = new wxMenu;
	debug_run_item_ = debug_menu->Append(ID_MENU_DEBUG_RUN, "Run");
	debug_pause_item_ = debug_menu->Append(ID_MENU_DEBUG_PAUSE, "Pause");
	debug_menu->AppendSeparator();
	debug_step_item_ = debug_menu->Append(ID_MENU_DEBUG_STEP, "Step");
	/* Decode the "×" explicitly from UTF-8 so the label is correct regardless of
	   the C locale; a narrow non-ASCII literal converts to an empty string under
	   a non-UTF-8 locale, which wxWidgets then asserts on. */
	debug_step5_item_ = debug_menu->Append(ID_MENU_DEBUG_STEP5, wxString::FromUTF8("Step \xC3\x97" "5"));
	debug_menu->AppendSeparator();
	debug_menu->Append(ID_MENU_MACHINE_INSPECTOR, "Machine Inspector...");

	auto *help_menu = new wxMenu;
	help_menu->Append(ID_MENU_ONLINE_MANUAL, "Online Manual...");
	help_menu->Append(ID_MENU_VISIT_WEBSITE, "Visit Website...");
	help_menu->AppendSeparator();
	help_menu->Append(ID_MENU_ABOUT, "About RPCEmu...");

	auto *menu_bar = new wxMenuBar;
	menu_bar->Append(file_menu, "&File");
	menu_bar->Append(disc_menu, "&Disc");
	menu_bar->Append(settings_menu, "&Settings");
	menu_bar->Append(debug_menu, "&Debug");
	menu_bar->Append(help_menu, "&Help");
	SetMenuBar(menu_bar);

	BindMenuItem(file_menu, ID_MENU_SCREENSHOT, this, &MainFrame::OnScreenshot);
	BindMenuItem(file_menu, ID_MENU_SAVE_STATE, this, &MainFrame::OnSaveState);
	BindMenuItem(file_menu, ID_MENU_LOAD_STATE, this, &MainFrame::OnLoadState);
	BindMenuItem(file_menu, ID_MENU_RESET, this, &MainFrame::OnReset);
	BindMenuItem(file_menu, ID_MENU_SUSPEND, this, &MainFrame::OnSuspend);
	BindMenuItem(file_menu, wxID_EXIT, this, &MainFrame::OnExit);
	BindMenuItem(recent_machines_menu, ID_MENU_CLEAR_RECENT_MACHINES, this, &MainFrame::OnClearRecentMachines);
	for (int i = 0; i < MaxRecentMachines; ++i) {
		BindMenuItem(recent_machines_menu, ID_MENU_RECENT_MACHINE_0 + i, this, &MainFrame::OnRecentMachine);
	}

	BindMenuItem(floppy_menu, ID_MENU_LOAD_DISC0, this, &MainFrame::OnLoadDisc0);
	BindMenuItem(floppy_menu, ID_MENU_LOAD_DISC1, this, &MainFrame::OnLoadDisc1);
	BindMenuItem(floppy_menu, ID_MENU_EJECT_DISC0, this, &MainFrame::OnEjectDisc0);
	BindMenuItem(floppy_menu, ID_MENU_EJECT_DISC1, this, &MainFrame::OnEjectDisc1);
	BindMenuItem(floppy_menu, ID_MENU_CREATE_DISC0, this, &MainFrame::OnCreateDisc0);
	BindMenuItem(floppy_menu, ID_MENU_CREATE_DISC1, this, &MainFrame::OnCreateDisc1);
	BindMenuItem(recent_floppies_menu, ID_MENU_CLEAR_RECENT_FLOPPIES, this, &MainFrame::OnClearRecentFloppies);
	for (int i = 0; i < MaxRecentFloppies; ++i) {
		BindMenuItem(recent_floppies_menu, ID_MENU_RECENT_FLOPPY_0 + i, this, &MainFrame::OnRecentFloppy);
	}

	BindMenuItem(cdrom_menu, ID_MENU_CDROM_DISABLED, this, &MainFrame::OnCdromDisabled);
	BindMenuItem(cdrom_menu, ID_MENU_CDROM_EMPTY, this, &MainFrame::OnCdromEmpty);
	BindMenuItem(cdrom_menu, ID_MENU_CDROM_ISO, this, &MainFrame::OnCdromIso);
	BindMenuItem(cdrom_menu, ID_MENU_CDROM_IOCTL, this, &MainFrame::OnCdromIoctl);
	BindMenuItem(recent_cdroms_menu, ID_MENU_CLEAR_RECENT_CDROMS, this, &MainFrame::OnClearRecentCdroms);
	for (int i = 0; i < MaxRecentCDROMs; ++i) {
		BindMenuItem(recent_cdroms_menu, ID_MENU_RECENT_CDROM_0 + i, this, &MainFrame::OnRecentCdrom);
	}

	BindMenuItem(settings_menu, ID_MENU_MACHINE, this, &MainFrame::OnMachine);
	BindMenuItem(settings_menu, ID_MENU_MUTE, this, &MainFrame::OnMute);
	BindMenuItem(settings_menu, ID_MENU_FULLSCREEN, this, &MainFrame::OnFullscreen);
	BindMenuItem(settings_menu, ID_MENU_INTEGER_SCALING, this, &MainFrame::OnIntegerScaling);
	BindMenuItem(settings_menu, ID_MENU_CPU_IDLE, this, &MainFrame::OnCpuIdle);
	BindMenuItem(mouse_menu, ID_MENU_MOUSE_HACK, this, &MainFrame::OnMouseHack);
	BindMenuItem(mouse_menu, ID_MENU_MOUSE_TWOBUTTON, this, &MainFrame::OnMouseTwobutton);
#ifdef RPCEMU_VNC
	BindMenuItem(settings_menu, ID_MENU_VNC, this, &MainFrame::OnVnc);
#endif
	BindMenuItem(settings_menu, ID_MENU_SERIAL, this, &MainFrame::OnSerial);
	BindMenuItem(settings_menu, ID_MENU_PARALLEL, this, &MainFrame::OnParallel);
#ifdef RPCEMU_NETWORKING
	BindMenuItem(settings_menu, ID_MENU_NAT_LIST, this, &MainFrame::OnNatList);
#endif

	BindMenuItem(debug_menu, ID_MENU_DEBUG_RUN, this, &MainFrame::OnDebugRun);
	BindMenuItem(debug_menu, ID_MENU_DEBUG_PAUSE, this, &MainFrame::OnDebugPause);
	BindMenuItem(debug_menu, ID_MENU_DEBUG_STEP, this, &MainFrame::OnDebugStep);
	BindMenuItem(debug_menu, ID_MENU_DEBUG_STEP5, this, &MainFrame::OnDebugStep5);
	BindMenuItem(debug_menu, ID_MENU_MACHINE_INSPECTOR, this, &MainFrame::OnMachineInspector);

	BindMenuItem(help_menu, ID_MENU_ONLINE_MANUAL, this, &MainFrame::OnOnlineManual);
	BindMenuItem(help_menu, ID_MENU_VISIT_WEBSITE, this, &MainFrame::OnVisitWebsite);
	BindMenuItem(help_menu, ID_MENU_ABOUT, this, &MainFrame::OnAbout);

	BindAllMenuOpenCloseHandlers();

	UpdateRecentMachinesMenu();
	UpdateRecentFloppiesMenu();
	UpdateRecentCdromsMenu();
	SyncSettingsMenuChecks();
	SyncCdromMenuChecks();
	UpdateDebuggerActionStates();
}

void MainFrame::BuildToolBar()
{
	const wxSize icon_size(24, 24);

	tool_bar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_NODIVIDER);
	tool_bar_->SetToolBitmapSize(icon_size);

	// Normal operations (left)
	tool_bar_->AddTool(ID_MENU_SCREENSHOT, wxEmptyString, ToolbarIconScreenshot(icon_size),
	                   "Save screenshot (F12)");
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_LOAD_DISC0, wxEmptyString, ToolbarIconFloppy(icon_size),
	                   "Load floppy disc into drive :0 (Ctrl+1)");
	tool_bar_->AddTool(ID_MENU_CDROM_ISO, wxEmptyString, ToolbarIconCdrom(icon_size),
	                   "Load CD-ROM ISO image");
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_RESET, wxEmptyString, ToolbarIconReset(icon_size),
	                   "Reset machine (Ctrl+R)");
	tool_bar_->AddSeparator();
	tb_mute_tool_ = tool_bar_->AddCheckTool(ID_MENU_MUTE, wxEmptyString,
	                                        ToolbarIconMute(false, icon_size), wxNullBitmap,
	                                        "Toggle sound mute (F10)");
	tool_bar_->AddTool(ID_MENU_FULLSCREEN, wxEmptyString, ToolbarIconFullscreen(icon_size),
	                   "Toggle full-screen mode (F11)");
	tool_bar_->AddTool(ID_MENU_MACHINE, wxEmptyString, ToolbarIconConfigure(icon_size),
	                   "Edit machine settings (Ctrl+,)");

	// Debugger (right)
	tool_bar_->AddStretchableSpace();
	tool_bar_->AddSeparator();
	tool_bar_->AddTool(ID_MENU_DEBUG_RUN, wxEmptyString, ToolbarIconDebugRun(icon_size),
	                   "Run emulation (F5)");
	tool_bar_->AddTool(ID_MENU_DEBUG_PAUSE, wxEmptyString, ToolbarIconDebugPause(icon_size),
	                   "Pause emulation (F6)");
	tool_bar_->AddTool(ID_MENU_DEBUG_STEP, wxEmptyString, ToolbarIconDebugStep(icon_size),
	                   "Single step (F7)");
	tool_bar_->AddTool(ID_MENU_MACHINE_INSPECTOR, wxEmptyString, ToolbarIconInspector(icon_size),
	                   "Open Machine Inspector (F9)");
	tool_bar_->Realize();

	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnScreenshot, this, ID_MENU_SCREENSHOT);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnLoadDisc0, this, ID_MENU_LOAD_DISC0);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnCdromIso, this, ID_MENU_CDROM_ISO);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnReset, this, ID_MENU_RESET);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnDebugRun, this, ID_MENU_DEBUG_RUN);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnDebugPause, this, ID_MENU_DEBUG_PAUSE);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnDebugStep, this, ID_MENU_DEBUG_STEP);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnMute, this, ID_MENU_MUTE);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnFullscreen, this, ID_MENU_FULLSCREEN);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnMachine, this, ID_MENU_MACHINE);
	tool_bar_->Bind(wxEVT_TOOL, &MainFrame::OnMachineInspector, this, ID_MENU_MACHINE_INSPECTOR);
}

void MainFrame::BuildStatusBar()
{
	CreateStatusBar(STATUS_FIELD_COUNT);

	int widths[STATUS_FIELD_COUNT] = {
	    90, 90, 50, 20, 80, 20, 50, 20, 30, 20, -1,
	};
	SetStatusWidths(STATUS_FIELD_COUNT, widths);

	SetStatusText("MIPS: -", STATUS_MIPS);
	SetStatusText("Avg: -", STATUS_AVG_MIPS);
	SetStatusText("Floppy", STATUS_FDC_LABEL);
	SetStatusText(LedText(false), STATUS_FDC_LED);
	SetStatusText("HDD/CDROM", STATUS_IDE_LABEL);
	SetStatusText(LedText(false), STATUS_IDE_LED);
	SetStatusText("HostFS", STATUS_HOSTFS_LABEL);
	SetStatusText(LedText(false), STATUS_HOSTFS_LED);
	SetStatusText("Net", STATUS_NET_LABEL);
	SetStatusText(LedText(false), STATUS_NET_LED);
	UpdateMachineStatus();
}

void MainFrame::UpdateRecentMachinesMenu()
{
	if (recent_machines_menu_ == nullptr) {
		return;
	}
	PopulateRecentMenu(recent_machines_menu_, GetRecentMachines(), MaxRecentMachines,
	                   ID_MENU_RECENT_MACHINE_0, ID_MENU_CLEAR_RECENT_MACHINES,
	                   "Clear Recent Machines", "(No recent machines)", false);
}

void MainFrame::UpdateRecentFloppiesMenu()
{
	if (recent_floppies_menu_ == nullptr) {
		return;
	}
	PopulateRecentMenu(recent_floppies_menu_, GetRecentFloppies(), MaxRecentFloppies,
	                   ID_MENU_RECENT_FLOPPY_0, ID_MENU_CLEAR_RECENT_FLOPPIES,
	                   "Clear Recent Images", "(No recent images)", true);
}

void MainFrame::UpdateRecentCdromsMenu()
{
	if (recent_cdroms_menu_ == nullptr) {
		return;
	}
	PopulateRecentMenu(recent_cdroms_menu_, GetRecentCDROMs(), MaxRecentCDROMs,
	                   ID_MENU_RECENT_CDROM_0, ID_MENU_CLEAR_RECENT_CDROMS,
	                   "Clear Recent Images", "(No recent images)", true);
}

void MainFrame::SyncSettingsMenuChecks()
{
	if (cpu_idle_menu_item_ != nullptr) {
		cpu_idle_menu_item_->Check(config_copy_.cpu_idle != 0);
	}
	if (mouse_hack_menu_item_ != nullptr) {
		mouse_hack_menu_item_->Check(config_copy_.mousehackon != 0);
	}
	if (mouse_twobutton_menu_item_ != nullptr) {
		mouse_twobutton_menu_item_->Check(config_copy_.mousetwobutton != 0);
	}
	if (integer_scaling_menu_item_ != nullptr) {
		integer_scaling_menu_item_->Check(config_copy_.integer_scaling != 0);
	}
	if (mute_menu_item_ != nullptr) {
		mute_menu_item_->Check(plt_sound_is_muted() != 0);
	}
	if (tb_mute_tool_ != nullptr && tool_bar_ != nullptr) {
		const bool muted = plt_sound_is_muted() != 0;
		tool_bar_->ToggleTool(ID_MENU_MUTE, muted);
		tool_bar_->SetToolNormalBitmap(ID_MENU_MUTE, ToolbarIconMute(muted));
	}
#ifdef RPCEMU_NETWORKING
	if (nat_list_item_ != nullptr) {
		nat_list_item_->Enable(config_copy_.network_type == NetworkType_NAT);
	}
#endif
}

void MainFrame::SyncCdromMenuChecks()
{
	if (cdrom_disabled_item_ == nullptr) {
		return;
	}

	int selected_id = ID_MENU_CDROM_DISABLED;
	if (config_copy_.cdromenabled) {
		if (config_copy_.cdromtype == 2) {
			selected_id = ID_MENU_CDROM_ISO;
		} else {
			selected_id = ID_MENU_CDROM_EMPTY;
		}
	}

	CdromMenuSelectionUpdate(selected_id);
}

void MainFrame::CdromMenuSelectionUpdate(int menu_id)
{
	if (cdrom_disabled_item_ != nullptr) {
		cdrom_disabled_item_->Check(menu_id == ID_MENU_CDROM_DISABLED);
	}
	if (cdrom_empty_item_ != nullptr) {
		cdrom_empty_item_->Check(menu_id == ID_MENU_CDROM_EMPTY);
	}
	if (cdrom_iso_item_ != nullptr) {
		cdrom_iso_item_->Check(menu_id == ID_MENU_CDROM_ISO);
	}
	if (cdrom_ioctl_item_ != nullptr) {
		cdrom_ioctl_item_->Check(menu_id == ID_MENU_CDROM_IOCTL);
	}
}

void MainFrame::UpdateDebuggerActionStates()
{
	if (emulator_ == nullptr || !emulator_->IsRunning()) {
		return;
	}

	const MachineSnapshot snapshot = emulator_->TakeSnapshot();
	const bool paused = snapshot.debug_paused != 0;
	const bool pausing = snapshot.debug_pause_requested != 0;

	if (debug_run_item_ != nullptr) {
		debug_run_item_->Enable(paused);
	}
	if (debug_pause_item_ != nullptr) {
		debug_pause_item_->Enable(!paused || pausing);
	}
	if (debug_step_item_ != nullptr) {
		debug_step_item_->Enable(paused);
	}
	if (debug_step5_item_ != nullptr) {
		debug_step5_item_->Enable(paused);
	}
	if (tool_bar_ != nullptr) {
		tool_bar_->EnableTool(ID_MENU_DEBUG_RUN, paused);
		tool_bar_->EnableTool(ID_MENU_DEBUG_PAUSE, !paused || pausing);
		tool_bar_->EnableTool(ID_MENU_DEBUG_STEP, paused);
	}
}
