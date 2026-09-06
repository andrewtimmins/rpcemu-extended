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

#include "machine_inspector_window.h"

#include <wx/settings.h>


#include <algorithm>
#include <vector>

#include <wx/spinctrl.h>
#include <wx/filedlg.h>
#include <wx/textfile.h>

#include "emulator_snapshot.h"

extern "C" {
#include "arm.h"
#include "arm_common.h"	/* ARM_MODE_32: bit 4 of the mode says 26- or 32-bit */
#include "mem.h"	/* mem_debug_read: the translated, side-effect-free read */
}

#include <cmath>
#include <cstring>

#include "arm_disasm.h"
#include "cp15.h"

namespace {

wxString FormatHex(uint32_t value, int width = 8)
{
	return wxString::Format("0x%0*X", width, value);
}

/*
 * An FPA register, for the narrow left-hand column.
 *
 * Ten significant digits, which is enough to recognise a value and to see it
 * change without the column growing wide enough to push the disassembly off
 * the window. The full seventeen and the stored words are on the tooltip, and
 * the debug socket's "fpregs" gives both to a program. Infinities and NaNs are
 * spelled out rather than left to the platform's own %g.
 */
wxString FormatFpRegister(double v)
{
	if (std::isnan(v)) {
		return "nan";
	}
	if (std::isinf(v)) {
		return v < 0.0 ? "-inf" : "inf";
	}
	return wxString::Format("%.8g", v);
}

/*
 * arm.mode holds the mode number in its bottom FOUR bits, with bit 4 saying
 * whether the processor is in a 32-bit mode - see ARM_MODE_32/ARM_MODE_PRIV in
 * arm_common.h and the numbering in arm.h.
 *
 * Masking 0x1f instead of 0xf, as this did, made every 32-bit mode unmatchable:
 * User came out of the emulator as 0x10 and was reported as "Unknown (0x10)".
 */
/*
 * The mode as an assembler would write it: SVC32, FIQ26, UND32.
 *
 * The long form ("Abort 32-bit (privileged)") took a line of its own in a
 * column that has none to spare, and is not what anybody reading a PSR calls
 * these. Asked for by JonAbbott2 on discussion #223. The word size is part of
 * the name because on this machine it is part of the mode: bit 4 of the mode
 * field is what tells USR26 from USR32.
 */
wxString ModeToShortString(uint32_t mode)
{
	const char *name;

	switch (mode & 0xf) {
	case USER: name = "USR"; break;
	case FIQ: name = "FIQ"; break;
	case IRQ: name = "IRQ"; break;
	case SUPERVISOR: name = "SVC"; break;
	case ABORT: name = "ABT"; break;
	case UNDEFINED: name = "UND"; break;
	case SYSTEM: name = "SYS"; break;
	default:
		/* One of the nine the architecture reserves. Shown as the number,
		   because there is no name to give it and it is worth seeing. */
		return wxString::Format("MODE%02X", mode & 0x1fu);
	}

	return wxString::Format("%s%s", name, ARM_MODE_32(mode) ? "32" : "26");
}


wxString NetworkTypeToString(NetworkType type)
{
	switch (type) {
	case NetworkType_Off: return "Off";
	case NetworkType_NAT: return "NAT";
	default: return "Unknown";
	}
}

wxString VidcBppToString(uint32_t bit8)
{
	switch (bit8) {
	case 0: return "1 bpp (mono)";
	case 1: return "2 bpp (4 colours)";
	case 2: return "4 bpp (16 colours)";
	case 3: return "8 bpp (256 colours)";
	case 4: return "16 bpp (high colour)";
	case 6: return "32 bpp (true colour)";
	default: return wxString::Format("Unknown (%u)", bit8);
	}
}

} // namespace

static int
ListBoxSelectionCount(wxListBox *list)
{
	if (list == nullptr) {
		return 0;
	}
	wxArrayInt selections;
	return list->GetSelections(selections);
}

/*
 * Update a read-only text control only when its content actually changes.
 *
 * wxTextCtrl::SetValue()/ChangeValue() always clear the current selection and
 * reset the insertion point, even when the new text is identical to the old.
 * The inspector auto-refreshes every 500 ms, so rewriting an unchanged control
 * would repeatedly wipe any selection the user has made - which is exactly what
 * stops register values being selected and copied while the machine is paused
 * (the register text is frozen, yet was being rewritten regardless). Skipping
 * the no-op rewrite preserves the selection (and avoids needless flicker).
 */
static void
SetTextIfChanged(wxTextCtrl *ctrl, const wxString &text)
{
	if (ctrl != nullptr && ctrl->GetValue() != text) {
		ctrl->ChangeValue(text);
	}
}

wxBEGIN_EVENT_TABLE(MachineInspectorWindow, wxFrame)
	EVT_TIMER(wxID_ANY, MachineInspectorWindow::OnTimer)
	EVT_BUTTON(ID_REFRESH_NOW, MachineInspectorWindow::OnRefreshNow)
	EVT_CHECKBOX(ID_AUTO_REFRESH, MachineInspectorWindow::OnAutoRefresh)
	EVT_BUTTON(ID_DISASM_GO, MachineInspectorWindow::OnDisasmGo)
	EVT_CHECKBOX(ID_DISASM_FOLLOW_PC, MachineInspectorWindow::OnDisasmFollowPc)
	EVT_BUTTON(ID_MEMORY_GO, MachineInspectorWindow::OnMemoryGo)
	EVT_BUTTON(ID_MEMORY_REFRESH, MachineInspectorWindow::OnMemoryRefresh)
	EVT_CHECKBOX(ID_MEMORY_WORDS, MachineInspectorWindow::OnMemoryRefresh)
	EVT_BUTTON(ID_RUN, MachineInspectorWindow::OnRun)
	EVT_BUTTON(ID_PAUSE, MachineInspectorWindow::OnPause)
	EVT_BUTTON(ID_STEP, MachineInspectorWindow::OnStep)
	EVT_BUTTON(ID_STEP_OVER, MachineInspectorWindow::OnStepOver)
	EVT_CHECKBOX(ID_DISASM_STYLE, MachineInspectorWindow::OnDisasmStyleChanged)
	EVT_CHECKBOX(ID_AUTOSTEP, MachineInspectorWindow::OnAutoStep)
	EVT_TIMER(ID_AUTOSTEP_TIMER, MachineInspectorWindow::OnAutoStepTimer)
	EVT_BUTTON(ID_SETTINGS_SAVE, MachineInspectorWindow::OnSaveSettings)
	EVT_BUTTON(ID_SETTINGS_LOAD, MachineInspectorWindow::OnLoadSettings)
	EVT_BUTTON(ID_SWI_NAMES, MachineInspectorWindow::OnLoadSwiNames)
	EVT_BUTTON(ID_BREAKPOINT_ADD, MachineInspectorWindow::OnAddBreakpoint)
	EVT_BUTTON(ID_BREAKPOINT_REMOVE, MachineInspectorWindow::OnRemoveBreakpoint)
	EVT_BUTTON(ID_WATCHPOINT_ADD, MachineInspectorWindow::OnAddWatchpoint)
	EVT_BUTTON(ID_WATCHPOINT_REMOVE, MachineInspectorWindow::OnRemoveWatchpoint)
	EVT_CHECKBOX(ID_TRACE_CONFIG, MachineInspectorWindow::OnTraceConfigChanged)
	EVT_BUTTON(ID_TRACE_CLEAR, MachineInspectorWindow::OnTraceClear)
wxEND_EVENT_TABLE()

MachineInspectorWindow::MachineInspectorWindow(wxWindow *parent, EmulatorHost &emulator)
	: wxFrame(parent, wxID_ANY, "Machine Inspector",
	          wxDefaultPosition, wxSize(1250, 820),
	          wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER)
	, emulator_(emulator)
{
	BuildUi();

	/*
	 * Opened at a size everything actually fits in - registers, code, memory and
	 * the breakpoint lists at once - and floored below that. At the old 700x500
	 * the panes were each too small to read and the notebook page overlapped
	 * itself, since a sizer squeezed past its minimum overlaps rather than
	 * clips. Smaller than the screen, for the laptop it may be opened on.
	 */
	const wxSize screen = wxGetClientDisplayRect().GetSize();

	SetMinSize(wxSize(std::min(950, screen.x), std::min(640, screen.y)));
	SetSize(wxSize(std::min(1250, screen.x), std::min(820, screen.y)));
	CentreOnParent();
	refresh_timer_.Start(500);
	RefreshSnapshot();
}

void MachineInspectorWindow::ShowAndRaise()
{
	if (!IsShown()) {
		Show();
	}
	Raise();
	SetFocus();
	RefreshSnapshot();
}

/*
 * The registers and the processor's state, as the left-hand column of the
 * debugger view.
 *
 * Laid out rather than printed. R13/R14/R15 carry their usual names beside the
 * number, because SP/LR/PC is what anybody reading a stack trace is looking for,
 * and the PSR is broken out into its flags: "CPSR = 0x60000010" is a number to
 * decode by hand, where "N. Z. C* V. I* F." can be read at a glance.
 */
wxWindow *MachineInspectorWindow::BuildStatePanel(wxWindow *parent)
{
	/*
	 * Scrolled, because this column is a splitter pane with a height it does
	 * not choose. Adding the floating point registers to it made the content
	 * taller than the pane on a 820-pixel window, and a plain panel does not
	 * report that: the last box is simply cut off part way through its first
	 * row, which reads as a rendering fault rather than as a window that is
	 * too short. Scrolling also means the next thing added here cannot repeat
	 * it. The rate is set only vertically; the pane's width is the sash's.
	 */
	auto *panel = new wxScrolledWindow(parent);
	auto *sizer = new wxBoxSizer(wxVERTICAL);

	panel->SetScrollRate(0, 10);

	/*
	 * R0-R7 | R8-R15 | F0-F7, in one grid of eight rows.
	 *
	 * The FPA registers went in a box of their own below the Status box first,
	 * and that was wrong twice over: this column is a splitter pane whose
	 * height it does not choose, so the box was cut off part way through its
	 * first row on an 820-pixel window - which is what "squashed" looks like -
	 * and even scrolled into view it was a long way from the registers it
	 * belongs beside. A third column of the same grid costs no height at all
	 * and puts F0 where you are already looking. Discussion #257.
	 */
	auto *regs_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Registers");
	auto *grid = new wxFlexGridSizer(8, 6, 2, 8);

	for (int row = 0; row < 8; row++) {
		for (int half = 0; half < 2; half++) {
			const int r = half * 8 + row;
			static const char *const alias[16] = {
				"", "", "", "", "", "", "", "", "", "", "", "", "",
				"SP", "LR", "PC"
			};
			wxString name = wxString::Format("R%d", r);

			if (alias[r][0] != '\0') {
				name += wxString::Format(" %s", alias[r]);
			}
			reg_name_[r] = new wxStaticText(panel, wxID_ANY, name);
			reg_value_[r] = new wxStaticText(panel, wxID_ANY, "00000000");
			ApplyMonoFont(reg_value_[r]);
			/*
			 * Double-click a register to look at what it points at - R13 for
			 * the stack being the case that comes up every time. Asked for by
			 * JonAbbott2 on discussion #223; before it, reading the stack meant
			 * copying eight hex digits into the address box by eye.
			 */
			reg_value_[r]->SetToolTip(
			    "Double-click to show this address in the memory view");
			reg_value_[r]->Bind(wxEVT_LEFT_DCLICK,
			    [this, r](wxMouseEvent &) { ShowRegisterInMemoryView(r); });
			grid->Add(reg_name_[r], 0, wxALIGN_CENTER_VERTICAL);
			grid->Add(reg_value_[r], 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		}

		auto *fp_name = new wxStaticText(panel, wxID_ANY,
		    wxString::Format("F%d", row));

		fpreg_value_[row] = new wxStaticText(panel, wxID_ANY, "0");
		ApplyMonoFont(fpreg_value_[row]);
		/* Room for the widest value this shows, so the column does not resize
		   itself - and the pane's sash with it - as a register changes
		   magnitude. */
		fpreg_value_[row]->SetMinSize(
		    wxSize(fpreg_value_[row]->GetTextExtent("-1.2345679e-300").x, -1));
		grid->Add(fp_name, 0, wxALIGN_CENTER_VERTICAL);
		grid->Add(fpreg_value_[row], 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
	}
	regs_box->Add(grid, 0, wxALL, 4);
	sizer->Add(regs_box, 0, wxEXPAND | wxALL, 6);

	auto *psr_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Status");
	auto *flags = new wxBoxSizer(wxHORIZONTAL);
	static const char *const flag_names[7] = { "N", "Z", "C", "V", "I", "F", "T" };

	for (int i = 0; i < 7; i++) {
		flag_label_[i] = new wxStaticText(panel, wxID_ANY, flag_names[i]);
		ApplyMonoFont(flag_label_[i]);
		flag_label_[i]->SetToolTip(
		    i == 0 ? "Negative" : i == 1 ? "Zero" : i == 2 ? "Carry" :
		    i == 3 ? "Overflow" : i == 4 ? "IRQs disabled" :
		    i == 5 ? "FIQs disabled" : "Thumb");
		flags->Add(flag_label_[i], 0, wxRIGHT, 6);
	}
	psr_box->Add(flags, 0, wxALL, 4);

	cpsr_label_ = new wxStaticText(panel, wxID_ANY, "CPSR 00000000");
	ApplyMonoFont(cpsr_label_);
	cpsr_label_->SetToolTip(
	    "The processor status, and beside it the mode it names: SVC32, FIQ26 "
	    "and so on, with whether that mode is privileged.");
	mmu_label_ = new wxStaticText(panel, wxID_ANY, "MMU");
	core_label_ = new wxStaticText(panel, wxID_ANY, "Core");
	spsr_label_ = new wxStaticText(panel, wxID_ANY, "SPSR --------");
	ApplyMonoFont(spsr_label_);
	spsr_label_->SetToolTip(
	    "The saved PSR of the current mode, and the mode a return through it "
	    "would enter. User and System have none.");

	/*
	 * The FPA's two status words, beside the processor's. FPSR's top byte is
	 * the system ID the FPEmulator support code reads to decide whether it is
	 * driving a chip, so it is named rather than left as a number: a machine
	 * showing anything but 81 there is one where F0-F7 above are not the
	 * registers RISC OS is using.
	 */
	fpsr_label_ = new wxStaticText(panel, wxID_ANY, "FPSR 00000000");
	ApplyMonoFont(fpsr_label_);
	fpsr_label_->SetToolTip(
	    "The FPA status register. The top byte is the system ID: 81 is an "
	    "FPA10. The low bits are whatever the support code last wrote, since "
	    "the emulated chip does not raise the exception flags itself.");
	fpcr_label_ = new wxStaticText(panel, wxID_ANY, "FPCR 00000000");
	ApplyMonoFont(fpcr_label_);

	/* Side by side rather than one under the other. This column is a splitter
	   pane, and two more full-width lines were enough to push the last of it
	   out of sight at the window's default height. */
	auto *fpsr_row = new wxBoxSizer(wxHORIZONTAL);

	fpsr_row->Add(fpsr_label_, 0, wxRIGHT, 12);
	fpsr_row->Add(fpcr_label_, 0);

	psr_box->Add(cpsr_label_, 0, wxLEFT | wxBOTTOM, 4);
	psr_box->Add(spsr_label_, 0, wxLEFT | wxBOTTOM, 4);
	psr_box->Add(fpsr_row, 0, wxLEFT | wxBOTTOM, 4);
	psr_box->Add(mmu_label_, 0, wxLEFT | wxBOTTOM, 4);
	psr_box->Add(core_label_, 0, wxLEFT | wxBOTTOM, 4);
	sizer->Add(psr_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);


	/* No stretch spacer: in a scrolled window it would claim whatever height
	   was left and the virtual size would never exceed the pane, which is the
	   one thing that has to happen for a scrollbar to appear. */
	panel->SetSizer(sizer);
	panel->FitInside();
	return panel;
}

void MachineInspectorWindow::BuildUi()
{
	summary_label_ = new wxStaticText(this, wxID_ANY, "Awaiting snapshot");
	auto_refresh_checkbox_ = new wxCheckBox(this, ID_AUTO_REFRESH, "Auto refresh");
	auto_refresh_checkbox_->SetValue(true);
	auto_refresh_checkbox_->SetToolTip("Refresh the view automatically every 500 ms");

	auto *refresh_button = new wxButton(this, ID_REFRESH_NOW, "Refresh now");

	auto *controls = new wxBoxSizer(wxHORIZONTAL);
	controls->Add(summary_label_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	controls->AddStretchSpacer();
	controls->Add(auto_refresh_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	controls->Add(refresh_button, 0);

	/*
	 * ★ One view rather than a tab per thing.
	 *
	 * The registers, the code and the memory are read together - a register is
	 * only meaningful beside the instruction that set it - so putting them on
	 * separate pages meant switching tabs and holding values in your head. The
	 * outer split puts the machine's state down the left with the code and
	 * memory to the right; the inner one divides those two, and both sashes are
	 * the user's to move.
	 *
	 * Trace and Peripherals stay on a notebook below: they are read on their
	 * own, and neither wants to be a quarter of the window all the time.
	 */
	/*
	 * Three sashes, because which pane matters depends on what is being looked
	 * at: reading code wants the disassembly tall, following a trace wants the
	 * log tall, and neither should be settled for the user by a fixed
	 * proportion.
	 */
	auto *split_outer = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
	    wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
	auto *split_main = new wxSplitterWindow(split_outer, wxID_ANY, wxDefaultPosition,
	    wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
	auto *split_code = new wxSplitterWindow(split_main, wxID_ANY,
	    wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);

	split_main->SetMinimumPaneSize(180);
	split_code->SetMinimumPaneSize(120);

	wxWindow *const state_panel = BuildStatePanel(split_main);

	auto *disasm_panel = new wxPanel(split_code);
	disasm_address_input_ = new wxTextCtrl(disasm_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
	disasm_address_input_->SetHint("Address (hex)");
	auto *disasm_go_button = new wxButton(disasm_panel, ID_DISASM_GO, "Go");
	disasm_follow_pc_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_FOLLOW_PC, "Follow PC");
	disasm_follow_pc_checkbox_->SetValue(true);

	/* How the disassembly is written down: see ArmDisasmOptions. Off by
	   default, so the view reads as it always has until asked otherwise. */
	disasm_lower_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_STYLE, "lower");
	disasm_lower_checkbox_->SetToolTip(
	    "Lower case, to match assembler source. SWI names and symbols keep "
	    "their own case.");
	disasm_hex_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_STYLE, "Hex");
	disasm_hex_checkbox_->SetToolTip("Immediates as &80 rather than 128");
	disasm_apcs_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_STYLE, "APCS");
	disasm_apcs_checkbox_->SetToolTip(
	    "Register names as a1-a4, v1-v5, sb, sl, fp, ip, sp, lr, pc");
	disasm_ranges_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_STYLE, "Ranges");
	disasm_ranges_checkbox_->SetToolTip(
	    "Collapse LDM/STM register lists: {R0-R4, R7}");
	disasm_resolve_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_STYLE, "PC-rel");
	disasm_resolve_checkbox_->SetToolTip(
	    "Show a PC-relative load as the address it reaches");

	auto *disasm_controls = new wxBoxSizer(wxHORIZONTAL);
	disasm_controls->Add(new wxStaticText(disasm_panel, wxID_ANY, "Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	disasm_controls->Add(disasm_address_input_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_go_button, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_follow_pc_checkbox_, 0, wxRIGHT, 12);
	disasm_controls->Add(disasm_lower_checkbox_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_hex_checkbox_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_apcs_checkbox_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_ranges_checkbox_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_resolve_checkbox_, 0, wxRIGHT, 12);

	auto *swi_names_button = new wxButton(disasm_panel, ID_SWI_NAMES,
	    "SWI names...", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	swi_names_button->SetToolTip(
	    "Load a CSV of SWI numbers and names, so a module's own SWIs "
	    "disassemble by name instead of by number");
	disasm_controls->Add(swi_names_button, 0);

	/* wxTE_RICH so one line can be coloured; wxMSW needs it and wxOSX ignores
	   it. See HighlightCurrentInstruction(). */
	disasm_view_ = new wxTextCtrl(disasm_panel, wxID_ANY, wxEmptyString,
	                              wxDefaultPosition, wxDefaultSize,
	                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP |
	                              wxTE_RICH);
	ApplyMonoFont(disasm_view_);

	auto *disasm_sizer = new wxBoxSizer(wxVERTICAL);
	disasm_sizer->Add(disasm_controls, 0, wxEXPAND | wxALL, 8);
	disasm_sizer->Add(disasm_view_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	disasm_panel->SetSizer(disasm_sizer);

	auto *memory_panel = new wxPanel(split_code);
	memory_address_input_ = new wxTextCtrl(memory_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
	memory_address_input_->SetHint("Address (hex)");
	memory_bytes_spin_ = new wxSpinCtrl(memory_panel, wxID_ANY, wxEmptyString,
	                                    wxDefaultPosition, wxSize(80, -1),
	                                    wxSP_ARROW_KEYS, 16, 4096, 256);
	memory_bytes_spin_->SetToolTip("Number of bytes to display");
	memory_physical_checkbox_ = new wxCheckBox(memory_panel, wxID_ANY, "Physical");
	memory_physical_checkbox_->SetToolTip(
	    "Read the address as a physical one. Off, it is a virtual address and "
	    "goes through the MMU, which is what every other address in this window "
	    "means. Matches the debug socket's 'mem <addr> <len> [phys]'.");
	/*
	 * Words, for reading a table of pointers or a block of instructions
	 * without assembling four bytes in your head. Asked for by JonAbbott2 on
	 * discussion #223. Little-endian, because that is how this machine stores
	 * a word and the point is to read the value the guest would.
	 */
	memory_words_checkbox_ = new wxCheckBox(memory_panel, ID_MEMORY_WORDS, "Words");
	memory_words_checkbox_->SetToolTip(
	    "Show 32-bit words, little-endian as the machine stores them, rather "
	    "than bytes");
	auto *memory_go_button = new wxButton(memory_panel, ID_MEMORY_GO, "Go");
	auto *memory_refresh_button = new wxButton(memory_panel, ID_MEMORY_REFRESH, "Refresh");

	auto *memory_controls = new wxBoxSizer(wxHORIZONTAL);
	memory_controls->Add(new wxStaticText(memory_panel, wxID_ANY, "Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_address_input_, 0, wxRIGHT, 6);
	memory_controls->Add(new wxStaticText(memory_panel, wxID_ANY, "Bytes:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_bytes_spin_, 0, wxRIGHT, 6);
	memory_controls->Add(memory_physical_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_words_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_go_button, 0, wxRIGHT, 6);
	memory_controls->Add(memory_refresh_button, 0);

	memory_view_ = new wxTextCtrl(memory_panel, wxID_ANY, wxEmptyString,
	                              wxDefaultPosition, wxDefaultSize,
	                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(memory_view_);

	auto *memory_sizer = new wxBoxSizer(wxVERTICAL);
	memory_sizer->Add(memory_controls, 0, wxEXPAND | wxALL, 8);
	memory_sizer->Add(memory_view_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	memory_panel->SetSizer(memory_sizer);

	split_code->SplitHorizontally(disasm_panel, memory_panel);
	split_main->SplitVertically(state_panel, split_code);

	auto *notebook = new wxNotebook(split_outer, wxID_ANY);
	auto *debug_panel = new wxPanel(notebook);
	debug_status_label_ = new wxStaticText(debug_panel, wxID_ANY, "State unknown");
	debug_hit_label_ = new wxStaticText(debug_panel, wxID_ANY, "Last watchpoint: none");

	run_button_ = new wxButton(debug_panel, ID_RUN, "Run");
	pause_button_ = new wxButton(debug_panel, ID_PAUSE, "Pause");
	step_button_ = new wxButton(debug_panel, ID_STEP, "Step");
	step_over_button_ = new wxButton(debug_panel, ID_STEP_OVER, "Step over");
	step_over_button_->SetToolTip(
	    "Run to the next instruction without going into a call or a SWI");
	run_button_->Enable(false);
	pause_button_->Enable(false);
	step_button_->Enable(false);

	/*
	 * Transport and state on one row. This page shares the window with the code
	 * and memory views now, so every row it does not need is a row they get.
	 */
	auto *debug_buttons = new wxBoxSizer(wxHORIZONTAL);
	debug_buttons->Add(run_button_, 0, wxRIGHT, 6);
	debug_buttons->Add(pause_button_, 0, wxRIGHT, 6);
	debug_buttons->Add(step_button_, 0, wxRIGHT, 6);
	debug_buttons->Add(step_over_button_, 0, wxRIGHT, 12);

	autostep_checkbox_ = new wxCheckBox(debug_panel, ID_AUTOSTEP, "Auto");
	autostep_checkbox_->SetToolTip(
	    "Keep stepping at the rate beside this, so code can be watched running "
	    "slowly without holding down Step. Ticking it stops a running machine "
	    "first; pressing Run turns it off again.");
	autostep_rate_spin_ = new wxSpinCtrl(debug_panel, wxID_ANY, "4",
	                                     wxDefaultPosition, wxSize(70, -1),
	                                     wxSP_ARROW_KEYS, 1, 200, 4);
	autostep_rate_spin_->SetToolTip("Steps per second");
	debug_buttons->Add(autostep_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
	debug_buttons->Add(autostep_rate_spin_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
	debug_buttons->Add(new wxStaticText(debug_panel, wxID_ANY, "/sec"), 0,
	    wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
	debug_buttons->Add(debug_status_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
	debug_buttons->Add(debug_hit_label_, 1, wxALIGN_CENTER_VERTICAL);

	auto *breakpoint_box = new wxStaticBoxSizer(wxVERTICAL, debug_panel, "Breakpoints");
	breakpoint_list_ = new wxListBox(debug_panel, wxID_ANY, wxDefaultPosition,
	                                 wxSize(-1, 90), 0, nullptr, wxLB_EXTENDED);
	breakpoint_input_ = new wxTextCtrl(debug_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
	breakpoint_input_->SetHint("Address (hex)");
	auto *breakpoint_add_button = new wxButton(debug_panel, ID_BREAKPOINT_ADD, "Add");
	breakpoint_remove_button_ = new wxButton(debug_panel, ID_BREAKPOINT_REMOVE, "Remove selected");
	breakpoint_remove_button_->Enable(false);

	auto *breakpoint_controls = new wxBoxSizer(wxHORIZONTAL);
	breakpoint_controls->Add(breakpoint_input_, 1, wxEXPAND | wxRIGHT, 6);
	breakpoint_controls->Add(breakpoint_add_button, 0, wxRIGHT, 6);
	breakpoint_controls->Add(breakpoint_remove_button_, 0);
	breakpoint_box->Add(breakpoint_list_, 1, wxEXPAND | wxBOTTOM, 6);
	breakpoint_box->Add(breakpoint_controls, 0, wxEXPAND);

	auto *watchpoint_box = new wxStaticBoxSizer(wxVERTICAL, debug_panel, "Watchpoints");
	watchpoint_list_ = new wxListBox(debug_panel, wxID_ANY, wxDefaultPosition,
	                                 wxSize(-1, 90), 0, nullptr, wxLB_EXTENDED);
	watchpoint_address_input_ = new wxTextCtrl(debug_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
	watchpoint_address_input_->SetHint("Address (hex)");
	watchpoint_size_choice_ = new wxChoice(debug_panel, wxID_ANY);
	watchpoint_size_choice_->Append("1 byte");
	watchpoint_size_choice_->Append("2 bytes");
	watchpoint_size_choice_->Append("4 bytes");
	watchpoint_size_choice_->Append("8 bytes");
	watchpoint_size_choice_->SetSelection(2);
	watchpoint_read_checkbox_ = new wxCheckBox(debug_panel, wxID_ANY, "Read");
	watchpoint_write_checkbox_ = new wxCheckBox(debug_panel, wxID_ANY, "Write");
	watchpoint_log_only_checkbox_ = new wxCheckBox(debug_panel, wxID_ANY, "Log only");
	watchpoint_log_only_checkbox_->SetToolTip("Record matching accesses to the Trace tab instead of halting");
	watchpoint_read_checkbox_->SetValue(true);
	watchpoint_write_checkbox_->SetValue(true);
	auto *watchpoint_add_button = new wxButton(debug_panel, ID_WATCHPOINT_ADD, "Add");
	watchpoint_remove_button_ = new wxButton(debug_panel, ID_WATCHPOINT_REMOVE, "Remove selected");
	watchpoint_remove_button_->Enable(false);

	auto *watchpoint_entry = new wxBoxSizer(wxHORIZONTAL);
	watchpoint_entry->Add(watchpoint_address_input_, 1, wxEXPAND | wxRIGHT, 6);
	watchpoint_entry->Add(watchpoint_size_choice_, 0, wxRIGHT, 6);
	watchpoint_entry->Add(watchpoint_add_button, 0);

	auto *watchpoint_options = new wxBoxSizer(wxHORIZONTAL);
	watchpoint_options->Add(watchpoint_read_checkbox_, 0,
	    wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	watchpoint_options->Add(watchpoint_write_checkbox_, 0,
	    wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	watchpoint_options->Add(watchpoint_log_only_checkbox_, 0,
	    wxALIGN_CENTER_VERTICAL);
	watchpoint_options->AddStretchSpacer();
	watchpoint_options->Add(watchpoint_remove_button_, 0);

	watchpoint_box->Add(watchpoint_list_, 1, wxEXPAND | wxBOTTOM, 6);
	watchpoint_box->Add(watchpoint_entry, 0, wxEXPAND | wxBOTTOM, 4);
	watchpoint_box->Add(watchpoint_options, 0, wxEXPAND);

	auto *debug_lists = new wxBoxSizer(wxHORIZONTAL);
	debug_lists->Add(breakpoint_box, 1, wxEXPAND | wxRIGHT, 8);
	debug_lists->Add(watchpoint_box, 2, wxEXPAND);

	auto *debug_sizer = new wxBoxSizer(wxVERTICAL);
	debug_sizer->Add(debug_buttons, 0, wxEXPAND | wxALL, 8);
	debug_sizer->Add(debug_lists, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	debug_panel->SetSizer(debug_sizer);
	notebook->AddPage(debug_panel, "Debugger");

	auto *trace_panel = new wxPanel(notebook);

	auto *exception_box = new wxStaticBoxSizer(wxHORIZONTAL, trace_panel, "Halt on exception");
	trap_undefined_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Undefined instruction");
	trap_prefetch_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Prefetch abort");
	trap_data_abort_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Data abort");
	log_exceptions_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Log all exceptions");
	exception_box->Add(trap_undefined_checkbox_, 0, wxALL, 4);
	exception_box->Add(trap_prefetch_checkbox_, 0, wxALL, 4);
	exception_box->Add(trap_data_abort_checkbox_, 0, wxALL, 4);
	exception_box->Add(log_exceptions_checkbox_, 0, wxALL, 4);

	auto *swi_box = new wxStaticBoxSizer(wxHORIZONTAL, trace_panel, "SWI tracing");
	swi_trace_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Log SWIs");
	swi_halt_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG, "Halt on SWI");
	swi_filter_min_input_ = new wxTextCtrl(trace_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(90, -1), wxTE_PROCESS_ENTER);
	swi_filter_min_input_->SetHint("min (hex)");
	swi_filter_max_input_ = new wxTextCtrl(trace_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(90, -1), wxTE_PROCESS_ENTER);
	swi_filter_max_input_->SetHint("max (hex)");
	swi_box->Add(swi_trace_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
	swi_box->Add(swi_halt_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
	swi_box->Add(new wxStaticText(trace_panel, wxID_ANY, "Filter:"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
	swi_box->Add(swi_filter_min_input_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
	swi_box->Add(new wxStaticText(trace_panel, wxID_ANY, ".."), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
	swi_box->Add(swi_filter_max_input_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

	auto *step_box = new wxStaticBoxSizer(wxHORIZONTAL, trace_panel,
	    "While stepping, pass through");
	step_skip_irq_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG,
	    "IRQ and FIQ");
	step_skip_irq_checkbox_->SetToolTip(
	    "A step that lands in an interrupt keeps going until it is back out. "
	    "Breakpoints still fire there.");
	step_skip_os_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG,
	    "the OS (ROM)");
	step_skip_os_checkbox_->SetToolTip(
	    "The same for anything at or above &F0000000, which is the ROM.");
	step_skip_swi_checkbox_ = new wxCheckBox(trace_panel, ID_TRACE_CONFIG,
	    "SWIs");
	step_skip_swi_checkbox_->SetToolTip(
	    "A step that executes a SWI runs it to completion and stops at the "
	    "instruction after it, whether its handler is in the ROM or in a "
	    "module in RAM. A trapped SWI, and a breakpoint inside the handler, "
	    "still stop.");
	step_box->Add(step_skip_irq_checkbox_, 0, wxALL, 4);
	step_box->Add(step_skip_os_checkbox_, 0, wxALL, 4);
	step_box->Add(step_skip_swi_checkbox_, 0, wxALL, 4);
	step_box->AddStretchSpacer();
	step_box->Add(new wxButton(trace_panel, ID_SETTINGS_SAVE, "Save session..."),
	    0, wxALL, 2);
	step_box->Add(new wxButton(trace_panel, ID_SETTINGS_LOAD, "Load session..."),
	    0, wxALL, 2);

	/* A floor on the log, so the tab opens showing a useful number of lines
	   rather than three; the sash below the code views takes it further. */
	trace_view_ = new wxTextCtrl(trace_panel, wxID_ANY, wxEmptyString,
	                             wxDefaultPosition, wxSize(-1, 120),
	                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(trace_view_);

	trace_autoscroll_checkbox_ = new wxCheckBox(trace_panel, wxID_ANY, "Auto-scroll");
	trace_autoscroll_checkbox_->SetValue(true);
	trace_dropped_label_ = new wxStaticText(trace_panel, wxID_ANY, "Dropped: 0");
	auto *trace_clear_button = new wxButton(trace_panel, ID_TRACE_CLEAR, "Clear");

	auto *trace_footer = new wxBoxSizer(wxHORIZONTAL);
	trace_footer->Add(trace_autoscroll_checkbox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	trace_footer->Add(trace_dropped_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	trace_footer->AddStretchSpacer();
	trace_footer->Add(trace_clear_button, 0);

	auto *trace_sizer = new wxBoxSizer(wxVERTICAL);
	trace_sizer->Add(exception_box, 0, wxEXPAND | wxALL, 8);
	trace_sizer->Add(swi_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	trace_sizer->Add(step_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	trace_sizer->Add(trace_view_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
	trace_sizer->Add(trace_footer, 0, wxEXPAND | wxALL, 8);
	trace_panel->SetSizer(trace_sizer);
	notebook->AddPage(trace_panel, "Trace");


	peripheral_view_ = new wxTextCtrl(notebook, wxID_ANY, wxEmptyString,
	                                  wxDefaultPosition, wxDefaultSize,
	                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(peripheral_view_);
	notebook->AddPage(peripheral_view_, "Peripherals");

	split_outer->SetMinimumPaneSize(120);
	split_outer->SplitHorizontally(split_main, notebook);
	split_outer->SetSashGravity(0.7);

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(controls, 0, wxEXPAND | wxALL, 8);
	main->Add(split_outer, 1, wxEXPAND | wxALL, 8);
	SetSizer(main);

	/*
	 * The state column is sashed to what it actually needs rather than to a
	 * fraction of the window: at a quarter it clipped the second column of
	 * registers and cut "privileged" off the mode. The code and memory split
	 * goes on proportion, since both grow usefully.
	 */
	CallAfter([split_outer, split_main, split_code, state_panel, notebook] {
		const int wanted = state_panel->GetBestSize().x + 12;
		const int height = split_outer->GetClientSize().y;

		/* The notebook opens at the height its own contents need, so nothing on
		   the page is squeezed, and the code above it takes what is left. Below
		   that it keeps a usable share and the page scrolls off, which is the
		   right way round on a small screen. */
		split_outer->SetSashPosition(std::max(height / 2,
		    height - notebook->GetBestSize().y));
		split_main->SetSashPosition(wanted);
		split_code->SetSashPosition(split_code->GetClientSize().y * 3 / 5);
	});

	disasm_address_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnDisasmGo, this);
	memory_address_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnMemoryGo, this);
	/* The same number in the other address space, so re-read it rather than
	   leaving the previous space's bytes on screen under the new setting. */
	memory_physical_checkbox_->Bind(wxEVT_CHECKBOX,
	    [this](wxCommandEvent &) { RefreshMemoryView(memory_current_address_); });
	breakpoint_list_->Bind(wxEVT_LISTBOX, &MachineInspectorWindow::OnBreakpointSelection, this);
	watchpoint_list_->Bind(wxEVT_LISTBOX, &MachineInspectorWindow::OnWatchpointSelection, this);
	/* Keys on the frame and on the two read-only views, which are where the
	   focus lands while stepping. A text entry keeps its own keys. */
	Bind(wxEVT_CHAR_HOOK, &MachineInspectorWindow::OnDebugKey, this);

	swi_filter_min_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnTraceConfigChanged, this);
	swi_filter_max_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnTraceConfigChanged, this);
}

void MachineInspectorWindow::ApplyMonoFont(wxWindow *window)
{
	wxFont mono(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
	window->SetFont(mono);
}

void MachineInspectorWindow::OnTimer(wxTimerEvent &)
{
	if (auto_refresh_checkbox_->GetValue()) {
		RefreshSnapshot();
	}
	DrainTraceEvents();
}

void MachineInspectorWindow::OnRefreshNow(wxCommandEvent &)
{
	RefreshSnapshot();
}

void MachineInspectorWindow::OnAutoRefresh(wxCommandEvent &event)
{
	if (event.IsChecked()) {
		if (!refresh_timer_.IsRunning()) {
			refresh_timer_.Start(500);
		}
	} else {
		refresh_timer_.Stop();
	}
}

void MachineInspectorWindow::RefreshFromStateChange()
{
	/* Only worth the round trip while the window is up. */
	if (IsShown()) {
		RefreshSnapshot();
	}
}

void MachineInspectorWindow::RefreshSnapshot()
{
	const MachineSnapshot snapshot = emulator_.TakeSnapshot();
	ApplySnapshot(snapshot);
}

void MachineInspectorWindow::ApplySnapshot(const MachineSnapshot &snapshot)
{
	last_snapshot_ = snapshot;

	summary_label_->SetLabel(MakeSummary(snapshot));
	ApplyProcessorState(snapshot);
	SetTextIfChanged(peripheral_view_, FormatPeripheralSummary(snapshot));
	PopulateBreakpointList(snapshot);
	PopulateWatchpointList(snapshot);
	UpdateDebuggerUi(snapshot);
	SeedTraceConfig(snapshot);

	/* Something useful in the memory pane on the first snapshot rather than an
	   empty box: the stack is what a stopped machine is usually asked about. */
	if (!memory_address_chosen_ && snapshot.regs[13] != 0) {
		RefreshMemoryView(snapshot.regs[13] & ~0xfu);
	}

	if (disasm_follow_pc_checkbox_->GetValue()) {
		/*
		 * Back from the PC, not at it.
		 *
		 * Starting the view at the PC put the instruction about to run on the
		 * top line, so the instructions that led to it - the ones you want
		 * when you have just stopped somewhere unexpected - were off the top
		 * and had to be fetched by typing a lower address in by hand. Asked
		 * for in discussion #223. Clamped so it cannot wrap below zero.
		 */
		const uint32_t back = kDisasmLeadIn * 4u;
		/*
		 * Where the machine STOPPED, not where R15 now points.
		 *
		 * They are the same for a step, a breakpoint or a manual pause. They
		 * are not the same for a trapped abort: the exception has been taken
		 * by then, so R15 is on the hardware vector while halt_pc is the
		 * instruction that faulted - which the debugger already works out and
		 * the status line already prints. The view followed R15 and so opened
		 * on the vector, which is not where anybody wants to be looking.
		 * Reported by JonAbbott2 on discussion #223.
		 */
		const uint32_t follow = (snapshot.debug_paused != 0)
		    ? snapshot.debug_halt_pc : snapshot.pc;

		RefreshDisassembly(follow >= back ? follow - back : 0u);
	}
}

/*
 * Fill in the registers and the processor's state.
 *
 * A register whose value has moved since the last refresh is coloured, which is
 * the whole reason these are separate controls: stepping through code, what you
 * want to see is which register the instruction just changed, and a wall of
 * identical text will not tell you.
 */
void MachineInspectorWindow::ApplyProcessorState(const MachineSnapshot &snapshot)
{
	const wxColour changed(0xff, 0x8c, 0x00);	/* amber, readable on either theme */
	const wxColour normal = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const wxColour dim = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

	for (int r = 0; r < 16; r++) {
		const uint32_t value = (r == 15) ? snapshot.pc : snapshot.regs[r];
		const wxString text = wxString::Format("%08X", value);

		if (reg_value_[r]->GetLabel() != text) {
			reg_value_[r]->SetLabel(text);
		}

		const bool moved = have_previous_regs_ && previous_regs_[r] != value;

		reg_value_[r]->SetForegroundColour(moved ? changed : normal);
		previous_regs_[r] = value;
	}
	have_previous_regs_ = true;

	/* The flags, lit when set and dimmed when not, rather than a hex word to
	   decode by hand. I and F are "disabled" bits, which is how the PSR carries
	   them - a lit I means interrupts are OFF. */
	static const uint32_t flag_bits[7] = {
		0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u,	/* N Z C V */
		0x08000000u, 0x04000000u, 0x00000020u			/* I F T   */
	};

	for (int i = 0; i < 7; i++) {
		const bool set = (snapshot.cpsr & flag_bits[i]) != 0;

		flag_label_[i]->SetForegroundColour(set ? changed : dim);
	}

	cpsr_label_->SetLabel(wxString::Format("CPSR %08X  %s %s", snapshot.cpsr,
	    ModeToShortString(snapshot.mode),
	    snapshot.privileged_mode ? "priv" : "user"));

	/*
	 * The SPSR, and - the useful part - the mode a return through it would
	 * enter. Tracing a fault back through an exception return means knowing
	 * where it is about to go, and the number alone does not say.
	 */
	if (snapshot.spsr_valid) {
		spsr_label_->SetLabel(wxString::Format("SPSR %08X  %s",
		    snapshot.spsr, ModeToShortString(snapshot.spsr)));
	} else {
		spsr_label_->SetLabel("SPSR --------  none in this mode");
	}
	mmu_label_->SetLabel(wxString::Format("MMU %s",
	    snapshot.mmu_enabled ? "enabled" : "disabled"));
	core_label_->SetLabel(wxString::Format("%s, idle %s, %.0f MIPS",
	    snapshot.dynarec ? "Dynarec" : "Interpreter",
	    snapshot.cpu_idle_enabled ? "on" : "off",
	    snapshot.perf_mips));

	/*
	 * The FPA's registers, coloured on the same rule as the ARM ones: what
	 * moved since the last look. The comparison is on the stored words rather
	 * than the value, so a change too small for the displayed digits still
	 * shows as a change.
	 */
	for (int f = 0; f < 8; f++) {
		const wxString text = FormatFpRegister(snapshot.fpvalues[f]);
		bool moved = false;

		if (fpreg_value_[f]->GetLabel() != text) {
			fpreg_value_[f]->SetLabel(text);
		}
		for (int w = 0; w < 3; w++) {
			moved = moved || (have_previous_fpregs_ &&
			    previous_fpregs_[f][w] != snapshot.fpregs[f][w]);
			previous_fpregs_[f][w] = snapshot.fpregs[f][w];
		}
		fpreg_value_[f]->SetForegroundColour(moved ? changed : normal);

		/*
		 * Only when the register has actually moved. Setting a tooltip on
		 * macOS tears down the control's tracking area and installs a new
		 * one, so doing it unconditionally re-made eight of them twice a
		 * second - and a mouse enter or leave arriving mid-swap crashed the
		 * window inside wxNSWindow's event filter, with nothing of ours on
		 * the stack. Nothing needs rewriting while the value has not changed.
		 */
		if (moved || !have_previous_fpregs_) {
			fpreg_value_[f]->SetToolTip(wxString::Format(
			    "F%d = %.17g\nExtended: %08X %08X %08X",
			    f, snapshot.fpvalues[f], snapshot.fpregs[f][0],
			    snapshot.fpregs[f][1], snapshot.fpregs[f][2]));
		}
	}
	have_previous_fpregs_ = true;

	/* The system ID byte named, because "is this machine using the FPA at all"
	   is the question the register is usually being read to answer. */
	fpsr_label_->SetLabel(wxString::Format("FPSR %08X%s", snapshot.fpsr,
	    (snapshot.fpsr >> 24) == 0x81 ? " (FPA10)" : ""));
	fpcr_label_->SetLabel(wxString::Format("FPCR %08X", snapshot.fpcr));

	/* One refresh for the lot: colouring a label does not repaint it. */
	for (int r = 0; r < 16; r++) {
		reg_value_[r]->Refresh();
	}
	for (int f = 0; f < 8; f++) {
		fpreg_value_[f]->Refresh();
	}
	for (int i = 0; i < 7; i++) {
		flag_label_[i]->Refresh();
	}
}

wxString MachineInspectorWindow::FormatPeripheralSummary(const MachineSnapshot &snapshot) const
{
	const VIDCStateSnapshot &vidc = snapshot.vidc;
	const uint32_t host_width = vidc.screen_width * (snapshot.vidc_double_x ? 2u : 1u);
	const uint32_t host_height = vidc.screen_height * (snapshot.vidc_double_y ? 2u : 1u);

	wxString scaling = "none";
	if (snapshot.vidc_double_x && snapshot.vidc_double_y) {
		scaling = "horizontal, vertical";
	} else if (snapshot.vidc_double_x) {
		scaling = "horizontal";
	} else if (snapshot.vidc_double_y) {
		scaling = "vertical";
	}

	wxString text;
	text += wxString::Format("RAM: %u MB | VRAM: %u MB\n",
	                         snapshot.config_mem_size, snapshot.config_vram_size);
	text += wxString::Format("Network: %s\n", NetworkTypeToString(snapshot.network_type));
	text += wxString::Format("IOMD IRQ A: status=%s mask=%s\n",
	                         FormatHex(snapshot.iomd_irqa_status, 2),
	                         FormatHex(snapshot.iomd_irqa_mask, 2));
	text += wxString::Format("IOMD IRQ B: status=%s mask=%s\n",
	                         FormatHex(snapshot.iomd_irqb_status, 2),
	                         FormatHex(snapshot.iomd_irqb_mask, 2));
	text += wxString::Format("IOMD FIQ: status=%s mask=%s\n",
	                         FormatHex(snapshot.iomd_fiq_status, 2),
	                         FormatHex(snapshot.iomd_fiq_mask, 2));
	text += wxString::Format("IOMD DMA: status=%s mask=%s\n",
	                         FormatHex(snapshot.iomd_dma_status, 2),
	                         FormatHex(snapshot.iomd_dma_mask, 2));
	text += wxString::Format("Timer0: counter=%u in=%u out=%u\n",
	                         snapshot.iomd_timer0_counter,
	                         snapshot.iomd_timer0_in_latch,
	                         snapshot.iomd_timer0_out_latch);
	text += wxString::Format("Timer1: counter=%u in=%u out=%u\n",
	                         snapshot.iomd_timer1_counter,
	                         snapshot.iomd_timer1_in_latch,
	                         snapshot.iomd_timer1_out_latch);
	text += wxString::Format("Sound DMA status: %s\n", FormatHex(snapshot.iomd_sound_status, 2));
	text += wxString::Format("Floppy motor: %s\n", snapshot.floppy_motor_on ? "on" : "off");
	text += wxString::Format("VIDC: %ux%u (host %ux%u) | scaling %s | %s\n",
	                         vidc.screen_width, vidc.screen_height,
	                         host_width, host_height,
	                         scaling, VidcBppToString(vidc.bit8));

	for (int slot = 0; slot < 8; slot++) {
		const PodulesStateSnapshot &pod = snapshot.podules;
		wxString attrs = pod.slot_used[slot] ? "populated" : "empty";
		if (pod.irq[slot]) {
			attrs += ", IRQ";
		}
		if (pod.fiq[slot]) {
			attrs += ", FIQ";
		}
		text += wxString::Format("Podule slot %d: %s\n", slot, attrs);
	}

	return text;
}



wxString MachineInspectorWindow::MakeSummary(const MachineSnapshot &snapshot) const
{
	const wxString core = snapshot.dynarec ? "Dynarec" : "Interpreter";
	wxString debug_state;
	if (snapshot.debug_paused) {
		debug_state = "Paused";
	} else if (snapshot.debug_pause_requested) {
		debug_state = wxString::FromUTF8("Pausing\xE2\x80\xA6");
	} else {
		debug_state = "Running";
	}

	return wxString::Format("%s | %s (%s) | Network %s | Debug %s",
	                        wxString::FromUTF8(snapshot.model_name),
	                        wxString::FromUTF8(snapshot.cpu_name),
	                        core,
	                        NetworkTypeToString(snapshot.network_type),
	                        debug_state);
}

uint32_t MachineInspectorWindow::ParseAddress(const wxString &text, bool *ok) const
{
	wxString trimmed = text;
	trimmed.Trim(true).Trim(false);
	const wxString lower = trimmed.Lower();
	if (lower.EndsWith("h")) {
		trimmed = trimmed.Left(trimmed.length() - 1);
	}
	if (lower.StartsWith("0x")) {
		trimmed = trimmed.Mid(2);
	}

	unsigned long parsed = 0;
	const bool local_ok = trimmed.ToULong(&parsed, 16) || trimmed.ToULong(&parsed, 10);
	const bool valid = local_ok && parsed <= 0xffffffffu;

	if (ok != nullptr) {
		*ok = valid;
	}
	return valid ? static_cast<uint32_t>(parsed) : 0;
}

void MachineInspectorWindow::PopulateBreakpointList(const MachineSnapshot &snapshot)
{
	std::vector<int> selected;
	for (unsigned int i = 0; i < breakpoint_list_->GetCount(); i++) {
		if (breakpoint_list_->IsSelected(i)) {
			selected.push_back(static_cast<int>(i));
		}
	}

	breakpoint_list_->Clear();
	for (uint32_t i = 0; i < snapshot.debug_breakpoint_count; i++) {
		const uint32_t address = snapshot.debug_breakpoints[i].address;
		breakpoint_list_->Append(FormatHex(address), reinterpret_cast<void *>(static_cast<uintptr_t>(address)));
	}

	for (int index : selected) {
		if (index >= 0 && static_cast<unsigned int>(index) < breakpoint_list_->GetCount()) {
			breakpoint_list_->SetSelection(index);
		}
	}

	breakpoint_remove_button_->Enable(ListBoxSelectionCount(breakpoint_list_) > 0);
}

void MachineInspectorWindow::PopulateWatchpointList(const MachineSnapshot &snapshot)
{
	watchpoint_list_->Clear();
	for (uint32_t i = 0; i < snapshot.debug_watchpoint_count; i++) {
		const DebugWatchpointInfo &wp = snapshot.debug_watchpoints[i];
		wxString flags;
		if (wp.on_read) {
			flags += "R";
		}
		if (wp.on_write) {
			flags += flags.empty() ? "W" : "/W";
		}
		if (flags.empty()) {
			flags = "N/A";
		}

		const wxString label = wxString::Format("%s | %u bytes | %s%s",
		                                        FormatHex(wp.address),
		                                        wp.size,
		                                        flags,
		                                        wp.log_only ? " | log" : "");
		watchpoint_list_->Append(label, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
	}

	watchpoint_remove_button_->Enable(ListBoxSelectionCount(watchpoint_list_) > 0);
}

void MachineInspectorWindow::UpdateDebuggerUi(const MachineSnapshot &snapshot)
{
	const bool paused = snapshot.debug_paused != 0;
	const bool pausing = snapshot.debug_pause_requested != 0;

	wxString reason;
	switch (snapshot.debug_pause_reason) {
	case DebugPauseReason_User: reason = "manual pause"; break;
	case DebugPauseReason_Breakpoint: reason = "breakpoint"; break;
	case DebugPauseReason_Watchpoint: reason = "watchpoint"; break;
	case DebugPauseReason_Step: reason = "single step"; break;
	case DebugPauseReason_Exception: reason = "exception trap"; break;
	case DebugPauseReason_Swi: reason = "SWI trap"; break;
	case DebugPauseReason_BadMode: reason = "reserved CPU mode written"; break;
	default: reason = "unknown"; break;
	}

	wxString status;
	if (paused) {
		status = wxString::Format("Paused: %s at PC %s, opcode %s",
		                          reason,
		                          FormatHex(snapshot.debug_halt_pc),
		                          FormatHex(snapshot.debug_halt_opcode));
	} else {
		const wxString state = pausing ? wxString::FromUTF8("Pausing\xE2\x80\xA6") : wxString("Running");
		status = wxString::Format("%s, last PC %s",
		                          state,
		                          FormatHex(snapshot.debug_last_pc));
	}
	if (!autostep_stopped_note_.empty()) {
		status += "   " + autostep_stopped_note_;
	}
	debug_status_label_->SetLabel(status);

	if (snapshot.debug_hit_size > 0) {
		const wxString access = snapshot.debug_hit_is_write ? "write" : "read";
		const int width = std::max(2, static_cast<int>(snapshot.debug_hit_size) * 2);
		debug_hit_label_->SetLabel(wxString::Format("Watchpoint %s: %u bytes %s, value %s",
		                                            FormatHex(snapshot.debug_hit_address),
		                                            snapshot.debug_hit_size,
		                                            access,
		                                            FormatHex(snapshot.debug_hit_value, width)));
	} else {
		debug_hit_label_->SetLabel("No watchpoint hit");
	}

	run_button_->Enable(paused);
	pause_button_->Enable(!paused);
	step_button_->Enable(paused);
	step_over_button_->Enable(paused);

	/*
	 * ★ Re-lay-out the row after changing the labels.
	 *
	 * Both labels are in a horizontal sizer, the status one sized to its
	 * content. Setting a longer string does not re-run the sizer, so the label
	 * kept the width it was created with and the two ran into each other:
	 * "Paused: manual pause at PC 0xFNo watchpoint hit", reported in
	 * discussion #223. Asking the parent to lay out again costs nothing at
	 * this rate and puts both labels where they belong.
	 */
	if (debug_status_label_->GetParent() != nullptr) {
		debug_status_label_->GetParent()->Layout();
	}
}

void MachineInspectorWindow::OnTraceConfigChanged(wxCommandEvent &)
{
	ApplyTraceConfig();
}

void MachineInspectorWindow::OnTraceClear(wxCommandEvent &)
{
	trace_view_->Clear();
	trace_dropped_total_ = 0;
	trace_dropped_label_->SetLabel("Dropped: 0");
}

/*
 * Show what the machine is actually doing, the first time this window sees it.
 *
 * The trapping and tracing settings live in the emulator, not in this window,
 * and they outlive it: closing the inspector destroys the frame but leaves the
 * machine trapping and tracing exactly as it was. Building the controls
 * unticked and leaving them there meant a reopened inspector disagreed with its
 * own machine - Halt on exception still halting with the box clear, and Log
 * SWIs still filling the ring while DrainTraceEvents(), which gates on these
 * same boxes, quietly stopped reading it, so the SWIs "stopped" being logged.
 * That is issue #221, and both halves of it are this one omission.
 *
 * Once only. After the first snapshot these controls belong to the user, and a
 * snapshot taken between a click and the emulator thread acting on it would
 * put the box back.
 */
void MachineInspectorWindow::SeedTraceConfig(const MachineSnapshot &snapshot)
{
	if (trace_config_seeded_) {
		return;
	}
	trace_config_seeded_ = true;

	const DebugTraceConfig &cfg = snapshot.debug_trace_config;

	trap_undefined_checkbox_->SetValue(cfg.trap_undefined != 0);
	trap_prefetch_checkbox_->SetValue(cfg.trap_prefetch_abort != 0);
	trap_data_abort_checkbox_->SetValue(cfg.trap_data_abort != 0);
	log_exceptions_checkbox_->SetValue(cfg.log_exceptions != 0);
	swi_trace_checkbox_->SetValue(cfg.swi_trace_enabled != 0);
	swi_halt_checkbox_->SetValue(cfg.swi_trace_halt != 0);
	step_skip_irq_checkbox_->SetValue(cfg.step_skip_irq != 0);
	step_skip_os_checkbox_->SetValue(cfg.step_skip_os != 0);
	step_skip_swi_checkbox_->SetValue(cfg.step_skip_swi != 0);

	/* The full range is "no filter", which is what an empty box means, so it
	   is left showing its hint rather than 0 and FFFFFFFF. */
	if (cfg.swi_filter_min != 0 || cfg.swi_filter_max != 0xffffffffu) {
		swi_filter_min_input_->SetValue(wxString::Format("%X", cfg.swi_filter_min));
		swi_filter_max_input_->SetValue(wxString::Format("%X", cfg.swi_filter_max));
	}
}

bool MachineInspectorWindow::LogTraceControlsAgainstMachine(const char *when)
{
	const MachineSnapshot snapshot = emulator_.TakeSnapshot();
	const DebugTraceConfig &cfg = snapshot.debug_trace_config;

	const struct {
		const char *name;
		int shown;
		int machine;
	} controls[] = {
		{ "undefined",  trap_undefined_checkbox_->GetValue() ? 1 : 0, cfg.trap_undefined != 0 },
		{ "prefetch",   trap_prefetch_checkbox_->GetValue() ? 1 : 0,  cfg.trap_prefetch_abort != 0 },
		{ "data-abort", trap_data_abort_checkbox_->GetValue() ? 1 : 0, cfg.trap_data_abort != 0 },
		{ "log-exc",    log_exceptions_checkbox_->GetValue() ? 1 : 0,  cfg.log_exceptions != 0 },
		{ "swi-log",    swi_trace_checkbox_->GetValue() ? 1 : 0,       cfg.swi_trace_enabled != 0 },
		{ "swi-halt",   swi_halt_checkbox_->GetValue() ? 1 : 0,        cfg.swi_trace_halt != 0 },
	};

	bool agree = true;

	for (const auto &c : controls) {
		if (c.shown != c.machine) {
			agree = false;
		}
		rpclog("TEST_INSPECTOR: %s: %-10s shown=%d machine=%d%s\n",
		       when, c.name, c.shown, c.machine,
		       c.shown != c.machine ? "  DISAGREE" : "");
	}

	rpclog("TEST_INSPECTOR: %s: controls %s the machine\n", when,
	       agree ? "agree with" : "DISAGREE with");
	return agree;
}

/*
 * The floating point panel against the machine's own FPA (discussion #257).
 *
 * The window is refreshed first and then asked what it is displaying, so what
 * is compared is the text on the labels rather than the snapshot that was used
 * to write them. That is the whole point: a register can reach the snapshot
 * intact and still be put on the wrong label, or on none.
 *
 * The registers are read again afterwards, so in principle the machine could
 * move one between the two reads. Nothing does at a Supervisor prompt, and a
 * disagreement is logged with both values rather than asserted, so a genuine
 * race would be visible for what it is.
 */
/*
 * The memory view's byte and word renderings side by side (discussion #223).
 *
 * A word is the four bytes on its line, low byte first, so the two lines have
 * to agree digit for digit once one is reversed in fours. Logged rather than
 * asserted because reading them beside each other is the check: a wrong
 * endianness or an off-by-one line is obvious there and awkward to phrase as
 * a comparison.
 */
void MachineInspectorWindow::LogMemoryWordsAgainstBytes(const char *when)
{
	wxString bytes_line;
	wxString words_line;

	memory_words_checkbox_->SetValue(false);
	RefreshMemoryView(memory_current_address_);
	bytes_line = memory_view_->GetLineText(0);

	memory_words_checkbox_->SetValue(true);
	RefreshMemoryView(memory_current_address_);
	words_line = memory_view_->GetLineText(0);

	memory_words_checkbox_->SetValue(false);
	RefreshMemoryView(memory_current_address_);

	/*
	 * And the double-click that points the view at a register. The event is
	 * sent to the label rather than clicked, so this proves the binding is
	 * live and the address is computed correctly; whether macOS delivers a
	 * real double-click to a static text is the one part only a person can
	 * settle.
	 */
	{
		wxMouseEvent click(wxEVT_LEFT_DCLICK);
		const uint32_t expect = last_snapshot_.regs[13] & ~0xfu;

		reg_value_[13]->ProcessWindowEvent(click);
		rpclog("TEST_INSPECTOR: %s: double-click R13 (%08x) put the memory view "
		       "at %08x, wanted %08x%s\n", when, last_snapshot_.regs[13],
		       memory_current_address_, expect,
		       memory_current_address_ == expect ? "" : "  <- DISAGREES");
	}

	rpclog("TEST_INSPECTOR: %s: bytes %s\n", when,
	       static_cast<const char *>(bytes_line.mb_str()));
	rpclog("TEST_INSPECTOR: %s: words %s\n", when,
	       static_cast<const char *>(words_line.mb_str()));
}

/*
 * Where the disassembly opened, against where the machine stopped.
 *
 * Only interesting when the two could differ, which is a trapped exception:
 * R15 is on the hardware vector by then and halt_pc is the instruction that
 * faulted. Discussion #223.
 */
void MachineInspectorWindow::LogDisassemblyFollowsHalt(const char *when)
{
	RefreshSnapshot();

	/*
	 * And the literal pool annotation, probed where every machine has one:
	 * the vector table at zero is eight "LDR PC, [PC, #n]" and nothing else,
	 * so with PC-rel ticked each line should carry both the address it reaches
	 * and the word there.
	 */
	{
		const bool was_set = disasm_resolve_checkbox_->GetValue();
		const uint32_t was_at = disasm_current_address_;

		disasm_resolve_checkbox_->SetValue(true);
		ApplyDisasmOptions();
		RefreshDisassembly(0);
		rpclog("TEST_INSPECTOR: %s: vector 0 with PC-rel on: %s\n", when,
		       static_cast<const char *>(disasm_view_->GetLineText(0).mb_str()));

		disasm_resolve_checkbox_->SetValue(false);
		ApplyDisasmOptions();
		RefreshDisassembly(0);
		rpclog("TEST_INSPECTOR: %s: and with it off:        %s\n", when,
		       static_cast<const char *>(disasm_view_->GetLineText(0).mb_str()));

		disasm_resolve_checkbox_->SetValue(was_set);
		ApplyDisasmOptions();
		RefreshDisassembly(was_at);
	}

	rpclog("TEST_INSPECTOR: %s: paused=%d reason=%u halt_pc=%08x R15=%08x "
	       "disassembly opens at %08x%s\n",
	       when, last_snapshot_.debug_paused,
	       (unsigned) last_snapshot_.debug_pause_reason,
	       last_snapshot_.debug_halt_pc, last_snapshot_.pc,
	       disasm_current_address_,
	       (last_snapshot_.debug_paused &&
	        last_snapshot_.debug_halt_pc != last_snapshot_.pc)
	           ? "  <- halt and R15 differ, which is the case that matters"
	           : "");
}

bool MachineInspectorWindow::LogFpRegistersAgainstMachine(const char *when)
{
	RefreshSnapshot();

	const MachineSnapshot snapshot = emulator_.TakeSnapshot();
	bool agree = true;

	for (int f = 0; f < 8; f++) {
		const wxString expect = FormatFpRegister(snapshot.fpvalues[f]);
		const wxString shown = fpreg_value_[f]->GetLabel();
		const bool same = (shown == expect);

		if (!same) {
			agree = false;
		}
		rpclog("TEST_INSPECTOR: %s: F%d shown=%s machine=%s [%08x %08x %08x]%s\n",
		       when, f, static_cast<const char *>(shown.mb_str()),
		       static_cast<const char *>(expect.mb_str()),
		       snapshot.fpregs[f][0], snapshot.fpregs[f][1], snapshot.fpregs[f][2],
		       same ? "" : "  <- DISAGREES");
	}

	/*
	 * FPSR matters beyond being one more number. Its top byte is the system ID
	 * the FPEmulator support code reads to decide whether it is driving a chip,
	 * so a machine reporting anything but 0x81 here is one where these
	 * registers are not the ones RISC OS is using.
	 */
	const bool fpsr_shown = fpsr_label_->GetLabel().Contains(
	    wxString::Format("%08X", snapshot.fpsr));
	const bool fpcr_shown = fpcr_label_->GetLabel().Contains(
	    wxString::Format("%08X", snapshot.fpcr));

	if (!fpsr_shown || !fpcr_shown) {
		agree = false;
	}
	rpclog("TEST_INSPECTOR: %s: FPSR %08x shown=%d (system ID %02x), FPCR %08x shown=%d\n",
	       when, snapshot.fpsr, fpsr_shown ? 1 : 0, snapshot.fpsr >> 24,
	       snapshot.fpcr, fpcr_shown ? 1 : 0);

	return agree;
}

bool MachineInspectorWindow::LogMemoryViewAgainstMachine(const char *when)
{
	const MachineSnapshot snapshot = emulator_.TakeSnapshot();
	bool agree = true;

	/* Somewhere the MMU actually moves, so an untranslated read is visibly
	   different rather than accidentally right. Virtual 0 is RISC OS's vector
	   table and is mapped on any booted machine. */
	const uint32_t probe[] = { 0x00000000u, 0x00008000u, snapshot.regs[13] & ~0xfu };

	for (uint32_t addr : probe) {
		const MemoryRead virt = emulator_read_memory(addr, 4, false);
		const MemoryRead phys = emulator_read_memory(addr, 4, true);
		uint32_t expect = 0;
		const int expect_ok = mem_debug_read(addr, 4, &expect);
		uint32_t got = 0;

		for (int i = 0; i < 4; i++) {
			got |= static_cast<uint32_t>(virt.data[i]) << (i * 8);
		}

		/* The view's virtual read must be the same thing the debugger's own
		   translated read gives. If this disagrees the view is reading some
		   other address space, which is the bug. */
		const bool matches = expect_ok
		    ? (virt.mapped[0] != 0 && got == expect)
		    : (virt.mapped[0] == 0);

		if (!matches) {
			agree = false;
		}
		rpclog("TEST_INSPECTOR: %s: &%08X virtual=%08X(mapped=%d) "
		       "physical=%02X%02X%02X%02X debugger=%08X(ok=%d)%s\n",
		       when, addr, got, virt.mapped[0],
		       phys.data[3], phys.data[2], phys.data[1], phys.data[0],
		       expect, expect_ok, matches ? "" : "  DISAGREE");

		/* And the two spaces must not be silently the same number, or the
		   check above proves nothing on this machine. */
		if (virt.data != phys.data) {
			rpclog("TEST_INSPECTOR: %s: &%08X translation is visible "
			       "(virtual and physical differ)\n", when, addr);
		}
	}

	/* The zero sentinel: a deliberate 0 must survive the next snapshot rather
	   than being replaced by R13. */
	RefreshMemoryView(0);
	ApplySnapshot(emulator_.TakeSnapshot());
	if (memory_current_address_ != 0) {
		agree = false;
		rpclog("TEST_INSPECTOR: %s: address 0 was replaced by &%08X  DISAGREE\n",
		       when, memory_current_address_);
	} else {
		rpclog("TEST_INSPECTOR: %s: address 0 stayed 0 across a snapshot\n", when);
	}

	rpclog("TEST_INSPECTOR: %s: memory view %s the machine\n", when,
	       agree ? "agrees with" : "DISAGREES with");
	return agree;
}

void MachineInspectorWindow::ApplyTraceConfig()
{
	DebugTraceConfig cfg{};
	cfg.trap_undefined = trap_undefined_checkbox_->GetValue() ? 1 : 0;
	cfg.trap_prefetch_abort = trap_prefetch_checkbox_->GetValue() ? 1 : 0;
	cfg.trap_data_abort = trap_data_abort_checkbox_->GetValue() ? 1 : 0;
	cfg.log_exceptions = log_exceptions_checkbox_->GetValue() ? 1 : 0;
	cfg.swi_trace_enabled = swi_trace_checkbox_->GetValue() ? 1 : 0;
	cfg.swi_trace_halt = swi_halt_checkbox_->GetValue() ? 1 : 0;
	cfg.step_skip_irq = step_skip_irq_checkbox_->GetValue() ? 1 : 0;
	cfg.step_skip_os = step_skip_os_checkbox_->GetValue() ? 1 : 0;
	cfg.step_skip_swi = step_skip_swi_checkbox_->GetValue() ? 1 : 0;

	bool ok = false;
	const uint32_t min = ParseAddress(swi_filter_min_input_->GetValue(), &ok);
	cfg.swi_filter_min = ok ? min : 0;
	ok = false;
	const uint32_t max = ParseAddress(swi_filter_max_input_->GetValue(), &ok);
	cfg.swi_filter_max = ok ? max : 0xffffffffu;

	emulator_.SetDebugTraceConfig(cfg);
}

void MachineInspectorWindow::DrainTraceEvents()
{
	/* Only poll the ring when something can be feeding it. */
	bool active = trap_undefined_checkbox_->GetValue() ||
	              trap_prefetch_checkbox_->GetValue() ||
	              trap_data_abort_checkbox_->GetValue() ||
	              log_exceptions_checkbox_->GetValue() ||
	              swi_trace_checkbox_->GetValue();
	if (!active) {
		for (uint32_t i = 0; i < last_snapshot_.debug_watchpoint_count; i++) {
			if (last_snapshot_.debug_watchpoints[i].log_only) {
				active = true;
				break;
			}
		}
	}
	/* A halted machine is drained whatever the boxes say. Nothing is running
	   to flood the ring, and the events that put it here - a reserved CPU
	   mode, which nobody switches on because it is never wanted - would
	   otherwise sit unread while the Trace tab showed an empty box. */
	if (!active && last_snapshot_.debug_paused != 0) {
		active = true;
	}
	if (!active) {
		return;
	}

	uint32_t dropped = 0;
	const std::vector<DebugTraceEvent> events = emulator_.DrainTraceEvents(2048, &dropped);

	if (dropped > 0) {
		trace_dropped_total_ += dropped;
		trace_dropped_label_->SetLabel(wxString::Format("Dropped: %u", trace_dropped_total_));
	}

	if (events.empty()) {
		return;
	}

	/* Cap the control's size so a long trace session does not grow unbounded. */
	if (trace_view_->GetLastPosition() > 256 * 1024) {
		trace_view_->Remove(0, trace_view_->GetLastPosition() / 2);
	}

	wxString chunk;
	char disasm_buf[128];
	for (const DebugTraceEvent &ev : events) {
		wxString line;
		switch (ev.type) {
		case TraceEvent_Swi: {
			arm_disasm(ev.opcode, ev.pc, disasm_buf, sizeof(disasm_buf));
			line = wxString::Format("%08u  PC=%s  SWI &%06X  %s  R0=%s",
			                        ev.seq, FormatHex(ev.pc), ev.arg0,
			                        wxString::FromUTF8(disasm_buf), FormatHex(ev.arg1));
			break;
		}
		case TraceEvent_Exception: {
			const char *kind = "exception";
			switch (ev.arg0) {
			case TraceException_Undefined: kind = "undefined instruction"; break;
			case TraceException_PrefetchAbort: kind = "prefetch abort"; break;
			case TraceException_DataAbort: kind = "data abort"; break;
			case TraceException_BadMode: kind = "reserved CPU mode"; break;
			default: break;
			}
			line = wxString::Format("%08u  PC=%s  EXCEPTION  %s",
			                        ev.seq, FormatHex(ev.pc), kind);
			/* A reserved mode carries the mode written where a fault
			   address would be; there is no fault status. */
			if (ev.arg0 == TraceException_BadMode) {
				line += wxString::Format("  mode=%02X", ev.arg1 & 0xffu);
			}
			/* Only a data abort carries a fault address and status; the
			   others leave them zero on purpose (see DebugTraceEvent). */
			if (ev.arg0 == TraceException_DataAbort) {
				line += wxString::Format("  addr=%s  status=%02X %s",
				                         FormatHex(ev.arg1),
				                         ev.arg2 & 0xff,
				                         wxString::FromUTF8(cp15_fault_status_name(ev.arg2)));
			}
			break;
		}
		case TraceEvent_Watchpoint: {
			const bool is_write = (ev.arg2 & 1u) != 0;
			const uint32_t size = ev.arg2 >> 1;
			line = wxString::Format("%08u  PC=%s  WATCH  %s %s  addr=%s  value=%s",
			                        ev.seq, FormatHex(ev.pc),
			                        is_write ? "write" : "read",
			                        wxString::Format("%ub", size),
			                        FormatHex(ev.arg0), FormatHex(ev.arg1));
			break;
		}
		default:
			line = wxString::Format("%08u  PC=%s  (unknown event)", ev.seq, FormatHex(ev.pc));
			break;
		}
		chunk += line + "\n";
	}

	trace_view_->AppendText(chunk);
	if (trace_autoscroll_checkbox_->GetValue()) {
		trace_view_->ShowPosition(trace_view_->GetLastPosition());
	}
}

/*
 * Colour the line the machine is about to execute.
 *
 * The disassembler already marks it, by writing "<" where the other lines have
 * ":", and that is too quiet to find when you are jumping between the
 * disassembly, the memory view and the tabs below - which is what JonAbbott2
 * said on discussion #223. The marker stays, because the debug socket's `dis`
 * hands out the same text and has nowhere to put a colour.
 *
 * The system's own selection colours, so this reads correctly in both light
 * and dark and against whatever theme the user has, rather than a hardcoded
 * yellow that disappears in one of them.
 */
void MachineInspectorWindow::HighlightCurrentInstruction()
{
	const wxString text = disasm_view_->GetValue();
	long line_start = 0;

	/* Clear whatever the last refresh coloured. */
	disasm_view_->SetStyle(0, static_cast<long>(text.length()),
	    wxTextAttr(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT),
	               wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));

	for (size_t i = 0; i <= text.length(); i++) {
		if (i == text.length() || text[i] == '\n') {
			const long line_end = static_cast<long>(i);

			/* "FC00A66C< E92D580E  stmdb ..." - the marker sits where the
			   other lines have a colon, right after the address. */
			if (line_end - line_start > 8 && text[line_start + 8] == '<') {
				disasm_view_->SetStyle(line_start, line_end,
				    wxTextAttr(
				        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT),
				        wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)));
				return;
			}
			line_start = line_end + 1;
		}
	}
}

void MachineInspectorWindow::RefreshDisassembly(uint32_t address)
{
	const wxString text =
	    wxString::FromUTF8(emulator_disassemble_at(address, kDisasmLines));

	disasm_current_address_ = address;

	/* Only when the text has moved. The marker is part of the text, so the
	   current line changing changes it - and re-styling an unchanged view on
	   every refresh would fight the user's own selection twice a second. */
	if (disasm_view_->GetValue() != text) {
		disasm_view_->ChangeValue(text);
		HighlightCurrentInstruction();
	}
}

/*
 * Point the memory view at what a register holds.
 *
 * Rounded down to the start of its line, so the address asked for is on the
 * first row with its neighbours either side, rather than at a ragged offset
 * that makes a stack hard to read.
 */
void MachineInspectorWindow::ShowRegisterInMemoryView(int reg)
{
	if (reg < 0 || reg > 15) {
		return;
	}

	const uint32_t value = (reg == 15) ? last_snapshot_.pc
	                                   : last_snapshot_.regs[reg];

	RefreshMemoryView(value & ~0xfu);
}

void MachineInspectorWindow::RefreshMemoryView(uint32_t address)
{
	memory_current_address_ = address;
	memory_address_chosen_ = true;
	memory_address_input_->SetValue(wxString::Format("%08X", address));

	int num_bytes = memory_bytes_spin_->GetValue();
	if (num_bytes < 16) {
		num_bytes = 16;
	}

	const bool physical = memory_physical_checkbox_ != nullptr &&
	                      memory_physical_checkbox_->GetValue();
	const MemoryRead read = emulator_read_memory(address,
	    static_cast<uint32_t>(num_bytes), physical);
	const std::vector<uint8_t> &data = read.data;
	if (data.empty()) {
		memory_view_->SetValue("Failed to read memory");
		return;
	}

	wxString text;
	const bool words = memory_words_checkbox_ != nullptr &&
	                   memory_words_checkbox_->GetValue();
	const int bytes_per_line = 16;
	for (size_t offset = 0; offset < data.size(); offset += bytes_per_line) {
		wxString hex_part;
		wxString ascii_part;

		if (words) {
			/* Four bytes at a time, low byte first, and "--------" when any
			   byte of the word is unmapped: a word is only a word if all of
			   it is there. */
			for (int i = 0; i < bytes_per_line; i += 4) {
				const size_t at = offset + static_cast<size_t>(i);
				uint32_t value = 0;
				bool mapped = true;

				for (int b = 0; b < 4; b++) {
					if (at + static_cast<size_t>(b) >= data.size()) {
						mapped = false;
						break;
					}
					if (at + static_cast<size_t>(b) < read.mapped.size() &&
					    read.mapped[at + static_cast<size_t>(b)] == 0) {
						mapped = false;
					}
					value |= static_cast<uint32_t>(data[at + static_cast<size_t>(b)])
					    << (b * 8);
				}
				hex_part += mapped ? wxString::Format("%08X ", value)
				                   : wxString("-------- ");
				for (int b = 0; b < 4; b++) {
					const size_t byte_at = at + static_cast<size_t>(b);

					if (!mapped || byte_at >= data.size()) {
						ascii_part += " ";
					} else {
						const uint8_t byte = data[byte_at];

						ascii_part += (byte >= 32 && byte < 127)
						    ? wxString(static_cast<char>(byte)) : ".";
					}
				}
			}
			text += wxString::Format("%08X: %s|%s|\n",
			                         address + static_cast<uint32_t>(offset),
			                         hex_part, ascii_part);
			continue;
		}

		for (int i = 0; i < bytes_per_line; i++) {
			if (offset + static_cast<size_t>(i) < data.size()) {
				const size_t at = offset + static_cast<size_t>(i);
				const uint8_t byte = data[at];

				/* An unmapped byte is not a zero. Printing it as 00 is what
				   made a wrong address look like a page of zeros rather than
				   like a question the machine cannot answer. */
				if (at < read.mapped.size() && read.mapped[at] == 0) {
					hex_part += "-- ";
					ascii_part += " ";
				} else {
					hex_part += wxString::Format("%02X ", byte);
					ascii_part += (byte >= 32 && byte < 127) ? wxString(static_cast<char>(byte)) : ".";
				}
			} else {
				hex_part += "   ";
				ascii_part += " ";
			}
			if (i == 7) {
				hex_part += " ";
			}
		}
		text += wxString::Format("%08X: %s |%s|\n",
		                         address + static_cast<uint32_t>(offset),
		                         hex_part,
		                         ascii_part);
	}

	memory_view_->SetValue(text);
}

void MachineInspectorWindow::OnDisasmGo(wxCommandEvent &)
{
	bool ok = false;
	const uint32_t address = ParseAddress(disasm_address_input_->GetValue(), &ok);
	if (!ok) {
		wxMessageBox("Please enter a valid hexadecimal address.", "Invalid address",
		             wxOK | wxICON_WARNING, this);
		return;
	}

	disasm_follow_pc_checkbox_->SetValue(false);
	RefreshDisassembly(address);
}

void MachineInspectorWindow::OnDisasmFollowPc(wxCommandEvent &event)
{
	if (event.IsChecked()) {
		RefreshSnapshot();
	}
}

void MachineInspectorWindow::OnMemoryGo(wxCommandEvent &)
{
	bool ok = false;
	const uint32_t address = ParseAddress(memory_address_input_->GetValue(), &ok);
	if (!ok) {
		wxMessageBox("Please enter a valid hexadecimal address.", "Invalid address",
		             wxOK | wxICON_WARNING, this);
		return;
	}

	RefreshMemoryView(address);
}

void MachineInspectorWindow::OnMemoryRefresh(wxCommandEvent &)
{
	if (memory_current_address_ != 0 || !memory_address_input_->GetValue().empty()) {
		bool ok = false;
		uint32_t address = memory_current_address_;
		if (!memory_address_input_->GetValue().empty()) {
			address = ParseAddress(memory_address_input_->GetValue(), &ok);
			if (!ok) {
				address = memory_current_address_;
			}
		}
		RefreshMemoryView(address);
	}
}

void MachineInspectorWindow::OnRun(wxCommandEvent &)
{
	emulator_.DebuggerResume();
	RefreshSnapshot();
}

void MachineInspectorWindow::OnPause(wxCommandEvent &)
{
	emulator_.DebuggerPause();
	RefreshSnapshot();
}

void MachineInspectorWindow::OnStep(wxCommandEvent &)
{
	emulator_.DebuggerStep();
	RefreshSnapshot();
}

void MachineInspectorWindow::OnStepOver(wxCommandEvent &)
{
	emulator_.DebuggerStepOver();
	RefreshSnapshot();
}

/*
 * A rendering option moved.
 *
 * The options live in the disassembler rather than in this window, because the
 * debug socket's "dis" renders through the same code and one machine should not
 * disassemble two different ways depending on who asked. The view is redrawn at
 * once so the effect of the click is visible without waiting for a refresh.
 */
void MachineInspectorWindow::OnDisasmStyleChanged(wxCommandEvent &)
{
	ApplyDisasmOptions();
}

void MachineInspectorWindow::ApplyDisasmOptions()
{
	ArmDisasmOptions opts;

	memset(&opts, 0, sizeof(opts));
	opts.hex_immediates = disasm_hex_checkbox_->GetValue() ? 1 : 0;
	opts.apcs_registers = disasm_apcs_checkbox_->GetValue() ? 1 : 0;
	opts.collapse_reglists = disasm_ranges_checkbox_->GetValue() ? 1 : 0;
	opts.resolve_pc_relative = disasm_resolve_checkbox_->GetValue() ? 1 : 0;
	opts.lowercase = disasm_lower_checkbox_->GetValue() ? 1 : 0;
	arm_disasm_set_options(&opts);

	RefreshDisassembly(disasm_current_address_);
}

/*
 * Keys for the things done most often while stepping through code.
 *
 * Asked for in discussion #223: reaching for the mouse between every step is
 * what makes stepping tedious. Enter toggles rather than having separate keys
 * for run and pause, for the same reason.
 */
void MachineInspectorWindow::OnDebugKey(wxKeyEvent &event)
{
	const bool paused = last_snapshot_.debug_paused != 0;

	/*
	 * ★ Not while something is being typed into.
	 *
	 * CHAR_HOOK sees the key before the focused control does, which is what
	 * makes it work on the read-only views - but it would also swallow the
	 * space bar and Return from the address and breakpoint boxes, so an
	 * address could not be typed or submitted. The focused window decides:
	 * anything that takes text keeps its keys.
	 */
	const wxWindow *focus = FindFocus();

	if (dynamic_cast<const wxTextEntry *>(focus) != nullptr) {
		event.Skip();
		return;
	}

	switch (event.GetKeyCode()) {
	case WXK_RETURN:
	case WXK_NUMPAD_ENTER:
		if (paused) {
			emulator_.DebuggerResume();
		} else {
			emulator_.DebuggerPause();
		}
		RefreshSnapshot();
		return;

	case WXK_SPACE:
		if (paused) {
			/*
			 * A step already in flight, and the key repeating.
			 *
			 * A step is asynchronous: the emulator thread has to execute the
			 * instruction. Holding the key asked for another on every repeat,
			 * which queues steps faster than the machine retires them and
			 * keeps the event loop busy enough that the views never repaint -
			 * "holding SPACE to constantly step doesn't update the disassembly
			 * window", on discussion #223. Dropping the repeat is the same
			 * rule AutoStepTick() already follows.
			 */
			if (last_snapshot_.debug_step_active != 0) {
				return;
			}
			if (event.ShiftDown()) {
				emulator_.DebuggerStepOver();
			} else {
				emulator_.DebuggerStep();
			}
			RefreshSnapshot();
		}
		return;

	default:
		event.Skip();
		return;
	}
}

/*
 * Auto-step. Driven by a timer rather than a loop, so the window stays alive and
 * the machine can be stopped mid-run by unticking the box.
 */
void MachineInspectorWindow::OnAutoStep(wxCommandEvent &)
{
	if (autostep_checkbox_->GetValue()) {
		const int rate = autostep_rate_spin_->GetValue();

		autostep_stopped_note_.clear();

		/*
		 * Ticked while the machine is running: stop it, then step.
		 *
		 * This used to do nothing visible. The first tick saw a machine that
		 * was not paused, concluded somebody else had resumed it, and unticked
		 * the box - so ticking Auto on a running machine turned itself off and
		 * said nothing about why. Reported by JonAbbott2 on discussion #223,
		 * who expected exactly what the name says: enable it and the machine
		 * advances an instruction at a time.
		 */
		if (last_snapshot_.debug_paused == 0 &&
		    last_snapshot_.debug_pause_requested == 0) {
			emulator_.DebuggerPause();
		}
		autostep_timer_.Start(1000 / (rate > 0 ? rate : 1));
	} else {
		autostep_timer_.Stop();
		autostep_stopped_note_.clear();
	}
}

void MachineInspectorWindow::OnAutoStepTimer(wxTimerEvent &)
{
	AutoStepTick();
}

void MachineInspectorWindow::AutoStepTick()
{
	RefreshSnapshot();

	/*
	 * A step already in flight. Asking for another now would queue steps faster
	 * than the machine retires them, so this tick is simply dropped and the
	 * rate becomes "as fast as the machine manages, up to the rate asked for".
	 *
	 * This case has to be tested BEFORE the resumed-elsewhere case below,
	 * because a machine mid-step reads as not paused. Getting that order wrong
	 * is not a small bug: the first tick steps, the second sees "not paused",
	 * concludes somebody hit Run and switches itself off - so auto-step took
	 * exactly one step and then stopped. Measured that way before this was
	 * split in two. See test_step_is_not_immediately_paused() in
	 * tests/test_debugger_gate.c, which is about the same transient.
	 */
	if (last_snapshot_.debug_step_active != 0) {
		return;
	}

	/*
	 * Only while stopped. A machine that has been resumed from elsewhere - the
	 * Run button, the debug socket, a breakpoint being cleared - should not be
	 * quietly single-stepped from under whoever did it, so the box turns itself
	 * off rather than fighting.
	 */
	if (last_snapshot_.debug_paused == 0) {
		/* A pause of our own asking, still on its way: pause is deferred to
		   the next instruction, so a tick or two lands here first. */
		if (last_snapshot_.debug_pause_requested != 0) {
			return;
		}
		autostep_timer_.Stop();
		autostep_checkbox_->SetValue(false);
		/* Said out loud. Silently unticking itself is what made this feature
		   look broken rather than declined. */
		autostep_stopped_note_ = "Auto step stopped: the machine was resumed";
		return;
	}

	autostep_stopped_note_.clear();
	emulator_.DebuggerStep();
}

/*
 * A debugging session written to a file: the breakpoints, the watchpoints, the
 * trapping and tracing settings, and how the disassembly is rendered.
 *
 * Plain text, one setting per line, because it is the sort of file somebody
 * will want to edit and put in a repository beside the code being debugged.
 * Unknown keys are ignored, so a file from a later version loads what it can.
 */
void MachineInspectorWindow::OnSaveSettings(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Save debugging session", wxEmptyString,
	                 "session.rpcdbg",
	                 "RPCEmu debugging session (*.rpcdbg)|*.rpcdbg|All files (*)|*",
	                 wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (!SaveSessionTo(dlg.GetPath())) {
		wxMessageBox("That file could not be written.", "Save failed",
		    wxOK | wxICON_ERROR, this);
	}
}

bool MachineInspectorWindow::SaveSessionTo(const wxString &path)
{
	wxTextFile file(path);

	if (file.Exists()) {
		if (!file.Open()) {
			return false;
		}
		file.Clear();
	} else if (!file.Create()) {
		return false;
	}

	file.AddLine("# RPCEmu debugging session");
	file.AddLine(wxString::Format("trap_undefined %d",
	    trap_undefined_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("trap_prefetch %d",
	    trap_prefetch_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("trap_data_abort %d",
	    trap_data_abort_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("log_exceptions %d",
	    log_exceptions_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("swi_trace %d",
	    swi_trace_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("swi_halt %d",
	    swi_halt_checkbox_->GetValue() ? 1 : 0));
	file.AddLine("swi_filter_min " + swi_filter_min_input_->GetValue());
	file.AddLine("swi_filter_max " + swi_filter_max_input_->GetValue());
	file.AddLine(wxString::Format("step_skip_irq %d",
	    step_skip_irq_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("step_skip_os %d",
	    step_skip_os_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("step_skip_swi %d",
	    step_skip_swi_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("disasm_lower %d",
	    disasm_lower_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("disasm_hex %d",
	    disasm_hex_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("disasm_apcs %d",
	    disasm_apcs_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("disasm_ranges %d",
	    disasm_ranges_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("disasm_pcrel %d",
	    disasm_resolve_checkbox_->GetValue() ? 1 : 0));
	file.AddLine(wxString::Format("autostep_rate %d",
	    autostep_rate_spin_->GetValue()));
	if (!swi_names_path_.IsEmpty()) {
		file.AddLine("swi_names " + swi_names_path_);
	}

	/* The breakpoints and watchpoints as the machine has them, not as this
	   window last drew them. */
	for (uint32_t i = 0; i < last_snapshot_.debug_breakpoint_count; i++) {
		const DebugBreakpointInfo &bp = last_snapshot_.debug_breakpoints[i];

		file.AddLine(wxString::Format("breakpoint %08X", bp.address));
	}
	for (uint32_t i = 0; i < last_snapshot_.debug_watchpoint_count; i++) {
		const DebugWatchpointInfo &wp = last_snapshot_.debug_watchpoints[i];

		file.AddLine(wxString::Format("watchpoint %08X %u %d %d %d",
		    wp.address, wp.size, wp.on_read ? 1 : 0, wp.on_write ? 1 : 0,
		    wp.log_only ? 1 : 0));
	}

	return file.Write();
}

void MachineInspectorWindow::OnLoadSettings(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Load debugging session", wxEmptyString, wxEmptyString,
	                 "RPCEmu debugging session (*.rpcdbg)|*.rpcdbg|All files (*)|*",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	if (!LoadSessionFrom(dlg.GetPath())) {
		wxMessageBox("That file could not be read.", "Load failed",
		    wxOK | wxICON_ERROR, this);
	}
}

bool MachineInspectorWindow::LoadSessionFrom(const wxString &path)
{
	wxTextFile file;

	if (!file.Open(path)) {
		return false;
	}

	/* Replaced rather than merged: a session file describes a whole setup, and
	   leaving yesterday's breakpoints in place beside it would be a third thing
	   that is neither. */
	emulator_.DebuggerClearBreakpoints();
	emulator_.DebuggerClearWatchpoints();

	for (wxString line = file.GetFirstLine(); !file.Eof();
	     line = file.GetNextLine()) {
		wxString key;
		long value = 0;

		line.Trim(true).Trim(false);
		if (line.IsEmpty() || line.StartsWith("#")) {
			continue;
		}
		key = line.BeforeFirst(' ');
		const wxString rest = line.AfterFirst(' ').Trim(false);

		auto flag = [&](wxCheckBox *box) {
			if (rest.ToLong(&value)) {
				box->SetValue(value != 0);
			}
		};

		if (key == "trap_undefined")   flag(trap_undefined_checkbox_);
		else if (key == "trap_prefetch")    flag(trap_prefetch_checkbox_);
		else if (key == "trap_data_abort")  flag(trap_data_abort_checkbox_);
		else if (key == "log_exceptions")   flag(log_exceptions_checkbox_);
		else if (key == "swi_trace")        flag(swi_trace_checkbox_);
		else if (key == "swi_halt")         flag(swi_halt_checkbox_);
		else if (key == "step_skip_irq")    flag(step_skip_irq_checkbox_);
		else if (key == "step_skip_os")     flag(step_skip_os_checkbox_);
		else if (key == "step_skip_swi")    flag(step_skip_swi_checkbox_);
		else if (key == "disasm_lower")     flag(disasm_lower_checkbox_);
		else if (key == "disasm_hex")       flag(disasm_hex_checkbox_);
		else if (key == "disasm_apcs")      flag(disasm_apcs_checkbox_);
		else if (key == "disasm_ranges")    flag(disasm_ranges_checkbox_);
		else if (key == "disasm_pcrel")     flag(disasm_resolve_checkbox_);
		else if (key == "swi_filter_min")   swi_filter_min_input_->SetValue(rest);
		else if (key == "swi_filter_max")   swi_filter_max_input_->SetValue(rest);
		else if (key == "autostep_rate") {
			if (rest.ToLong(&value) && value > 0) {
				autostep_rate_spin_->SetValue((int) value);
			}
		} else if (key == "swi_names") {
			unsigned count = 0;

			/* Not an error worth a dialog: the file may simply have moved
			   since the session was saved, and everything else in it is
			   still worth having. */
			if (arm_disasm_load_swi_names(rest.utf8_str(), &count) == NULL) {
				swi_names_path_ = rest;
			} else {
				rpclog("Machine Inspector: session names a SWI list that "
				    "cannot be read: %s\n", (const char *) rest.utf8_str());
			}
		} else if (key == "breakpoint") {
			bool ok = false;
			const uint32_t address = ParseAddress(rest, &ok);

			if (ok) {
				emulator_.DebuggerAddBreakpoint(address);
			}
		} else if (key == "watchpoint") {
			unsigned long addr = 0, size = 0;
			int rd = 0, wr = 0, log = 0;

			if (sscanf(rest.utf8_str().data(), "%lx %lu %d %d %d",
			           &addr, &size, &rd, &wr, &log) == 5) {
				emulator_.DebuggerAddWatchpoint((uint32_t) addr,
				    (uint32_t) size, rd != 0, wr != 0, log != 0);
			}
		}
		/* Anything else is from a version that knows more than this one. */
	}

	/* Push both sets of settings out, then read the machine back so the window
	   shows what actually took. */
	ApplyTraceConfig();
	ApplyDisasmOptions();
	RefreshSnapshot();

	return true;
}

/*
 * SWI names for the module being debugged.
 *
 * Asked for in discussion #223: the built-in table covers the OS, so a call
 * into somebody's own module disassembles as "SWI &62C40" and has to be looked
 * up by hand every time. A CSV is the format because that is what the tools
 * that already know these numbers - a module's own headers, an assembler
 * listing - can be made to produce in one line of anything.
 */
void MachineInspectorWindow::OnLoadSwiNames(wxCommandEvent &)
{
	wxFileDialog dlg(this, "Load SWI names", wxEmptyString, wxEmptyString,
	                 "SWI name list (*.csv;*.txt)|*.csv;*.txt|All files (*)|*",
	                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);

	if (dlg.ShowModal() != wxID_OK) {
		return;
	}

	unsigned count = 0;
	const char *error = arm_disasm_load_swi_names(dlg.GetPath().utf8_str(),
	                                              &count);

	if (error == NULL) {
		swi_names_path_ = dlg.GetPath();
	} else {
		wxMessageBox(wxString::Format("%s\n\nEach line is a SWI number and a "
		    "name, separated by a comma:\n\n  &42C40,MyModule_Doit\n"
		    "  &42C41,MyModule_Undo\n\nNumbers may be written &hex, 0xhex or "
		    "decimal. Give the chunk base as the module declares it - the X "
		    "form is matched too, and rendered with its X. Blank lines and "
		    "lines starting with # are ignored.", error),
		    "SWI names not loaded", wxOK | wxICON_ERROR, this);
		return;
	}

	rpclog("Machine Inspector: %u SWI names loaded from %s\n", count,
	    (const char *) dlg.GetPath().utf8_str());

	/* Redraw at once: the point of loading them is to read the code on
	   screen. */
	RefreshDisassembly(disasm_current_address_);
}

/*
 * Save a session, change everything, load it back, and say whether it came
 * back. Reports each setting rather than a single pass/fail, so a reader of the
 * log can see WHICH one was lost.
 */
bool MachineInspectorWindow::TestSessionRoundTrip(const wxString &path)
{
	/* Something distinctive, and not the default of anything. */
	trap_data_abort_checkbox_->SetValue(true);
	swi_trace_checkbox_->SetValue(true);
	step_skip_irq_checkbox_->SetValue(true);
	step_skip_os_checkbox_->SetValue(true);
	step_skip_swi_checkbox_->SetValue(true);
	disasm_lower_checkbox_->SetValue(true);
	disasm_apcs_checkbox_->SetValue(true);
	swi_filter_min_input_->SetValue("40000");
	autostep_rate_spin_->SetValue(17);
	emulator_.DebuggerClearBreakpoints();
	emulator_.DebuggerAddBreakpoint(0x00008abc);
	RefreshSnapshot();

	if (!SaveSessionTo(path)) {
		rpclog("TEST_INSPECTOR: session could not be saved to %s\n",
		    (const char *) path.utf8_str());
		return false;
	}

	/* Wipe every one of them, so anything that survives is the file's doing. */
	trap_data_abort_checkbox_->SetValue(false);
	swi_trace_checkbox_->SetValue(false);
	step_skip_irq_checkbox_->SetValue(false);
	step_skip_os_checkbox_->SetValue(false);
	step_skip_swi_checkbox_->SetValue(false);
	disasm_lower_checkbox_->SetValue(false);
	disasm_apcs_checkbox_->SetValue(false);
	swi_filter_min_input_->SetValue("0");
	autostep_rate_spin_->SetValue(1);
	ApplyTraceConfig();

	if (!LoadSessionFrom(path)) {
		rpclog("TEST_INSPECTOR: session could not be read back\n");
		return false;
	}

	struct {
		const char *what;
		int got;
	} checks[] = {
		{ "trap_data_abort", trap_data_abort_checkbox_->GetValue() ? 1 : 0 },
		{ "swi_trace",       swi_trace_checkbox_->GetValue() ? 1 : 0 },
		{ "step_skip_irq",   step_skip_irq_checkbox_->GetValue() ? 1 : 0 },
		{ "step_skip_os",    step_skip_os_checkbox_->GetValue() ? 1 : 0 },
		{ "step_skip_swi",   step_skip_swi_checkbox_->GetValue() ? 1 : 0 },
		{ "disasm_lower",    disasm_lower_checkbox_->GetValue() ? 1 : 0 },
		{ "disasm_apcs",     disasm_apcs_checkbox_->GetValue() ? 1 : 0 },
		{ "swi_filter_min",  swi_filter_min_input_->GetValue() == "40000" },
		{ "autostep_rate",   autostep_rate_spin_->GetValue() == 17 },
	};
	int ok = 1;

	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		rpclog("TEST_INSPECTOR: session %-16s %s\n", checks[i].what,
		    checks[i].got ? "restored" : "LOST");
		if (!checks[i].got) {
			ok = 0;
		}
	}

	/* The breakpoint is read back from the machine, not from the window, so
	   this also says the load reached the emulator and not just the controls. */
	RefreshSnapshot();
	{
		const int found = (last_snapshot_.debug_breakpoint_count == 1 &&
		    last_snapshot_.debug_breakpoints[0].address == 0x00008abc);

		rpclog("TEST_INSPECTOR: session breakpoint      %s\n",
		    found ? "restored" : "LOST");
		if (!found) {
			ok = 0;
		}
	}

	/* And the trace config as the MACHINE holds it, which is the whole point of
	   loading a session rather than just filling in a form. */
	if (!LogTraceControlsAgainstMachine("after loading a session")) {
		ok = 0;
	}

	emulator_.DebuggerClearBreakpoints();

	return ok != 0;
}

/*
 * Tick auto-step by hand and count what the machine actually retired. The
 * complaint in discussion #223 was about how many instructions a second the
 * window could manage, so the measurement is instructions, not ticks.
 */
void MachineInspectorWindow::TestAutoStep(int rate, int ticks)
{
	uint32_t first;
	int moved = 0;
	wxLongLong started;

	emulator_.DebuggerPause();
	RefreshSnapshot();
	first = last_snapshot_.regs[15];

	autostep_rate_spin_->SetValue(rate);
	autostep_checkbox_->SetValue(true);
	started = wxGetLocalTimeMillis();

	for (int i = 0; i < ticks; i++) {
		const uint32_t before = last_snapshot_.regs[15];

		AutoStepTick();

		/* A step is asynchronous: the emulator thread has to execute the
		   instruction. Sample until it lands rather than reading the PC back
		   immediately, which is the mistake this whole area is about. */
		for (int waited = 0; waited < 50; waited++) {
			wxMilliSleep(2);
			RefreshSnapshot();
			if (last_snapshot_.debug_step_active == 0 &&
			    last_snapshot_.debug_paused != 0) {
				break;
			}
		}
		if (last_snapshot_.regs[15] != before) {
			moved++;
		}
	}

	autostep_checkbox_->SetValue(false);
	autostep_timer_.Stop();

	{
		/*
		 * How long a step takes from the window, which is the question asked in
		 * discussion #223 - "about 1/2s for each step". Reported per step, not
		 * as a total, so it can be compared against that directly. The rate
		 * asked for is the CEILING; this is what the machine managed.
		 */
		const long elapsed = (wxGetLocalTimeMillis() - started).ToLong();

		rpclog("TEST_INSPECTOR: auto-step %d ticks at %d/sec moved the PC %d "
		    "times, %08x -> %08x, %ldms total, %ldms per step\n",
		    ticks, rate, moved, first, last_snapshot_.regs[15], elapsed,
		    moved > 0 ? elapsed / moved : 0L);
	}

	/*
	 * And the safety property: a machine resumed from elsewhere must switch
	 * auto-step off rather than single-step it from under whoever resumed it.
	 */
	autostep_checkbox_->SetValue(true);
	autostep_timer_.Start(1000);
	emulator_.DebuggerResume();
	RefreshSnapshot();
	AutoStepTick();
	rpclog("TEST_INSPECTOR: auto-step on a running machine turned itself %s\n",
	    autostep_checkbox_->GetValue() ? "ON, WHICH IS WRONG" : "off");

	/*
	 * And the case a person actually hits: TICKING Auto while the machine is
	 * running. That is not the same as the check above, which sets the box
	 * from code and never runs the handler - and the difference is the whole
	 * of discussion #223's "I couldn't figure out how to step using the Auto
	 * step feature". It used to do nothing at all: the box unticked itself on
	 * the first tick and said nothing. Now it stops the machine and steps it.
	 */
	{
		wxCommandEvent ticked(wxEVT_CHECKBOX, ID_AUTOSTEP);
		uint32_t before;
		int moved_after_tick = 0;

		emulator_.DebuggerResume();
		RefreshSnapshot();

		autostep_checkbox_->SetValue(true);
		OnAutoStep(ticked);

		/* The pause is deferred to the next instruction, so tick as the timer
		   would until it lands. */
		for (int i = 0; i < 200 && last_snapshot_.debug_paused == 0; i++) {
			wxMilliSleep(5);
			AutoStepTick();
		}
		rpclog("TEST_INSPECTOR: auto-step ticked on a running machine: "
		    "machine %s, box %s\n",
		    last_snapshot_.debug_paused ? "stopped" : "STILL RUNNING",
		    autostep_checkbox_->GetValue() ? "still ticked" : "UNTICKED ITSELF");

		before = last_snapshot_.regs[15];
		for (int i = 0; i < 5; i++) {
			AutoStepTick();
			for (int waited = 0; waited < 50; waited++) {
				wxMilliSleep(2);
				RefreshSnapshot();
				if (last_snapshot_.debug_step_active == 0 &&
				    last_snapshot_.debug_paused != 0) {
					break;
				}
			}
			if (last_snapshot_.regs[15] != before) {
				moved_after_tick++;
				before = last_snapshot_.regs[15];
			}
		}
		rpclog("TEST_INSPECTOR: and then stepped %d times out of 5\n",
		    moved_after_tick);

		autostep_checkbox_->SetValue(false);
		autostep_timer_.Stop();
	}
}

void MachineInspectorWindow::OnAddBreakpoint(wxCommandEvent &)
{
	bool ok = false;
	const uint32_t address = ParseAddress(breakpoint_input_->GetValue(), &ok);
	if (!ok) {
		wxMessageBox("Please enter a valid hexadecimal address.", "Invalid address",
		             wxOK | wxICON_WARNING, this);
		return;
	}

	emulator_.DebuggerAddBreakpoint(address);
	breakpoint_input_->Clear();
	RefreshSnapshot();
}

void MachineInspectorWindow::OnRemoveBreakpoint(wxCommandEvent &)
{
	wxArrayInt selections;
	const int count = breakpoint_list_->GetSelections(selections);
	if (count == 0) {
		wxMessageBox("Select at least one breakpoint to remove.", "Remove breakpoint",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	for (unsigned int i = 0; i < selections.GetCount(); i++) {
		const uintptr_t client_data = reinterpret_cast<uintptr_t>(breakpoint_list_->GetClientData(selections[i]));
		emulator_.DebuggerRemoveBreakpoint(static_cast<uint32_t>(client_data));
	}
	RefreshSnapshot();
}

void MachineInspectorWindow::OnAddWatchpoint(wxCommandEvent &)
{
	bool ok = false;
	const uint32_t address = ParseAddress(watchpoint_address_input_->GetValue(), &ok);
	if (!ok) {
		wxMessageBox("Please enter a valid hexadecimal address.", "Invalid address",
		             wxOK | wxICON_WARNING, this);
		return;
	}

	static const uint32_t sizes[] = {1, 2, 4, 8};
	const int selection = watchpoint_size_choice_->GetSelection();
	const uint32_t size = sizes[selection >= 0 && selection < 4 ? selection : 2];
	const bool on_read = watchpoint_read_checkbox_->GetValue();
	const bool on_write = watchpoint_write_checkbox_->GetValue();
	if (!on_read && !on_write) {
		wxMessageBox("Watchpoints must trigger on read and/or write.", "Invalid watchpoint",
		             wxOK | wxICON_WARNING, this);
		return;
	}

	const bool log_only = watchpoint_log_only_checkbox_ != nullptr &&
	                      watchpoint_log_only_checkbox_->GetValue();
	emulator_.DebuggerAddWatchpoint(address, size, on_read, on_write, log_only);
	watchpoint_address_input_->Clear();
	RefreshSnapshot();
}

void MachineInspectorWindow::OnRemoveWatchpoint(wxCommandEvent &)
{
	wxArrayInt selections;
	const int count = watchpoint_list_->GetSelections(selections);
	if (count == 0) {
		wxMessageBox("Select at least one watchpoint to remove.", "Remove watchpoint",
		             wxOK | wxICON_INFORMATION, this);
		return;
	}

	for (unsigned int i = 0; i < selections.GetCount(); i++) {
		const unsigned int index = static_cast<unsigned int>(
		    reinterpret_cast<uintptr_t>(watchpoint_list_->GetClientData(selections[i])));
		if (index >= last_snapshot_.debug_watchpoint_count) {
			continue;
		}
		const DebugWatchpointInfo &wp = last_snapshot_.debug_watchpoints[index];
		emulator_.DebuggerRemoveWatchpoint(wp.address, wp.size, wp.on_read != 0, wp.on_write != 0);
	}
	RefreshSnapshot();
}

void MachineInspectorWindow::OnBreakpointSelection(wxCommandEvent &)
{
	breakpoint_remove_button_->Enable(ListBoxSelectionCount(breakpoint_list_) > 0);
}

void MachineInspectorWindow::OnWatchpointSelection(wxCommandEvent &)
{
	watchpoint_remove_button_->Enable(ListBoxSelectionCount(watchpoint_list_) > 0);
}
