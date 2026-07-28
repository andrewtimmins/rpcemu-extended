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

#include "serial_dialog.h"

#include <wx/filefn.h>

#include <cstring>

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

extern "C" {
#include "peripheral_config.h"
#include "rpcemu.h"
}

enum {
	ID_SERIAL_COM1_DISABLED = wxID_HIGHEST + 250,
	ID_SERIAL_COM1_LOGFILE,
	ID_SERIAL_COM1_TCPMODEM,
	ID_SERIAL_COM1_PHYSICAL,
	ID_SERIAL_COM1_BROWSE,
	ID_SERIAL_APPLY,
};

/*
 * Serial ports the host actually has.
 *
 * Listing names that are not there is how this dialogue used to mislead people,
 * so only existing devices are offered. USB adapters come first, because that is
 * what a serial port on a modern machine usually is. The field stays editable
 * for anything unusual, a pseudo terminal included.
 */
static wxArrayString
HostSerialPorts()
{
	wxArrayString ports;

#ifdef __WXMSW__
	/* No cheap way to enumerate these without the setup API, and COM1 to COM8
	   covers the common cases; the field is editable for the rest. */
	for (int i = 1; i <= 8; i++) {
		ports.Add(wxString::Format("COM%d", i));
	}
#else
	static const char *const patterns[] = {
		"/dev/ttyUSB*",		/* USB serial adapters */
		"/dev/ttyACM*",		/* USB CDC devices */
		"/dev/tty.usbserial*",	/* macOS */
		"/dev/cu.usbserial*",
		"/dev/cu.usbmodem*",
		"/dev/ttyS*",		/* built-in, where there is one */
	};
	size_t p;

	for (p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
		wxString match = wxFindFirstFile(patterns[p], wxFILE);

		while (!match.empty()) {
			ports.Add(match);
			match = wxFindNextFile();
		}
	}
#endif

	return ports;
}

static SerialPortSettings
PeripheralSerialToDialog(PeripheralSerialMode mode, const char *log_path, const char *device_path)
{
	SerialPortSettings settings;
	settings.mode = static_cast<SerialPortMode>(mode);
	settings.log_file_path = wxString::FromUTF8(log_path);
	settings.physical_device = wxString::FromUTF8(device_path);
	return settings;
}

static bool
PeripheralSerialFromDialog(const SerialPortSettings &settings,
                           PeripheralSerialMode *mode,
                           char *log_path,
                           size_t log_path_len,
                           char *device,
                           size_t device_len,
                           const wxString &port_name,
                           wxString *warning)
{
	if (settings.mode == SerialPortMode::PhysicalDevice) {
		wxString chosen = settings.physical_device;

		chosen.Trim(true).Trim(false);
		if (chosen.empty()) {
			if (warning != nullptr) {
				*warning = wxString::Format(
				    "%s needs a serial device to use.", port_name);
			}
			return false;
		}
	}

	if (settings.mode == SerialPortMode::LogToFile) {
		wxString chosen_path = settings.log_file_path;
		chosen_path.Trim();
		if (chosen_path.empty()) {
			const char *machine_dir = rpcemu_get_machine_datadir();
			if (machine_dir != NULL && machine_dir[0] != '\0') {
				chosen_path = wxString::Format("%sserial_%s.log",
				                               wxString::FromUTF8(machine_dir),
				                               port_name.Lower());
			} else {
				chosen_path = wxStandardPaths::Get().GetDocumentsDir() +
				              wxFileName::GetPathSeparator() +
				              wxString::Format("rpcemu_%s.log", port_name.Lower());
			}
		}

		const wxScopedCharBuffer log_utf8 = chosen_path.utf8_str();
		strncpy(log_path, log_utf8.data(), log_path_len - 1);
		log_path[log_path_len - 1] = '\0';
		*mode = PeripheralSerial_LogToFile;
		return true;
	}

	*mode = static_cast<PeripheralSerialMode>(settings.mode);
	const wxScopedCharBuffer log_utf8 = settings.log_file_path.utf8_str();
	const wxScopedCharBuffer device_utf8 = settings.physical_device.utf8_str();
	strncpy(log_path, log_utf8.data(), log_path_len - 1);
	strncpy(device, device_utf8.data(), device_len - 1);
	log_path[log_path_len - 1] = '\0';
	device[device_len - 1] = '\0';
	return true;
}

SerialDialog::SerialDialog(wxWindow *parent)
	: wxDialog(parent, wxID_ANY, "Serial Port Configuration", wxDefaultPosition, wxSize(520, 360),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxCLOSE_BOX)
{
	BuildUi();

	SetCom1Settings(PeripheralSerialToDialog(
	    peripheral_config.com1_mode,
	    peripheral_config.com1_log_path,
	    peripheral_config.com1_device));

	Fit();
	CentreOnParent();
}

void SerialDialog::BuildUi()
{
	auto *main = new wxBoxSizer(wxVERTICAL);
	/* The Risc PC has a single hardware serial port (the 16550 at 0x3F8). */
	main->Add(CreatePortGroup("Serial (0x3F8)"), 0, wxEXPAND | wxALL, 10);

	auto *button_row = new wxBoxSizer(wxHORIZONTAL);
	button_row->AddStretchSpacer();
	auto *apply_btn = new wxButton(this, ID_SERIAL_APPLY, "Apply");
	auto *cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
	apply_btn->SetDefault();
	button_row->Add(apply_btn, 0, wxRIGHT, 4);
	button_row->Add(cancel_btn, 0);
	main->Add(button_row, 0, wxEXPAND | wxALL, 10);
	SetSizer(main);

	apply_btn->Bind(wxEVT_BUTTON, &SerialDialog::OnApply, this);
	cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
}

wxStaticBoxSizer *SerialDialog::CreatePortGroup(const wxString &title)
{
	auto *group = new wxStaticBoxSizer(wxVERTICAL, this, title);
	PortWidgets *widgets = &com1_;

	const int disabled_id = ID_SERIAL_COM1_DISABLED;
	const int logfile_id = ID_SERIAL_COM1_LOGFILE;
	const int tcpmodem_id = ID_SERIAL_COM1_TCPMODEM;
	const int physical_id = ID_SERIAL_COM1_PHYSICAL;
	const int browse_id = ID_SERIAL_COM1_BROWSE;

	widgets->disabled_radio = new wxRadioButton(this, disabled_id, "Disabled", wxDefaultPosition, wxDefaultSize,
	                                            wxRB_GROUP);
	widgets->logfile_radio = new wxRadioButton(this, logfile_id, "Log to File");
	widgets->tcpmodem_radio = new wxRadioButton(this, tcpmodem_id, "TCP Modem (AT commands)");
	widgets->physical_radio = new wxRadioButton(this, physical_id, "Physical Device");

	group->Add(widgets->disabled_radio, 0, wxBOTTOM, 4);

	auto *log_row = new wxBoxSizer(wxHORIZONTAL);
	log_row->Add(widgets->logfile_radio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	widgets->logfile_edit = new wxTextCtrl(this, wxID_ANY);
	widgets->browse_btn = new wxButton(this, browse_id, "Browse...");
	log_row->Add(widgets->logfile_edit, 1, wxEXPAND | wxRIGHT, 4);
	log_row->Add(widgets->browse_btn, 0);
	group->Add(log_row, 0, wxEXPAND | wxBOTTOM, 4);

	group->Add(widgets->tcpmodem_radio, 0, wxBOTTOM, 4);

	auto *phys_row = new wxBoxSizer(wxHORIZONTAL);
	phys_row->Add(widgets->physical_radio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	phys_row->Add(new wxStaticText(this, wxID_ANY, "Device:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
	widgets->device_combo = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
	                                       wxCB_DROPDOWN);
	{
		const wxArrayString ports = HostSerialPorts();

		for (size_t i = 0; i < ports.GetCount(); i++) {
			widgets->device_combo->Append(ports[i]);
		}
	}
	phys_row->Add(widgets->device_combo, 1, wxEXPAND);
	group->Add(phys_row, 0, wxEXPAND);

	Bind(wxEVT_RADIOBUTTON, &SerialDialog::OnCom1ModeChanged, this, ID_SERIAL_COM1_DISABLED, ID_SERIAL_COM1_PHYSICAL);
	Bind(wxEVT_BUTTON, &SerialDialog::OnBrowseLogFile1, this, ID_SERIAL_COM1_BROWSE);

	UpdatePortWidgets(*widgets);
	return group;
}

void SerialDialog::UpdatePortWidgets(PortWidgets &widgets)
{
	const bool log_mode = widgets.logfile_radio->GetValue();
	const bool physical_mode = widgets.physical_radio->GetValue();
	widgets.logfile_edit->Enable(log_mode);
	widgets.browse_btn->Enable(log_mode);
	widgets.device_combo->Enable(physical_mode);
}

SerialPortSettings SerialDialog::GetCom1Settings() const
{
	SerialPortSettings settings;
	if (com1_.disabled_radio->GetValue()) {
		settings.mode = SerialPortMode::Disabled;
	} else if (com1_.logfile_radio->GetValue()) {
		settings.mode = SerialPortMode::LogToFile;
	} else if (com1_.tcpmodem_radio->GetValue()) {
		settings.mode = SerialPortMode::TcpModem;
	} else {
		settings.mode = SerialPortMode::PhysicalDevice;
	}
	settings.log_file_path = com1_.logfile_edit->GetValue();
	settings.physical_device = com1_.device_combo->GetValue();
	return settings;
}

void SerialDialog::SetCom1Settings(const SerialPortSettings &settings)
{
	switch (settings.mode) {
	case SerialPortMode::Disabled:
		com1_.disabled_radio->SetValue(true);
		break;
	case SerialPortMode::LogToFile:
		com1_.logfile_radio->SetValue(true);
		break;
	case SerialPortMode::TcpModem:
		com1_.tcpmodem_radio->SetValue(true);
		break;
	case SerialPortMode::PhysicalDevice:
		com1_.physical_radio->SetValue(true);
		break;
	}

	com1_.logfile_edit->SetValue(settings.log_file_path);
	const int idx = com1_.device_combo->FindString(settings.physical_device);
	if (idx != wxNOT_FOUND) {
		com1_.device_combo->SetSelection(idx);
	} else {
		com1_.device_combo->SetValue(settings.physical_device);
	}
	UpdatePortWidgets(com1_);
}

void SerialDialog::OnCom1ModeChanged(wxCommandEvent &) { UpdatePortWidgets(com1_); }

void SerialDialog::OnBrowseLogFile1(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Select Log File", wxStandardPaths::Get().GetDocumentsDir(), wxEmptyString,
	                 "Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*",
	                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() == wxID_OK) {
		com1_.logfile_edit->SetValue(dlg.GetPath());
	}
}

bool SerialDialog::ApplySettings()
{
	wxString warning;
	const SerialPortSettings com1 = GetCom1Settings();

	if (!PeripheralSerialFromDialog(com1,
	                                &peripheral_config.com1_mode,
	                                peripheral_config.com1_log_path,
	                                sizeof(peripheral_config.com1_log_path),
	                                peripheral_config.com1_device,
	                                sizeof(peripheral_config.com1_device),
	                                "Serial",
	                                &warning)) {
		wxMessageBox(warning, "Serial Port Configuration", wxOK | wxICON_WARNING, this);
		return false;
	}

	peripheral_config_apply();
	config_save(&config);

	if (!warning.empty()) {
		wxMessageBox(warning, "Serial Port Configuration", wxOK | wxICON_INFORMATION, this);
	}

	return true;
}

void SerialDialog::OnApply(wxCommandEvent &)
{
	if (ApplySettings()) {
		EndModal(wxID_OK);
	}
}
