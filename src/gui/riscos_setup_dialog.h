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
 * riscos_setup_dialog.h - offering to fetch RISC OS.
 *
 * Shown at startup when there is no ROM to run, and reachable afterwards from
 * the machine editor and the menu.
 */

#ifndef RISCOS_SETUP_DIALOG_H
#define RISCOS_SETUP_DIALOG_H

#include <wx/dialog.h>

#include "riscos_fetch.h"

class wxCheckBox;
class wxRadioButton;
class wxStaticText;

class RiscosSetupDialog : public wxDialog {
public:
	/**
	 * @param parent    Owning window, may be null
	 * @param first_run Word it as "there is no ROM" rather than as an
	 *                  optional extra, and offer to build a machine
	 */
	RiscosSetupDialog(wxWindow *parent, bool first_run);

	/** What was fetched. Only meaningful after ShowModal() returns wxID_OK. */
	const RiscosFetchOutcome &Outcome() const { return outcome_; }

	/**
	 * Fetch for a machine that already exists, rather than building one.
	 *
	 * The version is still chosen here, but whether the hard disc comes too is
	 * the caller's decision, made where the machine is: the disc belongs to
	 * that machine and only it knows whether the disc is empty. Asking again
	 * here would put the same question in two places.
	 *
	 * @param machine_name Machine the disc is unpacked onto
	 * @param include_disc False to fetch the ROM alone
	 */
	void SetTargetMachine(const wxString &machine_name, bool include_disc);

private:
	void OnDownload(wxCommandEvent &event);
	void OnChoiceChanged(wxCommandEvent &event);
	void UpdateSizeLabel();
	bool WantsDisc() const;

	wxRadioButton *stable_ = nullptr;
	wxRadioButton *nightly_ = nullptr;
	wxCheckBox *include_disc_ = nullptr;
	wxStaticText *size_label_ = nullptr;
	wxStaticText *intro_text_ = nullptr;

	/* Empty means build a new machine; otherwise the machine to fetch for. */
	wxString target_machine_;
	bool target_include_disc_ = false;
	RiscosFetchOutcome outcome_;
};

#endif /* RISCOS_SETUP_DIALOG_H */
