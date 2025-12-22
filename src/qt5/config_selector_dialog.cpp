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

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QSettings>
#include <QInputDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QUrl>

#include "config_selector_dialog.h"

extern "C" {
#include "rpcemu.h"
}

// ============================================================================
// ConfigSelectorDialog Implementation
// ============================================================================

ConfigSelectorDialog::ConfigSelectorDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUI();
    
    // Ensure directories exist
    ensureDirectoriesExist();
    
    // Create default config if no configs exist
    createDefaultConfigIfNeeded();
    
    // Populate the list
    refreshConfigList();
    
    // Select first item if available
    if (configList->count() > 0) {
        configList->setCurrentRow(0);
    }
    
    // Update button states
    onSelectionChanged();
}

ConfigSelectorDialog::~ConfigSelectorDialog()
{
}

void ConfigSelectorDialog::setupUI()
{
    setWindowTitle("RPCEmu - Select Machine");
    setMinimumSize(500, 400);
    
    // Create the list widget
    configList = new QListWidget(this);
    configList->setSelectionMode(QAbstractItemView::SingleSelection);
    
    // Create buttons
    newButton = new QPushButton("New...", this);
    editButton = new QPushButton("Edit...", this);
    deleteButton = new QPushButton("Delete", this);
    cloneButton = new QPushButton("Clone...", this);
    startButton = new QPushButton("Start", this);
    
    // Make Start button the default and prominent
    startButton->setDefault(true);
    startButton->setMinimumHeight(40);
    
    // Button layout (right column)
    QVBoxLayout *buttonLayout = new QVBoxLayout();
    buttonLayout->addWidget(newButton);
    buttonLayout->addWidget(editButton);
    buttonLayout->addWidget(cloneButton);
    buttonLayout->addWidget(deleteButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(startButton);
    
    // Main layout
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->addWidget(configList, 1);  // List takes most space
    mainLayout->addLayout(buttonLayout);
    
    // Connect signals
    connect(newButton, &QPushButton::clicked, this, &ConfigSelectorDialog::onNewClicked);
    connect(editButton, &QPushButton::clicked, this, &ConfigSelectorDialog::onEditClicked);
    connect(deleteButton, &QPushButton::clicked, this, &ConfigSelectorDialog::onDeleteClicked);
    connect(cloneButton, &QPushButton::clicked, this, &ConfigSelectorDialog::onCloneClicked);
    connect(startButton, &QPushButton::clicked, this, &ConfigSelectorDialog::onStartClicked);
    connect(configList, &QListWidget::itemDoubleClicked, this, &ConfigSelectorDialog::onListDoubleClicked);
    connect(configList, &QListWidget::itemSelectionChanged, this, &ConfigSelectorDialog::onSelectionChanged);
}

void ConfigSelectorDialog::ensureDirectoriesExist()
{
    QDir dir;
    dir.mkpath(getConfigsDirectory());
    dir.mkpath(getMachinesDirectory());
}

QString ConfigSelectorDialog::getConfigsDirectory() const
{
    return QCoreApplication::applicationDirPath() + "/configs/";
}

QString ConfigSelectorDialog::getMachinesDirectory() const
{
    return QCoreApplication::applicationDirPath() + "/machines/";
}

QStringList ConfigSelectorDialog::findConfigFiles() const
{
    QDir dir(getConfigsDirectory());
    QStringList filters;
    filters << "*.cfg";
    
    return dir.entryList(filters, QDir::Files, QDir::Name);
}

QString ConfigSelectorDialog::readConfigName(const QString &filePath) const
{
    QSettings settings(filePath, QSettings::IniFormat);
    QString name = settings.value("name", "").toString();
    
    if (name.isEmpty()) {
        // Use filename without extension as fallback
        QFileInfo fi(filePath);
        name = fi.baseName();
    }
    
    return name;
}

QString ConfigSelectorDialog::sanitizeName(const QString &name) const
{
    QString sanitized = name;
    // Remove invalid filename characters
    sanitized.replace(QRegExp("[<>:\"/\\\\|?*]"), "_");
    // Trim whitespace
    sanitized = sanitized.trimmed();
    return sanitized;
}

bool ConfigSelectorDialog::isNameUnique(const QString &name) const
{
    QString sanitized = sanitizeName(name);
    QString configPath = getConfigsDirectory() + sanitized + ".cfg";
    QString machinePath = getMachinesDirectory() + sanitized;
    
    return !QFile::exists(configPath) && !QDir(machinePath).exists();
}

bool ConfigSelectorDialog::createMachineDirectory(const QString &machineName)
{
    QString machineDir = getMachinesDirectory() + machineName;
    QDir dir;
    
    if (!dir.mkpath(machineDir)) {
        return false;
    }
    
    // Create hostfs subdirectory
    if (!dir.mkpath(machineDir + "/hostfs")) {
        return false;
    }
    
    return true;
}

bool ConfigSelectorDialog::createBlankHardDisc(const QString &path, int sizeMB)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    
    // Create sparse file of specified size
    qint64 sizeBytes = (qint64)sizeMB * 1024 * 1024;
    if (!file.resize(sizeBytes)) {
        file.close();
        return false;
    }
    
    file.close();
    return true;
}

void ConfigSelectorDialog::copyDirectory(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    QDir dstDir(dstPath);
    
    if (!dstDir.exists()) {
        dstDir.mkpath(".");
    }
    
    // Copy files
    QStringList files = srcDir.entryList(QDir::Files);
    for (const QString &file : files) {
        QString srcFile = srcPath + "/" + file;
        QString dstFile = dstPath + "/" + file;
        QFile::copy(srcFile, dstFile);
    }
    
    // Recursively copy subdirectories
    QStringList dirs = srcDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &dir : dirs) {
        copyDirectory(srcPath + "/" + dir, dstPath + "/" + dir);
    }
}

bool ConfigSelectorDialog::createDefaultConfigIfNeeded()
{
    QStringList configs = findConfigFiles();
    
    if (configs.isEmpty()) {
        QString name = "Default";
        QString configPath = getConfigsDirectory() + name + ".cfg";
        
        // Create config file
        QSettings settings(configPath, QSettings::IniFormat);
        
        settings.setValue("name", name);
        settings.setValue("model", "RPCSA");
        settings.setValue("mem_size", "64");
        settings.setValue("vram_size", "2");
        settings.setValue("sound_enabled", 1);
        settings.setValue("refresh_rate", 60);
        settings.setValue("cdrom_enabled", 1);
        settings.setValue("cdrom_type", 0);
        settings.setValue("cdrom_iso", "");
        settings.setValue("mouse_following", 1);
        settings.setValue("mouse_twobutton", 0);
        settings.setValue("network_type", "nat");
        settings.setValue("cpu_idle", 0);
        settings.setValue("show_fullscreen_message", 1);
        
        settings.sync();
        
        // Create machine directory (no HD image - user can create via Edit)
        createMachineDirectory(name);
        
        return true;
    }
    
    return false;
}

void ConfigSelectorDialog::refreshConfigList()
{
    configList->clear();
    
    QStringList configFiles = findConfigFiles();
    QString configDir = getConfigsDirectory();
    
    for (const QString &filename : configFiles) {
        QString fullPath = configDir + filename;
        QString name = readConfigName(fullPath);
        
        QListWidgetItem *item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, fullPath);  // Store full path in item data
        configList->addItem(item);
    }
}

void ConfigSelectorDialog::onNewClicked()
{
    bool ok;
    QString name = QInputDialog::getText(this, "New Machine",
                                          "Machine name:",
                                          QLineEdit::Normal,
                                          "New Machine", &ok);
    
    if (!ok || name.isEmpty()) {
        return;
    }
    
    QString sanitized = sanitizeName(name);
    
    if (!isNameUnique(sanitized)) {
        QMessageBox::warning(this, "Name Already Exists",
                            QString("A machine named '%1' already exists.\n"
                                   "Please choose a different name.").arg(sanitized));
        return;
    }
    
    // Create config file
    QString configPath = getConfigsDirectory() + sanitized + ".cfg";
    QSettings settings(configPath, QSettings::IniFormat);
    
    settings.setValue("name", sanitized);
    settings.setValue("model", "RPCSA");
    settings.setValue("mem_size", "64");
    settings.setValue("vram_size", "2");
    settings.setValue("sound_enabled", 1);
    settings.setValue("refresh_rate", 60);
    settings.setValue("cdrom_enabled", 1);
    settings.setValue("cdrom_type", 0);
    settings.setValue("cdrom_iso", "");
    settings.setValue("mouse_following", 1);
    settings.setValue("mouse_twobutton", 0);
    settings.setValue("network_type", "nat");
    settings.setValue("cpu_idle", 0);
    settings.setValue("show_fullscreen_message", 1);
    
    settings.sync();
    
    // Create machine directory
    if (!createMachineDirectory(sanitized)) {
        QMessageBox::warning(this, "Error",
                            "Failed to create machine directory.");
        QFile::remove(configPath);
        return;
    }
    
    // Refresh list and select new item
    refreshConfigList();
    
    // Select the newly created config
    for (int i = 0; i < configList->count(); i++) {
        if (configList->item(i)->data(Qt::UserRole).toString() == configPath) {
            configList->setCurrentRow(i);
            break;
        }
    }
    
    // Open edit dialog for the new config
    onEditClicked();
}

void ConfigSelectorDialog::onEditClicked()
{
    QListWidgetItem *currentItem = configList->currentItem();
    if (!currentItem) {
        return;
    }
    
    QString filePath = currentItem->data(Qt::UserRole).toString();
    
    MachineEditDialog dialog(filePath, this);
    if (dialog.exec() == QDialog::Accepted) {
        if (dialog.wasRenamed()) {
            // Need to rename config file and machine directory
            QString oldName = currentItem->text();
            QString newName = dialog.getNewName();
            QString sanitizedNew = sanitizeName(newName);
            
            QString oldConfigPath = filePath;
            QString newConfigPath = getConfigsDirectory() + sanitizedNew + ".cfg";
            QString oldMachineDir = getMachinesDirectory() + oldName;
            QString newMachineDir = getMachinesDirectory() + sanitizedNew;
            
            // Rename machine directory
            if (QDir(oldMachineDir).exists()) {
                QDir().rename(oldMachineDir, newMachineDir);
            }
            
            // Rename config file
            if (oldConfigPath != newConfigPath) {
                QFile::rename(oldConfigPath, newConfigPath);
            }
        }
        
        refreshConfigList();
    }
}

void ConfigSelectorDialog::onDeleteClicked()
{
    QListWidgetItem *currentItem = configList->currentItem();
    if (!currentItem) {
        return;
    }
    
    QString name = currentItem->text();
    QString filePath = currentItem->data(Qt::UserRole).toString();
    
    // Don't allow deleting the last config
    if (configList->count() <= 1) {
        QMessageBox::warning(this, "Cannot Delete",
                            "Cannot delete the last machine.\n"
                            "At least one machine must exist.");
        return;
    }
    
    int ret = QMessageBox::question(this, "Delete Machine",
                                    QString("Are you sure you want to delete '%1'?\n\n"
                                           "This will delete the configuration file AND all machine data\n"
                                           "(hard disc images, CMOS, HostFS files, etc.)\n\n"
                                           "This cannot be undone!").arg(name),
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No);
    
    if (ret == QMessageBox::Yes) {
        // Delete config file
        QFile::remove(filePath);
        
        // Delete machine directory recursively
        QString machineDir = getMachinesDirectory() + name;
        QDir dir(machineDir);
        if (dir.exists()) {
            dir.removeRecursively();
        }
        
        refreshConfigList();
        
        if (configList->count() > 0) {
            configList->setCurrentRow(0);
        }
    }
}

void ConfigSelectorDialog::onCloneClicked()
{
    QListWidgetItem *currentItem = configList->currentItem();
    if (!currentItem) {
        return;
    }
    
    QString sourcePath = currentItem->data(Qt::UserRole).toString();
    QString sourceName = currentItem->text();
    
    bool ok;
    QString newName = QInputDialog::getText(this, "Clone Machine",
                                             "New machine name:",
                                             QLineEdit::Normal,
                                             sourceName + " (Copy)", &ok);
    
    if (!ok || newName.isEmpty()) {
        return;
    }
    
    QString sanitized = sanitizeName(newName);
    
    if (!isNameUnique(sanitized)) {
        QMessageBox::warning(this, "Name Already Exists",
                            QString("A machine named '%1' already exists.\n"
                                   "Please choose a different name.").arg(sanitized));
        return;
    }
    
    QString newConfigPath = getConfigsDirectory() + sanitized + ".cfg";
    
    // Copy the config file
    QFile::copy(sourcePath, newConfigPath);
    
    // Update the name in the new config
    QSettings settings(newConfigPath, QSettings::IniFormat);
    settings.setValue("name", sanitized);
    settings.sync();
    
    // Copy the machine directory (full clone including HD images)
    QString srcMachineDir = getMachinesDirectory() + sourceName;
    QString dstMachineDir = getMachinesDirectory() + sanitized;
    
    if (QDir(srcMachineDir).exists()) {
        copyDirectory(srcMachineDir, dstMachineDir);
    } else {
        // Source machine dir doesn't exist, create empty one
        createMachineDirectory(sanitized);
    }
    
    refreshConfigList();
    
    // Select the cloned config
    for (int i = 0; i < configList->count(); i++) {
        if (configList->item(i)->data(Qt::UserRole).toString() == newConfigPath) {
            configList->setCurrentRow(i);
            break;
        }
    }
}

void ConfigSelectorDialog::onStartClicked()
{
    QListWidgetItem *currentItem = configList->currentItem();
    if (!currentItem) {
        return;
    }
    
    selectedConfigPath = currentItem->data(Qt::UserRole).toString();
    selectedConfigName = currentItem->text();
    
    accept();
}

void ConfigSelectorDialog::onListDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    onStartClicked();
}

void ConfigSelectorDialog::onSelectionChanged()
{
    bool hasSelection = (configList->currentItem() != nullptr);
    
    editButton->setEnabled(hasSelection);
    deleteButton->setEnabled(hasSelection && configList->count() > 1);
    cloneButton->setEnabled(hasSelection);
    startButton->setEnabled(hasSelection);
}

QString ConfigSelectorDialog::getSelectedConfigPath() const
{
    return selectedConfigPath;
}

QString ConfigSelectorDialog::getSelectedConfigName() const
{
    return selectedConfigName;
}

// ============================================================================
// MachineEditDialog Implementation
// ============================================================================

MachineEditDialog::MachineEditDialog(const QString &configPath, QWidget *parent)
    : QDialog(parent)
    , configPath(configPath)
    , renamed(false)
{
    setupUI();
    loadSettings();
    updateHDStatus();
}

MachineEditDialog::~MachineEditDialog()
{
}

void MachineEditDialog::setupUI()
{
    setWindowTitle("Edit Machine");
    setMinimumWidth(450);
    
    // Name
    nameEdit = new QLineEdit(this);
    
    // ROM combo - populated dynamically
    romCombo = new QComboBox(this);
    populateRomList();
    
    // Model combo
    modelCombo = new QComboBox(this);
    modelCombo->addItem("RiscPC ARM610", "RPCARM610");
    modelCombo->addItem("RiscPC ARM710", "RPCARM710");
    modelCombo->addItem("RiscPC StrongARM", "RPCSA");
    modelCombo->addItem("A7000", "A7000");
    modelCombo->addItem("A7000+", "A7000+");
    modelCombo->addItem("RiscPC ARM810", "RPCARM810");
    modelCombo->addItem("Phoebe (RiscPC2)", "Phoebe");
    
    // Memory combo
    memCombo = new QComboBox(this);
    memCombo->addItem("4 MB", 4);
    memCombo->addItem("8 MB", 8);
    memCombo->addItem("16 MB", 16);
    memCombo->addItem("32 MB", 32);
    memCombo->addItem("64 MB", 64);
    memCombo->addItem("128 MB", 128);
    memCombo->addItem("256 MB", 256);
    
    // VRAM combo
    vramCombo = new QComboBox(this);
    vramCombo->addItem("None", 0);
    vramCombo->addItem("2 MB", 2);
    
    // Refresh rate slider
    refreshSlider = new QSlider(Qt::Horizontal, this);
    refreshSlider->setRange(20, 100);
    refreshSlider->setSingleStep(1);
    refreshSlider->setPageStep(10);
    refreshSlider->setValue(60);
    refreshLabel = new QLabel("60 Hz", this);
    refreshLabel->setMinimumWidth(50);
    connect(refreshSlider, &QSlider::valueChanged, this, [this](int value) {
        refreshLabel->setText(QString("%1 Hz").arg(value));
    });
    
    // Network combo
    networkCombo = new QComboBox(this);
    networkCombo->addItem("Off", "off");
    networkCombo->addItem("NAT", "nat");
    networkCombo->addItem("Ethernet Bridging", "bridging");
#if defined(Q_OS_LINUX)
    networkCombo->addItem("IP Tunnelling", "tunnelling");
#endif
    
    // Bridge name field (for Ethernet Bridging)
    bridgeLabel = new QLabel("Bridge Name:", this);
    bridgeEdit = new QLineEdit(this);
    bridgeEdit->setText("rpcemu");
    bridgeEdit->setMinimumWidth(150);
    bridgeLabel->setEnabled(false);
    bridgeEdit->setEnabled(false);
    
#if defined(Q_OS_LINUX)
    // IP Tunnelling field (Linux only)
    tunnelLabel = new QLabel("IP Address:", this);
    tunnelEdit = new QLineEdit(this);
    tunnelEdit->setText("172.31.0.1");
    tunnelEdit->setMinimumWidth(150);
    tunnelLabel->setEnabled(false);
    tunnelEdit->setEnabled(false);
#endif
    
    // Hard disc group
    QGroupBox *hdGroup = new QGroupBox("Hard Discs", this);
    QGridLayout *hdLayout = new QGridLayout(hdGroup);
    
    hd4StatusLabel = new QLabel("Not created", this);
    hd5StatusLabel = new QLabel("Not created", this);
    hd4Button = new QPushButton("HardDisc 4...", this);
    hd5Button = new QPushButton("HardDisc 5...", this);
    
    hdLayout->addWidget(hd4Button, 0, 0);
    hdLayout->addWidget(hd4StatusLabel, 0, 1);
    hdLayout->addWidget(hd5Button, 1, 0);
    hdLayout->addWidget(hd5StatusLabel, 1, 1);
    
    // Form layout for main settings
    QFormLayout *formLayout = new QFormLayout();
    formLayout->addRow("Name:", nameEdit);
    formLayout->addRow("ROM:", romCombo);
    formLayout->addRow("Model:", modelCombo);
    formLayout->addRow("RAM:", memCombo);
    formLayout->addRow("VRAM:", vramCombo);
    
    // Refresh rate with slider and label
    QHBoxLayout *refreshLayout = new QHBoxLayout();
    refreshLayout->addWidget(refreshSlider);
    refreshLayout->addWidget(refreshLabel);
    formLayout->addRow("Refresh Rate:", refreshLayout);
    
    formLayout->addRow("Network:", networkCombo);
    formLayout->addRow(bridgeLabel, bridgeEdit);
#if defined(Q_OS_LINUX)
    formLayout->addRow(tunnelLabel, tunnelEdit);
#endif
    
    // Button box
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    
    // Main layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(hdGroup);
    mainLayout->addStretch();
    mainLayout->addWidget(buttonBox);
    
    // Connect signals
    connect(buttonBox, &QDialogButtonBox::accepted, this, &MachineEditDialog::onOkClicked);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &MachineEditDialog::onCancelClicked);
    connect(hd4Button, &QPushButton::clicked, this, &MachineEditDialog::onHD4Clicked);
    connect(hd5Button, &QPushButton::clicked, this, &MachineEditDialog::onHD5Clicked);
    connect(networkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MachineEditDialog::onNetworkChanged);
}

void MachineEditDialog::loadSettings()
{
    QSettings settings(configPath, QSettings::IniFormat);
    
    originalName = settings.value("name", "").toString();
    if (originalName.isEmpty()) {
        QFileInfo fi(configPath);
        originalName = fi.baseName();
    }
    nameEdit->setText(originalName);
    
    // ROM directory
    QString romDir = settings.value("rom_dir", "").toString();
    for (int i = 0; i < romCombo->count(); i++) {
        if (romCombo->itemData(i).toString() == romDir) {
            romCombo->setCurrentIndex(i);
            break;
        }
    }
    
    // Model
    QString model = settings.value("model", "RPCSA").toString();
    for (int i = 0; i < modelCombo->count(); i++) {
        if (modelCombo->itemData(i).toString() == model) {
            modelCombo->setCurrentIndex(i);
            break;
        }
    }
    
    // Memory
    int memSize = settings.value("mem_size", 64).toInt();
    for (int i = 0; i < memCombo->count(); i++) {
        if (memCombo->itemData(i).toInt() == memSize) {
            memCombo->setCurrentIndex(i);
            break;
        }
    }
    
    // VRAM
    int vramSize = settings.value("vram_size", 2).toInt();
    for (int i = 0; i < vramCombo->count(); i++) {
        if (vramCombo->itemData(i).toInt() == vramSize) {
            vramCombo->setCurrentIndex(i);
            break;
        }
    }
    
    // Refresh rate
    int refreshRate = settings.value("refresh_rate", 60).toInt();
    refreshRate = qBound(20, refreshRate, 100);
    refreshSlider->setValue(refreshRate);
    refreshLabel->setText(QString("%1 Hz").arg(refreshRate));
    
    // Network type
    QString networkType = settings.value("network_type", "off").toString();
    for (int i = 0; i < networkCombo->count(); i++) {
        if (networkCombo->itemData(i).toString() == networkType) {
            networkCombo->setCurrentIndex(i);
            break;
        }
    }
    
    // Bridge name
    QString bridgeName = settings.value("bridge_name", "rpcemu").toString();
    if (!bridgeName.isEmpty()) {
        bridgeEdit->setText(bridgeName);
    }
    
#if defined(Q_OS_LINUX)
    // IP Tunnelling address
    QString ipAddress = settings.value("ip_address", "172.31.0.1").toString();
    if (!ipAddress.isEmpty()) {
        tunnelEdit->setText(ipAddress);
    }
#endif
    
    // Update enabled state of bridge/tunnel fields
    onNetworkChanged(networkCombo->currentIndex());
}

void MachineEditDialog::saveSettings()
{
    QSettings settings(configPath, QSettings::IniFormat);
    
    newName = nameEdit->text().trimmed();
    if (newName.isEmpty()) {
        newName = originalName;
    }
    
    settings.setValue("name", newName);
    settings.setValue("rom_dir", romCombo->currentData().toString());
    settings.setValue("model", modelCombo->currentData().toString());
    settings.setValue("mem_size", memCombo->currentData().toInt());
    settings.setValue("vram_size", vramCombo->currentData().toInt());
    settings.setValue("refresh_rate", refreshSlider->value());
    settings.setValue("network_type", networkCombo->currentData().toString());
    settings.setValue("bridge_name", bridgeEdit->text());
#if defined(Q_OS_LINUX)
    settings.setValue("ip_address", tunnelEdit->text());
#endif
    
    settings.sync();
    
    renamed = (newName != originalName);
}

QString MachineEditDialog::getRomsDirectory() const
{
    // ROMs are in the working directory, not the application directory
    return QDir::currentPath() + "/roms/";
}

void MachineEditDialog::populateRomList()
{
    romCombo->clear();
    
    // Scan roms directory for ROM files
    QDir romsDir(getRomsDirectory());
    if (!romsDir.exists()) {
        romCombo->addItem("(No roms/ directory found)", "");
        return;
    }
    
    // Get all files in the roms directory (excluding .txt and hidden files)
    QStringList files = romsDir.entryList(QDir::Files, QDir::Name);
    
    for (const QString &file : files) {
        // Skip text files and hidden files
        if (file.endsWith(".txt", Qt::CaseInsensitive) || file.startsWith(".")) {
            continue;
        }
        // Add ROM file to the list (use filename as both display and data)
        romCombo->addItem(file, file);
    }
    
    // If no ROMs found, show a message
    if (romCombo->count() == 0) {
        romCombo->addItem("(No ROM files found in roms/)", "");
    }
}

QString MachineEditDialog::getHDSizeString(const QString &path) const
{
    QFileInfo fi(path);
    if (!fi.exists()) {
        return "Not created";
    }
    
    qint64 sizeBytes = fi.size();
    if (sizeBytes == 0) {
        return "Empty (0 bytes)";
    }
    
    double sizeMB = sizeBytes / (1024.0 * 1024.0);
    if (sizeMB < 1024) {
        return QString("%1 MB").arg(sizeMB, 0, 'f', 1);
    } else {
        double sizeGB = sizeMB / 1024.0;
        return QString("%1 GB").arg(sizeGB, 0, 'f', 2);
    }
}

void MachineEditDialog::updateHDStatus()
{
    QString machineDir = QCoreApplication::applicationDirPath() + "/machines/" + originalName + "/";
    
    hd4StatusLabel->setText(getHDSizeString(machineDir + "hd4.hdf"));
    hd5StatusLabel->setText(getHDSizeString(machineDir + "hd5.hdf"));
}

void MachineEditDialog::onHD4Clicked()
{
    QString machineDir = QCoreApplication::applicationDirPath() + "/machines/" + originalName + "/";
    QString hdPath = machineDir + "hd4.hdf";
    
    QFileInfo fi(hdPath);
    
    QStringList options;
    if (fi.exists()) {
        options << "View in file manager" << "Delete" << "Cancel";
    } else {
        options << "Create 256 MB" << "Create 512 MB" << "Create 1 GB" << "Create 2 GB" << "Cancel";
    }
    
    bool ok;
    QString choice = QInputDialog::getItem(this, "HardDisc 4", 
                                           fi.exists() ? "HardDisc 4 exists. Choose action:" : "Create HardDisc 4:",
                                           options, 0, false, &ok);
    
    if (!ok || choice == "Cancel") {
        return;
    }
    
    if (choice == "Delete") {
        int ret = QMessageBox::question(this, "Delete HardDisc",
                                        "Are you sure you want to delete HardDisc 4?\n\n"
                                        "All data will be lost!",
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            QFile::remove(hdPath);
        }
    } else if (choice == "View in file manager") {
        // Open the machine directory
        QDesktopServices::openUrl(QUrl::fromLocalFile(machineDir));
    } else if (choice.startsWith("Create")) {
        int sizeMB = 512;
        if (choice.contains("256")) sizeMB = 256;
        else if (choice.contains("512")) sizeMB = 512;
        else if (choice.contains("1 GB")) sizeMB = 1024;
        else if (choice.contains("2 GB")) sizeMB = 2048;
        
        // Ensure machine directory exists
        QDir().mkpath(machineDir);
        
        // Create the file
        QFile file(hdPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.resize((qint64)sizeMB * 1024 * 1024);
            file.close();
        }
    }
    
    updateHDStatus();
}

void MachineEditDialog::onHD5Clicked()
{
    QString machineDir = QCoreApplication::applicationDirPath() + "/machines/" + originalName + "/";
    QString hdPath = machineDir + "hd5.hdf";
    
    QFileInfo fi(hdPath);
    
    QStringList options;
    if (fi.exists()) {
        options << "View in file manager" << "Delete" << "Cancel";
    } else {
        options << "Create 256 MB" << "Create 512 MB" << "Create 1 GB" << "Create 2 GB" << "Cancel";
    }
    
    bool ok;
    QString choice = QInputDialog::getItem(this, "HardDisc 5", 
                                           fi.exists() ? "HardDisc 5 exists. Choose action:" : "Create HardDisc 5:",
                                           options, 0, false, &ok);
    
    if (!ok || choice == "Cancel") {
        return;
    }
    
    if (choice == "Delete") {
        int ret = QMessageBox::question(this, "Delete HardDisc",
                                        "Are you sure you want to delete HardDisc 5?\n\n"
                                        "All data will be lost!",
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            QFile::remove(hdPath);
        }
    } else if (choice == "View in file manager") {
        QDesktopServices::openUrl(QUrl::fromLocalFile(machineDir));
    } else if (choice.startsWith("Create")) {
        int sizeMB = 512;
        if (choice.contains("256")) sizeMB = 256;
        else if (choice.contains("512")) sizeMB = 512;
        else if (choice.contains("1 GB")) sizeMB = 1024;
        else if (choice.contains("2 GB")) sizeMB = 2048;
        
        // Ensure machine directory exists
        QDir().mkpath(machineDir);
        
        // Create the file
        QFile file(hdPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.resize((qint64)sizeMB * 1024 * 1024);
            file.close();
        }
    }
    
    updateHDStatus();
}

void MachineEditDialog::onOkClicked()
{
    saveSettings();
    accept();
}

void MachineEditDialog::onCancelClicked()
{
    reject();
}

void MachineEditDialog::onNetworkChanged(int index)
{
    Q_UNUSED(index);
    QString networkType = networkCombo->currentData().toString();
    
    // Enable bridge fields only for Ethernet Bridging
    bool bridgingEnabled = (networkType == "bridging");
    bridgeLabel->setEnabled(bridgingEnabled);
    bridgeEdit->setEnabled(bridgingEnabled);
    
#if defined(Q_OS_LINUX)
    // Enable tunnel fields only for IP Tunnelling
    bool tunnelEnabled = (networkType == "tunnelling");
    tunnelLabel->setEnabled(tunnelEnabled);
    tunnelEdit->setEnabled(tunnelEnabled);
#endif
}
