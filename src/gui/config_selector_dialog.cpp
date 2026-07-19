#include "config_selector_dialog.h"

#include "config_paths.h"
#include "machine_edit_dialog.h"

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

	new_button_ = new wxButton(this, wxID_ANY, "New...");
	edit_button_ = new wxButton(this, wxID_ANY, "Edit...");
	clone_button_ = new wxButton(this, wxID_ANY, "Clone...");
	delete_button_ = new wxButton(this, wxID_ANY, "Delete");
	load_state_button_ = new wxButton(this, wxID_ANY, "Load State...");
	resume_button_ = new wxButton(this, wxID_ANY, "Resume");
	start_button_ = new wxButton(this, wxID_OK, "Start");
	auto *cancel_button = new wxButton(this, wxID_CANCEL, "Cancel");
	start_button_->SetDefault();

	auto *button_col = new wxBoxSizer(wxVERTICAL);
	button_col->Add(new_button_, 0, wxBOTTOM, 4);
	button_col->Add(edit_button_, 0, wxBOTTOM, 4);
	button_col->Add(clone_button_, 0, wxBOTTOM, 4);
	button_col->Add(delete_button_, 0, wxBOTTOM, 4);
	button_col->AddStretchSpacer();
	button_col->Add(load_state_button_, 0, wxBOTTOM, 4);
	button_col->Add(resume_button_, 0, wxBOTTOM, 4);
	button_col->Add(start_button_, 0, wxBOTTOM, 4);

	auto *body = new wxBoxSizer(wxHORIZONTAL);
	body->Add(config_list_, 1, wxEXPAND | wxRIGHT, 8);
	body->Add(button_col, 0, wxEXPAND);

	auto *bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->AddStretchSpacer();
	bottom->Add(cancel_button, 0, wxLEFT, 4);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(list_label, 0, wxALL, 8);
	main->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
	main->Add(bottom, 0, wxEXPAND | wxALL, 8);
	SetSizer(main);

	wxDir::Make(ConfigPathsConfigsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	wxDir::Make(ConfigPathsMachinesDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	EnsureDefaultConfig();
	RefreshConfigList();

	new_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnNew, this);
	edit_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnEdit, this);
	clone_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnClone, this);
	delete_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnDelete, this);
	start_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnStart, this);
	resume_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnResume, this);
	load_state_button_->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnLoadStateFile, this);
	cancel_button->Bind(wxEVT_BUTTON, &ConfigSelectorDialog::OnCancel, this);
	config_list_->Bind(wxEVT_LISTBOX_DCLICK, &ConfigSelectorDialog::OnListDoubleClick, this);
	config_list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { UpdateButtons(); });
}

void ConfigSelectorDialog::EnsureDefaultConfig()
{
	const wxString default_cfg = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + "Default.cfg";
	if (wxFileExists(default_cfg)) {
		return;
	}
	wxFileConfig settings(wxEmptyString, wxEmptyString, default_cfg, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);
	settings.Write("name", "Default");
	settings.Write("model", "RPCSA");
	settings.Write("mem_size", "64");
	settings.Write("vram_size", "2");
	settings.Write("sound_enabled", 1L);
	settings.Write("refresh_rate", 60L);
	settings.Write("network_type", "nat");
	settings.Flush();
	ConfigPathsCreateMachineDirectory("Default");
}

void ConfigSelectorDialog::RefreshConfigList()
{
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
		config_entries_.push_back(entry);
		config_list_->Append(name);
	}

	if (config_list_->GetCount() > 0) {
		config_list_->SetSelection(0);
	}
	UpdateButtons();
}

void ConfigSelectorDialog::UpdateButtons()
{
	const bool has_selection = config_list_->GetSelection() != wxNOT_FOUND;
	edit_button_->Enable(has_selection);
	clone_button_->Enable(has_selection);
	delete_button_->Enable(has_selection && config_list_->GetCount() > 1);
	start_button_->Enable(has_selection);

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

void ConfigSelectorDialog::OnNew(wxCommandEvent &)
{
	wxTextEntryDialog dlg(this, "Machine name:", "New Machine", "New Machine");
	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	const wxString sanitized = ConfigPathsSanitizeName(dlg.GetValue());
	if (!ConfigPathsIsNameUnique(sanitized)) {
		wxMessageBox(wxString::Format("A machine named '%s' already exists.", sanitized),
		             "Name Already Exists", wxOK | wxICON_WARNING, this);
		return;
	}

	const wxString config_path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() + sanitized + ".cfg";
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);
	settings.Write("name", sanitized);
	settings.Write("model", "RPCSA");
	settings.Write("mem_size", "64");
	settings.Write("vram_size", "2");
	settings.Write("sound_enabled", 1L);
	settings.Write("refresh_rate", 60L);
	settings.Write("cdrom_enabled", 1L);
	settings.Write("network_type", "nat");
	settings.Write("cpu_idle", 0L);
	settings.Write("show_fullscreen_message", 1L);
	settings.Flush();

	if (!ConfigPathsCreateMachineDirectory(sanitized)) {
		wxRemoveFile(config_path);
		wxMessageBox("Failed to create machine directory.", "Error", wxOK | wxICON_ERROR, this);
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

	wxString config_path = path;
	if (dlg.WasRenamed()) {
		const wxString old_name = config_entries_[static_cast<size_t>(config_list_->GetSelection())].display_name;
		config_path = ConfigPathsRenameMachine(old_name, dlg.GetNewName(), path);
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
	wxString cmd = wxString::Format("rm -rf '%s'", machine_dir);
	system(cmd.utf8_str().data());
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
	if (wxDirExists(src_machine)) {
		wxString cmd = wxString::Format("cp -a '%s' '%s'", src_machine, dst_machine);
		system(cmd.utf8_str().data());
	} else {
		ConfigPathsCreateMachineDirectory(sanitized);
	}

	RefreshConfigList();
}
