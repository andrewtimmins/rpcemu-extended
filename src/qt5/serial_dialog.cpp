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
#include <QFormLayout>
#include <QFileDialog>
#include <QDir>

#include "serial_dialog.h"

SerialDialog::SerialDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Serial Port Configuration"));
    setMinimumWidth(500);
    setupUi();
}

SerialDialog::~SerialDialog()
{
}

void SerialDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
    // COM1 group
    mainLayout->addWidget(createPortGroup(tr("COM1 (0x3F8)"), 1));
    
    // COM2 group
    mainLayout->addWidget(createPortGroup(tr("COM2 (0x2F8)"), 2));
    
    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    
    apply_button = new QPushButton(tr("Apply"), this);
    cancel_button = new QPushButton(tr("Cancel"), this);
    
    buttonLayout->addWidget(apply_button);
    buttonLayout->addWidget(cancel_button);
    
    mainLayout->addLayout(buttonLayout);
    
    connect(apply_button, &QPushButton::clicked, this, &SerialDialog::onApply);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
}

QGroupBox* SerialDialog::createPortGroup(const QString &title, int portIndex)
{
    QGroupBox *group = new QGroupBox(title, this);
    QVBoxLayout *layout = new QVBoxLayout(group);
    
    // Mode selection
    QButtonGroup *modeGroup = new QButtonGroup(this);
    
    QRadioButton *disabledRadio = new QRadioButton(tr("Disabled"), group);
    QRadioButton *logfileRadio = new QRadioButton(tr("Log to File"), group);
    QRadioButton *tcpmodemRadio = new QRadioButton(tr("TCP Modem (AT commands)"), group);
    QRadioButton *physicalRadio = new QRadioButton(tr("Physical Device"), group);
    
    modeGroup->addButton(disabledRadio, 0);
    modeGroup->addButton(logfileRadio, 1);
    modeGroup->addButton(tcpmodemRadio, 2);
    modeGroup->addButton(physicalRadio, 3);
    
    disabledRadio->setChecked(true);
    
    layout->addWidget(disabledRadio);
    
    // Log file options
    QHBoxLayout *logLayout = new QHBoxLayout();
    logLayout->addWidget(logfileRadio);
    QLineEdit *logEdit = new QLineEdit(group);
    logEdit->setPlaceholderText(tr("Path to log file..."));
    logEdit->setEnabled(false);
    QPushButton *browseBtn = new QPushButton(tr("Browse..."), group);
    browseBtn->setEnabled(false);
    logLayout->addWidget(logEdit);
    logLayout->addWidget(browseBtn);
    layout->addLayout(logLayout);
    
    // TCP modem option (no configuration needed - handled by guest via AT commands)
    layout->addWidget(tcpmodemRadio);
    
    // Physical device options
    QHBoxLayout *physLayout = new QHBoxLayout();
    physLayout->addWidget(physicalRadio);
    QComboBox *deviceCombo = new QComboBox(group);
    deviceCombo->setEnabled(false);
    // Populate with common device paths
#ifdef Q_OS_LINUX
    deviceCombo->addItem("/dev/ttyUSB0");
    deviceCombo->addItem("/dev/ttyUSB1");
    deviceCombo->addItem("/dev/ttyS0");
    deviceCombo->addItem("/dev/ttyS1");
#elif defined(Q_OS_WIN)
    deviceCombo->addItem("COM1");
    deviceCombo->addItem("COM2");
    deviceCombo->addItem("COM3");
    deviceCombo->addItem("COM4");
#endif
    deviceCombo->setEditable(true);
    physLayout->addWidget(new QLabel(tr("Device:"), group));
    physLayout->addWidget(deviceCombo);
    physLayout->addStretch();
    layout->addLayout(physLayout);
    
    // Store widget references
    if (portIndex == 1) {
        com1_mode_group = modeGroup;
        com1_disabled_radio = disabledRadio;
        com1_logfile_radio = logfileRadio;
        com1_tcpmodem_radio = tcpmodemRadio;
        com1_physical_radio = physicalRadio;
        com1_logfile_edit = logEdit;
        com1_browse_btn = browseBtn;
        com1_device_combo = deviceCombo;
        
        connect(modeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
                this, &SerialDialog::onCom1ModeChanged);
        connect(browseBtn, &QPushButton::clicked, this, &SerialDialog::onBrowseLogFile1);
    } else {
        com2_mode_group = modeGroup;
        com2_disabled_radio = disabledRadio;
        com2_logfile_radio = logfileRadio;
        com2_tcpmodem_radio = tcpmodemRadio;
        com2_physical_radio = physicalRadio;
        com2_logfile_edit = logEdit;
        com2_browse_btn = browseBtn;
        com2_device_combo = deviceCombo;
        
        connect(modeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
                this, &SerialDialog::onCom2ModeChanged);
        connect(browseBtn, &QPushButton::clicked, this, &SerialDialog::onBrowseLogFile2);
    }
    
    return group;
}

void SerialDialog::onCom1ModeChanged()
{
    int mode = com1_mode_group->checkedId();
    com1_logfile_edit->setEnabled(mode == 1);
    com1_browse_btn->setEnabled(mode == 1);
    com1_device_combo->setEnabled(mode == 3);
}

void SerialDialog::onCom2ModeChanged()
{
    int mode = com2_mode_group->checkedId();
    com2_logfile_edit->setEnabled(mode == 1);
    com2_browse_btn->setEnabled(mode == 1);
    com2_device_combo->setEnabled(mode == 3);
}

void SerialDialog::onBrowseLogFile1()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Select Log File"), QDir::homePath(), tr("Log Files (*.log *.txt);;All Files (*)"));
    if (!fileName.isEmpty()) {
        com1_logfile_edit->setText(fileName);
    }
}

void SerialDialog::onBrowseLogFile2()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Select Log File"), QDir::homePath(), tr("Log Files (*.log *.txt);;All Files (*)"));
    if (!fileName.isEmpty()) {
        com2_logfile_edit->setText(fileName);
    }
}

SerialPortSettings SerialDialog::getCom1Settings() const
{
    SerialPortSettings settings;
    int mode = com1_mode_group->checkedId();
    settings.mode = static_cast<SerialPortMode>(mode);
    settings.logFilePath = com1_logfile_edit->text();
    settings.physicalDevice = com1_device_combo->currentText();
    return settings;
}

SerialPortSettings SerialDialog::getCom2Settings() const
{
    SerialPortSettings settings;
    int mode = com2_mode_group->checkedId();
    settings.mode = static_cast<SerialPortMode>(mode);
    settings.logFilePath = com2_logfile_edit->text();
    settings.physicalDevice = com2_device_combo->currentText();
    return settings;
}

void SerialDialog::setCom1Settings(const SerialPortSettings &settings)
{
    switch (settings.mode) {
    case SerialPortMode::Disabled:
        com1_disabled_radio->setChecked(true);
        break;
    case SerialPortMode::LogToFile:
        com1_logfile_radio->setChecked(true);
        break;
    case SerialPortMode::TcpModem:
        com1_tcpmodem_radio->setChecked(true);
        break;
    case SerialPortMode::PhysicalDevice:
        com1_physical_radio->setChecked(true);
        break;
    }
    com1_logfile_edit->setText(settings.logFilePath);
    int idx = com1_device_combo->findText(settings.physicalDevice);
    if (idx >= 0) {
        com1_device_combo->setCurrentIndex(idx);
    } else {
        com1_device_combo->setEditText(settings.physicalDevice);
    }
    onCom1ModeChanged();
}

void SerialDialog::setCom2Settings(const SerialPortSettings &settings)
{
    switch (settings.mode) {
    case SerialPortMode::Disabled:
        com2_disabled_radio->setChecked(true);
        break;
    case SerialPortMode::LogToFile:
        com2_logfile_radio->setChecked(true);
        break;
    case SerialPortMode::TcpModem:
        com2_tcpmodem_radio->setChecked(true);
        break;
    case SerialPortMode::PhysicalDevice:
        com2_physical_radio->setChecked(true);
        break;
    }
    com2_logfile_edit->setText(settings.logFilePath);
    int idx = com2_device_combo->findText(settings.physicalDevice);
    if (idx >= 0) {
        com2_device_combo->setCurrentIndex(idx);
    } else {
        com2_device_combo->setEditText(settings.physicalDevice);
    }
    onCom2ModeChanged();
}

void SerialDialog::onApply()
{
    // For now, just accept - settings will be retrieved by caller
    accept();
}
