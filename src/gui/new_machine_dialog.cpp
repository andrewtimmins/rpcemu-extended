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

/*
 * new_machine_dialog.cpp - naming a new machine and putting RISC OS on it.
 *
 * The machine is created first and the download follows, so a transfer that
 * fails leaves a usable empty machine rather than nothing at all. The ROM and
 * disc go onto that machine, which is why the download cannot be offered any
 * earlier than this.
 */

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "config_paths.h"
#include "new_machine_dialog.h"
#include "riscos_fetch.h"

namespace {

wxString HumanSize(long long bytes)
{
	return wxString::Format("%.0f MB", static_cast<double>(bytes) / 1000000.0);
}

}  /* namespace */

NewMachineDialog::NewMachineDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "New Machine", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
	name_edit_ = new wxTextCtrl(this, wxID_ANY, "New Machine");

	stable_ = new wxRadioButton(this, wxID_ANY, "RISC OS 5.30, the stable release",
	                            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
	nightly_ = new wxRadioButton(this, wxID_ANY,
	    "RISC OS 5.31, the nightly development build");
	no_download_ = new wxRadioButton(this, wxID_ANY,
	    "Do not download; I will choose a ROM myself");
	stable_->SetValue(true);

	include_disc_ = new wxCheckBox(this, wxID_ANY,
	    "Include a ready-to-use hard disc");
	include_disc_->SetValue(true);
	include_disc_->SetToolTip(
	    "HardDisc4 from RISC OS Open: applications, utilities, !System and a "
	    "configured !Boot, set up for the ROM chosen above. Without it the "
	    "machine starts at the supervisor prompt with an empty disc.");

	size_label_ = new wxStaticText(this, wxID_ANY, wxEmptyString);

	auto *ok_button = new wxButton(this, wxID_OK, "OK");
	auto *cancel_button = new wxButton(this, wxID_CANCEL, "Cancel");

	ok_button->SetDefault();

	auto *name_row = new wxBoxSizer(wxHORIZONTAL);
	name_row->Add(new wxStaticText(this, wxID_ANY, "Name:"), 0,
	              wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	name_row->Add(name_edit_, 1);

	auto *os_box = new wxStaticBoxSizer(wxVERTICAL, this, "RISC OS");
	os_box->Add(stable_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
	os_box->Add(nightly_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
	os_box->Add(no_download_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
	os_box->Add(include_disc_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
	os_box->AddSpacer(6);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->AddStretchSpacer();
	buttons->Add(ok_button, 0, wxRIGHT, 8);
	buttons->Add(cancel_button, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(name_row, 0, wxEXPAND | wxALL, 12);
	main->Add(os_box, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(size_label_, 0, wxALL, 12);
	main->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

	SetSizerAndFit(main);
	CentreOnParent();

	stable_->Bind(wxEVT_RADIOBUTTON, &NewMachineDialog::OnChoiceChanged, this);
	nightly_->Bind(wxEVT_RADIOBUTTON, &NewMachineDialog::OnChoiceChanged, this);
	no_download_->Bind(wxEVT_RADIOBUTTON, &NewMachineDialog::OnChoiceChanged, this);
	include_disc_->Bind(wxEVT_CHECKBOX, &NewMachineDialog::OnChoiceChanged, this);
	ok_button->Bind(wxEVT_BUTTON, &NewMachineDialog::OnOk, this);

	name_edit_->SetFocus();
	name_edit_->SelectAll();
	UpdateSizeLabel();
}

void NewMachineDialog::OnChoiceChanged(wxCommandEvent &)
{
	/* Nothing to fetch means nothing to put on the disc either. */
	include_disc_->Enable(!no_download_->GetValue());
	UpdateSizeLabel();
}

void NewMachineDialog::UpdateSizeLabel()
{
	if (no_download_->GetValue()) {
		size_label_->SetLabel("Nothing will be downloaded. The machine will "
		                      "need a ROM before it can start.");
	} else {
		RiscosFetchRequest request;

		request.include_disc = include_disc_->GetValue();
		size_label_->SetLabel(wxString::Format(
		    "About %s will be downloaded from riscosopen.org.",
		    HumanSize(RiscosFetchApproximateSize(request))));
	}
	size_label_->Wrap(400);
	GetSizer()->Fit(this);
}

/*
 * Defaults for a machine that has just been made.
 *
 * 256MB and 8MB of VRAM rather than the 64MB and 2MB this dialogue's
 * predecessor used: it is what a RISC OS 5 machine wants, and it matches what
 * --fetch-riscos builds, so a machine made either way behaves the same.
 */
bool NewMachineDialog::CreateMachine(const wxString &name, wxString &config_path)
{
	config_path = ConfigPathsConfigsDir() + wxFileName::GetPathSeparator() +
	    name + ".cfg";

	{
		wxFileConfig settings(wxEmptyString, wxEmptyString, config_path,
		                      wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);

		ConfigFileUseGeneralGroup(settings);
		settings.Write("name", name);
		settings.Write("model", "RPCSA");
		settings.Write("mem_size", "256");
		settings.Write("vram_size", "8");
		settings.Write("sound_enabled", 1L);
		settings.Write("refresh_rate", 60L);
		settings.Write("cdrom_enabled", 1L);
		settings.Write("network_type", "nat");
		settings.Write("cpu_idle", 0L);
		settings.Write("show_fullscreen_message", 1L);

		if (!settings.Flush()) {
			return false;
		}
	}

	if (!ConfigPathsCreateMachineDirectory(name)) {
		wxRemoveFile(config_path);
		return false;
	}

	return true;
}

void NewMachineDialog::OnOk(wxCommandEvent &)
{
	const wxString name = ConfigPathsSanitizeName(name_edit_->GetValue());
	wxString config_path;

	if (name.empty()) {
		wxMessageBox("Please give the machine a name.", "New Machine",
		             wxOK | wxICON_WARNING, this);
		return;
	}
	if (!ConfigPathsIsNameUnique(name)) {
		wxMessageBox(wxString::Format("A machine named '%s' already exists.",
		                              name),
		             "Name Already Exists", wxOK | wxICON_WARNING, this);
		return;
	}

	/* Asked before the machine is made, so declining leaves nothing behind. */
	if (!no_download_->GetValue() && !RiscosFetchConfirmLicence(this)) {
		return;
	}

	if (!CreateMachine(name, config_path)) {
		wxMessageBox("The machine could not be created.", "New Machine",
		             wxOK | wxICON_ERROR, this);
		return;
	}
	created_config_path_ = config_path;

	if (no_download_->GetValue()) {
		EndModal(wxID_OK);
		return;
	}

	/* --- Fetch onto the machine just made --- */
	{
		RiscosFetchRequest request;
		RiscosFetchOutcome outcome;

		request.release = nightly_->GetValue() ? RiscosRelease::Nightly
		                                      : RiscosRelease::Stable;
		request.include_disc = include_disc_->GetValue();
		request.create_machine = false;
		request.machine_name = name;

		{
			RiscosFetchProgressReporter reporter(this);

			outcome = RiscosFetchPerform(request, reporter);
		}

		if (!outcome.ok) {
			/* The machine is kept either way. Throwing it away because a
			   download failed would lose the name and settings for no
			   reason, and the machine editor can fetch again. */
			const wxString why = outcome.cancelled
			    ? wxString("The download was cancelled.")
			    : outcome.message;

			wxMessageBox(wxString::Format(
			                 "%s\n\n'%s' has still been created, without "
			                 "RISC OS on it. You can try again with Get "
			                 "RISC OS in the machine editor.", why, name),
			             "Could not download RISC OS",
			             wxOK | wxICON_WARNING, this);
			EndModal(wxID_OK);
			return;
		}

		/* Point the machine at what was just fetched. */
		{
			wxFileConfig settings(wxEmptyString, wxEmptyString, config_path,
			                      wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);

			ConfigFileUseGeneralGroup(settings);
			settings.Write("rom_dir", outcome.rom_name);
			settings.Flush();
		}

		/* Say what arrived, and say plainly when the disc did not, rather
		   than leaving somebody to find an empty disc later. */
		wxString summary = wxString::Format(
		    "'%s' is ready, running RISC OS %s.", name, outcome.version);

		if (outcome.disc_files > 0) {
			summary += wxString::Format("\n\nIts hard disc holds %d files.",
			                            outcome.disc_files);
		} else if (request.include_disc) {
			summary += "\n\nNo hard disc was installed, so the machine will "
			           "start at the supervisor prompt.";
		}

		wxMessageBox(summary, "Machine created", wxOK | wxICON_INFORMATION,
		             this);
	}

	EndModal(wxID_OK);
}
