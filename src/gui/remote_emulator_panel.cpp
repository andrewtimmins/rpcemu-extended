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

#include "remote_emulator_panel.h"

#include <algorithm>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <chrono>

#include "gl_display_canvas.h"
#include "gui_preferences.h"
#include "input_helpers.h"
#include "remote_display_geometry.h"
#include "remote_display_scale.h"

extern "C" {
#include "rpcemu.h"
}

wxBEGIN_EVENT_TABLE(RemoteEmulatorPanel, wxPanel)
	EVT_PAINT(RemoteEmulatorPanel::OnPaint)
	EVT_SIZE(RemoteEmulatorPanel::OnSize)
	EVT_ERASE_BACKGROUND(RemoteEmulatorPanel::OnEraseBackground)
	EVT_MOTION(RemoteEmulatorPanel::OnMouseMove)
	EVT_LEFT_DOWN(RemoteEmulatorPanel::OnMouseDown)
	EVT_MIDDLE_DOWN(RemoteEmulatorPanel::OnMouseDown)
	EVT_RIGHT_DOWN(RemoteEmulatorPanel::OnMouseDown)
	EVT_LEFT_UP(RemoteEmulatorPanel::OnMouseUp)
	EVT_MIDDLE_UP(RemoteEmulatorPanel::OnMouseUp)
	EVT_RIGHT_UP(RemoteEmulatorPanel::OnMouseUp)
	/*
	 * The second press of a double click arrives as its own event, not as
	 * another LEFT_DOWN: wx sends down, up, dclick, up. Without these three
	 * that second press was dropped, the guest saw one press and two releases,
	 * and it took three clicks of the host mouse to make two arrive - so
	 * double clicking on an icon mostly did not work. EmulatorPanel has bound
	 * them all along (OnMouseDoubleClick); this panel had not.
	 */
	EVT_LEFT_DCLICK(RemoteEmulatorPanel::OnMouseDown)
	EVT_MIDDLE_DCLICK(RemoteEmulatorPanel::OnMouseDown)
	EVT_RIGHT_DCLICK(RemoteEmulatorPanel::OnMouseDown)
	EVT_MOUSEWHEEL(RemoteEmulatorPanel::OnMouseWheel)
	EVT_KEY_DOWN(RemoteEmulatorPanel::OnKeyDown)
	EVT_KEY_UP(RemoteEmulatorPanel::OnKeyUp)
	EVT_KILL_FOCUS(RemoteEmulatorPanel::OnKillFocus)
	EVT_TIMER(wxID_ANY, RemoteEmulatorPanel::OnSettleTimer)
wxEND_EVENT_TABLE()

RemoteEmulatorPanel::RemoteEmulatorPanel(wxWindow *parent, const std::string &shared_fb_name,
                                         const std::string &ipc_endpoint)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(640, 480), wxWANTS_CHARS)
	, settle_timer_(this)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(*wxBLACK);
	SetCanFocus(true);

	const bool fb_ok = shared_fb_.OpenExisting(shared_fb_name);
	const bool ipc_ok = ipc_client_.Connect(ipc_endpoint, [this](const IpcEvent &event) {
		/* Runs on MachineIpcClient's read thread; hop to the GUI thread
		   before touching any wx state. `event` is a small fixed-size POD,
		   cheap to copy across the CallAfter boundary. */
		CallAfter([this, event]() { HandleIpcEvent(event); });
	});

	live_ = fb_ok && ipc_ok;
	if (!live_) {
		/*
		 * Say which half failed.
		 *
		 * Without this the Manager can only report "did not start" for two
		 * quite different faults - a shared framebuffer it cannot open and a
		 * control socket it cannot reach - and the machine is meanwhile
		 * running perfectly well with neither the user nor the log able to
		 * say why it is not on screen.
		 */
		attach_error_ = wxString::Format(
		    "Framebuffer '%s': %s\nControl socket '%s': %s",
		    wxString::FromUTF8(shared_fb_name), fb_ok ? "opened" : "FAILED",
		    wxString::FromUTF8(ipc_endpoint), ipc_ok ? "connected" : "FAILED");

		rpclog("Manager: cannot attach to a machine: %s\n",
		    attach_error_.utf8_str().data());
		ipc_client_.Disconnect();
		shared_fb_.Close();
	}

	UpdateCursor();
}

void RemoteEmulatorPanel::CloseConnection()
{
	live_ = false;
	/* Before the cursor is put back, or a machine that has gone leaves the
	   pointer pinned to a panel with nothing in it. */
	ReleaseCapturedPointer();
	ipc_client_.Disconnect();
}

RemoteEmulatorPanel::~RemoteEmulatorPanel()
{
	ipc_client_.Disconnect();
}

/*
 * Hide the host arrow while a machine is being shown.
 *
 * Whenever RISC OS is drawing a pointer of its own - which it does in both mouse
 * modes - the host arrow would be a second pointer on screen, one on top of the
 * other. EmulatorPanel blanks it for exactly this reason in UpdateMouseCursor().
 *
 * The arrow comes back when the machine has gone, where the panel is showing a
 * "Machine stopped" message and there is no guest pointer to replace it, and in
 * captured mode until the click that captures.
 */
void RemoteEmulatorPanel::UpdateCursor()
{
	/*
	 * Hidden while there is a guest pointer standing in for it, which is both
	 * mouse modes bar one case: a machine in captured mode that has not been
	 * clicked yet. There the guest pointer is wherever RISC OS left it and the
	 * host one is the only thing tracking the hand, so hiding it would leave the
	 * user with no pointer at all and no clue that a click is what starts it.
	 */
	const bool hide = live_ && (follow_host_mouse_ || pointer_captured_);
	const wxCursor cursor(hide ? wxCURSOR_BLANK : wxCURSOR_ARROW);

	SetCursor(cursor);
#if wxUSE_GLCANVAS
	/* The canvas is the window the pointer is actually over. */
	if (gl_canvas_ != nullptr) {
		gl_canvas_->SetCursor(cursor);
	}
#endif
}

void RemoteEmulatorPanel::SetFollowHostMouse(bool follow)
{
	if (follow == follow_host_mouse_) {
		return;
	}
	follow_host_mouse_ = follow;

	/*
	 * Turning follow-mouse back on while the pointer is captured has to let it
	 * go: the machine is about to expect positions again, and a pointer still
	 * pinned to the middle of the picture could never send one.
	 */
	if (follow_host_mouse_ && pointer_captured_) {
		ReleaseCapturedPointer();
		return;
	}

	UpdateCursor();
}

wxPoint RemoteEmulatorPanel::CaptureCentre() const
{
	const wxRect rect = DisplayRect();

	return wxPoint(rect.x + rect.width / 2, rect.y + rect.height / 2);
}

void RemoteEmulatorPanel::CaptureThePointer(int x, int y)
{
	pointer_captured_ = true;

	/* Measured from where the pointer is, and deliberately not warped to the
	   middle: the click that captures should not make the mouse jump. */
	captured_pointer_begin(&captured_pointer_, x, y);
	UpdateCursor();

	if (on_capture_changed_) {
		on_capture_changed_();
	}
}

/*
 * The counters in the log now and then when asked for, the same as a machine's
 * own window does and sharing the decision with it. Labelled differently
 * because the two windows handle the mouse in different processes, and a report
 * that did not say which would be no use for answering "the Manager feels slow".
 */
void RemoteEmulatorPanel::ReportCapturedPointerRate()
{
	const unsigned long now_ms = (unsigned long)
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count();

	if (captured_pointer_should_report(&captured_pointer_, now_ms)) {
		rpclog("mouse: captured in the Manager - %lu movements, %lu re-centres\n",
		       captured_pointer_.moves, captured_pointer_.recentres);
	}
}

bool RemoteEmulatorPanel::ReleaseCapturedPointer()
{
	if (!pointer_captured_) {
		return false;
	}
	pointer_captured_ = false;
	captured_pointer_end(&captured_pointer_);
	UpdateCursor();

	if (on_capture_changed_) {
		on_capture_changed_();
	}
	return true;
}

/*
 * Movement while the pointer is captured.
 *
 * How far the pointer has come since it was last seen, which is what RISC OS is
 * told about - and the host pointer is put back in the middle of the picture when
 * it nears an edge, so it never runs out of room to move in. The same scheme
 * EmulatorPanel uses in a machine's own window, sharing captured_pointer.h with
 * it, including the reason it is not warped on every event.
 */
void RemoteEmulatorPanel::SendCapturedMotion(const wxMouseEvent &event)
{
	const wxPoint centre = CaptureCentre();
	const wxSize client = GetClientSize();
	int panel_dx = 0, panel_dy = 0, recentre = 0;
	const int moved = captured_pointer_motion(&captured_pointer_,
	    event.GetX(), event.GetY(), client.GetWidth(), client.GetHeight(),
	    centre.x, centre.y, &panel_dx, &panel_dy, &recentre);

	/*
	 * ★ Warped only when the pointer nears an edge - see captured_pointer.h.
	 *
	 * Warping on every event is what made captured mode slower than the ordinary
	 * one, and this panel can afford it least: its motion events are handled on
	 * the same thread that reads the machine's frames out of shared memory and
	 * scales them.
	 */
	if (recentre) {
		WarpPointer(centre.x, centre.y);
	}
	if (!moved) {
		return;
	}

	if (captured_pointer_debug_wanted()) {
		ReportCapturedPointerRate();
	}

	const struct remote_display_rect rect = remote_display_rect_for(
	    client.GetWidth(), client.GetHeight(), frame_width_, frame_height_);
	int guest_dx = 0, guest_dy = 0;

	remote_display_delta_to_guest(rect, frame_width_, frame_height_,
	    panel_dx, panel_dy, &captured_pointer_.carry_x, &captured_pointer_.carry_y,
	    &guest_dx, &guest_dy);

	if (guest_dx == 0 && guest_dy == 0) {
		/* Less than a guest pixel so far; the remainder is keeping it. */
		return;
	}

	IpcRequest request;
	request.type = IpcRequestType::MouseMoveRelative;
	request.arg1 = guest_dx;
	request.arg2 = guest_dy;

	SendRequest(request);
}

void RemoteEmulatorPanel::SetActive(bool active)
{
	active_ = active;

	/* Switching to another machine gives the pointer back: it was captured for
	   this one, and the user is now looking at a different screen. */
	if (!active_) {
		ReleaseCapturedPointer();
	}

	if (active_ && live_) {
		/* Show whatever the machine last drew immediately, rather than
		   waiting for the guest to draw something new after the switch -
		   the shared framebuffer already holds it. */
		frame_dirty_ = true;
		dirty_all_ = true;
		Refresh(false);
#if wxUSE_GLCANVAS
		if (gl_canvas_ != nullptr) {
			gl_canvas_->Refresh(false);
			gl_canvas_->SetFocus();
		} else {
			SetFocus();
		}
#else
		SetFocus();
#endif
	}
}

void RemoteEmulatorPanel::HandleIpcEvent(const IpcEvent &event)
{
	switch (event.type) {
	case IpcEventType::FrameReady:
		if (active_) {
			/*
			 * Union of the rows every frame since the last paint reported.
			 * Several frames arrive between two paints - wx coalesces the
			 * repaint requests, which is deliberate - so painting only the
			 * last frame's rows would leave the ones before it stale.
			 *
			 * A machine that reports an empty or impossible range is taken
			 * to mean the whole screen, which is what happened for every
			 * frame before the range existed.
			 */
			if (event.dirty_bottom > event.dirty_top) {
				if (dirty_bottom_ <= dirty_top_) {
					dirty_top_ = event.dirty_top;
					dirty_bottom_ = event.dirty_bottom;
				} else {
					dirty_top_ = std::min(dirty_top_, (int) event.dirty_top);
					dirty_bottom_ = std::max(dirty_bottom_,
					    (int) event.dirty_bottom);
				}
			} else {
				dirty_all_ = true;
			}

			/*
			 * ★ Mark it stale and ask for a paint; do not build the bitmap
			 * here.
			 *
			 * Building it means a copy out of shared memory, a pass over
			 * every pixel to convert it, a bilinear rescale to the panel
			 * size and then a wxBitmap conversion - four passes over the
			 * whole image, on the GUI thread. Doing that per frame cost a
			 * whole CPU core to show a machine that was sitting idle, and
			 * during a live resize the size events piled on top of the
			 * frames until the window stopped responding.
			 *
			 * wx coalesces Refresh() into one paint, so a burst of frames
			 * now costs one conversion rather than one each.
			 */
			frame_dirty_ = true;

#if wxUSE_GLCANVAS
			/*
			 * ★ With the GPU path the frame goes straight from shared memory
			 * into the texture, here, and only the rows that changed.
			 *
			 * Done on arrival rather than in a paint because there is nothing
			 * expensive to coalesce any more: an upload of a band is a DMA, not
			 * a pass over the picture. The canvas is then asked to redraw, and
			 * wx coalesces THAT as it always did.
			 */
			if (GlActive()) {
				const uint32_t *pixels = nullptr;
				int w = 0, h = 0;

				if (shared_fb_.AcquireFront(&pixels, &w, &h)) {
					frame_width_ = w;
					frame_height_ = h;
					gl_canvas_->SetDisplayRect(DisplayRect());
					gl_canvas_->UpdateFrame(pixels, w, h,
					    dirty_all_ ? 0 : dirty_top_,
					    dirty_all_ ? h : dirty_bottom_);
					dirty_all_ = false;
					dirty_top_ = dirty_bottom_ = 0;
					frame_dirty_ = false;
					gl_canvas_->Refresh(false);
					break;
				}
			}
#endif
			Refresh(false);
		}
		break;
	case IpcEventType::Fatal:
	case IpcEventType::Quit:
		live_ = false;
		UpdateCursor();
		if (on_gone_) {
			on_gone_();
		}
		break;
	case IpcEventType::StateReport:
		if (on_state_) {
			/* HandleIpcEvent runs on the client's reader thread; the
			   callback sets menu items, so it has to reach the GUI
			   thread first. */
			const wxString report = wxString::FromUTF8(event.path);

			CallAfter([this, report] {
				if (on_state_) {
					on_state_(report);
				}
			});
		}
		break;
	case IpcEventType::Error:
		if (on_error_) {
			const wxString text = wxString::FromUTF8(event.path);

			CallAfter([this, text] {
				if (on_error_) {
					on_error_(text);
				}
			});
		}
		break;
	case IpcEventType::TitleChanged:
		/* Nothing shown for this yet in the Manager - see the class
		   comment for what is deferred. */
		break;
	}
}

/*
 * The guest's screen at dw x dh, ready to blit, doing as little as it can.
 *
 * Three things can make work here: a new frame, which only dirties the rows the
 * guest redrew; a change of panel size or guest screen mode, which dirties all
 * of them; and a change of filter as the user starts or stops moving things,
 * which also dirties all of them, because half a picture at each quality looks
 * like a fault.
 *
 * A repaint on its own makes none: an expose, a menu closing over the panel, or
 * a machine sitting at a still desktop all ask for a paint with nothing to
 * recompute.
 */
wxBitmap &RemoteEmulatorPanel::ScaledBitmap(int dw, int dh)
{
	/*
	 * Which filter, and it is not simply "fast while interacting".
	 *
	 * The first attempt at this used nearest neighbour for everything the user
	 * was moving, and it looked it: dragging a window made the whole screen go
	 * grainy, which is a poor trade when dragging a window only dirties a band
	 * of rows and the box average over a band is cheap. So the test is the work
	 * in front of it, not the mood: keep the good filter while the rows that
	 * changed are a small enough part of the screen to afford it, and fall back
	 * only when the guest really is redrawing everything at once, which is
	 * where the cost was.
	 *
	 * Upscaling always takes nearest: a box average has nothing to average when
	 * a destination pixel covers less than one source pixel.
	 *
	 * The budget is in source pixels, because that is what the box average
	 * reads: 600,000 is about 4ms at the 7ns a pixel measured here, which
	 * leaves room for sixty of them a second.
	 */
	static const long kBoxPixelBudget = 600000;
	const bool upscaling = dw > frame_width_ || dh > frame_height_;
	long source_rows = frame_height_;

	if (!dirty_all_ && dirty_bottom_ > dirty_top_) {
		source_rows = dirty_bottom_ - dirty_top_;
	}

	const bool too_much_to_filter =
	    source_rows * (long) frame_width_ > kBoxPixelBudget;
	const bool fast = upscaling || (Interacting() && too_much_to_filter);

	if (!scaled_valid_ || scaled_width_ != dw || scaled_height_ != dh ||
	    scaled_from_width_ != frame_width_ || scaled_from_height_ != frame_height_ ||
	    scaled_is_fast_ != fast) {
		if (!scaled_image_.IsOk() || scaled_image_.GetWidth() != dw ||
		    scaled_image_.GetHeight() != dh) {
			scaled_image_ = wxImage(dw, dh, false);
		}
		scaled_width_ = dw;
		scaled_height_ = dh;
		scaled_from_width_ = frame_width_;
		scaled_from_height_ = frame_height_;
		scaled_is_fast_ = fast;
		scaled_valid_ = true;
		dirty_all_ = true;
	}

	int row_top = 0, row_bottom = dh;

	if (!dirty_all_) {
		remote_display_scale_rows_for(dirty_top_, dirty_bottom_, frame_height_,
		    dh, &row_top, &row_bottom);
	}

	if (row_bottom > row_top) {
		if (fast) {
			remote_display_scale_argb_nearest_rows(frame_pixels_.data(),
			    frame_width_, frame_height_, scaled_image_.GetData(), dw, dh,
			    row_top, row_bottom);
		} else {
			remote_display_scale_argb_rows(frame_pixels_.data(),
			    frame_width_, frame_height_, scaled_image_.GetData(), dw, dh,
			    row_top, row_bottom);
		}

		scaled_bitmap_stale_ = true;
	}

	dirty_all_ = false;
	dirty_top_ = dirty_bottom_ = 0;

	if (scaled_bitmap_stale_ || !scaled_bitmap_.IsOk()) {
		scaled_bitmap_ = wxBitmap(scaled_image_);
		scaled_bitmap_stale_ = false;
	}

	return scaled_bitmap_;
}

/*
 * Moving something counts for a quarter of a second after the last event, and
 * for as long as a button is held - a drag has long gaps between motion events
 * and must not flick to the slow filter in the middle of one.
 */
bool RemoteEmulatorPanel::Interacting() const
{
	if (held_buttons_ != 0) {
		return true;
	}
	if (last_input_ == std::chrono::steady_clock::time_point()) {
		return false;
	}

	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - last_input_).count() < 250;
}

void RemoteEmulatorPanel::NoteInput()
{
	last_input_ = std::chrono::steady_clock::now();

	/* Restarted on every event, so it fires once, after the last one. Longer
	   than the quarter second Interacting() allows, so the timer lands after
	   the fast filter has lapsed rather than racing it. */
	settle_timer_.Start(320, wxTIMER_ONE_SHOT);
}

void RemoteEmulatorPanel::OnSettleTimer(wxTimerEvent & /*event*/)
{
	/* They have stopped. Redraw the whole picture properly. */
	if (live_ && active_) {
		scaled_valid_ = false;
		Refresh(false);
	}
}

void RemoteEmulatorPanel::RefreshFrame()
{
	int w = 0, h = 0;

	if (!shared_fb_.ReadInto(&frame_pixels_, &w, &h)) {
		return;
	}
	frame_width_ = w;
	frame_height_ = h;
	frame_serial_++;

	/*
	 * ★ Only the copy out of shared memory happens here.
	 *
	 * Converting to RGB and building a guest-sized wxBitmap used to happen on
	 * every frame as well, whether anything needed them or not. The path
	 * taken for a scaled picture - which is every panel that is not exactly
	 * the guest's size - reads the 32-bit pixels directly and needs neither,
	 * so both are built on demand now (FullBitmap) and the common case does
	 * one pass over the frame instead of three.
	 */
	display_bitmap_ = wxBitmap();

	/* No Refresh() here: this is called from OnPaint, and asking for another
	   paint from inside one is how a repaint loop starts. */
}

/*
 * The guest's screen at its own size, ready to blit.
 *
 * Wanted only by the paths that do not scale it themselves: the 1:1 case, the
 * hardware-accelerated graphics context on Windows, and the StretchBlit
 * fallback.
 */
wxBitmap &RemoteEmulatorPanel::FullBitmap()
{
	if (display_bitmap_.IsOk()) {
		return display_bitmap_;
	}

	const int w = frame_width_, h = frame_height_;

	if (w <= 0 || h <= 0 || frame_pixels_.size() < (size_t) w * (size_t) h) {
		return display_bitmap_;
	}

	wxImage image(w, h, false);
	unsigned char *rgb = image.GetData();
	const uint32_t *src = frame_pixels_.data();

	/* Same 0x00RRGGBB layout VideoUpdate has always used - see
	   copy_rgb32_rows_to_image() in emulator_panel.cpp. */
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint32_t pixel = src[(size_t) y * (size_t) w + (size_t) x];
			const size_t idx = (size_t) (y * w + x) * 3;
			rgb[idx + 0] = (unsigned char) ((pixel >> 16) & 0xff);
			rgb[idx + 1] = (unsigned char) ((pixel >> 8) & 0xff);
			rgb[idx + 2] = (unsigned char) (pixel & 0xff);
		}
	}

	display_bitmap_ = wxBitmap(image);

	return display_bitmap_;
}

/*
 * Whether the GPU is drawing this machine at the moment.
 */
/*
 * Act on the setting having been changed while machines are running, so it takes
 * effect on the machine the user is looking at rather than at the next start.
 */
void RemoteEmulatorPanel::SetHardwareAcceleration(bool enabled)
{
#if wxUSE_GLCANVAS
	if (!enabled) {
		if (gl_canvas_ != nullptr) {
			DestroyGlCanvas("turned off in Settings");
		}
		/* Left tried, so nothing re-creates it until asked again. */
		gl_tried_ = true;
		return;
	}

	if (gl_canvas_ == nullptr) {
		gl_tried_ = false;	/* let the next paint try again */
	}
#else
	(void) enabled;
#endif
	scaled_valid_ = false;
	dirty_all_ = true;
	Refresh(false);
}

bool RemoteEmulatorPanel::GlActive() const
{
#if wxUSE_GLCANVAS
	return gl_canvas_ != nullptr && gl_canvas_->IsUsable();
#else
	return false;
#endif
}

/*
 * Set up the GPU path, once, and only where it is worth having.
 *
 * Not attempted at all on a platform whose own renderer is already hardware
 * accelerated - Windows, where the Direct2D path in OnPaint is the right answer
 * and a second display path would be two things to keep working for no gain.
 *
 * `--no-gl` turns it off everywhere, which is the escape hatch for a display
 * where GLX exists, succeeds, and then behaves badly: that is not something this
 * code can detect, so it has to be something the user can say.
 */
void RemoteEmulatorPanel::TryCreateGlCanvas()
{
#if wxUSE_GLCANVAS
	if (gl_tried_ || !live_) {
		return;
	}
	gl_tried_ = true;

#ifdef __WXMSW__
	/* Direct2D already does this in hardware; see OnPaint. */
	return;
#else
	if (!HardwareAccelerationWanted()) {
		rpclog("Manager: hardware acceleration is off, so a machine's screen "
		       "is drawn on the CPU\n");
		return;
	}

	gl_canvas_ = new GlDisplayCanvas(this);
	gl_canvas_->SetSize(GetClientSize());
	gl_canvas_->SetCursor(wxCursor(wxCURSOR_BLANK));

	/*
	 * Input goes to this panel's own handlers. The canvas covers the panel
	 * exactly, so the coordinates in its events are already panel coordinates
	 * and PanelPointToGuest needs no adjustment.
	 *
	 * Bound one by one rather than by pushing this panel as the canvas's event
	 * handler: this panel also handles EVT_PAINT and EVT_SIZE, and letting those
	 * fire for the canvas would have the CPU path drawing over the GPU one.
	 */
	gl_canvas_->Bind(wxEVT_MOTION, &RemoteEmulatorPanel::OnMouseMove, this);
	gl_canvas_->Bind(wxEVT_LEFT_DOWN, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_DOWN, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_RIGHT_DOWN, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_LEFT_DCLICK, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_DCLICK, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_RIGHT_DCLICK, &RemoteEmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_LEFT_UP, &RemoteEmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_UP, &RemoteEmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_RIGHT_UP, &RemoteEmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_MOUSEWHEEL, &RemoteEmulatorPanel::OnMouseWheel, this);
	gl_canvas_->Bind(wxEVT_KEY_DOWN, &RemoteEmulatorPanel::OnKeyDown, this);
	gl_canvas_->Bind(wxEVT_KEY_UP, &RemoteEmulatorPanel::OnKeyUp, this);
	gl_canvas_->Bind(wxEVT_KILL_FOCUS, &RemoteEmulatorPanel::OnKillFocus, this);

	gl_canvas_->SetFocus();
#endif
#endif
}

/*
 * Give up on the GPU and go back to drawing here.
 *
 * Called when the canvas reports itself unusable, which can happen on its first
 * paint rather than at construction: a GLX context can be created and only then
 * refuse to become current.
 */
void RemoteEmulatorPanel::DestroyGlCanvas(const wxString &why)
{
#if wxUSE_GLCANVAS
	if (gl_canvas_ == nullptr) {
		return;
	}

	rpclog("Manager: OpenGL display unavailable (%s); drawing on the CPU "
	       "instead\n", why.utf8_str().data());

	gl_canvas_->Destroy();
	gl_canvas_ = nullptr;

	/* The CPU path has drawn nothing so far, so everything is stale. */
	scaled_valid_ = false;
	dirty_all_ = true;
	SetFocus();
	Refresh(false);
#else
	(void) why;
#endif
}

void RemoteEmulatorPanel::OnPaint(wxPaintEvent & /*event*/)
{
	/* Buffered, as EmulatorPanel's own paint is: the letterboxed case clears
	   the panel and draws the picture over it, and unbuffered that black frame
	   reached the screen on its own first. */
	wxBufferedPaintDC dc(this);

	if (live_ && frame_dirty_) {
		RefreshFrame();
		frame_dirty_ = false;
	}

#if wxUSE_GLCANVAS
	/*
	 * The GPU path is set up on the first paint that has something to show, not
	 * in the constructor: a GL context needs a realised window, and this panel
	 * is created before it is shown.
	 */
	if (live_ && !frame_pixels_.empty()) {
		TryCreateGlCanvas();
	}

	if (gl_canvas_ != nullptr) {
		if (gl_canvas_->IsUsable() || !gl_canvas_->Failure().empty()) {
			if (!gl_canvas_->IsUsable()) {
				DestroyGlCanvas(gl_canvas_->Failure());
			} else {
				/* The canvas covers this panel and has drawn it. */
				return;
			}
		} else {
			/* Not decided yet - its own first paint will settle it. Leave the
			   panel black rather than drawing a frame that is about to be
			   covered. */
			dc.SetBackground(*wxBLACK_BRUSH);
			dc.Clear();
			return;
		}
	}
#endif

	if (!live_) {
		dc.SetBackground(*wxBLACK_BRUSH);
		dc.Clear();
		dc.SetTextForeground(*wxWHITE);
		const wxString msg = "Machine stopped";
		const wxSize extent = dc.GetTextExtent(msg);
		const wxSize client = GetClientSize();
		dc.DrawText(msg, (client.GetWidth() - extent.GetWidth()) / 2,
		            (client.GetHeight() - extent.GetHeight()) / 2);
		return;
	}

	if (frame_width_ > 0 && frame_height_ > 0 && !frame_pixels_.empty()) {
		const wxRect display = DisplayRect();

		/*
		 * ★ Fitted to the panel, keeping its shape, and drawn by whichever
		 * route is actually cheap on this platform.
		 *
		 * The panel is a fixed shape and the guest's screen is not, so the
		 * picture is fitted to whichever edge runs out first and centred, with
		 * the background showing through as bars. DisplayRect() works out
		 * where, and PanelPointToGuest maps the pointer through the same
		 * rectangle - they used to disagree, and that is why the guest pointer
		 * moved at the wrong speed.
		 *
		 * How it is scaled matters more than it looks, and the answer differs
		 * by platform. A graphics context lets the platform's renderer filter
		 * it, which is right where that renderer is hardware accelerated:
		 * Direct2D on Windows, asked for by name. Where it is not accelerated
		 * it is the most expensive option there is - GTK's Cairo took about
		 * 30ms for a 1600x1200 guest in a 933x700 panel, and is quick only at
		 * 1:1 and 1:2, never at the ratios a dragged window produces. So
		 * everywhere else the scaling is done here, incrementally: only the
		 * rows the guest redrew, and with the cheap filter while the user is
		 * moving something. Measured on a real session, that took a paint from
		 * 23ms to a few.
		 */
		dc.SetBackground(*wxBLACK_BRUSH);
		dc.Clear();

		wxGraphicsRenderer *accelerated = nullptr;

#ifdef __WXMSW__
		/*
		 * Direct2D rather than the GDI+ default, which is slow enough to create
		 * per paint that typing felt laggy - and only when the user has left
		 * hardware acceleration on, which is the same setting that gives Linux
		 * and macOS their OpenGL path. Off means the software path on all three,
		 * which is what makes the setting mean one thing.
		 */
		if (HardwareAccelerationWanted()) {
			accelerated = wxGraphicsRenderer::GetDirect2DRenderer();
		}
#endif

		wxGraphicsContext *gc = accelerated != nullptr
		    ? accelerated->CreateContext(dc) : nullptr;

		if (gc != nullptr) {
			wxBitmap &bitmap = FullBitmap();

			if (bitmap.IsOk()) {
				/* BEST, not GOOD: the guest screen is usually scaled down,
				   and GOOD is bilinear, which breaks single-pixel text. */
				gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
				gc->DrawBitmap(bitmap, display.GetX(), display.GetY(),
				    display.GetWidth(), display.GetHeight());
			}
			delete gc;
		} else {
			wxBitmap &bitmap =
			    ScaledBitmap(display.GetWidth(), display.GetHeight());

			if (bitmap.IsOk()) {
				wxMemoryDC memDC(bitmap);

				dc.Blit(display.GetX(), display.GetY(),
				    display.GetWidth(), display.GetHeight(), &memDC, 0, 0);
			}
		}

	} else {
		dc.SetBackground(*wxBLACK_BRUSH);
		dc.Clear();
	}
}

bool RemoteEmulatorPanel::SaveScreenshot(const wxString &path)
{
	int w = 0, h = 0;

	if (!live_ || !shared_fb_.ReadInto(&frame_pixels_, &w, &h) || w <= 0 || h <= 0) {
		return false;
	}

	wxImage image(w, h, false);
	unsigned char *rgb = image.GetData();
	const uint32_t *src = frame_pixels_.data();

	/* Saved at the guest's resolution, not the panel's: a screenshot of a
	   window that happened to be dragged smaller is not what was asked for. */
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint32_t pixel = src[(size_t) y * (size_t) w + (size_t) x];
			const size_t idx = (size_t) (y * w + x) * 3;

			rgb[idx + 0] = (unsigned char) ((pixel >> 16) & 0xff);
			rgb[idx + 1] = (unsigned char) ((pixel >> 8) & 0xff);
			rgb[idx + 2] = (unsigned char) (pixel & 0xff);
		}
	}

	return image.SaveFile(path, wxBITMAP_TYPE_PNG);
}

void RemoteEmulatorPanel::OnEraseBackground(wxEraseEvent & /*event*/)
{
	/* Avoided: painting is done in full by OnPaint (wxBG_STYLE_PAINT), same
	   as EmulatorPanel - stops the whole panel flickering to background
	   colour before every frame. */
}

void RemoteEmulatorPanel::OnSize(wxSizeEvent &event)
{
	/* A live resize sends these continuously. Rebuilding the scaled bitmap on
	   each one was most of what made the window stop responding while being
	   dragged; the paint that follows does it once. */
	if (live_) {
		/* A different size means a different scaled picture, all of it. */
		scaled_valid_ = false;
		frame_dirty_ = true;
		Refresh(false);
	}

#if wxUSE_GLCANVAS
	if (gl_canvas_ != nullptr) {
		gl_canvas_->SetSize(GetClientSize());
		gl_canvas_->SetDisplayRect(DisplayRect());
		gl_canvas_->Refresh(false);
	}
#endif
	event.Skip();
}

int RemoteEmulatorPanel::MapClickButton(const wxMouseEvent &event) const
{
	switch (event.GetButton()) {
	case wxMOUSE_BTN_LEFT: return 1;
	case wxMOUSE_BTN_RIGHT: return 2;
	case wxMOUSE_BTN_MIDDLE: return 4;
	default: return 0;
	}
}

wxRect RemoteEmulatorPanel::DisplayRect() const
{
	const wxSize client = GetClientSize();
	const struct remote_display_rect rect = remote_display_rect_for(
	    client.GetWidth(), client.GetHeight(), frame_width_, frame_height_);

	return wxRect(rect.x, rect.y, rect.w, rect.h);
}

/*
 * Panel coordinates to guest coordinates.
 *
 * Measured against the rectangle the picture is drawn in, not against the whole
 * panel. Against the whole panel - which is what this did - the guest pointer
 * moved at dw/panel_width of the speed the host pointer did, and started
 * offset by the width of the bar down the left, because the letterboxing the
 * paint applies was not accounted for here. The host pointer is hidden while a
 * machine is shown, so the guest one is all the user sees, and it read as a
 * pointer that moved more slowly than the hand moving it. EmulatorPanel maps
 * through its own offset_/scaled_ for the same reason.
 *
 * A point in the bars is clamped to the nearest edge of the picture, which is
 * what a guest pointer pressed against the side of its screen does anyway.
 */
wxPoint RemoteEmulatorPanel::PanelPointToGuest(int x, int y) const
{
	const wxSize client = GetClientSize();
	const struct remote_display_rect rect = remote_display_rect_for(
	    client.GetWidth(), client.GetHeight(), frame_width_, frame_height_);
	int gx = 0, gy = 0;

	remote_display_point_to_guest(rect, frame_width_, frame_height_, x, y, &gx, &gy);

	return wxPoint(gx, gy);
}

void RemoteEmulatorPanel::OnMouseMove(wxMouseEvent &event)
{
	if (!live_) {
		return;
	}

	NoteInput();

	/*
	 * ★ A machine in captured mode is never sent a position.
	 *
	 * Not merely pointless but wrong: the machine's own mouse code asserts that
	 * positions arrive only in follow-mouse mode, so sending one to a machine
	 * expecting movements is a bug on the other side of the socket. Before the
	 * click that captures the pointer, there is nothing to send at all - which is
	 * what a machine's own window does too.
	 */
	if (!follow_host_mouse_) {
		if (pointer_captured_) {
			SendCapturedMotion(event);
		}
		return;
	}

	const wxPoint guest = PanelPointToGuest(event.GetX(), event.GetY());
	IpcRequest request;
	request.type = IpcRequestType::MouseMove;
	request.arg1 = guest.x;
	request.arg2 = guest.y;

	SendRequest(request);
}

void RemoteEmulatorPanel::OnMouseDown(wxMouseEvent &event)
{
	SetFocus();
	if (!live_) {
		return;
	}

	/*
	 * In captured mode the first click takes the pointer rather than reaching
	 * RISC OS, as it does in a machine's own window. It has to be spent on
	 * something: the guest pointer is wherever RISC OS left it, so a click passed
	 * on now would land somewhere the user was not aiming at.
	 */
	if (!follow_host_mouse_ && !pointer_captured_) {
		NoteInput();
		CaptureThePointer(event.GetX(), event.GetY());
		return;
	}

	const int buttons = MapClickButton(event);
	if (buttons == 0) {
		return;
	}

	NoteInput();
	held_buttons_ |= buttons;

	/* Only in follow-mouse mode: a captured pointer has no position to send, and
	   RISC OS already has it from the movements. */
	if (follow_host_mouse_) {
		OnMouseMove(event);
	}

	IpcRequest request;
	request.type = IpcRequestType::MousePress;
	request.arg1 = buttons;
	SendRequest(request);
}

void RemoteEmulatorPanel::OnMouseUp(wxMouseEvent &event)
{
	if (!live_) {
		return;
	}

	const int buttons = MapClickButton(event);
	if (buttons == 0) {
		return;
	}

	NoteInput();
	held_buttons_ &= ~buttons;

	IpcRequest request;
	request.type = IpcRequestType::MouseRelease;
	request.arg1 = buttons;
	SendRequest(request);
}

void RemoteEmulatorPanel::OnMouseWheel(wxMouseEvent &event)
{
	if (!live_) {
		return;
	}

	IpcRequest request;
	request.type = IpcRequestType::MouseWheel;
	request.arg1 = event.GetWheelRotation();
	SendRequest(request);
}

void RemoteEmulatorPanel::OnKeyDown(wxKeyEvent &event)
{
	if (!live_) {
		event.Skip();
		return;
	}

	/*
	 * Alt+Enter is the way out of both things a user can get stuck in, and does
	 * not reach the guest when it is used for one of them. Nothing else is
	 * intercepted: every other key belongs to RISC OS.
	 *
	 * Full screen first, then the captured pointer - the same order a machine's
	 * own window uses, deliberately, because one key doing different things in
	 * the two windows would be worse than either order. So in full screen with
	 * the pointer captured it takes two presses: one to leave, one to be given
	 * the mouse back.
	 */
	if (InputIsReleaseMouseCaptureKey(event)) {
		if (on_leave_full_screen_ && on_leave_full_screen_()) {
			return;
		}
		if (ReleaseCapturedPointer()) {
			return;
		}
		/* Nothing to escape from, so the guest gets the key. */
	}

	/* A click rather than a keystroke, as in a machine's own window. */
	if (InputIsThirdMouseButtonKey(event)) {
		IpcRequest request;
		request.type = IpcRequestType::MousePress;
		request.arg1 = 4;
		SendRequest(request);
		return;
	}

	const unsigned key_id = InputKeyIdentityFromKeyEvent(event);
	const unsigned scan_code = InputNativeScancodeFromKeyEvent(event);

	if (scan_code != 0 && held_keys_press(&held_keys_, key_id, scan_code)) {
		IpcRequest request;
		request.type = IpcRequestType::KeyPress;
		request.arg1 = (int32_t) scan_code;
		SendRequest(request);
	}
}

void RemoteEmulatorPanel::OnKeyUp(wxKeyEvent &event)
{
	if (!live_) {
		event.Skip();
		return;
	}

	if (InputIsThirdMouseButtonKey(event)) {
		IpcRequest request;
		request.type = IpcRequestType::MouseRelease;
		request.arg1 = 4;
		SendRequest(request);
		return;
	}

	const unsigned key_id = InputKeyIdentityFromKeyEvent(event);
	unsigned scan_code = 0;

	if (held_keys_release(&held_keys_, key_id, &scan_code)) {
		IpcRequest request;
		request.type = IpcRequestType::KeyRelease;
		request.arg1 = (int32_t) scan_code;
		SendRequest(request);
	}
}

void RemoteEmulatorPanel::OnKillFocus(wxFocusEvent &event)
{
	/* Same reasoning as MainFrame/EmulatorPanel: the host stops telling us
	   about key events once focus goes elsewhere, so anything still marked
	   held has to be released now or the guest believes it forever. */
	unsigned scan_codes[HELD_KEYS_MAX];
	const size_t count = held_keys_release_all(&held_keys_, scan_codes, HELD_KEYS_MAX);

	for (size_t i = 0; i < count; i++) {
		IpcRequest request;
		request.type = IpcRequestType::KeyRelease;
		request.arg1 = (int32_t) scan_codes[i];
		SendRequest(request);
	}
	event.Skip();
}
