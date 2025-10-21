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

private:
	void applySnapshot(const MachineSnapshot &snapshot);
	QString formatRegisters(const MachineSnapshot &snapshot) const;
	QString formatPipeline(const MachineSnapshot &snapshot) const;
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
};

#endif // MACHINE_INSPECTOR_WINDOW_H
