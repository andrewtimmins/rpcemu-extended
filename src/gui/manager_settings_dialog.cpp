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
#include <wx/settings.h>
#include <wx/statline.h>
#include <wx/valtext.h>

#include <random>

#include "gui_preferences.h"

extern "C" {
#include "app_settings.h"
#include "guest_subnet.h"
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

/*
 * The guests' subnet as the settings file has it.
 *
 * Straight from the file rather than from the live config, which config_load()
 * has not filled in while the Manager is sitting with no machine started.
 */
static void
LoadGuestSubnet(unsigned *b, unsigned *c)
{
	Config app;

	memset(&app, 0, sizeof(app));
	app.guest_subnet_b = GUEST_SUBNET_DEFAULT_B;
	app.guest_subnet_c = GUEST_SUBNET_DEFAULT_C;
	app_settings_load(rpcemu_get_datadir(), &app);

	*b = app.guest_subnet_b;
	*c = app.guest_subnet_c;
}

ManagerSettingsDialog::ManagerSettingsDialog(wxWindow *parent,
                                             ChangeDataFolderFn change_data_folder)
	: wxDialog(parent, wxID_ANY, "RPCEmu Extended - Settings", wxDefaultPosition,
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

	/* ---- Window ---- */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, this, "Window");

		minimal_ui_check_ = new wxCheckBox(this, wxID_ANY,
		    "Start with a minimal interface");
		minimal_ui_check_->SetValue(GetMinimalUi());
		minimal_ui_check_->SetToolTip(
		    "Open with the toolbar and the machine list hidden. View > Minimal "
		    "Interface turns it off and on again without changing this.");
		box->Add(minimal_ui_check_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

		warn_stop_check_ = new wxCheckBox(this, wxID_ANY,
		    "Ask before stopping a machine");
		warn_stop_check_->SetValue(GetWarnOnStop());
		warn_stop_check_->SetToolTip(
		    "Not asked for a machine set to suspend on exit, which loses "
		    "nothing by being stopped.");
		box->Add(warn_stop_check_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

		warn_exit_check_ = new wxCheckBox(this, wxID_ANY,
		    "Ask when closing with machines running");
		warn_exit_check_->SetValue(GetWarnOnExit());
		box->Add(warn_exit_check_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

		update_check_ = new wxCheckBox(this, wxID_ANY,
		    "Automatically check for updates");
		update_check_->SetValue(GetCheckForUpdates());
		update_check_->SetToolTip("Checks for updates once per day.");
		box->Add(update_check_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
		box->AddSpacer(8);

		root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
	}

	/* ---- Networking ---- */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, this, "Networking");
		wxWindow *parent_win = box->GetStaticBox();

		auto *row = new wxBoxSizer(wxHORIZONTAL);

		/*
		 * ★ Read from the settings file, NOT from the live config.
		 *
		 * config_load() only runs when a machine starts, and the Manager can
		 * be sitting here with none - so the global still holds its built-in
		 * default. Showing that would report 10.10 whatever the file said, and
		 * pressing OK would then write it back over the real setting.
		 */
		LoadGuestSubnet(&subnet_was_b_, &subnet_was_c_);

		/* Digits only, three at most. The range is checked on OK: a
		   wxTextValidator can refuse a character but not a value. */
		const wxSize octet_size(
		    GetTextExtent("000").GetWidth() + FromDIP(16), -1);
		wxTextValidator digits(wxFILTER_DIGITS);

		subnet_b_edit_ = new wxTextCtrl(parent_win, wxID_ANY,
		    wxString::Format("%u", subnet_was_b_),
		    wxDefaultPosition, octet_size, 0, digits);
		subnet_b_edit_->SetMaxLength(3);
		subnet_c_edit_ = new wxTextCtrl(parent_win, wxID_ANY,
		    wxString::Format("%u", subnet_was_c_),
		    wxDefaultPosition, octet_size, 0, digits);
		subnet_c_edit_->SetMaxLength(3);

		row->Add(new wxStaticText(parent_win, wxID_ANY, "Guest network:"), 0,
		    wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		row->Add(new wxStaticText(parent_win, wxID_ANY, "10 ."), 0,
		    wxALIGN_CENTER_VERTICAL);
		row->Add(subnet_b_edit_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
		row->Add(new wxStaticText(parent_win, wxID_ANY, "."), 0,
		    wxALIGN_CENTER_VERTICAL);
		row->Add(subnet_c_edit_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
		row->Add(new wxStaticText(parent_win, wxID_ANY, ". 0 / 24"), 0,
		    wxALIGN_CENTER_VERTICAL);

		auto *random_button = new wxButton(parent_win, wxID_ANY, "Random");
		random_button->SetToolTip(
		    "Pick a subnet at random, which is what a new installation gets.");
		random_button->Bind(wxEVT_BUTTON,
		    [this](wxCommandEvent &) { OnRandomSubnet(); });
		row->Add(random_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

		box->Add(row, 0, wxALL, 8);

		auto *note = new wxStaticText(parent_win, wxID_ANY,
		    "Takes effect when a machine next starts.");
		note->SetForegroundColour(
		    wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		box->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

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

/* std::random_device, not rand(): an unseeded rand() is the same sequence in
   every process, so every installation pressing this would get one subnet. */
void ManagerSettingsDialog::OnRandomSubnet()
{
	std::random_device rd;
	std::uniform_int_distribution<unsigned> octet(0, 255);

	if (subnet_b_edit_ != nullptr) {
		subnet_b_edit_->ChangeValue(wxString::Format("%u", octet(rd)));
	}
	if (subnet_c_edit_ != nullptr) {
		subnet_c_edit_->ChangeValue(wxString::Format("%u", octet(rd)));
	}
}

/**
 * One octet from its field, or -1 if it is empty or out of range.
 *
 * The validator has already kept out everything but digits, so this is only
 * about the value: 256 is three digits and not an octet.
 */
static int
OctetFrom(const wxTextCtrl *field)
{
	long value = 0;

	if (field == nullptr || !field->GetValue().ToLong(&value)) {
		return -1;
	}
	return (value >= 0 && value <= 255) ? (int) value : -1;
}

void ManagerSettingsDialog::OnOk(wxCommandEvent &event)
{
	int subnet_b = -1;
	int subnet_c = -1;

	/*
	 * Checked before anything is applied. Everything below this takes effect as
	 * it is read, so refusing halfway would leave the window open with some of
	 * its settings already saved.
	 *
	 * Refused rather than clamped: somebody who typed 300 meant something, and
	 * silently making it 255 puts the guests somewhere they did not ask for.
	 */
	if (subnet_b_edit_ != nullptr && subnet_c_edit_ != nullptr) {
		subnet_b = OctetFrom(subnet_b_edit_);
		subnet_c = OctetFrom(subnet_c_edit_);

		if (subnet_b < 0 || subnet_c < 0) {
			wxMessageBox(
			    "Each part of the guest network must be a number from 0 to 255.",
			    "Guest Network", wxOK | wxICON_WARNING, this);
			(subnet_b < 0 ? subnet_b_edit_ : subnet_c_edit_)->SetFocus();
			return;	/* not Skip(), so the window stays open */
		}
	}

	if (acceleration_check_ != nullptr) {
		acceleration_chosen_ = acceleration_check_->GetValue();
	}

	if (acceleration_chosen_ != acceleration_was_) {
		SetHardwareAcceleration(acceleration_chosen_);
		rpclog("Settings: hardware acceleration turned %s\n",
		    acceleration_chosen_ ? "on" : "off");
	}

	if (minimal_ui_check_ != nullptr) {
		SetMinimalUi(minimal_ui_check_->GetValue());
	}
	if (warn_stop_check_ != nullptr) {
		SetWarnOnStop(warn_stop_check_->GetValue());
	}
	if (warn_exit_check_ != nullptr) {
		SetWarnOnExit(warn_exit_check_->GetValue());
	}
	if (update_check_ != nullptr) {
		SetCheckForUpdates(update_check_->GetValue());
	}

	if (subnet_b >= 0 && subnet_c >= 0 &&
	    ((unsigned) subnet_b != subnet_was_b_ ||
	     (unsigned) subnet_c != subnet_was_c_)) {
		/* The file re-read into a Config of its own: app_settings_save() writes
		   every key it knows, and the live one is not loaded while the Manager
		   has no machine. */
		Config app;

		memset(&app, 0, sizeof(app));
		app.vnc_port = 5900;
		app.hostcmd_enabled = 1;
		app_settings_load(rpcemu_get_datadir(), &app);

		app.guest_subnet_b = (unsigned) subnet_b;
		app.guest_subnet_c = (unsigned) subnet_c;

		if (app_settings_save(rpcemu_get_datadir(), &app) != 0) {
			wxLogWarning("RPCEmu could not save the guest network setting.");
		} else {
			/* The live config too, so a machine started afterwards is on the
			   network just chosen. */
			config.guest_subnet_b = (unsigned) subnet_b;
			config.guest_subnet_c = (unsigned) subnet_c;
			rpclog("Settings: guests are now on 10.%d.%d.0/24\n",
			    subnet_b, subnet_c);
		}
	}
	event.Skip();	/* let wxDialog close with wxID_OK */
}
