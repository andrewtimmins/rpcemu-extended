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

#include "emulator_panel.h"

#include <wx/display.h>

#include "gui_preferences.h"

#include <algorithm>
#include <numeric>
#include <cstdlib>
#include <cstring>

#include "main_frame.h"

#include <wx/dcbuffer.h>

#ifdef __WXGTK__
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#endif

extern "C" {
#include "rpcemu.h"
#include "vidc20.h"
}

wxBEGIN_EVENT_TABLE(EmulatorPanel, wxPanel)
	EVT_PAINT(EmulatorPanel::OnPaint)
	EVT_SIZE(EmulatorPanel::OnSize)
	EVT_MOTION(EmulatorPanel::OnMouseMove)
	EVT_LEFT_DOWN(EmulatorPanel::OnMouseDown)
	EVT_MIDDLE_DOWN(EmulatorPanel::OnMouseDown)
	EVT_RIGHT_DOWN(EmulatorPanel::OnMouseDown)
	EVT_LEFT_UP(EmulatorPanel::OnMouseUp)
	EVT_MIDDLE_UP(EmulatorPanel::OnMouseUp)
	EVT_RIGHT_UP(EmulatorPanel::OnMouseUp)
	EVT_LEFT_DCLICK(EmulatorPanel::OnMouseDoubleClick)
	EVT_RIGHT_DCLICK(EmulatorPanel::OnMouseDoubleClick)
	EVT_MIDDLE_DCLICK(EmulatorPanel::OnMouseDoubleClick)
	EVT_MOUSEWHEEL(EmulatorPanel::OnMouseWheel)
	EVT_ENTER_WINDOW(EmulatorPanel::OnEnterWindow)
	EVT_LEAVE_WINDOW(EmulatorPanel::OnLeaveWindow)
	EVT_MOUSE_CAPTURE_LOST(EmulatorPanel::OnMouseCaptureLost)
wxEND_EVENT_TABLE()

EmulatorPanel::EmulatorPanel(wxWindow *parent, EmulatorHost &emulator)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(640, 480),
	          wxWANTS_CHARS | wxTAB_TRAVERSAL)
	, emulator_(emulator)
{
	display_image_ = wxImage(640, 480, false);
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetCanFocus(true);
	if (pconfig_copy != nullptr) {
		display_scaling_ = pconfig_copy->display_scaling;
	}
#ifdef __WXGTK__
	if (GtkWidget *widget = GTK_WIDGET(GetHandle())) {
		gtk_widget_add_events(widget,
		                      GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);
	}
#endif
	SetFocus();
	UpdateMouseCursor();
}

void EmulatorPanel::MarkUserPointerActivity()
{
	user_pointer_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

/*
 * Is the user holding a button down? Warping mid-drag drags the wrong thing, so
 * the pointer is left alone until they let go.
 */
bool EmulatorPanel::IsPointerButtonDown() const
{
	const wxMouseState mouse = wxGetMouseState();

	return mouse.LeftIsDown() || mouse.RightIsDown() || mouse.MiddleIsDown();
}

/* Has the user moved the pointer in the last moment? */
bool EmulatorPanel::IsUserPointerActive() const
{
	return std::chrono::steady_clock::now() < user_pointer_until_;
}

/*
 * Turn the guest's pointer into this panel's cursor.
 *
 * The shape arrives as RGBA with a stride of its own; wxCursor is built from a
 * wxImage, which keeps colour and alpha separately, so the alpha becomes a mask.
 * Only rebuilt when the shape actually changes - RISC OS leaves it alone for
 * minutes at a time and building a cursor is not free.
 */
void EmulatorPanel::ApplyPointerShape(const PointerShape &shape)
{
	const bool was_host_side = cursor_host_side_;

	cursor_host_side_ = shape.host_side;

	if (!shape.host_side || !shape.visible || !guest_shape_.Set(shape)) {
		guest_shape_.valid = false;
		guest_cursor_ok_ = false;
		if (was_host_side != cursor_host_side_) {
			UpdateMouseCursor();
		}
		return;
	}

	/* Force the rebuild: the shape changed even if the scaling did not. */
	guest_cursor_scale_num_ = 0;
	guest_cursor_scale_den_ = 0;
	RebuildGuestCursor();
}

/* Build the cursor at the size one guest pixel now occupies on screen. Returns
   at once unless that has actually moved, so calling it per frame is free. */
void EmulatorPanel::RebuildGuestCursor()
{
	if (!guest_shape_.valid) {
		return;
	}

	const int num = (scaled_x_ > 0) ? scaled_x_ : 1;
	const int den = (image_width_ > 0) ? image_width_ : 1;

	if (num == guest_cursor_scale_num_ && den == guest_cursor_scale_den_) {
		return;
	}
	guest_cursor_scale_num_ = num;
	guest_cursor_scale_den_ = den;

	guest_cursor_ = guest_cursor_build(guest_shape_, num, den);
	guest_cursor_ok_ = guest_cursor_.IsOk();
	UpdateMouseCursor();
}

/*
 * Which cursor the host should be showing, in one place.
 *
 * Blank whenever RISC OS is drawing a pointer of its own, or the host one sits
 * on top of the guest's and there are two. An arrow when the guest is not
 * drawing one, so the hand still has something to follow.
 */
wxCursor EmulatorPanel::ChooseMouseCursor() const
{
	if (mouse_captured) {
		return wxCursor(wxCURSOR_BLANK);
	}
	if (pconfig_copy != nullptr && pconfig_copy->mousehackon && IsMouseOverPanel()) {
		return wxCursor(wxCURSOR_BLANK);
	}
	return wxCursor(wxCURSOR_ARROW);
}

void EmulatorPanel::ApplyMouseCursor(const wxCursor &cursor)
{
	SetCursor(cursor);

#if wxUSE_GLCANVAS
	/*
	 * And the canvas, which is a window in its own right sitting over this one
	 * and carrying its own cursor. Setting only the panel's left the host arrow
	 * drawn on top of the guest's pointer wherever the GPU path was up, because
	 * the cursor the mouse is actually over is the canvas's.
	 */
	if (gl_canvas_ != nullptr) {
		gl_canvas_->SetCursor(cursor);
	}
#endif
}

void EmulatorPanel::UpdateMouseCursor()
{
	if (mouse_captured) {
		SetCursor(wxCursor(wxCURSOR_BLANK));
		return;
	}

	if (pconfig_copy != nullptr && pconfig_copy->mousehackon && IsMouseOverPanel()) {
		/*
		 * The guest's own pointer, drawn by the host, when the emulator has
		 * stopped putting it in the frame. Blank otherwise: the frame already
		 * carries one and a second cursor on top of it looks like a fault.
		 */
		if (cursor_host_side_ && guest_cursor_ok_) {
			SetCursor(guest_cursor_);
		} else if (cursor_host_side_) {
			/*
			 * ★ The host would not build the cursor, and the emulator has
			 * already stopped putting the pointer in the frame - so blanking
			 * here would leave no pointer at all, anywhere. An arrow is the
			 * wrong shape but it is in the right place and it can be seen.
			 */
			SetCursor(wxCursor(wxCURSOR_ARROW));
		} else {
			/* The frame carries the pointer, so a host one on top of it would
			   be a second cursor. */
			SetCursor(wxCursor(wxCURSOR_BLANK));
		}
	} else {
		SetCursor(wxCursor(wxCURSOR_ARROW));
	}
}

bool EmulatorPanel::IsMouseOverPanel() const
{
	const wxMouseState mouse = wxGetMouseState();
	const wxPoint local = ScreenToClient(wxPoint(mouse.GetX(), mouse.GetY()));
	return GetClientRect().Contains(local);
}

void EmulatorPanel::SetFullScreen(bool full_screen)
{
	full_screen_ = full_screen;
	if (full_screen_) {
		SetMinSize(wxDefaultSize);
		SetMaxSize(wxDefaultSize);
	} else {
		ResizeToHostDisplay();
	}
	CalculateScaling();
	Refresh(false);
}

void EmulatorPanel::SetDisplayMode(int scaling)
{
	display_scaling_ = scaling;
	ResizeToHostDisplay();
	CalculateScaling();
	Refresh(false);
}

void EmulatorPanel::SizeToGuest()
{
	ResizeToHostDisplay();
	CalculateScaling();
	Refresh(false);
}

/* Force an immediate, full repaint from the retained frame. Used after
   transitions (full-screen enter/exit, scaling-mode change) where the guest
   desktop may be static and would otherwise send no fresh video update to
   trigger a paint - leaving a stale or blank panel. */
void EmulatorPanel::ForceRedraw()
{
	CalculateScaling();
	Refresh(false);
	Update();
}

bool EmulatorPanel::SaveScreenshot(const wxString &path)
{
	if (!display_image_.IsOk() || image_width_ <= 0 || image_height_ <= 0) {
		return false;
	}

	return display_image_.SaveFile(path, wxBITMAP_TYPE_PNG);
}

void EmulatorPanel::FocusPanel()
{
	SetFocus();
}

void EmulatorPanel::ReleaseMouseCapture()
{
	if (!mouse_captured) {
		return;
	}
	mouse_captured = 0;
	captured_pointer_end(&captured_pointer_);
	UpdateMouseCursor();
}

/*
 * The counters in the log now and then, when asked for - see
 * captured_pointer_debug_wanted().
 */
void EmulatorPanel::ReportCapturedPointerRate()
{
	const unsigned long now_ms = (unsigned long)
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count();

	if (captured_pointer_should_report(&captured_pointer_, now_ms)) {
		rpclog("mouse: captured in the machine's own window - %lu movements, "
		       "%lu re-centres\n",
		       captured_pointer_.moves, captured_pointer_.recentres);
	}
}

/*
 * One path, not two.
 *
 * The guest's picture is always placed within the panel - at actual size with the
 * guest in the configured mode the offsets are zero and the scale is 1, which is
 * the old fast path expressed as a special case of this one rather than as
 * separate code. Having a single mapping is what keeps the pointer, the dirty
 * rectangles and the blit agreeing with each other.
 */
wxPoint EmulatorPanel::PanelPointToHost(int x, int y) const
{
	const int local_x = x - offset_x_;
	const int local_y = y - offset_y_;

	if (scaled_x_ <= 0 || scaled_y_ <= 0) {
		return wxPoint(-1, -1);
	}
	if (local_x < 0 || local_y < 0 || local_x >= scaled_x_ || local_y >= scaled_y_) {
		return wxPoint(-1, -1);
	}

	return wxPoint(local_x * host_xsize_ / scaled_x_,
	               local_y * host_ysize_ / scaled_y_);
}

wxPoint EmulatorPanel::HostPointToPanel(int host_x, int host_y) const
{
	int panel_x = host_x;
	int panel_y = host_y;
	if (double_size_ == VIDC_DOUBLE_X || double_size_ == VIDC_DOUBLE_BOTH) {
		panel_x *= 2;
	}
	if (double_size_ == VIDC_DOUBLE_Y || double_size_ == VIDC_DOUBLE_BOTH) {
		panel_y *= 2;
	}

	return wxPoint(offset_x_ + (panel_x * scaled_x_) / std::max(host_xsize_, 1),
	               offset_y_ + (panel_y * scaled_y_) / std::max(host_ysize_, 1));
}

void EmulatorPanel::SyncMousePosition(int x, int y)
{
	const wxPoint host = PanelPointToHost(x, y);
	if (host.x < 0 || host.y < 0) {
		return;
	}
	if (host.x == last_mouse_x_ && host.y == last_mouse_y_) {
		return;
	}

	last_mouse_x_ = host.x;
	last_mouse_y_ = host.y;
	MarkUserPointerActivity();
	emulator_.MouseMove(host.x, host.y);
}

wxPoint EmulatorPanel::CaptureCentre() const
{
	return wxPoint(offset_x_ + scaled_x_ / 2, offset_y_ + scaled_y_ / 2);
}

void EmulatorPanel::ResizeToHostDisplay()
{
	const wxSize guest(host_xsize_, host_ysize_);
	const bool have_size = host_xsize_ > 0 && host_ysize_ > 0;

	/* Full screen: the display sets the size and nothing here has a say. */
	if (full_screen_ || !have_size) {
		SetMinSize(wxDefaultSize);
		SetMaxSize(wxDefaultSize);
		SetSizeHints(wxDefaultSize, wxDefaultSize);
		return;
	}

	if (display_scaling_ == DisplayScaling_WholeMultiples) {
		/*
		 * Free to grow, but never below one host pixel per guest pixel.
		 *
		 * The minimum matters: this used to be cleared. Toggling from the menu was
		 * fine, because by then the window had a size of its own, but a machine
		 * that started with the setting already on had a panel contributing no
		 * minimum at all, so the frame's Fit() shrink-wrapped it to just the menu
		 * and toolbar. That is issue #47, "the main window fails to open".
		 */
		SetMinSize(guest);
		SetMaxSize(wxDefaultSize);
		SetSizeHints(guest, wxDefaultSize);
		return;
	}

	/*
	 * Actual size: the panel is exactly the guest's screen.
	 *
	 * Both directions matter. Smaller and the desktop is clipped, which loses the
	 * icon bar off the bottom; larger and the difference is a black border. The
	 * screen-size setting decides what the guest is ASKED to be; what it actually
	 * is, is what gets shown.
	 */
	SetMinSize(guest);
	SetMaxSize(guest);
	SetSize(guest);
	SetSizeHints(guest, guest);
}

namespace {

void copy_rgb32_rows_to_image(const uint32_t *src, int xsize, int yl, int yh, wxImage &image)
{
	if (src == nullptr || !image.IsOk()) {
		return;
	}

	const int width = image.GetWidth();
	const int y0 = std::max(0, yl);
	const int y1 = std::min(yh, image.GetHeight());
	unsigned char *rgb = image.GetData();

	for (int y = y0; y < y1; ++y) {
		for (int x = 0; x < xsize && x < width; ++x) {
			const uint32_t pixel =
			    src[static_cast<size_t>(y) * static_cast<size_t>(xsize) + static_cast<size_t>(x)];
			const size_t idx = static_cast<size_t>((y * width + x) * 3);
			rgb[idx + 0] = static_cast<unsigned char>((pixel >> 16) & 0xff);
			rgb[idx + 1] = static_cast<unsigned char>((pixel >> 8) & 0xff);
			rgb[idx + 2] = static_cast<unsigned char>(pixel & 0xff);
		}
	}
}

} // namespace

void EmulatorPanel::ApplyVideoUpdate(const VideoUpdate &update)
{
	if (update.buffer == nullptr || update.xsize <= 0 || update.ysize <= 0) {
		return;
	}

	bool recalculate_needed = false;


	if (GlActive()) {
		/*
		 * The GPU path. The guest's pixels go into a texture, so none of the
		 * conversion below happens: no wxImage, no wxBitmap rebuild, no
		 * sub-image Blit, and no full-frame rescale in the paint - which is the
		 * cost this is here to remove.
		 *
		 * Only the CONVERSION is skipped. Everything after this, the scaling
		 * maths and the panel resize on a mode change, still has to run: the
		 * first version of this returned early and skipped it, so a guest
		 * changing to 1440x900 left the panel at the old mode's 640x480 while
		 * the destination rectangle grew to the new size, and all that could be
		 * seen was the top-left corner of the screen.
		 */
		if (image_width_ != update.xsize || image_height_ != update.ysize) {
			image_width_ = update.xsize;
			image_height_ = update.ysize;
			recalculate_needed = true;
		}
	} else if (!display_image_.IsOk() || display_image_.GetWidth() != update.xsize ||
	    display_image_.GetHeight() != update.ysize || !display_bitmap_.IsOk()) {
		display_image_ = wxImage(update.xsize, update.ysize, false);
		image_width_ = update.xsize;
		image_height_ = update.ysize;
		copy_rgb32_rows_to_image(update.buffer, update.xsize, 0, update.ysize, display_image_);
		/* New frame geometry: rebuild the whole cached bitmap once. */
		display_bitmap_ = wxBitmap(display_image_);
		recalculate_needed = true;
	} else {
		copy_rgb32_rows_to_image(update.buffer, update.xsize, update.yl, update.yh, display_image_);

		const int y0 = std::max(0, update.yl);
		const int y1 = std::min(update.yh, image_height_);

		if (y0 <= 0 && y1 >= image_height_) {
			/* The whole frame changed, so convert it once. Going through
			   GetSubImage and a Blit here costs two more full-frame copies,
			   which at 2560x1440 was enough that the panel never finished a
			   paint before the next frame arrived: the display stayed black
			   while the emulator was producing perfectly good frames. */
			display_bitmap_ = wxBitmap(display_image_);
		} else if (y1 > y0) {
			/* Part of the frame changed (e.g. a moving pointer): refresh only
			   those rows of the cached bitmap rather than rebuilding it all,
			   which is what made scaled modes crawl. */
			wxBitmap sub(display_image_.GetSubImage(wxRect(0, y0, image_width_, y1 - y0)));
			wxMemoryDC dst(display_bitmap_);
			wxMemoryDC srcdc(sub);
			dst.Blit(0, y0, image_width_, y1 - y0, &srcdc, 0, 0);
		}
	}

	if (double_size_ != update.double_size) {
		double_size_ = update.double_size;
		recalculate_needed = true;
	}

	/* The guest's size drives the scaling maths whatever the mode; what differs
	   is whether the window is then resized to match, below. */
	if (update.host_xsize != host_xsize_ || update.host_ysize != host_ysize_) {
		host_xsize_ = update.host_xsize;
		host_ysize_ = update.host_ysize;
		recalculate_needed = true;
	}

	if (recalculate_needed) {
		CalculateScaling();

		/*
		 * Deliberately nothing here any more.
		 *
		 * The window used to be resized to the guest's new mode at this point,
		 * and then re-centred, on every mode change. RISC OS changes mode two or
		 * three times while it boots - and on a machine with the graphics card
		 * the display is handed over part way through as well - so the window
		 * jumped through three sizes and positions before settling on one. As a
		 * startup it was indefensible.
		 *
		 * The window's size now comes from the configured screen size, and is set
		 * once by the frame: when the machine starts, or when the setting is
		 * changed. Whatever mode the guest is actually in is drawn centred inside
		 * it, so the modes passed through on the way up cost nothing more than a
		 * repaint. See ResizeToHostDisplay() and
		 * MainFrame::CentreWindowOnScreen().
		 */

		if (GlActive()) {
			/* The rectangle has just changed, so the texture is drawn against
			   the new one rather than a frame late. */
			StoreFrameForGl(update);
		}
		Refresh(false);
		return;
	}

	if (GlActive()) {
		StoreFrameForGl(update);
		return;
	}

	int width = image_width_;
	int ymin = update.yl;
	int ymax = update.yh;

	if (double_size_ & VIDC_DOUBLE_X) {
		width *= 2;
	}
	if (double_size_ & VIDC_DOUBLE_Y) {
		ymin *= 2;
		ymax *= 2;
	}

	{
		width = (width * scaled_x_) / std::max(host_xsize_, 1);

		if (ymin > 0) {
			ymin--;
		}
		if (ymax < host_ysize_) {
			ymax++;
		}

		ymin = (ymin * scaled_y_) / std::max(host_ysize_, 1);
		ymax = ((ymax * scaled_y_) + host_ysize_ - 1) / std::max(host_ysize_, 1);

		const int height = ymax - ymin;
		RefreshRect(wxRect(offset_x_, offset_y_ + ymin, width, height), false);
	}
}

void EmulatorPanel::HandleMoveHostMouse(const MouseMoveUpdate &update)
{
	/*
	 * How far the host pointer may drift from where the guest's is before it is
	 * put back even though the user is still moving the mouse.
	 *
	 * ★ Why there is a threshold at all rather than simply not warping while the
	 * user is busy.
	 *
	 * The guest clamps its pointer at the edge of the screen, or at whatever
	 * bounding box RISC OS has set. The host pointer does not: it carries on
	 * accumulating movement against the edge of the desktop. Nothing was putting
	 * it back, because a warp was suppressed for 500ms after any pointer motion
	 * and moving the mouse kept renewing that - so the suppression was in force
	 * exactly when it mattered. Push into the right or bottom edge and then
	 * reverse, and the pointer stayed put until the mouse had travelled all the
	 * way back: issue #128.
	 *
	 * Ordinary movement keeps the two within a pixel or two of each other, so a
	 * few pixels of slack is enough to tell "the user is moving and the guest is
	 * following" from "the guest has stopped and the host has not". Small
	 * corrections stay suppressed, which is what stops the emulator fighting the
	 * pointer; a drift this large only happens when something is clamped.
	 */
	static const int kReanchorSlack = 8;

	auto *frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
	if (frame != nullptr && !frame->IsWindowActive()) {
		return;
	}
	if (!IsMouseOverPanel()) {
		return;
	}
	if (IsPointerButtonDown()) {
		return;
	}

	wxPoint pos = HostPointToPanel(update.x, update.y);

	const wxMouseState mouse = wxGetMouseState();
	const wxPoint current = ScreenToClient(wxPoint(mouse.GetX(), mouse.GetY()));
	const int drift_x = std::abs(current.x - pos.x);
	const int drift_y = std::abs(current.y - pos.y);

	if (drift_x <= 1 && drift_y <= 1) {
		last_mouse_x_ = update.x;
		last_mouse_y_ = update.y;
		return;
	}

	if (IsUserPointerActive() &&
	    drift_x < kReanchorSlack && drift_y < kReanchorSlack) {
		return;
	}

	/*
	 * ★ A pointer the guest is holding in one place must not trap the host's.
	 *
	 * RISC OS sets a mouse bounding box for some of its own windows - an error
	 * box being the one people meet - and while that is up it pins its pointer
	 * inside it. Every frame it asks for the same position back. Re-anchoring on
	 * each of those requests, which is what the drift rule above does once the
	 * user has moved more than a few pixels away, drags the host pointer back
	 * into the box over and over: it cannot be moved off the error window, or out
	 * of the emulator window at all. That is issue #140, and it was introduced by
	 * the fix for #128 - one asks for the pointer to be put back more often, the
	 * other for it to be left alone.
	 *
	 * What separates them is repetition. A genuine re-anchor is the guest moving
	 * its pointer somewhere new and the host's needing to catch up, which happens
	 * once. A bounding box is the same position asked for again and again while
	 * the user is trying to move away. So a few requests for a position already
	 * warped to, with the user actively moving, are taken as a clamp and ignored
	 * until the guest asks for somewhere different - at which point the count
	 * resets and re-anchoring works as it did.
	 */
	static const int kRepeatedWarpsBeforeRelease = 3;

	if (pos == warp_target_) {
		if (warp_repeats_ < kRepeatedWarpsBeforeRelease) {
			warp_repeats_++;
		} else if (IsUserPointerActive()) {
			/* Held in one place while the user moves: let them go. The guest
			   keeps its pointer where RISC OS wants it, which is the behaviour
			   RISC OS is asking for; it is only the host pointer that must not
			   be dragged along with it. */
			return;
		}
	} else {
		warp_target_ = pos;
		warp_repeats_ = 0;
	}

	WarpPointer(pos.x, pos.y);
	last_mouse_x_ = update.x;
	last_mouse_y_ = update.y;
}

void EmulatorPanel::CalculateScaling()
{
	const wxSize client = GetClientSize();

	if (image_width_ <= 0 || image_height_ <= 0) {
		return;
	}

	if (double_size_ & VIDC_DOUBLE_X) {
		host_xsize_ = image_width_ * 2;
	} else {
		host_xsize_ = image_width_;
	}
	if (double_size_ & VIDC_DOUBLE_Y) {
		host_ysize_ = image_height_ * 2;
	} else {
		host_ysize_ = image_height_;
	}

	/*
	 * One rule: draw the guest's picture at a whole-number scale and centre it.
	 *
	 * There is no aspect-ratio fitting and no stretching. Both existed to paper
	 * over a window whose size did not match the guest's mode, and now that the
	 * window is the configured screen size, the only times they differ are while
	 * RISC OS is booting through other modes and when whole-multiple scaling has
	 * been asked for. Neither wants the picture distorted - a boot message
	 * stretched to an odd shape looks like a fault, and whole multiples exist
	 * precisely to keep pixels square.
	 *
	 * Full screen scales up as far as whole multiples allow, for the same reason:
	 * sharp and centred beats filling the screen with a soft picture.
	 */
	const int scale_x = host_xsize_ > 0 ? client.x / host_xsize_ : 0;
	const int scale_y = host_ysize_ > 0 ? client.y / host_ysize_ : 0;
	int scale = std::min(scale_x, scale_y);

	if (scale < 1) {
		scale = 1;	/* Smaller than the picture: centre it and clip */
	}
	if (display_scaling_ == DisplayScaling_ActualSize && !full_screen_) {
		scale = 1;	/* Exactly what it says */
	}

	scaled_x_ = host_xsize_ * scale;
	scaled_y_ = host_ysize_ * scale;
	offset_x_ = (client.x - scaled_x_) / 2;
	offset_y_ = (client.y - scaled_y_) / 2;

	/* One guest pixel is a different size on screen now, so the pointer has
	   to be rebuilt at the new size. Cheap: it returns at once unless the
	   scaling actually moved. */
	RebuildGuestCursor();
}

#if wxUSE_GLCANVAS
bool EmulatorPanel::GlActive() const
{
	return gl_canvas_ != nullptr && gl_canvas_->IsUsable();
}

/*
 * Set up the GPU path, once.
 *
 * Attempted on every platform, unlike the Manager's panel which skips it on
 * Windows: that one draws through wxGraphicsContext, which is Direct2D and
 * already accelerated there, whereas this panel has always used a plain
 * wxBufferedPaintDC everywhere. So Windows has the same full-frame rescale to
 * lose as macOS and Linux.
 */
void EmulatorPanel::TryCreateGlCanvas()
{
	if (gl_tried_) {
		return;
	}
	gl_tried_ = true;

	if (!HardwareAccelerationWanted()) {
		rpclog("Display: hardware acceleration is off, so the guest's screen "
		       "is drawn on the CPU\n");
		return;
	}

	gl_canvas_ = new GlDisplayCanvas(this);
	gl_canvas_->SetSize(GetClientSize());
	gl_undecided_paints_ = 0;

	/* The canvas asks for the newest frame from inside its paint, so the upload
	   is always followed by the swap that retires it. */
	gl_canvas_->SetFrameSupplier([this] { SupplyFrameToGl(); });
	/* Born with the default arrow otherwise, which UpdateMouseCursor() would
	   only correct the next time something happened to call it. */
	UpdateMouseCursor();

	/* Input keeps coming to this panel. The canvas covers it exactly, so the
	   coordinates in its events are already panel coordinates. Bound one by one
	   rather than by pushing this panel as the canvas's handler, because this
	   panel also handles EVT_PAINT and EVT_SIZE and letting those fire for the
	   canvas would have the CPU path drawing over the GPU one. */
	gl_canvas_->Bind(wxEVT_MOTION, &EmulatorPanel::OnMouseMove, this);
	gl_canvas_->Bind(wxEVT_LEFT_DOWN, &EmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_DOWN, &EmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_RIGHT_DOWN, &EmulatorPanel::OnMouseDown, this);
	gl_canvas_->Bind(wxEVT_LEFT_DCLICK, &EmulatorPanel::OnMouseDoubleClick, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_DCLICK, &EmulatorPanel::OnMouseDoubleClick, this);
	gl_canvas_->Bind(wxEVT_RIGHT_DCLICK, &EmulatorPanel::OnMouseDoubleClick, this);
	gl_canvas_->Bind(wxEVT_LEFT_UP, &EmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_MIDDLE_UP, &EmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_RIGHT_UP, &EmulatorPanel::OnMouseUp, this);
	gl_canvas_->Bind(wxEVT_MOUSEWHEEL, &EmulatorPanel::OnMouseWheel, this);
	gl_canvas_->Bind(wxEVT_ENTER_WINDOW, &EmulatorPanel::OnEnterWindow, this);
	gl_canvas_->Bind(wxEVT_LEAVE_WINDOW, &EmulatorPanel::OnLeaveWindow, this);

	/*
	 * ★ And the keyboard, which is not optional here.
	 *
	 * SetFocus() on this panel lands on the canvas instead: wxGTK gives focus to
	 * the first focusable child, and a wxGLCanvas accepts it. So the panel never
	 * holds focus once the canvas exists, and MainFrame's handlers - bound on the
	 * panel - saw nothing at all. No typing reached RISC OS, and Alt+Enter could
	 * not leave full screen, because that is decided in the same handler.
	 *
	 * Forwarded rather than handled: the frame owns what a key means, and one
	 * place deciding that is what keeps the panel out of it.
	 */
	gl_canvas_->Bind(wxEVT_KEY_DOWN, &EmulatorPanel::ForwardKeyEvent, this);
	gl_canvas_->Bind(wxEVT_KEY_UP, &EmulatorPanel::ForwardKeyEvent, this);

	/* Whatever had focus, the canvas is what has it now; say so, so the first
	   keystroke is not lost to a window that has gone. */
	gl_canvas_->SetFocus();
}

/*
 * Hand a key event to whoever is listening on this panel.
 *
 * Raised on this panel rather than left to Skip(): an unhandled event would
 * climb to the parent anyway, but only if nothing on the canvas consumed it
 * first, and wxGLCanvas is not required to leave it alone. Sending it here
 * directly is what the frame's binding is waiting for.
 */
void EmulatorPanel::ForwardKeyEvent(wxKeyEvent &event)
{
	if (!GetEventHandler()->ProcessEvent(event)) {
		event.Skip();
	}
}

void EmulatorPanel::DestroyGlCanvas(const wxString &why)
{
	if (gl_canvas_ == nullptr) {
		return;
	}

	rpclog("Display: OpenGL unavailable (%s); drawing the guest's screen on "
	       "the CPU instead\n", why.utf8_str().data());

	gl_canvas_->Destroy();
	gl_canvas_ = nullptr;

	/* The window holding the focus has just gone, so take it back: without this
	   the keyboard dies the moment GL is given up on. */
	SetFocus();

	/* The CPU path has drawn nothing so far, so its cache is empty rather than
	   stale: the next frame rebuilds it from scratch. */
	display_bitmap_ = wxBitmap();
	Refresh(false);
}

/*
 * Keep the newest frame, and remember which rows changed.
 *
 * Nothing is uploaded here. The canvas takes it from SupplyFrameToGl() during
 * its own paint, which is what lets the upload be followed by the buffer swap
 * that retires it - gl_display_canvas.h is explicit that uploading anywhere else
 * causes trouble, and this used to call UpdateFrame() straight from here.
 */
void EmulatorPanel::StoreFrameForGl(const VideoUpdate &update)
{
	const size_t needed = (size_t) update.xsize * (size_t) update.ysize;

	if (gl_frame_w_ != update.xsize || gl_frame_h_ != update.ysize ||
	    gl_frame_.size() != needed) {
		gl_frame_.assign(needed, 0);
		gl_frame_w_ = update.xsize;
		gl_frame_h_ = update.ysize;
		/* A new buffer holds none of the old frame, so all of it is stale
		   however few rows this update claims to have touched. */
		gl_dirty_yl_ = 0;
		gl_dirty_yh_ = update.ysize;
	}

	int yl = std::max(0, update.yl);
	int yh = std::min(update.yh, update.ysize);

	if (gl_dirty_yl_ == 0 && gl_dirty_yh_ == update.ysize) {
		/* Already owing a full frame: copy it all rather than this band. */
		yl = 0;
		yh = update.ysize;
	}

	if (yh > yl) {
		std::memcpy(gl_frame_.data() + (size_t) yl * (size_t) update.xsize,
		            update.buffer + (size_t) yl * (size_t) update.xsize,
		            (size_t) (yh - yl) * (size_t) update.xsize * sizeof(uint32_t));

		/* The union, because several frames can arrive between paints. */
		if (gl_dirty_yl_ < 0 || yl < gl_dirty_yl_) {
			gl_dirty_yl_ = yl;
		}
		if (yh > gl_dirty_yh_) {
			gl_dirty_yh_ = yh;
		}
	}

	gl_canvas_->SetSize(GetClientSize());
	gl_canvas_->SetDisplayRect(wxRect(offset_x_, offset_y_, scaled_x_, scaled_y_));
	gl_canvas_->Refresh(false);
}

/* Called by the canvas from inside its paint. */
void EmulatorPanel::SupplyFrameToGl()
{
	if (gl_frame_.empty() || gl_frame_w_ <= 0 || gl_frame_h_ <= 0) {
		return;
	}

	/* A range of nothing is passed on rather than skipped: the canvas forces a
	   full upload of its own accord when it has no frame yet, which is exactly
	   the case on its first paint. */
	gl_canvas_->UpdateFrame(gl_frame_.data(), gl_frame_w_, gl_frame_h_,
	                        gl_dirty_yl_ < 0 ? 0 : gl_dirty_yl_,
	                        gl_dirty_yh_ < 0 ? 0 : gl_dirty_yh_);

	gl_dirty_yl_ = -1;
	gl_dirty_yh_ = -1;
}

#endif /* wxUSE_GLCANVAS */

#if wxUSE_GLCANVAS
/*
 * How many paints a new GL canvas gets to say whether it works before the CPU
 * path takes over for good. Counted in paints because the panel is repainted as
 * frames arrive, so this is frames the guest has produced that GL has not drawn.
 */
static const int kGlUndecidedPaintLimit = 120;
#endif

void EmulatorPanel::OnPaint(wxPaintEvent &event)
{
	wxBufferedPaintDC dc(this);
	(void)event;

#if wxUSE_GLCANVAS
	/* Set up on the first paint that has something to show, not in the
	   constructor: a GL context needs a window that has been realised. */
	if (image_width_ > 0 && image_height_ > 0) {
		TryCreateGlCanvas();
	}

	if (gl_canvas_ != nullptr) {
		if (gl_canvas_->IsUsable()) {
			/* The canvas covers this panel and has drawn it. */
			return;
		}
		if (!gl_canvas_->Failure().empty()) {
			DestroyGlCanvas(gl_canvas_->Failure());
		} else if (++gl_undecided_paints_ > kGlUndecidedPaintLimit) {
			/* It never answered. Bounded deliberately: a canvas that stays
			   undecided must not mean a window that is never drawn, which is
			   exactly the fault that made a black machine screen so hard to
			   find on macOS. */
			DestroyGlCanvas("it never became ready");
		}

		/*
		 * Either way, fall through and draw on the CPU for this paint.
		 *
		 * A canvas waiting to be shown on screen is neither working nor failed,
		 * and clearing the panel black while it made up its mind put a black box
		 * on screen for the first few frames of every boot. There is nothing to
		 * gain by it: the CPU bitmap is kept up to date for exactly as long as
		 * the canvas is not drawing, so the picture is there to be drawn, and a
		 * frame that is about to be covered is better than a hole.
		 */
	}
#endif

	if (!display_bitmap_.IsOk() || image_width_ <= 0 || image_height_ <= 0) {
		return;
	}

	wxMemoryDC memDC;
	memDC.SelectObject(display_bitmap_);

	wxRect dest = GetUpdateRegion().GetBox();
	if (dest.IsEmpty()) {
		dest = GetClientRect();
	}

	{
		if ((dest.x < offset_x_) || (dest.y < offset_y_) ||
		    (dest.x + dest.width > offset_x_ + scaled_x_) ||
		    (dest.y + dest.height > offset_y_ + scaled_y_)) {
			dc.SetBrush(*wxBLACK_BRUSH);
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(dest);
		}

		if (scaled_x_ <= 0 || scaled_y_ <= 0) {
			return;
		}

		wxRect blit_dest = dest;
		blit_dest.Intersect(wxRect(offset_x_, offset_y_, scaled_x_, scaled_y_));
		if (blit_dest.IsEmpty()) {
			return;
		}

		/*
		 * Rescaling the whole guest frame to the whole screen on every paint
		 * call, even when GetUpdateRegion() said only a few rows were dirty,
		 * is what pegged a core on macOS: a hardware cursor gets re-marked
		 * dirty every vsync frame whether or not it moved (vidcthread_gfxcard()
		 * in vidc20.c), so this ran a full-frame CoreGraphics rescale up to 60
		 * times a second even on a static desktop - dropping the refresh rate
		 * didn't help because the per-frame cost, not the rate, dominated.
		 *
		 * Map the invalidated destination rect back to source pixels instead,
		 * padded by one guest pixel on each side so the two rects' scaled edges
		 * always overlap and no seam is left between repaints, and blit only
		 * that band - mirroring what the unscaled path below already does.
		 */
		int sx0 = (blit_dest.x - offset_x_) * image_width_ / scaled_x_;
		int sx1 = ((blit_dest.x + blit_dest.width - offset_x_) * image_width_ + scaled_x_ - 1) / scaled_x_;
		int sy0 = (blit_dest.y - offset_y_) * image_height_ / scaled_y_;
		int sy1 = ((blit_dest.y + blit_dest.height - offset_y_) * image_height_ + scaled_y_ - 1) / scaled_y_;

		/*
		 * ★ Snap the band out to where the scaling is exact.
		 *
		 * StretchBlit scales this band by (dst rows / src rows), and those are
		 * two small integers. Rounding them independently gives a band whose
		 * scale is not the scale the rest of the frame was drawn at: for a
		 * 768-line guest shown at 900, a twelve-row band came out fifteen rows
		 * tall, a ratio of 1.2500 against the frame's 1.1719. Everything inside
		 * such a band lands about a pixel from where a full repaint puts it, so
		 * whatever the pointer passed over appeared to shift as it was
		 * repainted underneath it.
		 *
		 * source_row * scaled / image is exact whenever source_row is a
		 * multiple of image / gcd(image, scaled), so snapping the edges out to
		 * that lattice makes the band's scale exactly the frame's scale. The
		 * band grows - by two to eight rows for most mode and window sizes,
		 * and in the worst case to the whole axis, which is what this was
		 * doing before the partial blit existed and so costs nothing that was
		 * not already being paid.
		 */
		const int period_x = image_width_ / std::gcd(image_width_, scaled_x_);
		const int period_y = image_height_ / std::gcd(image_height_, scaled_y_);

		sx0 = (std::max(0, sx0 - 1) / period_x) * period_x;
		sy0 = (std::max(0, sy0 - 1) / period_y) * period_y;
		sx1 = std::min(image_width_,
		    ((std::min(image_width_, sx1 + 1) + period_x - 1) / period_x) * period_x);
		sy1 = std::min(image_height_,
		    ((std::min(image_height_, sy1 + 1) + period_y - 1) / period_y) * period_y);

		if (sx1 <= sx0 || sy1 <= sy0) {
			return;
		}

		/* Exact, now the edges are on the lattice: no rounding is left to do,
		   and the ceiling these used to carry would reintroduce the error. */
		const int dx0 = offset_x_ + (sx0 * scaled_x_) / image_width_;
		const int dx1 = offset_x_ + (sx1 * scaled_x_) / image_width_;
		const int dy0 = offset_y_ + (sy0 * scaled_y_) / image_height_;
		const int dy1 = offset_y_ + (sy1 * scaled_y_) / image_height_;

		dc.StretchBlit(dx0, dy0, dx1 - dx0, dy1 - dy0, &memDC, sx0, sy0,
		               sx1 - sx0, sy1 - sy0, wxCOPY, false);
		return;
	}

	const wxRect host_rect(0, 0, host_xsize_, host_ysize_);
	dest.Intersect(host_rect);
	if (dest.IsEmpty()) {
		return;
	}

	wxRect source;
	switch (double_size_) {
	case VIDC_DOUBLE_NONE:
		source = dest;
		break;
	case VIDC_DOUBLE_X:
		source = wxRect(dest.x / 2, dest.y, dest.width / 2, dest.height);
		break;
	case VIDC_DOUBLE_Y:
		source = wxRect(dest.x, dest.y / 2, dest.width, dest.height / 2);
		break;
	case VIDC_DOUBLE_BOTH:
		source = wxRect(dest.x / 2, dest.y / 2, dest.width / 2, dest.height / 2);
		break;
	default:
		source = dest;
		break;
	}

	dc.StretchBlit(dest.x, dest.y, dest.width, dest.height, &memDC, source.x, source.y,
	               source.width, source.height, wxCOPY, false);
}

void EmulatorPanel::OnSize(wxSizeEvent &event)
{
	CalculateScaling();
	Refresh(false);
	event.Skip();
}

int EmulatorPanel::MapClickButton(const wxMouseEvent &event) const
{
	switch (event.GetButton()) {
	case wxMOUSE_BTN_LEFT:
		return 1;
	case wxMOUSE_BTN_RIGHT:
		return 2;
	case wxMOUSE_BTN_MIDDLE:
		return 4;
	default:
		return 0;
	}
}

void EmulatorPanel::OnMouseMove(wxMouseEvent &event)
{
	if (pconfig_copy == nullptr) {
		event.Skip();
		return;
	}

	MarkUserPointerActivity();

	if (!pconfig_copy->mousehackon && mouse_captured) {
		const wxPoint middle = CaptureCentre();
		const wxSize client = GetClientSize();
		int dx = 0, dy = 0, recentre = 0;
		const int moved = captured_pointer_motion(&captured_pointer_,
		    event.GetX(), event.GetY(),
		    client.GetWidth(), client.GetHeight(), middle.x, middle.y,
		    &dx, &dy, &recentre);

		/*
		 * ★ Warped only when the pointer is nearing an edge, not on every event.
		 *
		 * Warping every time is what made captured mode slower than the ordinary
		 * one: a warp per motion event, each with an X call and an extra event of
		 * its own, on the same thread that paints the machine's screen. See
		 * captured_pointer.h.
		 */
		if (recentre) {
			WarpPointer(middle.x, middle.y);
		}
		if (!moved) {
			event.Skip();
			return;
		}

		const int rawdx = dx, rawdy = dy;
		/* The captured pointer delta is measured in host-window pixels; when the
		   display is scaled down the guest is larger than the window, so scale
		   the delta up to guest units or the pointer crawls. */
		if (scaled_x_ > 0 && scaled_y_ > 0) {
			dx = (dx * host_xsize_) / scaled_x_;
			dy = (dy * host_ysize_) / scaled_y_;
		}
		if (captured_pointer_debug_wanted()) {
			rpclog("MOUSEDBG ev=%d,%d mid=%d,%d raw=%d,%d sent=%d,%d host=%dx%d scaled=%dx%d off=%d,%d full=%d scaling=%d moves=%lu recentres=%lu\n",
			       event.GetX(), event.GetY(), middle.x, middle.y, rawdx, rawdy, dx, dy,
			       host_xsize_, host_ysize_, scaled_x_, scaled_y_, offset_x_, offset_y_,
			       full_screen_, display_scaling_,
			       captured_pointer_.moves, captured_pointer_.recentres);
		}
		if (captured_pointer_debug_wanted()) {
			ReportCapturedPointerRate();
		}
		emulator_.MouseMoveRelative(dx, dy);
	} else if (pconfig_copy->mousehackon) {
		SyncMousePosition(event.GetX(), event.GetY());
	}

	event.Skip();
}

void EmulatorPanel::ForwardMousePress(const wxMouseEvent &event)
{
	if (pconfig_copy != nullptr && pconfig_copy->mousehackon) {
		SyncMousePosition(event.GetX(), event.GetY());
	}

	const int buttons = MapClickButton(event);
	if (buttons == 0) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (buttons == last_press_button_ &&
	    now - last_press_time_ < std::chrono::milliseconds(80)) {
		return;
	}

	last_press_button_ = buttons;
	last_press_time_ = now;
	held_buttons_ |= buttons;
	CapturePointerForDrag();
	emulator_.MousePress(buttons);
}

void EmulatorPanel::CapturePointerForDrag()
{
	/* Windows, unlike GTK/X11, does not implicitly grab the pointer for the
	   duration of a button press. Without an explicit capture a button
	   released after the pointer has wandered outside the panel (e.g. dragging
	   a window's resize corner past the edge of the RPCEmu area) never
	   delivers its EVT_*_UP here, leaving RISC OS convinced the button is
	   still down. Capturing the mouse for the lifetime of the drag guarantees
	   the matching release is delivered wherever it happens. Only needed in
	   follows-host (mousehack) mode; capture mode already pins the pointer to
	   the window centre so its release can never escape. */
	if (!pointer_captured_ && pconfig_copy != nullptr && pconfig_copy->mousehackon) {
		CaptureMouse();
		pointer_captured_ = true;
	}
}

void EmulatorPanel::ReleasePointerAfterDrag()
{
	if (pointer_captured_ && held_buttons_ == 0) {
		if (HasCapture()) {
			ReleaseMouse();
		}
		pointer_captured_ = false;
	}
}

void EmulatorPanel::OnMouseDown(wxMouseEvent &event)
{
	FocusPanel();
	MarkUserPointerActivity();

	if (pconfig_copy != nullptr && !pconfig_copy->mousehackon && !mouse_captured) {
		mouse_captured = 1;
		/* From where the pointer is, not from the middle: the click that captures
		   should not make the mouse jump. */
		captured_pointer_begin(&captured_pointer_, event.GetX(), event.GetY());
		UpdateMouseCursor();
		if (auto *frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame)) {
			frame->UpdateMachineStatus();
		}
		return;
	}

	ForwardMousePress(event);
	event.Skip();
}

void EmulatorPanel::OnMouseDoubleClick(wxMouseEvent &event)
{
	if (pconfig_copy == nullptr) {
		event.Skip();
		return;
	}

	FocusPanel();
	MarkUserPointerActivity();
	ForwardMousePress(event);
	event.Skip();
}

void EmulatorPanel::OnMouseUp(wxMouseEvent &event)
{
	MarkUserPointerActivity();

	if (pconfig_copy != nullptr && pconfig_copy->mousehackon) {
		SyncMousePosition(event.GetX(), event.GetY());
	}

	const int buttons = MapClickButton(event);
	if (buttons != 0) {
		last_press_button_ = 0;
		held_buttons_ &= ~buttons;
		emulator_.MouseRelease(buttons);
		ReleasePointerAfterDrag();
	}
	event.Skip();
}

void EmulatorPanel::OnMouseCaptureLost(wxMouseCaptureLostEvent &event)
{
	/* The OS can revoke the capture out from under us (Alt-Tab, another window
	   grabbing input, etc.). Treat that as a release of every button we still
	   believe is held so RISC OS is never left with a phantom pressed button. */
	pointer_captured_ = false;
	if (held_buttons_ != 0) {
		emulator_.MouseRelease(held_buttons_);
		held_buttons_ = 0;
	}
	last_press_button_ = 0;
	(void)event;
}

void EmulatorPanel::OnEnterWindow(wxMouseEvent &event)
{
	FocusPanel();
	last_mouse_x_ = -1;
	last_mouse_y_ = -1;
	UpdateMouseCursor();
	event.Skip();
}

void EmulatorPanel::OnLeaveWindow(wxMouseEvent &event)
{
	if (!mouse_captured) {
		ApplyMouseCursor(wxCursor(wxCURSOR_ARROW));
	}
	event.Skip();
}

/*
 * ★ A wheel click is a quantity of rotation, not the sign of an event.
 *
 * The guest gets one scroll click per event whose rotation is non-zero, in the
 * direction of its sign (podulerom_mouse_wheel_change). For a mouse that is
 * exactly right: one detent is one event carrying a full delta, usually 120.
 *
 * A trackpad does not work like that. It sends a stream of small rotations as
 * the fingers move, and around the ends of a gesture - the start, the stop, the
 * momentum - the sign flickers. Turning each of those into a whole click gives
 * a view that lurches up and down instead of scrolling, which is issue #197.
 *
 * So rotation is accumulated and a click is emitted per GetWheelDelta() worth,
 * with the remainder kept for the next event. A genuine change of direction
 * throws the remainder away rather than letting it cancel the new movement.
 */
void EmulatorPanel::OnMouseWheel(wxMouseEvent &event)
{
	/* Horizontal gestures are not this wheel. A trackpad sends them as a
	   separate axis, and treating them as vertical is scrolling nobody asked
	   for. */
	if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) {
		event.Skip();
		return;
	}

	const int rotation = event.GetWheelRotation();
	int delta = event.GetWheelDelta();

	if (delta <= 0) {
		delta = 120;	/* what a mouse reports; a sane fallback if wx cannot say */
	}
	if ((rotation < 0) != (wheel_accumulator_ < 0)) {
		wheel_accumulator_ = 0;
	}
	wheel_accumulator_ += rotation;

	while (wheel_accumulator_ >= delta) {
		emulator_.MouseWheel(1);
		wheel_accumulator_ -= delta;
	}
	while (wheel_accumulator_ <= -delta) {
		emulator_.MouseWheel(-1);
		wheel_accumulator_ += delta;
	}
	event.Skip();
}
