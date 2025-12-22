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

#ifndef CONFIG_SELECTOR_DIALOG_H
#define CONFIG_SELECTOR_DIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <QStringList>

/**
 * Dialog for selecting, creating, editing, and deleting machine configurations.
 * Shown at application startup before the emulator is initialized.
 * 
 * Configurations are stored in:
 *   configs/<name>.cfg   - Configuration file
 *   machines/<name>/     - Machine data (cmos.ram, hostfs/, hd4.hdf, etc.)
 */
class ConfigSelectorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConfigSelectorDialog(QWidget *parent = nullptr);
    virtual ~ConfigSelectorDialog();

    /**
     * Get the path to the selected configuration file.
     * Only valid after the dialog has been accepted.
     * @return Full path to the .cfg file
     */
    QString getSelectedConfigPath() const;

    /**
     * Get the name of the selected configuration.
     * @return Configuration name
     */
    QString getSelectedConfigName() const;

private slots:
    void onNewClicked();
    void onEditClicked();
    void onDeleteClicked();
    void onCloneClicked();
    void onStartClicked();
    void onListDoubleClicked(QListWidgetItem *item);
    void onSelectionChanged();

private:
    void setupUI();
    void refreshConfigList();
    void ensureDirectoriesExist();
    QString getConfigsDirectory() const;
    QString getMachinesDirectory() const;
    QStringList findConfigFiles() const;
    QString readConfigName(const QString &filePath) const;
    bool createDefaultConfigIfNeeded();
    QString sanitizeName(const QString &name) const;
    bool isNameUnique(const QString &name) const;
    bool createMachineDirectory(const QString &machineName);
    bool createBlankHardDisc(const QString &path, int sizeMB);
    void copyDirectory(const QString &srcPath, const QString &dstPath);

    QListWidget *configList;
    QPushButton *newButton;
    QPushButton *editButton;
    QPushButton *deleteButton;
    QPushButton *cloneButton;
    QPushButton *startButton;

    QString selectedConfigPath;
    QString selectedConfigName;
};

/**
 * Dialog for editing machine configuration settings.
 */
class MachineEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MachineEditDialog(const QString &configPath, QWidget *parent = nullptr);
    virtual ~MachineEditDialog();

    QString getNewName() const { return newName; }
    bool wasRenamed() const { return renamed; }

private slots:
    void onOkClicked();
    void onCancelClicked();
    void onHD4Clicked();
    void onHD5Clicked();
    void onNetworkChanged(int index);

private:
    void setupUI();
    void loadSettings();
    void saveSettings();
    void updateHDStatus();
    void populateRomList();
    QString getHDSizeString(const QString &path) const;
    QString getRomsDirectory() const;

    QString configPath;
    QString originalName;
    QString newName;
    bool renamed;

    QLineEdit *nameEdit;
    QComboBox *romCombo;
    QComboBox *modelCombo;
    QComboBox *memCombo;
    QComboBox *vramCombo;
    QSlider *refreshSlider;
    QLabel *refreshLabel;
    QComboBox *networkCombo;
    QLineEdit *bridgeEdit;
    QLabel *bridgeLabel;
#if defined(Q_OS_LINUX)
    QLineEdit *tunnelEdit;
    QLabel *tunnelLabel;
#endif
    QLabel *hd4StatusLabel;
    QLabel *hd5StatusLabel;
    QPushButton *hd4Button;
    QPushButton *hd5Button;
    QPushButton *okButton;
    QPushButton *cancelButton;
};

#endif // CONFIG_SELECTOR_DIALOG_H
