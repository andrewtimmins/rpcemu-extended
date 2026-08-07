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

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>

#include "config_paths.h"
#include "machine_edit_dialog.h"
#include "new_machine_dialog.h"

extern "C" {
#include "machine_lock.h"
#include "rpcemu.h"
}

namespace {

enum {
	ID_NEW = wxID_HIGHEST + 500,
	ID_EDIT,
	ID_CLONE,
	ID_DELETE,
	ID_START,
	ID_STOP,
	ID_RESET,
	ID_RESTART,
	ID_POLL_TIMER,
};

/* How long a --managed child gets to publish its control-channel endpoint
   before the Manager gives up on it (ROM loading and machine start can
   legitimately take a few seconds; a genuinely wedged/crashed launch should
   not hang the UI forever). */
constexpr int kStartupTimeoutMs = 20000;
constexpr int kPollIntervalMs = 200;

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
	EVT_LISTBOX(wxID_ANY, ManagerFrame::OnMachineSelected)
	EVT_LISTBOX_DCLICK(wxID_ANY, ManagerFrame::OnMachineActivated)
	EVT_BUTTON(ID_NEW, ManagerFrame::OnNew)
	EVT_BUTTON(ID_EDIT, ManagerFrame::OnEdit)
	EVT_BUTTON(ID_CLONE, ManagerFrame::OnClone)
	EVT_BUTTON(ID_DELETE, ManagerFrame::OnDelete)
	EVT_BUTTON(ID_START, ManagerFrame::OnStart)
	EVT_BUTTON(ID_STOP, ManagerFrame::OnStop)
	EVT_MENU(ID_NEW, ManagerFrame::OnNew)
	EVT_MENU(ID_EDIT, ManagerFrame::OnEdit)
	EVT_MENU(ID_CLONE, ManagerFrame::OnClone)
	EVT_MENU(ID_DELETE, ManagerFrame::OnDelete)
	EVT_MENU(ID_START, ManagerFrame::OnStart)
	EVT_MENU(ID_STOP, ManagerFrame::OnStop)
	EVT_MENU(ID_RESET, ManagerFrame::OnReset)
	EVT_MENU(ID_RESTART, ManagerFrame::OnRestart)
	EVT_MENU(wxID_EXIT, ManagerFrame::OnExit)
	EVT_CLOSE(ManagerFrame::OnClose)
	EVT_TIMER(ID_POLL_TIMER, ManagerFrame::OnPollTimer)
wxEND_EVENT_TABLE()

ManagerFrame::ManagerFrame()
	: wxFrame(nullptr, wxID_ANY, "RPCEmu Extended", wxDefaultPosition, wxSize(1000, 700))
	, poll_timer_(this, ID_POLL_TIMER)
{
	BuildUi();
	BuildMenus();
	RefreshMachineList();
	DiscoverAlreadyRunningMachines();
	poll_timer_.Start(kPollIntervalMs);
}

ManagerFrame::~ManagerFrame()
{
}

void ManagerFrame::BuildUi()
{
	auto *root = new wxBoxSizer(wxHORIZONTAL);

	auto *left_panel = new wxPanel(this);
	auto *left_sizer = new wxBoxSizer(wxVERTICAL);

	left_sizer->Add(new wxStaticText(left_panel, wxID_ANY, "Machines:"), 0, wxALL, 6);

	machine_list_ = new wxListBox(left_panel, wxID_ANY);
	left_sizer->Add(machine_list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

	auto *manage_row = new wxBoxSizer(wxHORIZONTAL);
	new_button_ = new wxButton(left_panel, ID_NEW, "New...");
	edit_button_ = new wxButton(left_panel, ID_EDIT, "Edit...");
	clone_button_ = new wxButton(left_panel, ID_CLONE, "Clone...");
	delete_button_ = new wxButton(left_panel, ID_DELETE, "Delete");
	manage_row->Add(new_button_, 0, wxALL, 3);
	manage_row->Add(edit_button_, 0, wxALL, 3);
	manage_row->Add(clone_button_, 0, wxALL, 3);
	manage_row->Add(delete_button_, 0, wxALL, 3);
	left_sizer->Add(manage_row, 0, wxALIGN_CENTER);

	auto *power_row = new wxBoxSizer(wxHORIZONTAL);
	start_button_ = new wxButton(left_panel, ID_START, "Start");
	stop_button_ = new wxButton(left_panel, ID_STOP, "Stop");
	power_row->Add(start_button_, 0, wxALL, 3);
	power_row->Add(stop_button_, 0, wxALL, 3);
	left_sizer->Add(power_row, 0, wxALIGN_CENTER | wxBOTTOM, 6);

	left_panel->SetSizer(left_sizer);

	display_book_ = new wxSimplebook(this);
	auto *placeholder = new wxPanel(display_book_);
	placeholder->SetBackgroundColour(*wxBLACK);
	auto *placeholder_sizer = new wxBoxSizer(wxVERTICAL);
	auto *placeholder_text = new wxStaticText(placeholder, wxID_ANY,
	    "Select a machine on the left and press Start.");
	placeholder_text->SetForegroundColour(*wxWHITE);
	placeholder_sizer->AddStretchSpacer();
	placeholder_sizer->Add(placeholder_text, 0, wxALIGN_CENTER);
	placeholder_sizer->AddStretchSpacer();
	placeholder->SetSizer(placeholder_sizer);
	placeholder_page_ = display_book_->GetPageCount();
	display_book_->AddPage(placeholder, "", true);

	root->Add(left_panel, 0, wxEXPAND);
	root->Add(display_book_, 1, wxEXPAND);
	SetSizer(root);
}

void ManagerFrame::BuildMenus()
{
	auto *file_menu = new wxMenu();
	file_menu->Append(ID_NEW, "&New Machine...\tCtrl+N");
	file_menu->Append(ID_EDIT, "&Edit Machine...");
	file_menu->Append(ID_CLONE, "&Clone Machine...");
	file_menu->Append(ID_DELETE, "&Delete Machine");
	file_menu->AppendSeparator();
	file_menu->Append(wxID_EXIT, "E&xit\tCtrl+Q");

	auto *machine_menu = new wxMenu();
	machine_menu->Append(ID_START, "&Start\tCtrl+S");
	stop_item_ = machine_menu->Append(ID_STOP, "S&top");
	machine_menu->AppendSeparator();
	reset_item_ = machine_menu->Append(ID_RESET, "&Reset");
	restart_item_ = machine_menu->Append(ID_RESTART, "Re&start");

	auto *menu_bar = new wxMenuBar();
	menu_bar->Append(file_menu, "&File");
	menu_bar->Append(machine_menu, "&Machine");
	SetMenuBar(menu_bar);
}

wxString ManagerFrame::MachineDirFor(const wxString &name) const
{
	/* Matches rpcemu_set_machine_datadir()'s convention exactly (see
	   rpc-machdep.c) - this process never loads any machine's config, so it
	   has to compute the directory the same way rather than ask the core
	   for "the" machine directory, which is a single-machine notion. */
	return wxString::FromUTF8(rpcemu_get_datadir()) + "machines" +
	    wxFileName::GetPathSeparator() + name + wxFileName::GetPathSeparator();
}

void ManagerFrame::RefreshMachineList()
{
	const wxString was_selected = SelectedMachineName();

	machine_list_->Clear();
	machine_names_.clear();

	for (const std::string &name_utf8 : ConfigPathsMachineNames()) {
		const wxString name = wxString::FromUTF8(name_utf8);
		wxString label = name;

		if (running_.count(name) != 0) {
			label += running_[name].starting ? "  (starting...)" : "  (running)";
		}

		machine_names_.push_back(name);
		machine_list_->Append(label);

		if (name == was_selected) {
			machine_list_->SetSelection((int) machine_names_.size() - 1);
		}
	}

	UpdateButtons();
}

wxString ManagerFrame::SelectedMachineName() const
{
	const int sel = machine_list_->GetSelection();

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
	start_button_->Enable(have_selection && !is_running);
	stop_button_->Enable(is_running);

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
		if (!machine_lock_read_ipc_endpoint(dir.utf8_str().data(), endpoint, sizeof(endpoint)) ||
		    endpoint[0] == '\0') {
			continue;	/* running, but not as a --managed child - nothing for us to attach to */
		}

		AttachPanelFor(name, wxString::FromUTF8(MachineIpcNameFor(dir.utf8_str().data(), pid)),
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
		/* Stale lock / the process ended between discovery and here. */
		panel->Destroy();
		if (newly_started) {
			wxMessageBox("'" + name + "' did not start.", "RPCEmu Extended Manager",
			    wxOK | wxICON_ERROR, this);
		}
		RemoveRunningEntry(name);
		return;
	}

	panel->SetGoneCallback([this, name]() { RemoveRunningEntry(name); });

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

void ManagerFrame::StartMachine(const wxString &name)
{
	auto existing = running_.find(name);
	if (existing != running_.end()) {
		ShowMachinePanel(name);
		return;
	}

	const wxString exe = wxStandardPaths::Get().GetExecutablePath();
	wxString cmd;
	cmd << '"' << exe << "\" --managed --machine \"" << name << '"';

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

		if (machine_lock_read_owner(dir.utf8_str().data(), &pid, &vnc_port) && pid != 0 &&
		    machine_lock_read_ipc_endpoint(dir.utf8_str().data(), endpoint, sizeof(endpoint)) &&
		    endpoint[0] != '\0') {
			AttachPanelFor(name, wxString::FromUTF8(MachineIpcNameFor(dir.utf8_str().data(), pid)),
			    wxString::FromUTF8(endpoint), true);
			continue;
		}

		if ((wxGetLocalTimeMillis() - it->second.start_time_ms).ToLong() > kStartupTimeoutMs) {
			wxMessageBox("'" + name + "' did not finish starting in time.",
			    "RPCEmu Extended Manager", wxOK | wxICON_ERROR, this);
			RemoveRunningEntry(name);
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
	} else {
		active_machine_.clear();
		display_book_->SetSelection((size_t) placeholder_page_);
	}
	UpdateButtons();
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
	   event RemoteEmulatorPanel reports through its GoneCallback, or the
	   OS process exiting, whichever the machine actually manages given
	   whatever state it is in. Asking twoffice is harmless. */
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
}

void ManagerFrame::OnChildProcessEnded(const wxString &machine_name, int /*pid*/, int /*status*/)
{
	RemoveRunningEntry(machine_name);
}

void ManagerFrame::OnMachineSelected(wxCommandEvent & /*event*/)
{
	UpdateButtons();

	const wxString name = SelectedMachineName();
	if (!name.empty() && running_.count(name) != 0 && running_[name].panel != nullptr) {
		ShowMachinePanel(name);
	}
}

void ManagerFrame::OnMachineActivated(wxCommandEvent & /*event*/)
{
	const wxString name = SelectedMachineName();
	if (name.empty()) {
		return;
	}
	StartMachine(name);
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
	ConfigPathsCopyDirectory(MachineDirFor(name), MachineDirFor(sanitized));

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

	wxArrayString files;
	const wxString dir = MachineDirFor(name);
	if (wxDirExists(dir)) {
		wxDir::GetAllFiles(dir, &files);
		files.Sort();
		std::reverse(files.begin(), files.end());
		for (const auto &f : files) {
			if (wxDirExists(f)) {
				wxRmdir(f);
			} else {
				wxRemoveFile(f);
			}
		}
		wxRmdir(dir);
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

void ManagerFrame::OnExit(wxCommandEvent & /*event*/)
{
	Close(true);
}

void ManagerFrame::OnClose(wxCloseEvent &event)
{
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
	}
	poll_timer_.Stop();
	event.Skip();
}
