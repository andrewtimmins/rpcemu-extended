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
 * netcap_dialog.h - Settings > Network Capture...
 *
 * Starts and stops a pcap file while the machine runs. It lives here rather
 * than on the machine editor's Network page because the setting there is read
 * when the machine resets, and the reason anybody wants a capture is usually
 * something going wrong right now - resetting destroys the thing they were
 * trying to look at.
 *
 * The tick that DOES write to the machine's configuration is "start capturing
 * when this machine starts", which is the one case the reset is what you want:
 * the boot sequence, DHCP and Access announcing itself.
 */

#ifndef NETCAP_DIALOG_H
#define NETCAP_DIALOG_H

#include <wx/wx.h>

class NetcapDialog : public wxDialog {
public:
	explicit NetcapDialog(wxWindow *parent);

private:
	void OnBrowse(wxCommandEvent &event);
	void OnStart(wxCommandEvent &event);
	void OnStop(wxCommandEvent &event);
	void OnTimer(wxTimerEvent &event);
	void OnClose(wxCommandEvent &event);
	void Refresh();

	wxTextCtrl *path_ = nullptr;
	wxCheckBox *limit_on_ = nullptr;
	wxSpinCtrl *limit_mb_ = nullptr;
	wxCheckBox *from_boot_ = nullptr;
	wxButton *start_ = nullptr;
	wxButton *stop_ = nullptr;
	wxStaticText *state_ = nullptr;
	wxTimer timer_;
};

#endif /* NETCAP_DIALOG_H */
