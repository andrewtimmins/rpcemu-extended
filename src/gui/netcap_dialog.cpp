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

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>

#include "netcap_dialog.h"

extern "C" {
#include "netcapture.h"
#include "rpcemu.h"
}

enum {
	ID_NETCAP_BROWSE = wxID_HIGHEST + 1400,
	ID_NETCAP_START,
	ID_NETCAP_STOP,
	ID_NETCAP_TIMER
};

NetcapDialog::NetcapDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Network Capture", wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
      timer_(this, ID_NETCAP_TIMER)
{
	auto *top = new wxBoxSizer(wxVERTICAL);

	auto *file_row = new wxBoxSizer(wxHORIZONTAL);
	file_row->Add(new wxStaticText(this, wxID_ANY, "File:"), 0,
	    wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	path_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
	    wxSize(360, -1));
	file_row->Add(path_, 1, wxEXPAND | wxRIGHT, 8);
	file_row->Add(new wxButton(this, ID_NETCAP_BROWSE, "Browse..."), 0);
	top->Add(file_row, 0, wxEXPAND | wxALL, 12);

	/* A default that is somewhere writable and obviously ours, so the common
	   case is press Start and nothing else. */
	{
		const wxString dir = wxStandardPaths::Get().GetDocumentsDir();
		const wxString name = (config.name[0] != '\0')
		    ? wxString::FromUTF8(config.name) : wxString("rpcemu");

		path_->SetValue(dir + wxFileName::GetPathSeparator() + name + ".pcap");
	}

	auto *limit_row = new wxBoxSizer(wxHORIZONTAL);
	limit_on_ = new wxCheckBox(this, wxID_ANY, "Stop after");
	limit_on_->SetValue(true);
	limit_mb_ = new wxSpinCtrl(this, wxID_ANY, "100", wxDefaultPosition,
	    wxSize(90, -1), wxSP_ARROW_KEYS, 1, 100000, 100);
	limit_row->Add(limit_on_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	limit_row->Add(limit_mb_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	limit_row->Add(new wxStaticText(this, wxID_ANY, "MB"), 0,
	    wxALIGN_CENTER_VERTICAL);
	top->Add(limit_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	from_boot_ = new wxCheckBox(this, wxID_ANY,
	    "Start capturing when this machine starts");
	from_boot_->SetValue(config.network_capture != nullptr &&
	    config.network_capture[0] != '\0');
	top->Add(from_boot_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	start_ = new wxButton(this, ID_NETCAP_START, "Start");
	stop_ = new wxButton(this, ID_NETCAP_STOP, "Stop");
	buttons->Add(start_, 0, wxRIGHT, 8);
	buttons->Add(stop_, 0);
	top->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	state_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
	top->Add(state_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	auto *note = new wxStaticText(this, wxID_ANY,
	    "Every frame the machine sends or receives, written as a pcap file. "
	    "Open it with Wireshark or tcpdump. Nothing is captured until you press "
	    "Start, and the file can be read while it is still being written.");
	note->Wrap(430);
	note->SetFont(note->GetFont().Smaller());
	top->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	top->Add(CreateButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 12);

	SetSizerAndFit(top);

	Bind(wxEVT_BUTTON, &NetcapDialog::OnBrowse, this, ID_NETCAP_BROWSE);
	Bind(wxEVT_BUTTON, &NetcapDialog::OnStart, this, ID_NETCAP_START);
	Bind(wxEVT_BUTTON, &NetcapDialog::OnStop, this, ID_NETCAP_STOP);
	Bind(wxEVT_BUTTON, &NetcapDialog::OnClose, this, wxID_CLOSE);
	Bind(wxEVT_TIMER, &NetcapDialog::OnTimer, this, ID_NETCAP_TIMER);

	Refresh();
	timer_.Start(500);
}

void
NetcapDialog::OnBrowse(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Capture to", wxEmptyString, path_->GetValue(),
	    "Packet captures (*.pcap)|*.pcap|All files (*)|*",
	    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dlg.ShowModal() == wxID_OK) {
		path_->SetValue(dlg.GetPath());
	}
}

void
NetcapDialog::OnStart(wxCommandEvent &)
{
	const wxString path = path_->GetValue();
	const uint64_t limit = limit_on_->IsChecked()
	    ? static_cast<uint64_t>(limit_mb_->GetValue()) * 1024u * 1024u : 0u;

	if (path.IsEmpty()) {
		wxMessageBox("Choose a file to capture into.", "Network Capture",
		    wxOK | wxICON_INFORMATION, this);
		return;
	}
	if (!netcap_file_start(path.utf8_str().data(), limit)) {
		wxMessageBox("Could not open that file to write.\n\n" + path,
		    "Network Capture", wxOK | wxICON_ERROR, this);
		return;
	}
	Refresh();
}

void
NetcapDialog::OnStop(wxCommandEvent &)
{
	netcap_file_stop();
	Refresh();
}

void
NetcapDialog::OnTimer(wxTimerEvent &)
{
	Refresh();
}

/**
 * The tick is the only thing here that outlives the dialogue, so it is applied
 * on the way out rather than as it is clicked: the machine's configuration
 * should not change because somebody opened a window and thought better of it.
 */
void
NetcapDialog::OnClose(wxCommandEvent &)
{
	const bool want = from_boot_->IsChecked();
	const bool have = (config.network_capture != nullptr &&
	    config.network_capture[0] != '\0');

	if (want != have) {
		free(config.network_capture);
		config.network_capture = want
		    ? strdup(path_->GetValue().utf8_str().data()) : nullptr;
		config_save(&config);
	} else if (want) {
		const wxString path = path_->GetValue();

		if (path != wxString::FromUTF8(config.network_capture)) {
			free(config.network_capture);
			config.network_capture = strdup(path.utf8_str().data());
			config_save(&config);
		}
	}
	EndModal(wxID_CLOSE);
}

void
NetcapDialog::Refresh()
{
	NetcapStats st;

	netcap_get_stats(&st);

	start_->Enable(!st.file_active);
	stop_->Enable(st.file_active != 0);
	path_->Enable(!st.file_active);
	limit_on_->Enable(!st.file_active);
	limit_mb_->Enable(!st.file_active && limit_on_->IsChecked());

	if (st.file_active) {
		state_->SetLabel(wxString::Format(
		    "Capturing. %llu frames, %llu KB written.",
		    static_cast<unsigned long long>(st.frames),
		    static_cast<unsigned long long>(st.file_bytes / 1024)));
	} else if (st.file_stopped_full) {
		/* Said out loud: a capture that quietly stopped looks exactly like a
		   network that quietly went idle, which is what somebody would be
		   trying to diagnose. */
		state_->SetLabel(wxString::Format(
		    "Stopped: the file reached its size limit at %llu KB.",
		    static_cast<unsigned long long>(st.file_bytes / 1024)));
	} else if (st.frames > 0) {
		state_->SetLabel(wxString::Format(
		    "Not capturing. %llu frames have passed since the machine started.",
		    static_cast<unsigned long long>(st.frames)));
	} else {
		state_->SetLabel("Not capturing.");
	}
}
