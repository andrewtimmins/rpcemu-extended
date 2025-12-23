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

#ifdef RPCEMU_VNC

#include "vnc_dialog.h"
#include "vnc_server.h"

#include <QMessageBox>
VncDialog::VncDialog(VncServer *server, const QString &currentPassword, QWidget *parent)
    : QDialog(parent)
    , vncServer(server)
{
    setWindowTitle(tr("VNC Server Settings"));
    setMinimumWidth(350);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Enable/disable group
    QGroupBox *serverGroup = new QGroupBox(tr("VNC Server"), this);
    QVBoxLayout *serverLayout = new QVBoxLayout(serverGroup);

    enableCheckBox = new QCheckBox(tr("Enable VNC Server"), this);
    enableCheckBox->setChecked(vncServer && vncServer->isRunning());
    serverLayout->addWidget(enableCheckBox);

    // Port configuration
    QHBoxLayout *portLayout = new QHBoxLayout();
    QLabel *portLabel = new QLabel(tr("Port:"), this);
    portSpinBox = new QSpinBox(this);
    portSpinBox->setRange(1024, 65535);
    portSpinBox->setValue(vncServer ? vncServer->getPort() : 5900);
    if (portSpinBox->value() == 0) {
        portSpinBox->setValue(5900); // Default
    }
    portSpinBox->setEnabled(!enableCheckBox->isChecked());
    portLayout->addWidget(portLabel);
    portLayout->addWidget(portSpinBox);
    portLayout->addStretch();
    serverLayout->addLayout(portLayout);

    // Password configuration
    QHBoxLayout *passwordLayout = new QHBoxLayout();
    QLabel *passwordLabel = new QLabel(tr("Password:"), this);
    passwordEdit = new QLineEdit(this);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(tr("Leave empty for no authentication"));
    passwordEdit->setText(currentPassword);
    passwordEdit->setMaxLength(63);  // VNC password limit
    passwordEdit->setEnabled(!enableCheckBox->isChecked());
    passwordLayout->addWidget(passwordLabel);
    passwordLayout->addWidget(passwordEdit);
    serverLayout->addLayout(passwordLayout);

    mainLayout->addWidget(serverGroup);

    // Status group
    QGroupBox *statusGroup = new QGroupBox(tr("Status"), this);
    QFormLayout *statusLayout = new QFormLayout(statusGroup);

    statusLabel = new QLabel(this);
    clientsLabel = new QLabel(this);
    statusLayout->addRow(tr("Server:"), statusLabel);
    statusLayout->addRow(tr("Clients:"), clientsLabel);

    mainLayout->addWidget(statusGroup);

    // Info box
    QLabel *infoLabel = new QLabel(tr(
        "<b>Usage:</b> Connect with any VNC client to<br>"
        "<code>localhost:%1</code> (or your machine's IP)<br><br>"
        "<b>Note:</b> VNC traffic is unencrypted. For remote access<br>"
        "over the internet, use SSH tunneling or a VPN.").arg(portSpinBox->value()), this);
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);

    mainLayout->addStretch();

    // Buttons
    buttonBox = new QDialogButtonBox(this);
    applyButton = buttonBox->addButton(QDialogButtonBox::Apply);
    buttonBox->addButton(QDialogButtonBox::Close);

    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(applyButton, &QPushButton::clicked, this, &VncDialog::onApply);

    mainLayout->addWidget(buttonBox);

    // Connections
    connect(enableCheckBox, &QCheckBox::toggled, this, &VncDialog::onEnableToggled);

    if (vncServer) {
        connect(vncServer, &VncServer::statusChanged,
                this, &VncDialog::onServerStatusChanged);
        connect(vncServer, &VncServer::clientConnected,
                this, &VncDialog::onClientConnected);
        connect(vncServer, &VncServer::clientDisconnected,
                this, &VncDialog::onClientDisconnected);
    }

    updateStatus();
}

void VncDialog::onEnableToggled(bool checked)
{
    portSpinBox->setEnabled(!checked);
    passwordEdit->setEnabled(!checked);
}

QString VncDialog::getPassword() const
{
    return passwordEdit->text();
}

void VncDialog::onApply()
{
    if (!vncServer) {
        QMessageBox::warning(this, tr("VNC Error"),
            tr("VNC server not available."));
        return;
    }

    if (enableCheckBox->isChecked()) {
        if (!vncServer->isRunning()) {
            int port = portSpinBox->value();
            QString password = passwordEdit->text();
            if (!vncServer->start(port, password)) {
                QMessageBox::warning(this, tr("VNC Error"),
                    tr("Failed to start VNC server on port %1.\n"
                       "The port may be in use.").arg(port));
                enableCheckBox->setChecked(false);
            }
        }
    } else {
        if (vncServer->isRunning()) {
            vncServer->stop();
        }
    }

    updateStatus();
}

void VncDialog::onServerStatusChanged(bool running, int port)
{
    Q_UNUSED(running);
    Q_UNUSED(port);
    updateStatus();
}

void VncDialog::onClientConnected(const QString &address)
{
    Q_UNUSED(address);
    updateStatus();
}

void VncDialog::onClientDisconnected(const QString &address)
{
    Q_UNUSED(address);
    updateStatus();
}

void VncDialog::updateStatus()
{
    if (!vncServer) {
        statusLabel->setText(tr("<span style='color: gray;'>Not available</span>"));
        clientsLabel->setText("-");
        return;
    }

    if (vncServer->isRunning()) {
        statusLabel->setText(tr("<span style='color: green;'>Running on port %1</span>")
                            .arg(vncServer->getPort()));
        int clients = vncServer->getClientCount();
        if (clients == 0) {
            clientsLabel->setText(tr("None connected"));
        } else {
            clientsLabel->setText(tr("%1 connected").arg(clients));
        }
    } else {
        statusLabel->setText(tr("<span style='color: red;'>Stopped</span>"));
        clientsLabel->setText("-");
    }

    // Update checkbox state
    enableCheckBox->setChecked(vncServer->isRunning());
    portSpinBox->setEnabled(!vncServer->isRunning());
    passwordEdit->setEnabled(!vncServer->isRunning());
}

#endif // RPCEMU_VNC