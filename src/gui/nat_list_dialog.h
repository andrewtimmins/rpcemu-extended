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

#ifndef NAT_LIST_DIALOG_H
#define NAT_LIST_DIALOG_H

#include <wx/wx.h>
#include <wx/listctrl.h>

#include "emulator_host.h"

extern "C" {
#include "rpcemu.h"
}

class NatEditDialog;

class NatListDialog : public wxDialog {
public:
	NatListDialog(wxWindow *parent, EmulatorHost *emulator_host);
	~NatListDialog() override;

	/* Built once at startup and kept hidden until asked for, so it is not a
	   window the user would think of as open. Every top level window claims
	   otherwise by default, which would leave wx believing the machine window
	   was not the last one and stop it ending the application. */
	bool ShouldPreventAppExit() const override { return false; }

	void AddNatRule(PortForwardRule rule);

	/* Show the dialog, re-reading the rules first. The dialog is built once at
	   startup and reused, so its list would otherwise keep whatever it was
	   populated with then. */
	int ShowRules();

	bool ValidateAdd(PortForwardRule rule);
	bool ValidateEdit(PortForwardRule old_rule, PortForwardRule new_rule);
	void ProcessAdd(PortForwardRule rule);
	void ProcessEdit(PortForwardRule old_rule, PortForwardRule new_rule);

private:
	void BuildUi();
	void LoadRulesFromConfig();
	PortForwardRule RuleFromRow(long row) const;
	long FindRuleRow(PortForwardRule rule) const;
	long SelectedRow() const;

	void OnAddRule(wxCommandEvent &event);
	void OnEditRule(wxCommandEvent &event);
	void OnDeleteRule(wxCommandEvent &event);
	void OnListActivated(wxListEvent &event);

	EmulatorHost *emulator_host_ = nullptr;
	wxListCtrl *rules_list_ = nullptr;
	NatEditDialog *nat_edit_dialog_ = nullptr;
};

void NatListDialogNotifyRule(PortForwardRule rule);

#endif
