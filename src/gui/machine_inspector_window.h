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

#ifndef MACHINE_INSPECTOR_WINDOW_H
#define MACHINE_INSPECTOR_WINDOW_H

#include <cstdint>

#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>

#include "emulator_host.h"
#include "machine_snapshot.h"

class MachineInspectorWindow : public wxFrame {
public:
	explicit MachineInspectorWindow(wxWindow *parent, EmulatorHost &emulator);

	void ShowAndRaise();

	/**
	 * Log the Trace tab's controls beside the machine's own trapping and
	 * tracing settings, and say whether they agree.
	 *
	 * For RPCEMU_TEST_INSPECTOR_AFTER. A checkbox cannot be read from a script
	 * on macOS, so the window is asked instead - which is the only way to get
	 * evidence for issue #221, where the controls came up clear on a machine
	 * that was still trapping and tracing.
	 *
	 * @param when Short label for the log line, so before and after are
	 *             distinguishable
	 * @return true if every control matches the machine
	 */
	bool LogTraceControlsAgainstMachine(const char *when);

	/**
	 * Check the memory view reads the address space it claims to (issue #258).
	 *
	 * Driven by RPCEMU_TEST_INSPECTOR_AFTER against a booted machine, because
	 * none of this can be constructed in a unit test: it needs real page tables
	 * and a real MMU. Returns true when every check agreed.
	 */
	bool LogMemoryViewAgainstMachine(const char *when);

	/**
	 * Check the floating point panel shows the machine's own FPA registers
	 * (discussion #257).
	 *
	 * Driven by RPCEMU_TEST_INSPECTOR_AFTER, for the same reason as the two
	 * above: what a wxStaticText is displaying cannot be read from a script on
	 * macOS, and a value that reaches the snapshot correctly and then the label
	 * wrongly looks right from every other angle. Each register is reported on
	 * its own line rather than as one verdict. Returns true when all of them,
	 * and FPSR and FPCR, agree with the machine.
	 */
	bool LogFpRegistersAgainstMachine(const char *when);

	/*
	 * Driven by RPCEMU_TEST_INSPECTOR_AFTER. Auto-step and the session file are
	 * both window behaviour - a timer and two file dialogs - so no unit test can
	 * reach them; these are how they get exercised against a real machine.
	 */
	bool TestSessionRoundTrip(const wxString &path);
	void TestAutoStep(int rate, int ticks);

	/**
	 * The debugger has started or stopped; read the machine again now.
	 *
	 * This window's own refresh is a half-second timer, which is fine for
	 * watching a running machine and far too slow for stepping: the snapshot
	 * taken immediately after a step still says "running", because the step has
	 * not finished yet, so every button that needs a stopped machine greys out
	 * and nothing re-reads until the tick. Jon Abbott measured that in
	 * discussion #223 as a consistent half second per step with Step unclickable
	 * in between.
	 */
	void RefreshFromStateChange();

private:
	enum {
		ID_AUTO_REFRESH = wxID_HIGHEST + 1,
		ID_REFRESH_NOW,
		ID_DISASM_GO,
		ID_DISASM_FOLLOW_PC,
		ID_MEMORY_GO,
		ID_MEMORY_REFRESH,
		ID_RUN,
		ID_PAUSE,
		ID_STEP,
		ID_STEP_OVER,
		ID_DISASM_STYLE,
		ID_BREAKPOINT_ADD,
		ID_BREAKPOINT_REMOVE,
		ID_WATCHPOINT_ADD,
		ID_WATCHPOINT_REMOVE,
		ID_TRACE_CONFIG,
		ID_TRACE_CLEAR,
		ID_AUTOSTEP,
		ID_AUTOSTEP_TIMER,
		ID_SETTINGS_SAVE,
		ID_SETTINGS_LOAD,
		ID_SWI_NAMES,
	};

	/*
	 * How much disassembly the view shows, and how much of it sits above the
	 * PC when the view is following it. A third above and two thirds below
	 * reads better than dead centre: what led here is usually a handful of
	 * instructions, and where it is going is the part being read.
	 */
	static const int kDisasmLines = 32;
	static const int kDisasmLeadIn = 10;

	void BuildUi();

	/* The left-hand column of the debugger view: the registers, the decoded
	   PSR flags and the machine's state. */
	wxWindow *BuildStatePanel(wxWindow *parent);
	void ApplyMonoFont(wxWindow *window);

	void OnTimer(wxTimerEvent &event);
	void OnRefreshNow(wxCommandEvent &event);
	void OnAutoRefresh(wxCommandEvent &event);
	void OnDisasmGo(wxCommandEvent &event);
	void OnDisasmFollowPc(wxCommandEvent &event);
	void OnMemoryGo(wxCommandEvent &event);
	void OnMemoryRefresh(wxCommandEvent &event);
	void OnRun(wxCommandEvent &event);
	void OnPause(wxCommandEvent &event);
	void OnStep(wxCommandEvent &event);
	void OnStepOver(wxCommandEvent &event);

	/**
	 * Auto-step: keep stepping at a chosen rate while the machine is paused.
	 *
	 * "so you can set it to auto step every 1/4 second and watch it execute
	 * slowly. This would really help when you're watching code, waiting for
	 * something to happen but don't want to keep hitting the Step button" -
	 * discussion #223.
	 */
	void OnAutoStep(wxCommandEvent &event);
	void OnAutoStepTimer(wxTimerEvent &event);
	void AutoStepTick();

	/**
	 * Save and load everything that makes up a debugging session: the
	 * breakpoints, the watchpoints, the trapping and tracing settings and how
	 * the disassembly is rendered.
	 *
	 * Asked for as import/export in discussion #223, "so you can resume
	 * debugging easily" - a module being debugged is reloaded at a different
	 * address every time, but the settings around it do not change.
	 */
	void OnSaveSettings(wxCommandEvent &event);
	void OnLoadSettings(wxCommandEvent &event);
	void OnLoadSwiNames(wxCommandEvent &event);

	/* The session file itself, without the dialogs, so it can be driven by a
	   test as well as by somebody clicking. */
	bool SaveSessionTo(const wxString &path);
	bool LoadSessionFrom(const wxString &path);

	/** A disassembly rendering option moved; push the set to arm_disasm. */
	void OnDisasmStyleChanged(wxCommandEvent &event);
	void ApplyDisasmOptions();

	/** Enter runs or pauses, Space steps, Shift+Space steps over. */
	void OnDebugKey(wxKeyEvent &event);
	void OnAddBreakpoint(wxCommandEvent &event);
	void OnRemoveBreakpoint(wxCommandEvent &event);
	void OnAddWatchpoint(wxCommandEvent &event);
	void OnRemoveWatchpoint(wxCommandEvent &event);
	void OnBreakpointSelection(wxCommandEvent &event);
	void OnWatchpointSelection(wxCommandEvent &event);
	void OnTraceConfigChanged(wxCommandEvent &event);
	void OnTraceClear(wxCommandEvent &event);

	void RefreshSnapshot();
	void ApplySnapshot(const MachineSnapshot &snapshot);
	void RefreshDisassembly(uint32_t address);
	void RefreshMemoryView(uint32_t address);

	void ApplyProcessorState(const MachineSnapshot &snapshot);
	wxString FormatPeripheralSummary(const MachineSnapshot &snapshot) const;
	wxString FormatAcceleratorSummary(const MachineSnapshot &snapshot) const;
	wxString MakeSummary(const MachineSnapshot &snapshot) const;
	void UpdateDebuggerUi(const MachineSnapshot &snapshot);
	void PopulateBreakpointList(const MachineSnapshot &snapshot);
	void PopulateWatchpointList(const MachineSnapshot &snapshot);
	void ApplyTraceConfig();

	/* Fill the Trace tab in from the machine's own settings, once, when the
	   window first sees a snapshot. */
	void SeedTraceConfig(const MachineSnapshot &snapshot);

	void DrainTraceEvents();

	uint32_t ParseAddress(const wxString &text, bool *ok) const;

	EmulatorHost &emulator_;
	wxTimer refresh_timer_{this};

	wxStaticText *summary_label_ = nullptr;
	wxCheckBox *auto_refresh_checkbox_ = nullptr;

	/*
	 * The registers, one control per value, so that a value which has changed
	 * since the last refresh can be coloured on its own. A single text control
	 * cannot do that, which is why the old CPU page was a flat dump: everything
	 * looked alike and nothing said what had just moved.
	 */
	wxStaticText *reg_value_[16] = {};
	wxStaticText *reg_name_[16] = {};
	wxStaticText *flag_label_[7] = {};
	wxStaticText *mode_label_ = nullptr;
	wxStaticText *cpsr_label_ = nullptr;
	wxStaticText *spsr_label_ = nullptr;
	wxStaticText *mmu_label_ = nullptr;
	wxStaticText *core_label_ = nullptr;
	uint32_t previous_regs_[16] = {};
	bool have_previous_regs_ = false;
	wxStaticText *fpreg_value_[8] = {};
	wxStaticText *fpsr_label_ = nullptr;
	wxStaticText *fpcr_label_ = nullptr;
	uint32_t previous_fpregs_[8][3] = {};
	bool have_previous_fpregs_ = false;
	wxTextCtrl *disasm_view_ = nullptr;
	wxTextCtrl *memory_view_ = nullptr;
	wxTextCtrl *peripheral_view_ = nullptr;
	wxTextCtrl *accel_view_ = nullptr;

	wxTextCtrl *disasm_address_input_ = nullptr;
	wxCheckBox *disasm_follow_pc_checkbox_ = nullptr;

	/* How the disassembly is written down, from discussion #223. These drive
	   arm_disasm's global options, so they change the debug socket's output
	   too, which is deliberate: one machine, one rendering. */
	wxCheckBox *disasm_lower_checkbox_ = nullptr;
	/* Remembered so a saved session can name it and reload it. */
	wxString swi_names_path_;
	wxCheckBox *disasm_hex_checkbox_ = nullptr;
	wxCheckBox *disasm_apcs_checkbox_ = nullptr;
	wxCheckBox *disasm_ranges_checkbox_ = nullptr;
	wxCheckBox *disasm_resolve_checkbox_ = nullptr;

	wxTextCtrl *memory_address_input_ = nullptr;
	wxSpinCtrl *memory_bytes_spin_ = nullptr;
	wxCheckBox *memory_physical_checkbox_ = nullptr;

	wxStaticText *debug_status_label_ = nullptr;
	wxStaticText *debug_hit_label_ = nullptr;
	wxButton *run_button_ = nullptr;
	wxButton *pause_button_ = nullptr;
	wxButton *step_button_ = nullptr;
	wxButton *step_over_button_ = nullptr;
	wxCheckBox *autostep_checkbox_ = nullptr;
	wxSpinCtrl *autostep_rate_spin_ = nullptr;
	wxTimer autostep_timer_{this, ID_AUTOSTEP_TIMER};
	wxListBox *breakpoint_list_ = nullptr;
	wxTextCtrl *breakpoint_input_ = nullptr;
	wxButton *breakpoint_remove_button_ = nullptr;
	wxListBox *watchpoint_list_ = nullptr;
	wxTextCtrl *watchpoint_address_input_ = nullptr;
	wxChoice *watchpoint_size_choice_ = nullptr;
	wxCheckBox *watchpoint_read_checkbox_ = nullptr;
	wxCheckBox *watchpoint_write_checkbox_ = nullptr;
	wxCheckBox *watchpoint_log_only_checkbox_ = nullptr;
	wxButton *watchpoint_remove_button_ = nullptr;

	wxCheckBox *trap_undefined_checkbox_ = nullptr;
	wxCheckBox *trap_prefetch_checkbox_ = nullptr;
	wxCheckBox *trap_data_abort_checkbox_ = nullptr;
	wxCheckBox *log_exceptions_checkbox_ = nullptr;
	wxCheckBox *swi_trace_checkbox_ = nullptr;
	wxCheckBox *swi_halt_checkbox_ = nullptr;
	wxCheckBox *step_skip_irq_checkbox_ = nullptr;
	wxCheckBox *step_skip_os_checkbox_ = nullptr;
	wxCheckBox *step_skip_swi_checkbox_ = nullptr;
	wxTextCtrl *swi_filter_min_input_ = nullptr;
	wxTextCtrl *swi_filter_max_input_ = nullptr;
	wxTextCtrl *trace_view_ = nullptr;
	wxCheckBox *trace_autoscroll_checkbox_ = nullptr;
	wxStaticText *trace_dropped_label_ = nullptr;
	uint32_t trace_dropped_total_ = 0;

	/* Cleared until the Trace tab has been filled in from the machine. Only
	   the first snapshot may write to those controls; after that they are the
	   user's, and a snapshot taken before a click reached the emulator thread
	   would undo it. */
	bool trace_config_seeded_ = false;

	uint32_t disasm_current_address_ = 0;
	uint32_t memory_current_address_ = 0;
	/* Whether memory_current_address_ has been chosen, rather than using zero
	   as the sentinel for "not yet". Zero is a perfectly good address to want
	   to look at, and using it as the sentinel meant a deliberate 0 was
	   replaced by R13 on the very next snapshot - the flicker reported in
	   issue #258. */
	bool memory_address_chosen_ = false;
	MachineSnapshot last_snapshot_{};

	wxDECLARE_EVENT_TABLE();
};

#endif
