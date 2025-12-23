/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2016-2017 Matthew Howkins

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
#include <assert.h>
#include <iostream>
#include <list>

#include <QGuiApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStatusBar>
#include <QSettings>

#if defined(Q_OS_WIN32)
#include "Windows.h"
#endif /* Q_OS_WIN32 */ 

#include "rpcemu.h"
#include "keyboard.h"
#include "main_window.h"
#include "rpc-qt5.h"
#include "vidc20.h"
#include "machine_inspector_window.h"
#include "machine_snapshot.h"
#include "config_selector_dialog.h"

#ifdef RPCEMU_VNC
#include "vnc_server.h"
#include "vnc_dialog.h"
#endif

#define URL_MANUAL	"http://www.marutan.net/rpcemu/manual/"
#define URL_WEBSITE	"http://www.marutan.net/rpcemu/"

MainDisplay::MainDisplay(Emulator &emulator, QWidget *parent)
    : QWidget(parent),
      emulator(emulator),
      double_size(VIDC_DOUBLE_NONE),
      full_screen(false),
      integer_scaling(false)
{
	assert(pconfig_copy);

	image = new QImage(640, 480, QImage::Format_RGB32);

	// No need to erase to background colour before painting
	this->setAttribute(Qt::WA_OpaquePaintEvent);

	// Initialize integer scaling from config
	integer_scaling = pconfig_copy->integer_scaling != 0;

	// Hide pointer in mouse hack mode
	if(pconfig_copy->mousehackon) {
		this->setCursor(Qt::BlankCursor);
	}
		
	calculate_scaling();
}

void
MainDisplay::mouseMoveEvent(QMouseEvent *event)
{
	if((!pconfig_copy->mousehackon && mouse_captured) || full_screen) {
		QPoint middle;

		// In mouse capture mode move the mouse back to the middle of the window */ 
		middle.setX(this->width() / 2);
		middle.setY(this->height() / 2);

		QCursor::setPos(this->mapToGlobal(middle));

		// Calculate relative deltas based on difference from centre of display widget
		int dx = event->x() - middle.x();
		int dy = event->y() - middle.y();

		emit this->emulator.mouse_move_relative_signal(dx, dy);
	} else if(pconfig_copy->mousehackon) {
		// Follows host mouse (mousehack) mode
		emit this->emulator.mouse_move_signal(event->x(), event->y());
	}

}

void
MainDisplay::mousePressEvent(QMouseEvent *event)
{
	// Handle turning on mouse capture in capture mouse mode
	if(!pconfig_copy->mousehackon) {
		if(!mouse_captured) {
			mouse_captured = 1;

			// Hide pointer in mouse capture mode when it's been captured
			this->setCursor(Qt::BlankCursor);

			return;
		}
	}

	if (event->button() & 7) {
		emit this->emulator.mouse_press_signal(event->button() & 7);
	}
}

void
MainDisplay::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() & 7) {
		emit this->emulator.mouse_release_signal(event->button() & 7);
	}
}

void
MainDisplay::wheelEvent(QWheelEvent *event)
{
	const int dy = event->angleDelta().y();

	emit this->emulator.mouse_wheel_signal(dy);
}

void
MainDisplay::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	// Use smooth scaling only in fullscreen without integer scaling
	if (full_screen && !integer_scaling) {
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	} else {
		painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
	}
	
	const QRect dest = event->rect();
	QRect source;

	switch (double_size) {
	case VIDC_DOUBLE_NONE:
		source = dest;
		break;
	case VIDC_DOUBLE_X:
		source = QRect(dest.x() / 2, dest.y(), dest.width() / 2, dest.height());
		break;
	case VIDC_DOUBLE_Y:
		source = QRect(dest.x(), dest.y() / 2, dest.width(), dest.height() / 2);
		break;
	case VIDC_DOUBLE_BOTH:
		source = QRect(dest.x() / 2, dest.y() / 2, dest.width() / 2, dest.height() / 2);
		break;
	}

	if (full_screen || integer_scaling) {
		// Fill background with black for letterboxing
		if ((dest.x() < offset_x) || (dest.y() < offset_y) ||
		    (dest.x() + dest.width() > offset_x + scaled_x) ||
		    (dest.y() + dest.height() > offset_y + scaled_y)) {
			painter.fillRect(dest, Qt::black);
		}

		const QRect rect(offset_x, offset_y, scaled_x, scaled_y);
		painter.drawImage(rect, *image);
	} else {
		painter.drawImage(dest, *image, source);
	}
}

void
MainDisplay::resizeEvent(QResizeEvent *)
{
	calculate_scaling();
}

void
MainDisplay::get_host_size(int& xsize, int& ysize) const
{
	xsize = this->host_xsize;
	ysize = this->host_ysize;
}

void
MainDisplay::set_full_screen(bool full_screen)
{
	this->full_screen = full_screen;

	calculate_scaling();
}

void
MainDisplay::set_integer_scaling(bool integer_scaling)
{
	this->integer_scaling = integer_scaling;

	calculate_scaling();
	update();
}

bool
MainDisplay::get_integer_scaling() const
{
	return this->integer_scaling;
}

void
MainDisplay::update_image(const QImage& img, int yl, int yh, int double_size)
{
	bool recalculate_needed = false;

	if (img.size() != image->size()) {
		// Re-create image with new size and copy of data
		*(this->image) = img.copy();

		recalculate_needed = true;

	} else {
		// Copy just the data that has changed
		const void *src = img.scanLine(yl);
		void *dest = image->scanLine(yl);

		const int lines = yh - yl;
		const int bytes = img.bytesPerLine() * lines;

		memcpy(dest, src, (size_t) bytes);
	}

	if (double_size != this->double_size) {
		this->double_size = double_size;
		recalculate_needed = true;
	}

	if (recalculate_needed) {
		calculate_scaling();
		this->update();
		return;
	}

	// Trigger repaint of changed region
	int width = image->width();
	int ymin = yl;
	int ymax = yh;

	if (double_size & VIDC_DOUBLE_X) {
		width *= 2;
	}
	if (double_size & VIDC_DOUBLE_Y) {
		ymin *= 2;
		ymax *= 2;
	}

	if (full_screen) {
		width = (width * scaled_x) / host_xsize;

		/* For the Pixmap Smoothing to work properly, the height
		 * needs to be expanded by one pixel to avoid visual
		 * artifacts */
		if (ymin > 0) {
			ymin--;
		}
		if (ymax < host_ysize) {
			ymax++;
		}

		// calculate 'ymin' rounded down, 'ymax' rounded up
		ymin = (ymin * scaled_y) / host_ysize;
		ymax = ((ymax * scaled_y) + host_ysize - 1) / host_ysize;

		int height = ymax - ymin;
		this->update(offset_x, ymin + offset_y, width, height);
	} else {
		int height = ymax - ymin;
		this->update(0, ymin, width, height);
	}
}

/**
 * Called to update the image scaling.
 *
 * Called when any of the following change:
 * - Image size
 * - Double-size
 * - Windowed or Full screen
 * - Widget size
 * - Integer scaling mode
 */
void
MainDisplay::calculate_scaling()
{
	if (double_size & VIDC_DOUBLE_X) {
		host_xsize = image->width() * 2;
	} else {
		host_xsize = image->width();
	}
	if (double_size & VIDC_DOUBLE_Y) {
		host_ysize = image->height() * 2;
	} else {
		host_ysize = image->height();
	}

	if (full_screen || integer_scaling) {
		const int widget_x = this->width();
		const int widget_y = this->height();

		if (integer_scaling) {
			// Calculate maximum integer scale factor that fits
			int scale_x = widget_x / host_xsize;
			int scale_y = widget_y / host_ysize;
			int scale = (scale_x < scale_y) ? scale_x : scale_y;

			// Ensure at least 1x scaling
			if (scale < 1) {
				scale = 1;
			}

			scaled_x = host_xsize * scale;
			scaled_y = host_ysize * scale;
		} else {
			// Smooth scaling to fit (fullscreen only)
			if ((widget_x * host_ysize) >= (widget_y * host_xsize)) {
				scaled_x = (widget_y * host_xsize) / host_ysize;
				scaled_y = widget_y;
			} else {
				scaled_x = widget_x;
				scaled_y = (widget_x * host_ysize) / host_xsize;
			}
		}

		offset_x = (widget_x - scaled_x) / 2;
		offset_y = (widget_y - scaled_y) / 2;
	}
}

/**
 * Is the display currently doubling in either direction
 * needed in MainWindow to adjust mouse coordinates from the emulator
 */
int
MainDisplay::get_double_size()
{
	return double_size;
}

/**
 * Dump the current display to the file specified.
 *
 * @param filename Filename to save display image to
 *
 * @return bool of success or failure
 */
bool
MainDisplay::save_screenshot(QString filename)
{
	return this->image->save(filename, "png");
}

MainWindow::MainWindow(Emulator &emulator)
		: full_screen(false),
			reenable_mousehack(false),
			machine_inspector_window(nullptr),
#ifdef RPCEMU_VNC
			vnc_server(nullptr),
#endif
			emulator(emulator),
			mips_timer(this),
			mips_total_instructions(0),
			mips_seconds(0),
			menu_open(false),
			status_mips(nullptr),
			status_avg_mips(nullptr),
			status_fdc_label(nullptr),
			status_fdc_led(nullptr),
			status_ide_label(nullptr),
			status_ide_led(nullptr),
			status_hostfs_label(nullptr),
			status_hostfs_led(nullptr),
			status_network_label(nullptr),
			status_network_led(nullptr),
			fdc_led_timer(nullptr),
			ide_led_timer(nullptr),
			hostfs_led_timer(nullptr),
			network_led_timer(nullptr)
{
	setWindowTitle("RPCEmu v" VERSION);

	// Copy the emulators config to a thread local copy
	memcpy(&config_copy, &config,  sizeof(Config));
	pconfig_copy = &config_copy;
	model_copy = machine.model;

	display = new MainDisplay(emulator);
	display->setFixedSize(640, 480);
	setCentralWidget(display);

	// Mouse handling
	display->setMouseTracking(true);

	create_actions();
	create_menus();
	create_tool_bars();
	create_status_bar();

	readSettings();
	
	// Add current machine to recent machines list
	add_to_recent_machines(QString::fromUtf8(config_copy.name));

	this->setFixedSize(this->sizeHint());
	setUnifiedTitleAndToolBarOnMac(true);

	// Update the gui with the initial config setting
	// TODO what about fullscreen? (probably handled in GUI only
	if(config_copy.cpu_idle) {
		cpu_idle_action->setChecked(true);
	}
	if(config.mousehackon) {
		mouse_hack_action->setChecked(true);
	}
	if(config.mousetwobutton) {
		mouse_twobutton_action->setChecked(true);
	}
	if(config.cdromenabled) {
		// TODO we could check config.cdromtype here, but it's a bit
		// unreliable
		cdrom_empty_action->setChecked(true);
	} else {
		cdrom_disabled_action->setChecked(true);
	}

	configure_dialog = new ConfigureDialog(emulator, &config_copy, &model_copy, this);
	nat_list_dialog = new NatListDialog(emulator, this);
	about_dialog = new AboutDialog(this);

#ifdef RPCEMU_VNC
	// VNC Server
	vnc_server = new VncServer(&emulator, this);
	g_vncServer = vnc_server;
	
	// Auto-start VNC if enabled in config
	if (config_copy.vnc_enabled) {
		QString password = QString::fromUtf8(config_copy.vnc_password);
		vnc_server->start(config_copy.vnc_port, password);
	}
#endif

	// MIPS counting
	window_title.reserve(128);
	connect(&mips_timer, &QTimer::timeout, this, &MainWindow::mips_timer_timeout);
	mips_timer.start(1000);
	
	// App losing/gaining focus
	connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::application_state_changed);
	
	// Workaround for for qt bug https://bugreports.qt.io/browse/QTBUG-67239
	// Prevents the menu code stealing the keyboard focus and preventing keys
	// like escape from working, when opening more than one menu on the menubar
	this->setFocus();
}

void
MainWindow::menu_machine_inspector()
{
	if (machine_inspector_window == nullptr) {
		machine_inspector_window = new MachineInspectorWindow(emulator, this);
		connect(machine_inspector_window, &QObject::destroyed, this, [this]() {
			machine_inspector_window = nullptr;
		});
	}

	machine_inspector_window->showAndRaise();
}

void
MainWindow::menu_debug_run()
{
	QMetaObject::invokeMethod(&emulator, "debugger_resume", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MainWindow::update_debugger_action_states);
}

void
MainWindow::menu_debug_pause()
{
	QMetaObject::invokeMethod(&emulator, "debugger_pause", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MainWindow::update_debugger_action_states);
}

void
MainWindow::menu_debug_step()
{
	QMetaObject::invokeMethod(&emulator, "debugger_step", Qt::QueuedConnection);
	QTimer::singleShot(0, this, &MainWindow::update_debugger_action_states);
}

void
MainWindow::menu_debug_step5()
{
	QMetaObject::invokeMethod(&emulator,
	                         "debugger_step_n",
	                         Qt::QueuedConnection,
	                         Q_ARG(quint32, (quint32) 5));
	QTimer::singleShot(0, this, &MainWindow::update_debugger_action_states);
}
MainWindow::~MainWindow()
{
#ifdef RPCEMU_NETWORKING
	delete nat_list_dialog;
#endif /* RPCEMU_NETWORKING */
	delete configure_dialog;
	delete about_dialog;
}

/**
 * Ask the user if they'd like to reset RPCEmu
 *
 * @param parent pointer to mainwindow, used to centre dialog over mainwindow
 * @return code of which button pressed
 */
int
MainWindow::reset_question(QWidget *parent)
{
	QMessageBox msgBox(parent);

	msgBox.setWindowTitle("RPCEmu");
	msgBox.setText("This will reset RPCEmu!\n\nOkay to continue?");
	msgBox.setIcon(QMessageBox::Warning);
	msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
	msgBox.setDefaultButton(QMessageBox::Cancel);

	return msgBox.exec();
}

/**
 * Signal received about window gaining/losing focus, or minimising etc.
 *
 * @param state new application state
 */
void
MainWindow::application_state_changed(Qt::ApplicationState state)
{
	// If the application loses focus, release all the keys
	// that are pressed down. Prevents key stuck down repeats in emulator
	if(state != Qt::ApplicationActive) {
		release_held_keys();
		infocus = false;
	} else {
		infocus = true;
	}
}

/**
 * Generate a key-release message for each key recorded as held down, and then
 * clear this list of held keys.
 */
void
MainWindow::release_held_keys()
{
	// Release keys in the emulator
	for (std::list<quint32>::reverse_iterator it = held_keys.rbegin(); it != held_keys.rend(); ++it) {
		emit this->emulator.key_release_signal(*it);
	}

	// Clear the list of keys considered to be held in the host
	held_keys.clear();
}

/**
 * Window close button or File->exit() selected
 * 
 * @param event
 */
void
MainWindow::closeEvent(QCloseEvent *event)
{
	// Request confirmation to exit
	QMessageBox msgBox(QMessageBox::Question,
	    "RPCEmu",
	    "Are you sure you want to exit?",
	    QMessageBox::Cancel,
	    this);
	QPushButton *exit_button = msgBox.addButton("Exit", QMessageBox::ActionRole);
	msgBox.setDefaultButton(QMessageBox::Cancel);
	msgBox.setInformativeText("Any unsaved data will be lost.");
	msgBox.exec();

	if (msgBox.clickedButton() != exit_button) {
		// Prevent this close message triggering any more effects
		event->ignore();
		return;
	}

	// Disconnect the applicationStateChanged event, because our handler
	// can generate messages the machine won't be able to handle when quit
	disconnect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::application_state_changed);

	// Inform the emulator thread that we're quitting
	emit this->emulator.exit_signal();

	// Wait until emulator thread has exited
	this->emulator.thread()->wait();

	// Pass on the close message for the main window, this
	// will cause the program to quit
	event->accept();
}

void
MainWindow::keyPressEvent(QKeyEvent *event)
{
	// Block keyboard input (to non-GUI elements) if menu open
	if (this->menu_open) {
		return;
	}

	// Ignore unknown key events (can be generated by dead keys)
	if (event->key() == 0 || event->key() == Qt::Key_unknown) {
		return;
	}

	// Special case, handle windows menu key as being menu mouse button
	if(Qt::Key_Menu == event->key()) {
		emit this->emulator.mouse_press_signal(Qt::MidButton);
		return;
	}

	// Special case, check for Ctrl-End, our multi purpose do clever things key
	if((Qt::Key_End == event->key()) && (event->modifiers() & Qt::ControlModifier)) {
		if(full_screen) {
			// Change Full Screen -> Windowed

			display->set_full_screen(false);

			int host_xsize, host_ysize;
			display->get_host_size(host_xsize, host_ysize);
			display->setFixedSize(host_xsize, host_ysize);

			menuBar()->setVisible(true);
			this->showNormal();
			this->setFixedSize(this->sizeHint());

			full_screen = false;

			// Request redraw of display
			display->update();
			
			// If we were in mousehack mode before entering fullscreen
			// return to it now
			if(reenable_mousehack) {
				emit this->emulator.mouse_hack_signal();	
			}
			reenable_mousehack = false;
			
			// If we were in mouse capture mode before entering fullscreen
			// and we hadn't captured the mouse, display the host cursor now
			if(!config_copy.mousehackon && !mouse_captured) {
				this->display->setCursor(Qt::ArrowCursor);
			}
			
			return;
		} else if(!pconfig_copy->mousehackon && mouse_captured) {
			// Turn off mouse capture
			mouse_captured = 0;

			// show pointer in mouse capture mode when it's not been captured
			this->display->setCursor(Qt::ArrowCursor);

			return;
		}
	}

	// Regular case pass key press onto the emulator
	if (!event->isAutoRepeat()) {
		native_keypress_event(event->nativeScanCode());
	}
}

void
MainWindow::keyReleaseEvent(QKeyEvent *event)
{
	// Ignore unknown key events (can be generated by dead keys)
	if (event->key() == 0 || event->key() == Qt::Key_unknown) {
		return;
	}

	// Special case, handle windows menu key as being menu mouse button
	if(Qt::Key_Menu == event->key()) {
		emit this->emulator.mouse_release_signal(Qt::MidButton);
		return;
	}

	// Regular case pass key release onto the emulator
	if (!event->isAutoRepeat()) {
		native_keyrelease_event(event->nativeScanCode());
	}
}

/**
 * Called by us with native scan-code to forward key-press to the emulator
 *
 * @param scan_code Native scan code of key
 */
void
MainWindow::native_keypress_event(unsigned scan_code)
{
	// Check the key isn't already marked as held down (else ignore)
	// (to deal with potentially inconsistent host messages)
	bool found = (std::find(held_keys.begin(), held_keys.end(), scan_code) != held_keys.end());

	if (!found) {
		// Add the key to the list of held_keys, that will be released
		// when the window loses the focus
		held_keys.insert(held_keys.end(), scan_code);

		emit this->emulator.key_press_signal(scan_code);
	}
}

/**
 * Called by us with native scan-code to forward key-release to the emulator
 *
 * @param scan_code Native scan code of key
 */
void
MainWindow::native_keyrelease_event(unsigned scan_code)
{
	// Check the key is marked as held down (else ignore)
	// (to deal with potentially inconsistent host messages)
	bool found = (std::find(held_keys.begin(), held_keys.end(), scan_code) != held_keys.end());

	if (found) {
		// Remove the key from the list of held_keys, that will be released
		// when the window loses the focus
		held_keys.remove(scan_code);

		emit this->emulator.key_release_signal(scan_code);
	}
}

void
MainWindow::menu_screenshot()
{
	QString fileName = QFileDialog::getSaveFileName(this,
	                                                tr("Save Screenshot"),
	                                                "screenshot.png",
	                                                tr("PNG (*.png)"));

	// fileName is NULL if user hit cancel
	if (fileName != NULL) {
		bool result = this->display->save_screenshot(fileName);

		if (result == false) {
			QMessageBox msgBox(this);
			msgBox.setText("Error saving screenshot");
			msgBox.setStandardButtons(QMessageBox::Ok);
			msgBox.setDefaultButton(QMessageBox::Ok);
			msgBox.exec();
		}
	}
}

void
MainWindow::menu_reset()
{
	int ret = MainWindow::reset_question(this);

	switch (ret) {
	case QMessageBox::Ok:
		emit this->emulator.reset_signal();
		break;

	case QMessageBox::Cancel:
	default:
		break;
	}
}

void
MainWindow::load_disc(int drive)
{
	QString fileName = QFileDialog::getOpenFileName(this,
	    tr("Open Disc Image"),
	    "",
	    tr("All disc images (*.adf *.adl *.hfe *.img);;ADFS D/E/F Disc Image (*.adf);;ADFS L Disc Image (*.adl);;DOS Disc Image (*.img);;HFE Disc Image (*.hfe)"));

	// fileName is Null and Empty if user hit Cancel
	if (fileName.isEmpty()) {
		return;
	}

	// Add to recent floppies list
	add_to_recent_floppies(fileName);

	// Inform Emulator thread of disc change
	if (drive == 0) {
		emit this->emulator.load_disc_0_signal(fileName);
	} else if (drive == 1) {
		emit this->emulator.load_disc_1_signal(fileName);
	}
}

void
MainWindow::menu_loaddisc0()
{
	load_disc(0);
}

void
MainWindow::menu_loaddisc1()
{
	load_disc(1);
}

void
MainWindow::menu_ejectdisc0()
{
	emit this->emulator.eject_disc_0_signal();
}

void
MainWindow::menu_ejectdisc1()
{
	emit this->emulator.eject_disc_1_signal();
}

static const struct discTypeFileMap {
	QString displayName;
	QString extension;
	QString blankFileName;
} discTypeFileMaps[] = {
	{ "ADFS E 800k Disc Image (*.adf)",  ".adf", ":resources/blank-e-800.adf" },
	{ "ADFS F 1600k Disc Image (*.adf)", ".adf", ":resources/blank-f-1600.adf" },
	{ "ADFS L 640k Disc Image (*.adl)",  ".adl", ":resources/blank-l-640.adl" },
	{ "DOS 720k Disc Image (*.img)",     ".img", ":resources/blank-pc-720.img" },
	{ "DOS 1440k Disc Image (*.img)",    ".img", ":resources/blank-pc-1440.img" },
};

/**
 * Return a 'filter' list, suitable for use in QFileDialog::getSaveFileName
 * which represents all of the blank disc image types we can create
 *
 * @return filter list
 */
static QString
getCreateBlankDiscFilters()
{
	QString filterList = "";

	for (size_t i = 0; i < sizeof(discTypeFileMaps) / sizeof(discTypeFileMap); i++) {
		if (i != 0) {  // We do not add a separator before first entry
			filterList += ";;";
		}
		filterList += discTypeFileMaps[i].displayName;
	}

	return filterList;
}

/**
 * Given a human readable blank disc type filter name, what extension should it
 * have, and what filename is the blank formatted disc image in the Resources.
 *
 * @param filterName human readable filter name
 * @return Pointer to row of discTypeFileMaps, with relevant extension and
 *         blank disc Resource file, or NULL if not found
 */
static const discTypeFileMap *
filter_to_disc_type_file(QString filterName)
{
	for (size_t i = 0; i < sizeof(discTypeFileMaps) / sizeof(discTypeFileMap); i++) {
		if (filterName == discTypeFileMaps[i].displayName) {
			return &discTypeFileMaps[i];
		}
	}

	// Not in list
	return NULL;
}

/**
 * Pop up a Save Dialog so the user can create a blank disk image.
 * Once a filename has been given, create a blank disk image, and 'insert' it
 * into the chosen drive.
 *
 * @param drive Drive number that the disk image will be associated with.
 */
void
MainWindow::create_disc(int drive)
{
	const QString filterList = getCreateBlankDiscFilters();

	QString selectedFilter;
	QString fileName = QFileDialog::getSaveFileName(this,
	    tr("Create Blank Disc Image"),
	    "",
	    filterList,
	    &selectedFilter);

	// fileName is Null and Empty if user hit Cancel
	if (fileName.isEmpty() || selectedFilter.isEmpty()) {
		return;
	}

	// Find the blank disc file in resources to copy
	// (this should never fail)
	const discTypeFileMap *d = filter_to_disc_type_file(selectedFilter);
	if (d == NULL) {
		::error("Filter name '%s' not in discTypeFileMaps, failing", selectedFilter.toStdString().c_str());
		return;
	}
	const QString &extension = d->extension;
	const QString &blankDiscFileName = d->blankFileName;

	// Check the file extension of the filename received is present and of
	// the expected value
	if (!fileName.endsWith(extension, Qt::CaseInsensitive)) {
		// The extension is either missing or not what was expected.
		// Append the expected extension:
		fileName += extension;

		// The Save Dialog sought permission to overwrite file, but the
		// filename has now been changed. If a file exists with this
		// modified filename, don't overwrite it, and report an error.
		if (QFile::exists(fileName)) {
			::error("Not overwriting existing file '%s'", fileName.toStdString().c_str());
			return;
		}
	}

	// The Save Dialog sought permission to overwrite file, but
	// QFile::copy() won't overwrite existing files, so remove the
	// destination file instead
	if (QFile::exists(fileName) && !QFile::remove(fileName)) {
		::error("Failed to remove existing file '%s' before overwriting", fileName.toStdString().c_str());
		return;
	}

	// Copy the file
	bool copySuccess = QFile::copy(blankDiscFileName, fileName);
	if (!copySuccess) {
		::error("Failed to create blank image file '%s'", fileName.toStdString().c_str());
		return;
	}

	// Make sure the new file is set to read/write
	bool permissionsSuccess = QFile::setPermissions(fileName, QFileDevice::ReadOwner
	    | QFileDevice::WriteOwner
	    | QFileDevice::ReadUser
	    | QFileDevice::WriteUser);
	if (!permissionsSuccess) {
		::error("Failed to set permissions on file '%s'", fileName.toStdString().c_str());
		return;
	}

	// Inform Emulator thread of disc change
	if (drive == 0) {
		emit this->emulator.load_disc_0_signal(fileName);
	} else if (drive == 1) {
		emit this->emulator.load_disc_1_signal(fileName);
	}
}

/**
 * Create a blank disk image for drive 0
 */
void
MainWindow::menu_create_disc0()
{
	create_disc(0);
}

/**
 * Create a blank disk image for drive 1
 */
void
MainWindow::menu_create_disc1()
{
	create_disc(1);
}

void
MainWindow::menu_configure()
{
	// Get current config path and make it absolute for QSettings
	QString configPath = QString::fromUtf8(config_get_path());
	QFileInfo fi(configPath);
	if (fi.isRelative()) {
		configPath = QDir::currentPath() + "/" + configPath;
	}
	MachineEditDialog dialog(configPath, this);
	if (dialog.exec() == QDialog::Accepted) {
		// Configuration was changed - show message that restart is needed
		QMessageBox::information(this, tr("Configuration Changed"),
			tr("Configuration changes will take effect after restarting the emulator."));
	}
}

#ifdef RPCEMU_NETWORKING
void
MainWindow::menu_nat_list()
{
	nat_list_dialog->exec(); // Modal
}
#endif /* RPCEMU_NETWORKING */

/**
 * Handle clicking on the Settings->Mute Sound option
 */
void
MainWindow::menu_mute_sound()
{
	bool muted = mute_sound_action->isChecked();
	plt_sound_set_muted(muted ? 1 : 0);
}

/**
 * Handle clicking on the Settings->Fullscreen option
 */
void
MainWindow::menu_fullscreen()
{
	if (!full_screen) {
		// Change Windowed -> Full Screen

		// Make sure people know how to exit full-screen
		if (config_copy.show_fullscreen_message) {
			QCheckBox *checkBox = new QCheckBox("Do not show this message again");

			QMessageBox msg_box(QMessageBox::Information,
			    "RPCEmu - Full-screen mode",
			    "<p>This window will now be switched to <b>full-screen</b> mode.</p>"
			    "<p>To leave full-screen mode press <b>Ctrl-End</b>.</p>",
			    QMessageBox::Ok | QMessageBox::Cancel,
			    this);
			msg_box.setDefaultButton(QMessageBox::Ok);
			msg_box.setCheckBox(checkBox);

			int ret = msg_box.exec();

			// If they didn't click OK, revert the tick on the menu item and return
			if (ret != QMessageBox::Ok) {
				// Keep tick of menu item in sync
				fullscreen_action->setChecked(false);
				return;
			}

			// If they checked the box don't show this message again
			if (msg_box.checkBox()->isChecked()) {
				emit this->emulator.show_fullscreen_message_off();
				config_copy.show_fullscreen_message = 0;
			}
		}

		display->set_full_screen(true);

		this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		display->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		menuBar()->setVisible(false);
		this->showFullScreen();

		full_screen = true;
		
		// If in mousehack mode, change to a temporary mouse capture style
		// during full screen
		if(config_copy.mousehackon) {
			emit this->emulator.mouse_hack_signal();
			reenable_mousehack = true; 
		}
		
		// If in mouse capture mode and not captured, the cursor will be visible, hide it
		this->display->setCursor(Qt::BlankCursor);
	}

	// Keep tick of menu item in sync
	fullscreen_action->setChecked(false);
}

void
MainWindow::menu_cpu_idle()
{
	int ret = MainWindow::reset_question(this);

	switch (ret) {
	case QMessageBox::Ok:
		emit this->emulator.cpu_idle_signal();
		config_copy.cpu_idle ^= 1;
		break;

	case QMessageBox::Cancel:
		// If cancelled reset the tick box on the menu back to current emu state
		cpu_idle_action->setChecked(config_copy.cpu_idle);
		break;
	default:
		break;
	}

}

/**
 * Handle clicking on the Settings->Integer Scaling option
 */
void
MainWindow::menu_integer_scaling()
{
	config_copy.integer_scaling = config_copy.integer_scaling ? 0 : 1;
	integer_scaling_action->setChecked(config_copy.integer_scaling != 0);
	display->set_integer_scaling(config_copy.integer_scaling != 0);

	// Signal the emulator thread to update its config
	emit this->emulator.integer_scaling_signal();
}

#ifdef RPCEMU_VNC
/**
 * Handle clicking on the Settings->VNC Server... option
 * Opens the VNC server configuration dialog
 */
void
MainWindow::menu_vnc_server()
{
	QString currentPassword = QString::fromUtf8(config_copy.vnc_password);
	VncDialog dialog(vnc_server, currentPassword, this);
	dialog.exec();
	
	// Update config based on VNC server state and dialog settings
	config_copy.vnc_enabled = vnc_server->isRunning() ? 1 : 0;
	config_copy.vnc_port = vnc_server->getPort();
	if (config_copy.vnc_port == 0) {
		config_copy.vnc_port = 5900;
	}
	
	// Save password from dialog
	QByteArray pwdBytes = dialog.getPassword().toUtf8();
	strncpy(config_copy.vnc_password, pwdBytes.constData(), sizeof(config_copy.vnc_password) - 1);
	config_copy.vnc_password[sizeof(config_copy.vnc_password) - 1] = '\0';
}
#endif


void
MainWindow::menu_cdrom_disabled()
{
	if (config_copy.cdromenabled) {
		int ret = MainWindow::reset_question(this);

		switch (ret) {
		case QMessageBox::Ok:
			break;

		case QMessageBox::Cancel:
			cdrom_disabled_action->setChecked(false);
			return;
		default:
			cdrom_disabled_action->setChecked(false);
			return;
		}
	}

	/* we now have either no need to reboot or an agreement to reboot */

	emit this->emulator.cdrom_disabled_signal();
	config_copy.cdromenabled = 0;

	cdrom_menu_selection_update(cdrom_disabled_action);
}

void
MainWindow::menu_cdrom_empty()
{
	if (!config_copy.cdromenabled) {
		int ret = MainWindow::reset_question(this);

		switch (ret) {
		case QMessageBox::Ok:
			break;

		case QMessageBox::Cancel:
			cdrom_empty_action->setChecked(false);
			return;
		default:
			cdrom_empty_action->setChecked(false);
			return;
		}
	}

	/* we now have either no need to reboot or an agreement to reboot */

	emit this->emulator.cdrom_empty_signal();
	config_copy.cdromenabled = 1;

	cdrom_menu_selection_update(cdrom_empty_action);
}

void
MainWindow::menu_cdrom_iso()
{
	QString fileName = QFileDialog::getOpenFileName(this,
	                                                tr("Open ISO Image"),
	                                                "",
	                                                tr("ISO CD-ROM Image (*.iso);;All Files (*.*)"));

	/* fileName is NULL if user hit cancel */
	if(fileName != NULL) {
		if (!config_copy.cdromenabled) {
			int ret = MainWindow::reset_question(this);

			switch (ret) {
			case QMessageBox::Ok:
				break;

			case QMessageBox::Cancel:
				cdrom_iso_action->setChecked(false);
				return;
			default:
				cdrom_iso_action->setChecked(false);
				return;
			}
		}

		/* we now have either no need to reboot or an agreement to reboot */

		// Add to recent CD-ROMs list
		add_to_recent_cdroms(fileName);

		emit this->emulator.cdrom_load_iso_signal(fileName);
		config_copy.cdromenabled = 1;

		cdrom_menu_selection_update(cdrom_iso_action);
		return;
	}

	cdrom_iso_action->setChecked(false);
}

void
MainWindow::menu_cdrom_ioctl()
{
#if defined(Q_OS_LINUX)
	if (!config_copy.cdromenabled) {
		int ret = MainWindow::reset_question(this);

		switch (ret) {
		case QMessageBox::Ok:
			break;

		case QMessageBox::Cancel:
			cdrom_ioctl_action->setChecked(false);
			return;
		default:
			cdrom_ioctl_action->setChecked(false);
			return;
		}
	}

	/* we now have either no need to reboot or an agreement to reboot */

	emit this->emulator.cdrom_ioctl_signal();
	config_copy.cdromenabled = 1;

	cdrom_menu_selection_update(cdrom_ioctl_action);
#endif /* linux */
}

void
MainWindow::menu_cdrom_win_ioctl()
{
#if defined(Q_OS_WIN32)
	QAction* action = qobject_cast<QAction *>(QObject::sender());
	if(!action) {
		fatal("menu_cdrom_win_ioctl no action\n");
	}
	char drive_letter = action->data().toChar().toLatin1();

	if (!config_copy.cdromenabled) {
		int ret = MainWindow::reset_question(this);

		switch (ret) {
		case QMessageBox::Ok:
			break;

		case QMessageBox::Cancel:
			action->setChecked(false);
			return;
		default:
			action->setChecked(false);
			return;
		}
	}

	/* we now have either no need to reboot or an agreement to reboot */

	emit this->emulator.cdrom_win_ioctl_signal(drive_letter);
	config_copy.cdromenabled = 1;

	cdrom_menu_selection_update(action);
#endif /* win32 */
}
void
MainWindow::menu_mouse_hack()
{
	emit this->emulator.mouse_hack_signal();
	config_copy.mousehackon ^= 1;

	// If we were previously in mouse capture mode (somehow having
	// escaped the mouse capturing), decapture the mouse
	if(config_copy.mousehackon) {
		mouse_captured = 0;

		// Hide pointer in mouse hack mode
		this->display->setCursor(Qt::BlankCursor);
	} else {
		// Show pointer in mouse capture mode when it's not been captured
		this->display->setCursor(Qt::ArrowCursor);
	}
}

void
MainWindow::menu_mouse_twobutton()
{
	emit this->emulator.mouse_twobutton_signal();
	config_copy.mousetwobutton ^= 1;
}


void
MainWindow::menu_online_manual()
{
	QDesktopServices::openUrl(QUrl(URL_MANUAL));
}

void
MainWindow::menu_visit_website()
{
	QDesktopServices::openUrl(QUrl(URL_WEBSITE));
}

void
MainWindow::menu_about()
{
	about_dialog->show(); // Modeless
}

/**
 * A menu is being shown.
 * Release held keys, and update the 'menu_open' variable.
 */
void
MainWindow::menu_aboutToShow()
{
	release_held_keys();
	this->menu_open = true;
}

/**
 * A menu is being hidden.
 * Update the 'menu_open' variable.
 */
void
MainWindow::menu_aboutToHide()
{
	this->menu_open = false;
}

void
MainWindow::create_actions()
{
	// Actions on File menu
	screenshot_action = new QAction(tr("Take Screenshot..."), this);
	screenshot_action->setShortcut(QKeySequence(Qt::Key_F12));
	connect(screenshot_action, &QAction::triggered, this, &MainWindow::menu_screenshot);
	reset_action = new QAction(tr("Reset"), this);
	reset_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_R));
	connect(reset_action, &QAction::triggered, this, &MainWindow::menu_reset);
	exit_action = new QAction(tr("Exit"), this);
	exit_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));
	exit_action->setStatusTip(tr("Exit the application"));
	connect(exit_action, &QAction::triggered, this, &QMainWindow::close);

	// Actions on Disc->Floppy menu
	loaddisc0_action = new QAction(tr("Load Drive :0..."), this);
	loaddisc0_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_1));
	connect(loaddisc0_action, &QAction::triggered, this, &MainWindow::menu_loaddisc0);
	loaddisc1_action = new QAction(tr("Load Drive :1..."), this);
	loaddisc1_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_2));
	connect(loaddisc1_action, &QAction::triggered, this, &MainWindow::menu_loaddisc1);
	ejectdisc0_action = new QAction(tr("Eject Drive :0"), this);
	ejectdisc0_action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_1));
	connect(ejectdisc0_action, &QAction::triggered, this, &MainWindow::menu_ejectdisc0);
	ejectdisc1_action = new QAction(tr("Eject Drive :1"), this);
	ejectdisc1_action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_2));
	connect(ejectdisc1_action, &QAction::triggered, this, &MainWindow::menu_ejectdisc1);
	create_disc0_action = new QAction(tr("Create Blank Drive :0..."), this);
	connect(create_disc0_action, &QAction::triggered, this, &MainWindow::menu_create_disc0);
	create_disc1_action = new QAction(tr("Create Blank Drive :1..."), this);
	connect(create_disc1_action, &QAction::triggered, this, &MainWindow::menu_create_disc1);

	// Actions on the Disc->CD-ROM menu
	cdrom_disabled_action = new QAction(tr("Disabled"), this);
	cdrom_disabled_action->setCheckable(true);
	connect(cdrom_disabled_action, &QAction::triggered, this, &MainWindow::menu_cdrom_disabled);

	cdrom_empty_action = new QAction(tr("Empty"), this);
	cdrom_empty_action->setCheckable(true);
	connect(cdrom_empty_action, &QAction::triggered, this, &MainWindow::menu_cdrom_empty);

	cdrom_iso_action = new QAction(tr("Iso Image..."), this);
	cdrom_iso_action->setCheckable(true);
	connect(cdrom_iso_action, &QAction::triggered, this, &MainWindow::menu_cdrom_iso);

#if defined(Q_OS_LINUX)
	cdrom_ioctl_action = new QAction(tr("Host CD/DVD Drive"), this);
	cdrom_ioctl_action->setCheckable(true);
	connect(cdrom_ioctl_action, &QAction::triggered, this, &MainWindow::menu_cdrom_ioctl);
#endif /* linux */
#if defined(Q_OS_WIN32)
	// Dynamically add windows cdrom drives to the settings->cdrom menu
	char s[32];
	// Loop through each Windows drive letter and test to see if it's a CDROM
	for (char c = 'A'; c <= 'Z'; c++) {
		sprintf(s, "%c:\\", c);
		if (GetDriveTypeA(s) == DRIVE_CDROM) {
			QAction *new_action = new QAction(s, this);
			new_action->setCheckable(true);
			new_action->setData(c);

			connect(new_action, &QAction::triggered, this, &MainWindow::menu_cdrom_win_ioctl);

			cdrom_win_ioctl_actions.insert(cdrom_win_ioctl_actions.end(), new_action);
		}
	}
#endif

	// Actions on Settings menu
	configure_action = new QAction(tr("Configure..."), this);
	connect(configure_action, &QAction::triggered, this, &MainWindow::menu_configure);
#ifdef RPCEMU_NETWORKING
	nat_list_action = new QAction(tr("NAT Port Forwarding Rules..."), this);
	connect(nat_list_action, &QAction::triggered, this, &MainWindow::menu_nat_list);
#endif /* RPCEMU_NETWORKING */
	mute_sound_action = new QAction(tr("Mute Sound"), this);
	mute_sound_action->setCheckable(true);
	mute_sound_action->setShortcut(QKeySequence(Qt::Key_F10));
	connect(mute_sound_action, &QAction::triggered, this, &MainWindow::menu_mute_sound);
	fullscreen_action = new QAction(tr("Full-screen Mode"), this);
	fullscreen_action->setCheckable(true);
	fullscreen_action->setShortcut(QKeySequence(Qt::Key_F11));
	connect(fullscreen_action, &QAction::triggered, this, &MainWindow::menu_fullscreen);
	integer_scaling_action = new QAction(tr("Pixel Perfect"), this);
	integer_scaling_action->setCheckable(true);
	integer_scaling_action->setChecked(config_copy.integer_scaling != 0);
	connect(integer_scaling_action, &QAction::triggered, this, &MainWindow::menu_integer_scaling);
#ifdef RPCEMU_VNC
	vnc_server_action = new QAction(tr("VNC Server..."), this);
	connect(vnc_server_action, &QAction::triggered, this, &MainWindow::menu_vnc_server);
#endif
	cpu_idle_action = new QAction(tr("Reduce CPU Usage"), this);
	cpu_idle_action->setCheckable(true);
	connect(cpu_idle_action, &QAction::triggered, this, &MainWindow::menu_cpu_idle);

	// Actions on the Settings->Mouse menu
	mouse_hack_action = new QAction(tr("Follow Host Mouse"), this);
	mouse_hack_action->setCheckable(true);
	mouse_hack_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_M));
	connect(mouse_hack_action, &QAction::triggered, this, &MainWindow::menu_mouse_hack);

	mouse_twobutton_action = new QAction(tr("Two-button Mouse Mode"), this);
	mouse_twobutton_action->setCheckable(true);
	connect(mouse_twobutton_action, &QAction::triggered, this, &MainWindow::menu_mouse_twobutton);

	// Actions on About menu
	online_manual_action = new QAction(tr("Online Manual..."), this);
	online_manual_action->setShortcut(QKeySequence(Qt::Key_F1));
	connect(online_manual_action, &QAction::triggered, this, &MainWindow::menu_online_manual);
	visit_website_action = new QAction(tr("Visit Website..."), this);
	connect(visit_website_action, &QAction::triggered, this, &MainWindow::menu_visit_website);

	about_action = new QAction(tr("About RPCEmu..."), this);
	about_action->setStatusTip(tr("Show the application's About box"));
	connect(about_action, &QAction::triggered, this, &MainWindow::menu_about);

	machine_inspector_action = new QAction(tr("Machine Inspector..."), this);
	machine_inspector_action->setShortcut(QKeySequence(Qt::Key_F9));
	connect(machine_inspector_action, &QAction::triggered, this, &MainWindow::menu_machine_inspector);

	debug_run_action = new QAction(tr("Run"), this);
	debug_run_action->setShortcut(QKeySequence(Qt::Key_F5));
	connect(debug_run_action, &QAction::triggered, this, &MainWindow::menu_debug_run);
	debug_pause_action = new QAction(tr("Pause"), this);
	debug_pause_action->setShortcut(QKeySequence(Qt::Key_F6));
	connect(debug_pause_action, &QAction::triggered, this, &MainWindow::menu_debug_pause);
	debug_step_action = new QAction(tr("Step"), this);
	debug_step_action->setShortcut(QKeySequence(Qt::Key_F7));
	connect(debug_step_action, &QAction::triggered, this, &MainWindow::menu_debug_step);
	debug_step5_action = new QAction(tr("Step ×5"), this);
	debug_step5_action->setShortcut(QKeySequence(Qt::Key_F8));
	connect(debug_step5_action, &QAction::triggered, this, &MainWindow::menu_debug_step5);

	connect(this, &MainWindow::main_display_signal, this, &MainWindow::main_display_update, Qt::BlockingQueuedConnection);
//	connect(this, &MainWindow::main_display_signal, this, &MainWindow::main_display_update);
	connect(this, &MainWindow::move_host_mouse_signal, this, &MainWindow::move_host_mouse);
	connect(this, &MainWindow::send_nat_rule_to_gui_signal, this, &MainWindow::send_nat_rule_to_gui);

	// Connections for displaying error messages in the GUI
	connect(this, &MainWindow::error_signal, this, &MainWindow::error);
	connect(this, &MainWindow::fatal_signal, this, &MainWindow::fatal);
	connect(&emulator, &Emulator::debugger_state_changed_signal, this, &MainWindow::update_debugger_action_states);
	connect(&emulator, &Emulator::machine_switched_signal, this, &MainWindow::on_machine_switched);

	// Initialise debugger action state optimistically; a real snapshot will
	// be requested once the emulator thread is running and emits an update.
	debug_run_action->setEnabled(false);
	debug_pause_action->setEnabled(true);
	debug_step_action->setEnabled(false);
	debug_step5_action->setEnabled(false);
}

void
MainWindow::create_menus()
{
	// File menu
	file_menu = menuBar()->addMenu(tr("File"));
	file_menu->addAction(screenshot_action);
	file_menu->addSeparator();
	
	// Recent Machines submenu
	recent_machines_menu = file_menu->addMenu(tr("Recent Machines"));
	for (int i = 0; i < MaxRecentMachines; i++) {
		QAction *action = new QAction(this);
		action->setVisible(false);
		connect(action, &QAction::triggered, this, &MainWindow::menu_recent_machine_triggered);
		recent_machines_menu->addAction(action);
		recent_machine_actions.append(action);
	}
	recent_machines_menu->addSeparator();
	clear_recent_machines_action = new QAction(tr("Clear Recent Machines"), this);
	connect(clear_recent_machines_action, &QAction::triggered, this, &MainWindow::menu_clear_recent_machines);
	recent_machines_menu->addAction(clear_recent_machines_action);
	update_recent_machines_menu();
	
	file_menu->addSeparator();
	file_menu->addAction(reset_action);
	file_menu->addSeparator();
	file_menu->addAction(exit_action);

	// Disc menu
	disc_menu = menuBar()->addMenu(tr("Disc"));
	floppy_menu = disc_menu->addMenu(tr("Floppy"));
	cdrom_menu = disc_menu->addMenu(tr("CD-ROM"));

	// Disc->Floppy menu
	floppy_menu->addAction(loaddisc0_action);
	floppy_menu->addAction(loaddisc1_action);
	floppy_menu->addSeparator();
	floppy_menu->addAction(ejectdisc0_action);
	floppy_menu->addAction(ejectdisc1_action);
	floppy_menu->addSeparator();
	floppy_menu->addAction(create_disc0_action);
	floppy_menu->addAction(create_disc1_action);
	floppy_menu->addSeparator();
	
	// Recent Floppies submenu
	recent_floppies_menu = floppy_menu->addMenu(tr("Recent Images"));
	for (int i = 0; i < MaxRecentFloppies; i++) {
		QAction *action = new QAction(this);
		action->setVisible(false);
		connect(action, &QAction::triggered, this, &MainWindow::menu_recent_floppy_triggered);
		recent_floppies_menu->addAction(action);
		recent_floppy_actions.append(action);
	}
	recent_floppies_menu->addSeparator();
	clear_recent_floppies_action = new QAction(tr("Clear Recent Images"), this);
	connect(clear_recent_floppies_action, &QAction::triggered, this, &MainWindow::menu_clear_recent_floppies);
	recent_floppies_menu->addAction(clear_recent_floppies_action);
	update_recent_floppies_menu();

	// Disc->CD-ROM menu
	cdrom_menu->addAction(cdrom_disabled_action);
	cdrom_menu->addAction(cdrom_empty_action);
	cdrom_menu->addAction(cdrom_iso_action);
#if defined(Q_OS_LINUX)
	cdrom_menu->addAction(cdrom_ioctl_action);
#endif /* linux */
#if defined(Q_OS_WIN32)
	for (unsigned i = 0; i < cdrom_win_ioctl_actions.size(); i++) {
		cdrom_menu->addAction(cdrom_win_ioctl_actions[i]);
	}
#endif
	cdrom_menu->addSeparator();
	
	// Recent CD-ROMs submenu
	recent_cdroms_menu = cdrom_menu->addMenu(tr("Recent Images"));
	for (int i = 0; i < MaxRecentCDROMs; i++) {
		QAction *action = new QAction(this);
		action->setVisible(false);
		connect(action, &QAction::triggered, this, &MainWindow::menu_recent_cdrom_triggered);
		recent_cdroms_menu->addAction(action);
		recent_cdrom_actions.append(action);
	}
	recent_cdroms_menu->addSeparator();
	clear_recent_cdroms_action = new QAction(tr("Clear Recent Images"), this);
	connect(clear_recent_cdroms_action, &QAction::triggered, this, &MainWindow::menu_clear_recent_cdroms);
	recent_cdroms_menu->addAction(clear_recent_cdroms_action);
	update_recent_cdroms_menu();

	// Settings menu
	settings_menu = menuBar()->addMenu(tr("Settings"));
	settings_menu->addAction(configure_action);
#ifdef RPCEMU_NETWORKING
	settings_menu->addAction(nat_list_action);
	if (this->config_copy.network_type != NetworkType_NAT) {
		nat_list_action->setEnabled(false);
	}
#endif /* RPCEMU_NETWORKING */
	settings_menu->addSeparator();
	settings_menu->addAction(mute_sound_action);
	settings_menu->addSeparator();
	settings_menu->addAction(fullscreen_action);
	settings_menu->addAction(integer_scaling_action);
	settings_menu->addSeparator();
#ifdef RPCEMU_VNC
	settings_menu->addAction(vnc_server_action);
	settings_menu->addSeparator();
#endif
	settings_menu->addAction(cpu_idle_action);
	settings_menu->addSeparator();
	mouse_menu = settings_menu->addMenu(tr("Mouse"));

	// Mouse submenu
	mouse_menu->addAction(mouse_hack_action);
	mouse_menu->addSeparator();
	mouse_menu->addAction(mouse_twobutton_action);

	debug_menu = menuBar()->addMenu(tr("Debug"));
	debug_menu->addAction(debug_run_action);
	debug_menu->addAction(debug_pause_action);
	debug_menu->addSeparator();
	debug_menu->addAction(debug_step_action);
	debug_menu->addAction(debug_step5_action);
	debug_menu->addSeparator();
	debug_menu->addAction(machine_inspector_action);

	menuBar()->addSeparator();

	// Help menu
	help_menu = menuBar()->addMenu(tr("Help"));
	help_menu->addAction(online_manual_action);
	help_menu->addAction(visit_website_action);
	help_menu->addSeparator();
	help_menu->addAction(about_action);

	// Add handlers to track menu show/hide events
	add_menu_show_hide_handlers();
}

void
MainWindow::create_tool_bars()
{
}

/**
 * Create the status bar with MIPS, average MIPS, HostFS and network activity indicators.
 */
void
MainWindow::create_status_bar()
{
	// LED style for inactive (grey) and active (colored)
	const QString led_style_off = "QLabel { color: #888888; font-size: 14px; }";

	status_mips = new QLabel("MIPS: -");
	status_mips->setToolTip(tr("Current emulation speed in million instructions per second"));
	status_avg_mips = new QLabel("Avg: -");
	status_avg_mips->setToolTip(tr("Average emulation speed over session"));
	
	// Floppy indicator with LED (orange)
	status_fdc_label = new QLabel("Floppy");
	status_fdc_label->setToolTip(tr("Floppy disk activity"));
	status_fdc_led = new QLabel(QString::fromUtf8("\xe2\x97\x8f")); // Unicode filled circle
	status_fdc_led->setStyleSheet(led_style_off);
	status_fdc_led->setToolTip(tr("Floppy disk activity"));
	
	// IDE indicator with LED (red for disk activity)
	status_ide_label = new QLabel("HDD/CDROM");
	status_ide_label->setToolTip(tr("Hard disk and CD-ROM activity"));
	status_ide_led = new QLabel(QString::fromUtf8("\xe2\x97\x8f")); // Unicode filled circle
	status_ide_led->setStyleSheet(led_style_off);
	status_ide_led->setToolTip(tr("Hard disk and CD-ROM activity"));
	
	// HostFS indicator with LED (green)
	status_hostfs_label = new QLabel("HostFS");
	status_hostfs_label->setToolTip(tr("Host filesystem activity"));
	status_hostfs_led = new QLabel(QString::fromUtf8("\xe2\x97\x8f")); // Unicode filled circle
	status_hostfs_led->setStyleSheet(led_style_off);
	status_hostfs_led->setToolTip(tr("Host filesystem activity"));
	
	// Network indicator with LED (blue)
	status_network_label = new QLabel("Net");
	status_network_label->setToolTip(tr("Network activity"));
	status_network_led = new QLabel(QString::fromUtf8("\xe2\x97\x8f")); // Unicode filled circle
	status_network_led->setStyleSheet(led_style_off);
	status_network_led->setToolTip(tr("Network activity"));

	// Add spacing between labels
	status_mips->setMinimumWidth(80);
	status_avg_mips->setMinimumWidth(80);

	statusBar()->addWidget(status_mips);
	statusBar()->addWidget(status_avg_mips);
	statusBar()->addPermanentWidget(status_fdc_label);
	statusBar()->addPermanentWidget(status_fdc_led);
	statusBar()->addPermanentWidget(status_ide_label);
	statusBar()->addPermanentWidget(status_ide_led);
	statusBar()->addPermanentWidget(status_hostfs_label);
	statusBar()->addPermanentWidget(status_hostfs_led);
	statusBar()->addPermanentWidget(status_network_label);
	statusBar()->addPermanentWidget(status_network_led);
	
	// Create timers for LED fade-off effect
	fdc_led_timer = new QTimer(this);
	fdc_led_timer->setSingleShot(true);
	connect(fdc_led_timer, &QTimer::timeout, this, &MainWindow::fdc_led_timeout);
	
	ide_led_timer = new QTimer(this);
	ide_led_timer->setSingleShot(true);
	connect(ide_led_timer, &QTimer::timeout, this, &MainWindow::ide_led_timeout);
	
	hostfs_led_timer = new QTimer(this);
	hostfs_led_timer->setSingleShot(true);
	connect(hostfs_led_timer, &QTimer::timeout, this, &MainWindow::hostfs_led_timeout);
	
	network_led_timer = new QTimer(this);
	network_led_timer->setSingleShot(true);
	connect(network_led_timer, &QTimer::timeout, this, &MainWindow::network_led_timeout);
}

/**
 * Update the Recent Machines menu from saved settings.
 */
void
MainWindow::update_recent_machines_menu()
{
	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentMachines").toStringList();

	// Clear existing actions
	for (int i = 0; i < recent_machine_actions.size(); i++) {
		recent_machine_actions[i]->setVisible(false);
	}

	// Populate with recent machines
	int num_recent = qMin(recent.size(), MaxRecentMachines);
	for (int i = 0; i < num_recent; i++) {
		QString text = QString("&%1 %2").arg(i + 1).arg(recent[i]);
		recent_machine_actions[i]->setText(text);
		recent_machine_actions[i]->setData(recent[i]);
		recent_machine_actions[i]->setVisible(true);
	}

	// Show/hide the separator and "no recent" message
	recent_machines_menu->setEnabled(num_recent > 0);
}

/**
 * Add a machine to the recent machines list.
 *
 * @param machine_name Name of the machine to add
 */
void
MainWindow::add_to_recent_machines(const QString &machine_name)
{
	if (machine_name.isEmpty()) {
		return;
	}

	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentMachines").toStringList();

	// Remove if already in list (to move to top)
	recent.removeAll(machine_name);

	// Add to front
	recent.prepend(machine_name);

	// Limit size
	while (recent.size() > MaxRecentMachines) {
		recent.removeLast();
	}

	settings.setValue("recentMachines", recent);

	update_recent_machines_menu();
}

/**
 * Handler for when user clicks a recent machine entry.
 */
void
MainWindow::menu_recent_machine_triggered()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (action) {
		QString machine_name = action->data().toString();
		
		// Check if config file exists
		QString config_path = QCoreApplication::applicationDirPath() + "/configs/" + machine_name + ".cfg";
		if (!QFile::exists(config_path)) {
			QMessageBox::warning(this, tr("Machine Not Found"),
				tr("The configuration for '%1' no longer exists.").arg(machine_name));
			return;
		}
		
		// Prompt user to confirm switch
		int ret = QMessageBox::question(this, tr("Switch Machine"),
			tr("Are you sure you want to switch to '%1'?\n\n"
			   "Any unsaved data in the current machine will be lost.")
			.arg(machine_name),
			QMessageBox::Yes | QMessageBox::No,
			QMessageBox::No);
		
		if (ret == QMessageBox::Yes) {
			// Add to recent (moves to top)
			add_to_recent_machines(machine_name);
			
			// Update window title immediately
			setWindowTitle(QString("RPCEmu - %1").arg(machine_name));
			
			// Emit signal to switch machine on emulator thread
			emit this->emulator.switch_machine_signal(config_path);
		}
	}
}

/**
 * Handler for clearing the recent machines list.
 */
void
MainWindow::menu_clear_recent_machines()
{
	QSettings settings("RPCEmu", "RPCEmu");
	settings.setValue("recentMachines", QStringList());
	update_recent_machines_menu();
}

/**
 * Handle the machine switched signal from the emulator thread.
 * Updates the GUI to reflect the new machine configuration.
 */
void
MainWindow::on_machine_switched(QString machine_name)
{
	// Update window title
	setWindowTitle(QString("RPCEmu - %1").arg(machine_name));
	
	// Update local config copy from global config
	memcpy(&config_copy, &config, sizeof(Config));
	model_copy = machine.model;
	
	rpclog("MainWindow: Machine switched to '%s'\n", machine_name.toUtf8().constData());
}

/**
 * Update the Recent Floppies menu from saved settings.
 */
void
MainWindow::update_recent_floppies_menu()
{
	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentFloppies").toStringList();

	// Clear existing actions
	for (int i = 0; i < recent_floppy_actions.size(); i++) {
		recent_floppy_actions[i]->setVisible(false);
	}

	// Populate with recent floppies
	int num_recent = qMin(recent.size(), MaxRecentFloppies);
	for (int i = 0; i < num_recent; i++) {
		// Show just the filename, store full path in data
		QFileInfo fi(recent[i]);
		QString text = QString("&%1 %2").arg(i < 9 ? QString::number(i + 1) : "0").arg(fi.fileName());
		recent_floppy_actions[i]->setText(text);
		recent_floppy_actions[i]->setData(recent[i]);
		recent_floppy_actions[i]->setVisible(true);
	}

	// Show/hide the menu based on whether there are entries
	recent_floppies_menu->setEnabled(num_recent > 0);
}

/**
 * Add a floppy image to the recent floppies list.
 *
 * @param floppy_path Full path to the floppy image
 */
void
MainWindow::add_to_recent_floppies(const QString &floppy_path)
{
	if (floppy_path.isEmpty()) {
		return;
	}

	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentFloppies").toStringList();

	// Remove if already in list (to move to top)
	recent.removeAll(floppy_path);

	// Add to front
	recent.prepend(floppy_path);

	// Limit size
	while (recent.size() > MaxRecentFloppies) {
		recent.removeLast();
	}

	settings.setValue("recentFloppies", recent);

	update_recent_floppies_menu();
}

/**
 * Handler for when user clicks a recent floppy entry.
 */
void
MainWindow::menu_recent_floppy_triggered()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (action) {
		QString floppy_path = action->data().toString();
		
		// Check if file still exists
		if (!QFile::exists(floppy_path)) {
			QMessageBox::warning(this, tr("File Not Found"),
				tr("The disc image '%1' no longer exists.").arg(floppy_path));
			return;
		}
		
		// Add to recent (moves to top)
		add_to_recent_floppies(floppy_path);
		
		// Load into drive :0 by default
		emit this->emulator.load_disc_0_signal(floppy_path);
	}
}

/**
 * Handler for clearing the recent floppies list.
 */
void
MainWindow::menu_clear_recent_floppies()
{
	QSettings settings("RPCEmu", "RPCEmu");
	settings.setValue("recentFloppies", QStringList());
	update_recent_floppies_menu();
}

/**
 * Update the Recent CD-ROMs menu from saved settings.
 */
void
MainWindow::update_recent_cdroms_menu()
{
	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentCDROMs").toStringList();

	// Clear existing actions
	for (int i = 0; i < recent_cdrom_actions.size(); i++) {
		recent_cdrom_actions[i]->setVisible(false);
	}

	// Populate with recent CD-ROMs
	int num_recent = qMin(recent.size(), MaxRecentCDROMs);
	for (int i = 0; i < num_recent; i++) {
		// Show just the filename, store full path in data
		QFileInfo fi(recent[i]);
		QString text = QString("&%1 %2").arg(i < 9 ? QString::number(i + 1) : "0").arg(fi.fileName());
		recent_cdrom_actions[i]->setText(text);
		recent_cdrom_actions[i]->setData(recent[i]);
		recent_cdrom_actions[i]->setVisible(true);
	}

	// Show/hide the menu based on whether there are entries
	recent_cdroms_menu->setEnabled(num_recent > 0);
}

/**
 * Add a CD-ROM image to the recent CD-ROMs list.
 *
 * @param cdrom_path Full path to the CD-ROM image
 */
void
MainWindow::add_to_recent_cdroms(const QString &cdrom_path)
{
	if (cdrom_path.isEmpty()) {
		return;
	}

	QSettings settings("RPCEmu", "RPCEmu");
	QStringList recent = settings.value("recentCDROMs").toStringList();

	// Remove if already in list (to move to top)
	recent.removeAll(cdrom_path);

	// Add to front
	recent.prepend(cdrom_path);

	// Limit size
	while (recent.size() > MaxRecentCDROMs) {
		recent.removeLast();
	}

	settings.setValue("recentCDROMs", recent);

	update_recent_cdroms_menu();
}

/**
 * Handler for when user clicks a recent CD-ROM entry.
 */
void
MainWindow::menu_recent_cdrom_triggered()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (action) {
		QString cdrom_path = action->data().toString();
		
		// Check if file still exists
		if (!QFile::exists(cdrom_path)) {
			QMessageBox::warning(this, tr("File Not Found"),
				tr("The CD-ROM image '%1' no longer exists.").arg(cdrom_path));
			return;
		}
		
		// Handle enabling CD-ROM if disabled (same logic as menu_cdrom_iso)
		if (!config_copy.cdromenabled) {
			int ret = MainWindow::reset_question(this);
			switch (ret) {
			case QMessageBox::Ok:
				break;
			case QMessageBox::Cancel:
			default:
				return;
			}
		}
		
		// Add to recent (moves to top)
		add_to_recent_cdroms(cdrom_path);
		
		// Load the ISO
		emit this->emulator.cdrom_load_iso_signal(cdrom_path);
		config_copy.cdromenabled = 1;
		
		cdrom_menu_selection_update(cdrom_iso_action);
	}
}

/**
 * Handler for clearing the recent CD-ROMs list.
 */
void
MainWindow::menu_clear_recent_cdroms()
{
	QSettings settings("RPCEmu", "RPCEmu");
	settings.setValue("recentCDROMs", QStringList());
	update_recent_cdroms_menu();
}

/**
 * Turn off the FDC LED after timeout.
 */
void
MainWindow::fdc_led_timeout()
{
	if (status_fdc_led) {
		status_fdc_led->setStyleSheet("QLabel { color: #888888; font-size: 14px; }");
	}
}

/**
 * Turn off the IDE LED after timeout.
 */
void
MainWindow::ide_led_timeout()
{
	if (status_ide_led) {
		status_ide_led->setStyleSheet("QLabel { color: #888888; font-size: 14px; }");
	}
}

/**
 * Turn off the HostFS LED after timeout.
 */
void
MainWindow::hostfs_led_timeout()
{
	if (status_hostfs_led) {
		status_hostfs_led->setStyleSheet("QLabel { color: #888888; font-size: 14px; }");
	}
}

/**
 * Turn off the Network LED after timeout.
 */
void
MainWindow::network_led_timeout()
{
	if (status_network_led) {
		status_network_led->setStyleSheet("QLabel { color: #888888; font-size: 14px; }");
	}
}

/**
 * Add handlers to track menu show/hide events.
 *
 * These are used to track open menus, and suppress key events.
 */
void
MainWindow::add_menu_show_hide_handlers()
{
	// Find the QMenu items that are children of menuBar()
	//
	// Filter for direct children only, otherwise when sub-menus are
	// hidden, we'll update the variable menu_open to false even though a
	// top-level menu remains open
	QList<QMenu *> menus = menuBar()->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly);

	QList<QMenu *>::const_iterator i;

	for (i = menus.constBegin(); i != menus.constEnd(); i++) {
		const QMenu *menu = *i;

		connect(menu, &QMenu::aboutToShow, this, &MainWindow::menu_aboutToShow);
		connect(menu, &QMenu::aboutToHide, this, &MainWindow::menu_aboutToHide);
	}
}

void
MainWindow::update_debugger_action_states()
{
	if (debug_run_action == nullptr) {
		return;
	}

	MachineSnapshot snapshot;
	bool ok = QMetaObject::invokeMethod(&emulator,
	                                   "takeSnapshot",
	                                   Qt::BlockingQueuedConnection,
	                                   Q_RETURN_ARG(MachineSnapshot, snapshot));
	if (!ok) {
		return;
	}

	const bool paused = (snapshot.debug_paused != 0);
	const bool pausing = (snapshot.debug_pause_requested != 0);

	debug_run_action->setEnabled(paused);
	debug_pause_action->setEnabled(!paused || pausing);
	debug_step_action->setEnabled(paused);
	debug_step5_action->setEnabled(paused);
}

void
MainWindow::readSettings()
{
	QSettings settings("QtProject", "Application Example");
	QPoint pos = settings.value("pos", QPoint(200, 200)).toPoint();
	QSize size = settings.value("size", QSize(400, 400)).toSize();
	resize(size);
	move(pos);
}

void
MainWindow::writeSettings()
{
	QSettings settings("QtProject", "Application Example");
	settings.setValue("pos", pos());
	settings.setValue("size", size());
}

void
MainWindow::main_display_update(VideoUpdate video_update)
{
	if (video_update.host_xsize != display->width() ||
	    video_update.host_ysize != display->height())
	{
		if (!full_screen) {
			// Resize Widget containing image
			display->setFixedSize(video_update.host_xsize, video_update.host_ysize);

			// Resize Window
			this->setFixedSize(this->sizeHint());
		}
	}

	// Copy image data
	display->update_image(video_update.image, video_update.yl, video_update.yh,
	    video_update.double_size);
}

/**
 * Received a request from the emulator thread to position the host mouse pointer
 * Used in sections of Follows host mouse/mousehack code
 *
 * @param mouse_update message struct containing desired mouse coordinates
 */
void
MainWindow::move_host_mouse(MouseMoveUpdate mouse_update)
{
	QPoint pos;
	int double_size = display->get_double_size();

	// Do not move the mouse if rpcemu window doesn't have the focus
	if(false == infocus) {
		return;
	}

	// Don't move the mouse if the display widget is not directly under the mouse
	// This handles the mouse being moved away from rpcemu or the mouse over one of our
	// menus or configuration dialog boxes.
	if(false == display->underMouse()) {
		return;
	}

	pos.setX(mouse_update.x);
	pos.setY(mouse_update.y);

	// The mouse coordinates from the backend do not know about frontend double sizing
	// Add that on if necessary
	if(double_size == VIDC_DOUBLE_X
	   || double_size == VIDC_DOUBLE_BOTH)
	{
		pos.setX(mouse_update.x * 2);
	}

	if(double_size == VIDC_DOUBLE_Y
	   || double_size == VIDC_DOUBLE_BOTH)
	{
		pos.setY(mouse_update.y * 2);
	}

	// Due to the front end and backend vidc doublesize values being
	// potentially out of sync on mode change, as a temporary HACK, cap the
	// mouse pos to under the main display widget
	if(pos.x() > (display->width() - 1)) {
		pos.setX(display->width() - 1);
	}
	if(pos.y() > (display->height() - 1)) {
		pos.setY(display->height() - 1);
	}

	QCursor::setPos(display->mapToGlobal(pos));
}

void
MainWindow::send_nat_rule_to_gui(PortForwardRule rule)
{
	nat_list_dialog->add_nat_rule(rule);
}

/**
 * Called each time the mips_timer times out.
 *
 * The shared instruction counter is read and the title updated with the
 * MIPS and Average.
 */
void
MainWindow::mips_timer_timeout()
{
	const char *capture_text = NULL;

	assert(pconfig_copy);

	// Read (and zero atomically) the instruction count from the emulator core
	// 'instruction_count' is in multiples of 65536
	const unsigned count = (unsigned) instruction_count.fetchAndStoreRelease(0);

	// Calculate MIPS
	const double mips = (double) count * 65536.0 / 1000000.0;

	// Update variables used for average
	mips_total_instructions += (uint64_t) count << 16;
	mips_seconds++;

	// Calculate Average
	const double average = (double) mips_total_instructions / ((double) mips_seconds * 1000000.0);

	// Update global perf structure for Machine Inspector
	perf.mips = (float) mips;

	// Read (and zero atomically) the activity counters
	const int fdc_ops = fdc_activity.fetchAndStoreRelease(0);
	const int ide_ops = ide_activity.fetchAndStoreRelease(0);
	const int hostfs_ops = hostfs_activity.fetchAndStoreRelease(0);
	const int network_ops = network_activity.fetchAndStoreRelease(0);

	// Update status bar
	if (status_mips) {
		status_mips->setText(QString("MIPS: %1").arg(mips, 0, 'f', 1));
	}
	if (status_avg_mips) {
		status_avg_mips->setText(QString("Avg: %1").arg(average, 0, 'f', 1));
	}
	
	// FDC LED - light up orange on activity
	if (status_fdc_led && fdc_ops > 0) {
		status_fdc_led->setStyleSheet("QLabel { color: #ff9900; font-size: 14px; }");
		fdc_led_timer->start(200); // LED stays on for 200ms
	}
	
	// IDE LED - light up red on activity
	if (status_ide_led && ide_ops > 0) {
		status_ide_led->setStyleSheet("QLabel { color: #ff3333; font-size: 14px; }");
		ide_led_timer->start(200); // LED stays on for 200ms
	}
	
	// HostFS LED - light up green on activity
	if (status_hostfs_led && hostfs_ops > 0) {
		status_hostfs_led->setStyleSheet("QLabel { color: #00cc00; font-size: 14px; }");
		hostfs_led_timer->start(200); // LED stays on for 200ms
	}
	
	// Network LED - light up blue on activity
	if (status_network_led && network_ops > 0) {
		status_network_led->setStyleSheet("QLabel { color: #0088ff; font-size: 14px; }");
		network_led_timer->start(200); // LED stays on for 200ms
	}

	if(!pconfig_copy->mousehackon) {
		if(mouse_captured) {
			capture_text = " Press CTRL-END to release mouse";
		} else {
			capture_text = " Click to capture mouse";
		}
	} else {
		capture_text = "";
	}

#if 1
	// Update window title with machine name
	window_title = QString("RPCEmu - %1%2")
	    .arg(config_copy.name)
	    .arg(capture_text);

#else
	// Read  (and zero atomically) the IOMD timer count from the emulator core
	const int icount = iomd_timer_count.fetchAndStoreRelease(0);

	// Read  (and zero atomically) the Video timer count from the emulator core
	const int vcount = video_timer_count.fetchAndStoreRelease(0);

	// Update window title (including timer information, for debug purposes)
	window_title = QString("RPCEmu - %1, ITimer: %2, VTimer: %3%4")
	    .arg(config_copy.name)
	    .arg(icount)
	    .arg(vcount)
	    .arg(capture_text);
#endif

	setWindowTitle(window_title);
}

/**
 * Present a model dialog to the user about an error that has occured.
 * Wait for them to dismiss it
 * 
 * @param error error string
 */
void
MainWindow::error(QString error)
{
	QMessageBox::warning(this, "RPCEmu Error", error);
}

/**
 * Present a model dialog to the user about a fatal error that has occured.
 * Wait for them to dismiss it and exit the program
 * 
 * @param error error string
 */
void
MainWindow::fatal(QString error)
{
	QMessageBox::critical(this, "RPCEmu Fatal Error", error);

	exit(EXIT_FAILURE);
}

/**
 * Make the selected CDROM menu item the only one selected 
 * 
 * @param cdrom_action CDROM item to make the selection
 */ 
void
MainWindow::cdrom_menu_selection_update(const QAction *cdrom_action)
{
	// Turn all tick boxes off 
	cdrom_disabled_action->setChecked(false);
	cdrom_empty_action->setChecked(false);
	cdrom_iso_action->setChecked(false);
#if defined(Q_OS_LINUX)
	cdrom_ioctl_action->setChecked(false);
#endif
#if defined(Q_OS_WIN32)
	for (unsigned i = 0; i < cdrom_win_ioctl_actions.size(); i++) {
		cdrom_win_ioctl_actions[i]->setChecked(false);
	}
#endif

	// Turn correct one on
	if(cdrom_action == cdrom_disabled_action) {
		cdrom_disabled_action->setChecked(true);
	} else if(cdrom_action == cdrom_empty_action) {
		cdrom_empty_action->setChecked(true);
	} else if(cdrom_action == cdrom_iso_action) {
		cdrom_iso_action->setChecked(true);
#if defined(Q_OS_LINUX)
	} else if(cdrom_action == cdrom_ioctl_action) {
		cdrom_ioctl_action->setChecked(true);
#endif
#if defined(Q_OS_WIN32)
	} else {
		for (unsigned i = 0; i < cdrom_win_ioctl_actions.size(); i++) {
			if(cdrom_action == cdrom_win_ioctl_actions[i]) {
				cdrom_win_ioctl_actions[i]->setChecked(true);
			}
		}
#endif
	}
}

#if defined(Q_OS_WIN32)
/**
 * windows pre event handler used by us to modify some default behaviour
 *
 * Disable the use of the virtual menu key (alt) that otherwise goes off
 * every time someone presses alt in the emulated OS
 *
 * @param eventType unused
 * @param message window event MSG struct
 * @param result unused
 * @return bool of whether we've handled the event (true) or windows/qt should deal with it (false) 
 */
bool
MainWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
{
	Q_UNUSED(result);
	Q_UNUSED(eventType);

	// Block keyboard input (to non-GUI elements) if menu open
	if (this->menu_open) {
		return false;
	}

	MSG *msg = static_cast<MSG*>(message);

	// Handle 'alt' key presses that would select the menu
	// Handle 'shift-f10' key presses that would select the context-menu
	// Fake 'normal' key press and release and then tell windows/qt to
	// not handle it
	if ((msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP)
	    && (msg->wParam == VK_MENU || msg->wParam == VK_F10))
	{
		unsigned scan_code = (unsigned) (msg->lParam >> 16) & 0x1ff;

		if (msg->message == WM_SYSKEYDOWN) {
			native_keypress_event(scan_code);
		} else {
			native_keyrelease_event(scan_code);
		}
		return true;
	}

	// Convert dead-key presses into normal key-presses:
	// If key pressed, delete the following message if it's WM_DEADCHAR.
	// This effectively converts it into a normal key-press and stops Qt bypassing QKeyEvent handling
	// Based on code @ https://stackoverflow.com/questions/3872085/dead-keys-dont-arrive-at-keypressedevent
	MSG peeked_msg;
	switch (msg->message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		PeekMessage(&peeked_msg, msg->hwnd, WM_DEADCHAR, WM_DEADCHAR, PM_REMOVE);
		break;
	}

	// Anything else should be handled by the regular qt and windows handlers
	return false;
}
#endif // Q_OS_WIN32
