/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <wx/wx.h>

#include "emulator_host.h"
#include "emulator_panel.h"
#include "gui_bridge.h"
#include "gui_preferences.h"

class MachineInspectorWindow;
class NatListDialog;

#ifdef RPCEMU_VNC
class VncServer;
#endif

enum MainFrameMenuId {
	ID_MENU_SCREENSHOT = wxID_HIGHEST + 1,
	ID_MENU_ABOUT_RISCOS,
	ID_MENU_RECENT_MACHINE_0,
	ID_MENU_RECENT_MACHINE_1,
	ID_MENU_RECENT_MACHINE_2,
	ID_MENU_RECENT_MACHINE_3,
	ID_MENU_RECENT_MACHINE_4,
	ID_MENU_CLEAR_RECENT_MACHINES,
	ID_MENU_RESET,
	ID_MENU_SAVE_STATE,
	ID_MENU_LOAD_STATE,
	ID_MENU_SUSPEND,
	ID_MENU_LOAD_DISC0,
	ID_MENU_LOAD_DISC1,
	ID_MENU_EJECT_DISC0,
	ID_MENU_EJECT_DISC1,
	ID_MENU_CREATE_DISC0,
	ID_MENU_CREATE_DISC1,
	ID_MENU_RECENT_FLOPPY_0,
	ID_MENU_RECENT_FLOPPY_1,
	ID_MENU_RECENT_FLOPPY_2,
	ID_MENU_RECENT_FLOPPY_3,
	ID_MENU_RECENT_FLOPPY_4,
	ID_MENU_RECENT_FLOPPY_5,
	ID_MENU_RECENT_FLOPPY_6,
	ID_MENU_RECENT_FLOPPY_7,
	ID_MENU_RECENT_FLOPPY_8,
	ID_MENU_RECENT_FLOPPY_9,
	ID_MENU_CLEAR_RECENT_FLOPPIES,
	ID_MENU_CDROM_DISABLED,
	ID_MENU_CDROM_EMPTY,
	ID_MENU_CDROM_ISO,
	ID_MENU_CDROM_IOCTL,
	ID_MENU_RECENT_CDROM_0,
	ID_MENU_RECENT_CDROM_1,
	ID_MENU_RECENT_CDROM_2,
	ID_MENU_RECENT_CDROM_3,
	ID_MENU_RECENT_CDROM_4,
	ID_MENU_RECENT_CDROM_5,
	ID_MENU_RECENT_CDROM_6,
	ID_MENU_RECENT_CDROM_7,
	ID_MENU_RECENT_CDROM_8,
	ID_MENU_RECENT_CDROM_9,
	ID_MENU_CLEAR_RECENT_CDROMS,
	ID_MENU_MACHINE,
	ID_MENU_NAT_LIST,
	ID_MENU_MUTE,
	ID_MENU_FULLSCREEN,
	ID_MENU_INTEGER_SCALING,
	ID_MENU_FIT_TO_WINDOW,
	ID_MENU_FOLLOW_HOST_DISPLAY,
	ID_MENU_SUSPEND_ON_EXIT,
	ID_MENU_VNC,
	ID_MENU_SERIAL,
	ID_MENU_PACKAGES,
	ID_MENU_USB,
	ID_MENU_PARALLEL,
	ID_MENU_CPU_IDLE,
	ID_MENU_MOUSE_HACK,
	ID_MENU_MOUSE_TWOBUTTON,
	ID_MENU_SHARED_CLIPBOARD,
	ID_MENU_DEFAULT_MACHINE,
	ID_MENU_DEBUG_RUN,
	ID_MENU_DEBUG_PAUSE,
	ID_MENU_DEBUG_STEP,
	ID_MENU_DEBUG_STEP5,
	ID_MENU_MACHINE_INSPECTOR,
	ID_MENU_ONLINE_MANUAL,
	ID_MENU_VISIT_WEBSITE,
	ID_MENU_ABOUT,
};

enum TimerId {
	ID_TIMER_MIPS = wxID_HIGHEST + 100,
	ID_TIMER_VIDEO,
	ID_TIMER_FDC_LED,
	ID_TIMER_IDE_LED,
	ID_TIMER_HOSTFS_LED,
	ID_TIMER_NETWORK_LED,
	ID_TIMER_CLIPBOARD,
};

enum StatusBarField {
	STATUS_MIPS = 0,
	STATUS_AVG_MIPS,
	STATUS_FDC_LABEL,
	STATUS_FDC_LED,
	STATUS_IDE_LABEL,
	STATUS_IDE_LED,
	STATUS_HOSTFS_LABEL,
	STATUS_HOSTFS_LED,
	STATUS_NET_LABEL,
	STATUS_NET_LED,
	STATUS_MACHINE,
	STATUS_FIELD_COUNT
};

class MainFrame : public wxFrame, public GuiBridge {
public:
	MainFrame();
	~MainFrame() override;

	void StartEmulator();
	void UpdateMachineStatus();

	/* Shut down because the process was signalled. Saves what a machine would
	   not want to lose and closes, without writing a snapshot - see
	   closing_for_signal_. */
	void CloseForSignal();

	/* Reset the running machine because the process was signalled. Does
	   nothing when no machine is running, there being nothing to reset. */
	void ResetForSignal();

	bool IsWindowActive() const { return window_active_; }
	bool IsFullScreen() const { return full_screen_; }

	bool IsGuiThread() const override;
	void PostVideoUpdate(VideoUpdate update) override;
	void PostError(const std::string &message) override;
	void PostFatal(const std::string &message) override;
	void PostMoveHostMouse(const MouseMoveUpdate &update) override;
	void ShowError(const std::string &message) override;
	void ShowFatal(const std::string &message) override;
	void PostNatRule(PortForwardRule rule) override;
	void PostDebuggerStateChanged() override;
	void PostMachineSwitched(const std::string &machine_name) override;
	void PostGuestCommandResult(unsigned token, unsigned rc,
	                            const std::string &output, bool ok) override;
	void PostQuit() override;
	void PostSetHostClipboard(const std::string &utf8) override;
	void PostSetHostClipboardImage(int file_type, const std::string &bytes) override;

private:
	void OnClose(wxCloseEvent &event);
	void OnExit(wxCommandEvent &event);
	void OnScreenshot(wxCommandEvent &event);
	void OnAboutRiscos(wxCommandEvent &event);
	void OnReset(wxCommandEvent &event);
	void OnSaveState(wxCommandEvent &event);
	void OnLoadState(wxCommandEvent &event);
	void OnSuspend(wxCommandEvent &event);
	void OnRecentMachine(wxCommandEvent &event);
	void OnClearRecentMachines(wxCommandEvent &event);
	void OnLoadDisc0(wxCommandEvent &event);
	void OnLoadDisc1(wxCommandEvent &event);
	void OnEjectDisc0(wxCommandEvent &event);
	void OnEjectDisc1(wxCommandEvent &event);
	void OnCreateDisc0(wxCommandEvent &event);
	void OnCreateDisc1(wxCommandEvent &event);
	void OnRecentFloppy(wxCommandEvent &event);
	void OnClearRecentFloppies(wxCommandEvent &event);
	void OnCdromDisabled(wxCommandEvent &event);
	void OnCdromEmpty(wxCommandEvent &event);
	void OnCdromIso(wxCommandEvent &event);
	void OnCdromIoctl(wxCommandEvent &event);
	void OnRecentCdrom(wxCommandEvent &event);
	void OnClearRecentCdroms(wxCommandEvent &event);
	void OnMachine(wxCommandEvent &event);
	void OnNatList(wxCommandEvent &event);
	void OnMute(wxCommandEvent &event);
	void OnFullscreen(wxCommandEvent &event);
	void OnIntegerScaling(wxCommandEvent &event);
	void OnFitToWindow(wxCommandEvent &event);
	void OnFollowHostDisplay(wxCommandEvent &event);
	void OnSuspendOnExit(wxCommandEvent &event);
	void OnCpuIdle(wxCommandEvent &event);
	void OnMouseHack(wxCommandEvent &event);
	void OnMouseTwobutton(wxCommandEvent &event);
	void OnSharedClipboard(wxCommandEvent &event);
	void OnDefaultMachine(wxCommandEvent &event);
	void OnDebugRun(wxCommandEvent &event);
	void OnDebugPause(wxCommandEvent &event);
	void OnDebugStep(wxCommandEvent &event);
	void OnDebugStep5(wxCommandEvent &event);
	void OnMachineInspector(wxCommandEvent &event);
	void OnOnlineManual(wxCommandEvent &event);
	void OnVisitWebsite(wxCommandEvent &event);
	void OnAbout(wxCommandEvent &event);
#ifdef RPCEMU_VNC
	void OnVnc(wxCommandEvent &event);
#endif
	void OnPackages(wxCommandEvent &event);
	void OnUsb(wxCommandEvent &event);
	void OnSerial(wxCommandEvent &event);
	void OnParallel(wxCommandEvent &event);

	void OnKeyDown(wxKeyEvent &event);
	void OnKeyUp(wxKeyEvent &event);
	void OnActivate(wxActivateEvent &event);
	void OnDisplayChanged(wxDisplayChangedEvent &event);
	void OnMenuOpen(wxMenuEvent &event);
	void OnMenuClose(wxMenuEvent &event);
	void OnLeftDown(wxMouseEvent &event);
	void OnMipsTimer(wxTimerEvent &event);
	void OnClipboardTimer(wxTimerEvent &event);
	void OnVideoTimer(wxTimerEvent &event);
	void OnFdcLedTimer(wxTimerEvent &event);
	void OnIdeLedTimer(wxTimerEvent &event);
	void OnHostfsLedTimer(wxTimerEvent &event);
	void OnNetworkLedTimer(wxTimerEvent &event);

	void ProcessEmulatorKeyEvent(wxKeyEvent &event, bool key_down);
	void ExitFullScreen();
	void EnterFullScreen();
	void ApplyFitToWindowSize();

	/* The running machine's configuration file name, without .cfg: what the
	   default-machine preference is keyed on. */
	wxString CurrentMachineBaseName() const;

	void BuildMenus();
	void BuildToolBar();
	void BuildStatusBar();
	void BindMenuOpenClose(wxMenu *menu);
	void BindAllMenuOpenCloseHandlers();

	void UpdateRecentMachinesMenu();
	void UpdateRecentFloppiesMenu();
	void UpdateRecentCdromsMenu();
	void SyncCdromMenuChecks();
	void CdromMenuSelectionUpdate(int menu_id);
	void UpdateDebuggerActionStates();
	void SyncSettingsMenuChecks();

	void LoadDisc(int drive);
	void CreateDisc(int drive);
	void EditMachineConfiguration();
	void ShutdownEmulator();
	void ReleaseHeldKeys();
	void NativeKeyPress(unsigned scan_code);
	void NativeKeyRelease(unsigned scan_code);

	wxString BlankDiscResourcePath(const wxString &filename) const;
	wxString ConfigPathForMachine(const wxString &machine_name) const;
	void RestartMachine();

	Config config_copy_{};
	Model model_copy_ = Model_RPCARM710;
	std::unique_ptr<EmulatorHost> emulator_;
	std::unique_ptr<NatListDialog> nat_list_dialog_;
	MachineInspectorWindow *machine_inspector_window_ = nullptr;
#ifdef RPCEMU_VNC
	std::unique_ptr<VncServer> vnc_server_;
#endif
	EmulatorPanel *panel_ = nullptr;
	wxToolBar *tool_bar_ = nullptr;

	wxMenu *recent_machines_menu_ = nullptr;
	wxMenu *recent_floppies_menu_ = nullptr;
	wxMenu *recent_cdroms_menu_ = nullptr;
	wxMenuItem *mute_menu_item_ = nullptr;
	wxMenuItem *fullscreen_menu_item_ = nullptr;
	wxMenuItem *integer_scaling_menu_item_ = nullptr;
	wxMenuItem *fit_to_window_menu_item_ = nullptr;
	wxMenuItem *follow_host_display_menu_item_ = nullptr;
	wxMenuItem *suspend_on_exit_menu_item_ = nullptr;
	wxMenuItem *cpu_idle_menu_item_ = nullptr;
	wxMenuItem *mouse_hack_menu_item_ = nullptr;
	wxMenuItem *mouse_twobutton_menu_item_ = nullptr;
	wxMenuItem *shared_clipboard_menu_item_ = nullptr;
	wxMenuItem *default_machine_menu_item_ = nullptr;
	wxMenuItem *cdrom_disabled_item_ = nullptr;
	wxMenuItem *cdrom_empty_item_ = nullptr;
	wxMenuItem *cdrom_iso_item_ = nullptr;
	wxMenuItem *cdrom_ioctl_item_ = nullptr;
	wxMenuItem *debug_run_item_ = nullptr;
	wxMenuItem *debug_pause_item_ = nullptr;
	wxMenuItem *debug_step_item_ = nullptr;
	wxMenuItem *debug_step5_item_ = nullptr;
	wxMenuItem *nat_list_item_ = nullptr;
	wxToolBarToolBase *tb_mute_tool_ = nullptr;

	bool shutting_down_ = false;
	bool suspend_on_exit_requested_ = false;
	/* Set when the window is closing because the process was signalled rather
	   than because the user asked. A signal is a way out in a hurry, so the
	   close it triggers saves what would otherwise be lost - CMOS, disc images,
	   the configuration - and skips the machine snapshot, which can run to
	   hundreds of megabytes and may not finish before the process is killed. */
	bool closing_for_signal_ = false;
	/* Set as soon as a fatal error is raised (possibly from the emulator
	   thread, which then spins forever and can no longer service commands).
	   Guards the save-on-exit so it never blocks trying to snapshot a machine
	   that has already failed. */
	std::atomic<bool> fatal_occurred_{false};
	bool menu_open_ = false;
	bool window_active_ = false;
	bool full_screen_ = false;
	bool reenable_mousehack_ = false;
	std::list<unsigned> held_keys_;

	wxTimer mips_timer_;
	wxTimer video_timer_;
	wxTimer fdc_led_timer_;
	wxTimer ide_led_timer_;
	wxTimer hostfs_led_timer_;
	wxTimer network_led_timer_;
	wxTimer clipboard_timer_;
	wxString clipboard_last_seen_;	/* host text already sent to the guest */
	std::string clipboard_image_last_seen_;	/* and the same for an image, as PNG */

	uint64_t mips_total_instructions_ = 0;
	unsigned long last_idle_ticks_ = 0;
	int32_t mips_seconds_ = 0;

	wxDECLARE_EVENT_TABLE();
};

#endif
