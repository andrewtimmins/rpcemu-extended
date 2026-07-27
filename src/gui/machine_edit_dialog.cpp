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

#include "machine_edit_dialog.h"

#include "config_paths.h"
#include "podule_config_dialog.h"

#include <cstring>

#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/utils.h>

extern "C" {
#include "romload.h"
#include "rpcemu.h"
#include "podules.h"
#include "podule_config.h"
}

namespace {

const wxColour kHdColourMissing(120, 120, 120);
const wxColour kHdColourEmpty(180, 120, 0);
const wxColour kHdColourReady(27, 94, 32);
const wxColour kHdColourBlocked(120, 120, 120);
const wxColour kHdColourMuted(100, 100, 100);

wxString FormatHardDiscSize(wxULongLong size_bytes)
{
	if (size_bytes == 0) {
		return "0 bytes";
	}

	const double mb = size_bytes.ToDouble() / (1024.0 * 1024.0);
	if (mb < 1024.0) {
		return wxString::Format("%.1f MB", mb);
	}
	return wxString::Format("%.2f GB", mb / 1024.0);
}

wxString TruncatePathMiddle(const wxString &path, size_t max_len = 58)
{
	if (path.length() <= max_len) {
		return path;
	}

	const size_t keep = max_len - 3;
	const size_t front = keep / 2;
	const size_t back = keep - front;
	return path.substr(0, front) + "..." + path.substr(path.length() - back);
}

wxString FormatModifiedTime(const wxString &path)
{
	wxFileName file(path);
	if (!file.FileExists()) {
		return wxEmptyString;
	}

	const wxDateTime modified = file.GetModificationTime();
	if (!modified.IsValid()) {
		return wxEmptyString;
	}

	return modified.Format("Modified: %d %b %Y, %H:%M");
}

} // namespace

enum {
	ID_MACHINE_EDIT_OK = wxID_HIGHEST + 100,
	ID_HD_CREATE_256_MB,
	ID_HD_CREATE_512_MB,
	ID_HD_CREATE_1_GB,
	ID_HD_CREATE_2_GB,
};

MachineEditDialog::MachineEditDialog(wxWindow *parent, const wxString &config_path, bool allow_rename,
                                     bool emulator_running)
	: wxDialog(parent, wxID_ANY, "Edit Machine", wxDefaultPosition, wxSize(560, 620),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, config_path_(config_path)
	, allow_rename_(allow_rename)
	, emulator_running_(emulator_running)
{
	BuildUi();
	LoadSettings();
	UpdateHdStatus();
	Fit();
	CentreOnParent();
}

void MachineEditDialog::BuildHardDiscPanel(wxWindow *parent, wxSizer *parent_sizer, HardDiscPanel &panel,
                                           int drive_num, int ide_index)
{
	panel.drive_num = drive_num;

	auto *drive_panel = new wxPanel(parent);
	auto *card = new wxBoxSizer(wxVERTICAL);

	card->Add(new wxStaticText(drive_panel, wxID_ANY,
	                           wxString::Format("HardDisc %d (IDE drive %d)", drive_num, ide_index)),
	          0, wxBOTTOM, 4);

	panel.badge = new wxStaticText(drive_panel, wxID_ANY, "Not created");
	panel.badge->SetFont(panel.badge->GetFont().Bold());
	card->Add(panel.badge, 0, wxEXPAND | wxBOTTOM, 4);

	panel.path_label = new wxStaticText(drive_panel, wxID_ANY, wxEmptyString);
	panel.path_label->SetForegroundColour(kHdColourMuted);
	card->Add(panel.path_label, 0, wxEXPAND | wxBOTTOM, 2);

	panel.modified_label = new wxStaticText(drive_panel, wxID_ANY, wxEmptyString);
	panel.modified_label->SetForegroundColour(kHdColourMuted);
	panel.modified_label->SetFont(panel.modified_label->GetFont().Smaller());
	card->Add(panel.modified_label, 0, wxEXPAND | wxBOTTOM, 8);

	auto *actions = new wxBoxSizer(wxHORIZONTAL);
	panel.create_btn = new wxButton(drive_panel, wxID_ANY, "New disc...");
	panel.delete_btn = new wxButton(drive_panel, wxID_ANY, "Delete");
	panel.open_folder_btn = new wxButton(drive_panel, wxID_ANY, "Open folder");
	actions->Add(panel.create_btn, 0, wxRIGHT, 4);
	actions->Add(panel.delete_btn, 0, wxRIGHT, 4);
	actions->Add(panel.open_folder_btn, 0);
	card->Add(actions, 0, wxEXPAND);

	drive_panel->SetSizer(card);
	parent_sizer->Add(drive_panel, 0, wxEXPAND | wxBOTTOM, 8);

	panel.create_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { ShowHardDiscCreateMenu(drive_num); });
	panel.delete_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { DeleteHardDisc(drive_num); });
	panel.open_folder_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { OpenHardDiscFolder(drive_num); });
}

void MachineEditDialog::BuildUi()
{
	name_edit_ = new wxTextCtrl(this, wxID_ANY);
	rom_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	model_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	mem_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	vram_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	refresh_slider_ = new wxSlider(this, wxID_ANY, 60, 20, 100);
	refresh_label_ = new wxStaticText(this, wxID_ANY, "60 Hz");
	network_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	bridge_label_ = new wxStaticText(this, wxID_ANY, "Bridge Name:");
	bridge_edit_ = new wxTextCtrl(this, wxID_ANY, "rpcemu");
	tunnel_label_ = new wxStaticText(this, wxID_ANY, "IP Address:");
	tunnel_edit_ = new wxTextCtrl(this, wxID_ANY, "172.31.0.1");
	compat_label_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
	compat_label_->SetMinSize(wxSize(460, -1));

	/* Why the RAM/VRAM selectors are fixed on some models. Without this the
	   greyed controls look like a fault rather than the shape of the machine
	   (reported as issue #37). */
	mem_note_ = new wxStaticText(this, wxID_ANY, wxEmptyString);

	PopulateRomList();

	PopulateModelList(Model_MAX);

	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	for (int mem : mem_values) {
		mem_combo_->Append(wxString::Format("%d MB", mem));
	}
	vram_combo_->Append("None");
	vram_combo_->Append("2 MB");
	vram_combo_->Append("4 MB");   /* Phoebe's fixed VRAM */
	vram_combo_->Append("8 MB");
	vram_combo_->Append("16 MB");

	/* The graphics card carries its own display memory, so it is the answer to
	   the VRAM limit rather than another size of it - hence its place here. */
	gfxcard_check_ = new wxCheckBox(this, wxID_ANY, "High-Resolution Graphics Card");
	gfxcard_boot_check_ = new wxCheckBox(this, wxID_ANY, "Make High-Resolution Graphics Card the default display");
	fullscreen_check_ = new wxCheckBox(this, wxID_ANY, "Start this machine full screen");
	fullscreen_check_->SetToolTip(
	    "Go full screen as soon as this machine starts, rather than opening a "
	    "window first. Press Ctrl+End or use Settings > Full Screen to leave it.");
	gfxcard_boot_check_->SetToolTip(
	    "Hand the display to the card as the machine boots, so RISC OS comes up on "
	    "it rather than on VIDC20 - no *GfxCardOn needed.\n\n"
	    "The driver also selects the EDID monitor type for the session if the "
	    "configured one would not offer the card's modes. Your configuration is "
	    "left as it is.");
	gfxcard_check_->SetToolTip(
	    "Fit an expansion card with 15MB of its own display memory, for modes the "
	    "fitted VRAM cannot reach (up to 2560 x 1440 in full colour).\n\n"
	    "RISC OS keeps using VIDC20 until you run *GfxCardOn. Needs RISC OS 5.");

	network_combo_->Append("Off");
	network_combo_->Append("NAT");
	network_combo_->Append("Ethernet Bridging");
	network_combo_->Append("IP Tunnelling");

	auto *form = new wxFlexGridSizer(2, 8, 8);
	form->AddGrowableCol(1, 1);
	form->Add(new wxStaticText(this, wxID_ANY, "Name:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(name_edit_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "ROM:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(rom_combo_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "Model:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(model_combo_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "RAM:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(mem_combo_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "VRAM:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(vram_combo_, 1, wxEXPAND);

	/* The note sits in the same column as the checkboxes and the ROM message
	   below it, so the three read as one block rather than the note starting
	   somewhere of its own. */
	form->Add(new wxStaticText(this, wxID_ANY, ""), 0);
	form->Add(mem_note_, 1, wxEXPAND);

	/* "Make it the default display" belongs beside the card it applies to, not
	   on a line of its own where it reads as a separate setting. */
	auto *gfxcard_row = new wxBoxSizer(wxHORIZONTAL);
	gfxcard_row->Add(gfxcard_check_, 0, wxALIGN_CENTER_VERTICAL);
	gfxcard_row->Add(gfxcard_boot_check_, 0,
	                 wxALIGN_CENTER_VERTICAL | wxLEFT, 16);
	form->Add(new wxStaticText(this, wxID_ANY, ""), 0);
	form->Add(gfxcard_row, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, ""), 0);
	form->Add(fullscreen_check_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, ""), 0);
	form->Add(compat_label_, 1, wxEXPAND);
	auto *refresh_row = new wxBoxSizer(wxHORIZONTAL);
	refresh_row->Add(refresh_slider_, 1, wxEXPAND | wxRIGHT, 8);
	refresh_row->Add(refresh_label_, 0, wxALIGN_CENTER_VERTICAL);
	form->Add(new wxStaticText(this, wxID_ANY, "Refresh Rate:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(refresh_row, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "Network:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(network_combo_, 1, wxEXPAND);
	form->Add(bridge_label_, 0, wxALIGN_CENTER_VERTICAL);
	form->Add(bridge_edit_, 1, wxEXPAND);
	form->Add(tunnel_label_, 0, wxALIGN_CENTER_VERTICAL);
	form->Add(tunnel_edit_, 1, wxEXPAND);

	auto *hd_box = new wxStaticBoxSizer(wxVERTICAL, this, "Hard Discs");
	wxWindow *const hd_parent = hd_box->GetStaticBox();
	BuildHardDiscPanel(hd_parent, hd_box, hd4_panel_, 4, 0);
	BuildHardDiscPanel(hd_parent, hd_box, hd5_panel_, 5, 1);

	hd_reset_note_ = new wxStaticText(hd_parent, wxID_ANY,
	                                  "Changes to hard discs take effect after emulator reset.");
	hd_reset_note_->SetForegroundColour(kHdColourMuted);
	hd_reset_note_->SetFont(hd_reset_note_->GetFont().Smaller());
	hd_box->Add(hd_reset_note_, 0, wxEXPAND | wxTOP, 6);

	auto *button_row = new wxBoxSizer(wxHORIZONTAL);
	button_row->AddStretchSpacer();
	auto *ok_button = new wxButton(this, ID_MACHINE_EDIT_OK, "OK");
	auto *cancel_button = new wxButton(this, wxID_CANCEL, "Cancel");
	ok_button->SetDefault();
	button_row->Add(ok_button, 0, wxRIGHT, 4);
	button_row->Add(cancel_button, 0);

	wxSizer *podule_box = BuildPoduleSection(this);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(form, 0, wxEXPAND | wxALL, 10);
	main->Add(hd_box, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
	main->Add(podule_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
	main->Add(button_row, 0, wxEXPAND | wxALL, 10);
	SetSizer(main);

	refresh_slider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
		refresh_label_->SetLabel(wxString::Format("%d Hz", refresh_slider_->GetValue()));
	});
	gfxcard_check_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
		gfxcard_boot_check_->Enable(gfxcard_check_->GetValue());
		event.Skip();
	});
	network_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnNetworkChanged, this);
	rom_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnRomOrModelChanged, this);
	model_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnRomOrModelChanged, this);
	/* 512MB RAM is Kinetic-only: if any other model is selected, snap a 512MB
	   choice back to 256MB immediately so it can't be left selected. */
	mem_combo_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) {
		if (CurrentModelSelection() != Model_Kinetic && mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 256 MB */
		}
		event.Skip();
	});
	name_edit_->Bind(wxEVT_TEXT, &MachineEditDialog::OnNameChanged, this);
	ok_button->Bind(wxEVT_BUTTON, &MachineEditDialog::OnOk, this);
	cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

	hd_reset_note_->Show(emulator_running_);
}

void MachineEditDialog::PopulateRomList()
{
	rom_combo_->Clear();
	const wxString roms_dir = ConfigPathsRomsDir();
	if (!wxDirExists(roms_dir)) {
		rom_combo_->Append("(No roms/ directory found)");
		return;
	}

	rom_combo_->Append("(All files in roms/)", new wxStringClientData(wxEmptyString));

	wxDir dir(roms_dir);
	wxString entry;
	bool has_entries = dir.GetFirst(&entry, wxEmptyString, wxDIR_DIRS);
	while (has_entries) {
		if (entry != "." && entry != "..") {
			rom_combo_->Append(entry + "/", new wxStringClientData(entry));
		}
		has_entries = dir.GetNext(&entry);
	}

	has_entries = dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES);
	while (has_entries) {
		if (!entry.StartsWith(".") && !entry.Lower().EndsWith(".txt")) {
			rom_combo_->Append(entry, new wxStringClientData(entry));
		}
		has_entries = dir.GetNext(&entry);
	}
}

wxString MachineEditDialog::SelectedRomDir() const
{
	const int sel = rom_combo_->GetSelection();
	if (sel < 0) {
		return wxEmptyString;
	}

	const wxString label = rom_combo_->GetString(sel);
	if (label.StartsWith("(")) {
		return wxEmptyString;
	}

	auto *data = dynamic_cast<wxStringClientData *>(rom_combo_->GetClientObject(sel));
	if (data != nullptr) {
		return data->GetData();
	}

	wxString rom_dir = label;
	if (rom_dir.EndsWith("/")) {
		rom_dir.RemoveLast();
	}
	return rom_dir;
}

void MachineEditDialog::SetRomSelection(const wxString &rom_dir)
{
	for (unsigned i = 0; i < rom_combo_->GetCount(); ++i) {
		auto *data = dynamic_cast<wxStringClientData *>(rom_combo_->GetClientObject(i));
		if (data != nullptr && data->GetData() == rom_dir) {
			rom_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}
	rom_combo_->SetSelection(0);
}

/**
 * Fill the model list.
 *
 * Two models are left out: Phoebe, the machine Acorn never shipped, and the
 * ARM810, a processor card that never reached the shops either. Both are still
 * implemented and still work if a configuration file names one - this only
 * decides what is offered in the list, so nobody picks a machine that never
 * existed while looking for a Risc PC.
 *
 * A machine already configured for one of them keeps it: pass its model as
 * keep_selectable and it is listed for as long as that dialogue is open, so
 * opening the settings of such a machine and pressing OK does not quietly turn
 * it into something else.
 *
 * @param keep_selectable A model to list even if it is normally hidden,
 *                        or Model_MAX for none
 */
void MachineEditDialog::PopulateModelList(Model keep_selectable)
{
	static const Model hidden[] = { Model_Phoebe, Model_RPCARM810 };

	model_combo_->Clear();
	model_choices_.clear();

	for (int i = 0; i < Model_MAX; ++i) {
		const Model model = static_cast<Model>(i);
		bool skip = false;

		for (Model h : hidden) {
			if (model == h && model != keep_selectable) {
				skip = true;
			}
		}
		if (skip) {
			continue;
		}
		model_combo_->Append(wxString::FromUTF8(models[i].name_gui));
		model_choices_.push_back(model);
	}
}

/** Select a model by value, listing it first if it is one of the hidden ones. */
void MachineEditDialog::SelectModel(Model model)
{
	for (size_t i = 0; i < model_choices_.size(); ++i) {
		if (model_choices_[i] == model) {
			model_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}

	/* Not listed, so this machine is configured for one of the hidden models.
	   Put it in rather than leaving the list on something else. */
	PopulateModelList(model);
	for (size_t i = 0; i < model_choices_.size(); ++i) {
		if (model_choices_[i] == model) {
			model_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}
}

Model MachineEditDialog::CurrentModelSelection() const
{
	const int sel = model_combo_->GetSelection();

	if (sel >= 0 && static_cast<size_t>(sel) < model_choices_.size()) {
		return model_choices_[static_cast<size_t>(sel)];
	}
	return Model_RPCARM710;
}

/* Show (or clear) the note under the VRAM selector, and let the dialog resize
   around it. Wrapping is done here because wxStaticText will not do it itself. */
/* Roughly the label column plus the dialogue's margins: what the note's own
   column is narrower than the window by. */
static const int kNoteColumnInset = 130;

void MachineEditDialog::SetMemoryNote(const char *text)
{
	if (mem_note_ == nullptr) {
		return;
	}
	if (text == nullptr || text[0] == '\0') {
		mem_note_->SetLabel(wxEmptyString);
		mem_note_->Show(false);
	} else {
		mem_note_->SetLabel(wxString::FromUTF8(text));
		/* Wrapped to the column it sits in, worked out from the dialogue rather
		   than written down here, so a line break the text asks for is the one
		   that decides where it breaks. A fixed figure had it wrapping at about
		   half the window whatever the window was doing. */
		{
			const int width = GetClientSize().GetWidth() - kNoteColumnInset;

			mem_note_->Wrap(width > 200 ? width : 200);
		}
		mem_note_->Show(true);
	}
	Layout();
}

void MachineEditDialog::UpdateRomModelCompatibility()
{
	char detail[64] = "";
	char msg[512] = "";
	const Model model = CurrentModelSelection();

	/* Constrain the RAM/VRAM selectors to what each model actually supports,
	   mirroring the core's own clamps (settings.cpp config_load and rpcemu.c
	   config_apply) so the dialog can never present an unsupported combination.
	     mem_values[] index: 6 = 256MB, 7 = 512MB.
	     vram combo index:   0 = None, 1 = 2MB, 2 = 4MB, 3 = 8MB, 4 = 16MB. */
	switch (model) {
	case Model_Kinetic:
		/* Defined by its 512MB (two on-card SDRAM banks), so there is nothing to
		   choose. VRAM is clamped to 2MB because more than that faults on the
		   HAL physical-map path; the graphics card supersedes that work, since it
		   carries its own display memory and reaches modes no amount of VRAM
		   would offer here. */
		mem_combo_->SetSelection(7);   /* 512 MB */
		mem_combo_->Enable(false);
		vram_combo_->SetSelection(1);  /* 2 MB */
		vram_combo_->Enable(false);
		/* Broken deliberately after the first sentence: what the machine is,
		   then what to do about it. */
		SetMemoryNote("The Kinetic has 512MB of RAM and 2MB of VRAM.\n"
		              "If you wish to have high-resolution modes available, "
		              "please enable the High-Resolution Graphics Card option "
		              "below.");
		break;

	case Model_Phoebe:
		/* Phoebe (RPC2) is a fixed 256MB RAM + 4MB VRAM machine. */
		mem_combo_->SetSelection(6);   /* 256 MB */
		mem_combo_->Enable(false);
		vram_combo_->SetSelection(2);  /* 4 MB */
		vram_combo_->Enable(false);
		SetMemoryNote("Phoebe is a fixed 256MB with 4MB of VRAM.");
		break;

	case Model_A7000:
	case Model_A7000plus:
		/* The A7000 family has no VRAM at all - video runs from main DRAM.
		   RAM stays user-selectable up to the 256MB motherboard limit. */
		vram_combo_->SetSelection(0);  /* None */
		vram_combo_->Enable(false);
		if (mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 512MB is Kinetic-only */
		}
		mem_combo_->Enable(true);
		SetMemoryNote("The A7000 has no VRAM: video runs from main memory.");
		break;

	default:
		/* Risc PC models: RAM up to 256MB (512MB is Kinetic-only), VRAM free. */
		if (mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 512MB is Kinetic-only -> drop to 256MB */
		}
		mem_combo_->Enable(true);
		vram_combo_->Enable(true);
		SetMemoryNote("");
		break;
	}

	const wxString rom_dir = SelectedRomDir();
	const wxScopedCharBuffer rom_dir_utf8 = rom_dir.utf8_str();

	if (!rom_model_is_compatible(model, rom_dir_utf8.data(), msg, sizeof(msg))) {
		compat_label_->SetLabel(wxString::FromUTF8(msg));
		compat_label_->SetForegroundColour(wxColour(176, 0, 32));
		return;
	}

	const RomAddressing addressing =
		rom_probe_addressing(rom_dir_utf8.data(), detail, sizeof(detail));

	wxString label;
	if (addressing == RomAddressing_32Bit) {
		label = "32-bit ROM - OK for this model";
	} else if (addressing == RomAddressing_26Bit) {
		label = "26-bit ROM - OK for this model";
	} else {
		compat_label_->SetLabel("ROM type unknown - 26-bit CPUs need RISC OS 3.xx ROMs");
		compat_label_->SetForegroundColour(wxColour(27, 94, 32));
		return;
	}

	if (detail[0] != '\0') {
		label += wxString(" (") + wxString::FromUTF8(detail) + wxString(")");
	}

	compat_label_->SetLabel(label);
	compat_label_->SetForegroundColour(wxColour(27, 94, 32));
}

wxSizer *MachineEditDialog::BuildPoduleSection(wxWindow *parent)
{
	auto *box = new wxStaticBoxSizer(wxVERTICAL, parent, "Podules");
	wxWindow *const p = box->GetStaticBox();

	/* Snapshot the available podules once. */
	const int count = podule_get_available_count();
	for (int i = 0; i < count; i++) {
		const char *sn = podule_get_available_short_name(i);
		const char *nm = podule_get_available_name(i);
		podule_available_.emplace_back(wxString::FromUTF8(sn ? sn : ""),
		                               wxString::FromUTF8(nm ? nm : (sn ? sn : "?")));
	}

	/* Two slots per row: label, combo, configure-button (x2). */
	auto *grid = new wxFlexGridSizer(0, 6, 6, 8);
	grid->AddGrowableCol(1, 1);
	grid->AddGrowableCol(4, 1);

	for (int i = 0; i < PODULE_CONFIG_SLOTS; i++) {
		grid->Add(new wxStaticText(p, wxID_ANY, wxString::Format("Slot %d:", i)),
		          0, wxALIGN_CENTER_VERTICAL);

		auto *choice = new wxChoice(p, wxID_ANY);
		choice->Bind(wxEVT_CHOICE, &MachineEditDialog::OnPoduleChanged, this);
		grid->Add(choice, 1, wxEXPAND);

		auto *cfg_btn = new wxButton(p, wxID_ANY, wxString::FromUTF8("\xE2\x80\xA6"),
		                             wxDefaultPosition, wxSize(32, -1));
		cfg_btn->SetToolTip("Configure this podule");
		cfg_btn->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { OnPoduleConfigure(i); });
		grid->Add(cfg_btn, 0, wxALIGN_CENTER_VERTICAL);

		podule_combos_.push_back(choice);
		podule_config_btns_.push_back(cfg_btn);
		podule_selection_.push_back("");
		podule_item_names_.emplace_back();
	}
	box->Add(grid, 0, wxEXPAND | wxALL, 6);

	auto *note = new wxStaticText(p, wxID_ANY,
	    "Eight expansion-card slots. Slot 0 (Support) and the network card are "
	    "built-in. A podule can only be assigned to one slot; changes take "
	    "effect after reset.");
	note->SetForegroundColour(kHdColourMuted);
	note->SetFont(note->GetFont().Smaller());
	box->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

	RebuildPoduleChoices();
	return box;
}

/* Rebuild every slot's dropdown for the 8-slot backplane view:
    - slot 0 is the built-in RPCEmu Support ROM (locked),
    - slot 1 is the network card when networking is enabled (locked),
    - the rest are user-assignable, and a podule chosen in one slot is hidden
      from the others (one slot per podule). */
void MachineEditDialog::RebuildPoduleChoices()
{
	if (podule_combos_.empty()) {
		return;
	}

	const bool network_on = network_combo_ != nullptr && network_combo_->GetSelection() > 0;

	for (size_t i = 0; i < podule_combos_.size(); i++) {
		wxChoice *combo = podule_combos_[i];
		std::vector<wxString> &names = podule_item_names_[i];

		combo->Clear();
		names.clear();

		/* Built-in (reserved) slots: shown locked, not user-assignable. */
		wxString builtin;
		if (i == 0) {
			builtin = "RPCEmu Support (built-in)";
		} else if (i == 1 && network_on) {
			builtin = "RPCEmu Ethernet (built-in)";
		}
		if (!builtin.IsEmpty()) {
			combo->Append(builtin);
			names.push_back("");
			combo->SetSelection(0);
			combo->Disable();
			if (i < podule_config_btns_.size()) {
				podule_config_btns_[i]->Enable(false);
			}
			podule_selection_[i] = ""; /* never a user podule */
			continue;
		}

		combo->Enable();
		combo->Append("(None)");
		names.push_back("");

		for (const auto &pa : podule_available_) {
			const wxString &short_name = pa.first;

			bool used_elsewhere = false;
			for (size_t j = 0; j < podule_selection_.size(); j++) {
				if (j != i && podule_selection_[j] == short_name) {
					used_elsewhere = true;
					break;
				}
			}
			if (used_elsewhere) {
				continue;
			}

			combo->Append(pa.second);
			names.push_back(short_name);
		}

		int sel = 0;
		for (size_t k = 0; k < names.size(); k++) {
			if (names[k] == podule_selection_[i]) {
				sel = static_cast<int>(k);
				break;
			}
		}
		combo->SetSelection(sel);

		/* The configure button is active only when the slot holds a podule
		   that exposes a configuration schema. */
		if (i < podule_config_btns_.size()) {
			bool has_config = false;
			if (!podule_selection_[i].IsEmpty()) {
				const podule_header_t *h =
				    podule_find(podule_selection_[i].utf8_str().data());
				has_config = (h != nullptr && h->config != nullptr);
			}
			podule_config_btns_[i]->Enable(has_config);
		}
	}
}

void MachineEditDialog::OnPoduleConfigure(int slot)
{
	if (slot < 0 || slot >= static_cast<int>(podule_selection_.size())) {
		return;
	}
	const wxString short_name = podule_selection_[slot];
	if (short_name.IsEmpty()) {
		return;
	}

	const podule_header_t *header = podule_find(short_name.utf8_str().data());
	if (header == nullptr || header->config == nullptr) {
		return;
	}

	const wxString section = wxString::Format("%s.%d", short_name, slot);
	const wxString title = wxString::Format("Configure %s",
	    wxString::FromUTF8(header->name ? header->name : header->short_name));

	PoduleConfigDialog dlg(this, title, header->config, &podule_kv_[section]);
	dlg.ShowModal();
}

void MachineEditDialog::OnPoduleChanged(wxCommandEvent &event)
{
	for (size_t i = 0; i < podule_combos_.size(); i++) {
		if (podule_combos_[i] != event.GetEventObject()) {
			continue;
		}
		const int sel = podule_combos_[i]->GetSelection();
		if (sel >= 0 && sel < static_cast<int>(podule_item_names_[i].size())) {
			podule_selection_[i] = podule_item_names_[i][sel];
		} else {
			podule_selection_[i] = "";
		}
		break;
	}
	RebuildPoduleChoices();
}

void MachineEditDialog::LoadPoduleSettings(wxFileConfig &settings)
{
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS &&
	     i < static_cast<int>(podule_selection_.size()); i++) {
		wxString val;
		if (settings.Read(wxString::Format("slot%d", i), &val) && !val.IsEmpty()) {
			podule_selection_[i] = val;
		} else {
			podule_selection_[i] = "";
		}
	}

	/* Per-podule key/value config: [PoduleConfig/<section>] key=value. Collect
	   group names first - changing SetPath mid-enumeration breaks the iterator. */
	podule_kv_.clear();
	settings.SetPath("/PoduleConfig");
	wxArrayString groups;
	wxString group;
	long gidx;
	for (bool c = settings.GetFirstGroup(group, gidx); c; c = settings.GetNextGroup(group, gidx)) {
		groups.Add(group);
	}
	for (size_t gi = 0; gi < groups.GetCount(); gi++) {
		const wxString &section = groups[gi];
		settings.SetPath("/PoduleConfig/" + section);
		wxString entry;
		long eidx;
		for (bool e = settings.GetFirstEntry(entry, eidx); e; e = settings.GetNextEntry(entry, eidx)) {
			wxString value;
			settings.Read(entry, &value);
			podule_kv_[section][entry] = value;
		}
		settings.SetPath("/PoduleConfig");
	}

	settings.SetPath("/General");
	RebuildPoduleChoices();
}

void MachineEditDialog::SavePoduleSettings(wxFileConfig &settings)
{
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS &&
	     i < static_cast<int>(podule_selection_.size()); i++) {
		settings.Write(wxString::Format("slot%d", i), podule_selection_[i]);
		/* Keep the live store in step so the next reset installs the new
		   selection (for the running machine; harmless otherwise). */
		podule_cfg_set_slot(i, podule_selection_[i].utf8_str().data());
	}

	/* Persist per-podule key/value config and mirror it into the live store. */
	for (const auto &section : podule_kv_) {
		settings.SetPath("/PoduleConfig/" + section.first);
		for (const auto &kv : section.second) {
			settings.Write(kv.first, kv.second);
			podule_cfg_set_string(section.first.utf8_str().data(),
			                      kv.first.utf8_str().data(),
			                      kv.second.utf8_str().data());
		}
	}

	settings.SetPath("/General");
}

void MachineEditDialog::LoadSettings()
{
	loading_settings_ = true;
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path_, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);

	settings.Read("name", &original_name_, wxEmptyString);
	if (original_name_.empty()) {
		original_name_ = wxFileName(config_path_).GetName();
	}
	name_edit_->SetValue(original_name_);

	wxString rom_dir;
	settings.Read("rom_dir", &rom_dir, wxEmptyString);
	SetRomSelection(rom_dir);

	wxString model_name;
	settings.Read("model", &model_name, "RPCSA");
	if (model_name == "RPCARM610") model_name = "RPC610";
	if (model_name == "RPCARM710") model_name = "RPC710";
	if (model_name == "RPCARM810") model_name = "RPC810";
	for (int i = 0; i < Model_MAX; ++i) {
		if (model_name == wxString::FromUTF8(models[i].name_config)) {
			SelectModel(static_cast<Model>(i));
			break;
		}
	}

	long mem_size = 64;
	settings.Read("mem_size", &mem_size, 64L);
	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	for (int i = 0; i < static_cast<int>(sizeof(mem_values) / sizeof(mem_values[0])); ++i) {
		if (static_cast<long>(mem_values[i]) == mem_size) {
			mem_combo_->SetSelection(i);
			break;
		}
	}

	wxString vram_text;
	settings.Read("vram_size", &vram_text, "2");
	int vram_index = 1; /* "2 MB" */
	if (vram_text == "0") {
		vram_index = 0;
	} else if (vram_text == "4") {
		vram_index = 2;
	} else if (vram_text == "8") {
		vram_index = 3;
	} else if (vram_text == "16") {
		vram_index = 4;
	}
	vram_combo_->SetSelection(vram_index);

	long gfxcard = 0;
	settings.Read("gfxcard_enabled", &gfxcard, 0L);
	gfxcard_check_->SetValue(gfxcard != 0);
	long gfxcard_boot = 0;
	settings.Read("gfxcard_boot_display", &gfxcard_boot, 0L);
	gfxcard_boot_check_->SetValue(gfxcard_boot != 0);
	gfxcard_boot_check_->Enable(gfxcard != 0);

	long fullscreen = 0;
	settings.Read("start_fullscreen", &fullscreen, 0L);
	fullscreen_check_->SetValue(fullscreen != 0);

	long refresh = 60;
	settings.Read("refresh_rate", &refresh, 60L);
	refresh = std::max(20L, std::min(100L, refresh));
	refresh_slider_->SetValue(static_cast<int>(refresh));
	refresh_label_->SetLabel(wxString::Format("%ld Hz", refresh));

	wxString network_type;
	settings.Read("network_type", &network_type, "off");
	const int net_index = network_combo_->FindString(
		network_type == "off" ? "Off" :
		network_type == "nat" ? "NAT" :
		network_type == "ethernetbridging" ? "Ethernet Bridging" : "IP Tunnelling");
	if (net_index != wxNOT_FOUND) {
		network_combo_->SetSelection(net_index);
	}

	wxString bridge;
	settings.Read("bridgename", &bridge, "rpcemu");
	bridge_edit_->SetValue(bridge);
	wxString ip;
	settings.Read("ipaddress", &ip, "172.31.0.1");
	tunnel_edit_->SetValue(ip);

	settings.Read("hd4_path", &hd4_path_, wxEmptyString);

	long cdrom_enabled = 0;
	settings.Read("cdrom_enabled", &cdrom_enabled, 0L);
	cdrom_enabled_ = cdrom_enabled != 0;

	wxCommandEvent dummy;
	OnNetworkChanged(dummy);
	loading_settings_ = false;

	LoadPoduleSettings(settings);

	CallAfter([this]() { UpdateRomModelCompatibility(); });

	if (!allow_rename_) {
		name_edit_->Disable();
	}
}

void MachineEditDialog::SaveSettings()
{
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path_, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);

	if (!allow_rename_) {
		new_name_ = original_name_;
	} else {
		new_name_ = ConfigPathsSanitizeName(name_edit_->GetValue());
		new_name_.Trim(true).Trim(false);
		if (new_name_.empty()) {
			new_name_ = original_name_;
		}
	}

	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	int mem_sel = std::max(0, mem_combo_->GetSelection());
	const int vram_sel = std::max(0, vram_combo_->GetSelection());
	const Model model_sel = CurrentModelSelection();

	/* Record the configured model in the global config so it persists on save.
	   (config_save writes cfg->model, not the running machine.model, so a model
	   change to a running machine is no longer reverted when the config saves.) */
	config.model = model_sel;

	/* VRAM combo: 0 = None, 1 = 2 MB, 2 = 4 MB, 3 = 8 MB, 4 = 16 MB */
	static const int vram_sizes[] = { 0, 2, 4, 8, 16 };
	int vram_mb = vram_sizes[vram_sel < 5 ? vram_sel : 1];

	/* Enforce the same per-model RAM/VRAM limits as UpdateRomModelCompatibility
	   (and the core clamps), in case a combo was left in a stale state. */
	switch (model_sel) {
	case Model_Kinetic:
		mem_sel = 7; vram_mb = 2; break;	/* 512MB + 2MB (HAL clamp) */
	case Model_Phoebe:
		mem_sel = 6; vram_mb = 4; break;	/* fixed 256MB + 4MB */
	case Model_A7000:
	case Model_A7000plus:
		if (mem_sel == 7) { mem_sel = 6; }	/* 512MB is Kinetic-only */
		vram_mb = 0; break;			/* no VRAM (video from DRAM) */
	default:
		if (mem_sel == 7) { mem_sel = 6; }	/* 512MB is Kinetic-only */
		break;
	}

	const wxString rom_dir = SelectedRomDir();

	wxString network_type = "off";
	const wxString network_label = network_combo_->GetStringSelection();
	if (network_label == "NAT") {
		network_type = "nat";
	} else if (network_label == "Ethernet Bridging") {
		network_type = "ethernetbridging";
	} else if (network_label == "IP Tunnelling") {
		network_type = "iptunnelling";
	}

	settings.Write("name", new_name_);
	settings.Write("rom_dir", rom_dir);
	settings.Write("model", wxString::FromUTF8(models[model_sel].name_config));
	settings.Write("mem_size", wxString::Format("%d", mem_values[mem_sel]));
	settings.Write("vram_size", wxString::Format("%d", vram_mb));
	settings.Write("gfxcard_enabled",
	               static_cast<long>(gfxcard_check_->GetValue() ? 1 : 0));
	settings.Write("gfxcard_boot_display",
	               static_cast<long>(gfxcard_boot_check_->GetValue() ? 1 : 0));
	settings.Write("start_fullscreen",
	               static_cast<long>(fullscreen_check_->GetValue() ? 1 : 0));
	settings.Write("refresh_rate", refresh_slider_->GetValue());
	settings.Write("network_type", network_type);
	settings.Write("bridgename", bridge_edit_->GetValue());
	settings.Write("ipaddress", tunnel_edit_->GetValue());

	SavePoduleSettings(settings);

	if (!settings.Flush()) {
		wxLogError("Failed to save machine configuration to %s", config_path_);
	}

	renamed_ = allow_rename_ && (new_name_ != original_name_);
	ApplySavedSettingsToGlobalConfig(rom_dir, mem_values[mem_sel], vram_mb,
	                                 refresh_slider_->GetValue(),
	                                 network_type == "nat" ? NetworkType_NAT :
	                                 network_type == "ethernetbridging" ? NetworkType_EthernetBridging :
	                                 network_type == "iptunnelling" ? NetworkType_IPTunnelling :
	                                 NetworkType_Off);
}

void MachineEditDialog::ApplySavedSettingsToGlobalConfig(const wxString &rom_dir, int mem_size,
                                                         int vram_internal, int refresh,
                                                         NetworkType network_type)
{
	const wxScopedCharBuffer name_utf8 = new_name_.utf8_str();
	const wxScopedCharBuffer rom_utf8 = rom_dir.utf8_str();
	const wxScopedCharBuffer bridge_utf8 = bridge_edit_->GetValue().utf8_str();
	const wxScopedCharBuffer ip_utf8 = tunnel_edit_->GetValue().utf8_str();
	config_apply_machine_edit(&config, name_utf8.data(), rom_utf8.data(),
	                          static_cast<unsigned>(mem_size),
	                          static_cast<unsigned>(vram_internal), refresh, network_type,
	                          bridge_utf8.data(), ip_utf8.data());
	/* Whether the graphics card is fitted is read when the machine resets, so
	   applying it here means a reset is enough - no need to restart. */
	config.gfxcard_enabled = gfxcard_check_->GetValue() ? 1 : 0;
	config.gfxcard_boot_display = gfxcard_boot_check_->GetValue() ? 1 : 0;
	/* Only read when a machine starts, so this one takes effect next time. */
	config.start_fullscreen = fullscreen_check_->GetValue() ? 1 : 0;
}

wxString MachineEditDialog::CurrentMachineNameForHd() const
{
	if (!allow_rename_) {
		return original_name_;
	}

	wxString name = ConfigPathsSanitizeName(name_edit_->GetValue());
	name.Trim(true).Trim(false);
	if (name.empty()) {
		return original_name_;
	}
	return name;
}

wxString MachineEditDialog::HardDiscFilePath(int drive) const
{
	if (drive == 4 && !hd4_path_.empty() && wxFileName(hd4_path_).IsAbsolute()) {
		return hd4_path_;
	}

	const wxString machine_dir = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() +
	                             CurrentMachineNameForHd();
	return machine_dir + wxFileName::GetPathSeparator() + wxString::Format("hd%d.hdf", drive);
}

MachineEditDialog::HardDiscInfo MachineEditDialog::QueryHardDiscInfo(int drive) const
{
	HardDiscInfo info;
	info.path = HardDiscFilePath(drive);
	info.uses_custom_path = drive == 4 && !hd4_path_.empty() && wxFileName(hd4_path_).IsAbsolute();

	if (!wxFileExists(info.path)) {
		info.state = HardDiscState::Missing;
		return info;
	}

	const wxULongLong size_bytes = wxFileName::GetSize(info.path);
	info.size_text = FormatHardDiscSize(size_bytes);
	info.modified_text = FormatModifiedTime(info.path);

	if (size_bytes == 0) {
		info.state = HardDiscState::Empty;
		return info;
	}

	if (drive == 5 && cdrom_enabled_) {
		info.state = HardDiscState::Blocked;
		return info;
	}

	info.state = info.uses_custom_path ? HardDiscState::CustomPath : HardDiscState::Ready;
	return info;
}

void MachineEditDialog::ApplyHardDiscPanel(HardDiscPanel &panel, const HardDiscInfo &info)
{
	wxString badge_text;
	wxColour badge_colour = kHdColourMissing;

	switch (info.state) {
	case HardDiscState::Missing:
		badge_text = "Not created";
		badge_colour = kHdColourMissing;
		break;
	case HardDiscState::Empty:
		badge_text = "Empty - will not attach";
		badge_colour = kHdColourEmpty;
		break;
	case HardDiscState::Ready:
		badge_text = wxString::Format("Ready - %s", info.size_text);
		badge_colour = kHdColourReady;
		break;
	case HardDiscState::CustomPath:
		badge_text = wxString::Format("Ready - %s (custom path)", info.size_text);
		badge_colour = kHdColourReady;
		break;
	case HardDiscState::Blocked:
		badge_text = wxString::Format("Unavailable - CD-ROM enabled (%s on disk)", info.size_text);
		badge_colour = kHdColourBlocked;
		break;
	}

	panel.badge->SetLabel(badge_text);
	panel.badge->SetForegroundColour(badge_colour);

	wxString path_text = TruncatePathMiddle(info.path);
	if (!allow_rename_ || CurrentMachineNameForHd() == original_name_) {
		panel.path_label->SetLabel(path_text);
	} else {
		path_text += "  (preview for unsaved name)";
		panel.path_label->SetLabel(path_text);
	}
	if (!info.path.empty()) {
		panel.path_label->SetToolTip(info.path);
	} else {
		panel.path_label->UnsetToolTip();
	}

	if (info.modified_text.empty()) {
		panel.modified_label->SetLabel(wxEmptyString);
	} else {
		panel.modified_label->SetLabel(info.modified_text);
	}

	const bool file_exists = info.state != HardDiscState::Missing;
	panel.delete_btn->Enable(file_exists);
	panel.open_folder_btn->Enable(file_exists);
	panel.create_btn->Enable(true);
}

void MachineEditDialog::UpdateHdStatus()
{
	ApplyHardDiscPanel(hd4_panel_, QueryHardDiscInfo(4));
	ApplyHardDiscPanel(hd5_panel_, QueryHardDiscInfo(5));
}

void MachineEditDialog::ShowHardDiscCreateMenu(int drive)
{
	wxMenu menu;
	menu.Append(ID_HD_CREATE_256_MB, "256 MB");
	menu.Append(ID_HD_CREATE_512_MB, "512 MB");
	menu.Append(ID_HD_CREATE_1_GB, "1 GB");
	menu.Append(ID_HD_CREATE_2_GB, "2 GB");

	menu.Bind(wxEVT_MENU, [this, drive](wxCommandEvent &event) {
		int size_mb = 512;
		switch (event.GetId()) {
		case ID_HD_CREATE_256_MB:
			size_mb = 256;
			break;
		case ID_HD_CREATE_1_GB:
			size_mb = 1024;
			break;
		case ID_HD_CREATE_2_GB:
			size_mb = 2048;
			break;
		default:
			break;
		}
		CreateHardDisc(drive, size_mb);
	});

	wxButton *const create_btn = drive == 4 ? hd4_panel_.create_btn : hd5_panel_.create_btn;
	if (create_btn != nullptr) {
		create_btn->PopupMenu(&menu);
	}
}

void MachineEditDialog::CreateHardDisc(int drive, int size_mb)
{
	const wxString hd_path = HardDiscFilePath(drive);
	const wxString machine_dir = wxFileName(hd_path).GetPath();

	if (wxFileExists(hd_path)) {
		const HardDiscInfo info = QueryHardDiscInfo(drive);
		if (info.state != HardDiscState::Empty) {
			wxMessageBox(wxString::Format("HardDisc %d already exists.", drive), "Create HardDisc",
			             wxOK | wxICON_INFORMATION, this);
			return;
		}
	}

	wxDir::Make(machine_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	wxFile file(hd_path, wxFile::write);
	if (!file.IsOpened()) {
		wxMessageBox(wxString::Format("Could not create %s", hd_path), "Create HardDisc",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	const wxULongLong bytes = static_cast<wxULongLong>(size_mb) * 1024ULL * 1024ULL;
	file.Seek(static_cast<wxFileOffset>((bytes - 1).GetValue()));
	file.Write("\0", 1);
	file.Close();

	UpdateHdStatus();

	wxString message = wxString::Format("Hard disc %d created (%d MB).", drive, size_mb);
	if (emulator_running_) {
		message += wxString::FromUTF8("\n\nUse Machine \xE2\x86\x92 Reset (not RISC OS Restart) so RPCEmu reloads the IDE image.");
	}
	wxMessageBox(message, "Create HardDisc", wxOK | wxICON_INFORMATION, this);
}

void MachineEditDialog::DeleteHardDisc(int drive)
{
	const wxString hd_path = HardDiscFilePath(drive);
	if (!wxFileExists(hd_path)) {
		return;
	}

	wxString message = wxString::Format(
	    "Delete HardDisc %d?\n\nAll data on this disc will be lost.", drive);
	if (emulator_running_) {
		message += "\n\nThe emulator is running - this change will take effect after reset.";
	}

	if (wxMessageBox(message, "Delete HardDisc", wxYES_NO | wxICON_WARNING, this) != wxYES) {
		return;
	}

	if (!wxRemoveFile(hd_path)) {
		wxMessageBox(wxString::Format("Could not delete %s", hd_path), "Delete HardDisc",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	UpdateHdStatus();
}

void MachineEditDialog::OpenHardDiscFolder(int drive)
{
	const wxString hd_path = HardDiscFilePath(drive);
	const wxString dir = wxFileName(hd_path).GetPath();
	if (!wxDirExists(dir)) {
		wxMessageBox(wxString::Format("Folder does not exist:\n%s", dir), "Open folder",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	if (!wxLaunchDefaultApplication(dir)) {
		wxMessageBox(wxString::Format("Could not open folder:\n%s", dir), "Open folder",
		             wxOK | wxICON_ERROR, this);
	}
}

void MachineEditDialog::OnNetworkChanged(wxCommandEvent &)
{
	const wxString label = network_combo_->GetStringSelection();
	const bool bridge = (label == "Ethernet Bridging");
	bridge_label_->Enable(bridge);
	bridge_edit_->Enable(bridge);
	const bool tunnel = (label == "IP Tunnelling");
	tunnel_label_->Enable(tunnel);
	tunnel_edit_->Enable(tunnel);

	/* Enabling/disabling networking changes whether slot 1 is the (locked)
	   network card or a free user slot. */
	RebuildPoduleChoices();
}

void MachineEditDialog::OnRomOrModelChanged(wxCommandEvent &)
{
	if (loading_settings_) {
		return;
	}
	UpdateRomModelCompatibility();
}

void MachineEditDialog::OnNameChanged(wxCommandEvent &)
{
	if (loading_settings_) {
		return;
	}
	UpdateHdStatus();
}

void MachineEditDialog::OnOk(wxCommandEvent &)
{
	char msg[512];
	const Model model = CurrentModelSelection();
	const wxString rom_dir = SelectedRomDir();
	const wxScopedCharBuffer rom_dir_utf8 = rom_dir.utf8_str();
	if (!rom_model_is_compatible(model, rom_dir_utf8.data(), msg, sizeof(msg))) {
		wxMessageBox(wxString::FromUTF8(msg), "ROM compatibility", wxOK | wxICON_WARNING, this);
		return;
	}
	SaveSettings();
	EndModal(wxID_OK);
}
