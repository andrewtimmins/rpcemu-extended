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
 * usb_dialog.cpp - what is plugged into the emulated USB ports.
 *
 * The choices are written into the machine's configuration and then applied, so
 * the configuration is the one account of what is plugged in: a machine started
 * again tomorrow has the same devices, and the dialogue is only a way of editing
 * it. Applying goes through the emulator's command queue, because the controller
 * belongs to that thread.
 *
 * What is on offer is the real devices plugged into this computer. Handing one
 * over is not a neutral act - the host's own driver is detached while the guest
 * has it - so a device the host is using is marked as such and takes a
 * confirmation, and a device that cannot be opened at all says why rather than
 * failing silently later.
 *
 * The dialogue is deliberately blunt about the state of the software on the
 * guest side too. A device that RISC OS has no driver for is still worth
 * attaching, since the emulation can be developed against it, but a user who
 * attaches something and sees nothing happen deserves to be told why rather
 * than left guessing.
 */

#include <wx/statline.h>

#include "emulator_host.h"
#include "usb_dialog.h"

extern "C" {
#include "rpcemu.h"
#include "usb_host.h"
#include "usb_ohci.h"
}

namespace {

/** The most host devices worth listing. Nobody has this many plugged in. */
constexpr int kMaxHostDevices = 64;

/** The width the text in here is wrapped to. */
constexpr int kWrapWidth = 460;

/** A rough name for a class code, for telling one device from another. */
wxString ClassName(uint8_t code)
{
	switch (code) {
	case 0x01: return "audio";
	case 0x02: return "communications";
	case 0x03: return "input";
	case 0x06: return "imaging";
	case 0x07: return "printer";
	case 0x08: return "storage";
	case 0x0b: return "smart card";
	case 0x0e: return "video";
	case 0xe0: return "wireless";
	default:   return wxEmptyString;
	}
}

}  /* namespace */

UsbDialog::UsbDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "USB Devices", wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
	auto *outer = new wxBoxSizer(wxVERTICAL);
	/* The count comes from the card rather than the prose, which said two long
	   after the card had four. */
	auto *intro = new wxStaticText(this, wxID_ANY,
	    wxString::Format("The emulated USB controller has %d ports. Choose what "
	    "is plugged into each; the guest sees it connect or disconnect straight "
	    "away. A real device from this computer is handed over to the guest "
	    "while it is attached, and is not available here in the meantime.",
	    USB_PORTS));
	/*
	 * Two columns and as many rows as it takes. Naming a row count instead
	 * fixes the grid at that many cells, so a card with more ports overflows
	 * it and wx asserts rather than laying it out - which is what happened
	 * when this card went from two ports to four.
	 */
	auto *grid = new wxFlexGridSizer(0, 2, 8, 12);

	intro->Wrap(kWrapWidth);
	outer->Add(intro, 0, wxALL, 12);

	for (int port = 0; port < USB_PORTS; port++) {
		grid->Add(new wxStaticText(this, wxID_ANY,
		    wxString::Format("Port %d:", port + 1)),
		    0, wxALIGN_CENTRE_VERTICAL);

		port_choice_[port] = new wxChoice(this, wxID_ANY, wxDefaultPosition,
		    wxSize(360, wxDefaultCoord));
		port_choice_[port]->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
			UpdateStatus();
		});

		grid->Add(port_choice_[port], 0, wxEXPAND);
	}

	outer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	status_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
	/*
	 * Four lines' worth, always. The text changes with what is attached, and a
	 * label that grows would push the buttons out of a dialogue that has already
	 * been fitted, so the space is reserved rather than negotiated.
	 */
	status_->SetMinSize(wxSize(kWrapWidth,
	    status_->GetCharHeight() * 4 + 6));
	outer->Add(status_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

	outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

	{
		auto *buttons = new wxStdDialogButtonSizer();
		auto *ok = new wxButton(this, wxID_OK, "OK");
		auto *apply = new wxButton(this, wxID_APPLY, "Apply");
		auto *refresh = new wxButton(this, wxID_REFRESH, "Refresh");

		buttons->AddButton(ok);
		buttons->AddButton(apply);
		buttons->AddButton(new wxButton(this, wxID_CANCEL, "Cancel"));
		buttons->Realize();
		/* Refresh belongs with the list rather than with the outcome, so it
		   sits apart from the standard buttons on the left. */
		{
			auto *row = new wxBoxSizer(wxHORIZONTAL);

			row->Add(refresh, 0, wxALIGN_CENTRE_VERTICAL);
			row->AddStretchSpacer();
			row->Add(buttons, 0, wxALIGN_CENTRE_VERTICAL);
			outer->Add(row, 0, wxEXPAND | wxALL, 12);
		}

		ok->Bind(wxEVT_BUTTON, &UsbDialog::OnOk, this);
		apply->Bind(wxEVT_BUTTON, &UsbDialog::OnApply, this);
		refresh->Bind(wxEVT_BUTTON, &UsbDialog::OnRefresh, this);
		ok->SetDefault();
	}

	RebuildChoices();
	SelectFromConfig();
	UpdateStatus();

	SetSizerAndFit(outer);
	CentreOnParent();
}

void UsbDialog::RebuildChoices()
{
	UsbHostDeviceInfo devices[kMaxHostDevices];
	int found;

	choices_.clear();
	choices_.push_back({UsbAttachment_None, "", false, true, "", false, "Nothing"});

	found = usb_host_enumerate(devices, kMaxHostDevices);

	for (int i = 0; i < found; i++) {
		const UsbHostDeviceInfo &d = devices[i];
		char id[USB_HOST_ID_LEN];
		wxString label;
		wxString note = ClassName(d.device_class);

		const char *why = usb_host_open_error_text(&d);

		snprintf(id, sizeof(id), "%04x:%04x", d.vendor, d.product);

		if (why != nullptr) {
			note = note.empty() ? wxString(why) : note + ", " + why;
		} else if (d.in_use == 1) {
			note = note.empty() ? "in use by this computer"
			                    : note + ", in use by this computer";
		}

		label = wxString::Format("%s  %s", id, d.name);
		if (!note.empty()) {
			label += wxString::Format("  (%s)", note);
		}

		choices_.push_back({UsbAttachment_Host, id, d.in_use == 1,
		                    d.openable != 0, why != nullptr ? why : "",
		                    d.high_speed != 0, label});
	}

	for (int port = 0; port < USB_PORTS; port++) {
		const int selected = port_choice_[port]->GetSelection();
		const std::string was = (selected >= 0 &&
		    selected < static_cast<int>(choices_.size()))
		    ? choices_[selected].host_id : std::string();

		port_choice_[port]->Clear();
		for (const Choice &choice : choices_) {
			port_choice_[port]->Append(choice.label);
		}
		port_choice_[port]->SetSelection(0);

		/* Keep the choice across a refresh where the device is still there,
		   so re-reading the list does not quietly unplug anything. */
		if (selected > 0) {
			for (size_t i = 0; i < choices_.size(); i++) {
				if (choices_[i].host_id == was &&
				    (was.empty() ? static_cast<int>(i) == selected : true)) {
					port_choice_[port]->SetSelection(static_cast<int>(i));
					break;
				}
			}
		}
	}
}

void UsbDialog::SelectFromConfig()
{
	for (int port = 0; port < USB_PORTS; port++) {
		int selection = 0;

		for (size_t i = 0; i < choices_.size(); i++) {
			if (choices_[i].kind != config.usb_port[port]) {
				continue;
			}
			if (choices_[i].kind == UsbAttachment_Host &&
			    choices_[i].host_id != config.usb_host[port]) {
				continue;
			}
			selection = static_cast<int>(i);
			break;
		}

		/*
		 * A configured device that is not plugged in now has no line to
		 * select, so it is added as one. Losing it from the configuration
		 * merely because it is unplugged at this moment would be worse:
		 * the machine is meant to find it again when it comes back.
		 */
		if (selection == 0 && config.usb_port[port] == UsbAttachment_Host &&
		    config.usb_host[port][0] != '\0') {
			Choice missing;

			missing.kind = UsbAttachment_Host;
			missing.host_id = config.usb_host[port];
			missing.in_use = false;
			missing.openable = true;
			missing.high_speed = false;
			missing.open_error.clear();
			missing.label = wxString::Format("%s  (not plugged in)",
			    config.usb_host[port]);

			choices_.push_back(missing);
			for (int p = 0; p < 2; p++) {
				port_choice_[p]->Append(missing.label);
			}
			selection = static_cast<int>(choices_.size()) - 1;
		}

		port_choice_[port]->SetSelection(selection);
	}
}

const UsbDialog::Choice *UsbDialog::SelectedChoice(int port) const
{
	const int selection = port_choice_[port]->GetSelection();

	if (selection < 0 || selection >= static_cast<int>(choices_.size())) {
		return nullptr;
	}

	return &choices_[selection];
}

void UsbDialog::UpdateStatus()
{
	wxString text;
	int host = 0;

	for (int port = 0; port < USB_PORTS; port++) {
		const Choice *choice = SelectedChoice(port);

		if (choice == nullptr || choice->kind != UsbAttachment_Host) {
			continue;
		}

		host++;
		if (!choice->openable) {
			text = wxString::Format("That device cannot be opened (%s). "
			    "See docs/usb.md for what your platform needs.",
			    choice->open_error);
		} else if (choice->in_use && text.empty()) {
			text = "That device is in use by this computer. Attaching it "
			       "takes it away from the host until you detach it.";
		} else if (choice->high_speed && text.empty()) {
			text = "That is a high speed device and the emulated "
			       "controller is full speed, so it may not work.";
		}
	}

	if (text.empty()) {
		if (!usb_host_available()) {
			text = usb_host_unavailable_reason();
		} else if (host == 0) {
			text = "Nothing is attached.";
		} else {
			/*
			 * Worth saying plainly. There is no USB stack in the RISC OS 5
			 * IOMD ROM and none on HardDisc4, so a stock guest will not
			 * notice a device at all: the card answers, but nothing is
			 * asking. Attaching one is still useful, since *USBDevices
			 * talks to it directly.
			 */
			text = "RISC OS needs a USB driver to use these, and the ROM does "
			       "not carry one. *USBDevices in the guest will still see "
			       "the device.";
		}
	}

	status_->SetLabel(text);
	status_->Wrap(kWrapWidth);
	Layout();
}

bool UsbDialog::ApplySettings()
{
	EmulatorHost *host = emulator_host_instance();

	/*
	 * Ask first about anything the host is using. Detaching its driver is what
	 * makes the device answer the guest, so doing it to the mouse in the user's
	 * hand would take the mouse away - and they would have no pointer left to
	 * put it back with.
	 */
	for (int port = 0; port < USB_PORTS; port++) {
		const Choice *choice = SelectedChoice(port);

		if (choice == nullptr || choice->kind != UsbAttachment_Host ||
		    !choice->in_use) {
			continue;
		}
		if (config.usb_port[port] == UsbAttachment_Host &&
		    choice->host_id == config.usb_host[port]) {
			continue;	/* already attached, so nothing is being taken */
		}

		if (wxMessageBox(wxString::Format(
		        "%s is in use by this computer.\n\n"
		        "Attaching it to port %d takes it away from the host until you "
		        "detach it again. If it is the keyboard or mouse you are using, "
		        "you will lose it.\n\nAttach it anyway?",
		        choice->label, port + 1),
		        "USB Devices", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
		        this) != wxYES) {
			return false;
		}
	}

	for (int port = 0; port < USB_PORTS; port++) {
		const Choice *choice = SelectedChoice(port);

		if (choice == nullptr) {
			config.usb_port[port] = UsbAttachment_None;
			config.usb_host[port][0] = '\0';
			continue;
		}

		config.usb_port[port] = choice->kind;
		snprintf(config.usb_host[port], sizeof(config.usb_host[port]), "%s",
		    choice->kind == UsbAttachment_Host ? choice->host_id.c_str() : "");
	}

	/*
	 * The same device in both ports would mean opening it twice, and the second
	 * attempt is the one that fails. Say so rather than leaving a port that
	 * silently did nothing.
	 */
	if (config.usb_port[0] == UsbAttachment_Host &&
	    config.usb_port[1] == UsbAttachment_Host &&
	    strcmp(config.usb_host[0], config.usb_host[1]) == 0) {
		wxMessageBox("The same device cannot be in both ports, so port 2 has "
		             "been left empty.", "USB Devices", wxOK | wxICON_INFORMATION,
		             this);
		config.usb_port[1] = UsbAttachment_None;
		config.usb_host[1][0] = '\0';
		port_choice_[1]->SetSelection(0);
	}

	config_save(&config);

	if (host != nullptr) {
		host->ApplyUsbConfig();
	}

	return true;
}

void UsbDialog::OnRefresh(wxCommandEvent &)
{
	RebuildChoices();
	SelectFromConfig();
	UpdateStatus();
}

void UsbDialog::OnApply(wxCommandEvent &)
{
	ApplySettings();
	UpdateStatus();
}

void UsbDialog::OnOk(wxCommandEvent &)
{
	if (ApplySettings()) {
		EndModal(wxID_OK);
	}
}
