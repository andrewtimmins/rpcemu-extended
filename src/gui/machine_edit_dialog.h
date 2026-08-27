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
#include <wx/spinctrl.h>
#include <wx/radiobut.h>

#include <functional>

#include <map>
#include <memory>
#include <utility>
#include <vector>

class wxFileConfig;
class wxNotebook;

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
	void UpdateJsonNetEnabled();

	/*
	 * The Community Network's tick box, which is not just a tick box: turning it
	 * on asks the user to accept what it means first, and puts the box back if
	 * they decline. Answers whether it is now on.
	 */
	bool ConfirmCommunityNetwork();
	void OnMacAddressText(wxCommandEvent &event);
	wxString SelectedMacAddress() const;
	wxWindow *BuildDrivesPage(wxWindow *parent);
	wxWindow *BuildPodulesPage(wxWindow *parent);
	wxWindow *BuildCoProcessorPage(wxWindow *parent);
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
	void UpdateGfxCardAvailability();
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
	void OnGetHardDisc(wxCommandEvent &event);
	void UpdateDiscDownloadAvailability();
	void OnNameChanged(wxCommandEvent &event);
	wxString SelectedRomDir() const;
	void SetRomSelection(const wxString &rom_dir);

	wxString config_path_;
	wxString original_name_;
	wxString new_name_;
	wxString hd4_path_;
	/* HostFS folder (discussion #77). Empty means this machine's own folder,
	   which is what everything before this used. See hostfs_path.h. */
	wxTextCtrl *hostfs_edit_ = nullptr;
	wxButton *hostfs_browse_ = nullptr;
	wxStaticText *hostfs_note_ = nullptr;

	wxString ResolvedHostfsRoot() const;
	wxString ResolveHostfsValue(const wxString &configured) const;
	/* Returns false when the user backed out and nothing should be saved. */
	bool OfferToBringHostfsFilesAcross();
	void UpdateHostfsNote();
	bool renamed_ = false;
	bool allow_rename_ = true;
	bool loading_settings_ = false;
	bool emulator_running_ = false;
	/* The hostfs_path as loaded, so a change can be spotted on OK and the files
	   offered a lift to the new folder. */
	wxString original_hostfs_;
	bool cdrom_enabled_ = false;

	wxTextCtrl *name_edit_ = nullptr;
	wxComboBox *rom_combo_ = nullptr;
	wxButton *get_rom_button_ = nullptr;
	wxButton *get_disc_button_ = nullptr;
	wxStaticText *get_disc_note_ = nullptr;

	/* The System page, kept so a note that grows can have its page laid out
	   again; inside a notebook the dialogue's own Layout() does not reach it. */
	wxWindow *system_page_ = nullptr;

	/* Options page. Each maps to one per-machine configuration field, except
	   default_machine_check_ which is a host preference. */
	wxCheckBox *fullscreen_msg_check_ = nullptr;
	/* The two display choices, mirroring the Settings menu. scaling_radio_ is
	   indexed by DisplayScaling, so a configured value selects directly. */
	wxRadioButton *scaling_radio_[2] = { nullptr, nullptr };
	wxChoice *fixed_mode_choice_ = nullptr;

	/* The modes fixed_mode_choice_ is currently offering, in its own order. */
	std::vector<std::pair<unsigned, unsigned>> fixed_modes_;

	/** Refill fixed_mode_choice_ for the VRAM and graphics card now selected. */
	void RebuildFixedModeChoice();

	/** Display memory the pending VRAM / graphics card selection would give. */
	size_t PendingDisplayMemory() const;

	/** Point fixed_mode_choice_ at a mode, or at the largest if it is not there. */
	void SelectFixedMode(unsigned width, unsigned height);

	/** Which drawing rule the radio group has selected (a DisplayScaling). */
	int SelectedDisplayScaling() const;

	/**
	 * The screen size chosen from the list, or (0, 0) if the list is empty -
	 * which only happens if the machine's display memory cannot hold even the
	 * smallest standard mode. Zero means "choose one for this display", which is
	 * what MainFrame::StartEmulator() does with it.
	 */
	void SelectedScreenSize(unsigned *width, unsigned *height) const;
	wxCheckBox *sound_check_ = nullptr;
	wxCheckBox *cdrom_check_ = nullptr;
	wxCheckBox *mouse_twobutton_check_ = nullptr;
	wxCheckBox *cpu_idle_check_ = nullptr;
	wxCheckBox *suspend_on_exit_check_ = nullptr;
	wxCheckBox *default_machine_check_ = nullptr;
	wxCheckBox *vnc_check_ = nullptr;
	wxSpinCtrl *vnc_port_spin_ = nullptr;
	wxTextCtrl *vnc_password_text_ = nullptr;
	wxTextCtrl *vnc_password_readonly_text_ = nullptr;
	wxCheckBox *hostcmd_check_ = nullptr;
	wxTextCtrl *hostcmd_socket_text_ = nullptr;
	/* Called after the values are loaded, so the fields start greyed out to match
	   their checkbox rather than only doing so once it is clicked. */
	std::function<void()> vnc_fields_follow_;
	std::function<void()> hostcmd_fields_follow_;
	wxCheckBox *clipboard_check_ = nullptr;
	wxComboBox *model_combo_ = nullptr;
	wxComboBox *mem_combo_ = nullptr;
	wxComboBox *vram_combo_ = nullptr;
	wxCheckBox *gfxcard_check_ = nullptr;
	wxCheckBox *accelerators_check_ = nullptr;
	wxCheckBox *gfxcard_boot_check_ = nullptr;

	/* The tooltips these two normally carry. Kept because they are replaced by
	   the reason the card is unavailable when the chosen ROM cannot drive it. */
	wxString gfxcard_tooltip_;
	wxString gfxcard_boot_tooltip_;
	wxCheckBox *fullscreen_check_ = nullptr;
	wxSlider *refresh_slider_ = nullptr;
	wxStaticText *refresh_label_ = nullptr;
	wxComboBox *network_combo_ = nullptr;
	wxTextCtrl *mac_address_edit_ = nullptr;
	/* Set while the field is being rewritten, so the change that causes does
	   not come back round as another edit to correct. */
	bool in_mac_address_update_ = false;
	/* JSON networking: the tun/tap server this machine joins. */
	wxCheckBox *json_net_check_ = nullptr;
	wxStaticText *json_net_host_label_ = nullptr;
	wxTextCtrl *json_net_host_edit_ = nullptr;
	wxStaticText *json_net_port_label_ = nullptr;
	wxSpinCtrl *json_net_port_edit_ = nullptr;
	wxCheckBox *community_net_check_ = nullptr;

	/*
	 * The explanatory paragraphs on these pages.
	 *
	 * Each one used to carry its own wrap width written into the call that
	 * made it - 440, 460, 470 and 500 across the dialogue - and several a
	 * smaller font as well. Every one of those widths is narrower than the
	 * dialogue, which is sized by its widest page, so the text stopped short
	 * of the right-hand edge and read as truncated; and the mixed sizes made
	 * one window look like three. They go through MakeNote() now, which uses
	 * the page's own font and re-wraps each paragraph to whatever width the
	 * sizer gives it, so there is no width here to be wrong at another size
	 * or under another platform's default font.
	 *
	 * The text is kept because wxStaticText::Wrap() inserts the line breaks
	 * into the label itself: re-wrapping wider has to start from the original
	 * string, or the old breaks stay in.
	 */
	struct Note {
		wxStaticText *label = nullptr;
		wxString text;
		int wrapped_at = 0;
	};
	std::vector<std::shared_ptr<Note>> notes_;

	wxStaticText *MakeNote(wxWindow *parent, const wxString &text = wxEmptyString);
	void SetNoteText(wxStaticText *label, const wxString &text);
	void WrapNotesToPageWidth();
	int UsableNoteWidth(wxStaticText *label) const;

	/* Kept so the paragraphs can be measured against the page area. */
	wxNotebook *notebook_ = nullptr;

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
	wxChoice *copro_choice_ = nullptr;
	wxChoice *copro_ram_choice_ = nullptr;
	wxStaticText *copro_ram_label_ = nullptr;
	/* The sizes currently offered, in KB, in the order the choice lists them.
	   Rebuilt when the core changes, because what a processor can address is
	   what decides them. */
	std::vector<unsigned> copro_ram_sizes_;
	void RebuildCoProcessorRamChoices(unsigned keep_kb);
	std::vector<wxButton *> podule_config_btns_;
	std::vector<wxString> podule_selection_;
	std::vector<std::vector<wxString>> podule_item_names_;
	std::vector<std::pair<wxString, wxString>> podule_available_; /* short_name, display */
	/* Per-podule config, keyed by section "<short_name>.<slot>" -> key -> value.
	   Loaded from / saved to this machine's .cfg [PoduleConfig] groups. */
	std::map<wxString, std::map<wxString, wxString>> podule_kv_;
};

#endif
