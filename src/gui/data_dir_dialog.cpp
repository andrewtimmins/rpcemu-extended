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
 * data_dir_dialog.cpp - the first-run question about where data goes.
 *
 * Deliberately small, and deliberately answerable by pressing Return. Issue #67
 * was not that the default was wrong for everybody, it was that it was silent
 * and unchangeable, so the fix is to say what is about to happen and allow it to
 * be changed - not to interrogate somebody before they can use the emulator.
 */

#include "data_dir_dialog.h"

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>

namespace {

class DataDirDialog : public wxDialog {
public:
	DataDirDialog(wxWindow *parent, const wxString &suggested)
	    : wxDialog(parent, wxID_ANY, "RPCEmu Extended - Where to keep your data",
	               wxDefaultPosition, wxDefaultSize,
	               wxDEFAULT_DIALOG_STYLE)
	{
		wxBoxSizer *main = new wxBoxSizer(wxVERTICAL);

		main->Add(new wxStaticText(this, wxID_ANY,
		    "RPCEmu keeps your machines, ROMs, hard discs and settings in one\n"
		    "folder. Choose where that folder should be, or keep the suggestion."),
		    0, wxALL, 12);

		path_ = new wxTextCtrl(this, wxID_ANY, suggested,
		    wxDefaultPosition, wxSize(460, -1));

		wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(path_, 1, wxALIGN_CENTRE_VERTICAL);
		wxButton *browse = new wxButton(this, wxID_ANY, "Browse...");
		row->Add(browse, 0, wxLEFT, 8);
		main->Add(row, 0, wxLEFT | wxRIGHT | wxEXPAND, 12);

#ifdef __WXOSX__
		/* What a Mac would normally use, offered because issue #67 was raised
		   by a Mac user pointing out that the home directory is the wrong place
		   by convention. Not the default: making it one would move the folder
		   out from under everybody who already has ~/RPCEmu. */
		wxButton *appsupport = new wxButton(this, wxID_ANY,
		    "Use ~/Library/Application Support");
		main->Add(appsupport, 0, wxLEFT | wxTOP, 12);
		appsupport->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
			path_->SetValue(wxStandardPaths::Get().GetUserLocalDataDir());
		});
#endif

		main->Add(new wxStaticText(this, wxID_ANY,
		    "It will be created if it does not exist. You can change it later from\n"
		    "Options on the machine selector, and RPCEMU_DATADIR or --datadir\n"
		    "override it for one run."),
		    0, wxALL, 12);

		main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

		wxSizer *buttons = CreateButtonSizer(wxOK);
		if (buttons != nullptr) {
			main->Add(buttons, 0, wxALL | wxALIGN_RIGHT, 12);
		}

		browse->Bind(wxEVT_BUTTON, &DataDirDialog::OnBrowse, this);

		SetSizerAndFit(main);
		Centre();

		/* So that Return alone accepts the suggestion. */
		path_->SetFocus();
		path_->SetInsertionPointEnd();
	}

	wxString Path() const { return path_->GetValue().Trim().Trim(false); }

private:
	void OnBrowse(wxCommandEvent &)
	{
		/* Seeded with the deepest part of the current value that exists, so
		   Browse opens somewhere useful even though the folder itself is
		   usually still to be created. */
		wxFileName start(Path(), "");
		while (start.GetDirCount() > 0 && !start.DirExists()) {
			start.RemoveLastDir();
		}

		wxDirDialog dlg(this, "Where should RPCEmu keep its data?",
		    start.GetPath(), wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);

		if (dlg.ShowModal() == wxID_OK) {
			path_->SetValue(dlg.GetPath());
		}
	}

	wxTextCtrl *path_ = nullptr;
};

} // namespace

bool AskForDataDir(wxWindow *parent, const wxString &suggested, wxString *chosen)
{
	DataDirDialog dlg(parent, suggested);

	/* Only OK is offered, so there is no Cancel to mean "do not run". Closing
	   the dialogue by its window control is the same as accepting the
	   suggestion, which is why the caller treats false as "use the default"
	   rather than as an error. */
	if (dlg.ShowModal() != wxID_OK) {
		return false;
	}

	const wxString path = dlg.Path();

	if (path.empty()) {
		return false;
	}
	*chosen = path;
	return true;
}
