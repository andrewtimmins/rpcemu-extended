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
#include "machine_ipc.h"

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
	ID_MENU_NETCAP,
	ID_MENU_MUTE,
	ID_MENU_FULLSCREEN,
	/* Show In Window: one drawing rule, in DisplayScaling order. */
	ID_MENU_SCALING_ACTUAL,
	ID_MENU_SCALING_MULTIPLES,
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
	ID_MENU_NETWORK_ANALYSER,
	ID_MENU_ONLINE_MANUAL,
	ID_MENU_VISIT_WEBSITE,
	ID_MENU_REPORT_ISSUE,
	ID_MENU_SUPPORT_BUNDLE,
	ID_MENU_CHECK_UPDATE,
};

/*
 * These ids cross a process boundary: the Manager forwards a menu command to a
 * machine by id (machine_ipc.h), so the two processes must agree on what each
 * one means. Append to the enum above rather than inserting into it, or an old
 * Manager talking to a newer machine sends one command and gets another.
 */
constexpr int kForwardableFirst = ID_MENU_SCREENSHOT;
constexpr int kForwardableLast = ID_MENU_CHECK_UPDATE;

/* This window's own, and deliberately outside the range above: it is about the
   window rather than the machine, so there is nothing to forward. */
enum LocalMenuId {
	ID_MENU_MINIMAL_UI = ID_MENU_CHECK_UPDATE + 1,
};

/*
 * RISC OS Screen Size: a run of consecutive ids, since the list is built from the
 * modes this machine's display memory can hold and so is not known at compile
 * time. ID_MENU_SCREEN_FIXED_LAST bounds the run for the range binding.
 *
 * Deliberately NOT in the forwardable enum above, for two reasons. Sixty-four
 * reserved ids inside a range two processes must agree on is a lot of room to
 * give up, and it pushed that range into the timer ids - which the static_assert
 * below caught. And the meaning of each id here depends on what the machine's
 * display memory can hold, so it is not a stable command that one process can
 * send another in the first place.
 */
enum ScreenSizeMenuId {
	ID_MENU_SCREEN_FIXED_FIRST = wxID_HIGHEST + 400,
	ID_MENU_SCREEN_FIXED_LAST = ID_MENU_SCREEN_FIXED_FIRST + 63,
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
	ID_TIMER_MODE_VERIFY,
	ID_TIMER_GUEST_RESIZE,
	ID_TIMER_TEST_CLOSE,
	ID_TIMER_TEST_FULLSCREEN,
};

/* The menu ids are bound as a range, so they must not grow into the timers. */
static_assert(kForwardableLast < ID_TIMER_MIPS,
    "menu command ids have run into the timer ids");

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

	/*
	 * Run as a machine owned by the Manager window rather than shown as its
	 * own top-level window: publish video into a shared-memory framebuffer
	 * and accept input/control requests over a local socket instead of (or
	 * rather, in addition to - both keep working) taking them from this
	 * process's own wxFrame. Call once, right after construction and before
	 * Show()/StartEmulator(); see main.cpp's --managed handling.
	 */
	void EnableManagedMode();
	bool IsManagedMode() const { return managed_mode_; }

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
	void PostPointerShape(const PointerShape &shape) override;

	/*
	 * Prepare a window this machine is about to show: name the Manager's window
	 * as its owner so it opens in front, and say which machine it belongs to.
	 * Both matter only for a managed machine - one in its own window is already
	 * identified by that window, and its dialogues are parented to something
	 * visible.
	 */
	void PrepareMachineWindow(wxWindow *window, const wxString &what);
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

	/**
	 * Ask before shutting a running machine down, offering the machine list as
	 * an alternative to quitting.
	 *
	 * @return true to carry on closing, false to leave the machine running
	 *         (the caller must veto the close event)
	 */
	bool ConfirmCloseOrSwitch();

	/** Ask whether to close, then offer the machine list. */
	void AskAboutClosing();

	/** This machine's name for a dialogue title, or the application's. */
	wxString MachineDisplayName() const;
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
	void OnMinimalUi(wxCommandEvent &event);
	void ApplyMinimalUi(bool minimal);
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
	void OnNetcap(wxCommandEvent &event);
	void OnNetworkAnalyser(wxCommandEvent &event);
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
	 * Centre the window on its display, keeping the title bar reachable.
	 *
	 * Called after the window's size changes and at no other time, so the window
	 * never moves while the guest is doing something.
	 */
	void CentreWindowOnScreen();

	/** Make the window the size of the guest's desktop, and centre it. */
	void SizeWindowToGuest();

	/**
	 * Rebuild the entries in the Screen Size menu.
	 *
	 * The list is what the machine's display memory can hold, so it changes when
	 * the graphics card is fitted or the VRAM altered, not just at startup.
	 */
	void RebuildScreenSizeMenu();
	void ApplyScreenSize(unsigned want_x, unsigned want_y);
	wxSize CurrentGuestScreenSize() const;

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


	/**
	 * Ask the guest for a screen size, and check that it takes it.
	 *
	 * @param width    Requested width in pixels
	 * @param height   Requested height
	 * @param explicit_choice True when the user named this size, so a refusal
	 *                 should be reported rather than silently worked around.
	 */
	void RequestGuestMode(unsigned width, unsigned height, bool explicit_choice);
	void OnModeVerifyTimer(wxTimerEvent &event);
	void OnGuestResizeTimer(wxTimerEvent &event);

	/**
	 * RPCEMU_TEST_CLOSE_AFTER: ask the window to close, as the close button does.
	 *
	 * There to make the close flow testable without a pointer. The dialogues it
	 * raises cannot be driven by a script here (macOS accessibility refuses to see
	 * the process), but this proves the part that was actually broken: that the
	 * question is asked at all and waits for an answer, rather than answering
	 * itself and shutting the machine down. See AskAboutClosing().
	 */
	void OnTestCloseTimer(wxTimerEvent &event);

	/**
	 * RPCEMU_TEST_FULLSCREEN_AFTER: enter full screen, then leave it again, and
	 * log the window's size at each step.
	 *
	 * Full screen needs a real display, so this cannot be driven headless, and
	 * the dialogues and key handling cannot be driven by a script here. What it
	 * does prove is the part that broke: that leaving full screen puts the window
	 * back to the size of the guest's desktop rather than leaving it filling the
	 * screen.
	 */
	void OnTestFullscreenTimer(wxTimerEvent &event);

	/**
	 * A frame arrived from the guest. Notices a change of desktop size and, for
	 * ScreenSize_MatchWindow, waits for it to settle before acting.
	 */
	void NoteGuestFrame();

	/** Repaint the panel from the retained frame, after the current event. */
	void ForcePanelRedraw();


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

	void MirrorToSharedFramebuffer(const VideoUpdate &update);
	void HandleIpcRequest(const IpcRequest &request);

	/* Menu commands forwarded from the Manager, and the tick-box state sent
	   back so its copies of them can agree. See machine_ipc.h. */
	void DispatchMenuCommand(int id, bool checked, const wxString &argument,
	                         int filter = 0);
	bool AskForFile(const wxString &title, const wxString &wildcard, bool save,
	                wxString *path, const wxString &default_dir = wxEmptyString,
	                const wxString &default_file = wxEmptyString);
	void ReportMenuState();

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
	class NetworkAnalyserWindow *network_analyser_window_ = nullptr;
#ifdef RPCEMU_VNC
	/* Borrowed from vnc_app: the process owns the server, this window only points
	   a machine at it. Not owning it is what lets a client stay connected while
	   machines start and stop. */
	VncServer *vnc_server_ = nullptr;
#endif
	EmulatorPanel *panel_ = nullptr;
	wxToolBar *tool_bar_ = nullptr;

	/* Managed-mode plumbing (see EnableManagedMode()). Null/false for an
	   ordinary single-window launch. */
	bool managed_mode_ = false;
	/* The Manager's window, for windows this machine opens to sit above. Zero
	   until it says, and on the platforms where there is no such thing. */
	uint64_t owner_window_id_ = 0;

	/* A string argument sent with a forwarded menu command - a file the
	   Manager's own dialogue chose, for instance. Set for the duration of the
	   handler and cleared afterwards, so a handler that runs from a real menu
	   click sees it empty and asks the user as it always has. */
	wxString pending_menu_argument_;

	/* Which entry of a forwarded command's file-type list was chosen. Only
	   Create Disc needs it: the disc type comes from the dialogue's filter
	   rather than from the file name. */
	int pending_menu_filter_ = 0;
	/*
	 * Frame buffers handed to the GUI thread, reused rather than allocated.
	 *
	 * A frame used to be a fresh std::vector every time, which at 1600x1200 is
	 * 7.3MB and at 2560x1440 is 14.7MB - above glibc's mmap threshold, so each
	 * frame took a new mapping, faulted in its pages and gave them back again,
	 * sixty times a second, on the VIDC thread and inside video_mutex. That was
	 * 439MB/s measured on a screen that was not changing, and drawscr() drops a
	 * frame outright when its trylock fails, so the cost is paid in lost frames
	 * on a host that cannot keep up.
	 *
	 * A slot is only refilled with the rows that changed, but every slot handed
	 * out still holds a COMPLETE frame, because EmulatorPanel reads all of it
	 * when the geometry changes. That is what stale_top/stale_bottom are for: a
	 * slot out on loan misses the rows other frames dirtied meanwhile, so it
	 * carries the union of those ranges and catches up on them when it is next
	 * filled.
	 *
	 * Three slots because the GUI thread consumes frames promptly; if all three
	 * are still out, the frame falls back to an allocation of its own rather
	 * than stalling the VIDC thread.
	 */
	struct FrameSlot {
		std::vector<uint32_t> pixels;
		std::atomic<bool> in_use{false};
		/* Rows this slot has not been given yet, as [top, bottom). */
		int stale_top = 0;
		int stale_bottom = 0;
		bool stale_all = true;
	};
	static constexpr size_t kFramePoolSize = 3;
	std::unique_ptr<FrameSlot> frame_pool_[kFramePoolSize];
	int frame_pool_width_ = 0;
	int frame_pool_height_ = 0;

	std::unique_ptr<SharedFramebuffer> shared_fb_;
	std::unique_ptr<MachineIpcServer> ipc_server_;

	wxMenu *recent_machines_menu_ = nullptr;
	wxMenu *recent_floppies_menu_ = nullptr;
	wxMenu *recent_cdroms_menu_ = nullptr;
	wxMenuItem *mute_menu_item_ = nullptr;
	wxMenuItem *fullscreen_menu_item_ = nullptr;
	wxMenuItem *minimal_ui_item_ = nullptr;
	/* Indexed by DisplayScaling, so a value out of the configuration selects the
	   right item directly. */
	wxMenuItem *scaling_menu_items_[2] = { nullptr, nullptr };
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
	wxMenuItem *netcap_item_ = nullptr;
	wxToolBarToolBase *tb_mute_tool_ = nullptr;

	bool shutting_down_ = false;

	/* Set once the user has confirmed closing, so the close that follows is not
	   questioned all over again. */
	bool close_confirmed_ = false;

	/* A close question is already on screen. One click on the close button can
	   raise more than one close event, and each was queueing another question. */
	bool close_question_pending_ = false;
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

	/* The guest's screen size, as the frames say it is. Only kept for a managed
	   machine, whose panel is never given a frame to learn it from, and written
	   on the VIDC thread - hence atomic. */
	std::atomic<int> managed_guest_x_{0};
	std::atomic<int> managed_guest_y_{0};
	bool menu_open_ = false;
	bool window_active_ = false;
	bool full_screen_ = false;
	bool minimal_ui_ = false;
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

	/*
	 * Waiting to see whether the guest adopted the mode it was asked for.
	 *
	 * RISC OS refuses any mode its monitor definition does not declare, and says
	 * so only on its own screen - the host is told nothing. So the request is
	 * checked instead of trusted: if the desktop has not become the requested
	 * size by the time this fires, that mode is marked unavailable and the next
	 * one down is tried. See VerifyRequestedMode().
	 */
	wxTimer mode_verify_timer_;

	/*
	 * Waiting for the guest's screen mode to stop changing.
	 *
	 * RISC OS changes mode two or three times on its way to a desktop, and on a
	 * machine with the graphics card the display is handed over part way through
	 * as well. The window has to end up the size of the desktop - smaller clips
	 * it and loses the icon bar, larger leaves a border - but resizing on every
	 * one of those changes made it jump through three sizes and positions before
	 * settling. So each change restarts this, and the window is sized once the
	 * changes stop.
	 */
	wxTimer guest_resize_timer_;

	/* RPCEMU_TEST_CLOSE_AFTER only; never started otherwise. */
	wxTimer test_close_timer_;

	/* RPCEMU_TEST_FULLSCREEN_AFTER only. */
	wxTimer test_fullscreen_timer_;
	int test_fullscreen_step_ = 0;

	/** The size last asked of the guest, or (0,0) when nothing is outstanding. */
	wxSize mode_requested_ = wxSize(0, 0);



	/*
	 * The guest desktop size this window has already reacted to.
	 *
	 * Two jobs. With ScreenSize_Fixed it stops the window being resized over and
	 * over for frames that say nothing new. With ScreenSize_MatchWindow it tells
	 * the two directions apart: a change the guest made for its own reasons - and
	 * RISC OS makes several while it boots - means the window should follow it,
	 * while a window that has moved with the guest standing still means the user
	 * dragged an edge and the guest should follow instead. Without that, the boot
	 * sequence read as the second case and the frame's own layout events
	 * published an intermediate 800x520, driving the guest to 640x480 before the
	 * desktop had even appeared.
	 */
	wxSize guest_size_seen_ = wxSize(0, 0);


	wxString clipboard_last_seen_;	/* host text already sent to the guest */
	std::string clipboard_image_last_seen_;	/* and the same for an image, as PNG */

	uint64_t mips_total_instructions_ = 0;
	unsigned long last_idle_ticks_ = 0;
	int32_t mips_seconds_ = 0;

	wxDECLARE_EVENT_TABLE();
};

#endif
