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
#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QSettings>
#include <QToolBar>

#include "configure_dialog.h"
#include "nat_list_dialog.h"
#include "about_dialog.h"
#include "rpc-qt5.h"

#ifdef RPCEMU_VNC
#include "vnc_server.h"
#endif

#include "rpcemu.h"

class MachineInspectorWindow;


/**
 * Used to pass data from Emulator thread to GUI thread
 * when the main display needs to be updated
 */
struct VideoUpdate {
	QImage		image;
	int		yl;
	int		yh;

	int		double_size;
	int		host_xsize;
	int		host_ysize;
};

/**
 * Used to pass data from Emulator thread to GUI thread
 * when in mousehack wants to move the host mouse
 */
struct MouseMoveUpdate {
	int16_t x;
	int16_t y;
};

class MainDisplay : public QWidget
{
	Q_OBJECT

public:
	MainDisplay(Emulator &emulator, QWidget *parent = 0);

	void get_host_size(int& host_xsize, int& host_ysize) const;
	void set_full_screen(bool full_screen);
	void set_integer_scaling(bool integer_scaling);
	bool get_integer_scaling() const;
	void update_image(const QImage& img, int yl, int yh, int double_size);
	int get_double_size();
	bool save_screenshot(QString filename);

protected:
	void mousePressEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
	void mouseReleaseEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
	void mouseMoveEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
	void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
	void paintEvent(QPaintEvent *event) Q_DECL_OVERRIDE;
	void resizeEvent(QResizeEvent *event) Q_DECL_OVERRIDE;

private:
	void calculate_scaling();

	Emulator &emulator;

	QImage *image;
	int double_size;

	bool full_screen;
	bool integer_scaling;
	int host_xsize, host_ysize;
	int scaled_x, scaled_y;
	int offset_x, offset_y;
};


class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow(Emulator &emulator);
	virtual ~MainWindow();

	/* Handle displaying error messages */
	void error(QString error);
	void fatal(QString error);

	static int reset_question(QWidget *parent);

protected:
	void closeEvent(QCloseEvent *event) Q_DECL_OVERRIDE;
	void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
	void keyReleaseEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
#if defined(Q_OS_WIN32)
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) Q_DECL_OVERRIDE;
#endif /* Q_OS_WIN32 */
	
private slots:
	void menu_screenshot();
	void menu_reset();
	void menu_loaddisc0();
	void menu_loaddisc1();
	void menu_ejectdisc0();
	void menu_ejectdisc1();
	void menu_create_disc0();
	void menu_create_disc1();
	void menu_cdrom_disabled();
	void menu_cdrom_empty();
	void menu_cdrom_iso();
	void menu_cdrom_ioctl();
	void menu_cdrom_win_ioctl();
	void menu_configure();
#ifdef RPCEMU_NETWORKING
	void menu_nat_list();
#endif /* RPCEMU_NETWORKING */
	void menu_mute_sound();
	void menu_fullscreen();
	void menu_integer_scaling();
#ifdef RPCEMU_VNC
	void menu_vnc_server();
#endif
	void menu_serial();
	void menu_parallel();

	void menu_cpu_idle();
	void menu_mouse_hack();
	void menu_mouse_twobutton();
	void menu_online_manual();
	void menu_visit_website();
	void menu_about();
	void menu_machine_inspector();
	void menu_debug_run();
	void menu_debug_pause();
	void menu_debug_step();
	void menu_debug_step5();
	void menu_recent_machine_triggered();
	void menu_clear_recent_machines();
	void menu_recent_floppy_triggered();
	void menu_clear_recent_floppies();
	void menu_recent_cdrom_triggered();
	void menu_clear_recent_cdroms();
	void fdc_led_timeout();
	void ide_led_timeout();
	void hostfs_led_timeout();
	void network_led_timeout();

	// Track menu show/hide
	void menu_aboutToShow();
	void menu_aboutToHide();

	void main_display_update(VideoUpdate video_update);
	void move_host_mouse(MouseMoveUpdate mouse_update);
	void send_nat_rule_to_gui(PortForwardRule rule);
	void on_machine_switched(QString machine_name);

	// MIPS counting
	void mips_timer_timeout();

	void application_state_changed(Qt::ApplicationState state);
signals:
	void main_display_signal(VideoUpdate video_update);
	void move_host_mouse_signal(MouseMoveUpdate mouse_update);
	void send_nat_rule_to_gui_signal(PortForwardRule rule);

	void error_signal(QString error);
	void fatal_signal(QString error);

private:
	void create_actions();
	void create_menus();
	void create_tool_bars();
	void create_status_bar();

	void add_menu_show_hide_handlers();
	void update_debugger_action_states();
	void update_recent_machines_menu();
	void add_to_recent_machines(const QString &machineName);
	void update_recent_floppies_menu();
	void add_to_recent_floppies(const QString &floppyPath);
	void update_recent_cdroms_menu();
	void add_to_recent_cdroms(const QString &cdromPath);

	void readSettings();
	void writeSettings();

	void cdrom_menu_selection_update(const QAction *cdrom_action);

	void native_keypress_event(unsigned scan_code);
	void native_keyrelease_event(unsigned scan_code);
	void release_held_keys();

	void load_disc(int drive);
	void create_disc(int drive);

	bool full_screen;
	bool reenable_mousehack; ///< Did we disable mousehack entering fullscreen and have to reenable it on leaving fullscreen?

	MainDisplay *display;

	QString curFile;

	// Menus
	QMenu *file_menu;
	QMenu *recent_machines_menu;
	QMenu *disc_menu;
	QMenu *recent_floppies_menu;
	QMenu *floppy_menu;
	QMenu *cdrom_menu;
	QMenu *recent_cdroms_menu;
	QMenu *settings_menu;
	QMenu *mouse_menu;
	QMenu *debug_menu;
	QMenu *help_menu;

	// Toolbar
	QToolBar *main_toolbar;

	// Actions on File menu
	QAction *screenshot_action;
	QAction *reset_action;
	QAction *exit_action;

	// Actions on Disc menu (and submenus)
	QAction *loaddisc0_action;
	QAction *loaddisc1_action;
	QAction *ejectdisc0_action;
	QAction *ejectdisc1_action;
	QAction *create_disc0_action;
	QAction *create_disc1_action;
	QAction *cdrom_disabled_action;
	QAction *cdrom_empty_action;
#if defined(Q_OS_LINUX)
	QAction *cdrom_ioctl_action;
#endif /* linux */
#if defined(Q_OS_WIN32)
	std::vector<QAction *> cdrom_win_ioctl_actions;
#endif /* win32 */
	QAction *cdrom_iso_action;

	// Actions on Settings menu (and submenus)
	QAction *configure_action;
#ifdef RPCEMU_NETWORKING
	QAction *nat_list_action;
#endif /* RPCEMU_NETWORKING */
	QAction *mute_sound_action;
	QAction *fullscreen_action;
	QAction *integer_scaling_action;
#ifdef RPCEMU_VNC
	QAction *vnc_server_action;
#endif
	QAction *serial_action;
	QAction *parallel_action;

	QAction *cpu_idle_action;
	QAction *mouse_hack_action;
	QAction *mouse_twobutton_action;
	QAction *machine_inspector_action;
	QAction *debug_run_action;
	QAction *debug_pause_action;
	QAction *debug_step_action;
	QAction *debug_step5_action;

	// Actions on About menu
	QAction *online_manual_action;
	QAction *visit_website_action;
	QAction *about_action;

	// Dialogs
	ConfigureDialog *configure_dialog;
	NatListDialog *nat_list_dialog;
	AboutDialog *about_dialog;
 	MachineInspectorWindow *machine_inspector_window;
	class SerialDialog *serial_dialog;
	class ParallelDialog *parallel_dialog;

#ifdef RPCEMU_VNC
	// VNC Server
	VncServer *vnc_server;
#endif

	// Pointer to emulator instance
	Emulator &emulator;

	// GUI thread copy of the emulator's config
	Config config_copy;
	Model model_copy;

	// MIPS counting
	QTimer mips_timer;
	uint64_t mips_total_instructions;
	int32_t mips_seconds;
	QString window_title;
	
	// List of keys currently held down, released when window loses focus
	std::list<quint32> held_keys;

	bool menu_open; ///< Is there a menu open? Used to suppress key-presses

	bool infocus; ///< Does the main window currently have the focus

	// Status bar labels
	QLabel *status_mips;
	QLabel *status_avg_mips;
	QLabel *status_fdc_label;
	QLabel *status_fdc_led;
	QLabel *status_ide_label;
	QLabel *status_ide_led;
	QLabel *status_hostfs_label;
	QLabel *status_hostfs_led;
	QLabel *status_network_label;
	QLabel *status_network_led;
	
	// LED fade timers
	QTimer *fdc_led_timer;
	QTimer *ide_led_timer;
	QTimer *hostfs_led_timer;
	QTimer *network_led_timer;
	
	// Recent machines
	static const int MaxRecentMachines = 5;
	QList<QAction*> recent_machine_actions;
	QAction *clear_recent_machines_action;
	
	// Recent floppies
	static const int MaxRecentFloppies = 10;
	QList<QAction*> recent_floppy_actions;
	QAction *clear_recent_floppies_action;
	
	// Recent CD-ROMs
	static const int MaxRecentCDROMs = 10;
	QList<QAction*> recent_cdrom_actions;
	QAction *clear_recent_cdroms_action;
};

#endif
