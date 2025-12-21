/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

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
 * machine_inspector_window.h - Machine Inspector debugging window
 *
 * Declares the MachineInspectorWindow class which provides a Qt-based
 * debugging interface for inspecting CPU state, memory, disassembly,
 * peripherals, and managing breakpoints/watchpoints.
 */

#ifndef MACHINE_INSPECTOR_WINDOW_H
#define MACHINE_INSPECTOR_WINDOW_H

#include <QWidget>
#include <QTimer>

#include "machine_snapshot.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

class Emulator;

class MachineInspectorWindow : public QWidget
{
	Q_OBJECT

public:
	explicit MachineInspectorWindow(Emulator &emulator, QWidget *parent = nullptr);

	void showAndRaise();

public slots:
	void refreshSnapshot();
	void setAutoRefresh(bool enabled);
	void onRunClicked();
	void onPauseClicked();
	void onStepClicked();
	void onStepFiveClicked();
	void onAddBreakpoint();
	void onRemoveBreakpoint();
	void onAddWatchpoint();
	void onRemoveWatchpoint();
	void onDisasmGoToAddress();
	void onDisasmFollowPC(bool checked);
	void onMemoryGoToAddress();
	void onMemoryRefresh();
	void onMemoryPrevPage();
	void onMemoryNextPage();
	void onMemoryWordSizeChanged(int index);
	void onMemoryJumpROM();
	void onMemoryJumpRAM();
	void onMemoryJumpSP();
	void onMemoryJumpPC();
	void onMemorySearch();
	void onMemoryCopy();

private:
	void applySnapshot(const MachineSnapshot &snapshot);
	QString formatRegisters(const MachineSnapshot &snapshot) const;
	QString formatPipeline(const MachineSnapshot &snapshot) const;
	QString formatDisassembly(uint32_t address, int count);
	QString formatPeripheralSummary(const MachineSnapshot &snapshot) const;
	QString formatVidc(const MachineSnapshot &snapshot) const;
	QString formatSuperIO(const MachineSnapshot &snapshot) const;
	QString formatIDE(const MachineSnapshot &snapshot) const;
	QString formatPodules(const MachineSnapshot &snapshot) const;
	QString makeSummary(const MachineSnapshot &snapshot) const;
	uint32_t parseAddress(const QString &text, bool *ok) const;
	void updateDebuggerUi(const MachineSnapshot &snapshot);
	void populateBreakpointList(const MachineSnapshot &snapshot);
	void populateWatchpointList(const MachineSnapshot &snapshot);
	void refreshDisassembly(uint32_t address);
	void refreshMemoryView(uint32_t address);

private:
	Emulator &emulator;
	QTimer refresh_timer;

	QLabel *summary_label;
	QPlainTextEdit *cpu_view;
	QPlainTextEdit *pipeline_view;
	QPlainTextEdit *peripheral_summary_view;
	QPlainTextEdit *peripheral_vidc_view;
	QPlainTextEdit *peripheral_superio_view;
	QPlainTextEdit *peripheral_ide_view;
	QPlainTextEdit *peripheral_podules_view;
	QTabWidget *peripheral_tabs;
	QCheckBox *auto_refresh_checkbox;
	QLabel *debug_status_label;
	QLabel *debug_hit_label;
	QPushButton *run_button;
	QPushButton *pause_button;
	QPushButton *step_button;
	QPushButton *step5_button;
	QListWidget *breakpoint_list;
	QLineEdit *breakpoint_input;
	QPushButton *breakpoint_add_button;
	QPushButton *breakpoint_remove_button;
	QListWidget *watchpoint_list;
	QLineEdit *watchpoint_address_input;
	QComboBox *watchpoint_size_combo;
	QCheckBox *watchpoint_read_checkbox;
	QCheckBox *watchpoint_write_checkbox;
	QPushButton *watchpoint_add_button;
	QPushButton *watchpoint_remove_button;

	/* Disassembly view */
	QPlainTextEdit *disasm_view;
	QLineEdit *disasm_address_input;
	QPushButton *disasm_go_button;
	QCheckBox *disasm_follow_pc_checkbox;
	uint32_t disasm_current_address;

	/* Memory viewer */
	QPlainTextEdit *memory_view;
	QLineEdit *memory_address_input;
	QSpinBox *memory_bytes_spin;
	QPushButton *memory_go_button;
	QPushButton *memory_refresh_button;
	QPushButton *memory_prev_button;
	QPushButton *memory_next_button;
	QComboBox *memory_word_size_combo;
	QPushButton *memory_jump_rom_button;
	QPushButton *memory_jump_ram_button;
	QPushButton *memory_jump_sp_button;
	QPushButton *memory_jump_pc_button;
	QLineEdit *memory_search_input;
	QPushButton *memory_search_button;
	QPushButton *memory_copy_button;
	uint32_t memory_current_address;
	int memory_word_size;           /**< 1=bytes, 2=16-bit, 4=32-bit */
	MachineSnapshot last_snapshot;  /**< Cached for quick jump to SP/PC */
};

#endif // MACHINE_INSPECTOR_WINDOW_H
