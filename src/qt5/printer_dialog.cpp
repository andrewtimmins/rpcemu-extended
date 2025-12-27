/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2024 RPCEmu contributors

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

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QSettings>
#include <QTimer>
#include <QMessageBox>

#include "printer_dialog.h"

extern "C" {
#include "printer.h"
}

PrinterDialog::PrinterDialog(QWidget *parent)
    : QDialog(parent)
{
	setWindowTitle(tr("Printer Settings"));
	setMinimumWidth(450);

	QVBoxLayout *main_layout = new QVBoxLayout(this);

	// Enable checkbox
	enabled_checkbox = new QCheckBox(tr("Enable parallel port printer capture"));
	main_layout->addWidget(enabled_checkbox);

	// Output path group
	QGroupBox *output_group = new QGroupBox(tr("Output Settings"));
	QVBoxLayout *output_layout = new QVBoxLayout(output_group);

	QHBoxLayout *path_layout = new QHBoxLayout();
	QLabel *path_label = new QLabel(tr("Output folder:"));
	output_path_edit = new QLineEdit();
	output_path_edit->setPlaceholderText(tr("Leave empty for current directory"));
	browse_button = new QPushButton(tr("Browse..."));
	path_layout->addWidget(path_label);
	path_layout->addWidget(output_path_edit, 1);
	path_layout->addWidget(browse_button);
	output_layout->addLayout(path_layout);

	QLabel *info_label = new QLabel(
		tr("Print jobs will be saved as timestamped .prn files.\n"
		   "These can be viewed with a PostScript viewer or converted to PDF."));
	info_label->setWordWrap(true);
	info_label->setStyleSheet("color: #666;");
	output_layout->addWidget(info_label);

	main_layout->addWidget(output_group);

	// Status group
	QGroupBox *status_group = new QGroupBox(tr("Current Status"));
	QHBoxLayout *status_layout = new QHBoxLayout(status_group);

	buffer_status_label = new QLabel(tr("Buffer: 0 bytes"));
	flush_button = new QPushButton(tr("Flush Now"));
	flush_button->setToolTip(tr("Save any pending print data to file immediately"));
	status_layout->addWidget(buffer_status_label, 1);
	status_layout->addWidget(flush_button);

	main_layout->addWidget(status_group);

	// Spacer
	main_layout->addStretch();

	// Button box
	QHBoxLayout *button_layout = new QHBoxLayout();
	button_layout->addStretch();
	ok_button = new QPushButton(tr("OK"));
	ok_button->setDefault(true);
	cancel_button = new QPushButton(tr("Cancel"));
	button_layout->addWidget(ok_button);
	button_layout->addWidget(cancel_button);
	main_layout->addLayout(button_layout);

	// Connections
	connect(browse_button, &QPushButton::clicked, this, &PrinterDialog::on_browse_clicked);
	connect(flush_button, &QPushButton::clicked, this, &PrinterDialog::on_flush_clicked);
	connect(ok_button, &QPushButton::clicked, this, &PrinterDialog::on_ok_clicked);
	connect(cancel_button, &QPushButton::clicked, this, &PrinterDialog::on_cancel_clicked);

	// Timer to update buffer status
	status_timer = new QTimer(this);
	connect(status_timer, &QTimer::timeout, this, &PrinterDialog::update_buffer_status);
	status_timer->start(500);

	load_settings();
	update_buffer_status();
}

PrinterDialog::~PrinterDialog()
{
}

void
PrinterDialog::load_settings()
{
	QSettings settings("RPCEmu", "RPCEmu");

	bool enabled = settings.value("printer/enabled", false).toBool();
	QString path = settings.value("printer/output_path", "").toString();

	enabled_checkbox->setChecked(enabled);
	output_path_edit->setText(path);
}

void
PrinterDialog::save_settings()
{
	QSettings settings("RPCEmu", "RPCEmu");

	settings.setValue("printer/enabled", enabled_checkbox->isChecked());
	settings.setValue("printer/output_path", output_path_edit->text());
}

void
PrinterDialog::apply_settings()
{
	// Apply to printer module
	if (enabled_checkbox->isChecked()) {
		printer_set_output_mode(PrinterOutput_File);
	} else {
		printer_set_output_mode(PrinterOutput_Disabled);
	}

	printer_set_output_path(output_path_edit->text().toUtf8().constData());
}

void
PrinterDialog::on_browse_clicked()
{
	QString dir = QFileDialog::getExistingDirectory(
		this,
		tr("Select Output Folder"),
		output_path_edit->text(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (!dir.isEmpty()) {
		output_path_edit->setText(dir);
	}
}

void
PrinterDialog::on_flush_clicked()
{
	size_t bytes = printer_get_buffer_size();
	
	if (bytes == 0) {
		QMessageBox::information(this, tr("Flush Print Buffer"),
			tr("The print buffer is empty."));
		return;
	}

	printer_flush();
	update_buffer_status();

	QMessageBox::information(this, tr("Flush Print Buffer"),
		tr("Saved %1 bytes to file.").arg(bytes));
}

void
PrinterDialog::on_ok_clicked()
{
	save_settings();
	apply_settings();
	accept();
}

void
PrinterDialog::on_cancel_clicked()
{
	reject();
}

void
PrinterDialog::update_buffer_status()
{
	size_t bytes = printer_get_buffer_size();
	
	if (bytes == 0) {
		buffer_status_label->setText(tr("Buffer: empty"));
		flush_button->setEnabled(false);
	} else if (bytes < 1024) {
		buffer_status_label->setText(tr("Buffer: %1 bytes").arg(bytes));
		flush_button->setEnabled(true);
	} else {
		buffer_status_label->setText(tr("Buffer: %1 KB").arg(bytes / 1024));
		flush_button->setEnabled(true);
	}
}
