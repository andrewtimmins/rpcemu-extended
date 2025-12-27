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

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>

#include "parallel_dialog.h"

ParallelDialog::ParallelDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Parallel Port Configuration"));
    setMinimumWidth(450);
    setupUi();
}

ParallelDialog::~ParallelDialog()
{
}

void ParallelDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
    // Port configuration group
    QGroupBox *group = new QGroupBox(tr("Parallel Port"), this);
    QVBoxLayout *layout = new QVBoxLayout(group);
    
    // Mode selection
    mode_group = new QButtonGroup(this);
    
    disabled_radio = new QRadioButton(tr("Disabled"), group);
    logfile_radio = new QRadioButton(tr("Log to File"), group);
    printer_radio = new QRadioButton(tr("Virtual Printer (PDF)"), group);
    physical_radio = new QRadioButton(tr("Physical Device"), group);
    
    mode_group->addButton(disabled_radio, 0);
    mode_group->addButton(logfile_radio, 1);
    mode_group->addButton(printer_radio, 2);
    mode_group->addButton(physical_radio, 3);
    
    disabled_radio->setChecked(true);
    
    layout->addWidget(disabled_radio);
    
    // Log file options
    QHBoxLayout *logLayout = new QHBoxLayout();
    logLayout->addWidget(logfile_radio);
    logfile_edit = new QLineEdit(group);
    logfile_edit->setPlaceholderText(tr("Path to log file..."));
    logfile_edit->setEnabled(false);
    logbrowse_btn = new QPushButton(tr("Browse..."), group);
    logbrowse_btn->setEnabled(false);
    logLayout->addWidget(logfile_edit);
    logLayout->addWidget(logbrowse_btn);
    layout->addLayout(logLayout);
    
    // Virtual printer option (PDF output - no configuration needed)
    layout->addWidget(printer_radio);
    
    // Physical device options
    QHBoxLayout *physLayout = new QHBoxLayout();
    physLayout->addWidget(physical_radio);
    device_combo = new QComboBox(group);
    device_combo->setEnabled(false);
#ifdef Q_OS_LINUX
    device_combo->addItem("/dev/lp0");
    device_combo->addItem("/dev/lp1");
    device_combo->addItem("/dev/usb/lp0");
    device_combo->addItem("/dev/usb/lp1");
#elif defined(Q_OS_WIN)
    device_combo->addItem("LPT1");
    device_combo->addItem("LPT2");
#endif
    device_combo->setEditable(true);
    physLayout->addWidget(new QLabel(tr("Device:"), group));
    physLayout->addWidget(device_combo);
    physLayout->addStretch();
    layout->addLayout(physLayout);
    
    mainLayout->addWidget(group);
    
    // Connect signals
    connect(mode_group, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &ParallelDialog::onModeChanged);
    connect(logbrowse_btn, &QPushButton::clicked, this, &ParallelDialog::onBrowseLogFile);
    
    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    
    apply_button = new QPushButton(tr("Apply"), this);
    cancel_button = new QPushButton(tr("Cancel"), this);
    
    buttonLayout->addWidget(apply_button);
    buttonLayout->addWidget(cancel_button);
    
    mainLayout->addLayout(buttonLayout);
    
    connect(apply_button, &QPushButton::clicked, this, &ParallelDialog::onApply);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
}

void ParallelDialog::onModeChanged()
{
    int mode = mode_group->checkedId();
    logfile_edit->setEnabled(mode == 1);
    logbrowse_btn->setEnabled(mode == 1);
    device_combo->setEnabled(mode == 3);
}

void ParallelDialog::onBrowseLogFile()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Select Log File"), QDir::homePath(), tr("Log Files (*.log *.txt);;All Files (*)"));
    if (!fileName.isEmpty()) {
        logfile_edit->setText(fileName);
    }
}

ParallelPortSettings ParallelDialog::getSettings() const
{
    ParallelPortSettings settings;
    int mode = mode_group->checkedId();
    settings.mode = static_cast<ParallelPortMode>(mode);
    settings.logFilePath = logfile_edit->text();
    settings.physicalDevice = device_combo->currentText();
    return settings;
}

void ParallelDialog::setSettings(const ParallelPortSettings &settings)
{
    switch (settings.mode) {
    case ParallelPortMode::Disabled:
        disabled_radio->setChecked(true);
        break;
    case ParallelPortMode::LogToFile:
        logfile_radio->setChecked(true);
        break;
    case ParallelPortMode::VirtualPrinter:
        printer_radio->setChecked(true);
        break;
    case ParallelPortMode::PhysicalDevice:
        physical_radio->setChecked(true);
        break;
    }
    logfile_edit->setText(settings.logFilePath);
    int idx = device_combo->findText(settings.physicalDevice);
    if (idx >= 0) {
        device_combo->setCurrentIndex(idx);
    } else {
        device_combo->setEditText(settings.physicalDevice);
    }
    onModeChanged();
}

void ParallelDialog::onApply()
{
    accept();
}
