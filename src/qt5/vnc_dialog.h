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

#ifndef VNC_DIALOG_H
#define VNC_DIALOG_H

#ifdef RPCEMU_VNC

#include <QDialog>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>

class VncServer;

/**
 * Dialog for configuring the built-in VNC server
 */
class VncDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VncDialog(VncServer *server, const QString &currentPassword = QString(),
                       QWidget *parent = nullptr);
    
    /**
     * Get the password entered by the user
     */
    QString getPassword() const;

private slots:
    void onEnableToggled(bool checked);
    void onApply();
    void onServerStatusChanged(bool running, int port);
    void onClientConnected(const QString &address);
    void onClientDisconnected(const QString &address);

private:
    void updateStatus();

    VncServer *vncServer;

    QCheckBox *enableCheckBox;
    QSpinBox *portSpinBox;
    QLineEdit *passwordEdit;
    QLabel *statusLabel;
    QLabel *clientsLabel;
    QPushButton *applyButton;
    QDialogButtonBox *buttonBox;
};

#endif // RPCEMU_VNC

#endif // VNC_DIALOG_H