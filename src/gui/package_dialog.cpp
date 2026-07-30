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

#include <algorithm>
#include <map>

#include <wx/button.h>
#include <wx/evtloop.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/settings.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/tokenzr.h>
#include <wx/wrapsizer.h>

#include "guest_command.h"
#include "package_dialog.h"
#include "package_sources_dialog.h"
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

/*
 * Ask the machine whether it has the modules a package needs.
 *
 * OSDepends is a comma-separated list, each entry a module name and optionally a
 * version ("SharedCLibrary 5.17"). RMEnsure says nothing when satisfied and
 * reports an error otherwise, so an empty reply is the pass.
 *
 * Returns a description of what is missing, empty if everything is there. A
 * machine that cannot be asked returns empty too: an unanswerable question should
 * not become a warning about the package.
 */
wxString CheckOsDepends(const PackageRecord &pkg,
                        const RiscosFetchLoopFactory &make_loop)
{
	wxString missing;

	if (pkg.osdepends.empty() || !GuestCommandAvailable()) {
		return missing;
	}

	wxStringTokenizer entries(pkg.osdepends, ",", wxTOKEN_STRTOK);

	while (entries.HasMoreTokens()) {
		wxString entry = entries.GetNextToken();

		entry.Trim(true).Trim(false);
		entry.Replace("(", " ");
		entry.Replace(")", "");
		if (entry.empty()) {
			continue;
		}

		const GuestCommandResult result = GuestCommandRun(
		    wxString::Format("RMEnsure %s Error missing", entry), make_loop, 4000);

		if (result.ran && result.rc != 0) {
			missing += wxString::Format("    %s needs %s\n", pkg.name, entry);
		}
	}

	return missing;
}

/*
 * Runs a package's triggers in the machine.
 *
 * Triggers are RISC OS code, so this is simply the guest command channel wearing
 * the interface package_install expects. It keeps the distinction that matters:
 * false means the machine could not be asked, which is not the same as a trigger
 * that ran and said it failed.
 */
class GuestTriggerRunner : public PackageTriggerRunner {
public:
	bool Run(const wxString &command, wxString &output) override
	{
		/* Generous: a trigger may unpack things or poke !Boot, and the policy
		   manual expects it to run in a TaskWindow, which is not instant. */
		const GuestCommandResult result = GuestCommandRun(command,
		    []() -> wxEventLoopBase * { return new wxEventLoop; }, 20000);

		output = result.output;
		if (!result.ran) {
			rpclog("packages: trigger step could not run: %s\n",
			       static_cast<const char *>(result.error.utf8_str()));
		}

		return result.ran;
	}
};

}  /* namespace */

PackageDialog::PackageDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Package Manager", wxDefaultPosition,
               wxSize(880, 620),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	BuildUi();

	installed_ = PackageInstalledList(HostfsDir());


	/* Show whatever was cached at once, so the window is useful immediately,
	   then only go to the network if there is nothing to show. */
	if (PackageIndexLoadCached(catalogue_)) {
		BuildSectionFilters();
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
	    "Software packaged for RISC OS, from the repositories listed under "
	    "Sources. Installing puts the files on this machine's disc and records "
	    "them, so they can be removed again cleanly.");

	intro->Wrap(840);

	search_ = new wxSearchCtrl(this, wxID_ANY);
	search_->ShowSearchButton(true);
	search_->ShowCancelButton(true);
	search_->SetDescriptiveText("Search names, sections and descriptions");

	auto *refresh = new wxButton(this, wxID_ANY, "Refresh list");
	auto *sources = new wxButton(this, wxID_ANY, "Sources...");

	list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	                       wxLC_REPORT | wxLC_SINGLE_SEL);
	list_->AppendColumn("Package", wxLIST_FORMAT_LEFT, 150);
	list_->AppendColumn("Version", wxLIST_FORMAT_LEFT, 90);
	list_->AppendColumn("Installed", wxLIST_FORMAT_LEFT, 80);
	list_->AppendColumn("Section", wxLIST_FORMAT_LEFT, 110);
	list_->AppendColumn("Description", wxLIST_FORMAT_LEFT, 400);

	/* Its own panel so the buttons can be thrown away and remade whenever the
	   catalogue changes, without disturbing anything around them. */
	filter_panel_ = new wxWindow(this, wxID_ANY);
	filter_sizer_ = new wxWrapSizer(wxHORIZONTAL);
	filter_panel_->SetSizer(filter_sizer_);

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
	search_row->Add(refresh, 0, wxRIGHT, 8);
	search_row->Add(sources, 0);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(summary_, 0, wxALIGN_CENTER_VERTICAL);
	buttons->AddStretchSpacer();
	buttons->Add(install_button_, 0, wxRIGHT, 8);
	buttons->Add(remove_button_, 0, wxRIGHT, 8);
	buttons->Add(close_button, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(intro, 0, wxEXPAND | wxALL, 12);
	main->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
	main->Add(filter_panel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
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
	sources->Bind(wxEVT_BUTTON, &PackageDialog::OnSources, this);
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

	/* A fetch can change which sections exist and how big they are. */
	BuildSectionFilters();
	Populate();
}

void PackageDialog::Populate()
{
	const wxString filter = search_->GetValue().Lower();

	list_->DeleteAllItems();
	shown_.clear();

	for (size_t i = 0; i < catalogue_.size(); i++) {
		const PackageRecord &pkg = catalogue_[i];

		if (!pkg.RunsHere()) {
			continue;
		}
		/* The section buttons and the search box narrow together rather
		   than replacing each other: pick Games, then type "pinball". */
		if (!section_filter_.empty() &&
		    pkg.section.CmpNoCase(section_filter_) != 0) {
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

	summary_->SetLabel(wxString::Format("%d package%s shown%s, %d installed",
	    static_cast<int>(shown_.size()), shown_.size() == 1 ? "" : "s",
	    section_filter_.empty() ? wxString()
	                            : wxString::Format(" in %s", section_filter_),
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

/*
 * Which sections earn a button.
 *
 * The catalogue's sections are long-tailed: Games has 185 and a dozen sections
 * have one or two. A button each would be a wall of them, so the ones worth a
 * click get one and the rest stay reachable by typing the section name into the
 * search box, which already matches on it.
 *
 * The cap matters more than the threshold. Filters that push the list of
 * packages off the bottom of the window have taken more than they gave, and
 * without a cap that is exactly what adding a repository or two would do.
 */
static const int kSectionButtonMinimum = 3;
static const size_t kSectionButtonMaximum = 8;

void PackageDialog::BuildSectionFilters()
{
	/* Remade from scratch: the sections depend on the catalogue, and the
	   catalogue changes when a source is added, removed or turned off. */
	filter_sizer_->Clear(true);

	std::map<wxString, int> counts;

	for (const auto &pkg : catalogue_) {
		if (pkg.RunsHere() && !pkg.section.empty()) {
			counts[pkg.section]++;
		}
	}

	/* Biggest first, so the button somebody wants is where they look. Ties
	   go alphabetically, so the order does not wander between refreshes. */
	std::vector<std::pair<wxString, int>> ordered(counts.begin(), counts.end());

	std::sort(ordered.begin(), ordered.end(),
	    [](const std::pair<wxString, int> &a, const std::pair<wxString, int> &b) {
		    if (a.second != b.second) {
			    return a.second > b.second;
		    }
		    return a.first.CmpNoCase(b.first) < 0;
	    });

	const auto add_button = [&](const wxString &label, const wxString &section,
	                            const wxString &tip) {
		auto *button = new wxToggleButton(filter_panel_, wxID_ANY, label,
		                                  wxDefaultPosition, wxDefaultSize,
		                                  wxBU_EXACTFIT);

		button->SetValue(section_filter_.CmpNoCase(section) == 0);
		button->SetToolTip(tip);
		/* The section travels with the button rather than in a parallel
		   array, so nothing can fall out of step when the row is rebuilt. */
		button->SetClientObject(new wxStringClientData(section));
		button->Bind(wxEVT_TOGGLEBUTTON, &PackageDialog::OnSectionFilter, this);
		filter_sizer_->Add(button, 0, wxRIGHT | wxBOTTOM, 4);
	};

	add_button("All", wxEmptyString,
	           "Show every package again, clearing the section filter");

	int shown = 0;

	for (const auto &entry : ordered) {
		if (entry.second < kSectionButtonMinimum ||
		    shown >= static_cast<int>(kSectionButtonMaximum)) {
			break;		/* sorted, so everything after is smaller too */
		}
		add_button(wxString::Format("%s (%d)", entry.first, entry.second),
		           entry.first,
		           wxString::Format("Show only the %d package%s in %s",
		               entry.second, entry.second == 1 ? "" : "s", entry.first));
		shown++;
	}

	if (shown < static_cast<int>(ordered.size())) {
		auto *rest = new wxStaticText(filter_panel_, wxID_ANY,
		    wxString::Format("+%d smaller section%s, try the search box",
		        static_cast<int>(ordered.size()) - shown,
		        ordered.size() - shown == 1 ? "" : "s"));

		rest->SetForegroundColour(
		    wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		filter_sizer_->Add(rest, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
	}

	filter_panel_->Layout();
	Layout();
}

void PackageDialog::OnSectionFilter(wxCommandEvent &event)
{
	auto *button = dynamic_cast<wxToggleButton *>(event.GetEventObject());

	if (button == nullptr) {
		return;
	}

	const auto *data =
	    dynamic_cast<wxStringClientData *>(button->GetClientObject());
	const wxString section = data == nullptr ? wxString() : data->GetData();

	/* Pressing the section already being shown turns it off again, which is
	   what a pressed-in button invites. "All" is the same act by another
	   name, and both leave the search box alone. */
	section_filter_ = (button->GetValue() && !section.empty()) ? section
	                                                           : wxString();

	/* Only one section at a time, so the rest come back up. Rebuilding is
	   simpler than hunting for the previous one and cannot leave two of them
	   looking pressed. */
	BuildSectionFilters();
	Populate();
}

void PackageDialog::OnSources(wxCommandEvent &WXUNUSED(event))
{
	PackageSourcesDialog dialog(this);

	dialog.ShowModal();

	/* Changing where the catalogue comes from and still being shown the old
	   one would read as the change not having taken. Refetch, forcing past
	   the cache, because the cache is per source and says nothing about a
	   source having been added or dropped. */
	if (dialog.Changed()) {
		RefreshCatalogue(true);
		/* A source coming or going changes which sections exist. */
		BuildSectionFilters();
		Populate();
	}
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

	/*
	 * OSDepends names RISC OS modules the package needs. The machine is running,
	 * so it can be asked rather than guessed at: RMEnsure fails with an error if
	 * the module is absent or too old, which is exactly the question.
	 */
	{
		const auto make_loop = []() -> wxEventLoopBase * {
			return new wxEventLoop;
		};
		wxString unmet;

		for (const auto &item : to_install) {
			unmet += CheckOsDepends(item, make_loop);
		}
		unmet += CheckOsDepends(*pkg, make_loop);

		if (!unmet.empty() &&
		    wxMessageBox(wxString::Format(
		        "This machine does not have everything these packages ask for:\n%s\n"
		        "They may not work. Install anyway?", unmet),
		        "Package Manager", wxYES_NO | wxICON_WARNING, this) != wxYES) {
			return;
		}
	}

	to_install.push_back(*pkg);

	GuestTriggerRunner trigger_runner;

	for (const auto &item : to_install) {
		RiscosFetchProgressReporter reporter(this);
		const PackageActionResult result = PackageInstall(item, hostfs,
		    reporter, []() -> wxEventLoopBase * { return new wxEventLoop; },
		    GuestCommandAvailable() ? &trigger_runner : nullptr);

		if (result.cancelled) {
			break;
		}
		if (!result.ok) {
			wxMessageBox(result.message, "Could not install",
			             wxOK | wxICON_ERROR, this);
			break;
		}
		if (!result.message.empty()) {
			/* Installed, but a step after it complained. Worth saying, and not
			   worth treating as a failure. */
			wxMessageBox(result.message, "Installed with a warning",
			             wxOK | wxICON_WARNING, this);
		}
	}

	installed_ = PackageInstalledList(hostfs);
	Populate();

	/*
	 * RISC OS caches directory listings, so files appearing underneath an open
	 * Filer window are not shown until it re-reads.
	 *
	 * There is no * command that refreshes one: *Filer_Boot boots a named
	 * application, and *Filer_OpenDir would open windows the user did not ask
	 * for. Doing it properly means a Wimp message, which is work for the guest
	 * support module rather than a command line. So this still asks.
	 */
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

	GuestTriggerRunner trigger_runner;
	const PackageActionResult result = PackageRemove(pkg->name, HostfsDir(),
	    GuestCommandAvailable() ? &trigger_runner : nullptr);

	if (!result.ok) {
		wxMessageBox(result.message, "Could not remove", wxOK | wxICON_ERROR,
		             this);
	}

	installed_ = PackageInstalledList(HostfsDir());
	Populate();
}
