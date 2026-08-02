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

#include "config_selector_dialog.h"

#include "config_paths.h"
#include "gui_preferences.h"
#include "machine_edit_dialog.h"
#include "new_machine_dialog.h"

#include <cstdio>

#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/textdlg.h>

extern "C" {
#include "rpcemu.h"
#include "savestate.h"
}

ConfigSelectorDialog::ConfigSelectorDialog(wxWindow *parent)
	: wxDialog(parent, wxID_ANY, "RPCEmu - Select Machine",
	           wxDefaultPosition, wxSize(520, 420),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto *list_label = new wxStaticText(this, wxID_ANY, "Available machines:");
	config_list_ = new wxListBox(this, wxID_ANY);
	empty_note_ = new wxStaticText(this, wxID_ANY,
	    "No machines yet.\n\nChoose New... to set one up, with RISC OS "
	    "downloaded and ready to run.");
	empty_note_->Wrap(230);

	new_button_ = new wxButton(this, wxID_ANY, "New...");
	edit_button_ = new wxButton(this, wxID_ANY, "Edit...");
	clone_button_ = new wxButton(this, wxID_ANY, "Clone...");
	delete_button_ = new wxButton(this, wxID_ANY, "Delete");
	load_state_button_ = new wxButton(this, wxID_ANY, "Load State...");
	resume_button_ = new wxButton(this, wxID_ANY, "Resume");
	start_button_ = new wxButton(this, wxID_OK, "Start");
	auto *cancel_button = new wxButton(this, wxID_CANCEL, "Cancel");
	default_button_ = new wxButton(this, wxID_ANY, "Set as Default");
	default_button_->SetToolTip(
	    "Open this machine on startup without showing this list.\n"
	    "Hold Shift while starting RPCEmu to get the list back, or turn it off "
	    "from Settings > Open This Machine Automatically.");
	start_button_->SetDefault();

	/* One size for every button, taken from whichever needs the most room, so
	   the column reads as a column rather than a ragged edge. Cancel is included
	   even though it sits on its own row: at the same width it lines up under
	   the others instead of ending a few pixels short of them.

	   The buttons also change label as the selection changes - Start becomes
	   Restart, Resume appears - so a size that followed the text would shift
	   about while the dialogue was open. */
	{
		wxButton *const buttons[] = {
			new_button_, edit_button_, clone_button_, delete_button_,
			load_state_button_, resume_button_, start_button_, cancel_button,
			default_button_
		};
		wxSize uniform(0, 0);

		for (wxButton *b : buttons) {
			const wxSize best = b->GetBestSize();

			uniform.x = wxMax(uniform.x, best.x);
			uniform.y = wxMax(uniform.y, best.y);
		}
		/* Room for the longest label any of them takes later on. */
		uniform.x = wxMax(uniform.x, GetTextExtent("Restart").GetWidth() + 40);
		uniform.x = wxMax(uniform.x, GetTextExtent("Clear Default").GetWidth() + 40);
		for (wxButton *b : buttons) {
			b->SetMinSize(uniform);
		}
	}

	auto *button_col = new wxBoxSizer(wxVERTICAL);
	button_col->Add(new_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->Add(edit_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->Add(clone_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->Add(delete_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->AddStretchSpacer();
	button_col->Add(load_state_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->Add(resume_button_, 0, wxEXPAND | wxBOTTOM, 4);
	button_col->Add(start_button_, 0, wxEXPAND | wxBOTTOM, 4);

	/* The prompt sits over the list's own column, shown only when it is empty. */
	auto *list_col = new wxBoxSizer(wxVERTICAL);
	list_col->Add(config_list_, 1, wxEXPAND);
	list_col->Add(empty_note_, 0, wxEXPAND | wxTOP, 8);

	auto *body = new wxBoxSizer(wxHORIZONTAL);
	body->Add(list_col, 1, wxEXPAND | wxRIGHT, 8);
	body->Add(button_col, 0, wxEXPAND);

	auto *bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->Add(default_button_, 0);
	bottom->AddStretchSpacer();
	bottom->Add(cancel_button, 0);	/* no inset: its edges match the column above */

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(list_label, 0, wxALL, 8);
	main->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
	main->Add(bottom, 0, wxEXPAND | wxALL, 8);
	SetSizer(main);

	wxDir::Make(ConfigPathsConfigsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	wxDir::Make(ConfigPathsMachinesDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	/* No machine is invented here. A machine RPCEmu made up has no ROM and an
	   empty disc, so it cannot start, and offering one as the first thing in
	   the list only teaches somebody that starting a machine does not work.
	   New... sets one up properly, RISC OS and all. */
	RefreshConfigList();

	new_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnNew, this);
	edit_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnEdit, this);
	clone_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnClone, this);
	delete_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnDelete, this);
	start_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnStart, this);
	resume_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnResume, this);
	load_state_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnLoadStateFile, this);
	cancel_button->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnCancel, this);
	default_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnToggleDefault, this);
	config_list_->Bind(wxEVT_LISTBOX_DCLICK, &ConfigSelectorDialog::OnListDoubleClick, this);
	config_list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { UpdateButtons(); });
}

void ConfigSelectorDialog::RefreshConfigList()
{
	const wxString was_selected = SelectedMachineName();
	const wxString default_machine = wxString::FromUTF8(GetDefaultMachine());
	int select_row = 0;

	config_list_->Clear();
	config_entries_.clear();

	wxArrayString files;
	wxDir::GetAllFiles(ConfigPathsConfigsDir(), &files, "*.cfg", wxDIR_FILES);
	files.Sort();

	for (const auto &path : files) {
		wxFileConfig settings(wxEmptyString, wxEmptyString, path, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
		ConfigFileUseGeneralGroup(settings);
		wxString name;
		settings.Read("name", &name, wxEmptyString);
		if (name.empty()) {
			name = wxFileName(path).GetName();
		}
		ConfigEntry entry;
		entry.display_name = name;
		entry.config_path = ConfigPathsAbsoluteConfigPath(path);

		/* Marked in the list as well as on the button, so which machine opens
		   on startup can be seen without clicking through them. */
		const wxString base = wxFileName(path).GetName();
		if (!default_machine.empty() && base == default_machine) {
			name += "   (opens on startup)";
		}
		if (base == was_selected) {
			select_row = static_cast<int>(config_entries_.size());
		}

		config_entries_.push_back(entry);
		config_list_->Append(name);
	}

	if (config_list_->GetCount() > 0) {
		config_list_->SetSelection(select_row);
	}
	UpdateButtons();
}

void ConfigSelectorDialog::UpdateButtons()
{
	const bool has_selection = config_list_->GetSelection() != wxNOT_FOUND;

	/* With no machines at all, every button but New... is unavailable, which on
	   its own reads as something being broken rather than as a starting point. */
	if (empty_note_ != nullptr) {
		empty_note_->Show(config_list_->IsEmpty());
		Layout();
	}
	edit_button_->Enable(has_selection);
	clone_button_->Enable(has_selection);
	/* Deleting the last machine is allowed now that having none is a state the
	   selector handles; New... is how you get back from it. */
	delete_button_->Enable(has_selection);
	start_button_->Enable(has_selection);

	/* Whether this machine is the one opened without asking. The label says
	   what pressing it would do, rather than what the machine currently is. */
	{
		const wxString name = SelectedMachineName();
		const bool is_default =
		    has_selection && !name.empty() &&
		    name == wxString::FromUTF8(GetDefaultMachine());

		default_button_->Enable(has_selection);
		default_button_->SetLabel(is_default ? "Clear Default" : "Set as Default");
	}

	// If the selected machine has a suspend snapshot, offer Resume (default)
	// and relabel Start as Restart (cold boot). Otherwise just Start.
	bool has_snapshot = false;
	if (has_selection) {
		has_snapshot = wxFileExists(ConfigPathsSnapshotForConfig(SelectedConfigPath()));
	}

	resume_button_->Show(has_snapshot);
	resume_button_->Enable(has_snapshot);
	start_button_->SetLabel(has_snapshot ? "Restart" : "Start");
	if (has_snapshot) {
		resume_button_->SetDefault();
	} else {
		start_button_->SetDefault();
	}
	Layout();
}

wxString ConfigSelectorDialog::SelectedConfigPath() const
{
	const int sel = config_list_->GetSelection();
	if (sel < 0 || sel >= static_cast<int>(config_entries_.size())) {
		return wxEmptyString;
	}
	return config_entries_[static_cast<size_t>(sel)].config_path;
}

wxString ConfigSelectorDialog::SelectedMachineName() const
{
	const wxString path = SelectedConfigPath();

	if (path.empty()) {
		return wxEmptyString;
	}
	return wxFileName(path).GetName();
}

/**
 * Mark the selected machine as the one to open on startup, or stop it being so.
 *
 * The list is rebuilt afterwards because the machine that is the default is
 * marked in it, so the change has to be visible somewhere other than this
 * button's own label.
 */
void ConfigSelectorDialog::OnToggleDefault(wxCommandEvent &)
{
	const wxString name = SelectedMachineName();

	if (name.empty()) {
		return;
	}
	if (name == wxString::FromUTF8(GetDefaultMachine())) {
		ClearDefaultMachine();
	} else {
		SetDefaultMachine(name.utf8_string());
	}
	RefreshConfigList();
}

void ConfigSelectorDialog::OnStart(wxCommandEvent &)
{
	selected_config_path_ = SelectedConfigPath();
	if (selected_config_path_.empty()) {
		wxMessageBox("Select a machine configuration to start.", "RPCEmu", wxOK | wxICON_INFORMATION, this);
		return;
	}
	resume_selected_ = false;
	state_file_to_load_.clear();
	EndModal(wxID_OK);
}

void ConfigSelectorDialog::OnResume(wxCommandEvent &)
{
	selected_config_path_ = SelectedConfigPath();
	if (selected_config_path_.empty()) {
		return;
	}
	resume_selected_ = true;
	state_file_to_load_ = ConfigPathsSnapshotForConfig(selected_config_path_);
	EndModal(wxID_OK);
}

void ConfigSelectorDialog::OnLoadStateFile(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Load Machine State", ConfigPathsMachinesDir(), wxEmptyString,
	                 "RPCEmu machine state (*.state)|*.state|All files (*)|*",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}
	const wxString file = dlg.GetPath();

	/* The snapshot records the machine it belongs to; boot that machine. */
	char name[256];
	if (state_get_machine_name(file.utf8_str().data(), name, sizeof(name)) != 0) {
		wxMessageBox("This is not a valid RPCEmu state file, or it was made by a "
		             "different version of RPCEmu.",
		             "RPCEmu", wxOK | wxICON_WARNING, this);
		return;
	}
	const wxString machine_name = wxString::FromUTF8(name);

	wxString config_path;
	for (const auto &entry : config_entries_) {
		if (entry.display_name == machine_name) {
			config_path = entry.config_path;
			break;
		}
	}
	if (config_path.empty()) {
		wxMessageBox(wxString::Format(
		                 "This state file belongs to machine '%s', but no machine "
		                 "with that name exists.\n\nCreate or rename a machine to "
		                 "'%s' and try again.",
		                 machine_name, machine_name),
		             "RPCEmu", wxOK | wxICON_WARNING, this);
		return;
	}

	selected_config_path_ = config_path;
	state_file_to_load_ = file;
	resume_selected_ = true;
	EndModal(wxID_OK);
}

void ConfigSelectorDialog::OnCancel(wxCommandEvent &)
{
	EndModal(wxID_CANCEL);
}

void ConfigSelectorDialog::OnListDoubleClick(wxCommandEvent &event)
{
	// Double-click performs the default action: Resume if the machine has a
	// snapshot, otherwise Start.
	const wxString path = SelectedConfigPath();
	if (!path.empty() && wxFileExists(ConfigPathsSnapshotForConfig(path))) {
		OnResume(event);
	} else {
		OnStart(event);
	}
}

/*
 * Create a machine, and offer to put RISC OS on it while we are here.
 *
 * The naming and the download are one step because this is the only moment when
 * the machine is guaranteed to have no hard disc of its own, so the rule that a
 * download never overwrites one cannot get in the way. The dialogue creates the
 * machine itself, so by the time it returns there is something to select.
 */
void ConfigSelectorDialog::OnNew(wxCommandEvent &)
{
	NewMachineDialog dlg(this);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString config_path = dlg.CreatedConfigPath();

	if (config_path.empty()) {
		return;
	}

	RefreshConfigList();
	for (unsigned i = 0; i < config_entries_.size(); ++i) {
		if (config_entries_[i].config_path == config_path) {
			config_list_->SetSelection(static_cast<int>(i));
			break;
		}
	}

	wxCommandEvent dummy;
	OnEdit(dummy);
}

void ConfigSelectorDialog::OnEdit(wxCommandEvent &)
{
	const wxString path = SelectedConfigPath();
	if (path.empty()) {
		return;
	}

	MachineEditDialog dlg(this, path);
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (dlg.WasRenamed()) {
		const wxString old_name = config_entries_[static_cast<size_t>(config_list_->GetSelection())].display_name;
		/* The new path is unused: the list is rebuilt from disc below. */
		ConfigPathsRenameMachine(old_name, dlg.GetNewName(), path);
	}

	RefreshConfigList();
}

void ConfigSelectorDialog::OnDelete(wxCommandEvent &)
{
	const int sel = config_list_->GetSelection();
	if (sel < 0) {
		return;
	}

	const wxString name = config_entries_[static_cast<size_t>(sel)].display_name;
	const wxString path = config_entries_[static_cast<size_t>(sel)].config_path;

	if (wxMessageBox(wxString::Format(
	                     "Delete '%s'?\n\nThis removes the configuration and all machine data. This cannot be undone.",
	                     name),
	                 "Delete Machine",
	                 wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
	                 this) != wxYES) {
		return;
	}

	wxRemoveFile(path);
	const wxString machine_dir = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + name;

	/*
	 * wx's own recursive remove, not a shell command. This was
	 * system("rm -rf '<dir>'"), which had three problems: Windows has no rm, so
	 * deleting a machine there removed its configuration and silently left every
	 * byte of its data behind; a machine name is typed by the user and went into
	 * a shell command inside single quotes, so a name containing one would have
	 * been able to run whatever followed it; and the result was not looked at, so
	 * a failure to delete reported success.
	 */
	if (wxFileName::DirExists(machine_dir)) {
		if (!wxFileName::Rmdir(machine_dir, wxPATH_RMDIR_RECURSIVE)) {
			wxMessageBox(wxString::Format(
			                 "'%s' has been removed from the list, but its data "
			                 "could not be deleted from\n\n%s\n\nYou may want to "
			                 "remove that folder yourself.",
			                 name, machine_dir),
			             "Delete Machine", wxOK | wxICON_WARNING, this);
		}
	}
	RefreshConfigList();
}

void ConfigSelectorDialog::OnClone(wxCommandEvent &)
{
	const int sel = config_list_->GetSelection();
	if (sel < 0) {
		return;
	}

	const wxString source_path = config_entries_[static_cast<size_t>(sel)].config_path;
	const wxString source_name = config_entries_[static_cast<size_t>(sel)].display_name;

	wxTextEntryDialog dlg(this, "New machine name:", "Clone Machine", source_name + " (Copy)");
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString sanitized = ConfigPathsSanitizeName(dlg.GetValue());
	if (!ConfigPathsIsNameUnique(sanitized)) {
		wxMessageBox("That machine name already exists.", "Clone Machine", wxOK | wxICON_WARNING, this);
		return;
	}

	const wxString new_config = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + sanitized + ".cfg";
	wxCopyFile(source_path, new_config);

	wxFileConfig settings(wxEmptyString, wxEmptyString, new_config, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);
	settings.Write("name", sanitized);
	settings.Flush();

	const wxString src_machine = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + source_name;
	const wxString dst_machine = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() + sanitized;
	/* Copy the machine's own files (CMOS, HostFS, hard discs). This used to shell
	   out to "cp -a", which does not exist on Windows: the clone was listed but
	   its directory was never created, and nothing said so. */
	bool copied;
	if (wxDirExists(src_machine)) {
		copied = ConfigPathsCopyDirectory(src_machine, dst_machine);
	} else {
		copied = ConfigPathsCreateMachineDirectory(sanitized);
	}
	if (!copied) {
		wxRemoveFile(new_config);
		wxMessageBox("Could not copy the machine's files.", "Clone Machine",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	RefreshConfigList();
}
