#include "machine_inspector_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QtGlobal>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QStringList>
#include <QSignalBlocker>
#include <QVariant>
#include <QSet>

#include "rpc-qt5.h"
#include "arm.h"

namespace {

QString modeToString(uint32_t mode)
{
	const uint32_t masked = mode & 0x1f;
	switch (masked) {
	case USER:       return QObject::tr("User");
	case FIQ:        return QObject::tr("FIQ");
	case IRQ:        return QObject::tr("IRQ");
	case SUPERVISOR: return QObject::tr("Supervisor");
	case ABORT:      return QObject::tr("Abort");
	case UNDEFINED:  return QObject::tr("Undefined");
	case SYSTEM:     return QObject::tr("System");
	default:         return QObject::tr("Unknown (0x%1)").arg(masked, 0, 16);
	}
}

QString networkTypeToString(NetworkType type)
{
	switch (type) {
	case NetworkType_Off:
		return QObject::tr("Off");
	case NetworkType_NAT:
		return QObject::tr("NAT");
	case NetworkType_EthernetBridging:
		return QObject::tr("Bridge");
	case NetworkType_IPTunnelling:
		return QObject::tr("IP Tunnel");
	default:
		return QObject::tr("Unknown");
	}
}

QString formatHex(uint32_t value, int width = 8)
{
	return QStringLiteral("0x%1").arg((qulonglong) value, width, 16, QLatin1Char('0')).toUpper();
}

QString formatColour24(uint32_t value)
{
	return QStringLiteral("#%1").arg((qulonglong) (value & 0xFFFFFFu), 6, 16, QLatin1Char('0')).toUpper();
}

QString vidcBppToString(uint32_t bit8)
{
	switch (bit8) {
	case 0: return QObject::tr("1 bpp (mono)");
	case 1: return QObject::tr("2 bpp (4 colours)");
	case 2: return QObject::tr("4 bpp (16 colours)");
	case 3: return QObject::tr("8 bpp (256 colours)");
	case 4: return QObject::tr("16 bpp (high colour)");
	case 6: return QObject::tr("32 bpp (true colour)");
	default: return QObject::tr("Unknown (%1)").arg(bit8);
	}
}

QString superIOTypeToString(int type)
{
	switch (type) {
	case SuperIOType_FDC37C665GT:
		return QObject::tr("SMC FDC37C665GT");
	case SuperIOType_FDC37C672:
		return QObject::tr("SMC FDC37C672");
	default:
		return QObject::tr("Unknown (%1)").arg(type);
	}
}

QString superIOModeToString(uint8_t mode)
{
	switch (mode) {
	case 0: return QObject::tr("Normal");
	case 1: return QObject::tr("Intermediate");
	case 2: return QObject::tr("Configuration");
	default: return QObject::tr("Unknown (%1)").arg(mode);
	}
}

}

MachineInspectorWindow::MachineInspectorWindow(Emulator &emulator, QWidget *parent)
    : QWidget(parent),
      emulator(emulator),
      summary_label(new QLabel(this)),
      cpu_view(new QPlainTextEdit(this)),
      pipeline_view(new QPlainTextEdit(this)),
	peripheral_summary_view(new QPlainTextEdit(this)),
	peripheral_vidc_view(new QPlainTextEdit(this)),
	peripheral_superio_view(new QPlainTextEdit(this)),
	peripheral_ide_view(new QPlainTextEdit(this)),
	peripheral_podules_view(new QPlainTextEdit(this)),
	peripheral_tabs(new QTabWidget(this)),
	auto_refresh_checkbox(new QCheckBox(tr("Auto refresh"), this)),
	debug_status_label(new QLabel(this)),
	debug_hit_label(new QLabel(this)),
	run_button(nullptr),
	pause_button(nullptr),
	step_button(nullptr),
	step5_button(nullptr),
	breakpoint_list(nullptr),
	breakpoint_input(nullptr),
	breakpoint_add_button(nullptr),
	breakpoint_remove_button(nullptr),
	watchpoint_list(nullptr),
	watchpoint_address_input(nullptr),
	watchpoint_size_combo(nullptr),
	watchpoint_read_checkbox(nullptr),
	watchpoint_write_checkbox(nullptr),
	watchpoint_add_button(nullptr),
	watchpoint_remove_button(nullptr)
{
	setWindowTitle(tr("Machine Inspector"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setWindowFlag(Qt::Window, true);

	summary_label->setText(tr("Awaiting snapshot"));
	summary_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

	QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	cpu_view->setFont(mono);
	pipeline_view->setFont(mono);
	peripheral_summary_view->setFont(mono);
	peripheral_vidc_view->setFont(mono);
	peripheral_superio_view->setFont(mono);
	peripheral_ide_view->setFont(mono);
	peripheral_podules_view->setFont(mono);

	cpu_view->setReadOnly(true);
	pipeline_view->setReadOnly(true);
	peripheral_summary_view->setReadOnly(true);
	peripheral_vidc_view->setReadOnly(true);
	peripheral_superio_view->setReadOnly(true);
	peripheral_ide_view->setReadOnly(true);
	peripheral_podules_view->setReadOnly(true);
	cpu_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	pipeline_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	peripheral_summary_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	peripheral_vidc_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	peripheral_superio_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	peripheral_ide_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	peripheral_podules_view->setLineWrapMode(QPlainTextEdit::NoWrap);

	auto_refresh_checkbox->setChecked(true);
	auto_refresh_checkbox->setToolTip(tr("Refresh the view automatically every 500 ms"));

	QPushButton *refresh_button = new QPushButton(tr("Refresh now"), this);

	run_button = new QPushButton(tr("Run"), this);
	pause_button = new QPushButton(tr("Pause"), this);
	step_button = new QPushButton(tr("Step"), this);
	step5_button = new QPushButton(tr("Step ×5"), this);

	run_button->setEnabled(false);
	pause_button->setEnabled(false);
	step_button->setEnabled(false);
	step5_button->setEnabled(false);

	debug_status_label->setText(tr("Debugger state: unknown"));
	debug_status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	debug_hit_label->setText(tr("Last watchpoint: none"));
	debug_hit_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

	breakpoint_list = new QListWidget(this);
	breakpoint_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	breakpoint_input = new QLineEdit(this);
	breakpoint_input->setPlaceholderText(tr("Address (hex)"));
	breakpoint_add_button = new QPushButton(tr("Add"), this);
	breakpoint_remove_button = new QPushButton(tr("Remove selected"), this);
	breakpoint_remove_button->setEnabled(false);

	watchpoint_list = new QListWidget(this);
	watchpoint_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	watchpoint_address_input = new QLineEdit(this);
	watchpoint_address_input->setPlaceholderText(tr("Address (hex)"));
	watchpoint_size_combo = new QComboBox(this);
	watchpoint_size_combo->addItem(tr("1 byte"), QVariant::fromValue(quint32(1)));
	watchpoint_size_combo->addItem(tr("2 bytes"), QVariant::fromValue(quint32(2)));
	watchpoint_size_combo->addItem(tr("4 bytes"), QVariant::fromValue(quint32(4)));
	watchpoint_size_combo->addItem(tr("8 bytes"), QVariant::fromValue(quint32(8)));
	watchpoint_read_checkbox = new QCheckBox(tr("Read"), this);
	watchpoint_write_checkbox = new QCheckBox(tr("Write"), this);
	watchpoint_read_checkbox->setChecked(true);
	watchpoint_write_checkbox->setChecked(true);
	watchpoint_add_button = new QPushButton(tr("Add"), this);
	watchpoint_remove_button = new QPushButton(tr("Remove selected"), this);
	watchpoint_remove_button->setEnabled(false);

	QTabWidget *tabs = new QTabWidget(this);

	QWidget *cpu_tab = new QWidget(this);
	QVBoxLayout *cpu_layout = new QVBoxLayout(cpu_tab);
	cpu_layout->addWidget(cpu_view);
	cpu_tab->setLayout(cpu_layout);
	tabs->addTab(cpu_tab, tr("CPU"));

	QWidget *pipeline_tab = new QWidget(this);
	QVBoxLayout *pipeline_layout = new QVBoxLayout(pipeline_tab);
	pipeline_layout->addWidget(pipeline_view);
	pipeline_tab->setLayout(pipeline_layout);
	tabs->addTab(pipeline_tab, tr("Pipeline"));

	QWidget *peripheral_tab = new QWidget(this);
	QVBoxLayout *peripheral_layout = new QVBoxLayout(peripheral_tab);
	peripheral_tabs->addTab(peripheral_summary_view, tr("Summary"));
	peripheral_tabs->addTab(peripheral_vidc_view, tr("VIDC"));
	peripheral_tabs->addTab(peripheral_superio_view, tr("SuperIO"));
	peripheral_tabs->addTab(peripheral_ide_view, tr("IDE"));
	peripheral_tabs->addTab(peripheral_podules_view, tr("Podules"));
	peripheral_layout->addWidget(peripheral_tabs);
	peripheral_tab->setLayout(peripheral_layout);
	tabs->addTab(peripheral_tab, tr("Peripherals"));

	QWidget *debug_tab = new QWidget(this);
	QVBoxLayout *debug_layout = new QVBoxLayout(debug_tab);
	debug_layout->addWidget(debug_status_label);
	debug_layout->addWidget(debug_hit_label);

	QHBoxLayout *debug_button_layout = new QHBoxLayout;
	debug_button_layout->addWidget(run_button);
	debug_button_layout->addWidget(pause_button);
	debug_button_layout->addWidget(step_button);
	debug_button_layout->addWidget(step5_button);
	debug_button_layout->addStretch(1);
	debug_layout->addLayout(debug_button_layout);

	QGroupBox *breakpoint_group = new QGroupBox(tr("Breakpoints"), debug_tab);
	QVBoxLayout *breakpoint_layout = new QVBoxLayout(breakpoint_group);
	breakpoint_layout->addWidget(breakpoint_list);
	QHBoxLayout *breakpoint_controls = new QHBoxLayout;
	breakpoint_controls->addWidget(breakpoint_input);
	breakpoint_controls->addWidget(breakpoint_add_button);
	breakpoint_controls->addWidget(breakpoint_remove_button);
	breakpoint_layout->addLayout(breakpoint_controls);
	debug_layout->addWidget(breakpoint_group);

	QGroupBox *watchpoint_group = new QGroupBox(tr("Watchpoints"), debug_tab);
	QVBoxLayout *watchpoint_layout = new QVBoxLayout(watchpoint_group);
	watchpoint_layout->addWidget(watchpoint_list);
	QHBoxLayout *watchpoint_controls = new QHBoxLayout;
	watchpoint_controls->addWidget(watchpoint_address_input);
	watchpoint_controls->addWidget(watchpoint_size_combo);
	watchpoint_controls->addWidget(watchpoint_read_checkbox);
	watchpoint_controls->addWidget(watchpoint_write_checkbox);
	watchpoint_controls->addWidget(watchpoint_add_button);
	watchpoint_controls->addWidget(watchpoint_remove_button);
	watchpoint_layout->addLayout(watchpoint_controls);
	debug_layout->addWidget(watchpoint_group);

	debug_layout->addStretch(1);
	debug_tab->setLayout(debug_layout);
	tabs->addTab(debug_tab, tr("Debugger"));

	QHBoxLayout *controls_layout = new QHBoxLayout;
	controls_layout->addWidget(summary_label, 1);
	controls_layout->addStretch();
	controls_layout->addWidget(auto_refresh_checkbox);
	controls_layout->addWidget(refresh_button);

	QVBoxLayout *root_layout = new QVBoxLayout(this);
	root_layout->addLayout(controls_layout);
	root_layout->addWidget(tabs);
	setLayout(root_layout);

	resize(700, 500);

	refresh_timer.setInterval(500);

	connect(&refresh_timer, &QTimer::timeout, this, &MachineInspectorWindow::refreshSnapshot);
	connect(refresh_button, &QPushButton::clicked, this, &MachineInspectorWindow::refreshSnapshot);
	connect(auto_refresh_checkbox, &QCheckBox::toggled, this, &MachineInspectorWindow::setAutoRefresh);
	connect(run_button, &QPushButton::clicked, this, &MachineInspectorWindow::onRunClicked);
	connect(pause_button, &QPushButton::clicked, this, &MachineInspectorWindow::onPauseClicked);
	connect(step_button, &QPushButton::clicked, this, &MachineInspectorWindow::onStepClicked);
	connect(step5_button, &QPushButton::clicked, this, &MachineInspectorWindow::onStepFiveClicked);
	connect(breakpoint_add_button, &QPushButton::clicked, this, &MachineInspectorWindow::onAddBreakpoint);
	connect(breakpoint_remove_button, &QPushButton::clicked, this, &MachineInspectorWindow::onRemoveBreakpoint);
	connect(watchpoint_add_button, &QPushButton::clicked, this, &MachineInspectorWindow::onAddWatchpoint);
	connect(watchpoint_remove_button, &QPushButton::clicked, this, &MachineInspectorWindow::onRemoveWatchpoint);
	connect(&emulator, &Emulator::debugger_state_changed_signal, this, &MachineInspectorWindow::refreshSnapshot);
	connect(breakpoint_list, &QListWidget::itemSelectionChanged, this, [this]() {
		breakpoint_remove_button->setEnabled(!breakpoint_list->selectedItems().isEmpty());
	});
	connect(watchpoint_list, &QListWidget::itemSelectionChanged, this, [this]() {
		watchpoint_remove_button->setEnabled(!watchpoint_list->selectedItems().isEmpty());
	});

	refresh_timer.start();
}

void
MachineInspectorWindow::showAndRaise()
{
	if (!isVisible()) {
		show();
	}
	raise();
	activateWindow();
	refreshSnapshot();
}

void
MachineInspectorWindow::refreshSnapshot()
{
	MachineSnapshot snapshot;
	bool ok = QMetaObject::invokeMethod(&emulator,
	                                   "takeSnapshot",
	                                   Qt::BlockingQueuedConnection,
	                                   Q_RETURN_ARG(MachineSnapshot, snapshot));
	if (!ok) {
		summary_label->setText(tr("Snapshot failed"));
		return;
	}

	applySnapshot(snapshot);
}

void
MachineInspectorWindow::setAutoRefresh(bool enabled)
{
	if (enabled) {
		if (!refresh_timer.isActive()) {
			refresh_timer.start();
		}
	} else {
		refresh_timer.stop();
	}
}

void
MachineInspectorWindow::applySnapshot(const MachineSnapshot &snapshot)
{
	summary_label->setText(makeSummary(snapshot));
	cpu_view->setPlainText(formatRegisters(snapshot));
	pipeline_view->setPlainText(formatPipeline(snapshot));
	peripheral_summary_view->setPlainText(formatPeripheralSummary(snapshot));
	peripheral_vidc_view->setPlainText(formatVidc(snapshot));
	peripheral_superio_view->setPlainText(formatSuperIO(snapshot));
	peripheral_ide_view->setPlainText(formatIDE(snapshot));
	peripheral_podules_view->setPlainText(formatPodules(snapshot));
	populateBreakpointList(snapshot);
	populateWatchpointList(snapshot);
	updateDebuggerUi(snapshot);
}

QString
MachineInspectorWindow::formatRegisters(const MachineSnapshot &snapshot) const
{
	QStringList lines;
	for (int row = 0; row < 4; row++) {
		QString line;
		for (int col = 0; col < 4; col++) {
			const int reg_index = row * 4 + col;
			line += QStringLiteral("R%1=%2  ")
			            .arg(reg_index, 2, 10, QLatin1Char('0'))
			            .arg(formatHex(snapshot.regs[reg_index]));
		}
		lines << line.trimmed();
	}

	lines << QStringLiteral("PC = %1").arg(formatHex(snapshot.pc));
	lines << QStringLiteral("CPSR = %1").arg(formatHex(snapshot.cpsr));
	lines << tr("Mode: %1%2 | MMU: %3")
	            .arg(modeToString(snapshot.mode))
	            .arg(snapshot.privileged_mode ? QStringLiteral(" (privileged)") : QString())
	            .arg(snapshot.mmu_enabled ? tr("enabled") : tr("disabled"));

	lines << tr("Core: %1 | CPU idle: %2")
	            .arg(snapshot.dynarec ? tr("Dynarec") : tr("Interpreter"))
	            .arg(snapshot.cpu_idle_enabled ? tr("enabled") : tr("disabled"));

	lines << tr("Performance: MIPS=%1  MHz=%2  TLB=%3 sec  Flush=%4 sec")
	            .arg(snapshot.perf_mips, 0, 'f', 2)
	            .arg(snapshot.perf_mhz, 0, 'f', 2)
	            .arg(snapshot.perf_tlb_sec, 0, 'f', 3)
	            .arg(snapshot.perf_flush_sec, 0, 'f', 3);
	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatPipeline(const MachineSnapshot &snapshot) const
{
	QStringList lines;
	for (int i = 0; i < 8; i++) {
		lines << QStringLiteral("%1 : %2")
		            .arg(formatHex(snapshot.pipeline_addr[i]))
		            .arg(formatHex(snapshot.pipeline_data[i]));
	}
	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatPeripheralSummary(const MachineSnapshot &snapshot) const
{
	QStringList lines;
	lines << tr("RAM: %1 MB | VRAM: %2 MB")
	            .arg(snapshot.config_mem_size)
	            .arg(snapshot.config_vram_size);
	lines << tr("Network: %1").arg(networkTypeToString(snapshot.network_type));
	lines << tr("IOMD IRQ A: status=%1 mask=%2")
	            .arg(formatHex(snapshot.iomd_irqa_status, 2))
	            .arg(formatHex(snapshot.iomd_irqa_mask, 2));
	lines << tr("IOMD IRQ B: status=%1 mask=%2")
	            .arg(formatHex(snapshot.iomd_irqb_status, 2))
	            .arg(formatHex(snapshot.iomd_irqb_mask, 2));
	lines << tr("IOMD FIQ: status=%1 mask=%2")
	            .arg(formatHex(snapshot.iomd_fiq_status, 2))
	            .arg(formatHex(snapshot.iomd_fiq_mask, 2));
	lines << tr("IOMD DMA: status=%1 mask=%2")
	            .arg(formatHex(snapshot.iomd_dma_status, 2))
	            .arg(formatHex(snapshot.iomd_dma_mask, 2));
	lines << tr("Timer0: counter=%1 in=%2 out=%3")
	            .arg(snapshot.iomd_timer0_counter)
	            .arg(snapshot.iomd_timer0_in_latch)
	            .arg(snapshot.iomd_timer0_out_latch);
	lines << tr("Timer1: counter=%1 in=%2 out=%3")
	            .arg(snapshot.iomd_timer1_counter)
	            .arg(snapshot.iomd_timer1_in_latch)
	            .arg(snapshot.iomd_timer1_out_latch);
	lines << tr("Sound DMA status: %1").arg(formatHex(snapshot.iomd_sound_status, 2));
	lines << tr("Floppy motor: %1").arg(snapshot.floppy_motor_on ? tr("on") : tr("off"));

	const VIDCStateSnapshot &vidc = snapshot.vidc;
	const uint32_t host_width = vidc.screen_width * (snapshot.vidc_double_x ? 2u : 1u);
	const uint32_t host_height = vidc.screen_height * (snapshot.vidc_double_y ? 2u : 1u);
	QStringList scaling;
	if (snapshot.vidc_double_x) {
		scaling << tr("horizontal");
	}
	if (snapshot.vidc_double_y) {
		scaling << tr("vertical");
	}
	const QString scaling_text = scaling.isEmpty() ? tr("none") : scaling.join(QLatin1String(", "));
	lines << tr("VIDC: %1×%2 (host %3×%4) | scaling %5 | %6")
	            .arg(vidc.screen_width)
	            .arg(vidc.screen_height)
	            .arg(host_width)
	            .arg(host_height)
	            .arg(scaling_text)
	            .arg(vidcBppToString(vidc.bit8));

	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatVidc(const MachineSnapshot &snapshot) const
{
	const VIDCStateSnapshot &vidc = snapshot.vidc;
	QStringList lines;
	lines << tr("Horizontal: display=%1 cursor=%2 end=%3")
	            .arg(formatHex(vidc.hdsr))
	            .arg(formatHex(vidc.hcsr))
	            .arg(formatHex(vidc.hder));
	lines << tr("Vertical: display=%1 cursor=%2 end=%3")
	            .arg(formatHex(vidc.vdsr))
	            .arg(formatHex(vidc.vcsr))
	            .arg(formatHex(vidc.vder));
	lines << tr("Cursor height: %1 scanlines").arg(vidc.cursor_height);
	lines << tr("Display size: %1×%2 | Cursor in window: X=%3 Y=%4")
	            .arg(vidc.screen_width)
	            .arg(vidc.screen_height)
	            .arg(vidc.cursor_x)
	            .arg(vidc.cursor_y);
	lines << tr("Palette index: %1").arg(vidc.palindex);
	lines << tr("Border colour: %1").arg(formatColour24(vidc.border_colour));
	lines << tr("Cursor palette: %1 %2 %3")
	            .arg(formatColour24(vidc.cursor_palette[0]))
	            .arg(formatColour24(vidc.cursor_palette[1]))
	            .arg(formatColour24(vidc.cursor_palette[2]));
	lines << tr("BPP mode: %1").arg(vidcBppToString(vidc.bit8));
	lines << tr("B0=%1 | B1=%2").arg(formatHex(vidc.b0)).arg(formatHex(vidc.b1));

	lines << QString();
	lines << tr("Palette preview (0-15):");
	for (int base = 0; base < 16; base += 4) {
		QString row;
		for (int i = 0; i < 4; i++) {
			const int index = base + i;
			row += QStringLiteral("P[%1]=%2  ")
			            .arg(index, 2, 10, QLatin1Char('0'))
			            .arg(formatColour24(vidc.palette[index]));
		}
		lines << row.trimmed();
	}

	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatSuperIO(const MachineSnapshot &snapshot) const
{
	const SuperIOStateSnapshot &sio = snapshot.superio;
	QStringList lines;
	lines << tr("Chip: %1").arg(superIOTypeToString(sio.super_type));
	lines << tr("Mode: %1").arg(superIOModeToString(sio.config_mode));
	lines << tr("Selected register: %1").arg(formatHex(sio.config_register, 2));
	lines << tr("Scratch=%1 | LineCtrl=%2 | PrintStatus=%3")
	            .arg(formatHex(sio.scratch, 2))
	            .arg(formatHex(sio.line_ctrl, 2))
	            .arg(formatHex(sio.print_status, 2));
	lines << tr("GP index=%1").arg(sio.gp_index);
	for (int base = 0; base < 16; base += 8) {
		QString row;
		for (int i = 0; i < 8; i++) {
			const int index = base + i;
			row += QStringLiteral("GP%1=%2  ")
			            .arg(index, 2, 10, QLatin1Char('0'))
			            .arg(formatHex(sio.gp_regs[index], 2));
		}
		lines << row.trimmed();
	}

	if (sio.super_type == SuperIOType_FDC37C665GT) {
		lines << QString();
		lines << tr("FDC37C665GT configuration registers:");
		QString row;
		for (int i = 0; i < 16; i++) {
			const QString reg = QStringLiteral("%1").arg(i, 2, 16, QLatin1Char('0')).toUpper();
			row += QStringLiteral("%1=%2  ").arg(reg, formatHex(sio.config_regs_665[i], 2));
			if ((i % 8) == 7) {
				lines << row.trimmed();
				row.clear();
			}
		}
		if (!row.isEmpty()) {
			lines << row.trimmed();
		}
	} else if (sio.super_type == SuperIOType_FDC37C672) {
		lines << QString();
		lines << tr("FDC37C672 configuration registers (0xB0-0xBF):");
		QString row;
		for (int i = 0; i < 16; i++) {
			const int reg_index = 0xB0 + i;
			const QString reg = QStringLiteral("%1").arg(reg_index, 2, 16, QLatin1Char('0')).toUpper();
			row += QStringLiteral("%1=%2  ").arg(reg, formatHex(sio.config_regs_672[reg_index], 2));
			if ((i % 8) == 7) {
				lines << row.trimmed();
				row.clear();
			}
		}
		if (!row.isEmpty()) {
			lines << row.trimmed();
		}
	}

	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatIDE(const MachineSnapshot &snapshot) const
{
	const IDEStateSnapshot &ide = snapshot.ide;
	QStringList lines;
	lines << tr("ATA status=%1 | error=%2 | command=%3")
	            .arg(formatHex(ide.atastat, 2))
	            .arg(formatHex(ide.error, 2))
	            .arg(formatHex(ide.command, 2));
	lines << tr("Drive select=%1 | FDISK=%2 | Packet status=%3")
	            .arg(ide.drive)
	            .arg(formatHex(ide.fdisk, 2))
	            .arg(ide.packet_status);
	lines << tr("CHS: C=%1 H=%2 S=%3 | Precomp=%4")
	            .arg(ide.cylinder)
	            .arg(ide.head)
	            .arg(ide.sector)
	            .arg(ide.cylprecomp);
	lines << tr("Sector count=%1 | Buffer pos=%2 | Packet length=%3")
	            .arg(ide.secount)
	            .arg(ide.pos)
	            .arg(ide.packlen);
	lines << tr("CD pos=%1 len=%2 | ASC=%3")
	            .arg(ide.cdpos)
	            .arg(ide.cdlen)
	            .arg(formatHex(ide.asc, 2));
	lines << tr("Disc changed: %1 | Reset: %2")
	            .arg(ide.disc_changed ? tr("yes") : tr("no"))
	            .arg(ide.reset_in_progress ? tr("yes") : tr("no"));

	for (int d = 0; d < 2; d++) {
		QStringList flags;
		flags << (ide.drive_present[d] ? tr("present") : tr("empty"));
		flags << (ide.drive_is_cdrom[d] ? tr("CD-ROM") : tr("Hard disc"));
		if (ide.drive_lba[d]) {
			flags << tr("LBA");
		}
		if (ide.drive_skip512[d]) {
			flags << tr("skip512");
		}
		lines << tr("Drive %1: %2 | SPT=%3 | HPC=%4")
		            .arg(d)
		            .arg(flags.join(QLatin1String(", ")))
		            .arg(ide.spt[d])
		            .arg(ide.hpc[d]);
	}

	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::formatPodules(const MachineSnapshot &snapshot) const
{
	const PodulesStateSnapshot &pod = snapshot.podules;
	QStringList lines;
	for (int slot = 0; slot < 8; slot++) {
		QStringList attrs;
		attrs << (pod.slot_used[slot] ? tr("populated") : tr("empty"));
		if (pod.irq[slot]) {
			attrs << tr("IRQ");
		}
		if (pod.fiq[slot]) {
			attrs << tr("FIQ");
		}
		lines << tr("Slot %1: %2")
		            .arg(slot)
		            .arg(attrs.join(QLatin1String(", ")));
	}
	return lines.join(QLatin1Char('\n'));
}

QString
MachineInspectorWindow::makeSummary(const MachineSnapshot &snapshot) const
{
	const QString core = snapshot.dynarec ? tr("Dynarec") : tr("Interpreter");
	const QString debug_state = snapshot.debug_paused
	    ? tr("Paused")
	    : (snapshot.debug_pause_requested ? tr("Pausing…") : tr("Running"));
	return tr("%1 | %2 (%3) | MIPS %4 | Network %5 | Debug %6")
	        .arg(QString::fromUtf8(snapshot.model_name))
	        .arg(QString::fromUtf8(snapshot.cpu_name))
	        .arg(core)
	        .arg(snapshot.perf_mips, 0, 'f', 1)
	        .arg(networkTypeToString(snapshot.network_type))
	        .arg(debug_state);
}

uint32_t
MachineInspectorWindow::parseAddress(const QString &text, bool *ok) const
{
	QString trimmed = text.trimmed();
	if (trimmed.endsWith(QLatin1Char('h'), Qt::CaseInsensitive)) {
		trimmed.chop(1);
	}
	if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
		trimmed = trimmed.mid(2);
	}
	bool local_ok = false;
	qulonglong parsed = trimmed.toULongLong(&local_ok, 16);
	if (!local_ok) {
		parsed = trimmed.toULongLong(&local_ok, 10);
	}
	if (!(local_ok && parsed <= 0xffffffffu)) {
		local_ok = false;
		parsed = 0;
	}
	if (ok != NULL) {
		*ok = local_ok;
	}
	return (uint32_t) parsed;
}

void
MachineInspectorWindow::populateBreakpointList(const MachineSnapshot &snapshot)
{
	if (breakpoint_list == NULL) {
		return;
	}

	QSet<quint32> selected;
	const QList<QListWidgetItem *> current_selection = breakpoint_list->selectedItems();
	for (QListWidgetItem *item : current_selection) {
		selected.insert(item->data(Qt::UserRole).toUInt());
	}

	{
		QSignalBlocker blocker(breakpoint_list);
		breakpoint_list->clear();
		for (uint32_t i = 0; i < snapshot.debug_breakpoint_count; i++) {
			const quint32 address = snapshot.debug_breakpoints[i];
			QListWidgetItem *item = new QListWidgetItem(formatHex(address), breakpoint_list);
			item->setData(Qt::UserRole, QVariant::fromValue(address));
			if (selected.contains(address)) {
				item->setSelected(true);
			}
		}
	}

	breakpoint_remove_button->setEnabled(!breakpoint_list->selectedItems().isEmpty());
}

void
MachineInspectorWindow::populateWatchpointList(const MachineSnapshot &snapshot)
{
	if (watchpoint_list == NULL) {
		return;
	}

	QSet<QString> selected_keys;
	const QList<QListWidgetItem *> current_selection = watchpoint_list->selectedItems();
	for (QListWidgetItem *item : current_selection) {
		selected_keys.insert(item->data(Qt::UserRole + 1).toString());
	}

	{
		QSignalBlocker blocker(watchpoint_list);
		watchpoint_list->clear();
		for (uint32_t i = 0; i < snapshot.debug_watchpoint_count; i++) {
			const DebugWatchpointInfo &wp = snapshot.debug_watchpoints[i];
			const quint32 address = wp.address;
			const quint32 size = wp.size;
			const bool on_read = (wp.on_read != 0);
			const bool on_write = (wp.on_write != 0);

			QStringList flags;
			if (on_read) {
				flags << tr("R");
			}
			if (on_write) {
				flags << tr("W");
			}
			if (flags.isEmpty()) {
				flags << tr("N/A");
			}

			const QString text = tr("%1 | %2 bytes | %3")
			        .arg(formatHex(address))
			        .arg(size)
			        .arg(flags.join(QStringLiteral("/")));
			QListWidgetItem *item = new QListWidgetItem(text, watchpoint_list);
			QVariantMap payload;
			payload.insert(QStringLiteral("address"), address);
			payload.insert(QStringLiteral("size"), size);
			payload.insert(QStringLiteral("read"), on_read);
			payload.insert(QStringLiteral("write"), on_write);
			item->setData(Qt::UserRole, payload);
			const QString key = QStringLiteral("%1|%2|%3|%4")
			        .arg(address)
			        .arg(size)
			        .arg(on_read ? 1 : 0)
			        .arg(on_write ? 1 : 0);
			item->setData(Qt::UserRole + 1, key);
			if (selected_keys.contains(key)) {
				item->setSelected(true);
			}
		}
	}

	watchpoint_remove_button->setEnabled(!watchpoint_list->selectedItems().isEmpty());
}

void
MachineInspectorWindow::updateDebuggerUi(const MachineSnapshot &snapshot)
{
	const bool paused = (snapshot.debug_paused != 0);
	const bool pausing = (snapshot.debug_pause_requested != 0);

	QString reason;
	switch (snapshot.debug_pause_reason) {
	case DebugPauseReason_User:
		reason = tr("manual pause");
		break;
	case DebugPauseReason_Breakpoint:
		reason = tr("breakpoint");
		break;
	case DebugPauseReason_Watchpoint:
		reason = tr("watchpoint");
		break;
	case DebugPauseReason_Step:
		reason = tr("single step");
		break;
	default:
		reason = tr("unknown");
		break;
	}

	QStringList status_lines;
	if (paused) {
		status_lines << tr("Debugger: Paused (%1)").arg(reason);
		status_lines << tr("PC %1 | Opcode %2")
		        .arg(formatHex(snapshot.debug_halt_pc))
		        .arg(formatHex(snapshot.debug_halt_opcode));
	} else {
		const QString state = pausing ? tr("Pausing…") : tr("Running");
		status_lines << tr("Debugger: %1").arg(state);
		status_lines << tr("Last PC %1 | Last opcode %2")
		        .arg(formatHex(snapshot.debug_last_pc))
		        .arg(formatHex(snapshot.debug_last_opcode));
	}
	debug_status_label->setText(status_lines.join(QLatin1Char('\n')));

	if (snapshot.debug_hit_size > 0) {
		const QString access = snapshot.debug_hit_is_write ? tr("write") : tr("read");
		const int width = qBound(2, (int) snapshot.debug_hit_size * 2, 8);
		debug_hit_label->setText(tr("Last watchpoint: %1 | %2 bytes %3 | value %4")
		        .arg(formatHex(snapshot.debug_hit_address))
		        .arg(snapshot.debug_hit_size)
		        .arg(access)
		        .arg(formatHex(snapshot.debug_hit_value, width)));
	} else {
		debug_hit_label->setText(tr("Last watchpoint: none"));
	}

	run_button->setEnabled(paused);
	pause_button->setEnabled(!paused);
	step_button->setEnabled(paused);
	step5_button->setEnabled(paused);

	{
		QSignalBlocker blocker_bp(breakpoint_list);
		QSignalBlocker blocker_wp(watchpoint_list);
		breakpoint_list->clearSelection();
		watchpoint_list->clearSelection();

		if (paused && snapshot.debug_pause_reason == DebugPauseReason_Breakpoint) {
			const quint32 halt_address = snapshot.debug_halt_pc;
			for (int i = 0; i < breakpoint_list->count(); i++) {
				QListWidgetItem *item = breakpoint_list->item(i);
				if (item->data(Qt::UserRole).toUInt() == halt_address) {
					item->setSelected(true);
					breakpoint_list->scrollToItem(item);
					break;
				}
			}
		} else if (paused && snapshot.debug_pause_reason == DebugPauseReason_Watchpoint && snapshot.debug_hit_size > 0) {
			const quint32 hit_address = snapshot.debug_hit_address;
			const quint32 hit_size = snapshot.debug_hit_size;
			const bool is_write = (snapshot.debug_hit_is_write != 0);
			for (int i = 0; i < watchpoint_list->count(); i++) {
				QListWidgetItem *item = watchpoint_list->item(i);
				const QVariantMap payload = item->data(Qt::UserRole).toMap();
				if (payload.value(QStringLiteral("address")).toUInt() == hit_address &&
				    payload.value(QStringLiteral("size")).toUInt() == hit_size) {
					const bool handles_write = payload.value(QStringLiteral("write")).toBool();
					const bool handles_read = payload.value(QStringLiteral("read")).toBool();
					if ((is_write && handles_write) || (!is_write && handles_read)) {
						item->setSelected(true);
						watchpoint_list->scrollToItem(item);
						break;
					}
				}
			}
		}
	}

	breakpoint_remove_button->setEnabled(!breakpoint_list->selectedItems().isEmpty());
	watchpoint_remove_button->setEnabled(!watchpoint_list->selectedItems().isEmpty());
}

void
MachineInspectorWindow::onRunClicked()
{
	QMetaObject::invokeMethod(&emulator, "debugger_resume", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onPauseClicked()
{
	QMetaObject::invokeMethod(&emulator, "debugger_pause", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onStepClicked()
{
	QMetaObject::invokeMethod(&emulator, "debugger_step", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onStepFiveClicked()
{
	QMetaObject::invokeMethod(&emulator,
	                         "debugger_step_n",
	                         Qt::QueuedConnection,
	                         Q_ARG(quint32, (quint32) 5));
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onAddBreakpoint()
{
	bool ok = false;
	const uint32_t address = parseAddress(breakpoint_input->text(), &ok);
	if (!ok) {
		QMessageBox::warning(this, tr("Invalid address"), tr("Please enter a valid hexadecimal address."));
		return;
	}
	QMetaObject::invokeMethod(&emulator,
	                         "debugger_add_breakpoint",
	                         Qt::QueuedConnection,
	                         Q_ARG(quint32, (quint32) address));
	breakpoint_input->clear();
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onRemoveBreakpoint()
{
	const QList<QListWidgetItem *> items = breakpoint_list->selectedItems();
	if (items.isEmpty()) {
		QMessageBox::information(this, tr("Remove breakpoint"), tr("Select at least one breakpoint to remove."));
		return;
	}
	for (QListWidgetItem *item : items) {
		const quint32 address = item->data(Qt::UserRole).toUInt();
		QMetaObject::invokeMethod(&emulator,
		                         "debugger_remove_breakpoint",
		                         Qt::QueuedConnection,
		                         Q_ARG(quint32, address));
	}
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onAddWatchpoint()
{
	bool ok = false;
	const uint32_t address = parseAddress(watchpoint_address_input->text(), &ok);
	if (!ok) {
		QMessageBox::warning(this, tr("Invalid address"), tr("Please enter a valid hexadecimal address."));
		return;
	}
	const quint32 size = watchpoint_size_combo->currentData().toUInt();
	const bool on_read = watchpoint_read_checkbox->isChecked();
	const bool on_write = watchpoint_write_checkbox->isChecked();
	if (!on_read && !on_write) {
		QMessageBox::warning(this, tr("Invalid watchpoint"), tr("Watchpoints must trigger on read and/or write."));
		return;
	}
	QMetaObject::invokeMethod(&emulator,
	                         "debugger_add_watchpoint",
	                         Qt::QueuedConnection,
	                         Q_ARG(quint32, (quint32) address),
	                         Q_ARG(quint32, size),
	                         Q_ARG(bool, on_read),
	                         Q_ARG(bool, on_write));
	watchpoint_address_input->clear();
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}

void
MachineInspectorWindow::onRemoveWatchpoint()
{
	const QList<QListWidgetItem *> items = watchpoint_list->selectedItems();
	if (items.isEmpty()) {
		QMessageBox::information(this, tr("Remove watchpoint"), tr("Select at least one watchpoint to remove."));
		return;
	}
	for (QListWidgetItem *item : items) {
		const QVariantMap payload = item->data(Qt::UserRole).toMap();
		const quint32 address = payload.value(QStringLiteral("address")).toUInt();
		const quint32 size = payload.value(QStringLiteral("size")).toUInt();
		const bool on_read = payload.value(QStringLiteral("read")).toBool();
		const bool on_write = payload.value(QStringLiteral("write")).toBool();
		QMetaObject::invokeMethod(&emulator,
		                         "debugger_remove_watchpoint",
		                         Qt::QueuedConnection,
		                         Q_ARG(quint32, address),
		                         Q_ARG(quint32, size),
		                         Q_ARG(bool, on_read),
		                         Q_ARG(bool, on_write));
	}
	QTimer::singleShot(0, this, &MachineInspectorWindow::refreshSnapshot);
}
