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

#include "manager_settings_dialog.h"

#include <wx/filename.h>
#include <wx/statline.h>

#include "gui_preferences.h"

extern "C" {
#include "rpcemu.h"
}

/*
 * A directory as somebody would write it, rather than as the core keeps it.
 * The core appends '/' whatever the platform, so a Windows path that already
 * ends in '\' comes back as "C:\Users\David\RPCEmu\/".
 */
static wxString
DisplayPath(wxString path)
{
	while (!path.empty() &&
	    (path.Last() == '/' || path.Last() == wxFileName::GetPathSeparator())) {
		path.RemoveLast();
	}
	return path;
}

/*
 * The accelerated path is not the same thing on every platform, and the window
 * says which one it is offering rather than making the user guess. Written out
 * here rather than in the label so the label stays short.
 */
static wxString
AccelerationExplanation()
{
#ifdef __WXMSW__
	return "Draws a machine's screen through Direct2D, which the graphics card "
	       "does. Turn it off to draw on the CPU instead.";
#elif defined(__WXMAC__)
	return "Draws a machine's screen as an OpenGL texture, which the graphics "
	       "card scales and filters. Turn it off to draw on the CPU instead - "
	       "macOS scales in software otherwise, which costs a good deal more.";
#else
	return "Draws a machine's screen as an OpenGL texture, which the graphics "
	       "card scales and filters. Turn it off to draw on the CPU instead - "
	       "the alternative on Linux is Cairo, which scales in software and is "
	       "slower than doing it by hand.";
#endif
}

ManagerSettingsDialog::ManagerSettingsDialog(wxWindow *parent,
                                             ChangeDataFolderFn change_data_folder)
	: wxDialog(parent, wxID_ANY, "RPCEmu Settings", wxDefaultPosition,
	    wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
	, change_data_folder_(std::move(change_data_folder))
{
	acceleration_was_ = GetHardwareAcceleration();
	acceleration_chosen_ = acceleration_was_;

	auto *root = new wxBoxSizer(wxVERTICAL);

	/* ---- Data folder ---- */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, this, "Data folder");

		box->Add(new wxStaticText(this, wxID_ANY,
		    "Where RPCEmu keeps machines, ROMs, discs and settings."),
		    0, wxLEFT | wxRIGHT | wxTOP, 8);

		data_folder_text_ = new wxStaticText(this, wxID_ANY,
		    DisplayPath(wxString::FromUTF8(rpcemu_get_datadir())));
		data_folder_text_->SetFont(data_folder_text_->GetFont().Bold());
		box->Add(data_folder_text_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

		auto *change = new wxButton(this, wxID_ANY, "Change...");

		change->SetToolTip("Choose a different folder, and offer to move what is "
		                   "in this one");
		change->Bind(wxEVT_BUTTON, &ManagerSettingsDialog::OnChangeDataFolder, this);
		box->Add(change, 0, wxLEFT | wxRIGHT | wxTOP, 8);
		box->AddSpacer(8);

		root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
	}

	/* ---- Display ---- */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, this, "Display");

		acceleration_check_ = new wxCheckBox(this, wxID_ANY,
		    "Enable hardware acceleration");
		acceleration_check_->SetValue(acceleration_was_);
		box->Add(acceleration_check_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

		auto *note = new wxStaticText(this, wxID_ANY, AccelerationExplanation());

		note->Wrap(420);
		note->SetFont(note->GetFont().Smaller());
		box->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

#if !defined(__WXMSW__) && !wxUSE_GLCANVAS
		/*
		 * This build's wxWidgets has no OpenGL canvas, so there is nothing to
		 * turn on. Said rather than silently ignored: a tick-box that does
		 * nothing is worse than one that explains itself.
		 */
		acceleration_check_->Disable();
		auto *unavailable = new wxStaticText(this, wxID_ANY,
		    "This build of RPCEmu was made against a wxWidgets without OpenGL "
		    "support, so a machine's screen is always drawn on the CPU.");

		unavailable->Wrap(420);
		unavailable->SetForegroundColour(*wxRED);
		box->Add(unavailable, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
#endif

		auto *applies = new wxStaticText(this, wxID_ANY,
		    "Applies to a machine shown in this window. A machine in its own "
		    "window is unaffected, and a display that cannot start the "
		    "accelerated path falls back to the CPU by itself.");

		applies->Wrap(420);
		applies->SetForegroundColour(
		    wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		applies->SetFont(applies->GetFont().Smaller());
		box->Add(applies, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
		box->AddSpacer(8);

		root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
	}

	root->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, 10);

	auto *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);

	root->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	Bind(wxEVT_BUTTON, &ManagerSettingsDialog::OnOk, this, wxID_OK);

	SetSizerAndFit(root);
	CentreOnParent();
}

/*
 * The folder is changed there and then, not on OK.
 *
 * It has to be: the Manager's flow asks its own questions - whether to move the
 * files, what to do about machines that are running - and answering those and
 * then having the user press Cancel would leave the answers half applied. So the
 * button does the whole thing, and the label is updated from whatever it ended
 * up being.
 */
void ManagerSettingsDialog::OnChangeDataFolder(wxCommandEvent & /*event*/)
{
	if (!change_data_folder_) {
		return;
	}

	const wxString now = change_data_folder_();

	if (!now.empty() && data_folder_text_ != nullptr) {
		data_folder_text_->SetLabel(DisplayPath(now));
		Layout();
		Fit();
	}
}

void ManagerSettingsDialog::OnOk(wxCommandEvent &event)
{
	if (acceleration_check_ != nullptr) {
		acceleration_chosen_ = acceleration_check_->GetValue();
	}

	if (acceleration_chosen_ != acceleration_was_) {
		SetHardwareAcceleration(acceleration_chosen_);
		rpclog("Settings: hardware acceleration turned %s\n",
		    acceleration_chosen_ ? "on" : "off");
	}
	event.Skip();	/* let wxDialog close with wxID_OK */
}
