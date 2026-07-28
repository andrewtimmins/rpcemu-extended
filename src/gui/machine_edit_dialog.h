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

#ifndef MACHINE_EDIT_DIALOG_H
#define MACHINE_EDIT_DIALOG_H

#include <wx/wx.h>

#include <map>
#include <vector>

class wxFileConfig;

extern "C" {
#include "rpcemu.h"
}

class MachineEditDialog : public wxDialog {
public:
	MachineEditDialog(wxWindow *parent, const wxString &config_path, bool allow_rename = true,
	                  bool emulator_running = false);

	wxString GetNewName() const { return new_name_; }
	bool WasRenamed() const { return renamed_; }

private:
	enum class HardDiscState {
		Missing,
		Empty,
		Ready,
		Blocked,
		CustomPath,
	};

	struct HardDiscInfo {
		HardDiscState state = HardDiscState::Missing;
		wxString path;
		wxString size_text;
		wxString modified_text;
		bool uses_custom_path = false;
	};

	struct HardDiscPanel {
		wxStaticText *badge = nullptr;
		wxStaticText *path_label = nullptr;
		wxStaticText *modified_label = nullptr;
		wxButton *create_btn = nullptr;
		wxButton *delete_btn = nullptr;
		wxButton *open_folder_btn = nullptr;
		int drive_num = 0;
	};

	void BuildUi();
	wxWindow *BuildSystemPage(wxWindow *parent);
	wxWindow *BuildOptionsPage(wxWindow *parent);
	wxWindow *BuildNetworkPage(wxWindow *parent);
	wxWindow *BuildDrivesPage(wxWindow *parent);
	wxWindow *BuildPodulesPage(wxWindow *parent);
	void BuildHardDiscPanel(wxWindow *parent, wxSizer *parent_sizer, HardDiscPanel &panel, int drive_num,
	                        int ide_index);
	wxSizer *BuildPoduleSection(wxWindow *parent);
	void LoadPoduleSettings(wxFileConfig &settings);
	void SavePoduleSettings(wxFileConfig &settings);
	void RebuildPoduleChoices();
	void OnPoduleChanged(wxCommandEvent &event);
	void OnPoduleConfigure(int slot);
	void LoadSettings();
	void SaveSettings();
	void ApplySavedSettingsToGlobalConfig(const wxString &rom_dir, int mem_size, int vram_internal,
	                                      int refresh, NetworkType network_type);
	void PopulateRomList();
	void UpdateRomModelCompatibility();
	void SetMemoryNote(const char *text);
	void GrowToFitContents();
	void UpdateHdStatus();
	void ApplyHardDiscPanel(HardDiscPanel &panel, const HardDiscInfo &info);
	HardDiscInfo QueryHardDiscInfo(int drive) const;
	wxString CurrentMachineNameForHd() const;
	wxString HardDiscFilePath(int drive) const;
	void CreateHardDisc(int drive, int size_mb);
	void DeleteHardDisc(int drive);
	void OpenHardDiscFolder(int drive);
	void ShowHardDiscCreateMenu(int drive);
	Model CurrentModelSelection() const;

	/* Which Model each row of model_combo_ stands for. The combo does not list
	   every model, so its selection index is not the enum value. */
	std::vector<Model> model_choices_;
	void PopulateModelList(Model keep_selectable);
	void SelectModel(Model model);

	void OnOk(wxCommandEvent &event);
	void OnNetworkChanged(wxCommandEvent &event);
	void OnRomOrModelChanged(wxCommandEvent &event);
	void OnGetRiscos(wxCommandEvent &event);
	void UpdateDiscDownloadAvailability();
	void OnNameChanged(wxCommandEvent &event);
	wxString SelectedRomDir() const;
	void SetRomSelection(const wxString &rom_dir);

	wxString config_path_;
	wxString original_name_;
	wxString new_name_;
	wxString hd4_path_;
	bool renamed_ = false;
	bool allow_rename_ = true;
	bool loading_settings_ = false;
	bool emulator_running_ = false;
	bool cdrom_enabled_ = false;

	wxTextCtrl *name_edit_ = nullptr;
	wxComboBox *rom_combo_ = nullptr;
	wxButton *get_rom_button_ = nullptr;
	wxCheckBox *get_disc_check_ = nullptr;
	wxStaticText *get_disc_note_ = nullptr;

	/* The System page, kept so a note that grows can have its page laid out
	   again; inside a notebook the dialogue's own Layout() does not reach it. */
	wxWindow *system_page_ = nullptr;

	/* Options page. Each maps to one per-machine configuration field, except
	   default_machine_check_ which is a host preference. */
	wxCheckBox *fullscreen_msg_check_ = nullptr;
	wxCheckBox *integer_scaling_check_ = nullptr;
	wxCheckBox *fit_to_window_check_ = nullptr;
	wxCheckBox *follow_host_check_ = nullptr;
	wxCheckBox *sound_check_ = nullptr;
	wxCheckBox *cdrom_check_ = nullptr;
	wxCheckBox *mouse_twobutton_check_ = nullptr;
	wxCheckBox *cpu_idle_check_ = nullptr;
	wxCheckBox *suspend_on_exit_check_ = nullptr;
	wxCheckBox *default_machine_check_ = nullptr;
	wxCheckBox *vnc_check_ = nullptr;
	wxCheckBox *clipboard_check_ = nullptr;
	wxComboBox *model_combo_ = nullptr;
	wxComboBox *mem_combo_ = nullptr;
	wxComboBox *vram_combo_ = nullptr;
	wxCheckBox *gfxcard_check_ = nullptr;
	wxCheckBox *gfxcard_boot_check_ = nullptr;
	wxCheckBox *fullscreen_check_ = nullptr;
	wxSlider *refresh_slider_ = nullptr;
	wxStaticText *refresh_label_ = nullptr;
	wxComboBox *network_combo_ = nullptr;
	wxTextCtrl *bridge_edit_ = nullptr;
	wxStaticText *bridge_label_ = nullptr;
	wxTextCtrl *tunnel_edit_ = nullptr;
	wxStaticText *tunnel_label_ = nullptr;
	wxStaticText *mem_note_ = nullptr;
	wxStaticText *compat_label_ = nullptr;
	wxStaticText *hd_reset_note_ = nullptr;
	HardDiscPanel hd4_panel_;
	HardDiscPanel hd5_panel_;

	/* Podule slot selection. podule_selection_[i] is the chosen podule
	   short_name for slot i ("" == none); podule_item_names_[i][k] maps choice
	   k of combo i back to a short_name (lists differ per combo because a
	   podule already used elsewhere is hidden). */
	std::vector<wxChoice *> podule_combos_;
	std::vector<wxButton *> podule_config_btns_;
	std::vector<wxString> podule_selection_;
	std::vector<std::vector<wxString>> podule_item_names_;
	std::vector<std::pair<wxString, wxString>> podule_available_; /* short_name, display */
	/* Per-podule config, keyed by section "<short_name>.<slot>" -> key -> value.
	   Loaded from / saved to this machine's .cfg [PoduleConfig] groups. */
	std::map<wxString, std::map<wxString, wxString>> podule_kv_;
};

#endif
