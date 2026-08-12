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

#include <chrono>
#include <functional>
#include <vector>

#include <wx/timer.h>
#include <wx/wx.h>

#include "gl_display_canvas.h"
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
 * Scope note: this reproduces EmulatorPanel's absolute ("mouse follows host
 * pointer" / mousehackon) mouse mode and its 1:1 keyboard forwarding, which
 * covers ordinary use. It does not yet reproduce the relative/captured
 * pointer mode EmulatorPanel offers when mousehackon is off (host pointer
 * warped to centre, deltas forwarded) - that needs the remote machine's
 * mousehackon setting mirrored to the Manager first, deferred as follow-up
 * work rather than guessed at here. Nor does it offer integer-scaling /
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

	using LeaveFullScreenCallback = std::function<bool()>;
	void SetLeaveFullScreenCallback(LeaveFullScreenCallback callback)
	{
		on_leave_full_screen_ = std::move(callback);
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

	bool live_ = false;
	wxString attach_error_;
	bool active_ = false;
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
#endif

	HeldKeys held_keys_{};

	wxDECLARE_EVENT_TABLE();
};

#endif /* REMOTE_EMULATOR_PANEL_H */
