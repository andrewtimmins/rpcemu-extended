/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

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

#ifndef REMOTE_EMULATOR_PANEL_H
#define REMOTE_EMULATOR_PANEL_H

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

#include <wx/timer.h>
#include <wx/wx.h>

#include "captured_pointer.h"
#include "gl_display_canvas.h"
#include "guest_cursor.h"
#include "machine_ipc.h"

extern "C" {
#include "held_keys.h"
}

/*
 * The Manager's view of one running machine: the ManagerFrame counterpart of
 * EmulatorPanel, reading pixels out of a SharedFramebuffer instead of
 * receiving them in-process, and sending input over a MachineIpcClient
 * instead of calling EmulatorHost directly.
 *
 * Scope note: this reproduces both of EmulatorPanel's mouse modes and its 1:1
 * keyboard forwarding. Absolute ("mouse follows host pointer" / mousehackon)
 * covers ordinary use; captured mode - a click pins the host pointer to the
 * middle of the picture and RISC OS is sent movements instead of positions - is
 * what games that drive the pointer themselves need. Which one is in force is
 * the machine's business, not this window's: it arrives in the machine's own
 * StateReport, via SetFollowHostMouse(). It does not offer integer-scaling /
 * fit-to-window as separate modes; it always scales to fill the panel.
 */
class RemoteEmulatorPanel : public wxPanel {
public:
	/*
	 * `shared_fb_name` and `ipc_endpoint` come from machine_lock's IPC
	 * fields (see machine_lock_read_ipc_endpoint) for a machine already
	 * running, or are derived directly (MachineIpcNameFor) right after
	 * spawning one. Construction attempts to open/connect immediately;
	 * check IsLive() afterwards; a false result means the machine is not
	 * actually reachable (a stale lock file left by a crash, most likely)
	 * and the caller should treat it as not running.
	 */
	RemoteEmulatorPanel(wxWindow *parent, const std::string &shared_fb_name,
	                     const std::string &ipc_endpoint);
	~RemoteEmulatorPanel() override;

	bool IsLive() const { return live_; }

	/* Why IsLive() is false, for a caller with a window to say so in. */
	const wxString &AttachError() const { return attach_error_; }

	/* Drop the connection to the machine. Safe to call twice; the destructor
	   does it too. */
	void CloseConnection();

	/* Whether this is the machine currently shown to the user. Only an
	   active panel pulls new frames out of shared memory and repaints;
	   backgrounded machines keep running (this process has no say in
	   that - it is entirely up to their own, unrelated process) but cost
	   the Manager nothing while not being looked at. */
	void SetActive(bool active);

	void SendRequest(const IpcRequest &request) { ipc_client_.Send(request); }

	/* Write what the machine is currently showing, at its own resolution.
	   Done here rather than by asking the machine: a managed machine's own
	   panel never receives frames (they go to shared memory instead), so it
	   would screenshot an empty window. */
	bool SaveScreenshot(const wxString &path);

	/* Called by ManagerFrame when the underlying machine has gone away
	   (Fatal/Quit event, or the child process itself exiting). */
	using GoneCallback = std::function<void()>;
	void SetGoneCallback(GoneCallback callback) { on_gone_ = std::move(callback); }

	/* Called with the machine's tick-box menu state, so the Manager's copies
	   of those items can agree with it. Runs on the GUI thread. */
	using StateCallback = std::function<void(const wxString &)>;
	void SetStateCallback(StateCallback callback) { on_state_ = std::move(callback); }

	/* Called with an error the machine could not show itself, having no
	   window. Runs on the GUI thread. */
	using ErrorCallback = std::function<void(const wxString &)>;
	void SetErrorCallback(ErrorCallback callback) { on_error_ = std::move(callback); }

	/*
	 * Asked to leave full screen, on Alt+Enter. Returns true if it did, in
	 * which case the key is not passed to the guest; false means there was no
	 * full screen to leave and RISC OS should have the key after all.
	 */
	/* Act on Settings... having been changed while this machine is running. */
	void SetHardwareAcceleration(bool enabled);

	/*
	 * Which mouse mode the machine is in, as the machine itself reports it (see
	 * IpcEventType::StateReport). True is the ordinary "pointer follows the host
	 * one" mode; false means the machine expects movements, which this panel
	 * only starts sending once the user has clicked to capture.
	 *
	 * Asked rather than assumed because the setting belongs to the machine and
	 * survives in its config: a machine left in capture mode is still in it the
	 * next time the Manager shows it.
	 */
	void SetFollowHostMouse(bool follow);

	/*
	 * Give the mouse back, if this panel has it. Answers whether it did, so a
	 * caller handling Alt+Enter can tell whether the key has been used up.
	 */
	bool ReleaseCapturedPointer();

	/* The two answers a window with a status bar needs to say how the mouse is
	   behaving and what to press. */
	bool PointerCaptured() const { return pointer_captured_; }
	bool FollowHostMouse() const { return follow_host_mouse_; }

	/*
	 * How fast the machine is running, for a window that wants to show it.
	 *
	 * MIPS comes from the machine itself, once a second
	 * (IpcEventType::PerfReport). Frames per second is counted here instead,
	 * from the FrameReady events the machine sends anyway - one per frame it
	 * draws - over the second between two reports.
	 *
	 * HasPerf() is false until the first report arrives - which is up to a
	 * second after a machine starts, and never at all from one built before the
	 * event existed - so a caller can show nothing rather than a confident zero.
	 */
	bool HasPerf() const { return has_perf_.load(std::memory_order_relaxed); }
	float Mips() const { return mips_.load(std::memory_order_relaxed); }
	float Fps() const { return fps_.load(std::memory_order_relaxed); }
	bool GuestIdle() const { return guest_idle_.load(std::memory_order_relaxed); }

	using LeaveFullScreenCallback = std::function<bool()>;
	void SetLeaveFullScreenCallback(LeaveFullScreenCallback callback)
	{
		on_leave_full_screen_ = std::move(callback);
	}

	/*
	 * The pointer has been captured or given back. Called because capture begins
	 * with a click inside this panel, which the window around it never sees, and
	 * that window is the one with a status bar to say how to escape.
	 */
	using CaptureChangedCallback = std::function<void()>;
	void SetCaptureChangedCallback(CaptureChangedCallback callback)
	{
		on_capture_changed_ = std::move(callback);
	}

	/*
	 * The machine has asked for the window showing it to be brought to the front
	 * (IpcEventType::ManagerActivate). Runs on the GUI thread.
	 */
	using ActivateRequestCallback = std::function<void()>;
	void SetActivateRequestCallback(ActivateRequestCallback callback)
	{
		on_activate_request_ = std::move(callback);
	}

private:
	void OnPaint(wxPaintEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnEraseBackground(wxEraseEvent &event);
	void OnMouseMove(wxMouseEvent &event);
	void OnMouseDown(wxMouseEvent &event);
	void OnMouseUp(wxMouseEvent &event);
	void OnMouseWheel(wxMouseEvent &event);
	void OnKeyDown(wxKeyEvent &event);
	void OnKeyUp(wxKeyEvent &event);
	void OnKillFocus(wxFocusEvent &event);

	void HandleIpcEvent(const IpcEvent &event);
	void RefreshFrame();
	void UpdateCursor();
	int MapClickButton(const wxMouseEvent &event) const;
	wxPoint PanelPointToGuest(int x, int y) const;

	/* Take the pointer, on a click in captured mode, at the point clicked. */
	void CaptureThePointer(int x, int y);

	/* Where the pointer is put back to when it nears an edge: the middle of the
	   picture, not of the panel, so it lands inside the guest's screen rather
	   than in the bars beside it. */
	wxPoint CaptureCentre() const;

	/* Send the machine how far the pointer has moved since it was last seen,
	   re-centring it if it is getting close to an edge. */
	void SendCapturedMotion(const wxMouseEvent &event);

	/* Movements and re-centres in the log every few seconds, so a report of
	   captured mode feeling slow can be answered with a number. */
	void ReportCapturedPointerRate();

	/*
	 * Where in the panel the guest's screen is actually drawn: aspect-fitted
	 * and centred, so there are bars down two sides whenever the panel is a
	 * different shape from the guest's screen.
	 *
	 * One function, used by both the painting and the pointer mapping. They
	 * disagreeing is what made the guest pointer travel at the wrong speed:
	 * the paint letterboxed the picture while the mapping measured against
	 * the whole panel.
	 */
	wxRect DisplayRect() const;

	/*
	 * The guest's screen, scaled to dw x dh and ready to blit.
	 *
	 * Only the rows the guest has redrawn since the last paint are rescaled;
	 * the rest of the image is still there from before. A change of size, of
	 * guest screen mode, or of filter redoes all of it.
	 */
	wxBitmap &ScaledBitmap(int dw, int dh);

	/* The guest's screen at its own size, converted on demand. */
	wxBitmap &FullBitmap();

	/*
	 * Whether the user is moving something about right now.
	 *
	 * While they are, the scaling is nearest-neighbour, which costs what the
	 * panel costs rather than what the guest's screen costs; when they stop,
	 * a timer fires one repaint at the box-average quality. Pointer events are
	 * handled on the thread that paints, so a cheaper paint while dragging is
	 * a pointer that keeps up - which is the whole reason for the distinction.
	 */
	bool Interacting() const;
	void NoteInput();
	void OnSettleTimer(wxTimerEvent &event);

	/*
	 * The GPU path, if this machine can have one.
	 *
	 * Tried once, on the first paint that has a frame to show. When it works the
	 * canvas covers this panel and does the drawing; when it does not - no GLX
	 * on a remote display, a context that will not create, a driver missing
	 * something - it is destroyed and the CPU path below carries on as though it
	 * had never been attempted. That fallback is not optional: a display that
	 * can fail to start must never leave the user with a black window.
	 *
	 * Input still belongs to this panel. The canvas sits on top, so its mouse
	 * and key events are bound straight to this panel's handlers rather than
	 * duplicated, and it fills the panel exactly so the coordinates need no
	 * adjustment.
	 */
	/* Hand the newest frame to the OpenGL canvas. Called by the canvas from
	   inside its own paint, never on frame arrival - see
	   GlDisplayCanvas::SetFrameSupplier. */
	void UploadPendingFrame();

	void TryCreateGlCanvas();
	void DestroyGlCanvas(const wxString &why);
	bool GlActive() const;

	SharedFramebuffer shared_fb_;
	MachineIpcClient ipc_client_;
	/* Set when the shared framebuffer or the panel size has changed, cleared
	   when the scaled bitmap is rebuilt in OnPaint. See HandleIpcEvent. */
	bool frame_dirty_ = true;

	GoneCallback on_gone_;
	StateCallback on_state_;
	ErrorCallback on_error_;
	LeaveFullScreenCallback on_leave_full_screen_;
	CaptureChangedCallback on_capture_changed_;
	ActivateRequestCallback on_activate_request_;

	/*
	 * The mouse mode, and whether the pointer has been captured in it.
	 *
	 * follow_host_mouse_ starts true so a machine whose state has not arrived yet
	 * behaves as it always has, rather than ignoring the mouse until it does.
	 * captured_pointer_ holds where the pointer was last seen and the
	 * sub-pixel remainder; see captured_pointer.h.
	 */
	bool follow_host_mouse_ = true;
	bool pointer_captured_ = false;
	struct captured_pointer captured_pointer_{};

	bool live_ = false;
	wxString attach_error_;
	bool active_ = false;

	/*
	 * The speed figures behind HasPerf()/Mips()/Fps().
	 *
	 * Atomic because they are written on the IPC reader thread, where frames
	 * and perf reports arrive, and read on the GUI thread when a status bar is
	 * being filled in. They are four independent numbers shown together and
	 * never compared with each other, so a torn set is not worth a lock: the
	 * worst case is one of them being a second out of date.
	 *
	 * frames_since_report_ counts frames between two reports and perf_at_ms_
	 * is when the last one came, both touched only on the reader thread.
	 */
	std::atomic<bool> has_perf_{false};
	std::atomic<float> mips_{0.0f};
	std::atomic<float> fps_{0.0f};
	std::atomic<bool> guest_idle_{false};
	std::atomic<unsigned> frames_since_report_{0};

	/*
	 * The machine's pointer as this panel's cursor.
	 *
	 * The machine sends the shape when it changes and stops putting the pointer
	 * in the frame; drawn by the host it tracks the hand at the host's own rate
	 * rather than moving once per emulated frame. cursor_host_side_ is the
	 * machine's instruction: false means the pointer is still in the frame and
	 * this panel must not draw one, or there would be two.
	 */
	GuestCursor guest_shape_;
	wxCursor guest_cursor_;
	bool cursor_host_side_ = false;
	bool guest_cursor_ok_ = false;
	int guest_cursor_scale_num_ = 0;
	int guest_cursor_scale_den_ = 0;

	void RebuildGuestCursor();
	wxLongLong perf_at_ms_ = 0;
	std::vector<uint32_t> frame_pixels_;
	int frame_width_ = 640;
	int frame_height_ = 480;
	wxBitmap display_bitmap_;

	/*
	 * The scaled copy, kept between paints so that only what changed has to be
	 * redone. scaled_image_ is the pixels; scaled_bitmap_ is what a wxDC can
	 * blit, rebuilt from it when the image has changed.
	 */
	wxImage scaled_image_;
	wxBitmap scaled_bitmap_;
	int scaled_width_ = 0;
	int scaled_height_ = 0;
	int scaled_from_width_ = 0;	/* the guest screen size it was made from */
	int scaled_from_height_ = 0;
	bool scaled_valid_ = false;
	bool scaled_is_fast_ = false;	/* made with nearest neighbour */
	bool scaled_bitmap_stale_ = true;

	/*
	 * Rows of the guest's screen redrawn since the last paint, as the
	 * half-open range the FrameReady events carry. Several frames can arrive
	 * between two paints - wx coalesces the repaint requests - so this is the
	 * union of what they reported, and it is reset once painted.
	 */
	int dirty_top_ = 0;
	int dirty_bottom_ = 0;
	bool dirty_all_ = true;

	/* Bumped for every frame taken out of shared memory. */
	int frame_serial_ = 0;

	/* When the user last moved or clicked, and the one-shot timer that fires
	   after they stop to repaint at full quality. */
	std::chrono::steady_clock::time_point last_input_;
	int held_buttons_ = 0;
	wxTimer settle_timer_;

#if wxUSE_GLCANVAS
	GlDisplayCanvas *gl_canvas_ = nullptr;
	bool gl_tried_ = false;
	/* Paints spent waiting for a new canvas to say whether it works. See the
	   limit in OnPaint: a canvas that never answers must not mean a window that
	   is never drawn. */
	int gl_undecided_paints_ = 0;
#endif

	HeldKeys held_keys_{};

	wxDECLARE_EVENT_TABLE();
};

#endif /* REMOTE_EMULATOR_PANEL_H */
