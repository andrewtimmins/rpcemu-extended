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
 * package_dialog.h - browsing and installing RISC OS packages.
 *
 * Reached from Tools > Package Manager, so a machine is always running by the
 * time this opens and its disc is the one being installed onto.
 */

#ifndef PACKAGE_DIALOG_H
#define PACKAGE_DIALOG_H

#include <vector>

#include <wx/dialog.h>

#include "package_index.h"
#include "package_install.h"

class wxButton;
class wxListCtrl;
class wxSearchCtrl;
class wxStaticText;
class wxTextCtrl;

class PackageDialog : public wxDialog {
public:
	PackageDialog(wxWindow *parent);

private:
	void BuildUi();
	void Populate();
	void UpdateButtons();
	void RefreshCatalogue(bool force);

	void OnSelect(wxListEvent &event);
	void OnSearch(wxCommandEvent &event);
	void OnInstall(wxCommandEvent &event);
	void OnRemove(wxCommandEvent &event);
	void OnRefresh(wxCommandEvent &event);

	/** The package the list has selected, or null. */
	const PackageRecord *Selected() const;

	/** This machine's disc, which is what gets installed onto. */
	wxString HostfsDir() const;

	std::vector<PackageRecord> catalogue_;
	std::vector<int> shown_;	/* indices into catalogue_, as listed */
	PackageInstalledMap installed_;

	wxListCtrl *list_ = nullptr;
	wxSearchCtrl *search_ = nullptr;
	wxTextCtrl *details_ = nullptr;
	wxStaticText *summary_ = nullptr;
	wxButton *install_button_ = nullptr;
	wxButton *remove_button_ = nullptr;
};

#endif /* PACKAGE_DIALOG_H */
