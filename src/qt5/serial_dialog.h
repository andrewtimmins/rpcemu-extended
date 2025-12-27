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

#ifndef SERIAL_DIALOG_H
#define SERIAL_DIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>

/**
 * Serial Port Mode - determines how the emulated serial port behaves
 */
enum class SerialPortMode {
    Disabled,       // Port is disabled
    LogToFile,      // Log all TX data to a file
    TcpModem,       // Emulated TCP modem (AT command set)
    PhysicalDevice  // Pass through to host serial port
};

/**
 * Serial Port configuration settings
 */
struct SerialPortSettings {
    SerialPortMode mode;
    QString logFilePath;        // For LogToFile mode
    QString physicalDevice;     // For PhysicalDevice mode (e.g., /dev/ttyUSB0)
};

/**
 * Dialog for configuring serial port emulation
 */
class SerialDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SerialDialog(QWidget *parent = nullptr);
    virtual ~SerialDialog();
    
    SerialPortSettings getCom1Settings() const;
    SerialPortSettings getCom2Settings() const;
    
    void setCom1Settings(const SerialPortSettings &settings);
    void setCom2Settings(const SerialPortSettings &settings);

private slots:
    void onCom1ModeChanged();
    void onCom2ModeChanged();
    void onBrowseLogFile1();
    void onBrowseLogFile2();
    void onApply();

private:
    void setupUi();
    QGroupBox* createPortGroup(const QString &title, int portIndex);
    void updatePortWidgets(int portIndex);
    
    // COM1 widgets
    QButtonGroup *com1_mode_group;
    QRadioButton *com1_disabled_radio;
    QRadioButton *com1_logfile_radio;
    QRadioButton *com1_tcpmodem_radio;
    QRadioButton *com1_physical_radio;
    QLineEdit *com1_logfile_edit;
    QPushButton *com1_browse_btn;
    QComboBox *com1_device_combo;
    
    // COM2 widgets
    QButtonGroup *com2_mode_group;
    QRadioButton *com2_disabled_radio;
    QRadioButton *com2_logfile_radio;
    QRadioButton *com2_tcpmodem_radio;
    QRadioButton *com2_physical_radio;
    QLineEdit *com2_logfile_edit;
    QPushButton *com2_browse_btn;
    QComboBox *com2_device_combo;
    
    QPushButton *apply_button;
    QPushButton *cancel_button;
};

#endif /* SERIAL_DIALOG_H */
