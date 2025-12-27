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

#ifndef PARALLEL_DIALOG_H
#define PARALLEL_DIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>

/**
 * Parallel Port Mode - determines how the emulated parallel port behaves
 */
enum class ParallelPortMode {
    Disabled,       // Port is disabled
    LogToFile,      // Log all data to a file
    VirtualPrinter, // Emulated printer (outputs to PDF)
    PhysicalDevice  // Pass through to host parallel port
};

/**
 * Parallel Port configuration settings
 */
struct ParallelPortSettings {
    ParallelPortMode mode;
    QString logFilePath;        // For LogToFile mode
    QString physicalDevice;     // For PhysicalDevice mode (e.g., /dev/lp0)
};

/**
 * Dialog for configuring parallel port emulation
 * Note: RISC OS only exposes a single parallel port
 */
class ParallelDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParallelDialog(QWidget *parent = nullptr);
    virtual ~ParallelDialog();
    
    ParallelPortSettings getSettings() const;
    void setSettings(const ParallelPortSettings &settings);

private slots:
    void onModeChanged();
    void onBrowseLogFile();
    void onApply();

private:
    void setupUi();
    
    // Widgets
    QButtonGroup *mode_group;
    QRadioButton *disabled_radio;
    QRadioButton *logfile_radio;
    QRadioButton *printer_radio;
    QRadioButton *physical_radio;
    QLineEdit *logfile_edit;
    QPushButton *logbrowse_btn;
    QComboBox *device_combo;
    
    QPushButton *apply_button;
    QPushButton *cancel_button;
};

#endif /* PARALLEL_DIALOG_H */
