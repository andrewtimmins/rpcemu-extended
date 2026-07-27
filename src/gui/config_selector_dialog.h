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

#ifndef CONFIG_SELECTOR_DIALOG_H
#define CONFIG_SELECTOR_DIALOG_H

#include <vector>

#include <wx/wx.h>

struct ConfigEntry {
	wxString display_name;
	wxString config_path;
};

class ConfigSelectorDialog : public wxDialog {
public:
	ConfigSelectorDialog(wxWindow *parent);

	wxString GetSelectedConfigPath() const { return selected_config_path_; }

	/* True if the user chose Resume / Load State (load a snapshot) rather than
	   Start/Restart (cold boot). */
	bool ShouldResume() const { return resume_selected_; }

	/* The snapshot file to load when ShouldResume() is true. For Resume this is
	   the selected machine's own snapshot; for Load State it is the file the
	   user picked (whose machine determines which config is booted). */
	wxString GetStateFileToLoad() const { return state_file_to_load_; }

private:
	void EnsureDefaultConfig();
	void RefreshConfigList();
	void UpdateButtons();
	wxString SelectedConfigPath() const;

	void OnStart(wxCommandEvent &event);
	void OnResume(wxCommandEvent &event);
	void OnLoadStateFile(wxCommandEvent &event);
	void OnCancel(wxCommandEvent &event);
	void OnToggleDefault(wxCommandEvent &event);
	wxString SelectedMachineName() const;
	void OnListDoubleClick(wxCommandEvent &event);
	void OnNew(wxCommandEvent &event);
	void OnEdit(wxCommandEvent &event);
	void OnDelete(wxCommandEvent &event);
	void OnClone(wxCommandEvent &event);

	wxListBox *config_list_ = nullptr;
	wxButton *new_button_ = nullptr;
	wxButton *edit_button_ = nullptr;
	wxButton *clone_button_ = nullptr;
	wxButton *delete_button_ = nullptr;
	wxButton *start_button_ = nullptr;
	wxButton *resume_button_ = nullptr;
	wxButton *load_state_button_ = nullptr;
	wxButton *default_button_ = nullptr;
	wxString selected_config_path_;
	wxString state_file_to_load_;
	bool resume_selected_ = false;
	std::vector<ConfigEntry> config_entries_;
};

#endif
