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

#ifndef NAT_EDIT_DIALOG_H
#define NAT_EDIT_DIALOG_H

#include <wx/wx.h>
#include <wx/spinctrl.h>

extern "C" {
#include "rpcemu.h"
}

class NatListDialog;

class NatEditDialog : public wxDialog {
public:
	NatEditDialog(NatListDialog *parent);

	void SetValues(bool is_edit, PortForwardRule rule);

private:
	void BuildUi();
	void OnSave(wxCommandEvent &event);

	NatListDialog *nat_list_dialog_ = nullptr;
	wxComboBox *proto_combo_ = nullptr;
	wxSpinCtrl *emu_port_spin_ = nullptr;
	wxSpinCtrl *host_port_spin_ = nullptr;

	bool is_edit_ = false;
	PortForwardRule old_rule_{};
};

#endif
