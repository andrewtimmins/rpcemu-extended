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
#ifndef PRINTER_DIALOG_H
#define PRINTER_DIALOG_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

class PrinterDialog : public QDialog
{
	Q_OBJECT

public:
	PrinterDialog(QWidget *parent = nullptr);
	virtual ~PrinterDialog();

private slots:
	void on_browse_clicked();
	void on_flush_clicked();
	void on_ok_clicked();
	void on_cancel_clicked();
	void update_buffer_status();

private:
	void load_settings();
	void save_settings();
	void apply_settings();

	QCheckBox *enabled_checkbox;
	QLineEdit *output_path_edit;
	QPushButton *browse_button;
	QLabel *buffer_status_label;
	QPushButton *flush_button;
	QPushButton *ok_button;
	QPushButton *cancel_button;
	
	QTimer *status_timer;
};

#endif // PRINTER_DIALOG_H
