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
 * usb_dialog.h - what is plugged into the emulated USB ports.
 *
 * One row per port, as VirtualBox and VMware do it: a device is bound to the
 * emulated bus deliberately, and the guest sees a plug going in or coming out at
 * the moment you say so. As well as the devices RPCEmu synthesises, the list
 * offers the real devices plugged into this computer.
 */

#ifndef USB_DIALOG_H
#define USB_DIALOG_H

#include <string>
#include <vector>

#include <wx/wx.h>

class UsbDialog : public wxDialog {
public:
	explicit UsbDialog(wxWindow *parent);

private:
	/** One line of a port's dropdown, and what choosing it means. */
	struct Choice {
		int kind;		/**< A UsbAttachment */
		std::string host_id;	/**< "vvvv:pppp" for a host device */
		bool in_use;		/**< The host has its own driver bound to it */
		bool openable;		/**< Permissions allow it to be taken at all */
		bool high_speed;	/**< Faster than the emulated controller can be */
		wxString label;
	};

	void OnApply(wxCommandEvent &event);
	void OnOk(wxCommandEvent &event);
	void OnRefresh(wxCommandEvent &event);

	/** Ask the host what is plugged into it and rebuild the dropdowns. */
	void RebuildChoices();

	/** Put the dropdowns back to what the configuration says. */
	void SelectFromConfig();

	/**
	 * Copy the choices into the configuration and tell the machine.
	 *
	 * @return False if the user was warned about something and backed out.
	 */
	bool ApplySettings();

	/** Say what the guest will make of the current state. */
	void UpdateStatus();

	/** The choice a port's dropdown is currently on. */
	const Choice *SelectedChoice(int port) const;

	std::vector<Choice> choices_;
	wxChoice *port_choice_[2];
	wxStaticText *status_;
};

#endif /* USB_DIALOG_H */
