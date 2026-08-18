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
 * network_analyser_window.h - Debug > Network Analyser...
 *
 * The frames a machine is sending and receiving, as they happen, with the one
 * selected broken into its fields and shown as bytes.
 *
 * ★ Not a rival to Wireshark, and deliberately not trying to be.
 *
 * Wireshark has decades of dissectors and is a pipe away:
 * `rpcemu-netcap --pcap - | wireshark -k -i -`. This is for looking without
 * leaving the emulator, and for the people who have not got it installed. What
 * it does have that Wireshark has not is the RISC OS end - Freeway and ShareFS
 * are unregistered ports that every general-purpose tool shows as anonymous
 * numbers.
 *
 * The list is virtual: the rows come from a copy of the capture ring taken on
 * a timer, so a machine sending thousands of frames does not turn into
 * thousands of wxWidgets objects.
 */

#ifndef NETWORK_ANALYSER_WINDOW_H
#define NETWORK_ANALYSER_WINDOW_H

#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/tglbtn.h>

extern "C" {
#include "netcapture.h"
}

class NetworkAnalyserWindow : public wxFrame {
public:
	explicit NetworkAnalyserWindow(wxWindow *parent);
	~NetworkAnalyserWindow() override;

	void ShowAndRaise();

private:
	class FrameList;

	void OnTimer(wxTimerEvent &event);
	void OnSelected(wxListEvent &event);
	void OnCapture(wxCommandEvent &event);
	void OnClear(wxCommandEvent &event);
	void OnSave(wxCommandEvent &event);
	void OnAutoScroll(wxCommandEvent &event);
	void ShowDetail(long row);

	FrameList *list_ = nullptr;
	wxTextCtrl *detail_ = nullptr;
	wxTextCtrl *bytes_ = nullptr;
	wxStaticText *status_ = nullptr;
	wxToggleButton *capture_ = nullptr;
	wxCheckBox *autoscroll_ = nullptr;
	wxTimer timer_;

	std::vector<NetcapFrame> frames_;
	uint64_t last_serial_ = 0;
	long selected_ = -1;
};

#endif /* NETWORK_ANALYSER_WINDOW_H */
