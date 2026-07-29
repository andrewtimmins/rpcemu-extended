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
 * package_dialog.cpp - browsing and installing RISC OS packages.
 */

#include <wx/button.h>
#include <wx/evtloop.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "package_dialog.h"
#include "riscos_fetch.h"

extern "C" {
#include "rpcemu.h"
}

namespace {

enum {
	COL_NAME = 0,
	COL_VERSION,
	COL_INSTALLED,
	COL_SECTION,
	COL_DESCRIPTION,
};

}  /* namespace */

PackageDialog::PackageDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Package Manager", wxDefaultPosition,
               wxSize(880, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	BuildUi();

	installed_ = PackageInstalledList(HostfsDir());

	/* Show whatever was cached at once, so the window is useful immediately,
	   then only go to the network if there is nothing to show. */
	if (PackageIndexLoadCached(catalogue_)) {
		Populate();
	} else {
		RefreshCatalogue(false);
	}
}

wxString PackageDialog::HostfsDir() const
{
	const char *dir = rpcemu_get_machine_datadir();

	if (dir == NULL || dir[0] == '\0') {
		return wxEmptyString;
	}

	return wxString::FromUTF8(dir) + "hostfs";
}

void PackageDialog::BuildUi()
{
	auto *intro = new wxStaticText(this, wxID_ANY,
	    "Software packaged for RISC OS, from RISC OS Open and the RISC OS "
	    "Community. Installing puts the files on this machine's disc and records "
	    "them, so they can be removed again cleanly.");

	intro->Wrap(840);

	search_ = new wxSearchCtrl(this, wxID_ANY);
	search_->ShowSearchButton(true);
	search_->ShowCancelButton(true);
	search_->SetDescriptiveText("Search names, sections and descriptions");

	auto *refresh = new wxButton(this, wxID_ANY, "Refresh list");

	list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	                       wxLC_REPORT | wxLC_SINGLE_SEL);
	list_->AppendColumn("Package", wxLIST_FORMAT_LEFT, 150);
	list_->AppendColumn("Version", wxLIST_FORMAT_LEFT, 90);
	list_->AppendColumn("Installed", wxLIST_FORMAT_LEFT, 80);
	list_->AppendColumn("Section", wxLIST_FORMAT_LEFT, 110);
	list_->AppendColumn("Description", wxLIST_FORMAT_LEFT, 400);

	details_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
	                          wxSize(-1, 96),
	                          wxTE_MULTILINE | wxTE_READONLY | wxBORDER_THEME);

	summary_ = new wxStaticText(this, wxID_ANY, wxEmptyString);

	install_button_ = new wxButton(this, wxID_ANY, "Install");
	remove_button_ = new wxButton(this, wxID_ANY, "Remove");
	auto *close_button = new wxButton(this, wxID_CANCEL, "Close");

	install_button_->Enable(false);
	remove_button_->Enable(false);

	auto *search_row = new wxBoxSizer(wxHORIZONTAL);
	search_row->Add(search_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	search_row->Add(refresh, 0);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(summary_, 0, wxALIGN_CENTER_VERTICAL);
	buttons->AddStretchSpacer();
	buttons->Add(install_button_, 0, wxRIGHT, 8);
	buttons->Add(remove_button_, 0, wxRIGHT, 8);
	buttons->Add(close_button, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(intro, 0, wxEXPAND | wxALL, 12);
	main->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(details_, 0, wxEXPAND | wxALL, 12);
	main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
	main->Add(buttons, 0, wxEXPAND | wxALL, 12);
	SetSizer(main);
	CentreOnParent();

	list_->Bind(wxEVT_LIST_ITEM_SELECTED, &PackageDialog::OnSelect, this);
	search_->Bind(wxEVT_TEXT, &PackageDialog::OnSearch, this);
	search_->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, &PackageDialog::OnSearch, this);
	install_button_->Bind(wxEVT_BUTTON, &PackageDialog::OnInstall, this);
	remove_button_->Bind(wxEVT_BUTTON, &PackageDialog::OnRemove, this);
	refresh->Bind(wxEVT_BUTTON, &PackageDialog::OnRefresh, this);
}

void PackageDialog::RefreshCatalogue(bool force)
{
	RiscosFetchProgressReporter reporter(this);
	const PackageIndexResult result = PackageIndexRefresh(catalogue_, reporter,
	    []() -> wxEventLoopBase * { return new wxEventLoop; }, force);

	if (!result.ok && !result.cancelled) {
		wxMessageBox(result.message.empty()
		    ? wxString("The package lists could not be fetched.")
		    : result.message, "Package Manager", wxOK | wxICON_WARNING, this);
	}

	Populate();
}

void PackageDialog::Populate()
{
	const wxString filter = search_->GetValue().Lower();
	int unsuitable = 0;

	list_->DeleteAllItems();
	shown_.clear();

	for (size_t i = 0; i < catalogue_.size(); i++) {
		const PackageRecord &pkg = catalogue_[i];

		if (!pkg.RunsHere()) {
			unsuitable++;
			continue;
		}
		if (!filter.empty() && !pkg.name.Lower().Contains(filter) &&
		    !pkg.section.Lower().Contains(filter) &&
		    !pkg.description.Lower().Contains(filter)) {
			continue;
		}

		const auto it = installed_.find(pkg.name);
		const long row = list_->InsertItem(list_->GetItemCount(), pkg.name);

		list_->SetItem(row, COL_VERSION, pkg.version);
		list_->SetItem(row, COL_INSTALLED,
		    it == installed_.end() ? wxString() : it->second);
		list_->SetItem(row, COL_SECTION, pkg.section);
		list_->SetItem(row, COL_DESCRIPTION, pkg.description);
		shown_.push_back(static_cast<int>(i));
	}

	summary_->SetLabel(wxString::Format("%d package%s shown, %d installed",
	    static_cast<int>(shown_.size()), shown_.size() == 1 ? "" : "s",
	    static_cast<int>(installed_.size())));
	UpdateButtons();
}

const PackageRecord *PackageDialog::Selected() const
{
	const long row = list_->GetNextItem(-1, wxLIST_NEXT_ALL,
	                                    wxLIST_STATE_SELECTED);

	if (row < 0 || row >= static_cast<long>(shown_.size())) {
		return nullptr;
	}

	return &catalogue_[shown_[row]];
}

void PackageDialog::UpdateButtons()
{
	const PackageRecord *pkg = Selected();
	const bool have_disc = !HostfsDir().empty() && wxDirExists(HostfsDir());
	const bool is_installed = pkg != nullptr &&
	    installed_.find(pkg->name) != installed_.end();

	install_button_->Enable(pkg != nullptr && have_disc && !is_installed);
	remove_button_->Enable(pkg != nullptr && have_disc && is_installed);
	install_button_->SetLabel(is_installed ? "Reinstall" : "Install");
}

void PackageDialog::OnSelect(wxListEvent &)
{
	const PackageRecord *pkg = Selected();

	if (pkg == nullptr) {
		details_->Clear();
		UpdateButtons();
		return;
	}

	wxString text = pkg->description;

	if (!pkg->description_long.empty()) {
		text += "\n\n" + pkg->description_long;
	}
	text += wxString::Format("\n\nLicence: %s",
	    pkg->licence.empty() ? wxString("not stated") : pkg->licence);
	if (!pkg->maintainer.empty()) {
		text += "\nMaintained by: " + pkg->maintainer;
	}
	if (!pkg->depends.empty()) {
		text += "\nNeeds: " + pkg->depends;
	}
	if (pkg->size > 0) {
		text += wxString::Format("\nDownload: %.0f kB",
		                         static_cast<double>(pkg->size) / 1000.0);
	}

	details_->SetValue(text);
	details_->SetInsertionPoint(0);
	UpdateButtons();
}

void PackageDialog::OnSearch(wxCommandEvent &)
{
	Populate();
}

void PackageDialog::OnRefresh(wxCommandEvent &)
{
	RefreshCatalogue(true);
}

void PackageDialog::OnInstall(wxCommandEvent &)
{
	const PackageRecord *pkg = Selected();

	if (pkg == nullptr) {
		return;
	}

	const wxString hostfs = HostfsDir();
	std::vector<wxString> missing;
	std::vector<PackageRecord> to_install = PackageResolveDepends(*pkg,
	    catalogue_, installed_, missing);

	if (!missing.empty()) {
		wxString names;

		for (const auto &name : missing) {
			names += (names.empty() ? "" : ", ") + name;
		}
		if (wxMessageBox(wxString::Format(
		        "%s says it needs %s, which is not in any of the package lists.\n\n"
		        "Install it anyway?", pkg->name, names),
		        "Package Manager", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
			return;
		}
	}

	if (!to_install.empty()) {
		wxString names;

		for (const auto &dep : to_install) {
			names += (names.empty() ? "" : ", ") + dep.name;
		}
		if (wxMessageBox(wxString::Format(
		        "%s also needs %s.\n\nInstall %s as well?",
		        pkg->name, names, to_install.size() == 1 ? "it" : "them"),
		        "Package Manager", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
			return;
		}
	}

	to_install.push_back(*pkg);

	for (const auto &item : to_install) {
		RiscosFetchProgressReporter reporter(this);
		const PackageActionResult result = PackageInstall(item, hostfs,
		    reporter, []() -> wxEventLoopBase * { return new wxEventLoop; });

		if (result.cancelled) {
			break;
		}
		if (!result.ok) {
			wxMessageBox(result.message, "Could not install",
			             wxOK | wxICON_ERROR, this);
			break;
		}
	}

	installed_ = PackageInstalledList(hostfs);
	Populate();

	wxMessageBox("The files are on the machine's disc now.\n\nIf a Filer window "
	             "for that directory is already open, choose Refresh from its "
	             "menu to see them.", "Package Manager",
	             wxOK | wxICON_INFORMATION, this);
}

void PackageDialog::OnRemove(wxCommandEvent &)
{
	const PackageRecord *pkg = Selected();

	if (pkg == nullptr) {
		return;
	}
	if (wxMessageBox(wxString::Format("Remove %s from this machine?", pkg->name),
	        "Package Manager", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
		return;
	}

	const PackageActionResult result = PackageRemove(pkg->name, HostfsDir());

	if (!result.ok) {
		wxMessageBox(result.message, "Could not remove", wxOK | wxICON_ERROR,
		             this);
	}

	installed_ = PackageInstalledList(HostfsDir());
	Populate();
}
