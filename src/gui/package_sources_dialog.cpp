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

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "package_sources_dialog.h"

namespace {

/*
 * Add or edit one source.
 *
 * Validation happens here rather than on the way out of the list, so a name
 * that cannot work is refused while the person is still looking at the field
 * they typed it into, and with the same explanation the file parser would give.
 */
class SourceEditDialog : public wxDialog {
public:
	SourceEditDialog(wxWindow *parent, const wxString &title,
	                 const PackageSource &initial, bool name_is_fixed)
	    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
	               wxDEFAULT_DIALOG_STYLE)
	    , source_(initial)
	{
		name_ = new wxTextCtrl(this, wxID_ANY, initial.name);
		url_ = new wxTextCtrl(this, wxID_ANY, initial.url,
		                      wxDefaultPosition, wxSize(460, -1));
		description_ = new wxTextCtrl(this, wxID_ANY, initial.description);

		if (name_is_fixed) {
			/* Renaming would orphan the cached index under the old name
			   and is not worth the tidying it would need. Remove and add
			   instead, which is what somebody means anyway. */
			name_->Enable(false);
		}

		auto *grid = new wxFlexGridSizer(2, 8, 8);
		grid->AddGrowableCol(1, 1);

		const auto row = [&](const wxString &label, wxWindow *field) {
			grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
			          wxALIGN_CENTER_VERTICAL);
			grid->Add(field, 1, wxEXPAND);
		};

		row("Name", name_);
		row("URL", url_);
		row("Description", description_);

		auto *hint = new wxStaticText(this, wxID_ANY,
		    "The URL is the index file itself, not the page it is linked from. "
		    "The name is used as a filename for that source's cached copy, so it "
		    "is limited to letters, digits, - and _.");

		hint->Wrap(480);

		auto *main = new wxBoxSizer(wxVERTICAL);
		main->Add(grid, 0, wxEXPAND | wxALL, 12);
		main->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
		main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
		main->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
		          wxEXPAND | wxALL, 12);
		SetSizerAndFit(main);
		CentreOnParent();

		Bind(wxEVT_BUTTON, &SourceEditDialog::OnOk, this, wxID_OK);
	}

	const PackageSource &Source() const { return source_; }

private:
	void OnOk(wxCommandEvent &event)
	{
		const wxString name = name_->GetValue().Trim(true).Trim(false);
		const wxString url = url_->GetValue().Trim(true).Trim(false);
		wxString why;

		if (!PackageSourceNameValid(name, &why)) {
			wxMessageBox(wxString::Format("That name will not do: %s.", why),
			             "Add a source", wxOK | wxICON_WARNING, this);
			name_->SetFocus();
			return;
		}
		if (!PackageSourceUrlValid(url, &why)) {
			wxMessageBox(wxString::Format("That URL will not do: %s.", why),
			             "Add a source", wxOK | wxICON_WARNING, this);
			url_->SetFocus();
			return;
		}

		source_.name = name;
		source_.url = url;
		source_.description = description_->GetValue().Trim(true).Trim(false);

		event.Skip();
	}

	PackageSource source_;
	wxTextCtrl *name_ = nullptr;
	wxTextCtrl *url_ = nullptr;
	wxTextCtrl *description_ = nullptr;
};

} /* namespace */

PackageSourcesDialog::PackageSourcesDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Package Sources", wxDefaultPosition,
               wxSize(760, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	sources_ = PackageSourcesLoad();
	BuildUi();
	Populate();
	UpdateButtons();
}

void PackageSourcesDialog::BuildUi()
{
	auto *intro = new wxStaticText(this, wxID_ANY,
	    "Where the package catalogue is fetched from. Each source is an index "
	    "in the RISC OS Packaging Project's format. Turning one off keeps it in "
	    "the list without reading it.");

	intro->Wrap(720);

	list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	                       wxLC_REPORT | wxLC_SINGLE_SEL);
	list_->AppendColumn("Name", wxLIST_FORMAT_LEFT, 110);
	list_->AppendColumn("Enabled", wxLIST_FORMAT_LEFT, 70);
	list_->AppendColumn("URL", wxLIST_FORMAT_LEFT, 330);
	list_->AppendColumn("Description", wxLIST_FORMAT_LEFT, 230);

	auto *add = new wxButton(this, wxID_ANY, "Add...");
	edit_button_ = new wxButton(this, wxID_ANY, "Edit...");
	toggle_button_ = new wxButton(this, wxID_ANY, "Disable");
	remove_button_ = new wxButton(this, wxID_ANY, "Remove");
	auto *defaults = new wxButton(this, wxID_ANY, "Restore defaults");
	auto *close_button = new wxButton(this, wxID_CANCEL, "Close");

	/* Ellipsised in the middle rather than left to be clipped: a data
	   directory path is long, and the end of it is the part worth reading.
	   The full path is in the tooltip for anyone who wants to copy it. */
	path_label_ = new wxStaticText(this, wxID_ANY,
	    "Stored in " + PackageSourcesPath(), wxDefaultPosition, wxDefaultSize,
	    wxST_ELLIPSIZE_MIDDLE);
	path_label_->SetToolTip(PackageSourcesPath());
	path_label_->SetForegroundColour(
	    wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(add, 0, wxRIGHT, 8);
	buttons->Add(edit_button_, 0, wxRIGHT, 8);
	buttons->Add(toggle_button_, 0, wxRIGHT, 8);
	buttons->Add(remove_button_, 0, wxRIGHT, 8);
	buttons->Add(defaults, 0);
	buttons->AddStretchSpacer();
	buttons->Add(close_button, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(intro, 0, wxEXPAND | wxALL, 12);
	main->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(path_label_, 0, wxEXPAND | wxALL, 12);
	main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(buttons, 0, wxEXPAND | wxALL, 12);
	SetSizer(main);
	CentreOnParent();

	add->Bind(wxEVT_BUTTON, &PackageSourcesDialog::OnAdd, this);
	edit_button_->Bind(wxEVT_BUTTON, &PackageSourcesDialog::OnEdit, this);
	toggle_button_->Bind(wxEVT_BUTTON, &PackageSourcesDialog::OnToggleEnabled, this);
	remove_button_->Bind(wxEVT_BUTTON, &PackageSourcesDialog::OnRemove, this);
	defaults->Bind(wxEVT_BUTTON, &PackageSourcesDialog::OnRestoreDefaults, this);
	list_->Bind(wxEVT_LIST_ITEM_SELECTED,
	            &PackageSourcesDialog::OnSelectionChanged, this);
	list_->Bind(wxEVT_LIST_ITEM_DESELECTED,
	            &PackageSourcesDialog::OnSelectionChanged, this);
	list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &PackageSourcesDialog::OnActivated,
	            this);
}

void PackageSourcesDialog::Populate(long select)
{
	list_->DeleteAllItems();

	for (size_t i = 0; i < sources_.size(); i++) {
		const PackageSource &source = sources_[i];
		const long row = list_->InsertItem(static_cast<long>(i), source.name);

		list_->SetItem(row, 1, source.enabled ? "Yes" : "No");
		list_->SetItem(row, 2, source.url);
		list_->SetItem(row, 3, source.description);

		/* A source that is off should read as off at a glance, not only in
		   the column that says so. */
		if (!source.enabled) {
			list_->SetItemTextColour(row,
			    wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		}
	}

	if (select >= 0 && select < static_cast<long>(sources_.size())) {
		list_->SetItemState(select, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
		                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
		list_->EnsureVisible(select);
	}

	UpdateButtons();
}

int PackageSourcesDialog::SelectedRow() const
{
	return static_cast<int>(list_->GetNextItem(-1, wxLIST_NEXT_ALL,
	                                           wxLIST_STATE_SELECTED));
}

void PackageSourcesDialog::UpdateButtons()
{
	const int row = SelectedRow();
	const bool has_selection = row >= 0;

	edit_button_->Enable(has_selection);
	remove_button_->Enable(has_selection);
	toggle_button_->Enable(has_selection);

	/* The button says what it will do, not what the state is. */
	toggle_button_->SetLabel(has_selection && !sources_[row].enabled
	                             ? "Enable" : "Disable");
}

void PackageSourcesDialog::OnSelectionChanged(wxListEvent &event)
{
	UpdateButtons();
	event.Skip();
}

void PackageSourcesDialog::OnActivated(wxListEvent &event)
{
	wxCommandEvent unused;

	OnEdit(unused);
	event.Skip();
}

void PackageSourcesDialog::OnAdd(wxCommandEvent &WXUNUSED(event))
{
	SourceEditDialog dialog(this, "Add a package source", PackageSource(), false);

	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	const PackageSource added = dialog.Source();

	for (const auto &existing : sources_) {
		if (existing.name.CmpNoCase(added.name) == 0) {
			wxMessageBox(wxString::Format(
			    "There is already a source called \"%s\". Edit that one, or "
			    "give this a different name.", existing.name),
			    "Add a source", wxOK | wxICON_WARNING, this);
			return;
		}
	}

	sources_.push_back(added);

	if (Save()) {
		Populate(static_cast<long>(sources_.size()) - 1);
	}
}

void PackageSourcesDialog::OnEdit(wxCommandEvent &WXUNUSED(event))
{
	const int row = SelectedRow();

	if (row < 0) {
		return;
	}

	SourceEditDialog dialog(this, "Edit a package source", sources_[row], true);

	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	const bool enabled = sources_[row].enabled;

	sources_[row] = dialog.Source();
	sources_[row].enabled = enabled;

	if (Save()) {
		Populate(row);
	}
}

void PackageSourcesDialog::OnRemove(wxCommandEvent &WXUNUSED(event))
{
	const int row = SelectedRow();

	if (row < 0) {
		return;
	}

	const wxString name = sources_[row].name;

	if (wxMessageBox(wxString::Format(
	        "Remove the source \"%s\"?\n\nPackages already installed from it "
	        "stay where they are; they simply stop appearing in the "
	        "catalogue.", name),
	        "Remove a source", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
		return;
	}

	sources_.erase(sources_.begin() + row);

	if (Save()) {
		Populate(row < static_cast<int>(sources_.size()) ? row
		                                                 : static_cast<int>(sources_.size()) - 1);
	}
}

void PackageSourcesDialog::OnRestoreDefaults(wxCommandEvent &WXUNUSED(event))
{
	if (wxMessageBox(
	        "Put the shipped list of sources back?\n\nAnything added here is "
	        "lost, and any source turned off is turned back on.",
	        "Restore defaults", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
		return;
	}

	sources_ = PackageSourcesDefaults();

	if (Save()) {
		Populate(0);
	}
}

void PackageSourcesDialog::OnToggleEnabled(wxCommandEvent &WXUNUSED(event))
{
	const int row = SelectedRow();

	if (row < 0) {
		return;
	}

	/* Turning a source off rather than removing it is the useful thing when
	   a repository is down, or slow, or simply not wanted today: the URL is
	   kept, so putting it back is one click and not a re-typed address. */
	sources_[row].enabled = !sources_[row].enabled;

	if (Save()) {
		Populate(row);
	}
}

bool PackageSourcesDialog::Save()
{
	wxString error;

	if (!PackageSourcesSave(sources_, &error)) {
		wxMessageBox(wxString::Format(
		    "The list of sources could not be saved: %s\n\nThe change applies "
		    "to this session only.", error),
		    "Package Sources", wxOK | wxICON_ERROR, this);
		return false;
	}

	changed_ = true;

	return true;
}
