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

#ifdef RPCEMU_VNC

#include "app_settings.h"
#include "vnc_dialog.h"

#include <cstring>

VncDialog::VncDialog(wxWindow *parent, VncServer *server, const wxString &current_password, Config *config_copy)
	: wxDialog(parent, wxID_ANY, "VNC Server", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
	, vnc_server_(server)
	, config_copy_(config_copy)
{
	BuildUi();
	password_edit_->SetValue(current_password);
	port_spin_->SetValue(config_copy_->vnc_port > 0 ? config_copy_->vnc_port : 5900);
	enable_checkbox_->SetValue(config_copy_->vnc_enabled != 0);
	UpdateStatus();
	Fit();
	CentreOnParent();
}

void VncDialog::BuildUi()
{
	enable_checkbox_ = new wxCheckBox(this, wxID_ANY, "Enable VNC server");
	port_spin_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
	                            wxSP_ARROW_KEYS, 1024, 65535, 5900);
	password_edit_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(200, -1), wxTE_PASSWORD);
	status_label_ = new wxStaticText(this, wxID_ANY, "Stopped");
	clients_label_ = new wxStaticText(this, wxID_ANY, "Clients: 0");

	auto *form = new wxFlexGridSizer(2, 8, 8);
	form->AddGrowableCol(1, 1);
	form->Add(enable_checkbox_, 0, wxALIGN_CENTER_VERTICAL);
	form->AddStretchSpacer();
	form->Add(new wxStaticText(this, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(port_spin_, 0, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "Password:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(password_edit_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(status_label_, 1, wxEXPAND);
	form->Add(new wxStaticText(this, wxID_ANY, ""), 0);
	form->Add(clients_label_, 1, wxEXPAND);

	auto *apply_btn = new wxButton(this, wxID_APPLY, "Apply");
	auto *ok_btn = new wxButton(this, wxID_OK, "OK");
	auto *cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
	ok_btn->SetDefault();

	auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
	btn_row->Add(apply_btn, 0, wxRIGHT, 8);
	btn_row->AddStretchSpacer();
	btn_row->Add(ok_btn, 0, wxRIGHT, 4);
	btn_row->Add(cancel_btn, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(form, 0, wxEXPAND | wxALL, 10);
	main->Add(btn_row, 0, wxEXPAND | wxALL, 10);
	SetSizer(main);

	enable_checkbox_->Bind(wxEVT_CHECKBOX, &VncDialog::OnEnable, this);
	apply_btn->Bind(wxEVT_BUTTON, &VncDialog::OnApply, this);
	ok_btn->Bind(wxEVT_BUTTON, &VncDialog::OnOk, this);
	cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
}

void VncDialog::UpdateStatus()
{
	if (vnc_server_ && vnc_server_->isRunning()) {
		status_label_->SetLabel(wxString::Format("Running on port %d", vnc_server_->getPort()));
		clients_label_->SetLabel(wxString::Format("Clients: %d", vnc_server_->getClientCount()));
	} else {
		status_label_->SetLabel("Stopped");
		clients_label_->SetLabel("Clients: 0");
	}
}

bool VncDialog::ApplySettings()
{
	if (!vnc_server_ || config_copy_ == nullptr) {
		return true;
	}

	const int port = port_spin_->GetValue();
	const wxString password_wx = password_edit_->GetValue();
	const std::string password = password_wx.utf8_str().data();

	config_copy_->vnc_port = port;
	strncpy(config_copy_->vnc_password, password_wx.utf8_str().data(), sizeof(config_copy_->vnc_password) - 1);
	config_copy_->vnc_password[sizeof(config_copy_->vnc_password) - 1] = '\0';

	if (enable_checkbox_->GetValue()) {
		if (!vnc_server_->start(port, password)) {
			wxMessageBox("Failed to start VNC server.", "VNC", wxOK | wxICON_ERROR, this);
			enable_checkbox_->SetValue(false);
			config_copy_->vnc_enabled = 0;
			return false;
		}
		config_copy_->vnc_enabled = 1;
	} else {
		vnc_server_->stop();
		config_copy_->vnc_enabled = 0;
	}

	/* Persist app-wide. These are the emulator's settings, not the running
	   machine's, so they no longer travel in the machine file and saving the
	   machine will not save them. Without this the dialogue would apply for the
	   session and be forgotten on restart. */
	if (app_settings_save(rpcemu_get_datadir(), config_copy_) != 0) {
		wxMessageBox("The VNC settings are in use for this session, but could "
		             "not be saved.", "VNC", wxOK | wxICON_WARNING, this);
	}

	return true;
}

void VncDialog::OnEnable(wxCommandEvent &)
{
	if (!enable_checkbox_->GetValue() && vnc_server_) {
		vnc_server_->stop();
		config_copy_->vnc_enabled = 0;
		UpdateStatus();
	}
}

void VncDialog::OnApply(wxCommandEvent &)
{
	if (ApplySettings()) {
		UpdateStatus();
	}
}

void VncDialog::OnOk(wxCommandEvent &)
{
	if (!ApplySettings()) {
		return;
	}
	EndModal(wxID_OK);
}

#endif /* RPCEMU_VNC */
