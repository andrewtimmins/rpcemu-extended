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
 * new_machine_dialog.h - naming a new machine and putting RISC OS on it.
 *
 * Creating a machine used to be a bare name prompt, which left somebody with an
 * empty machine and no indication that it needed a ROM and a hard disc before it
 * would do anything. Both are offered here instead, because this is the one
 * moment when the machine definitely has no disc of its own to protect.
 */

#ifndef NEW_MACHINE_DIALOG_H
#define NEW_MACHINE_DIALOG_H

#include <wx/dialog.h>
#include <wx/string.h>

class wxCheckBox;
class wxRadioButton;
class wxStaticText;
class wxTextCtrl;

class NewMachineDialog : public wxDialog {
public:
	explicit NewMachineDialog(wxWindow *parent);

	/**
	 * Configuration file of the machine that was created.
	 *
	 * Only meaningful after ShowModal() returns wxID_OK, and non-empty then:
	 * the machine exists by that point, even if a download it was asked for
	 * failed, so nothing is silently lost.
	 */
	const wxString &CreatedConfigPath() const { return created_config_path_; }

private:
	void OnOk(wxCommandEvent &event);
	void OnChoiceChanged(wxCommandEvent &event);
	void UpdateSizeLabel();

	/** Write the configuration and create the machine's directory. */
	bool CreateMachine(const wxString &name, wxString &config_path);

	wxTextCtrl *name_edit_ = nullptr;
	wxRadioButton *stable_ = nullptr;
	wxRadioButton *nightly_ = nullptr;
	wxRadioButton *no_download_ = nullptr;
	wxCheckBox *include_disc_ = nullptr;
	wxCheckBox *configure_network_ = nullptr;
	wxStaticText *size_label_ = nullptr;

	wxString created_config_path_;
};

#endif /* NEW_MACHINE_DIALOG_H */
