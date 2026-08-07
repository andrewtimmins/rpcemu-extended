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
		wxLongLong start_time_ms;
	};

	void BuildUi();
	void BuildMenus();
	void BuildToolBar();
	void BuildStatusImages();
	wxPanel *BuildPlaceholderPage();
	void RefreshMachineList();
	wxString SelectedMachineName() const;
	void UpdateButtons();
	wxString MachineDirFor(const wxString &name) const;

	void DiscoverAlreadyRunningMachines();
	void StartMachine(const wxString &name);
	void StopMachine(const wxString &name);
	void ShowMachinePanel(const wxString &name);
	void RemoveRunningEntry(const wxString &name);
	void AttachPanelFor(const wxString &name, const wxString &shared_fb_name,
	                     const wxString &ipc_endpoint, bool newly_started);

	void OnMachineSelected(wxListEvent &event);
	void OnMachineActivated(wxListEvent &event);
	void OnNew(wxCommandEvent &event);
	void OnEdit(wxCommandEvent &event);
	void OnClone(wxCommandEvent &event);
	void OnDelete(wxCommandEvent &event);
	void OnStart(wxCommandEvent &event);
	void OnStop(wxCommandEvent &event);
	void OnReset(wxCommandEvent &event);
	void OnRestart(wxCommandEvent &event);
	void OnExit(wxCommandEvent &event);
	void OnClose(wxCloseEvent &event);
	void OnPollTimer(wxTimerEvent &event);

	wxListCtrl *machine_list_ = nullptr;
	wxImageList *status_images_ = nullptr;
	wxSimplebook *display_book_ = nullptr;
	int placeholder_page_ = -1;
	wxButton *new_button_ = nullptr;
	wxButton *edit_button_ = nullptr;
	wxButton *clone_button_ = nullptr;
	wxButton *delete_button_ = nullptr;
	wxToolBar *tool_bar_ = nullptr;
	wxMenuItem *reset_item_ = nullptr;
	wxMenuItem *restart_item_ = nullptr;
	wxMenuItem *stop_item_ = nullptr;
	wxMenuItem *start_item_ = nullptr;

	std::map<wxString, RunningMachine> running_;
	std::vector<wxString> machine_names_;	/* same order as machine_list_'s rows */
	wxString active_machine_;
	wxTimer poll_timer_;

	wxDECLARE_EVENT_TABLE();
};

#endif /* MANAGER_FRAME_H */
