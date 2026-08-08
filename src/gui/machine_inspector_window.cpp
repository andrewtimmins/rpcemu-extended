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

#include <algorithm>
#include <vector>

#include <wx/spinctrl.h>

#include "emulator_snapshot.h"

extern "C" {
#include "arm.h"
}

#include "arm_disasm.h"

namespace {

wxString FormatHex(uint32_t value, int width = 8)
{
	return wxString::Format("0x%0*X", width, value);
}

wxString ModeToString(uint32_t mode)
{
	switch (mode & 0x1f) {
	case USER: return "User";
	case FIQ: return "FIQ";
	case IRQ: return "IRQ";
	case SUPERVISOR: return "Supervisor";
	case ABORT: return "Abort";
	case UNDEFINED: return "Undefined";
	case SYSTEM: return "System";
	default: return wxString::Format("Unknown (0x%X)", mode & 0x1f);
	}
}

wxString NetworkTypeToString(NetworkType type)
{
	switch (type) {
	case NetworkType_Off: return "Off";
	case NetworkType_NAT: return "NAT";
	case NetworkType_EthernetBridging: return "Bridge";
	case NetworkType_IPTunnelling: return "IP Tunnel";
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
	EVT_BUTTON(ID_RUN, MachineInspectorWindow::OnRun)
	EVT_BUTTON(ID_PAUSE, MachineInspectorWindow::OnPause)
	EVT_BUTTON(ID_STEP, MachineInspectorWindow::OnStep)
	EVT_BUTTON(ID_BREAKPOINT_ADD, MachineInspectorWindow::OnAddBreakpoint)
	EVT_BUTTON(ID_BREAKPOINT_REMOVE, MachineInspectorWindow::OnRemoveBreakpoint)
	EVT_BUTTON(ID_WATCHPOINT_ADD, MachineInspectorWindow::OnAddWatchpoint)
	EVT_BUTTON(ID_WATCHPOINT_REMOVE, MachineInspectorWindow::OnRemoveWatchpoint)
	EVT_CHECKBOX(ID_TRACE_CONFIG, MachineInspectorWindow::OnTraceConfigChanged)
	EVT_BUTTON(ID_TRACE_CLEAR, MachineInspectorWindow::OnTraceClear)
wxEND_EVENT_TABLE()

MachineInspectorWindow::MachineInspectorWindow(wxWindow *parent, EmulatorHost &emulator)
	: wxFrame(parent, wxID_ANY, "Machine Inspector",
	          wxDefaultPosition, wxSize(700, 500),
	          wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER)
	, emulator_(emulator)
{
	BuildUi();
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

	auto *notebook = new wxNotebook(this, wxID_ANY);

	cpu_view_ = new wxTextCtrl(notebook, wxID_ANY, wxEmptyString,
	                           wxDefaultPosition, wxDefaultSize,
	                           wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(cpu_view_);
	notebook->AddPage(cpu_view_, "CPU");

	auto *disasm_panel = new wxPanel(notebook);
	disasm_address_input_ = new wxTextCtrl(disasm_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
	disasm_address_input_->SetHint("Address (hex)");
	auto *disasm_go_button = new wxButton(disasm_panel, ID_DISASM_GO, "Go");
	disasm_follow_pc_checkbox_ = new wxCheckBox(disasm_panel, ID_DISASM_FOLLOW_PC, "Follow PC");
	disasm_follow_pc_checkbox_->SetValue(true);

	auto *disasm_controls = new wxBoxSizer(wxHORIZONTAL);
	disasm_controls->Add(new wxStaticText(disasm_panel, wxID_ANY, "Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	disasm_controls->Add(disasm_address_input_, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_go_button, 0, wxRIGHT, 6);
	disasm_controls->Add(disasm_follow_pc_checkbox_, 0);

	disasm_view_ = new wxTextCtrl(disasm_panel, wxID_ANY, wxEmptyString,
	                              wxDefaultPosition, wxDefaultSize,
	                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(disasm_view_);

	auto *disasm_sizer = new wxBoxSizer(wxVERTICAL);
	disasm_sizer->Add(disasm_controls, 0, wxEXPAND | wxALL, 8);
	disasm_sizer->Add(disasm_view_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	disasm_panel->SetSizer(disasm_sizer);
	notebook->AddPage(disasm_panel, "Disassembly");

	auto *memory_panel = new wxPanel(notebook);
	memory_address_input_ = new wxTextCtrl(memory_panel, wxID_ANY, wxEmptyString,
	                                       wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
	memory_address_input_->SetHint("Address (hex)");
	memory_bytes_spin_ = new wxSpinCtrl(memory_panel, wxID_ANY, wxEmptyString,
	                                    wxDefaultPosition, wxSize(80, -1),
	                                    wxSP_ARROW_KEYS, 16, 4096, 256);
	memory_bytes_spin_->SetToolTip("Number of bytes to display");
	auto *memory_go_button = new wxButton(memory_panel, ID_MEMORY_GO, "Go");
	auto *memory_refresh_button = new wxButton(memory_panel, ID_MEMORY_REFRESH, "Refresh");

	auto *memory_controls = new wxBoxSizer(wxHORIZONTAL);
	memory_controls->Add(new wxStaticText(memory_panel, wxID_ANY, "Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_address_input_, 0, wxRIGHT, 6);
	memory_controls->Add(new wxStaticText(memory_panel, wxID_ANY, "Bytes:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	memory_controls->Add(memory_bytes_spin_, 0, wxRIGHT, 6);
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
	notebook->AddPage(memory_panel, "Memory");

	auto *debug_panel = new wxPanel(notebook);
	debug_status_label_ = new wxStaticText(debug_panel, wxID_ANY, "Debugger state: unknown");
	debug_hit_label_ = new wxStaticText(debug_panel, wxID_ANY, "Last watchpoint: none");

	run_button_ = new wxButton(debug_panel, ID_RUN, "Run");
	pause_button_ = new wxButton(debug_panel, ID_PAUSE, "Pause");
	step_button_ = new wxButton(debug_panel, ID_STEP, "Step");
	run_button_->Enable(false);
	pause_button_->Enable(false);
	step_button_->Enable(false);

	auto *debug_buttons = new wxBoxSizer(wxHORIZONTAL);
	debug_buttons->Add(run_button_, 0, wxRIGHT, 6);
	debug_buttons->Add(pause_button_, 0, wxRIGHT, 6);
	debug_buttons->Add(step_button_, 0);

	auto *breakpoint_box = new wxStaticBoxSizer(wxVERTICAL, debug_panel, "Breakpoints");
	breakpoint_list_ = new wxListBox(debug_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr,
	                                 wxLB_EXTENDED);
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
	watchpoint_list_ = new wxListBox(debug_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr,
	                                 wxLB_EXTENDED);
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

	auto *watchpoint_controls = new wxBoxSizer(wxHORIZONTAL);
	watchpoint_controls->Add(watchpoint_address_input_, 1, wxEXPAND | wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_size_choice_, 0, wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_read_checkbox_, 0, wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_write_checkbox_, 0, wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_log_only_checkbox_, 0, wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_add_button, 0, wxRIGHT, 6);
	watchpoint_controls->Add(watchpoint_remove_button_, 0);
	watchpoint_box->Add(watchpoint_list_, 1, wxEXPAND | wxBOTTOM, 6);
	watchpoint_box->Add(watchpoint_controls, 0, wxEXPAND);

	auto *debug_sizer = new wxBoxSizer(wxVERTICAL);
	debug_sizer->Add(debug_status_label_, 0, wxEXPAND | wxALL, 8);
	debug_sizer->Add(debug_hit_label_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	debug_sizer->Add(debug_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
	debug_sizer->Add(breakpoint_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	debug_sizer->Add(watchpoint_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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

	trace_view_ = new wxTextCtrl(trace_panel, wxID_ANY, wxEmptyString,
	                             wxDefaultPosition, wxDefaultSize,
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
	trace_sizer->Add(trace_view_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
	trace_sizer->Add(trace_footer, 0, wxEXPAND | wxALL, 8);
	trace_panel->SetSizer(trace_sizer);
	notebook->AddPage(trace_panel, "Trace");

	peripheral_view_ = new wxTextCtrl(notebook, wxID_ANY, wxEmptyString,
	                                  wxDefaultPosition, wxDefaultSize,
	                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
	ApplyMonoFont(peripheral_view_);
	notebook->AddPage(peripheral_view_, "Peripherals");

	auto *main = new wxBoxSizer(wxVERTICAL);
	main->Add(controls, 0, wxEXPAND | wxALL, 8);
	main->Add(notebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	SetSizer(main);

	disasm_address_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnDisasmGo, this);
	memory_address_input_->Bind(wxEVT_TEXT_ENTER, &MachineInspectorWindow::OnMemoryGo, this);
	breakpoint_list_->Bind(wxEVT_LISTBOX, &MachineInspectorWindow::OnBreakpointSelection, this);
	watchpoint_list_->Bind(wxEVT_LISTBOX, &MachineInspectorWindow::OnWatchpointSelection, this);
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

void MachineInspectorWindow::RefreshSnapshot()
{
	const MachineSnapshot snapshot = emulator_.TakeSnapshot();
	ApplySnapshot(snapshot);
}

void MachineInspectorWindow::ApplySnapshot(const MachineSnapshot &snapshot)
{
	last_snapshot_ = snapshot;

	summary_label_->SetLabel(MakeSummary(snapshot));
	SetTextIfChanged(cpu_view_, FormatRegisters(snapshot));
	SetTextIfChanged(peripheral_view_, FormatPeripheralSummary(snapshot));
	PopulateBreakpointList(snapshot);
	PopulateWatchpointList(snapshot);
	UpdateDebuggerUi(snapshot);

	if (disasm_follow_pc_checkbox_->GetValue()) {
		RefreshDisassembly(snapshot.pc);
	}
}

wxString MachineInspectorWindow::FormatRegisters(const MachineSnapshot &snapshot) const
{
	wxString text;

	for (int row = 0; row < 4; row++) {
		wxString line;
		for (int col = 0; col < 4; col++) {
			const int reg_index = row * 4 + col;
			line += wxString::Format("R%02d=%s  ", reg_index, FormatHex(snapshot.regs[reg_index]));
		}
		text += line.Trim() + "\n";
	}

	text += wxString::Format("PC = %s\n", FormatHex(snapshot.pc));
	text += wxString::Format("CPSR = %s\n", FormatHex(snapshot.cpsr));
	text += wxString::Format("Mode: %s%s | MMU: %s\n",
	                         ModeToString(snapshot.mode),
	                         snapshot.privileged_mode ? " (privileged)" : "",
	                         snapshot.mmu_enabled ? "enabled" : "disabled");
	text += wxString::Format("Core: %s | CPU idle: %s\n",
	                         snapshot.dynarec ? "Dynarec" : "Interpreter",
	                         snapshot.cpu_idle_enabled ? "enabled" : "disabled");
	text += wxString::Format("Performance: MIPS=%.2f", snapshot.perf_mips);
	return text;
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
	default: reason = "unknown"; break;
	}

	wxString status;
	if (paused) {
		status = wxString::Format("Debugger: Paused (%s)\nPC %s | Opcode %s",
		                          reason,
		                          FormatHex(snapshot.debug_halt_pc),
		                          FormatHex(snapshot.debug_halt_opcode));
	} else {
		const wxString state = pausing ? wxString::FromUTF8("Pausing\xE2\x80\xA6") : wxString("Running");
		status = wxString::Format("Debugger: %s\nLast PC %s | Last opcode %s",
		                          state,
		                          FormatHex(snapshot.debug_last_pc),
		                          FormatHex(snapshot.debug_last_opcode));
	}
	debug_status_label_->SetLabel(status);

	if (snapshot.debug_hit_size > 0) {
		const wxString access = snapshot.debug_hit_is_write ? "write" : "read";
		const int width = std::max(2, static_cast<int>(snapshot.debug_hit_size) * 2);
		debug_hit_label_->SetLabel(wxString::Format("Last watchpoint: %s | %u bytes %s | value %s",
		                                            FormatHex(snapshot.debug_hit_address),
		                                            snapshot.debug_hit_size,
		                                            access,
		                                            FormatHex(snapshot.debug_hit_value, width)));
	} else {
		debug_hit_label_->SetLabel("Last watchpoint: none");
	}

	run_button_->Enable(paused);
	pause_button_->Enable(!paused);
	step_button_->Enable(paused);
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

void MachineInspectorWindow::ApplyTraceConfig()
{
	DebugTraceConfig cfg{};
	cfg.trap_undefined = trap_undefined_checkbox_->GetValue() ? 1 : 0;
	cfg.trap_prefetch_abort = trap_prefetch_checkbox_->GetValue() ? 1 : 0;
	cfg.trap_data_abort = trap_data_abort_checkbox_->GetValue() ? 1 : 0;
	cfg.log_exceptions = log_exceptions_checkbox_->GetValue() ? 1 : 0;
	cfg.swi_trace_enabled = swi_trace_checkbox_->GetValue() ? 1 : 0;
	cfg.swi_trace_halt = swi_halt_checkbox_->GetValue() ? 1 : 0;

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
			default: break;
			}
			line = wxString::Format("%08u  PC=%s  EXCEPTION  %s",
			                        ev.seq, FormatHex(ev.pc), kind);
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

void MachineInspectorWindow::RefreshDisassembly(uint32_t address)
{
	disasm_current_address_ = address;
	SetTextIfChanged(disasm_view_, wxString::FromUTF8(emulator_disassemble_at(address, 32)));
}

void MachineInspectorWindow::RefreshMemoryView(uint32_t address)
{
	memory_current_address_ = address;
	memory_address_input_->SetValue(wxString::Format("%08X", address));

	int num_bytes = memory_bytes_spin_->GetValue();
	if (num_bytes < 16) {
		num_bytes = 16;
	}

	const std::vector<uint8_t> data = emulator_read_memory(address, static_cast<uint32_t>(num_bytes));
	if (data.empty()) {
		memory_view_->SetValue("Failed to read memory");
		return;
	}

	wxString text;
	const int bytes_per_line = 16;
	for (size_t offset = 0; offset < data.size(); offset += bytes_per_line) {
		wxString hex_part;
		wxString ascii_part;
		for (int i = 0; i < bytes_per_line; i++) {
			if (offset + static_cast<size_t>(i) < data.size()) {
				const uint8_t byte = data[offset + static_cast<size_t>(i)];
				hex_part += wxString::Format("%02X ", byte);
				ascii_part += (byte >= 32 && byte < 127) ? wxString(static_cast<char>(byte)) : ".";
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
