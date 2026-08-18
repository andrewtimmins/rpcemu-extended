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

#include "gui_resources.h"
#include "about_dialog.h"

#include <algorithm>

#include <wx/arrstr.h>
#include <wx/artprov.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/statline.h>

extern "C" {
#include "rpcemu.h"
}

namespace {

wxBitmap
CreateAboutIcon(int size)
{
	wxImage image(size, size);
	image.InitAlpha();

	const wxColour outer(0x2C, 0x5F, 0x8A);
	const wxColour inner(0xF0, 0xE8, 0xD0);
	const int margin = std::max(1, size / 8);
	const int radius = std::max(1, size / 10);

	auto inside_rounded_rect = [&](int x, int y) {
		const int left = margin;
		const int top = margin;
		const int right = size - margin - 1;
		const int bottom = size - margin - 1;

		if (x < left || x > right || y < top || y > bottom) {
			return false;
		}

		auto inside_corner = [&](int cx, int cy) {
			const int dx = x - cx;
			const int dy = y - cy;
			return (dx * dx + dy * dy) <= (radius * radius);
		};

		if (x < left + radius && y < top + radius) {
			return inside_corner(left + radius, top + radius);
		}
		if (x > right - radius && y < top + radius) {
			return inside_corner(right - radius, top + radius);
		}
		if (x < left + radius && y > bottom - radius) {
			return inside_corner(left + radius, bottom - radius);
		}
		if (x > right - radius && y > bottom - radius) {
			return inside_corner(right - radius, bottom - radius);
		}
		return true;
	};

	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const wxColour &colour = inside_rounded_rect(x, y) ? inner : outer;
			image.SetRGB(x, y, colour.Red(), colour.Green(), colour.Blue());
			image.SetAlpha(x, y, 255);
		}
	}

	wxBitmap bitmap(image);
	if (bitmap.IsOk()) {
		return bitmap;
	}

	return wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_OTHER, wxSize(size, size));
}

// The application logo: prefer the shipped rpcemu.png, fall back to the
// procedural placeholder if it can't be found/loaded.
wxBitmap
AboutLogo(int size)
{
	const wxString path = AppLogoPath();
	wxImage image;

	if (wxFileExists(path) && image.LoadFile(path, wxBITMAP_TYPE_PNG)) {
		if (image.GetWidth() != size || image.GetHeight() != size) {
			image.Rescale(size, size, wxIMAGE_QUALITY_HIGH);
		}
		return wxBitmap(image);
	}

	return CreateAboutIcon(size);
}

wxString
BuildDescription()
{
#if defined(RPCEMU_BUILD_DYNAREC)
	const wxString core = "Dynamic recompiler";
#else
	const wxString core = "Interpreter";
#endif
	// wxString::Format format strings must be ASCII-only; UTF-8 breaks wxFormatString.
	return core + wxString::Format(" build, %ld-bit", static_cast<long>(sizeof(void *) * 8));
}

/*
 * Who made what.
 *
 * Named individually rather than as "contributors", because a blanket phrase
 * credits nobody. Anyone whose work is in the build belongs here: the people
 * who wrote RPCEmu, the people who have contributed to this fork, and the
 * projects whose code it carries. The last group is not a courtesy but a
 * licence condition, and the detail behind each line is in the README.
 *
 * Keep this in step with the credits section of the README when either changes.
 */
struct CreditGroup {
	const char *heading;
	const char *entries;
};

const CreditGroup kCredits[] = {
	{ "RPCEmu",
	  "Sarah Walker\n"
	  "Peter Howkins\n"
	  "Matthew Howkins" },
	{ "RPCEmu Extended",
	  "Andy Timmins\n"
	  "David Ramsden" },
	{ "Contributed to RPCEmu Extended",
	  "Nick Brown: saving and restoring machine state, and the idea of "
	  "putting the clock right afterwards\n"
	  "Charles Ferguson: the host interfaces manual, building the guest "
	  "network driver on the RISC OS build service, and read-only VNC "
	  "access" },
	{ "Code this build carries, with thanks",
	  "Arculator, Sarah Walker: the expansion card subsystem\n"
	  "EtherRPCEm, J Ballance and Alex Waugh: the guest network driver, "
	  "from Castle Technology's EtherY\n"
	  "Acorn Computers Ltd: the DCI4 structure layouts that driver "
	  "hardcodes\n"
	  "RISC OS Open Ltd: USBDriver and OHCIDriver, the USB stack the "
	  "emulated card runs\n"
	  "SLiRP, Danny Gasparovski and the Regents of the University of "
	  "California, with four files by Fabrice Bellard\n"
	  "libslirp, Samuel Thibault and Marc-Andre Lureau: fragment "
	  "reassembly fixes\n"
	  "Linux, Russell King: the RISC OS timestamp conversion HostFS uses\n"
	  "RiscOS Cloverleaf: the shared clipboard, its design and guest module\n"
	  "SyncClock, DEEJ Technology PLC: the clock synchronisation module\n"
	  "NetSurf, John M Bell: the Latin-1 conversion table\n"
	  "Lucide and Feather: the toolbar icons" },
	{ "With acknowledgement to",
	  "ViewFinder, John Kortink: the idea behind the graphics card\n"
	  "Fat32Fs, Jeff Doggett, itself built on efsl by Lennart Yseboodt and "
	  "Michael De Nil: the convention for recording a RISC OS file type on "
	  "FAT media, which MultiFS keeps so that media move between machines\n"
	  "Charles Ferguson: the JSON tun/tap protocol and server this shares a "
	  "virtual network through, and RISC OS Pyromaniac at the other end of "
	  "it\n"
	  "The RISC OS Packaging Project: the package and database format the "
	  "package manager implements, designed by Graham Shaw, its policy "
	  "manual maintained by Alan Buckley\n"
	  "RISC OS Open and riscoscommunity.org: hosting the package indexes "
	  "and the packages themselves\n"
	  "The Archimedes Software Preservation Project (JASPP), Jonathan "
	  "Abbott and its contributors: preserving the games of the period "
	  "and packaging them to run on a machine like this one" },
};

} // namespace

AboutDialog::AboutDialog(wxWindow *parent)
	: wxDialog(parent, wxID_ANY, "About RPCEmu Extended", wxDefaultPosition, wxDefaultSize,
	           wxDEFAULT_DIALOG_STYLE | wxCLOSE_BOX)
{
	BuildUi();
	Fit();
	SetMinSize(GetSize());
	CentreOnParent();
}

void AboutDialog::BuildUi()
{
	const int year = wxDateTime::Now().GetYear();
	const wxColour muted = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

	wxFont title_font = GetFont();
	title_font.SetPointSize(title_font.GetPointSize() + 6);
	title_font.SetWeight(wxFONTWEIGHT_BOLD);

	wxFont edition_font = GetFont();
	edition_font.SetPointSize(edition_font.GetPointSize() + 1);

	wxFont small_font = GetFont();
	small_font.SetPointSize(std::max(8, small_font.GetPointSize() - 1));

	auto *icon = new wxStaticBitmap(this, wxID_ANY, AboutLogo(64));

	auto *title = new wxStaticText(this, wxID_ANY, "RPCEmu");
	title->SetFont(title_font);

	auto *edition = new wxStaticText(this, wxID_ANY, "Extended");
	edition->SetFont(edition_font);
	edition->SetForegroundColour(muted);

	auto *version = new wxStaticText(this, wxID_ANY, "Version " + wxString(VERSION));

	auto *title_block = new wxBoxSizer(wxVERTICAL);
	title_block->Add(title, 0, wxBOTTOM, 2);
	title_block->Add(edition, 0, wxBOTTOM, 4);
	title_block->Add(version, 0);

	auto *header = new wxBoxSizer(wxHORIZONTAL);
	header->Add(icon, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 16);
	header->Add(title_block, 1, wxALIGN_CENTRE_VERTICAL);

	auto *tagline = new wxStaticText(this, wxID_ANY, "Acorn Risc PC and A7000 emulator");

	/* Outside the scrolling panel, so it stays put as the list moves. */
	auto *credits_intro = new wxStaticText(this, wxID_ANY, "Brought to you by:");

	auto *license = new wxStaticText(this, wxID_ANY,
	                                 wxString::Format("Copyright 2005-%d the respective "
	                                                  "authors. ", year) +
	                                 "This program is free software, licensed under the "
	                                 "GNU General Public License, version 2. "
	                                 "See the COPYING file for details.");
	license->Wrap(420);
	license->SetForegroundColour(muted);

	auto *build_info = new wxStaticText(this, wxID_ANY, BuildDescription());
	build_info->SetFont(small_font);
	build_info->SetForegroundColour(muted);

	/*
	 * The credits scroll. They do not fit in a dialogue anybody would want to
	 * look at, and shortening them would defeat the point of having them.
	 *
	 * Sized from the font rather than written down in pixels. It was
	 * wxSize(440, 270) with the entries wrapped at 380: three numbers that had
	 * to agree with each other and with whatever font the system happens to be
	 * using, so a larger font or a HiDPI screen got a pane that did not fit its
	 * own contents.
	 *
	 * The width is measured from a line of the text itself, and the height from
	 * how tall a row actually is - the character height plus the padding each
	 * row is added with, which is what makes it a row pitch rather than a line
	 * height. A part-shown row at the bottom is left deliberately: it is what
	 * tells the reader there is more below, and the list is far longer than any
	 * dialogue should be.
	 */
	const int line_height = GetCharHeight();
	const int row_padding = FromDIP(10);
	const int row_pitch = line_height + row_padding;
	const int indent = FromDIP(16);
	const int scrollbar = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
	const int credits_width =
	    GetTextExtent("Fat32Fs, Jeff Doggett, itself built on efsl by Lennart")
	        .GetWidth() + indent + scrollbar + FromDIP(24);
	const int credits_height = row_pitch * 11 + row_padding;
	const int wrap_width = credits_width - indent - scrollbar - FromDIP(20);

	auto *credits = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
	                                     wxSize(credits_width, credits_height),
	                                     wxVSCROLL | wxBORDER_THEME);
	credits->SetBackgroundColour(
	    wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

	wxFont heading_font = GetFont();
	heading_font.SetWeight(wxFONTWEIGHT_BOLD);

	auto *credits_sizer = new wxBoxSizer(wxVERTICAL);
	bool first = true;

	for (const CreditGroup &group : kCredits) {
		auto *heading = new wxStaticText(credits, wxID_ANY, group.heading);

		heading->SetFont(heading_font);
		credits_sizer->AddSpacer(first ? 10 : 14);
		credits_sizer->Add(heading, 0, wxLEFT | wxRIGHT, 10);

		for (const wxString &entry :
		     wxSplit(wxString::FromUTF8(group.entries), '\n')) {
			auto *text = new wxStaticText(credits, wxID_ANY, entry);

			auto *row = new wxBoxSizer(wxHORIZONTAL);

			text->Wrap(wrap_width);
			row->AddSpacer(indent);
			row->Add(text, 1);
			credits_sizer->Add(row, 0, wxRIGHT | wxTOP, row_padding);
		}
		first = false;
	}
	credits_sizer->AddSpacer(10);

	credits->SetSizer(credits_sizer);
	/* A line at a time, so scrolling lands on whole lines too. */
	credits->SetScrollRate(0, line_height);
	/* Work the virtual size out from the contents, or the scrollbar never
	   appears and the entries past the bottom simply cannot be reached. */
	credits->FitInside();

	auto *buttons = CreateStdDialogButtonSizer(wxOK);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(header, 0, wxEXPAND | wxALL, 16);
	main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
	main->Add(tagline, 0, wxLEFT | wxRIGHT | wxTOP, 16);
	main->Add(credits_intro, 0, wxLEFT | wxRIGHT | wxTOP, 16);
	main->Add(credits, 1, wxEXPAND | wxALL, 16);
	main->Add(license, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	main->Add(build_info, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	main->Add(buttons, 0, wxEXPAND | wxALL, 10);
	SetSizer(main);
}
