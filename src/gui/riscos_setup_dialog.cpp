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
 * riscos_setup_dialog.cpp - offering to fetch RISC OS.
 *
 * Two decisions and one button. Everything else about the machine that gets
 * built is a sensible default, because somebody meeting RISC OS for the first
 * time has no basis on which to choose a memory size, and somebody who does
 * can change it in the machine editor afterwards.
 */

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "riscos_setup_dialog.h"

namespace {

wxString HumanSize(long long bytes)
{
	return wxString::Format("%.0f MB", static_cast<double>(bytes) / 1000000.0);
}

}  /* namespace */

RiscosSetupDialog::RiscosSetupDialog(wxWindow *parent, bool first_run)
    : wxDialog(parent, wxID_ANY, "Set up RISC OS", wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
      first_run_(first_run)
{
	const wxString intro = first_run
	    ? "RPCEmu did not find a RISC OS ROM, and cannot start a machine "
	      "without one.\n\n"
	      "It can fetch one from RISC OS Open, along with a ready-to-use "
	      "hard disc, and build a machine around them."
	    : "Fetch a RISC OS ROM from RISC OS Open, with an optional "
	      "ready-to-use hard disc, and build a machine around them.";

	intro_text_ = new wxStaticText(this, wxID_ANY, intro);
	intro_text_->Wrap(430);

	stable_ = new wxRadioButton(this, wxID_ANY, "RISC OS 5.30",
	                            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
	auto *stable_note = new wxStaticText(this, wxID_ANY,
	    "The current stable release.");

	nightly_ = new wxRadioButton(this, wxID_ANY, "RISC OS 5.31");
	auto *nightly_note = new wxStaticText(this, wxID_ANY,
	    "The nightly development build, rebuilt as changes land.");

	stable_->SetValue(true);

	include_disc_ = new wxCheckBox(this, wxID_ANY,
	    "Include the HardDisc4 hard disc");
	include_disc_->SetValue(true);

	auto *disc_note = new wxStaticText(this, wxID_ANY,
	    "Applications, utilities, !System and a configured !Boot. Without "
	    "it the machine starts at the supervisor prompt.");
	disc_note->Wrap(370);

	size_label_ = new wxStaticText(this, wxID_ANY, wxEmptyString);

	auto *download = new wxButton(this, wxID_OK, "Download and set up");
	auto *cancel = new wxButton(this, wxID_CANCEL,
	                            first_run ? "Not now" : "Cancel");

	download->SetDefault();

	/* Indent the explanatory lines under the control they belong to. */
	const int indent = 22;
	auto *version_box = new wxStaticBoxSizer(wxVERTICAL, this, "Version");
	version_box->Add(stable_, 0, wxLEFT | wxTOP, 4);
	version_box->Add(stable_note, 0, wxLEFT | wxBOTTOM, indent);
	version_box->Add(nightly_, 0, wxLEFT, 4);
	version_box->Add(nightly_note, 0, wxLEFT | wxBOTTOM, indent);

	auto *disc_box = new wxStaticBoxSizer(wxVERTICAL, this, "Hard disc");
	disc_box->Add(include_disc_, 0, wxLEFT | wxTOP, 4);
	disc_box->Add(disc_note, 0, wxLEFT | wxBOTTOM, indent);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->AddStretchSpacer();
	buttons->Add(download, 0, wxRIGHT, 8);
	buttons->Add(cancel, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(intro_text_, 0, wxALL, 12);
	main->Add(version_box, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->AddSpacer(8);
	main->Add(disc_box, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(size_label_, 0, wxALL, 12);
	main->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

	SetSizerAndFit(main);
	CentreOnParent();

	stable_->Bind(wxEVT_RADIOBUTTON, &RiscosSetupDialog::OnChoiceChanged, this);
	nightly_->Bind(wxEVT_RADIOBUTTON, &RiscosSetupDialog::OnChoiceChanged, this);
	include_disc_->Bind(wxEVT_CHECKBOX, &RiscosSetupDialog::OnChoiceChanged, this);
	download->Bind(wxEVT_BUTTON, &RiscosSetupDialog::OnDownload, this);

	UpdateSizeLabel();
}

void RiscosSetupDialog::SetTargetMachine(const wxString &machine_name,
                                        bool include_disc)
{
	target_machine_ = machine_name;
	target_include_disc_ = include_disc;

	/* The disc question has been answered by the caller, so hide it rather
	   than showing a control that no longer decides anything. Hiding the box
	   is not enough on its own: the item has to be hidden in the sizer that
	   holds it, or the layout keeps reserving the space it used to fill. */
	include_disc_->SetValue(include_disc);
	include_disc_->Show(false);

	wxSizer *disc_box = include_disc_->GetContainingSizer();
	disc_box->Show(false);
	GetSizer()->Show(disc_box, false, true);

	/* No machine is being built here, and the disc is already decided, so say
	   what will happen rather than repeating the first-run wording. */
	intro_text_->SetLabel(include_disc
	    ? wxString::Format("Fetch RISC OS from RISC OS Open for '%s', with a "
	                       "ready-to-use hard disc.", machine_name)
	    : wxString::Format("Fetch a RISC OS ROM from RISC OS Open and select "
	                       "it for '%s'.", machine_name));
	intro_text_->Wrap(430);

	UpdateSizeLabel();

	/* SetSizerAndFit() in the constructor pinned a minimum size covering the
	   disc box, and Fit() will not shrink below that. Drop the old minimum
	   first so the dialogue can close up around what is left. */
	GetSizer()->Layout();
	SetMinSize(wxDefaultSize);
	GetSizer()->SetSizeHints(this);
	GetSizer()->Fit(this);
	CentreOnParent();
}

/** Is the hard disc part of this fetch? */
bool RiscosSetupDialog::WantsDisc() const
{
	return target_machine_.empty() ? include_disc_->GetValue()
	                               : target_include_disc_;
}

void RiscosSetupDialog::OnChoiceChanged(wxCommandEvent &)
{
	UpdateSizeLabel();
}

void RiscosSetupDialog::UpdateSizeLabel()
{
	RiscosFetchRequest request;

	request.include_disc = WantsDisc();
	size_label_->SetLabel(wxString::Format(
	    "About %s will be downloaded from riscosopen.org.",
	    HumanSize(RiscosFetchApproximateSize(request))));
}

void RiscosSetupDialog::OnDownload(wxCommandEvent &)
{
	RiscosFetchRequest request;

	/* Asked here rather than by the caller, so that the version has been
	   chosen by the time the terms are shown and declining returns to this
	   dialogue with nothing written. */
	if (!RiscosFetchConfirmLicence(this)) {
		return;
	}

	request.release = nightly_->GetValue() ? RiscosRelease::Nightly
	                                       : RiscosRelease::Stable;
	request.include_disc = WantsDisc();
	request.create_machine = target_machine_.empty();
	request.machine_name = target_machine_;

	{
		RiscosFetchProgressReporter reporter(this);

		outcome_ = RiscosFetchPerform(request, reporter);
	}

	if (outcome_.ok) {
		EndModal(wxID_OK);
		return;
	}

	if (outcome_.cancelled) {
		return;		/* Back to the dialog, nothing written */
	}

	wxMessageBox(outcome_.message + "\n\nNothing has been changed.",
	             "Could not set up RISC OS", wxOK | wxICON_ERROR, this);
}
