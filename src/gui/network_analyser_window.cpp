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

#include <ctime>

#include <wx/filedlg.h>
#include <wx/settings.h>
#include <wx/splitter.h>
#include <wx/tglbtn.h>

#include "network_analyser_window.h"

extern "C" {
#include "net_dissect.h"
#include "rpcemu.h"
}

enum {
	ID_NA_TIMER = wxID_HIGHEST + 1500,
	ID_NA_CAPTURE,
	ID_NA_CLEAR,
	ID_NA_SAVE,
	ID_NA_AUTOSCROLL
};

/* How many frames the window keeps. The capture ring holds its own; this is
   what has been copied out of it and is what the list indexes into. */
static const size_t kMaxRows = NETCAP_RING_FRAMES;

/**
 * The list itself.
 *
 * Virtual, so the rows are fetched as they are drawn. A machine on a busy
 * network produces frames faster than a list control can be given items, and
 * the whole thing is redrawn on a timer.
 */
class NetworkAnalyserWindow::FrameList : public wxListCtrl {
public:
	FrameList(wxWindow *parent, const std::vector<NetcapFrame> &frames)
	    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
	                 wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL),
	      frames_(frames)
	{
		AppendColumn("No.", wxLIST_FORMAT_RIGHT, 70);
		AppendColumn("Time", wxLIST_FORMAT_LEFT, 110);
		AppendColumn("", wxLIST_FORMAT_CENTER, 34);
		AppendColumn("Protocol", wxLIST_FORMAT_LEFT, 90);
		AppendColumn("Source", wxLIST_FORMAT_LEFT, 210);
		AppendColumn("Destination", wxLIST_FORMAT_LEFT, 210);
		AppendColumn("Length", wxLIST_FORMAT_RIGHT, 70);
		AppendColumn("Info", wxLIST_FORMAT_LEFT, 360);
		SetUpTints();

		/* A theme can change while the window is open, and the tints are
		   worked out from it rather than fixed, so they are worked out again
		   rather than left showing the old theme's idea of a light row. */
		Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent &e) {
			SetUpTints();
			Refresh();
			e.Skip();
		});
	}

protected:
	wxString OnGetItemText(long item, long column) const override
	{
		if (item < 0 || static_cast<size_t>(item) >= frames_.size()) {
			return wxEmptyString;
		}
		const NetcapFrame &f = frames_[static_cast<size_t>(item)];

		switch (column) {
		case 0:
			return wxString::Format("%llu",
			    static_cast<unsigned long long>(f.serial));
		case 1: {
			/* Local time: the first thing anybody does with a capture is line
			   it up against something else that happened. */
			const time_t when = static_cast<time_t>(f.sec);
			struct tm tmv;

#ifdef _WIN32
			localtime_s(&tmv, &when);
#else
			localtime_r(&when, &tmv);
#endif
			return wxString::Format("%02d:%02d:%02d.%06u", tmv.tm_hour,
			    tmv.tm_min, tmv.tm_sec, f.usec);
		}
		case 2:
			return (f.direction == NETCAP_TX) ? wxString::FromUTF8("\xe2\x86\x91")
			                                  : wxString::FromUTF8("\xe2\x86\x93");
		default:
			break;
		}

		NetDissectSummary sum;

		netdis_summary(f.data, f.captured, &sum);
		switch (column) {
		case 3: return wxString::FromUTF8(sum.protocol);
		case 4: return wxString::FromUTF8(sum.source);
		case 5: return wxString::FromUTF8(sum.dest);
		case 6: return wxString::Format("%u", f.length);
		case 7: return wxString::FromUTF8(sum.info);
		default: return wxEmptyString;
		}
	}

	wxListItemAttr *OnGetItemAttr(long item) const override
	{
		/* Sent and received tinted differently. On a list where nearly every
		   line looks the same, the direction is the thing the eye needs. */
		if (item < 0 || static_cast<size_t>(item) >= frames_.size()) {
			return nullptr;
		}
		return (frames_[static_cast<size_t>(item)].direction == NETCAP_TX)
		    ? &tx_attr_ : &rx_attr_;
	}

	/**
	 * ★ The tints are DERIVED from the theme, never written down.
	 *
	 * Setting a background without setting a foreground hands the control one
	 * of the two colours and lets the theme choose the other. A pale tint
	 * written in here therefore reads perfectly on a light theme and puts the
	 * theme's near-white text on near-white on a dark one, which is what
	 * happened. So the tint starts from the colour the list is actually using
	 * and moves a little way from it, in whichever direction that colour is
	 * not.
	 */
	void SetUpTints()
	{
		const wxColour base = GetBackgroundColour().IsOk()
		    ? GetBackgroundColour()
		    : wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
		const bool dark =
		    (base.Red() + base.Green() + base.Blue()) < 3 * 128;
		const int sign = dark ? 1 : -1;
		auto shift = [&](int r, int g, int b) {
			auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };

			return wxColour(clamp(base.Red() + sign * r),
			    clamp(base.Green() + sign * g), clamp(base.Blue() + sign * b));
		};

		/* Sent leans blue, received leans green, both barely: this separates
		   two kinds of row, it is not meant to be noticed on its own. */
		tx_attr_ = wxListItemAttr(wxNullColour, shift(2, 6, 18), wxNullFont);
		rx_attr_ = wxListItemAttr(wxNullColour, shift(2, 18, 6), wxNullFont);
	}

private:
	const std::vector<NetcapFrame> &frames_;
	mutable wxListItemAttr tx_attr_;
	mutable wxListItemAttr rx_attr_;
};

NetworkAnalyserWindow::NetworkAnalyserWindow(wxWindow *parent)
    : wxFrame(parent, wxID_ANY, "Network Analyser", wxDefaultPosition,
              wxSize(1100, 680)),
      timer_(this, ID_NA_TIMER)
{
	auto *panel = new wxPanel(this);
	auto *top = new wxBoxSizer(wxVERTICAL);

	auto *bar = new wxBoxSizer(wxHORIZONTAL);
	capture_ = new wxToggleButton(panel, ID_NA_CAPTURE, "Capture");
	autoscroll_ = new wxCheckBox(panel, ID_NA_AUTOSCROLL, "Follow");
	autoscroll_->SetValue(true);
	bar->Add(capture_, 0, wxRIGHT, 8);
	bar->Add(new wxButton(panel, ID_NA_CLEAR, "Clear"), 0, wxRIGHT, 8);
	bar->Add(new wxButton(panel, ID_NA_SAVE, "Save as pcap..."), 0, wxRIGHT, 16);
	bar->Add(autoscroll_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
	status_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
	bar->Add(status_, 1, wxALIGN_CENTER_VERTICAL);
	top->Add(bar, 0, wxEXPAND | wxALL, 8);

	auto *splitter = new wxSplitterWindow(panel, wxID_ANY, wxDefaultPosition,
	    wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
	list_ = new FrameList(splitter, frames_);

	auto *lower = new wxPanel(splitter);
	auto *lower_sizer = new wxBoxSizer(wxHORIZONTAL);
	detail_ = new wxTextCtrl(lower, wxID_ANY, wxEmptyString, wxDefaultPosition,
	    wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	bytes_ = new wxTextCtrl(lower, wxID_ANY, wxEmptyString, wxDefaultPosition,
	    wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	{
		const wxFont mono(wxFontInfo(detail_->GetFont().GetPointSize())
		    .Family(wxFONTFAMILY_TELETYPE));

		detail_->SetFont(mono);
		bytes_->SetFont(mono);

		/*
		 * ★ The hex pane's width is not a matter of taste, it is arithmetic.
		 *
		 * A hex dump line is a fixed shape - offset, sixteen pairs with a gap
		 * in the middle, then the sixteen characters - and a pane narrower
		 * than that wraps or scrolls it, at which point the alignment that is
		 * the entire point of a hex dump is gone. So the width is measured
		 * from a full line in the font actually in use, and the pane is given
		 * that as its minimum and no proportion at all: it takes what it needs
		 * and the field tree beside it flexes into the rest.
		 */
		const wxString widest =
		    "0000  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  "
		    "................";
		const wxSize needed = bytes_->GetTextExtent(widest);

		bytes_->SetMinSize(wxSize(needed.GetWidth() +
		    wxSystemSettings::GetMetric(wxSYS_VSCROLL_X) + 16, -1));
	}
	lower_sizer->Add(detail_, 1, wxEXPAND | wxRIGHT, 4);
	lower_sizer->Add(bytes_, 0, wxEXPAND);
	lower->SetSizer(lower_sizer);

	splitter->SplitHorizontally(list_, lower, 380);
	splitter->SetMinimumPaneSize(120);
	top->Add(splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

	panel->SetSizer(top);

	Bind(wxEVT_TIMER, &NetworkAnalyserWindow::OnTimer, this, ID_NA_TIMER);
	Bind(wxEVT_TOGGLEBUTTON, &NetworkAnalyserWindow::OnCapture, this,
	    ID_NA_CAPTURE);
	Bind(wxEVT_BUTTON, &NetworkAnalyserWindow::OnClear, this, ID_NA_CLEAR);
	Bind(wxEVT_BUTTON, &NetworkAnalyserWindow::OnSave, this, ID_NA_SAVE);
	list_->Bind(wxEVT_LIST_ITEM_SELECTED, &NetworkAnalyserWindow::OnSelected,
	    this);

	/*
	 * Opening the window is asking to see frames, so it turns the ring on.
	 * Switched back off when it closes: with nothing looking, keeping them is
	 * a memcpy per frame for no reason.
	 */
	capture_->SetValue(true);
	netcap_ring_enable(1);
	timer_.Start(400);
}

NetworkAnalyserWindow::~NetworkAnalyserWindow()
{
	timer_.Stop();
	netcap_ring_enable(0);
}

void
NetworkAnalyserWindow::ShowAndRaise()
{
	Show();
	Raise();
}

void
NetworkAnalyserWindow::OnCapture(wxCommandEvent &)
{
	netcap_ring_enable(capture_->GetValue() ? 1 : 0);
}

void
NetworkAnalyserWindow::OnClear(wxCommandEvent &)
{
	netcap_clear();
	frames_.clear();
	last_serial_ = 0;
	selected_ = -1;
	list_->SetItemCount(0);
	detail_->Clear();
	bytes_->Clear();
}

void
NetworkAnalyserWindow::OnSave(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Save the frames shown", wxEmptyString,
	    "capture.pcap", "Packet captures (*.pcap)|*.pcap|All files (*)|*",
	    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	FILE *f = fopen(dlg.GetPath().utf8_str().data(), "wb");

	if (f == nullptr) {
		wxMessageBox("Could not write that file.", "Network Analyser",
		    wxOK | wxICON_ERROR, this);
		return;
	}
	{
		uint8_t header[NETCAP_PCAP_HEADER_LEN];

		netcap_pcap_file_header(header);
		fwrite(header, 1, sizeof(header), f);
	}
	for (const NetcapFrame &fr : frames_) {
		uint8_t record[NETCAP_PCAP_RECORD_LEN];

		netcap_pcap_record_header(record, fr.sec, fr.usec, fr.captured,
		    fr.length);
		fwrite(record, 1, sizeof(record), f);
		fwrite(fr.data, 1, fr.captured, f);
	}
	fclose(f);
}

void
NetworkAnalyserWindow::OnTimer(wxTimerEvent &)
{
	NetcapFrame *batch = new NetcapFrame[256];
	unsigned count = 0;

	netcap_copy_since(last_serial_, batch, 256, &count);
	if (count > 0) {
		for (unsigned i = 0; i < count; i++) {
			frames_.push_back(batch[i]);
			last_serial_ = batch[i].serial;
		}
		/* Oldest first out of the front, so the window holds the most recent
		   frames rather than the first ones it ever saw. */
		if (frames_.size() > kMaxRows) {
			frames_.erase(frames_.begin(),
			    frames_.begin() + static_cast<long>(frames_.size() - kMaxRows));
		}
		list_->SetItemCount(static_cast<long>(frames_.size()));
		if (autoscroll_->IsChecked() && !frames_.empty()) {
			list_->EnsureVisible(static_cast<long>(frames_.size()) - 1);
		}
		list_->Refresh();
	}
	delete[] batch;

	{
		NetcapStats st;

		netcap_get_stats(&st);

		/* Assembled from named strings rather than in one Format: a ternary
		   picking between a wxString's buffer and a literal has to convert one
		   of them, and the buffer belongs to a temporary. */
		wxString label = wxString::Format("%llu frames  (%llu sent, %llu received)",
		    static_cast<unsigned long long>(st.frames),
		    static_cast<unsigned long long>(st.frames_tx),
		    static_cast<unsigned long long>(st.frames_rx));

		if (st.file_active) {
			label += "   writing to a file";
		}
		if (st.dropped_ring > 0) {
			label += wxString::Format("   %llu older frames dropped",
			    static_cast<unsigned long long>(st.dropped_ring));
		}
		status_->SetLabel(label);
	}
}

void
NetworkAnalyserWindow::OnSelected(wxListEvent &event)
{
	selected_ = event.GetIndex();
	ShowDetail(selected_);
}

void
NetworkAnalyserWindow::ShowDetail(long row)
{
	if (row < 0 || static_cast<size_t>(row) >= frames_.size()) {
		return;
	}
	const NetcapFrame &f = frames_[static_cast<size_t>(row)];

	{
		NetDissectLine lines[NETDIS_MAX_LINES];
		const unsigned n = netdis_detail(f.data, f.captured, lines,
		    NETDIS_MAX_LINES);
		wxString text;

		text += wxString::Format("Frame %llu: %u bytes on the wire, %u captured\n",
		    static_cast<unsigned long long>(f.serial), f.length, f.captured);
		text += (f.direction == NETCAP_TX) ? "Sent by the machine\n"
		                                   : "Received by the machine\n";
		for (unsigned i = 0; i < n; i++) {
			text += wxString(' ', lines[i].depth * 4);
			text += wxString::FromUTF8(lines[i].text);
			text += "\n";
		}
		detail_->SetValue(text);
	}

	{
		/* The usual layout: offset, sixteen bytes, then the printable ones. */
		wxString hex;

		for (uint32_t off = 0; off < f.captured; off += 16) {
			wxString line = wxString::Format("%04x  ", off);
			wxString ascii;
			uint32_t i;

			for (i = 0; i < 16; i++) {
				if (off + i < f.captured) {
					const uint8_t b = f.data[off + i];

					line += wxString::Format("%02x ", b);
					ascii += (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
				} else {
					line += "   ";
				}
				if (i == 7) {
					line += " ";
				}
			}
			hex += line + " " + ascii + "\n";
		}
		bytes_->SetValue(hex);
	}
}
