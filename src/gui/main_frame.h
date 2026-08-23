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
#include <utility>
#include <vector>

#include <wx/wx.h>

#include "emulator_host.h"
#include "emulator_panel.h"
#include "gui_bridge.h"
#include "gui_preferences.h"
#include "held_keys.h"

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
	/* Show In Window: one drawing rule, in DisplayScaling order. */
	ID_MENU_SCALING_ACTUAL,
	ID_MENU_SCALING_MULTIPLES,
	ID_MENU_SCALING_FIT,
	/* RISC OS Screen Size: where the desktop's size comes from. The fixed sizes
	   are a run of consecutive ids, since the list is built from the standard
	   modes the machine's display memory can hold and so is not fixed at compile
	   time. ID_MENU_SCREEN_FIXED_LAST bounds the run for the range binding. */
	ID_MENU_SCREEN_AUTOMATIC,
	ID_MENU_SCREEN_MATCH_WINDOW,
	ID_MENU_SCREEN_FIXED_FIRST,
	ID_MENU_SCREEN_FIXED_LAST = ID_MENU_SCREEN_FIXED_FIRST + 63,
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
	ID_MENU_REPORT_ISSUE,
	ID_MENU_SUPPORT_BUNDLE,
	ID_MENU_CHECK_UPDATE,
};

enum TimerId {
	ID_TIMER_MIPS = wxID_HIGHEST + 100,
	ID_TIMER_VIDEO,
	ID_TIMER_FDC_LED,
	ID_TIMER_IDE_LED,
	ID_TIMER_HOSTFS_LED,
	ID_TIMER_NETWORK_LED,
	ID_TIMER_CLIPBOARD,
	ID_TIMER_SYNTHETIC_RELEASE,
	ID_TIMER_MATCH_WINDOW,
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
	/* Overrides wxTopLevelWindow's, so wx's own callers see this flag too. */
	bool IsFullScreen() const override { return full_screen_; }

	bool IsGuiThread() const override;
	void PostVideoUpdate(VideoUpdate update) override;
	void PostError(const std::string &message) override;
	void PostFatal(const std::string &message) override;
	void PostMoveHostMouse(const MouseMoveUpdate &update) override;
	void ShowError(const std::string &message) override;
	void ShowFatal(const std::string &message) override;
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
	void OnDisplayScaling(wxCommandEvent &event);
	void OnScreenSize(wxCommandEvent &event);
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
	void OnReportIssue(wxCommandEvent &event);
	void OnSupportBundle(wxCommandEvent &event);
	void OnCheckUpdate(wxCommandEvent &event);
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
	void OnSyntheticReleaseTimer(wxTimerEvent &event);
	void QueueSyntheticRelease(unsigned key_id);
	void OnVideoTimer(wxTimerEvent &event);
	void OnFdcLedTimer(wxTimerEvent &event);
	void OnIdeLedTimer(wxTimerEvent &event);
	void OnHostfsLedTimer(wxTimerEvent &event);
	void OnNetworkLedTimer(wxTimerEvent &event);

	void ProcessEmulatorKeyEvent(wxKeyEvent &event, bool key_down);
	void ExitFullScreen();
	void EnterFullScreen();
	/**
	 * Give a newly-freed window a comfortable size, and re-lay-out either way.
	 *
	 * Called when the window stops being locked to the guest's desktop. Without
	 * it the window keeps whatever size the lock left it at, which for a small
	 * screen mode is a postage stamp the user then has to drag out by hand.
	 */
	void ApplyFreeWindowSize();

	/**
	 * Close the gap between the window and the desktop the guest just moved to.
	 *
	 * For ScreenSize_MatchWindow at actual size only, where the two are meant to
	 * be the same size. Bounded by the display's work area rather than by
	 * ApplyFreeWindowSize()'s opening size, so the desktop is not capped.
	 */
	void SnapWindowToGuest();

	/**
	 * Rebuild the fixed-size entries in the Screen Size menu.
	 *
	 * The list is what the machine's display memory can hold, so it changes when
	 * the graphics card is fitted or the VRAM altered, not just at startup.
	 */
	void RebuildScreenSizeMenu();

	/** True when the window's size is not derived from the guest's desktop. */
	bool WindowSizeIsFree() const;

	/** Push the current scaling and screen-size choices into the panel. */
	void ApplyDisplayModeToPanel();

	/**
	 * Tell the guest the window's size, for ScreenSize_MatchWindow.
	 *
	 * Debounced: a resize drag fires continuously, and a mode change reflows
	 * every window on the RISC OS desktop, so acting on each intermediate size
	 * would leave the guest thrashing through modes it is about to leave. The
	 * timer restarts on every event and only the size the drag settles on is
	 * published.
	 */
	void PublishWindowSizeToGuest();
	void OnMatchWindowTimer(wxTimerEvent &event);

	/**
	 * A frame arrived from the guest. Notices a change of desktop size and, for
	 * ScreenSize_MatchWindow, waits for it to settle before acting.
	 */
	void NoteGuestFrame();

	/** Repaint the panel from the retained frame, after the current event. */
	void ForcePanelRedraw();

	void OnFrameSize(wxSizeEvent &event);

	/* The running machine's configuration file name, without .cfg: what the
	   default-machine preference is keyed on. */
	wxString CurrentMachineBaseName() const;

	void BuildMenus();
	void BuildToolBar();
	void BuildStatusBar();

	/* The timers keep running in full-screen, where there is no status bar to
	   write to - wx asserts on that rather than ignoring it. */
	void SetStatusText(const wxString &text, int number = 0) override;
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
	void NativeKeyPress(unsigned key_id, unsigned scan_code);
	void NativeKeyRelease(unsigned key_id);

	wxString BlankDiscResourcePath(const wxString &filename) const;
	wxString ConfigPathForMachine(const wxString &machine_name) const;
	void RestartMachine();

	Config config_copy_{};
	Model model_copy_ = Model_RPCARM710;
	std::unique_ptr<EmulatorHost> emulator_;
	MachineInspectorWindow *machine_inspector_window_ = nullptr;
#ifdef RPCEMU_VNC
	/* Borrowed from vnc_app: the process owns the server, this window only points
	   a machine at it. Not owning it is what lets a client stay connected while
	   machines start and stop. */
	VncServer *vnc_server_ = nullptr;
#endif
	EmulatorPanel *panel_ = nullptr;
	wxToolBar *tool_bar_ = nullptr;

	wxMenu *recent_machines_menu_ = nullptr;
	wxMenu *recent_floppies_menu_ = nullptr;
	wxMenu *recent_cdroms_menu_ = nullptr;
	wxMenuItem *mute_menu_item_ = nullptr;
	wxMenuItem *fullscreen_menu_item_ = nullptr;
	/* The two radio groups. Indexed by DisplayScaling and ScreenSize, so a
	   value straight out of the configuration selects the right item. */
	wxMenuItem *scaling_menu_items_[3] = { nullptr, nullptr, nullptr };
	wxMenuItem *screen_size_menu_items_[2] = { nullptr, nullptr };
	wxMenu *screen_size_menu_ = nullptr;

	/* The fixed sizes actually offered, in the order they appear in the menu, so
	   an id can be turned back into a mode without re-deriving the list. */
	std::vector<std::pair<unsigned, unsigned>> fixed_mode_items_;
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
	/* Keyed by physical key, not by scancode - see held_keys.h and issue #70. */
	HeldKeys held_keys_ = {};

	wxTimer mips_timer_;
	wxTimer video_timer_;
	wxTimer fdc_led_timer_;
	wxTimer ide_led_timer_;
	wxTimer hostfs_led_timer_;
	wxTimer network_led_timer_;
	wxTimer clipboard_timer_;

	/*
	 * Keys pressed for the guest whose release the host is never going to send,
	 * waiting to be let go of. See ProcessEmulatorKeyEvent(): the release cannot
	 * be sent in the same breath as the press, because then there is no interval
	 * in which the key was held and the guest sees nothing at all.
	 */
	wxTimer synthetic_release_timer_;
	std::vector<unsigned> synthetic_release_pending_;

	/* Waiting for a resize drag to settle, for ScreenSize_MatchWindow. See
	   PublishWindowSizeToGuest(). */
	wxTimer match_window_timer_;

	/*
	 * The guest's desktop size as of the last quiet moment, for
	 * ScreenSize_MatchWindow.
	 *
	 * The point of remembering it is to tell the two directions apart. A change
	 * the guest made for its own reasons - and RISC OS makes several while it
	 * boots - means the window should follow it. A window that has moved while
	 * the guest has stayed put means the user dragged an edge, and the guest
	 * should follow instead. Without this the boot sequence was read as the
	 * second case: the frame's own layout events published an intermediate
	 * 800x520 and drove the guest to 640x480 before the desktop had appeared.
	 */
	wxSize match_window_guest_size_ = wxSize(0, 0);

	wxString clipboard_last_seen_;	/* host text already sent to the guest */
	std::string clipboard_image_last_seen_;	/* and the same for an image, as PNG */

	uint64_t mips_total_instructions_ = 0;
	unsigned long last_idle_ticks_ = 0;
	int32_t mips_seconds_ = 0;

	wxDECLARE_EVENT_TABLE();
};

#endif
