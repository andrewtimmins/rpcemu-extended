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

#ifndef MANAGER_FRAME_H
#define MANAGER_FRAME_H

#include <map>
#include <vector>

#include <wx/wx.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/process.h>
#include <wx/simplebook.h>
#include <wx/splitter.h>

#include "remote_emulator_panel.h"

class ManagerChildProcess;

/*
 * The Manager window: replaces the modal machine selector as RPCEmu's normal
 * entry point. Lists every configured machine, can start any number of them
 * at once (each its own "--managed" child process - see main.cpp and
 * MainFrame::EnableManagedMode()), and shows whichever one is selected in a
 * single display area, the way VirtualBox/VMware Workstation's manager shows
 * one VM at a time out of several that are all actually running.
 *
 * A machine that is not currently shown is not paused or throttled in any
 * way by this window - it is a normal, independent process, doing whatever
 * it would be doing whether or not anything was looking at it. This window's
 * only job is to start/stop them and to display whichever one has focus.
 */
class ManagerFrame : public wxFrame {
public:
	ManagerFrame();
	~ManagerFrame() override;

	/* Called by ManagerChildProcess::OnTerminate. */
	void OnChildProcessEnded(const wxString &machine_name, int pid, int status);

private:
	struct RunningMachine {
		long pid = 0;
		wxProcess *process = nullptr;	/* null for a machine found already running at startup */
		RemoteEmulatorPanel *panel = nullptr;
		int book_page = -1;
		bool starting = false;		/* waiting for the child to publish its IPC endpoint */
		wxString last_attach_error;	/* why the most recent attempt failed */
		wxLongLong start_time_ms;
		/* The machine's own setting, from its state report. Starts true so a
		   machine that has not reported yet still explains itself. */
		bool show_fullscreen_message = true;
		/* Also from the state report. Starts false, so a machine that has not
		   said yet is asked about rather than stopped without warning. */
		bool suspend_on_exit = false;
	};

	void BuildUi();
	void BuildMenus();
	void BuildToolBar();
	void BuildStatusImages();
	wxPanel *BuildPlaceholderPage();
	void RefreshMachineList();
	void UpdateStatusText();
	wxString SelectedMachineName() const;
	void UpdateButtons();
	wxString MachineDirFor(const wxString &name) const;

	void StopAllAndClose();

	void DiscoverAlreadyRunningMachines();
	void StartMachine(const wxString &name, bool resume = false);
	void StopMachine(const wxString &name);

	/* Ask before stopping a running machine - Stop sits next to Start on the
	   toolbar and shutting a machine down by accident costs unsaved work.
	   Answers true when there is nothing running to lose. */
	bool ConfirmStop(const wxString &name);
	void ShowMachinePanel(const wxString &name);
	void RemoveRunningEntry(const wxString &name);
	void AttachPanelFor(const wxString &name, const wxString &shared_fb_name,
	                     const wxString &ipc_endpoint, bool newly_started);

	void OnMachineSelected(wxListEvent &event);
	void OnMachineActivated(wxListEvent &event);
	void OnMachineRightClick(wxListEvent &event);
	void OnStatusBarSize(wxSizeEvent &event);

	void SetMachinesPanelCollapsed(bool collapsed);
	void PositionCollapseButton();

	/*
	 * Full screen, done by this window rather than asked of the machine.
	 *
	 * Forwarding it, as every other machine command is forwarded, put the
	 * machine's own window full screen - and a managed machine's window never
	 * receives frames, because they go to shared memory for this window to
	 * draw instead (PostVideoUpdate, managed_mode_). So the command worked
	 * exactly as written and produced a full screen of black.
	 *
	 * Left with Alt+Enter, the same key a machine's own window uses.
	 */
	void EnterFullScreen();
	void ExitFullScreen();
	void SetFullScreenMenuChecked(bool checked);
	bool IsShowingFullScreen() const { return full_screen_; }
	void OnNew(wxCommandEvent &event);
	void OnEdit(wxCommandEvent &event);
	void OnClone(wxCommandEvent &event);
	void OnDelete(wxCommandEvent &event);
	void OnStart(wxCommandEvent &event);
	void OnResume(wxCommandEvent &event);
	void OnDataFolder(wxCommandEvent &event);

	/* RPCEmu's own settings - the data folder and how a machine is drawn.
	   Machine settings are a different window and belong to the machine. */
	void OnSettings(wxCommandEvent &event);
	void OnStop(wxCommandEvent &event);
	void OnReset(wxCommandEvent &event);
	void OnRestart(wxCommandEvent &event);
	void OnCreateShortcut(wxCommandEvent &event);
	void OnMinimalUi(wxCommandEvent &event);
	void OnMachineListToggle(wxCommandEvent &event);
	void ApplyMinimalUi(bool minimal);

	/* Showing or hiding the toolbar needs both a size event and a repaint;
	   neither implies the other. */
	void RelayoutAroundToolBar();
	void OnExit(wxCommandEvent &event);
	void OnClose(wxCloseEvent &event);
	void OnMachineListSize(wxSizeEvent &event);
	void OnPollTimer(wxTimerEvent &event);

	/*
	 * The machine window's own menus, rebuilt here and forwarded to whichever
	 * machine is being shown. See machine_ipc.h for why this is one message
	 * rather than a message per command.
	 */
	void BuildMachineMenus(wxMenuBar *menu_bar);
	void OnMachineMenuCommand(wxCommandEvent &event);
	bool SendMenuCommand(int id, bool checked, const wxString &argument = wxEmptyString);
	void UpdateMachineMenuState();
	void ApplyStateReport(const wxString &machine, const wxString &report);

	/* Commands that ask for a file. The dialogue belongs here, this being the
	   process with a window to put it over; the machine is sent the path. */
	void ForwardWithFileDialog(int id, const wxString &title,
	                           const wxString &wildcard, bool save);

	/* Help items that are about the application rather than about a machine,
	   and so are answered here whether or not one is running. */
	void ShowAboutDialog();

	/*
	 * A support bundle for any machine, running or not: everything in it but
	 * the screenshot is a file on disk, and a machine that has stopped - or
	 * crashed - is exactly the one somebody wants a bundle for.
	 */
	void CreateSupportBundle();

	wxMenu *machine_disc_menu_ = nullptr;
	wxMenu *machine_settings_menu_ = nullptr;
	wxMenu *machine_tools_menu_ = nullptr;
	wxMenu *machine_debug_menu_ = nullptr;
	wxMenu *machine_help_menu_ = nullptr;

	wxListCtrl *machine_list_ = nullptr;
	wxImageList *status_images_ = nullptr;
	wxSimplebook *display_book_ = nullptr;
	int placeholder_page_ = -1;

	/* The machine list collapses, so the width it had is kept to put it back
	   where the user had it. */
	wxSplitterWindow *splitter_ = nullptr;
	wxWindow *machines_panel_ = nullptr;
	int machines_panel_width_ = 300;
	bool machines_panel_collapsed_ = false;
	wxBitmapButton *collapse_button_ = nullptr;

	/* Full screen, and what to put back when leaving it. */
	bool full_screen_ = false;
	bool collapsed_before_full_screen_ = false;
	wxButton *new_button_ = nullptr;
	wxButton *edit_button_ = nullptr;
	wxButton *clone_button_ = nullptr;
	wxButton *delete_button_ = nullptr;
	wxToolBar *tool_bar_ = nullptr;
	bool mute_tool_muted_ = false;	/* which bitmap the mute tool is showing */
	wxMenuItem *reset_item_ = nullptr;
	wxMenuItem *restart_item_ = nullptr;
	wxMenuItem *resume_item_ = nullptr;
	wxMenuItem *shortcut_item_ = nullptr;
	wxMenuItem *minimal_ui_item_ = nullptr;
	wxMenuItem *machine_list_item_ = nullptr;
	bool minimal_ui_ = false;
	wxMenuItem *stop_item_ = nullptr;
	wxMenuItem *start_item_ = nullptr;

	/* Set while waiting for the machines to stop so the window can close
	   behind them; suppresses the warning when that close arrives. */
	bool closing_after_stop_ = false;

	std::map<wxString, RunningMachine> running_;
	std::vector<wxString> machine_names_;	/* same order as machine_list_'s rows */
	wxString active_machine_;
	size_t running_count_ = 0;
	wxTimer poll_timer_;

	wxDECLARE_EVENT_TABLE();
};

#endif /* MANAGER_FRAME_H */
