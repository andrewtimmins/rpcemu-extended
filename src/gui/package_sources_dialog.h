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
 * package_sources_dialog.h - managing the list of package repositories.
 *
 * Opened from the Package Manager. Add, edit, remove and turn sources off, and
 * put the shipped list back. Nothing here fetches anything: it edits the list
 * and says whether it changed, and the Package Manager decides what to do
 * about that.
 */

#ifndef PACKAGE_SOURCES_DIALOG_H
#define PACKAGE_SOURCES_DIALOG_H

#include <vector>

#include <wx/dialog.h>

#include "package_sources.h"

class wxButton;
class wxListCtrl;
class wxStaticText;

class PackageSourcesDialog : public wxDialog {
public:
	explicit PackageSourcesDialog(wxWindow *parent);

	/**
	 * True if the list was written, so the catalogue is now out of step.
	 *
	 * Whoever opened this is expected to refetch when it is true; changing a
	 * source and still seeing the old list would look like nothing happened.
	 */
	bool Changed() const { return changed_; }

private:
	void BuildUi();
	void Populate(long select = -1);
	void UpdateButtons();
	int SelectedRow() const;

	void OnAdd(wxCommandEvent &event);
	void OnEdit(wxCommandEvent &event);
	void OnRemove(wxCommandEvent &event);
	void OnRestoreDefaults(wxCommandEvent &event);
	void OnToggleEnabled(wxCommandEvent &event);
	void OnSelectionChanged(wxListEvent &event);
	void OnActivated(wxListEvent &event);

	/** Write the list out, reporting any failure. True if it was written. */
	bool Save();

	std::vector<PackageSource> sources_;
	bool changed_ = false;

	wxListCtrl *list_ = nullptr;
	wxButton *edit_button_ = nullptr;
	wxButton *remove_button_ = nullptr;
	wxButton *toggle_button_ = nullptr;
	wxStaticText *path_label_ = nullptr;
};

#endif /* PACKAGE_SOURCES_DIALOG_H */
