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

#ifndef VNC_DIALOG_H
#define VNC_DIALOG_H

#include <wx/wx.h>
#include <wx/spinctrl.h>

#include "vnc_server.h"

extern "C" {
#include "rpcemu.h"
}

class VncDialog : public wxDialog {
public:
	VncDialog(wxWindow *parent, VncServer *server, const wxString &current_password, Config *config_copy);

	wxString GetPassword() const { return password_edit_->GetValue(); }

private:
	void BuildUi();
	void UpdateStatus();
	bool ApplySettings();
	void OnEnable(wxCommandEvent &event);
	void OnApply(wxCommandEvent &event);
	void OnOk(wxCommandEvent &event);

	VncServer *vnc_server_;
	Config *config_copy_;
	wxCheckBox *enable_checkbox_ = nullptr;
	wxSpinCtrl *port_spin_ = nullptr;
	wxTextCtrl *password_edit_ = nullptr;
	wxTextCtrl *password_readonly_edit_ = nullptr;
	wxStaticText *status_label_ = nullptr;
	wxStaticText *clients_label_ = nullptr;
};

#endif /* VNC_DIALOG_H */

#endif /* RPCEMU_VNC */
