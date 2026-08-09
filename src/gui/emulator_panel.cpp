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
		integer_scaling_ = pconfig_copy->integer_scaling != 0;
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

bool EmulatorPanel::ShouldSuppressHostMouseWarp() const
{
	const wxMouseState mouse = wxGetMouseState();
	if (mouse.LeftIsDown() || mouse.RightIsDown() || mouse.MiddleIsDown()) {
		return true;
	}

	return std::chrono::steady_clock::now() < user_pointer_until_;
}

void EmulatorPanel::UpdateMouseCursor()
{
	if (mouse_captured) {
		SetCursor(wxCursor(wxCURSOR_BLANK));
		return;
	}

	if (pconfig_copy != nullptr && pconfig_copy->mousehackon && IsMouseOverPanel()) {
		SetCursor(wxCursor(wxCURSOR_BLANK));
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

void EmulatorPanel::SetIntegerScaling(bool integer_scaling)
{
	integer_scaling_ = integer_scaling;
	ResizeToHostDisplay();
	CalculateScaling();
	Refresh(false);
}

void EmulatorPanel::SetFitToWindow(bool fit_to_window)
{
	fit_to_window_ = fit_to_window;
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
	UpdateMouseCursor();
}

wxPoint EmulatorPanel::PanelPointToHost(int x, int y) const
{
	if (integer_scaling_ || fit_to_window_) {
		const int local_x = x - offset_x_;
		const int local_y = y - offset_y_;
		if (local_x < 0 || local_y < 0 || local_x >= scaled_x_ || local_y >= scaled_y_) {
			return wxPoint(-1, -1);
		}

		return wxPoint(local_x * host_xsize_ / scaled_x_,
		                 local_y * host_ysize_ / scaled_y_);
	}

	return wxPoint(std::clamp(x, 0, std::max(host_xsize_ - 1, 0)),
	               std::clamp(y, 0, std::max(host_ysize_ - 1, 0)));
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

	if (integer_scaling_ || fit_to_window_) {
		return wxPoint(offset_x_ + (panel_x * scaled_x_) / std::max(host_xsize_, 1),
		               offset_y_ + (panel_y * scaled_y_) / std::max(host_ysize_, 1));
	}

	panel_x = std::clamp(panel_x, 0, std::max(GetClientSize().x - 1, 0));
	panel_y = std::clamp(panel_y, 0, std::max(GetClientSize().y - 1, 0));
	return wxPoint(panel_x, panel_y);
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
	if (full_screen_ || fit_to_window_ || host_xsize_ <= 0 || host_ysize_ <= 0) {
		SetMinSize(wxDefaultSize);
		SetMaxSize(wxDefaultSize);
		SetSizeHints(wxDefaultSize, wxDefaultSize);
		return;
	}

	const wxSize host_size(host_xsize_, host_ysize_);

	/*
	 * Pixel-perfect scaling keeps a minimum of one host pixel per guest pixel
	 * and is free to grow to a whole multiple of it.
	 *
	 * The minimum matters: this used to clear it, like the freely-resizable
	 * modes above. Toggling from the menu was fine, because by then the window
	 * had a size of its own, but a machine that started with the setting
	 * already on had a panel contributing no minimum at all, so the frame's
	 * Fit() shrink-wrapped it to just the menu and toolbar. That is issue #47:
	 * "the main window fails to open".
	 */
	if (integer_scaling_) {
		SetMinSize(host_size);
		SetMaxSize(wxDefaultSize);
		SetSizeHints(host_size, wxDefaultSize);
		return;
	}

	SetMinSize(host_size);
	SetMaxSize(host_size);
	SetSize(host_size);
	SetSizeHints(host_size, host_size);
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

	if (!display_image_.IsOk() || display_image_.GetWidth() != update.xsize ||
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

	if (!full_screen_ && !integer_scaling_ && !fit_to_window_ &&
	    (update.host_xsize != host_xsize_ || update.host_ysize != host_ysize_)) {
		host_xsize_ = update.host_xsize;
		host_ysize_ = update.host_ysize;
		recalculate_needed = true;
	}

	/* In fit-to-window mode the guest size still drives the scaling maths, but
	   the window is left free for the user to resize. */
	if (fit_to_window_ &&
	    (update.host_xsize != host_xsize_ || update.host_ysize != host_ysize_)) {
		host_xsize_ = update.host_xsize;
		host_ysize_ = update.host_ysize;
		recalculate_needed = true;
	}

	if (recalculate_needed) {
		CalculateScaling();

		if (!full_screen_ && !fit_to_window_) {
			if (integer_scaling_) {
				ResizeToHostDisplay();
			} else {
				const wxSize host_size(update.host_xsize, update.host_ysize);
				SetMinSize(host_size);
				SetMaxSize(host_size);
				SetSize(host_size);
				SetSizeHints(host_size, host_size);
			}

			if (wxWindow *top = wxGetTopLevelParent(this)) {
				top->Layout();
				if (auto *frame = wxDynamicCast(top, wxFrame)) {
					frame->Fit();
				}
			}
		}

		Refresh(false);
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

	if (full_screen_ || integer_scaling_ || fit_to_window_) {
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
	} else {
		const int height = ymax - ymin;
		RefreshRect(wxRect(0, ymin, width, height), false);
	}
}

void EmulatorPanel::HandleMoveHostMouse(const MouseMoveUpdate &update)
{
	auto *frame = wxDynamicCast(wxGetTopLevelParent(this), MainFrame);
	if (frame != nullptr && !frame->IsWindowActive()) {
		return;
	}
	if (!IsMouseOverPanel()) {
		return;
	}
	if (ShouldSuppressHostMouseWarp()) {
		return;
	}

	wxPoint pos = HostPointToPanel(update.x, update.y);

	const wxMouseState mouse = wxGetMouseState();
	const wxPoint current = ScreenToClient(wxPoint(mouse.GetX(), mouse.GetY()));
	if (std::abs(current.x - pos.x) <= 1 && std::abs(current.y - pos.y) <= 1) {
		last_mouse_x_ = update.x;
		last_mouse_y_ = update.y;
		return;
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

	if (full_screen_ || integer_scaling_ || fit_to_window_) {
		const int widget_x = client.x;
		const int widget_y = client.y;

		if (integer_scaling_) {
			const int scale_x = std::max(1, widget_x / host_xsize_);
			const int scale_y = std::max(1, widget_y / host_ysize_);
			const int scale = std::min(scale_x, scale_y);
			scaled_x_ = host_xsize_ * scale;
			scaled_y_ = host_ysize_ * scale;
		} else {
			if ((widget_x * host_ysize_) >= (widget_y * host_xsize_)) {
				scaled_x_ = (widget_y * host_xsize_) / host_ysize_;
				scaled_y_ = widget_y;
			} else {
				scaled_x_ = widget_x;
				scaled_y_ = (widget_x * host_ysize_) / host_xsize_;
			}
		}

		offset_x_ = (widget_x - scaled_x_) / 2;
		offset_y_ = (widget_y - scaled_y_) / 2;
	} else {
		scaled_x_ = host_xsize_;
		scaled_y_ = host_ysize_;
		offset_x_ = 0;
		offset_y_ = 0;
	}
}

void EmulatorPanel::OnPaint(wxPaintEvent &event)
{
	wxBufferedPaintDC dc(this);
	(void)event;

	if (!display_bitmap_.IsOk() || image_width_ <= 0 || image_height_ <= 0) {
		return;
	}

	wxMemoryDC memDC;
	memDC.SelectObject(display_bitmap_);

	wxRect dest = GetUpdateRegion().GetBox();
	if (dest.IsEmpty()) {
		dest = GetClientRect();
	}

	if (full_screen_ || integer_scaling_ || fit_to_window_) {
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
		WarpPointer(middle.x, middle.y);

		int dx = event.GetX() - middle.x;
		int dy = event.GetY() - middle.y;
		const int rawdx = dx, rawdy = dy;
		/* The captured pointer delta is measured in host-window pixels; when the
		   display is scaled down the guest is larger than the window, so scale
		   the delta up to guest units or the pointer crawls. */
		if ((integer_scaling_ || fit_to_window_ || full_screen_) &&
		    scaled_x_ > 0 && scaled_y_ > 0) {
			dx = (dx * host_xsize_) / scaled_x_;
			dy = (dy * host_ysize_) / scaled_y_;
		}
		if (getenv("RPCEMU_MOUSEDBG") != nullptr) {
			rpclog("MOUSEDBG ev=%d,%d mid=%d,%d raw=%d,%d sent=%d,%d host=%dx%d scaled=%dx%d off=%d,%d full=%d fit=%d\n",
			       event.GetX(), event.GetY(), middle.x, middle.y, rawdx, rawdy, dx, dy,
			       host_xsize_, host_ysize_, scaled_x_, scaled_y_, offset_x_, offset_y_,
			       full_screen_, fit_to_window_);
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
		SetCursor(wxCursor(wxCURSOR_ARROW));
	}
	event.Skip();
}

void EmulatorPanel::OnMouseWheel(wxMouseEvent &event)
{
	emulator_.MouseWheel(event.GetWheelRotation());
	event.Skip();
}
