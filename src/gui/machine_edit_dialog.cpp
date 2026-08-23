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

#include "app_settings.h"
#include "machine_edit_dialog.h"
#include "http_transfer.h"	/* RPCEMU_HAVE_HTTP, HttpUnavailableMessage */
#include "riscos_setup_dialog.h"
#include "riscos_fetch.h"

#include "config_paths.h"
#include "folder_transfer.h"

extern "C" {
#include "hostfs_advice.h"
#include "hostfs_path.h"
}
#include "gui_preferences.h"
#include "openbus_coproc.h"
#include "podule_config_dialog.h"
#include "toolbar_icons.h"

#include <cstring>

#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/bmpbuttn.h>
#include <wx/notebook.h>
#include <wx/settings.h>
#include <wx/filename.h>
#include <wx/utils.h>

extern "C" {
#include "romload.h"
#include "rpcemu.h"
#include "gfxcard.h"
#include "podules.h"
#include "podule_config.h"
}

#include "display_options.h"

namespace {

const wxColour kHdColourMissing(120, 120, 120);
const wxColour kHdColourEmpty(180, 120, 0);
const wxColour kHdColourReady(27, 94, 32);
const wxColour kHdColourBlocked(120, 120, 120);

/*
 * Secondary text takes its colour from the theme rather than a grey written
 * down here. A fixed mid-grey is right on a light desktop and nearly invisible
 * on a dark one, and this dialogue had one.
 */
wxColour MutedTextColour()
{
	return wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
}

/*
 * The width to wrap a paragraph at before the dialogue has been laid out.
 *
 * It only has to be a sane starting point for Fit(): unwrapped, one of these
 * paragraphs is a single line thousands of pixels wide and the dialogue would
 * open wider than the display. Everything after the first layout re-wraps to
 * the width the sizer actually hands out, so this number decides the initial
 * shape of the window and nothing else.
 */
const int kNoteInitialWrap = 480;

/* The border every page puts around its contents. */
const int kPageMargin = 10;

wxString FormatHardDiscSize(wxULongLong size_bytes)
{
	if (size_bytes == 0) {
		return "0 bytes";
	}

	const double mb = size_bytes.ToDouble() / (1024.0 * 1024.0);
	if (mb < 1024.0) {
		return wxString::Format("%.1f MB", mb);
	}
	return wxString::Format("%.2f GB", mb / 1024.0);
}

wxString TruncatePathMiddle(const wxString &path, size_t max_len = 58)
{
	if (path.length() <= max_len) {
		return path;
	}

	const size_t keep = max_len - 3;
	const size_t front = keep / 2;
	const size_t back = keep - front;
	return path.substr(0, front) + "..." + path.substr(path.length() - back);
}

wxString FormatModifiedTime(const wxString &path)
{
	wxFileName file(path);
	if (!file.FileExists()) {
		return wxEmptyString;
	}

	const wxDateTime modified = file.GetModificationTime();
	if (!modified.IsValid()) {
		return wxEmptyString;
	}

	return modified.Format("Modified: %d %b %Y, %H:%M");
}

/*
 * Re-fit a wxChoice to the strings it now holds.
 *
 * A choice keeps the width it was measured at, so one whose list is replaced
 * with longer strings after the dialogue was laid out shows them cut off. The
 * best size is right for whatever is in it; this asks for it again and re-runs
 * the layout that used the old one.
 */
void RefitChoice(wxChoice *choice)
{
	wxWindow *const parent = choice->GetParent();

	choice->InvalidateBestSize();
	choice->SetMinSize(choice->GetBestSize());

	if (parent != nullptr && parent->GetSizer() != nullptr) {
		parent->GetSizer()->Layout();
	}
}

} // namespace

enum {
	ID_MACHINE_EDIT_OK = wxID_HIGHEST + 100,
	ID_HD_CREATE_256_MB,
	ID_HD_CREATE_512_MB,
	ID_HD_CREATE_1_GB,
	ID_HD_CREATE_2_GB,
};

MachineEditDialog::MachineEditDialog(wxWindow *parent, const wxString &config_path, bool allow_rename,
                                     bool emulator_running)
	/* Sized from its contents rather than to a fixed height: as a notebook
	   the tallest page decides, and that is a good deal shorter than the one
	   long form this replaced. */
	: wxDialog(parent, wxID_ANY, "Edit Machine", wxDefaultPosition, wxDefaultSize,
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, config_path_(config_path)
	, allow_rename_(allow_rename)
	, emulator_running_(emulator_running)
{
	BuildUi();
	LoadSettings();
	UpdateHdStatus();
	UpdateDiscDownloadAvailability();
	Fit();
	/* Now that the pages have a width, give the paragraphs all of it. Fit()
	   again because a paragraph re-wrapped wider is shorter, and the dialogue
	   was sized around the taller one. */
	WrapNotesToPageWidth();
	Fit();
	CentreOnParent();
}

/*
 * One of the explanatory paragraphs on these pages.
 *
 * Two things are deliberately not decided here: the font, which is the page's
 * own so that every page reads at one size, and the width, which follows
 * whatever the sizer gives the label. Add the result with wxEXPAND and it fills
 * the page; the paragraph then re-wraps itself whenever the dialogue is
 * resized, so a wider window shows longer lines rather than the same short ones
 * with empty space beside them.
 */
wxStaticText *MachineEditDialog::MakeNote(wxWindow *parent, const wxString &text)
{
	auto note = std::make_shared<Note>();

	note->label = new wxStaticText(parent, wxID_ANY, text);
	note->text = text;
	note->label->SetForegroundColour(MutedTextColour());
	note->label->Wrap(kNoteInitialWrap);
	note->wrapped_at = kNoteInitialWrap;

	notes_.push_back(note);
	return note->label;
}

/*
 * Widen every paragraph to the width its page actually has, once.
 *
 * ★ THIS MUST NOT BE DRIVEN FROM A SIZE EVENT, which is how it was written
 * first and why clicking Edit killed the Manager. wxStaticText::Wrap() rewrites
 * the label and resizes the control, GTK allocates it again, and that delivers
 * another wxEVT_SIZE - so a handler that wraps on resize calls itself. It is
 * not even slowed down by only re-wrapping when the width changes, because
 * every Wrap() changes the width. The core showed the cycle 52,000 frames deep:
 * the lambda, Wrap(), DoSetSize(), gtk_widget_size_allocate, the lambda again.
 *
 * So the width is measured once here, after the dialogue has been laid out and
 * from outside any size allocation, and that is the width the paragraph keeps.
 * A paragraph cannot be measured in MakeNote() because inside a notebook a page
 * has no useful width until the dialogue has been sized - which is what the
 * fixed widths written into each call site were working around.
 */
void MachineEditDialog::WrapNotesToPageWidth()
{
	/*
	 * Several passes, because wrapping the text changes how wide the pages want
	 * to be and therefore how wide the dialogue ends up. One pass measured the
	 * window as it was before Fit() had settled it, so the paragraphs kept a
	 * width from an earlier, narrower window and stopped short of the edge -
	 * which is exactly the fault this was supposed to remove, and it showed up
	 * on the Co-Processor Card page where the paragraphs are longest.
	 *
	 * It settles after two, and the third is only there so a page that behaves
	 * unexpectedly cannot leave the text wrapped to a stale width. This is a
	 * plain loop and not a resize handler: it is called once from the
	 * constructor, so nothing it does comes back round to it.
	 */
	for (int pass = 0; pass < 3; pass++) {
		bool changed = false;

		/* An unselected notebook page may not have been laid out, and a
		   paragraph on it would then measure its parent as narrower than it
		   really is. */
		if (notebook_ != nullptr) {
			for (size_t i = 0; i < notebook_->GetPageCount(); i++) {
				notebook_->GetPage(i)->Layout();
			}
		}

		for (const auto &note : notes_) {
			const int width = UsableNoteWidth(note->label);

			if (width <= 0 || width == note->wrapped_at) {
				continue;
			}
			note->wrapped_at = width;
			note->label->SetLabel(note->text);
			note->label->Wrap(width);
			changed = true;
		}

		Layout();
		if (!changed) {
			break;
		}
		/* Let the dialogue settle around the new shape before measuring
		   again: this is what the single pass was missing. */
		Fit();
	}
}

/*
 * How wide a paragraph may be: what its container has, less the border the
 * pages put round their contents.
 *
 * Taken from the container rather than from the label, because a label's own
 * width is a result of the last wrap and measuring it feeds the previous answer
 * back in. The notebook's client area stands in for a page that has not been
 * laid out yet.
 */
int MachineEditDialog::UsableNoteWidth(wxStaticText *label) const
{
	int width = 0;

	if (label->GetParent() != nullptr) {
		width = label->GetParent()->GetClientSize().GetWidth() - 2 * kPageMargin;
	}

	if (width < kNoteInitialWrap && notebook_ != nullptr) {
		const int inner = notebook_->GetClientSize().GetWidth() - 2 * kPageMargin;
		if (inner > width) {
			width = inner;
		}
	}

	return width < kNoteInitialWrap ? kNoteInitialWrap : width;
}

/*
 * Change a paragraph's text. Goes through here rather than SetLabel() because
 * the stored copy is what a later re-wrap starts from: setting the label
 * directly would leave the paragraph wrapped correctly now and wrongly the next
 * time the window is resized.
 */
void MachineEditDialog::SetNoteText(wxStaticText *label, const wxString &text)
{
	for (const auto &note : notes_) {
		if (note->label != label) {
			continue;
		}
		note->text = text;
		note->label->SetLabel(text);
		if (note->wrapped_at > 0) {
			note->label->Wrap(note->wrapped_at);
		}
		return;
	}

	/* Not a registered paragraph: still do the useful thing. */
	label->SetLabel(text);
}

void MachineEditDialog::BuildHardDiscPanel(wxWindow *parent, wxSizer *parent_sizer, HardDiscPanel &panel,
                                           int drive_num, int ide_index)
{
	panel.drive_num = drive_num;

	/* Each drive gets its own framed group. Stacked as plain text the two ran
	   together, and it was not obvious at a glance which path and which
	   buttons belonged to which disc. */
	auto *card = new wxStaticBoxSizer(wxVERTICAL, parent,
	    wxString::Format("HardDisc %d (IDE drive %d)", drive_num, ide_index));
	wxWindow *const drive_panel = card->GetStaticBox();

	panel.badge = new wxStaticText(drive_panel, wxID_ANY, "Not created");
	panel.badge->SetFont(panel.badge->GetFont().Bold());
	card->Add(panel.badge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

	panel.path_label = new wxStaticText(drive_panel, wxID_ANY, wxEmptyString);
	panel.path_label->SetForegroundColour(MutedTextColour());
	card->Add(panel.path_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

	panel.modified_label = new wxStaticText(drive_panel, wxID_ANY, wxEmptyString);
	panel.modified_label->SetForegroundColour(MutedTextColour());
	card->Add(panel.modified_label, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

	auto *actions = new wxBoxSizer(wxHORIZONTAL);
	panel.create_btn = new wxButton(drive_panel, wxID_ANY, "New disc...");
	panel.delete_btn = new wxButton(drive_panel, wxID_ANY, "Delete");
	panel.open_folder_btn = new wxButton(drive_panel, wxID_ANY, "Open folder");
	actions->Add(panel.create_btn, 0, wxRIGHT, 4);
	actions->Add(panel.delete_btn, 0, wxRIGHT, 4);
	actions->Add(panel.open_folder_btn, 0);
	card->Add(actions, 0, wxEXPAND | wxALL, 6);

	parent_sizer->Add(card, 0, wxEXPAND | wxBOTTOM, 10);

	panel.create_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { ShowHardDiscCreateMenu(drive_num); });
	panel.delete_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { DeleteHardDisc(drive_num); });
	panel.open_folder_btn->Bind(wxEVT_BUTTON, [this, drive_num](wxCommandEvent &) { OpenHardDiscFolder(drive_num); });
}

/*
 * The System page: what the machine is made of.
 *
 * Everything here is fixed at the point the machine starts, which is what
 * separates it from the other pages.
 */
/*
 * Which other machine already uses @root as its HostFS folder, or empty.
 *
 * Two machines sharing one folder is the point of discussion #77, so this does
 * not forbid it - it says so, because the failure mode is not obvious: HostFS
 * holds open handles and RISC OS caches directory contents, so two guests
 * writing the same tree at the same time can lose files. Resolved through the
 * same rules the emulator uses rather than compared as raw strings, so a machine
 * that reaches the folder by a relative path is still recognised.
 */
static wxString MachineSharingHostfs(const wxString &root, const wxString &self)
{
	wxArrayString files;
	const wxString dir = ConfigPathsConfigsDir();

	if (!wxDirExists(dir)) {
		return wxEmptyString;
	}
	wxDir::GetAllFiles(dir, &files, "*.cfg", wxDIR_FILES);

	for (const wxString &file : files) {
		const wxString name = wxFileName(file).GetName();

		if (name.IsSameAs(self, false)) {
			continue;
		}

		wxFileConfig cfg(wxEmptyString, wxEmptyString, file, wxEmptyString,
		    wxCONFIG_USE_RELATIVE_PATH);
		ConfigFileUseGeneralGroup(cfg);

		wxString configured;
		cfg.Read("hostfs_path", &configured, wxEmptyString);

		const wxString other_dir =
		    ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() +
		    ConfigPathsSanitizeName(name) + wxFileName::GetPathSeparator();
		char resolved[1024];

		if (!hostfs_path_resolve(configured.utf8_str().data(),
		        other_dir.utf8_str().data(), resolved, sizeof(resolved))) {
			continue;
		}
		if (hostfs_path_same_root(root.utf8_str().data(), resolved)) {
			return name;
		}
	}
	return wxEmptyString;
}

/*
 * The folder the current setting resolves to.
 *
 * Uses the same rules the emulator will, rather than a second copy of them, so
 * the dialogue cannot disagree with what actually happens at startup.
 */
wxString MachineEditDialog::ResolveHostfsValue(const wxString &configured) const
{
	const wxString name = new_name_.empty() ? original_name_ : new_name_;
	const wxString machine_dir =
	    ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() +
	    ConfigPathsSanitizeName(name) + wxFileName::GetPathSeparator();
	char out[1024];

	if (!hostfs_path_resolve(configured.utf8_str().data(),
	        machine_dir.utf8_str().data(), out, sizeof(out))) {
		return machine_dir;
	}
	return wxString::FromUTF8(out);
}

wxString MachineEditDialog::ResolvedHostfsRoot() const
{
	return ResolveHostfsValue(hostfs_edit_ != nullptr
	    ? hostfs_edit_->GetValue().Trim().Trim(false) : wxString());
}

/*
 * Say what the setting will actually mean, and warn about the two things worth
 * warning about: a folder that does not exist yet, and a folder another machine
 * is already using.
 *
 * Said here rather than left to be discovered because a mistyped absolute path
 * is created on startup and then looks like an empty drive, which is
 * indistinguishable from lost files.
 */
void MachineEditDialog::UpdateHostfsNote()
{
	if (hostfs_note_ == nullptr) {
		return;
	}

	const wxString configured = hostfs_edit_->GetValue().Trim().Trim(false);
	const wxString resolved = ResolvedHostfsRoot();
	wxString note;

	if (configured.empty()) {
		note = "This machine's own folder: " + resolved;
	} else {
		note = resolved;

		/*
		 * Both warnings come from hostfs_advice(), which is where the decision
		 * lives and where it is tested. It used to be decided here, in two
		 * branches, and the branch for a folder that does not exist yet said only
		 * that it would be created - never that it would be empty, which is the
		 * case that most needs saying. See src/hostfs_advice.h.
		 *
		 * Only ever warnings. An empty folder is perfectly reasonable if this
		 * machine does not boot from HostFS, or if you are about to put
		 * something in it.
		 */
		const bool exists = wxDirExists(resolved);
		const bool has_boot = exists &&
		    (wxDirExists(resolved + wxFileName::GetPathSeparator() + "!Boot") ||
		     wxFileExists(resolved + wxFileName::GetPathSeparator() + "!Boot"));
		const unsigned advice = hostfs_advice(exists ? 1 : 0, has_boot ? 1 : 0);

		static const unsigned bits[] = {
			HOSTFS_ADVICE_WILL_CREATE,
			HOSTFS_ADVICE_NO_BOOT,
		};
		for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
			if (advice & bits[i]) {
				note += "\n";
				note += hostfs_advice_text(bits[i]);
			}
		}

		const wxString other = MachineSharingHostfs(resolved,
		    new_name_.empty() ? original_name_ : new_name_);
		if (!other.empty()) {
			note += "\nAlso used by '" + other +
			    "'. Do not run both machines at once: RISC OS caches "
			    "directory contents, so two guests writing here together "
			    "can lose files.";
		}
	}

	SetNoteText(hostfs_note_, note);
	Layout();
}

wxWindow *MachineEditDialog::BuildSystemPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);

	name_edit_ = new wxTextCtrl(page, wxID_ANY);
	rom_combo_ = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	get_rom_button_ = new wxButton(page, wxID_ANY, "Get RISC OS...");
	get_rom_button_->SetToolTip("Download a RISC OS ROM from RISC OS Open");
	get_disc_button_ = new wxButton(page, wxID_ANY, "Get hard disc...");
	get_disc_button_->SetToolTip(
	    "Download HardDisc4 from RISC OS Open: applications, utilities, "
	    "!System and a configured !Boot, set up for the ROM this machine "
	    "uses (about 13 MB).\n\n"
	    "Only offered while this machine's hard disc is empty. An existing "
	    "disc is never overwritten.");
	hostfs_edit_ = new wxTextCtrl(page, wxID_ANY);
	hostfs_edit_->SetToolTip(
	    "Where this machine's HostFS drive is on the host.\n\n"
	    "Leave it empty for this machine's own folder, which is what every "
	    "machine has used until now. A relative path is taken from the machine's "
	    "folder and moves with it. An absolute path is used as given, and is how "
	    "several machines can share one folder.");
	hostfs_browse_ = new wxButton(page, wxID_ANY, "Browse...");
	hostfs_note_ = MakeNote(page);

	get_disc_note_ = MakeNote(page);
	model_combo_ = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	mem_combo_ = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	vram_combo_ = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	refresh_slider_ = new wxSlider(page, wxID_ANY, 60, 20, 100);
	refresh_label_ = new wxStaticText(page, wxID_ANY, "60 Hz");
	compat_label_ = MakeNote(page);

	/* Why the RAM/VRAM selectors are fixed on some models. Without this the
	   greyed controls look like a fault rather than the shape of the machine
	   (reported as issue #37). */
	mem_note_ = MakeNote(page);

	hostfs_browse_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
		wxDirDialog dlg(this, "Where should this machine's HostFS drive be?",
		    ResolvedHostfsRoot(), wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);

		if (dlg.ShowModal() == wxID_OK) {
			hostfs_edit_->SetValue(dlg.GetPath());
			UpdateHostfsNote();
		}
	});
	hostfs_edit_->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { UpdateHostfsNote(); });

	PopulateRomList();
	PopulateModelList(Model_MAX);

	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	for (int mem : mem_values) {
		mem_combo_->Append(wxString::Format("%d MB", mem));
	}
	vram_combo_->Append("None");
	vram_combo_->Append("2 MB");
	vram_combo_->Append("4 MB");   /* Phoebe's fixed VRAM */
	vram_combo_->Append("8 MB");
	vram_combo_->Append("16 MB");

	/* The graphics card carries its own display memory, so it is the answer to
	   the VRAM limit rather than another size of it - hence its place here. */
	gfxcard_check_ = new wxCheckBox(page, wxID_ANY, "High-Resolution Graphics Card");
	gfxcard_boot_check_ = new wxCheckBox(page, wxID_ANY, "Make High-Resolution Graphics Card the default display");
	/* Not a graphics setting as such, but this is where what the machine's
	   display costs is decided, and it is the only place a user would look. */
	accelerators_check_ = new wxCheckBox(page, wxID_ANY,
	    "Use Host's Graphics Card to accelerate Sprite Plotting");
	accelerators_check_->SetToolTip(
	    "Some of what RISC OS draws a pixel at a time can be done by this "
	    "computer instead, in one operation, with the same result. On a "
	    "1920 x 1080 desktop that is most of the pixels a redraw copies.\n\n"
	    "Only operations that can be reproduced exactly are taken; everything "
	    "else is left to RISC OS. Debug > Machine Inspector > Accelerators "
	    "reports what was done.");

	fullscreen_check_ = new wxCheckBox(page, wxID_ANY, "Start this machine full screen");
	fullscreen_check_->SetToolTip(
	    "Go full screen as soon as this machine starts, rather than opening a "
	    "window first. Press Alt+Enter or use Settings > Full Screen to leave it.");
	default_machine_check_ = new wxCheckBox(page, wxID_ANY,
	    "Open this machine automatically at startup");
	default_machine_check_->SetToolTip(
	    "Skip the machine selector and open this machine. Hold Shift while "
	    "starting RPCEmu to get the selector back.");
	gfxcard_boot_check_->SetToolTip(
	    "Hand the display to the card as the machine boots, so RISC OS comes up on "
	    "it rather than on VIDC20 - no *GfxCardOn needed.\n\n"
	    "The driver also selects the EDID monitor type for the session if the "
	    "configured one would not offer the card's modes. Your configuration is "
	    "left as it is.");
	gfxcard_check_->SetToolTip(
	    "Fit an expansion card with 15MB of its own display memory, for modes the "
	    "fitted VRAM cannot reach (up to 2560 x 1440 in full colour).\n\n"
	    "RISC OS keeps using VIDC20 until you run *GfxCardOn. Needs RISC OS 5.");

	auto *form = new wxFlexGridSizer(2, 14, 8);
	form->AddGrowableCol(1, 1);
	form->Add(new wxStaticText(page, wxID_ANY, "Name:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(name_edit_, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, "ROM:"), 0, wxALIGN_CENTER_VERTICAL);
	{
		/* The download sits beside the chooser because that is where
		   somebody discovers they have no ROM to choose. */
		auto *rom_row = new wxBoxSizer(wxHORIZONTAL);

		rom_row->Add(rom_combo_, 1, wxEXPAND | wxRIGHT, 8);
		rom_row->Add(get_rom_button_, 0);
		form->Add(rom_row, 1, wxEXPAND);
	}

	/* The disc is its own download, under the ROM it will boot. A button
	   rather than something to tick: it happens when it is pressed, which is
	   what the rest of this row does, and nothing here is applied by OK. */
	form->Add(new wxStaticText(page, wxID_ANY, "Hard disc:"), 0, wxALIGN_CENTER_VERTICAL);
	{
		auto *disc_col = new wxBoxSizer(wxVERTICAL);

		disc_col->Add(get_disc_button_, 0);
		disc_col->Add(get_disc_note_, 0, wxTOP, 2);
		form->Add(disc_col, 1, wxEXPAND);
	}
	form->Add(new wxStaticText(page, wxID_ANY, "HostFS folder:"), 0, wxALIGN_CENTER_VERTICAL);
	{
		auto *hostfs_col = new wxBoxSizer(wxVERTICAL);
		auto *hostfs_row = new wxBoxSizer(wxHORIZONTAL);

		hostfs_row->Add(hostfs_edit_, 1, wxEXPAND | wxRIGHT, 8);
		hostfs_row->Add(hostfs_browse_, 0);
		hostfs_col->Add(hostfs_row, 0, wxEXPAND);
		hostfs_col->Add(hostfs_note_, 0, wxTOP, 2);
		form->Add(hostfs_col, 1, wxEXPAND);
	}
	form->Add(new wxStaticText(page, wxID_ANY, "Model:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(model_combo_, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, "RAM:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(mem_combo_, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, "VRAM:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(vram_combo_, 1, wxEXPAND);

	/* The note sits in the same column as the checkboxes and the ROM message
	   below it, so the three read as one block rather than the note starting
	   somewhere of its own. */
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(mem_note_, 1, wxEXPAND);

	/* "Make it the default display" belongs beside the card it applies to, not
	   on a line of its own where it reads as a separate setting. */
	auto *gfxcard_row = new wxBoxSizer(wxHORIZONTAL);
	gfxcard_row->Add(gfxcard_check_, 0, wxALIGN_CENTER_VERTICAL);
	gfxcard_row->Add(gfxcard_boot_check_, 0,
	                 wxALIGN_CENTER_VERTICAL | wxLEFT, 16);
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(gfxcard_row, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(compat_label_, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(accelerators_check_, 1, wxEXPAND);

	/* How this machine starts: both are statements about this machine rather
	   than preferences about how to display it, so they sit here beside the
	   hardware rather than on the Options page. */
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(fullscreen_check_, 1, wxEXPAND);
	form->Add(new wxStaticText(page, wxID_ANY, ""), 0);
	form->Add(default_machine_check_, 1, wxEXPAND);

	auto *refresh_row = new wxBoxSizer(wxHORIZONTAL);
	refresh_row->Add(refresh_slider_, 1, wxEXPAND | wxRIGHT, 8);
	refresh_row->Add(refresh_label_, 0, wxALIGN_CENTER_VERTICAL);
	form->Add(new wxStaticText(page, wxID_ANY, "Refresh Rate:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(refresh_row, 1, wxEXPAND);

	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(form, 0, wxEXPAND | wxALL, 10);
	page->SetSizer(sizer);
	system_page_ = page;
	return page;
}

/**
 * The Options page: the per-machine switches.
 *
 * These are the same settings as the check items on the Settings menu, which
 * can only be reached once a machine is running and are easy to miss. Here
 * they can be set before it starts, and seen all at once. Both routes write
 * the same configuration, so a change made in either shows up in the other.
 *
 * "Open this machine automatically" is the exception: it is not a property of
 * the machine but of which machine to open, so it lives in the host's own
 * preferences rather than in the configuration file.
 */
/*
 * The display memory the machine WOULD have if the dialog were accepted now.
 *
 * Not rpcemu_display_memory(), which answers for the running machine: the point
 * of asking is to filter the fixed-size list, and somebody who has just fitted
 * the graphics card should see the modes it makes possible without having to
 * save, reopen and look again.
 */
size_t MachineEditDialog::PendingDisplayMemory() const
{
	if (gfxcard_check_ != nullptr && gfxcard_check_->GetValue()) {
		return (size_t) GFXCARD_FB_SIZE;
	}

	/* VRAM combo: 0 = None, 1 = 2 MB, 2 = 4 MB, 3 = 8 MB, 4 = 16 MB. None means
	   screen memory comes out of DRAM, where there is no figure to reason about,
	   so nothing is filtered - the same rule display_mode_fit() applies. */
	static const int vram_sizes[] = { 0, 2, 4, 8, 16 };
	const int sel = vram_combo_ != nullptr
	    ? std::max(0, vram_combo_->GetSelection()) : 0;
	const int mb = vram_sizes[sel < 5 ? sel : 1];

	return (size_t) mb * 1024u * 1024u;
}

void MachineEditDialog::RebuildFixedModeChoice()
{
	if (fixed_mode_choice_ == nullptr) {
		return;
	}

	/* Remember the selection so changing the VRAM does not silently move the
	   chosen mode to whatever happens to land at the same index. */
	unsigned want_x = 0, want_y = 0;
	const int previous = fixed_mode_choice_->GetSelection();
	if (previous != wxNOT_FOUND && (size_t) previous < fixed_modes_.size()) {
		want_x = fixed_modes_[(size_t) previous].first;
		want_y = fixed_modes_[(size_t) previous].second;
	}

	DisplayOptions::FixedModes(PendingDisplayMemory(), fixed_modes_);

	fixed_mode_choice_->Clear();
	for (const auto &mode : fixed_modes_) {
		fixed_mode_choice_->Append(
		    DisplayOptions::ModeLabel(mode.first, mode.second));
	}

	SelectFixedMode(want_x, want_y);
}

void MachineEditDialog::SelectFixedMode(unsigned width, unsigned height)
{
	if (fixed_mode_choice_ == nullptr || fixed_modes_.empty()) {
		return;
	}

	for (size_t i = 0; i < fixed_modes_.size(); i++) {
		if (fixed_modes_[i].first == width && fixed_modes_[i].second == height) {
			fixed_mode_choice_->SetSelection((int) i);
			return;
		}
	}

	/* Not on offer, or nothing asked for: the largest the machine can hold, which
	   is what somebody choosing a fixed size almost always wants. */
	fixed_mode_choice_->SetSelection(0);
}

int MachineEditDialog::SelectedDisplayScaling() const
{
	if (scaling_radio_[DisplayScaling_WholeMultiples] != nullptr &&
	    scaling_radio_[DisplayScaling_WholeMultiples]->GetValue())
	{
		return DisplayScaling_WholeMultiples;
	}

	return DisplayScaling_ActualSize;
}

void MachineEditDialog::SelectedScreenSize(unsigned *width,
                                           unsigned *height) const
{
	const int sel = fixed_mode_choice_ != nullptr
	    ? fixed_mode_choice_->GetSelection() : wxNOT_FOUND;

	if (sel == wxNOT_FOUND || (size_t) sel >= fixed_modes_.size()) {
		*width = 0;
		*height = 0;
		return;
	}

	*width = fixed_modes_[(size_t) sel].first;
	*height = fixed_modes_[(size_t) sel].second;
}

wxWindow *MachineEditDialog::BuildOptionsPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);

	fullscreen_msg_check_ = new wxCheckBox(page, wxID_ANY,
	    "Explain how to leave full screen when entering it");

	/*
	 * The two display choices, worded by display_options.h.
	 *
	 * Every label and tooltip here is the same string the Settings menu shows,
	 * because they are the same settings and reading two different names for one
	 * of them is how somebody concludes there are two. That was the state of
	 * things before: "Pixel Perfect" in the menu against "Pixel perfect
	 * (whole-number scaling)" here, and so on down the list.
	 */
	fixed_mode_choice_ = new wxChoice(page, wxID_ANY);
	fixed_mode_choice_->SetToolTip(DisplayOptions::ScreenSizeHelp());

	scaling_radio_[DisplayScaling_ActualSize] = new wxRadioButton(page, wxID_ANY,
	    DisplayOptions::ScalingActualSize(), wxDefaultPosition, wxDefaultSize,
	    wxRB_GROUP);
	scaling_radio_[DisplayScaling_ActualSize]->SetToolTip(
	    DisplayOptions::ScalingActualSizeHelp());
	scaling_radio_[DisplayScaling_WholeMultiples] = new wxRadioButton(page,
	    wxID_ANY, DisplayOptions::ScalingWholeMultiples());
	scaling_radio_[DisplayScaling_WholeMultiples]->SetToolTip(
	    DisplayOptions::ScalingWholeMultiplesHelp());

	sound_check_ = new wxCheckBox(page, wxID_ANY, "Sound");
	cdrom_check_ = new wxCheckBox(page, wxID_ANY, "CD-ROM drive");
	mouse_twobutton_check_ = new wxCheckBox(page, wxID_ANY, "Two-button mouse");
	cpu_idle_check_ = new wxCheckBox(page, wxID_ANY, "Reduce CPU usage when idle");
	suspend_on_exit_check_ = new wxCheckBox(page, wxID_ANY,
	    "Suspend to a snapshot on exit, instead of shutting down");

	vnc_check_ = new wxCheckBox(page, wxID_ANY, "VNC server");
	vnc_check_->SetToolTip(
	    "Serve this machine's display over VNC. The port and passwords below "
	    "belong to this machine, so several machines can each have their own and "
	    "run at the same time.");
	vnc_port_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString,
	    wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 5900);
	vnc_port_spin_->SetToolTip(
	    "TCP port for this machine's VNC server. Give each machine a different "
	    "one if you want to run more than one at once; 5900 is the usual first.");
	vnc_password_text_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
	    wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
	vnc_password_text_->SetToolTip(
	    "Password VNC clients must give for keyboard and mouse control. Empty "
	    "means no password, which is only "
	    "reasonable on a machine nobody else can reach.");
	vnc_password_readonly_text_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
	    wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
	vnc_password_readonly_text_->SetToolTip(
	    "Optional password for clients that may watch the display but cannot "
	    "use the keyboard, mouse or VNC control menu. Requires a control password.");

	hostcmd_check_ = new wxCheckBox(page, wxID_ANY, "Command socket (HostCmd)");
	hostcmd_check_->SetToolTip(
	    "Let rpcemu-run and rpcemu-shell drive this machine's command line from "
	    "the host.");
	hostcmd_socket_text_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString);
	hostcmd_socket_text_->SetToolTip(
	    "Where to listen: empty for the default socket in the data directory, a "
	    "path for a socket of your own, or a plain number for a TCP port on "
	    "loopback. Give each machine its own to run more than one at once.");

	clipboard_check_ = new wxCheckBox(page, wxID_ANY, "Share the clipboard with RISC OS");
	clipboard_check_->SetToolTip(
	    "Copy and paste text and images between the host and RISC OS. Off by "
	    "default, since it puts the host clipboard within the guest's reach.");


	auto add_group = [page](wxSizer *into, const wxString &title,
	                        std::initializer_list<wxCheckBox *> items) {
		auto *box = new wxStaticBoxSizer(wxVERTICAL, page, title);

		for (wxCheckBox *item : items) {
			box->Add(item, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		}
		box->AddSpacer(6);
		into->Add(box, 0, wxEXPAND | wxBOTTOM, 8);
	};

	auto *sizer = new wxBoxSizer(wxVERTICAL);

	/* Two boxes, one question each, in the same order and with the same names as
	   the Settings menu's two submenus. */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, page,
		    DisplayOptions::ScreenSizeGroup());

		box->Add(fixed_mode_choice_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		box->AddSpacer(6);
		sizer->Add(box, 0, wxEXPAND | wxBOTTOM, 8);
	}
	{
		/* add_group() takes checkboxes alone, so this one goes in by hand. The
		   full-screen explanation sits here because full screen is where the
		   window goes rather than how it is drawn, so it is adjacent to these
		   choices without being one of them. "Start this machine full screen"
		   stays on the hardware page, with the other statement about how the
		   machine starts. */
		auto *box = new wxStaticBoxSizer(wxVERTICAL, page,
		    DisplayOptions::ScalingGroup());

		for (wxRadioButton *button : scaling_radio_) {
			box->Add(button, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		}
		box->Add(fullscreen_msg_check_, 0, wxLEFT | wxRIGHT | wxTOP, 12);
		box->AddSpacer(6);
		sizer->Add(box, 0, wxEXPAND | wxBOTTOM, 8);
	}
	add_group(sizer, "Hardware", { sound_check_, cdrom_check_,
	                               mouse_twobutton_check_ });
	add_group(sizer, "Behaviour", { cpu_idle_check_, suspend_on_exit_check_ });

	/* Host access has fields as well as switches, so it is built here rather than
	   through add_group(), which takes checkboxes alone. The port and passwords are
	   indented under the checkbox that turns them on, and greyed out with it. */
	{
		auto *box = new wxStaticBoxSizer(wxVERTICAL, page, "Host access");

		box->Add(vnc_check_, 0, wxLEFT | wxRIGHT | wxTOP, 6);

		auto *vnc_form = new wxFlexGridSizer(2, 6, 8);
		vnc_form->AddGrowableCol(1, 1);
		vnc_form->Add(new wxStaticText(page, wxID_ANY, "Port:"), 0,
		    wxALIGN_CENTER_VERTICAL);
		vnc_form->Add(vnc_port_spin_, 0);
		vnc_form->Add(new wxStaticText(page, wxID_ANY, "Control password:"), 0,
		    wxALIGN_CENTER_VERTICAL);
		vnc_form->Add(vnc_password_text_, 1, wxEXPAND);
		vnc_form->Add(new wxStaticText(page, wxID_ANY, "Read-only password:"), 0,
		    wxALIGN_CENTER_VERTICAL);
		vnc_form->Add(vnc_password_readonly_text_, 1, wxEXPAND);
		box->Add(vnc_form, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 24);

		box->Add(hostcmd_check_, 0, wxLEFT | wxRIGHT | wxTOP, 6);

		auto *hc_form = new wxFlexGridSizer(2, 6, 8);
		hc_form->AddGrowableCol(1, 1);
		hc_form->Add(new wxStaticText(page, wxID_ANY, "Socket:"), 0,
		    wxALIGN_CENTER_VERTICAL);
		hc_form->Add(hostcmd_socket_text_, 1, wxEXPAND);
		box->Add(hc_form, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 24);

		box->Add(clipboard_check_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		box->AddSpacer(6);
		sizer->Add(box, 0, wxEXPAND | wxBOTTOM, 8);
	}

	/* A port and passwords mean nothing with the server off, and a socket path
	   means nothing with the channel off, so they follow their checkbox. */
	/* Takes the fields by value in a vector, and the stored lambda captures that
	   vector. Not an initializer_list: it does not own its backing array, which
	   dies with the full expression that made it, so a captured one leaves the
	   lambda holding a dangling pointer to use later. */
	auto follow_check = [](wxCheckBox *box, std::vector<wxWindow *> fields) {
		auto apply = [box, fields]() {
			for (wxWindow *field : fields) {
				field->Enable(box->GetValue());
			}
		};

		box->Bind(wxEVT_CHECKBOX, [apply](wxCommandEvent &event) {
			apply();
			event.Skip();
		});
		return apply;
	};

	vnc_fields_follow_ = follow_check(vnc_check_, { vnc_port_spin_,
	    vnc_password_text_, vnc_password_readonly_text_ });
	hostcmd_fields_follow_ = follow_check(hostcmd_check_, { hostcmd_socket_text_ });

	auto *outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(sizer, 1, wxEXPAND | wxALL, 10);
	page->SetSizer(outer);
	return page;
}

/** The Network page: how the machine reaches the outside world. */
wxWindow *MachineEditDialog::BuildNetworkPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);

	network_combo_ = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);

	network_combo_->Append("Off");
	network_combo_->Append("NAT");

	auto *form = new wxFlexGridSizer(2, 8, 8);
	form->AddGrowableCol(1, 1);
	form->Add(new wxStaticText(page, wxID_ANY, "Network:"), 0, wxALIGN_CENTER_VERTICAL);
	form->Add(network_combo_, 1, wxEXPAND);

	auto *note = MakeNote(page,
	    "NAT needs no configuration of this computer and suits every use: the "
	    "guest sits behind it on a private address, and ports can be forwarded "
	    "to it. Changes take effect after reset.");

	/*
	 * Pyromaniac Networking: Charles Ferguson's JSON tun/tap server, which
	 * RISC OS Pyromaniac speaks. Its own block rather than another entry in the
	 * Network combo, because it is not an alternative to NAT - the machine
	 * keeps whatever it has for reaching the outside world and this decides
	 * which other emulators it can see.
	 */
	auto *json_box = new wxStaticBoxSizer(wxVERTICAL, page, "Pyromaniac Networking");
	wxWindow *json_parent = json_box->GetStaticBox();

	json_net_check_ = new wxCheckBox(json_parent, wxID_ANY,
	    "Enable Pyromaniac Networking Support");
	json_net_host_label_ = new wxStaticText(json_parent, wxID_ANY, "Server:");
	json_net_host_edit_ = new wxTextCtrl(json_parent, wxID_ANY, "localhost");
	json_net_port_label_ = new wxStaticText(json_parent, wxID_ANY, "Port:");
	json_net_port_edit_ = new wxSpinCtrl(json_parent, wxID_ANY, wxEmptyString,
	    wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 33445);

	auto *json_form = new wxFlexGridSizer(2, 8, 8);
	json_form->AddGrowableCol(1, 1);
	json_form->Add(json_net_host_label_, 0, wxALIGN_CENTER_VERTICAL);
	json_form->Add(json_net_host_edit_, 1, wxEXPAND);
	json_form->Add(json_net_port_label_, 0, wxALIGN_CENTER_VERTICAL);
	json_form->Add(json_net_port_edit_, 0);

	auto *json_note = MakeNote(json_parent,
	    "Frames are carried to and from a tun/tap JSON server, so this machine "
	    "shares one virtual network with every other emulator connected to it, "
	    "RISC OS Pyromaniac included. The server can run on another computer, "
	    "which is the point: it need not be this one. While this is on, the "
	    "machine does not use the local wire between machines here - both carry "
	    "every frame, and being on both would deliver everything twice. "
	    "Addresses are not handled for you: anything sharing this network needs "
	    "an address on it that does not collide.");

	json_box->Add(json_net_check_, 0, wxALL, 6);
	json_box->Add(json_form, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);
	json_box->Add(json_note, 0, wxEXPAND | wxALL, 6);

	json_net_check_->Bind(wxEVT_CHECKBOX,
	    [this](wxCommandEvent &) { UpdateJsonNetEnabled(); });

	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(form, 0, wxEXPAND | wxALL, 10);
	sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	sizer->Add(json_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	page->SetSizer(sizer);
	return page;
}

/** Grey the server fields when the machine is not joining one. */
void MachineEditDialog::UpdateJsonNetEnabled()
{
	const bool on = json_net_check_ != nullptr && json_net_check_->GetValue();

	if (json_net_host_label_ != nullptr) { json_net_host_label_->Enable(on); }
	if (json_net_host_edit_ != nullptr) { json_net_host_edit_->Enable(on); }
	if (json_net_port_label_ != nullptr) { json_net_port_label_->Enable(on); }
	if (json_net_port_edit_ != nullptr) { json_net_port_edit_->Enable(on); }
}

/** The IDE Drives page: the two emulated hard discs. */
wxWindow *MachineEditDialog::BuildDrivesPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);
	auto *sizer = new wxBoxSizer(wxVERTICAL);

	BuildHardDiscPanel(page, sizer, hd4_panel_, 4, 0);
	BuildHardDiscPanel(page, sizer, hd5_panel_, 5, 1);

	hd_reset_note_ = MakeNote(page,
	    "Changes to hard discs take effect after emulator reset.");
	sizer->Add(hd_reset_note_, 0, wxEXPAND | wxTOP, 6);

	auto *outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(sizer, 1, wxEXPAND | wxALL, 10);
	page->SetSizer(outer);
	return page;
}

/** The Podules page: the expansion backplane. */
wxWindow *MachineEditDialog::BuildPodulesPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);
	auto *outer = new wxBoxSizer(wxVERTICAL);

	outer->Add(BuildPoduleSection(page), 1, wxEXPAND | wxALL, 10);
	page->SetSizer(outer);
	return page;
}


/*
 * The Co-Processor Card page: what is fitted to the OPEN Bus second processor
 * slot.
 *
 * The list of cores comes from openbus_coproc_core_name() rather than being
 * written out here, so the option parser, the machine configuration and this
 * choice cannot drift apart. Entry 0 is the empty slot, which is what every Risc
 * PC had unless somebody bought a card.
 */
wxWindow *MachineEditDialog::BuildCoProcessorPage(wxWindow *parent)
{
	auto *page = new wxPanel(parent);
	auto *sizer = new wxBoxSizer(wxVERTICAL);

	auto *intro = MakeNote(page,
	    "The Risc PC's OPEN Bus is its second processor interface: a card on it "
	    "is a full bus master, not a podule. RPCEmu Extended emulates a "
	    "co-processor card for that slot, with a choice of processor.");
	/* The opening paragraph is the page's own description, not an aside, so it
	   keeps the ordinary text colour. */
	intro->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
	sizer->Add(intro, 0, wxEXPAND | wxBOTTOM, 10);

	auto *row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(new wxStaticText(page, wxID_ANY, "Second processor:"), 0,
	         wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

	copro_choice_ = new wxChoice(page, wxID_ANY);
	/* ★ The order must match the openbus_coproc_core enum, because the
	   selection index is turned into a core by subtracting one. A new core
	   goes on the end of both. */
	copro_choice_->Append("None (empty slot)");
	copro_choice_->Append("RISC-V RV32IM");
	copro_choice_->Append("MOS 6502");
	copro_choice_->Append("Zilog Z80");
	copro_choice_->Append("WDC 65C02 (CMOS 6502)");
	copro_choice_->Append("Intel 8080");
	copro_choice_->Append("Motorola 6809");
	/* One entry, not three: a 6802 and a 6808 are the same instruction set. */
	copro_choice_->Append("Motorola 6800/6802/6808");
	copro_choice_->Append("Motorola 68000");
	copro_choice_->SetSelection(0);
	copro_choice_->SetToolTip(
	    "Fits a co-processor card to the second processor slot. The card carries "
	    "its own RAM: a megabyte for RV32IM, and the whole 64K address space for "
	    "each of the 8-bit cores.");
	row->Add(copro_choice_, 0, wxALIGN_CENTER_VERTICAL);
	sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 10);

	/*
	 * How much RAM the card carries.
	 *
	 * The list is per core rather than fixed, because the ceiling is not a
	 * preference: a 6502 or a Z80 cannot form an address above &FFFF, so 64K
	 * is the whole of what they can reach and offering more would be offering
	 * memory the processor has no way to address. RV32I has a 32-bit space, so
	 * its list runs up to a size chosen for the host's sake instead.
	 */
	auto *ram_row = new wxBoxSizer(wxHORIZONTAL);
	copro_ram_label_ = new wxStaticText(page, wxID_ANY, "Card RAM:");
	ram_row->Add(copro_ram_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

	copro_ram_choice_ = new wxChoice(page, wxID_ANY);
	copro_ram_choice_->SetToolTip(
	    "How much memory the card carries. This is the processor's whole "
	    "address space, so the largest offered is what the fitted processor "
	    "can address: 64K for the 6502 and the Z80, more for RV32IM.\n\n"
	    "A program on the card never sees the host's memory, and the card "
	    "takes this much when the machine starts.");
	ram_row->Add(copro_ram_choice_, 0, wxALIGN_CENTER_VERTICAL);
	sizer->Add(ram_row, 0, wxEXPAND | wxBOTTOM, 10);

	auto *detail = MakeNote(page,
	    "Programs are loaded and run through the RPCEmuCoPro module in the "
	    "guest, which provides the CoPro_* SWIs and the *CoProLoad, *CoProRun, "
	    "*CoProInfo and *CoProStatus commands. Nothing else in RISC OS knows "
	    "about this card: no such card was ever made, so there is no software "
	    "for it beyond what you write yourself. See docs/openbus.md.");
	sizer->Add(detail, 0, wxEXPAND | wxBOTTOM, 10);

	auto *note = MakeNote(page,
	    "Changing the second processor takes effect when this machine next "
	    "starts, as fitting a card would.");
	sizer->Add(note, 0, wxEXPAND);

	auto *outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(sizer, 1, wxEXPAND | wxALL, 10);
	page->SetSizer(outer);
	return page;
}

/*
 * Fill the card RAM list for whichever core is selected, keeping @keep_kb if it
 * is still offered and falling back to the core's default if it is not.
 *
 * Sizes are powers of two from 4K up to what the core can address. The default
 * is marked rather than left to be guessed, because "1 MB" means nothing to
 * somebody who has not read the docs and "1 MB (default)" does.
 */
void MachineEditDialog::RebuildCoProcessorRamChoices(unsigned keep_kb)
{
	const int selection = copro_choice_->GetSelection();
	const bool fitted = selection > 0;

	copro_ram_choice_->Clear();
	copro_ram_sizes_.clear();

	if (!fitted) {
		/* Nothing in the slot: the question does not arise. */
		copro_ram_choice_->Append("-");
		copro_ram_choice_->SetSelection(0);
		copro_ram_choice_->Enable(false);
		copro_ram_label_->Enable(false);
		return;
	}

	{
		const auto core = static_cast<openbus_coproc_core>(selection - 1);
		const unsigned max_kb = openbus_coproc_ram_max(core) / 1024u;
		const unsigned def_kb = openbus_coproc_ram_default(core) / 1024u;
		const unsigned min_kb = OPENBUS_COPROC_RAM_MIN / 1024u;
		int keep_index = -1;
		int def_index = 0;

		const unsigned flat_kb = openbus_coproc_ram_flat_limit(core) / 1024u;

		for (unsigned kb = min_kb; kb <= max_kb; kb *= 2u) {
			wxString label = (kb >= 1024u)
			    ? wxString::Format("%u MB", kb / 1024u)
			    : wxString::Format("%u KB", kb);

			if (kb == def_kb) {
				label += " (default)";
				def_index = (int) copro_ram_sizes_.size();
			} else if (kb > flat_kb) {
				/* Said here rather than left to be discovered: a 6502
				   cannot name an address above &FFFF, so memory beyond
				   its flat space is only reachable by paging it through
				   a window. Offering it silently would look like a
				   promise the processor cannot keep. */
				label += " (needs paging)";
			}
			if (kb == keep_kb) {
				keep_index = (int) copro_ram_sizes_.size();
			}
			copro_ram_sizes_.push_back(kb);
			copro_ram_choice_->Append(label);
		}

		copro_ram_choice_->SetSelection(keep_index >= 0 ? keep_index : def_index);
		copro_ram_choice_->Enable(true);
		copro_ram_label_->Enable(true);
	}

	RefitChoice(copro_ram_choice_);
}

/*
 * The dialog is a notebook of six pages.
 *
 * As one long form it ran to well over 600 pixels and did not fit comfortably
 * on a small display. The split is by what the settings are rather than by
 * size: what the machine is made of, how it reaches the network, its discs,
 * its expansion cards, and what is in its second processor slot.
 *
 * The pages are built in this order because the podule list is rebuilt from
 * the networking selection - the network card occupies a slot - so the
 * Network page has to exist before the Podules page is made.
 */
void MachineEditDialog::BuildUi()
{
	auto *notebook = new wxNotebook(this, wxID_ANY);
	notebook_ = notebook;

	notebook->AddPage(BuildSystemPage(notebook), "System", true);
	notebook->AddPage(BuildOptionsPage(notebook), "Options");
	notebook->AddPage(BuildNetworkPage(notebook), "Network");
	notebook->AddPage(BuildDrivesPage(notebook), "IDE Drives");
	notebook->AddPage(BuildPodulesPage(notebook), "Podules");
	notebook->AddPage(BuildCoProcessorPage(notebook), "Co-Processor Card");

	auto *button_row = new wxBoxSizer(wxHORIZONTAL);
	button_row->AddStretchSpacer();
	auto *ok_button = new wxButton(this, ID_MACHINE_EDIT_OK, "OK");
	auto *cancel_button = new wxButton(this, wxID_CANCEL, "Cancel");
	ok_button->SetDefault();
	button_row->Add(ok_button, 0, wxRIGHT, 4);
	button_row->Add(cancel_button, 0);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(notebook, 1, wxEXPAND | wxALL, 8);
	main->Add(button_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	SetSizer(main);

	refresh_slider_->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
		refresh_label_->SetLabel(wxString::Format("%d Hz", refresh_slider_->GetValue()));
	});
	gfxcard_check_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
		gfxcard_boot_check_->Enable(gfxcard_check_->GetValue());
		/* The card carries 15MB of its own, which changes which fixed screen
		   sizes are possible. Somebody fitting it should see the larger modes
		   appear without saving and reopening the dialog. */
		RebuildFixedModeChoice();
		event.Skip();
	});
	/* Same for the VRAM: it is the other thing that decides what will fit. */
	vram_combo_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) {
		RebuildFixedModeChoice();
		event.Skip();
	});
	copro_choice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
		/* Keep what is selected if the new core can also do it: changing
		   from a Z80 to a 6502 should not silently move a machine off 64K. */
		unsigned keep_kb = 0;

		if (copro_ram_choice_ != nullptr) {
			const int at = copro_ram_choice_->GetSelection();

			if (at >= 0 && (size_t) at < copro_ram_sizes_.size()) {
				keep_kb = copro_ram_sizes_[(size_t) at];
			}
		}
		RebuildCoProcessorRamChoices(keep_kb);
		event.Skip();
	});
	network_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnNetworkChanged, this);
	rom_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnRomOrModelChanged, this);
	get_rom_button_->Bind(wxEVT_BUTTON, &MachineEditDialog::OnGetRiscos, this);
	get_disc_button_->Bind(wxEVT_BUTTON, &MachineEditDialog::OnGetHardDisc, this);
	model_combo_->Bind(wxEVT_COMBOBOX, &MachineEditDialog::OnRomOrModelChanged, this);
	/* 512MB RAM is Kinetic-only: if any other model is selected, snap a 512MB
	   choice back to 256MB immediately so it can't be left selected. */
	mem_combo_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) {
		if (CurrentModelSelection() != Model_Kinetic && mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 256 MB */
		}
		event.Skip();
	});
	name_edit_->Bind(wxEVT_TEXT, &MachineEditDialog::OnNameChanged, this);
	ok_button->Bind(wxEVT_BUTTON, &MachineEditDialog::OnOk, this);
	cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

	hd_reset_note_->Show(emulator_running_);
}

void MachineEditDialog::PopulateRomList()
{
	rom_combo_->Clear();
	const wxString roms_dir = ConfigPathsRomsDir();
	if (!wxDirExists(roms_dir)) {
		rom_combo_->Append("No roms/ directory found",
		                   new wxStringClientData(wxEmptyString));
		return;
	}

	/* An empty rom_dir means "load whatever loose files are in roms/", which
	   is also the state a machine is in before a ROM has been chosen. Worded
	   as the prompt it is in practice, rather than as the mechanism. */
	rom_combo_->Append("Select RISC OS version...", new wxStringClientData(wxEmptyString));

	wxDir dir(roms_dir);
	wxString entry;
	bool has_entries = dir.GetFirst(&entry, wxEmptyString, wxDIR_DIRS);
	while (has_entries) {
		if (entry != "." && entry != "..") {
			rom_combo_->Append(entry + "/", new wxStringClientData(entry));
		}
		has_entries = dir.GetNext(&entry);
	}

	has_entries = dir.GetFirst(&entry, wxEmptyString, wxDIR_FILES);
	while (has_entries) {
		if (!entry.StartsWith(".") && !entry.Lower().EndsWith(".txt")) {
			rom_combo_->Append(entry, new wxStringClientData(entry));
		}
		has_entries = dir.GetNext(&entry);
	}
}

wxString MachineEditDialog::SelectedRomDir() const
{
	const int sel = rom_combo_->GetSelection();
	if (sel < 0) {
		return wxEmptyString;
	}

	/* The client data is what counts, not the label: the first entry and the
	   no-directory entry both carry an empty string, meaning "no particular
	   ROM chosen". Matching on the text instead would break the moment the
	   wording changed, which is exactly what happened once already. */
	auto *data = dynamic_cast<wxStringClientData *>(rom_combo_->GetClientObject(sel));
	if (data != nullptr) {
		return data->GetData();
	}

	/* No client data: a subdirectory entry, shown with a trailing separator. */
	wxString rom_dir = rom_combo_->GetString(sel);
	if (rom_dir.EndsWith("/")) {
		rom_dir.RemoveLast();
	}
	return rom_dir;
}

void MachineEditDialog::SetRomSelection(const wxString &rom_dir)
{
	for (unsigned i = 0; i < rom_combo_->GetCount(); ++i) {
		auto *data = dynamic_cast<wxStringClientData *>(rom_combo_->GetClientObject(i));
		if (data != nullptr && data->GetData() == rom_dir) {
			rom_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}
	rom_combo_->SetSelection(0);
}

/*
 * Offer the hard disc only while there is room for it.
 *
 * A machine's HostFS is the user's, so a download never writes over one. Rather
 * than letting the option be chosen and then refused, it is unavailable while
 * the disc has anything on it, and says why.
 */
void MachineEditDialog::UpdateDiscDownloadAvailability()
{
	if (get_disc_button_ == nullptr) {
		return;
	}

	const bool empty = RiscosFetchMachineDiscIsEmpty(CurrentMachineNameForHd());

	get_disc_button_->Enable(empty);
	if (!empty) {
		get_disc_note_->SetLabel("This machine already has files on its hard "
		                         "disc, which are never overwritten.");
	} else {
		get_disc_note_->SetLabel(wxEmptyString);
	}
	get_disc_note_->Show(!empty);
}

/**
 * Download a ROM and select it here.
 *
 * Only the ROM: this machine already exists and has a hard disc of its own,
 * and replacing that is never something to do as a side effect of choosing a
 * ROM. The disc has its own button beneath, for when that is what is wanted.
 */
void MachineEditDialog::OnGetRiscos(wxCommandEvent &)
{
	if (!RPCEMU_HAVE_HTTP) {
		wxMessageBox(HttpUnavailableMessage() +
		    "\n\nA ROM image can still be pointed at by hand: put one in this "
		    "machine's directory, or fetch it from riscosopen.org yourself.",
		    "Cannot download RISC OS", wxOK | wxICON_INFORMATION, this);
		return;
	}

	RiscosSetupDialog dialog(this, false);
	RiscosFetchOutcome outcome;

	/* The licensing terms are asked for by the dialogue itself, once the
	   version has been chosen. */
	dialog.SetTargetMachine(CurrentMachineNameForHd(), false);
	if (dialog.ShowModal() != wxID_OK) {
		return;
	}

	outcome = dialog.Outcome();
	PopulateRomList();
	SetRomSelection(outcome.rom_name);

	wxCommandEvent dummy;
	OnRomOrModelChanged(dummy);

	UpdateDiscDownloadAvailability();
	UpdateHdStatus();

	wxMessageBox(wxString::Format("RISC OS %s has been downloaded and "
	                              "selected for this machine.",
	                              outcome.version),
	             "RISC OS is ready", wxOK | wxICON_INFORMATION, this);
}

/**
 * Download the ready-made hard disc for this machine.
 *
 * Separate from the ROM because the two are wanted separately: a machine that
 * has its ROM and nothing to boot should not have to fetch the ROM again to
 * get a disc. The disc is set up for the ROM the machine is configured with,
 * which is read from that ROM's name rather than downloaded.
 */
void MachineEditDialog::OnGetHardDisc(wxCommandEvent &)
{
	const wxString machine_name = CurrentMachineNameForHd();
	RiscosFetchRequest request;
	RiscosFetchOutcome outcome;

	if (!RiscosFetchConfirmLicence(this)) {
		return;
	}

	request.include_rom = false;
	request.include_disc = true;
	request.create_machine = false;
	request.machine_name = machine_name;

	{
		RiscosFetchProgressReporter reporter(this);

		outcome = RiscosFetchPerform(request, reporter);
	}

	if (outcome.cancelled) {
		return;
	}

	if (!outcome.ok) {
		wxMessageBox(outcome.message + "\n\nNothing has been changed.",
		             "Could not fetch the hard disc", wxOK | wxICON_ERROR,
		             this);
		return;
	}

	/* It is no longer empty, so the button no longer applies. */
	UpdateDiscDownloadAvailability();
	UpdateHdStatus();

	wxMessageBox(wxString::Format("The hard disc now holds %d files, ready "
	                              "to boot to the desktop.",
	                              outcome.disc_files),
	             "Hard disc is ready", wxOK | wxICON_INFORMATION, this);
}

/**
 * Fill the model list.
 *
 * Two models are left out: Phoebe, the machine Acorn never shipped, and the
 * ARM810, a processor card that never reached the shops either. Both are still
 * implemented and still work if a configuration file names one - this only
 * decides what is offered in the list, so nobody picks a machine that never
 * existed while looking for a Risc PC.
 *
 * A machine already configured for one of them keeps it: pass its model as
 * keep_selectable and it is listed for as long as that dialogue is open, so
 * opening the settings of such a machine and pressing OK does not quietly turn
 * it into something else.
 *
 * @param keep_selectable A model to list even if it is normally hidden,
 *                        or Model_MAX for none
 */
void MachineEditDialog::PopulateModelList(Model keep_selectable)
{
	static const Model hidden[] = { Model_Phoebe, Model_RPCARM810 };

	model_combo_->Clear();
	model_choices_.clear();

	for (int i = 0; i < Model_MAX; ++i) {
		const Model model = static_cast<Model>(i);
		bool skip = false;

		for (Model h : hidden) {
			if (model == h && model != keep_selectable) {
				skip = true;
			}
		}
		if (skip) {
			continue;
		}
		model_combo_->Append(wxString::FromUTF8(models[i].name_gui));
		model_choices_.push_back(model);
	}
}

/** Select a model by value, listing it first if it is one of the hidden ones. */
void MachineEditDialog::SelectModel(Model model)
{
	for (size_t i = 0; i < model_choices_.size(); ++i) {
		if (model_choices_[i] == model) {
			model_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}

	/* Not listed, so this machine is configured for one of the hidden models.
	   Put it in rather than leaving the list on something else. */
	PopulateModelList(model);
	for (size_t i = 0; i < model_choices_.size(); ++i) {
		if (model_choices_[i] == model) {
			model_combo_->SetSelection(static_cast<int>(i));
			return;
		}
	}
}

Model MachineEditDialog::CurrentModelSelection() const
{
	const int sel = model_combo_->GetSelection();

	if (sel >= 0 && static_cast<size_t>(sel) < model_choices_.size()) {
		return model_choices_[static_cast<size_t>(sel)];
	}
	return Model_RPCARM710;
}

/*
 * Grow the dialogue to fit its contents, never shrink it.
 *
 * The notes appear and disappear as the model changes, and a taller note needs
 * a taller page than the one the dialogue was sized for. Shrinking again on
 * the way back would make the window jump about as somebody looked through the
 * model list, so this only ever gives more room.
 */
void MachineEditDialog::GrowToFitContents()
{
	if (system_page_ != nullptr) {
		system_page_->Layout();
	}
	Layout();

	const wxSize best = GetBestSize();
	const wxSize current = GetSize();

	if (best.x > current.x || best.y > current.y) {
		SetSize(wxSize(wxMax(best.x, current.x), wxMax(best.y, current.y)));
		Layout();
	}
}

/* Show (or clear) the note under the VRAM selector, and let the dialog resize
   around it. Wrapping is done here because wxStaticText will not do it itself. */
void MachineEditDialog::SetMemoryNote(const char *text)
{
	if (mem_note_ == nullptr) {
		return;
	}
	if (text == nullptr || text[0] == '\0') {
		SetNoteText(mem_note_, wxEmptyString);
		mem_note_->Show(false);
	} else {
		SetNoteText(mem_note_, wxString::FromUTF8(text));
		mem_note_->Show(true);
	}
	GrowToFitContents();
}

void MachineEditDialog::UpdateRomModelCompatibility()
{
	char detail[64] = "";
	char msg[512] = "";
	const Model model = CurrentModelSelection();

	/* Constrain the RAM/VRAM selectors to what each model actually supports,
	   mirroring the core's own clamps (settings.cpp config_load and rpcemu.c
	   config_apply) so the dialog can never present an unsupported combination.
	     mem_values[] index: 6 = 256MB, 7 = 512MB.
	     vram combo index:   0 = None, 1 = 2MB, 2 = 4MB, 3 = 8MB, 4 = 16MB. */
	switch (model) {
	case Model_Kinetic:
		/* Defined by its 512MB (two on-card SDRAM banks), so there is nothing to
		   choose. VRAM is clamped to 2MB because 512MB of RAM and more VRAM
		   together overrun the RISC OS memory map; the graphics card answers the
		   ceiling that leaves, carrying its own display memory and reaching modes
		   no amount of VRAM would offer here. See docs/kinetic.md. */
		mem_combo_->SetSelection(7);   /* 512 MB */
		mem_combo_->Enable(false);
		vram_combo_->SetSelection(1);  /* 2 MB */
		vram_combo_->Enable(false);
		/* Broken deliberately after the first sentence: what the machine is,
		   then what to do about it. */
		SetMemoryNote("The Kinetic has 512MB of RAM and 2MB of VRAM.\n"
		              "If you wish to have high-resolution modes available, "
		              "please enable the High-Resolution Graphics Card option "
		              "below.");
		break;

	case Model_Phoebe:
		/* Phoebe (RPC2) is a fixed 256MB RAM + 4MB VRAM machine. */
		mem_combo_->SetSelection(6);   /* 256 MB */
		mem_combo_->Enable(false);
		vram_combo_->SetSelection(2);  /* 4 MB */
		vram_combo_->Enable(false);
		SetMemoryNote("Phoebe is a fixed 256MB with 4MB of VRAM.");
		break;

	case Model_A7000:
	case Model_A7000plus:
		/* The A7000 family has no VRAM at all - video runs from main DRAM.
		   RAM stays user-selectable up to the 256MB motherboard limit. */
		vram_combo_->SetSelection(0);  /* None */
		vram_combo_->Enable(false);
		if (mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 512MB is Kinetic-only */
		}
		mem_combo_->Enable(true);
		SetMemoryNote("The A7000 has no VRAM: video runs from main memory.");
		break;

	default:
		/* Risc PC models: RAM up to 256MB (512MB is Kinetic-only), VRAM free. */
		if (mem_combo_->GetSelection() == 7) {
			mem_combo_->SetSelection(6); /* 512MB is Kinetic-only -> drop to 256MB */
		}
		mem_combo_->Enable(true);
		vram_combo_->Enable(true);
		SetMemoryNote("");
		break;
	}

	const wxString rom_dir = SelectedRomDir();
	const wxScopedCharBuffer rom_dir_utf8 = rom_dir.utf8_str();

	if (!rom_model_is_compatible(model, rom_dir_utf8.data(), msg, sizeof(msg))) {
		SetNoteText(compat_label_, wxString::FromUTF8(msg));
		compat_label_->SetForegroundColour(wxColour(176, 0, 32));
		GrowToFitContents();
		return;
	}

	const RomAddressing addressing =
		rom_probe_addressing(rom_dir_utf8.data(), detail, sizeof(detail));

	wxString label;
	if (addressing == RomAddressing_32Bit) {
		label = "32-bit ROM - OK for this model";
	} else if (addressing == RomAddressing_26Bit) {
		label = "26-bit ROM - OK for this model";
	} else {
		SetNoteText(compat_label_,
		    "ROM type unknown - 26-bit CPUs need RISC OS 3.xx ROMs");
		compat_label_->SetForegroundColour(wxColour(27, 94, 32));
		GrowToFitContents();
		return;
	}

	if (detail[0] != '\0') {
		label += wxString(" (") + wxString::FromUTF8(detail) + wxString(")");
	}

	SetNoteText(compat_label_, label);
	compat_label_->SetForegroundColour(wxColour(27, 94, 32));

	/* This is also where a model change clamps the VRAM (a Kinetic to 2MB, for
	   one), and it does that without a combo event, so the fixed screen sizes
	   have to be recalculated from here as well. */
	RebuildFixedModeChoice();
	GrowToFitContents();
}

/*
 * The backplane. No enclosing group box: this has a page to itself now, and a
 * box labelled "Podules" inside a tab labelled "Podules" says it twice.
 */
wxSizer *MachineEditDialog::BuildPoduleSection(wxWindow *parent)
{
	auto *box = new wxBoxSizer(wxVERTICAL);
	wxWindow *const p = parent;

	/* Snapshot the available podules once. */
	const int count = podule_get_available_count();
	for (int i = 0; i < count; i++) {
		const char *sn = podule_get_available_short_name(i);
		const char *nm = podule_get_available_name(i);
		podule_available_.emplace_back(wxString::FromUTF8(sn ? sn : ""),
		                               wxString::FromUTF8(nm ? nm : (sn ? sn : "?")));
	}

	/* One slot per row: label, combo, configure button. Two slots abreast
	   made the whole dialog about half as wide again as anything else in it
	   needed, and on a page of its own there is room to go down instead. */
	auto *grid = new wxFlexGridSizer(0, 3, 6, 8);
	grid->AddGrowableCol(1, 1);

	for (int i = 0; i < PODULE_CONFIG_SLOTS; i++) {
		/*
		 * The low slots carry the cards RPCEmu fits itself and are shown
		 * rather than offered. The vectors stay indexed by slot number so
		 * that everything else can go on saying "slot i" and mean it.
		 */
		if (i < PODULE_CONFIG_FIRST_USER_SLOT) {
			grid->Add(new wxStaticText(p, wxID_ANY,
			              wxString::Format("Slot %d:", i)),
			          0, wxALIGN_CENTER_VERTICAL);
			grid->Add(new wxStaticText(p, wxID_ANY,
			              i == 0 ? "USB card (built in)"
			                     : "RPCEmu support card (built in)"),
			          1, wxALIGN_CENTER_VERTICAL);
			grid->Add(new wxStaticText(p, wxID_ANY, ""), 0);

			podule_combos_.push_back(nullptr);
			podule_config_btns_.push_back(nullptr);
			podule_selection_.push_back("");
			podule_item_names_.emplace_back();
			continue;
		}

		grid->Add(new wxStaticText(p, wxID_ANY, wxString::Format("Slot %d:", i)),
		          0, wxALIGN_CENTER_VERTICAL);

		auto *choice = new wxChoice(p, wxID_ANY);
		choice->Bind(wxEVT_CHOICE, &MachineEditDialog::OnPoduleChanged, this);
		grid->Add(choice, 1, wxEXPAND);

		/* A cog rather than a label. This was an ellipsis, which rendered
		   as an empty button: the glyph is not in every interface font. */
		auto *cfg_btn = new wxBitmapButton(p, wxID_ANY,
		                                   ToolbarIconConfigure(wxSize(16, 16)));
		cfg_btn->SetToolTip("Configure this podule");
		cfg_btn->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { OnPoduleConfigure(i); });
		grid->Add(cfg_btn, 0, wxALIGN_CENTER_VERTICAL);

		podule_combos_.push_back(choice);
		podule_config_btns_.push_back(cfg_btn);
		podule_selection_.push_back("");
		podule_item_names_.emplace_back();
	}
	box->Add(grid, 0, wxEXPAND);

	auto *note = MakeNote(p,
	    "Eight expansion-card slots. Slot 0 (Support) and the network card are "
	    "built-in. A podule can only be assigned to one slot; changes take "
	    "effect after reset.");
	box->Add(note, 0, wxEXPAND | wxTOP, 8);

	RebuildPoduleChoices();
	return box;
}

/* Rebuild every slot's dropdown for the 8-slot backplane view:
    - slot 0 is the built-in RPCEmu Support ROM (locked),
    - slot 1 is the network card when networking is enabled (locked),
    - the rest are user-assignable, and a podule chosen in one slot is hidden
      from the others (one slot per podule). */
void MachineEditDialog::RebuildPoduleChoices()
{
	if (podule_combos_.empty()) {
		return;
	}

	const bool network_on = network_combo_ != nullptr && network_combo_->GetSelection() > 0;

	for (size_t i = 0; i < podule_combos_.size(); i++) {
		wxChoice *combo = podule_combos_[i];

		if (combo == nullptr) {
			continue;	/* a built-in card's slot */
		}
		std::vector<wxString> &names = podule_item_names_[i];

		combo->Clear();
		names.clear();

		/* Built-in (reserved) slots: shown locked, not user-assignable. */
		wxString builtin;
		if (i == 0) {
			builtin = "RPCEmu Support (built-in)";
		} else if (i == 1 && network_on) {
			builtin = "RPCEmu Ethernet (built-in)";
		}
		if (!builtin.IsEmpty()) {
			combo->Append(builtin);
			names.push_back("");
			combo->SetSelection(0);
			combo->Disable();
			if (i < podule_config_btns_.size()) {
				podule_config_btns_[i]->Enable(false);
			}
			podule_selection_[i] = ""; /* never a user podule */
			continue;
		}

		combo->Enable();
		combo->Append("(None)");
		names.push_back("");

		for (const auto &pa : podule_available_) {
			const wxString &short_name = pa.first;

			bool used_elsewhere = false;
			for (size_t j = 0; j < podule_selection_.size(); j++) {
				if (j != i && podule_selection_[j] == short_name) {
					used_elsewhere = true;
					break;
				}
			}
			if (used_elsewhere) {
				continue;
			}

			combo->Append(pa.second);
			names.push_back(short_name);
		}

		int sel = 0;
		for (size_t k = 0; k < names.size(); k++) {
			if (names[k] == podule_selection_[i]) {
				sel = static_cast<int>(k);
				break;
			}
		}
		combo->SetSelection(sel);

		/* The configure button is active only when the slot holds a podule
		   that exposes a configuration schema. */
		if (i < podule_config_btns_.size()) {
			bool has_config = false;
			if (!podule_selection_[i].IsEmpty()) {
				const podule_header_t *h =
				    podule_find(podule_selection_[i].utf8_str().data());
				has_config = (h != nullptr && h->config != nullptr);
			}
			podule_config_btns_[i]->Enable(has_config);
		}
	}
}

void MachineEditDialog::OnPoduleConfigure(int slot)
{
	if (slot < 0 || slot >= static_cast<int>(podule_selection_.size())) {
		return;
	}
	const wxString short_name = podule_selection_[slot];
	if (short_name.IsEmpty()) {
		return;
	}

	const podule_header_t *header = podule_find(short_name.utf8_str().data());
	if (header == nullptr || header->config == nullptr) {
		return;
	}

	const wxString section = wxString::Format("%s.%d", short_name, slot);
	const wxString title = wxString::Format("Configure %s",
	    wxString::FromUTF8(header->name ? header->name : header->short_name));

	PoduleConfigDialog dlg(this, title, header->config, &podule_kv_[section]);
	dlg.ShowModal();
}

void MachineEditDialog::OnPoduleChanged(wxCommandEvent &event)
{
	for (size_t i = 0; i < podule_combos_.size(); i++) {
		if (podule_combos_[i] == nullptr ||
		    podule_combos_[i] != event.GetEventObject()) {
			continue;
		}
		const int sel = podule_combos_[i]->GetSelection();
		if (sel >= 0 && sel < static_cast<int>(podule_item_names_[i].size())) {
			podule_selection_[i] = podule_item_names_[i][sel];
		} else {
			podule_selection_[i] = "";
		}
		break;
	}
	RebuildPoduleChoices();
}

void MachineEditDialog::LoadPoduleSettings(wxFileConfig &settings)
{
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS &&
	     i < static_cast<int>(podule_selection_.size()); i++) {
		wxString val;
		if (i >= PODULE_CONFIG_FIRST_USER_SLOT &&
		    settings.Read(wxString::Format("slot%d", i), &val) && !val.IsEmpty()) {
			podule_selection_[i] = val;
		} else {
			podule_selection_[i] = "";
		}
	}

	/* Per-podule key/value config: [PoduleConfig/<section>] key=value. Collect
	   group names first - changing SetPath mid-enumeration breaks the iterator. */
	podule_kv_.clear();
	settings.SetPath("/PoduleConfig");
	wxArrayString groups;
	wxString group;
	long gidx;
	for (bool c = settings.GetFirstGroup(group, gidx); c; c = settings.GetNextGroup(group, gidx)) {
		groups.Add(group);
	}
	for (size_t gi = 0; gi < groups.GetCount(); gi++) {
		const wxString &section = groups[gi];
		settings.SetPath("/PoduleConfig/" + section);
		wxString entry;
		long eidx;
		for (bool e = settings.GetFirstEntry(entry, eidx); e; e = settings.GetNextEntry(entry, eidx)) {
			wxString value;
			settings.Read(entry, &value);
			podule_kv_[section][entry] = value;
		}
		settings.SetPath("/PoduleConfig");
	}

	settings.SetPath("/General");
	RebuildPoduleChoices();
}

void MachineEditDialog::SavePoduleSettings(wxFileConfig &settings)
{
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS &&
	     i < static_cast<int>(podule_selection_.size()); i++) {
		settings.Write(wxString::Format("slot%d", i), podule_selection_[i]);
		/* Keep the live store in step so the next reset installs the new
		   selection (for the running machine; harmless otherwise). */
		podule_cfg_set_slot(i, podule_selection_[i].utf8_str().data());
	}

	/* Persist per-podule key/value config and mirror it into the live store. */
	for (const auto &section : podule_kv_) {
		settings.SetPath("/PoduleConfig/" + section.first);
		for (const auto &kv : section.second) {
			settings.Write(kv.first, kv.second);
			podule_cfg_set_string(section.first.utf8_str().data(),
			                      kv.first.utf8_str().data(),
			                      kv.second.utf8_str().data());
		}
	}

	settings.SetPath("/General");
}

void MachineEditDialog::LoadSettings()
{
	loading_settings_ = true;
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path_, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);

	settings.Read("name", &original_name_, wxEmptyString);
	if (original_name_.empty()) {
		original_name_ = wxFileName(config_path_).GetName();
	}
	name_edit_->SetValue(original_name_);

	wxString rom_dir;
	settings.Read("rom_dir", &rom_dir, wxEmptyString);
	SetRomSelection(rom_dir);

	wxString model_name;
	settings.Read("model", &model_name, "RPCSA");
	if (model_name == "RPCARM610") model_name = "RPC610";
	if (model_name == "RPCARM710") model_name = "RPC710";
	if (model_name == "RPCARM810") model_name = "RPC810";
	for (int i = 0; i < Model_MAX; ++i) {
		if (model_name == wxString::FromUTF8(models[i].name_config)) {
			SelectModel(static_cast<Model>(i));
			break;
		}
	}

	long mem_size = 64;
	settings.Read("mem_size", &mem_size, 64L);
	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	for (int i = 0; i < static_cast<int>(sizeof(mem_values) / sizeof(mem_values[0])); ++i) {
		if (static_cast<long>(mem_values[i]) == mem_size) {
			mem_combo_->SetSelection(i);
			break;
		}
	}

	wxString vram_text;
	settings.Read("vram_size", &vram_text, "2");
	int vram_index = 1; /* "2 MB" */
	if (vram_text == "0") {
		vram_index = 0;
	} else if (vram_text == "4") {
		vram_index = 2;
	} else if (vram_text == "8") {
		vram_index = 3;
	} else if (vram_text == "16") {
		vram_index = 4;
	}
	vram_combo_->SetSelection(vram_index);

	/* The second processor. Read as a name and matched against the core list,
	   so an unrecognised name in a hand-edited configuration shows as an empty
	   slot rather than as whichever core happened to be first. */
	wxString copro;
	settings.Read("openbus_card", &copro, wxEmptyString);
	copro_choice_->SetSelection(0);
	for (int core = OPENBUS_COPROC_RV32I; core <= OPENBUS_COPROC_Z80; core++) {
		const char *name =
		    openbus_coproc_core_name(static_cast<openbus_coproc_core>(core));

		if (name != nullptr && copro.IsSameAs(wxString::FromUTF8(name), false)) {
			copro_choice_->SetSelection(core + 1);
			break;
		}
	}

	{
		long ram_kb = 0;

		settings.Read("openbus_ram_kb", &ram_kb, 0);
		RebuildCoProcessorRamChoices(ram_kb > 0 ? (unsigned) ram_kb : 0u);
	}

	long gfxcard = 0;
	long accelerators = 1;
	settings.Read("gfxcard_enabled", &gfxcard, 0L);
	gfxcard_check_->SetValue(gfxcard != 0);
	/* On unless this machine has said otherwise, which is also what a machine
	   configured before the setting existed gets. */
	settings.Read("accelerators_enabled", &accelerators, 1L);
	accelerators_check_->SetValue(accelerators != 0);
	long gfxcard_boot = 0;
	settings.Read("gfxcard_boot_display", &gfxcard_boot, 0L);
	gfxcard_boot_check_->SetValue(gfxcard_boot != 0);
	gfxcard_boot_check_->Enable(gfxcard != 0);

	/* Options page. The defaults here have to match the ones settings.cpp
	   uses when it reads the same keys, or opening the editor and pressing
	   OK would silently change a setting that was never touched. */
	auto read_flag = [&settings](const wxString &key, long fallback) {
		long value = fallback;

		settings.Read(key, &value, fallback);
		return value != 0;
	};

	fullscreen_check_->SetValue(read_flag("start_fullscreen", 0));
	fullscreen_msg_check_->SetValue(read_flag("show_fullscreen_message", 1));
	/* The display settings, and the switches they grew out of. Same conversion
	   as settings.cpp, for the same reason: a machine written before any of this
	   has only the old keys, and opening its editor must show what it has actually
	   been running with. */
	{
		long value = 0;
		const bool old_fit = read_flag("fit_to_window", 0);
		const bool old_integer = read_flag("integer_scaling", 0);
		long fixed_x = 0, fixed_y = 0;

		const long scaling_default = (old_integer && !old_fit)
		    ? DisplayScaling_WholeMultiples : DisplayScaling_ActualSize;

		settings.Read("display_scaling", &value, scaling_default);
		scaling_radio_[DisplayOptions::ClampDisplayScaling((int) value)]
		    ->SetValue(true);

		settings.Read("screen_size_x", &fixed_x, 0L);
		settings.Read("screen_size_y", &fixed_y, 0L);

		RebuildFixedModeChoice();
		SelectFixedMode((unsigned) (fixed_x > 0 ? fixed_x : 0),
		                (unsigned) (fixed_y > 0 ? fixed_y : 0));
	}
	sound_check_->SetValue(read_flag("sound_enabled", 1));
	cdrom_check_->SetValue(read_flag("cdrom_enabled", 0));
	mouse_twobutton_check_->SetValue(read_flag("mouse_twobutton", 0));
	cpu_idle_check_->SetValue(read_flag("cpu_idle", 0));
	suspend_on_exit_check_->SetValue(read_flag("suspend_on_exit", 0));
	/* Same layering as config_load(): the app settings file supplies what this
	   machine's file has not got, so a machine written while these keys were
	   app-wide opens showing the values it has actually been running with rather
	   than the built-in defaults. */
	{
		Config defaults;

		memset(&defaults, 0, sizeof(defaults));
		defaults.vnc_enabled = 0;
		defaults.vnc_port = 5900;
		defaults.hostcmd_enabled = 1;
		app_settings_load(rpcemu_get_datadir(), &defaults);

		vnc_check_->SetValue(read_flag("vnc_enabled", defaults.vnc_enabled ? 1 : 0));
		hostcmd_check_->SetValue(
		    read_flag("hostcmd_enabled", defaults.hostcmd_enabled ? 1 : 0));

		long port = defaults.vnc_port;
		settings.Read("vnc_port", &port, port);
		vnc_port_spin_->SetValue(static_cast<int>(port));

		wxString text;
		settings.Read("vnc_password", &text,
		    wxString::FromUTF8(defaults.vnc_password));
		vnc_password_text_->SetValue(text);
		settings.Read("vnc_password_readonly", &text,
		    wxString::FromUTF8(defaults.vnc_password_readonly));
		vnc_password_readonly_text_->SetValue(text);
		settings.Read("hostcmd_socket", &text,
		    wxString::FromUTF8(defaults.hostcmd_socket));
		hostcmd_socket_text_->SetValue(text);
	}
	vnc_fields_follow_();
	hostcmd_fields_follow_();
	clipboard_check_->SetValue(read_flag("clipboard_enabled", 0));

	/* Not a setting of this machine but of which machine to open, so it is
	   read from the host's preferences rather than the configuration file. */
	default_machine_check_->SetValue(
	    !original_name_.empty() &&
	    wxString::FromUTF8(GetDefaultMachine()) == original_name_);

	long refresh = 60;
	settings.Read("refresh_rate", &refresh, 60L);
	refresh = std::max(20L, std::min(100L, refresh));
	refresh_slider_->SetValue(static_cast<int>(refresh));
	refresh_label_->SetLabel(wxString::Format("%ld Hz", refresh));

	wxString network_type;
	settings.Read("network_type", &network_type, "off");
	/* Anything that is not "off" shows as NAT, which is what a machine
	   configured for bridging or tunnelling is now given. */
	const int net_index = network_combo_->FindString(
		network_type == "off" ? "Off" : "NAT");
	if (net_index != wxNOT_FOUND) {
		network_combo_->SetSelection(net_index);
	}

	{
		long json_on = 0, json_port = 33445;
		wxString json_host;

		settings.Read("json_net_enabled", &json_on, 0L);
		settings.Read("json_net_host", &json_host, "localhost");
		settings.Read("json_net_port", &json_port, 33445L);
		json_net_check_->SetValue(json_on != 0);
		json_net_host_edit_->SetValue(json_host);
		json_net_port_edit_->SetValue(static_cast<int>(json_port));
		UpdateJsonNetEnabled();
	}

	settings.Read("hd4_path", &hd4_path_, wxEmptyString);

	{
		wxString hostfs;

		settings.Read("hostfs_path", &hostfs, wxEmptyString);
		hostfs_edit_->SetValue(hostfs);
		/* Kept so OK can tell whether the folder changed, and offer to bring
		   the files across if it did. */
		original_hostfs_ = hostfs;
		UpdateHostfsNote();
	}

	long cdrom_enabled = 0;
	settings.Read("cdrom_enabled", &cdrom_enabled, 0L);
	cdrom_enabled_ = cdrom_enabled != 0;

	wxCommandEvent dummy;
	OnNetworkChanged(dummy);
	loading_settings_ = false;

	LoadPoduleSettings(settings);

	CallAfter([this]() { UpdateRomModelCompatibility(); });

	if (!allow_rename_) {
		name_edit_->Disable();
	}
}

void MachineEditDialog::SaveSettings()
{
	wxFileConfig settings(wxEmptyString, wxEmptyString, config_path_, wxEmptyString, wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);

	if (!allow_rename_) {
		new_name_ = original_name_;
	} else {
		new_name_ = ConfigPathsSanitizeName(name_edit_->GetValue());
		new_name_.Trim(true).Trim(false);
		if (new_name_.empty()) {
			new_name_ = original_name_;
		}
	}

	const int mem_values[] = {4, 8, 16, 32, 64, 128, 256, 512};
	int mem_sel = std::max(0, mem_combo_->GetSelection());
	const int vram_sel = std::max(0, vram_combo_->GetSelection());
	const Model model_sel = CurrentModelSelection();

	/* Record the configured model in the global config so it persists on save.
	   (config_save writes cfg->model, not the running machine.model, so a model
	   change to a running machine is no longer reverted when the config saves.) */
	config.model = model_sel;

	/* VRAM combo: 0 = None, 1 = 2 MB, 2 = 4 MB, 3 = 8 MB, 4 = 16 MB */
	static const int vram_sizes[] = { 0, 2, 4, 8, 16 };
	int vram_mb = vram_sizes[vram_sel < 5 ? vram_sel : 1];

	/* Enforce the same per-model RAM/VRAM limits as UpdateRomModelCompatibility
	   (and the core clamps), in case a combo was left in a stale state. */
	switch (model_sel) {
	case Model_Kinetic:
		mem_sel = 7; vram_mb = 2; break;	/* 512MB + 2MB (HAL clamp) */
	case Model_Phoebe:
		mem_sel = 6; vram_mb = 4; break;	/* fixed 256MB + 4MB */
	case Model_A7000:
	case Model_A7000plus:
		if (mem_sel == 7) { mem_sel = 6; }	/* 512MB is Kinetic-only */
		vram_mb = 0; break;			/* no VRAM (video from DRAM) */
	default:
		if (mem_sel == 7) { mem_sel = 6; }	/* 512MB is Kinetic-only */
		break;
	}

	const wxString rom_dir = SelectedRomDir();

	wxString network_type = "off";
	const wxString network_label = network_combo_->GetStringSelection();
	if (network_label == "NAT") {
		network_type = "nat";
	}

	/*
	 * ★ Written to the LIVE CONFIG as well as to the file, and not only to the
	 * file, which is what it did at first and was wrong.
	 *
	 * Opened from the machine selector nothing else holds a Config, so writing
	 * the file was enough and it appeared to work. Opened from a running
	 * machine's Settings > Machine, the emulator is holding a Config that still
	 * has the OLD value, and the next config_save() writes that back over the
	 * file - so the setting silently reverted. Reported by David Ramsden.
	 *
	 * Updating it here also earns the restart prompt for nothing:
	 * MachineNeedsRestart() memcmp()s the whole structure, so a changed
	 * hostfs_path is now a change it can see, and the machine is offered a
	 * restart exactly as it is for any other hardware-shaped setting.
	 */
	{
		const wxString hostfs = hostfs_edit_->GetValue().Trim().Trim(false);

		settings.Write("hostfs_path", hostfs);

		if (snprintf(config.hostfs_path, sizeof(config.hostfs_path), "%s",
		        hostfs.utf8_str().data()) >= (int) sizeof(config.hostfs_path)) {
			/* Emptied rather than truncated, matching config_load(): a
			   truncated path is a different directory, and HostFS would create
			   it and put the guest's files there. */
			config.hostfs_path[0] = '\0';
		}
	}
	settings.Write("name", new_name_);
	settings.Write("rom_dir", rom_dir);
	settings.Write("model", wxString::FromUTF8(models[model_sel].name_config));
	settings.Write("mem_size", wxString::Format("%d", mem_values[mem_sel]));
	settings.Write("vram_size", wxString::Format("%d", vram_mb));
	settings.Write("accelerators_enabled",
	               static_cast<long>(accelerators_check_->GetValue() ? 1 : 0));
	/* Selection 0 is the empty slot, so the core names start at 1. */
	{
		const int selection = copro_choice_->GetSelection();
		wxString name;

		if (selection > 0) {
			const char *core = openbus_coproc_core_name(
			    static_cast<openbus_coproc_core>(selection - 1));

			if (core != nullptr) {
				name = wxString::FromUTF8(core);
			}
		}
		settings.Write("openbus_card", name);

		/* Written as zero when the slot is empty or the core's default is
		   chosen, so a machine that has not been given a size does not
		   acquire one - and so the default can be changed later without
		   every machine being pinned to the old one. */
		long ram_kb = 0;

		if (selection > 0) {
			const int at = copro_ram_choice_->GetSelection();

			if (at >= 0 && (size_t) at < copro_ram_sizes_.size()) {
				const auto core =
				    static_cast<openbus_coproc_core>(selection - 1);
				const unsigned kb = copro_ram_sizes_[(size_t) at];

				if (kb != openbus_coproc_ram_default(core) / 1024u) {
					ram_kb = (long) kb;
				}
			}
		}
		settings.Write("openbus_ram_kb", ram_kb);
	}

	settings.Write("gfxcard_enabled",
	               static_cast<long>(gfxcard_check_->GetValue() ? 1 : 0));
	settings.Write("gfxcard_boot_display",
	               static_cast<long>(gfxcard_boot_check_->GetValue() ? 1 : 0));
	/* Options page. */
	auto write_flag = [&settings](const wxString &key, bool on) {
		settings.Write(key, static_cast<long>(on ? 1 : 0));
	};

	write_flag("start_fullscreen", fullscreen_check_->GetValue());
	write_flag("show_fullscreen_message", fullscreen_msg_check_->GetValue());
	settings.Write("display_scaling",
	               static_cast<long>(SelectedDisplayScaling()));
	{
		unsigned fixed_x = 0, fixed_y = 0;

		SelectedScreenSize(&fixed_x, &fixed_y);
		settings.Write("screen_size_x", static_cast<long>(fixed_x));
		settings.Write("screen_size_y", static_cast<long>(fixed_y));
	}
	/* The keys these replaced are deliberately not written any more. Leaving
	   them behind would have an older RPCEmu and this one disagree about the same
	   machine, each reading the set it knows and neither seeing the other's
	   edits. Reading them (above) is enough to carry a machine forward. */
	write_flag("sound_enabled", sound_check_->GetValue());
	write_flag("cdrom_enabled", cdrom_check_->GetValue());
	write_flag("mouse_twobutton", mouse_twobutton_check_->GetValue());
	write_flag("cpu_idle", cpu_idle_check_->GetValue());
	write_flag("suspend_on_exit", suspend_on_exit_check_->GetValue());
	/* The ways in to this machine, written with it: this dialog edits one machine,
	   so what it says here applies to that machine and not to every other one. */
	write_flag("vnc_enabled", vnc_check_->GetValue());
	settings.Write("vnc_port", static_cast<long>(vnc_port_spin_->GetValue()));
	settings.Write("vnc_password", vnc_password_text_->GetValue());
	settings.Write("vnc_password_readonly",
	    vnc_password_readonly_text_->GetValue());
	write_flag("hostcmd_enabled", hostcmd_check_->GetValue());
	settings.Write("hostcmd_socket", hostcmd_socket_text_->GetValue());
	write_flag("clipboard_enabled", clipboard_check_->GetValue());

	/* The default machine is a host preference keyed by name, so a rename has
	   to carry it across rather than leaving it pointing at a machine that no
	   longer exists. */
	{
		const wxString was_default = wxString::FromUTF8(GetDefaultMachine());

		if (default_machine_check_->GetValue()) {
			SetDefaultMachine(new_name_.utf8_str().data());
		} else if (was_default == original_name_ || was_default == new_name_) {
			ClearDefaultMachine();
		}
	}

	settings.Write("refresh_rate", refresh_slider_->GetValue());
	settings.Write("network_type", network_type);
	settings.Write("json_net_enabled",
	    static_cast<long>(json_net_check_->GetValue() ? 1 : 0));
	settings.Write("json_net_host", json_net_host_edit_->GetValue());
	settings.Write("json_net_port",
	    static_cast<long>(json_net_port_edit_->GetValue()));

	SavePoduleSettings(settings);

	if (!settings.Flush()) {
		wxLogError("Failed to save machine configuration to %s", config_path_);
	}

	renamed_ = allow_rename_ && (new_name_ != original_name_);
	ApplySavedSettingsToGlobalConfig(rom_dir, mem_values[mem_sel], vram_mb,
	                                 refresh_slider_->GetValue(),
	                                 network_type == "nat" ? NetworkType_NAT
	                                                       : NetworkType_Off);
}

void MachineEditDialog::ApplySavedSettingsToGlobalConfig(const wxString &rom_dir, int mem_size,
                                                         int vram_internal, int refresh,
                                                         NetworkType network_type)
{
	const wxScopedCharBuffer name_utf8 = new_name_.utf8_str();
	const wxScopedCharBuffer rom_utf8 = rom_dir.utf8_str();
	config_apply_machine_edit(&config, name_utf8.data(), rom_utf8.data(),
	                          static_cast<unsigned>(mem_size),
	                          static_cast<unsigned>(vram_internal), refresh, network_type);
	/* Whether the graphics card is fitted is read when the machine resets, so
	   applying it here means a reset is enough - no need to restart. */
	config.gfxcard_enabled = gfxcard_check_->GetValue() ? 1 : 0;
	config.accelerators_enabled = accelerators_check_->GetValue() ? 1 : 0;
	config.gfxcard_boot_display = gfxcard_boot_check_->GetValue() ? 1 : 0;
	/* Only read when a machine starts, so this one takes effect next time. */
	config.start_fullscreen = fullscreen_check_->GetValue() ? 1 : 0;

	/*
	 * Everything on the Options page has to be pushed into the live
	 * configuration as well as written to the file.
	 *
	 * A running machine rewrites its whole configuration from memory when it
	 * exits. Anything set here that was only written to disc would be
	 * overwritten by the stale in-memory value at that point, so the change
	 * would appear to work and then quietly undo itself.
	 *
	 * These are the values only. Where a setting also has a live effect - the
	 * scaling mode, muting, the clipboard - that is applied by the Settings
	 * menu when it is toggled there; from here they take effect when the
	 * machine next starts.
	 */
	config.show_fullscreen_message = fullscreen_msg_check_->GetValue() ? 1 : 0;
	config.display_scaling = SelectedDisplayScaling();
	{
		unsigned fixed_x = 0, fixed_y = 0;

		SelectedScreenSize(&fixed_x, &fixed_y);
		config.screen_size_x = fixed_x;
		config.screen_size_y = fixed_y;

		/* Asking the guest is left to the frame, which applies these as soon as
		   this dialogue closes and can check that the mode was accepted. */
	}
	config.soundenabled = sound_check_->GetValue() ? 1 : 0;
	config.cdromenabled = cdrom_check_->GetValue() ? 1 : 0;
	config.mousetwobutton = mouse_twobutton_check_->GetValue() ? 1 : 0;
	config.cpu_idle = cpu_idle_check_->GetValue() ? 1 : 0;
	config.suspend_on_exit = suspend_on_exit_check_->GetValue() ? 1 : 0;
	config.vnc_enabled = vnc_check_->GetValue() ? 1 : 0;
	config.vnc_port = vnc_port_spin_->GetValue();
	strncpy(config.vnc_password, vnc_password_text_->GetValue().utf8_str().data(),
	    sizeof(config.vnc_password) - 1);
	config.vnc_password[sizeof(config.vnc_password) - 1] = '\0';
	strncpy(config.vnc_password_readonly,
	    vnc_password_readonly_text_->GetValue().utf8_str().data(),
	    sizeof(config.vnc_password_readonly) - 1);
	config.vnc_password_readonly[
	    sizeof(config.vnc_password_readonly) - 1] = '\0';
	config.clipboard_enabled = clipboard_check_->GetValue() ? 1 : 0;
}

wxString MachineEditDialog::CurrentMachineNameForHd() const
{
	if (!allow_rename_) {
		return original_name_;
	}

	wxString name = ConfigPathsSanitizeName(name_edit_->GetValue());
	name.Trim(true).Trim(false);
	if (name.empty()) {
		return original_name_;
	}
	return name;
}

wxString MachineEditDialog::HardDiscFilePath(int drive) const
{
	if (drive == 4 && !hd4_path_.empty() && wxFileName(hd4_path_).IsAbsolute()) {
		return hd4_path_;
	}

	const wxString machine_dir = ConfigPathsMachinesDir() + wxFileName::GetPathSeparator() +
	                             CurrentMachineNameForHd();
	return machine_dir + wxFileName::GetPathSeparator() + wxString::Format("hd%d.hdf", drive);
}

MachineEditDialog::HardDiscInfo MachineEditDialog::QueryHardDiscInfo(int drive) const
{
	HardDiscInfo info;
	info.path = HardDiscFilePath(drive);
	info.uses_custom_path = drive == 4 && !hd4_path_.empty() && wxFileName(hd4_path_).IsAbsolute();

	if (!wxFileExists(info.path)) {
		info.state = HardDiscState::Missing;
		return info;
	}

	const wxULongLong size_bytes = wxFileName::GetSize(info.path);
	info.size_text = FormatHardDiscSize(size_bytes);
	info.modified_text = FormatModifiedTime(info.path);

	if (size_bytes == 0) {
		info.state = HardDiscState::Empty;
		return info;
	}

	if (drive == 5 && cdrom_enabled_) {
		info.state = HardDiscState::Blocked;
		return info;
	}

	info.state = info.uses_custom_path ? HardDiscState::CustomPath : HardDiscState::Ready;
	return info;
}

void MachineEditDialog::ApplyHardDiscPanel(HardDiscPanel &panel, const HardDiscInfo &info)
{
	wxString badge_text;
	wxColour badge_colour = kHdColourMissing;

	switch (info.state) {
	case HardDiscState::Missing:
		badge_text = "Not created";
		badge_colour = kHdColourMissing;
		break;
	case HardDiscState::Empty:
		badge_text = "Empty - will not attach";
		badge_colour = kHdColourEmpty;
		break;
	case HardDiscState::Ready:
		badge_text = wxString::Format("Ready - %s", info.size_text);
		badge_colour = kHdColourReady;
		break;
	case HardDiscState::CustomPath:
		badge_text = wxString::Format("Ready - %s (custom path)", info.size_text);
		badge_colour = kHdColourReady;
		break;
	case HardDiscState::Blocked:
		badge_text = wxString::Format("Unavailable - CD-ROM enabled (%s on disk)", info.size_text);
		badge_colour = kHdColourBlocked;
		break;
	}

	panel.badge->SetLabel(badge_text);
	panel.badge->SetForegroundColour(badge_colour);

	wxString path_text = TruncatePathMiddle(info.path);
	if (!allow_rename_ || CurrentMachineNameForHd() == original_name_) {
		panel.path_label->SetLabel(path_text);
	} else {
		path_text += "  (preview for unsaved name)";
		panel.path_label->SetLabel(path_text);
	}
	if (!info.path.empty()) {
		panel.path_label->SetToolTip(info.path);
	} else {
		panel.path_label->UnsetToolTip();
	}

	if (info.modified_text.empty()) {
		panel.modified_label->SetLabel(wxEmptyString);
	} else {
		panel.modified_label->SetLabel(info.modified_text);
	}

	const bool file_exists = info.state != HardDiscState::Missing;
	panel.delete_btn->Enable(file_exists);
	panel.open_folder_btn->Enable(file_exists);
	panel.create_btn->Enable(true);
}

void MachineEditDialog::UpdateHdStatus()
{
	ApplyHardDiscPanel(hd4_panel_, QueryHardDiscInfo(4));
	ApplyHardDiscPanel(hd5_panel_, QueryHardDiscInfo(5));
}

void MachineEditDialog::ShowHardDiscCreateMenu(int drive)
{
	wxMenu menu;
	menu.Append(ID_HD_CREATE_256_MB, "256 MB");
	menu.Append(ID_HD_CREATE_512_MB, "512 MB");
	menu.Append(ID_HD_CREATE_1_GB, "1 GB");
	menu.Append(ID_HD_CREATE_2_GB, "2 GB");

	menu.Bind(wxEVT_MENU, [this, drive](wxCommandEvent &event) {
		int size_mb = 512;
		switch (event.GetId()) {
		case ID_HD_CREATE_256_MB:
			size_mb = 256;
			break;
		case ID_HD_CREATE_1_GB:
			size_mb = 1024;
			break;
		case ID_HD_CREATE_2_GB:
			size_mb = 2048;
			break;
		default:
			break;
		}
		CreateHardDisc(drive, size_mb);
	});

	wxButton *const create_btn = drive == 4 ? hd4_panel_.create_btn : hd5_panel_.create_btn;
	if (create_btn != nullptr) {
		create_btn->PopupMenu(&menu);
	}
}

void MachineEditDialog::CreateHardDisc(int drive, int size_mb)
{
	const wxString hd_path = HardDiscFilePath(drive);
	const wxString machine_dir = wxFileName(hd_path).GetPath();

	if (wxFileExists(hd_path)) {
		const HardDiscInfo info = QueryHardDiscInfo(drive);
		if (info.state != HardDiscState::Empty) {
			wxMessageBox(wxString::Format("HardDisc %d already exists.", drive), "Create HardDisc",
			             wxOK | wxICON_INFORMATION, this);
			return;
		}
	}

	wxDir::Make(machine_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	wxFile file(hd_path, wxFile::write);
	if (!file.IsOpened()) {
		wxMessageBox(wxString::Format("Could not create %s", hd_path), "Create HardDisc",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	const wxULongLong bytes = static_cast<wxULongLong>(size_mb) * 1024ULL * 1024ULL;
	file.Seek(static_cast<wxFileOffset>((bytes - 1).GetValue()));
	file.Write("\0", 1);
	file.Close();

	UpdateHdStatus();

	wxString message = wxString::Format("Hard disc %d created (%d MB).", drive, size_mb);
	if (emulator_running_) {
		message += wxString::FromUTF8("\n\nUse Machine \xE2\x86\x92 Reset (not RISC OS Restart) so RPCEmu reloads the IDE image.");
	}
	wxMessageBox(message, "Create HardDisc", wxOK | wxICON_INFORMATION, this);
}

void MachineEditDialog::DeleteHardDisc(int drive)
{
	const wxString hd_path = HardDiscFilePath(drive);
	if (!wxFileExists(hd_path)) {
		return;
	}

	wxString message = wxString::Format(
	    "Delete HardDisc %d?\n\nAll data on this disc will be lost.", drive);
	if (emulator_running_) {
		message += "\n\nThe emulator is running - this change will take effect after reset.";
	}

	if (wxMessageBox(message, "Delete HardDisc", wxYES_NO | wxICON_WARNING, this) != wxYES) {
		return;
	}

	if (!wxRemoveFile(hd_path)) {
		wxMessageBox(wxString::Format("Could not delete %s", hd_path), "Delete HardDisc",
		             wxOK | wxICON_ERROR, this);
		return;
	}

	UpdateHdStatus();
}

void MachineEditDialog::OpenHardDiscFolder(int drive)
{
	const wxString hd_path = HardDiscFilePath(drive);
	const wxString dir = wxFileName(hd_path).GetPath();
	if (!wxDirExists(dir)) {
		wxMessageBox(wxString::Format("Folder does not exist:\n%s", dir), "Open folder",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	if (!wxLaunchDefaultApplication(dir)) {
		wxMessageBox(wxString::Format("Could not open folder:\n%s", dir), "Open folder",
		             wxOK | wxICON_ERROR, this);
	}
}

void MachineEditDialog::OnNetworkChanged(wxCommandEvent &)
{
	/* Enabling/disabling networking changes whether slot 1 is the (locked)
	   network card or a free user slot. */
	RebuildPoduleChoices();
}

void MachineEditDialog::OnRomOrModelChanged(wxCommandEvent &)
{
	if (loading_settings_) {
		return;
	}
	UpdateRomModelCompatibility();
}

void MachineEditDialog::OnNameChanged(wxCommandEvent &)
{
	if (loading_settings_) {
		return;
	}
	UpdateHdStatus();
	/* A different name is a different machine's data directory, which is
	   keyed on the configuration's "name" field (see
	   rpcemu_set_machine_datadir), so it is a different hard disc too. */
	UpdateDiscDownloadAvailability();
}

void MachineEditDialog::OnOk(wxCommandEvent &)
{
	char msg[512];
	const Model model = CurrentModelSelection();
	const wxString rom_dir = SelectedRomDir();
	const wxScopedCharBuffer rom_dir_utf8 = rom_dir.utf8_str();
	const std::string vnc_password =
	    vnc_password_text_->GetValue().utf8_str().data();
	const std::string vnc_password_readonly =
	    vnc_password_readonly_text_->GetValue().utf8_str().data();
	if (vnc_check_->GetValue() && vnc_password.empty() &&
	    !vnc_password_readonly.empty()) {
		wxMessageBox("A read-only VNC password requires a control password.",
		    "VNC", wxOK | wxICON_ERROR, this);
		return;
	}
	if (vnc_check_->GetValue() && !vnc_password_readonly.empty() &&
	    vnc_password.substr(0, 8) == vnc_password_readonly.substr(0, 8)) {
		wxMessageBox("The control and read-only VNC passwords must differ in "
		    "their first eight bytes.", "VNC", wxOK | wxICON_ERROR, this);
		return;
	}
	if (!rom_model_is_compatible(model, rom_dir_utf8.data(), msg, sizeof(msg))) {
		wxMessageBox(wxString::FromUTF8(msg), "ROM compatibility", wxOK | wxICON_WARNING, this);
		return;
	}
	/* Before saving, because the setting must not change unless the files
	   actually got there. See folder_transfer.h. */
	if (!OfferToBringHostfsFilesAcross()) {
		return;
	}
	SaveSettings();
	EndModal(wxID_OK);
}

/*
 * The HostFS folder has been pointed somewhere new: offer to take the files with
 * it.
 *
 * ★ THE ORDER IS THE SAFETY. The transfer happens first and the configuration is
 * only written if it succeeded, so a failure leaves the machine booting from the
 * folder it was booting from before. Getting this the other way round would mean a
 * failed copy and a machine pointed at an empty folder, which is the very thing
 * this feature exists to stop.
 *
 * @return false when the user cancelled, or when a transfer failed - in both
 *         cases nothing should be saved and the dialogue stays open.
 */
bool MachineEditDialog::OfferToBringHostfsFilesAcross()
{
	if (hostfs_edit_ == nullptr) {
		return true;
	}

	const wxString now = hostfs_edit_->GetValue().Trim().Trim(false);

	if (now == original_hostfs_) {
		return true;
	}

	const wxString from = ResolveHostfsValue(original_hostfs_);
	const wxString to = ResolveHostfsValue(now);
	unsigned long long bytes = 0;
	const folder_move_facts facts =
	    FolderTransferGatherFacts(from, to, emulator_running_, &bytes);
	folder_move_reason why = FOLDER_MOVE_OK;
	const folder_move_offer offer = folder_move_decide(&facts, &why);

	if (offer == FOLDER_MOVE_OFFER_NOTHING) {
		/*
		 * Nothing to move is the ordinary case - a new machine, or the same
		 * folder spelled differently - and saying so would be noise. The others
		 * are worth a word, because the user asked for something that cannot
		 * happen and would otherwise find an empty disc later.
		 */
		if (why == FOLDER_MOVE_SAME_PLACE || why == FOLDER_MOVE_SOURCE_EMPTY ||
		    why == FOLDER_MOVE_SOURCE_MISSING) {
			return true;
		}
		const FolderTransferChoice choice = FolderTransferAsk(this, "hard disc",
		    from, to, offer, why, bytes);

		return choice != FolderTransferChoice::Cancel;
	}

	const FolderTransferChoice choice = FolderTransferAsk(this, "hard disc",
	    from, to, offer, why, bytes);

	switch (choice) {
	case FolderTransferChoice::Cancel:
		return false;

	case FolderTransferChoice::Manual:
		/* The note under the field already says what an empty folder means, and
		   they have just read it in the dialogue. Nothing more to add. */
		return true;

	case FolderTransferChoice::Move:
	case FolderTransferChoice::Copy: {
		const FolderTransferResult result =
		    FolderTransferRun(this, from, to, choice);

		if (!result.ok) {
			wxMessageBox(result.message, "The files were not moved",
			    wxOK | wxICON_ERROR, this);
			/* Put the field back, so pressing OK again saves everything else
			   without pointing the machine at a folder its files are not in. */
			hostfs_edit_->SetValue(original_hostfs_);
			UpdateHostfsNote();
			return false;
		}
		wxMessageBox(result.message,
		    choice == FolderTransferChoice::Move ? "Files moved" : "Files copied",
		    wxOK | wxICON_INFORMATION, this);
		return true;
	}
	}
	return true;
}
