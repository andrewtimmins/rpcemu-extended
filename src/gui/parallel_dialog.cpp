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

#include "parallel_dialog.h"

#include <cstdio>
#include <cstring>

#include <wx/dirdlg.h>
#include <wx/utils.h>
#include <wx/filedlg.h>
#include <wx/stdpaths.h>

extern "C" {
#include "peripheral_config.h"
#include "print_convert.h"
#include "rpcemu.h"
}

enum {
	ID_PARALLEL_DISABLED = wxID_HIGHEST + 280,
	ID_PARALLEL_LOGFILE,
	ID_PARALLEL_PRINTER,
	ID_PARALLEL_PHYSICAL,
	ID_PARALLEL_LOG_BROWSE,
	ID_PARALLEL_PRINTER_BROWSE,
	ID_PARALLEL_APPLY,
};

static ParallelPortSettings
PeripheralParallelToDialog()
{
	ParallelPortSettings settings;
	char default_printjobs[600];

	settings.mode = static_cast<ParallelPortMode>(peripheral_config.parallel_mode);
	settings.log_file_path = wxString::FromUTF8(peripheral_config.parallel_log_path);
	settings.printer_output_folder = wxString::FromUTF8(peripheral_config.printer_output_path);
	settings.printer_auto_pdf = peripheral_config.printer_auto_pdf != 0;
	settings.physical_device = wxString::FromUTF8(peripheral_config.parallel_device);

	if (settings.mode == ParallelPortMode::VirtualPrinter &&
	    settings.printer_output_folder.Trim().empty()) {
		snprintf(default_printjobs, sizeof(default_printjobs), "%sprintjobs",
		         rpcemu_get_machine_datadir());
		settings.printer_output_folder = wxString::FromUTF8(default_printjobs);
	}

	return settings;
}

/*
 * Printers this computer can already print to: named queues first, then any
 * printer device node that actually exists.
 *
 * Best effort, and an empty result is a normal answer, not an error: the field
 * stays editable, and the dialogue says so rather than offering names that are
 * not there.
 */
/* Shown in place of a printer list when the host has none. Compared by value
   when the dialogue is read back, so it can never be mistaken for a queue name. */
static const char *const kNoPrintersFound = "No printers found";

static wxArrayString
HostPrinters()
{
	wxArrayString printers;

#ifdef __WXMSW__
	/* Enumerating the spooler needs the print API, which the parallel backend
	   does not use, so only the port a device path can reach is offered. */
	if (wxFileExists("\\\\.\\LPT1")) {
		printers.Add("\\\\.\\LPT1");
	}
#else
	{
		wxArrayString output;
		wxArrayString errors;

		/* lpstat lists CUPS destinations, which is what "print on this
		   computer" means on Linux and macOS. */
		if (wxExecute("lpstat -a", output, errors,
		        wxEXEC_SYNC | wxEXEC_NODISABLE) == 0) {
			for (size_t i = 0; i < output.GetCount(); i++) {
				const wxString name = output[i].BeforeFirst(' ');

				if (!name.empty()) {
					printers.Add(name);
				}
			}
		}
	}

	{
		static const char *const nodes[] = {
			"/dev/usb/lp0", "/dev/usb/lp1", "/dev/lp0", "/dev/lp1"
		};
		size_t i;

		for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
			if (wxFileExists(nodes[i])) {
				printers.Add(nodes[i]);
			}
		}
	}
#endif

	return printers;
}

static bool
PeripheralParallelFromDialog(const ParallelPortSettings &settings, wxString *warning)
{
	if (settings.mode == ParallelPortMode::PhysicalDevice) {
		wxString target = settings.physical_device;

		target.Trim(true).Trim(false);
		if (target == kNoPrintersFound) {
			target.clear();
		}
		if (target.empty()) {
			if (warning != nullptr) {
				*warning = "Choose a printer to print to, or type the "
				           "name of a print queue or a device such as "
				           "/dev/usb/lp0.";
			}
			return false;
		}
	}

	if (settings.mode == ParallelPortMode::LogToFile) {
		wxString log_path = settings.log_file_path;
		if (log_path.Trim().empty()) {
			if (warning != nullptr) {
				*warning = "Parallel log mode requires a log file path.";
			}
			return false;
		}
	}

	peripheral_config.parallel_mode = static_cast<PeripheralParallelMode>(settings.mode);

	const wxScopedCharBuffer log_utf8 = settings.log_file_path.utf8_str();
	const wxScopedCharBuffer printer_utf8 = settings.printer_output_folder.utf8_str();
	const wxScopedCharBuffer device_utf8 = settings.physical_device.utf8_str();

	strncpy(peripheral_config.parallel_log_path, log_utf8.data(),
	        sizeof(peripheral_config.parallel_log_path) - 1);
	peripheral_config.parallel_log_path[sizeof(peripheral_config.parallel_log_path) - 1] = '\0';

	strncpy(peripheral_config.printer_output_path, printer_utf8.data(),
	        sizeof(peripheral_config.printer_output_path) - 1);
	peripheral_config.printer_output_path[sizeof(peripheral_config.printer_output_path) - 1] = '\0';
	peripheral_config.printer_auto_pdf = settings.printer_auto_pdf ? 1 : 0;

	strncpy(peripheral_config.parallel_device, device_utf8.data(),
	        sizeof(peripheral_config.parallel_device) - 1);
	peripheral_config.parallel_device[sizeof(peripheral_config.parallel_device) - 1] = '\0';
	return true;
}

ParallelDialog::ParallelDialog(wxWindow *parent)
	: wxDialog(parent, wxID_ANY, "Parallel Port Configuration", wxDefaultPosition, wxSize(520, 420),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxCLOSE_BOX)
{
	BuildUi();
	SetSettings(PeripheralParallelToDialog());
	Fit();
	CentreOnParent();
}

void ParallelDialog::BuildUi()
{
	auto *group = new wxStaticBoxSizer(wxVERTICAL, this, "Parallel Port");

	disabled_radio_ = new wxRadioButton(this, ID_PARALLEL_DISABLED, "Disabled", wxDefaultPosition, wxDefaultSize,
	                                    wxRB_GROUP);
	logfile_radio_ = new wxRadioButton(this, ID_PARALLEL_LOGFILE, "Log to File");
	printer_radio_ = new wxRadioButton(this, ID_PARALLEL_PRINTER, "Virtual Printer");
	physical_radio_ = new wxRadioButton(this, ID_PARALLEL_PHYSICAL,
	    "Print on this computer");

	group->Add(disabled_radio_, 0, wxBOTTOM, 4);

	auto *log_row = new wxBoxSizer(wxHORIZONTAL);
	log_row->Add(logfile_radio_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	logfile_edit_ = new wxTextCtrl(this, wxID_ANY);
	logbrowse_btn_ = new wxButton(this, ID_PARALLEL_LOG_BROWSE, "Browse...");
	log_row->Add(logfile_edit_, 1, wxEXPAND | wxRIGHT, 4);
	log_row->Add(logbrowse_btn_, 0);
	group->Add(log_row, 0, wxEXPAND | wxBOTTOM, 4);

	auto *printer_row = new wxBoxSizer(wxHORIZONTAL);
	printer_row->Add(printer_radio_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	printer_output_edit_ = new wxTextCtrl(this, wxID_ANY);
	printer_browse_btn_ = new wxButton(this, ID_PARALLEL_PRINTER_BROWSE, "Browse...");
	printer_row->Add(printer_output_edit_, 1, wxEXPAND | wxRIGHT, 4);
	printer_row->Add(printer_browse_btn_, 0);
	group->Add(printer_row, 0, wxEXPAND | wxBOTTOM, 4);

	printer_help_ = new wxStaticText(
	    this, wxID_ANY,
	    "Captures Centronics printer data and writes printjob_*.prn files "
	    "when a job ends. Enable PDF conversion to also create a .pdf "
	    "alongside each print job.");
	printer_help_->Wrap(460);
	group->Add(printer_help_, 0, wxEXPAND | wxBOTTOM, 4);

	printer_auto_pdf_checkbox_ = new wxCheckBox(this, wxID_ANY, "Also create PDF files");
	if (!print_convert_available()) {
		printer_auto_pdf_checkbox_->SetToolTip(
		    "Rebuild RPCEmu with GhostPDL/Ghostscript support to enable "
		    "in-process PDF conversion.");
	}
	group->Add(printer_auto_pdf_checkbox_, 0, wxBOTTOM, 4);

	auto *phys_row = new wxBoxSizer(wxHORIZONTAL);
	phys_row->Add(physical_radio_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	phys_row->Add(new wxStaticText(this, wxID_ANY, "Printer:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
	device_combo_ = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
	                               wxCB_DROPDOWN);

	/* The printers this computer has, then any printer device node that is
	   really there. Offering /dev/usb/lp0 on a machine without one is how the
	   old "physical device" list misled people, so nothing goes in this list
	   unless it exists. */
	{
		const wxArrayString printers = HostPrinters();

		for (size_t i = 0; i < printers.GetCount(); i++) {
			device_combo_->Append(printers[i]);
		}
		if (printers.IsEmpty()) {
			device_combo_->Append(kNoPrintersFound);
		}
	}
	phys_row->Add(device_combo_, 1, wxEXPAND);
	group->Add(phys_row, 0, wxEXPAND);

	auto *button_row = new wxBoxSizer(wxHORIZONTAL);
	button_row->AddStretchSpacer();
	auto *apply_btn = new wxButton(this, ID_PARALLEL_APPLY, "Apply");
	auto *cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
	apply_btn->SetDefault();
	button_row->Add(apply_btn, 0, wxRIGHT, 4);
	button_row->Add(cancel_btn, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(group, 0, wxEXPAND | wxALL, 10);
	main->Add(button_row, 0, wxEXPAND | wxALL, 10);
	SetSizer(main);

	Bind(wxEVT_RADIOBUTTON, &ParallelDialog::OnModeChanged, this, ID_PARALLEL_DISABLED, ID_PARALLEL_PHYSICAL);
	logbrowse_btn_->Bind(wxEVT_BUTTON, &ParallelDialog::OnBrowseLogFile, this);
	printer_browse_btn_->Bind(wxEVT_BUTTON, &ParallelDialog::OnBrowsePrinterFolder, this);
	apply_btn->Bind(wxEVT_BUTTON, &ParallelDialog::OnApply, this);
	cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

	UpdateModeWidgets();
}

void ParallelDialog::UpdateModeWidgets()
{
	const bool log_mode = logfile_radio_->GetValue();
	const bool printer_mode = printer_radio_->GetValue();
	const bool physical_mode = physical_radio_->GetValue();

	logfile_edit_->Enable(log_mode);
	logbrowse_btn_->Enable(log_mode);

	printer_output_edit_->Enable(printer_mode);
	printer_browse_btn_->Enable(printer_mode);
	printer_help_->Enable(printer_mode);
	printer_auto_pdf_checkbox_->Enable(printer_mode && print_convert_available() != 0);

	device_combo_->Enable(physical_mode);
}

ParallelPortSettings ParallelDialog::GetSettings() const
{
	ParallelPortSettings settings;
	if (disabled_radio_->GetValue()) {
		settings.mode = ParallelPortMode::Disabled;
	} else if (logfile_radio_->GetValue()) {
		settings.mode = ParallelPortMode::LogToFile;
	} else if (printer_radio_->GetValue()) {
		settings.mode = ParallelPortMode::VirtualPrinter;
	} else {
		settings.mode = ParallelPortMode::PhysicalDevice;
	}

	settings.log_file_path = logfile_edit_->GetValue();
	settings.printer_output_folder = printer_output_edit_->GetValue();
	settings.printer_auto_pdf = printer_auto_pdf_checkbox_->GetValue();
	settings.physical_device = device_combo_->GetValue();
	return settings;
}

void ParallelDialog::SetSettings(const ParallelPortSettings &settings)
{
	switch (settings.mode) {
	case ParallelPortMode::Disabled:
		disabled_radio_->SetValue(true);
		break;
	case ParallelPortMode::LogToFile:
		logfile_radio_->SetValue(true);
		break;
	case ParallelPortMode::VirtualPrinter:
		printer_radio_->SetValue(true);
		break;
	case ParallelPortMode::PhysicalDevice:
		physical_radio_->SetValue(true);
		break;
	}

	logfile_edit_->SetValue(settings.log_file_path);
	printer_output_edit_->SetValue(settings.printer_output_folder);
	printer_auto_pdf_checkbox_->SetValue(settings.printer_auto_pdf);

	const int idx = device_combo_->FindString(settings.physical_device);
	if (idx != wxNOT_FOUND) {
		device_combo_->SetSelection(idx);
	} else {
		device_combo_->SetValue(settings.physical_device);
	}

	UpdateModeWidgets();
}

void ParallelDialog::OnModeChanged(wxCommandEvent &) { UpdateModeWidgets(); }

void ParallelDialog::OnBrowseLogFile(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Select Log File", wxStandardPaths::Get().GetDocumentsDir(), wxEmptyString,
	                 "Log files (*.log;*.txt)|*.log;*.txt|All files (*.*)|*.*",
	                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() == wxID_OK) {
		logfile_edit_->SetValue(dlg.GetPath());
	}
}

void ParallelDialog::OnBrowsePrinterFolder(wxCommandEvent &)
{
	wxDirDialog dlg(this, "Select Printer Output Folder", printer_output_edit_->GetValue());
	if (dlg.ShowModal() == wxID_OK) {
		printer_output_edit_->SetValue(dlg.GetPath());
	}
}

bool ParallelDialog::ApplySettings()
{
	wxString warning;
	const ParallelPortSettings settings = GetSettings();

	if (!PeripheralParallelFromDialog(settings, &warning)) {
		wxMessageBox(warning, "Parallel Port Configuration", wxOK | wxICON_WARNING, this);
		return false;
	}

	peripheral_config_apply();
	config_save(&config);

	if (!warning.empty()) {
		wxMessageBox(warning, "Parallel Port Configuration", wxOK | wxICON_INFORMATION, this);
	}

	return true;
}

void ParallelDialog::OnApply(wxCommandEvent &)
{
	if (ApplySettings()) {
		EndModal(wxID_OK);
	}
}
