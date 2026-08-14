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

#ifndef MANAGER_SETTINGS_DIALOG_H
#define MANAGER_SETTINGS_DIALOG_H

#include <functional>

#include <wx/wx.h>

/*
 * RPCEmu's own settings, as opposed to a machine's.
 *
 * The distinction is the whole reason this window exists. Everything on the
 * Manager's Settings menu belongs to the machine being shown and is forwarded to
 * it; these two belong to this computer and this installation, and had nowhere to
 * live. The data folder was on the File menu, between machine commands, where it
 * read like one of them.
 *
 * Both entries here are host preferences (gui_preferences.h): they are statements
 * about this computer, not about any machine, and they follow the user's login
 * rather than their data folder - which the data folder setting could hardly do
 * otherwise, since it is what says where that folder is.
 */
class ManagerSettingsDialog : public wxDialog {
public:
	/*
	 * `change_data_folder` runs the Manager's existing data-folder flow - the
	 * running-machine check, the folder chooser, the offer to move the files.
	 * Passed in rather than reproduced here, because that flow refuses while
	 * machines are running and only the Manager knows which are.
	 *
	 * It returns the folder in use afterwards, which is the same as before if
	 * the user cancelled.
	 */
	using ChangeDataFolderFn = std::function<wxString()>;

	ManagerSettingsDialog(wxWindow *parent, ChangeDataFolderFn change_data_folder);

	/* What the user left the hardware-acceleration box at. Only meaningful
	   after ShowModal() returned wxID_OK. */
	bool HardwareAccelerationChosen() const { return acceleration_chosen_; }

	/* Whether that differs from what it was when the window opened, so the
	   caller can avoid rebuilding displays that do not need it. */
	bool HardwareAccelerationChanged() const
	{
		return acceleration_chosen_ != acceleration_was_;
	}

private:
	void OnChangeDataFolder(wxCommandEvent &event);
	void OnOk(wxCommandEvent &event);

	ChangeDataFolderFn change_data_folder_;
	wxStaticText *data_folder_text_ = nullptr;
	wxCheckBox *acceleration_check_ = nullptr;
	wxCheckBox *minimal_ui_check_ = nullptr;
	wxCheckBox *warn_stop_check_ = nullptr;
	wxCheckBox *warn_exit_check_ = nullptr;

	bool acceleration_was_ = true;
	bool acceleration_chosen_ = true;
};

#endif /* MANAGER_SETTINGS_DIALOG_H */
