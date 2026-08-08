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
#include <cstdio>

#include <wx/graphics.h>

#include "input_helpers.h"

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
	EVT_MOUSEWHEEL(RemoteEmulatorPanel::OnMouseWheel)
	EVT_KEY_DOWN(RemoteEmulatorPanel::OnKeyDown)
	EVT_KEY_UP(RemoteEmulatorPanel::OnKeyUp)
	EVT_KILL_FOCUS(RemoteEmulatorPanel::OnKillFocus)
wxEND_EVENT_TABLE()

RemoteEmulatorPanel::RemoteEmulatorPanel(wxWindow *parent, const std::string &shared_fb_name,
                                         const std::string &ipc_endpoint)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(640, 480), wxWANTS_CHARS)
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
		std::fprintf(stderr, "Manager: cannot attach to a machine: "
		    "framebuffer '%s' %s, control socket '%s' %s\n",
		    shared_fb_name.c_str(), fb_ok ? "opened" : "FAILED",
		    ipc_endpoint.c_str(), ipc_ok ? "connected" : "FAILED");
		ipc_client_.Disconnect();
		shared_fb_.Close();
	}

	UpdateCursor();
}

RemoteEmulatorPanel::~RemoteEmulatorPanel()
{
	ipc_client_.Disconnect();
}

/*
 * Hide the host arrow while a machine is being shown.
 *
 * This panel always runs in the absolute "guest pointer follows the host one"
 * mode (see the class comment), so RISC OS draws its own pointer wherever the
 * host pointer is. Leaving the host arrow visible as well put two pointers on
 * screen, one on top of the other - EmulatorPanel blanks it for exactly this
 * reason in UpdateMouseCursor().
 *
 * The arrow comes back when the machine has gone, where the panel is showing a
 * "Machine stopped" message and there is no guest pointer to replace it.
 */
void RemoteEmulatorPanel::UpdateCursor()
{
	SetCursor(wxCursor(live_ ? wxCURSOR_BLANK : wxCURSOR_ARROW));
}

void RemoteEmulatorPanel::SetActive(bool active)
{
	active_ = active;
	if (active_ && live_) {
		/* Show whatever the machine last drew immediately, rather than
		   waiting for the guest to draw something new after the switch -
		   the shared framebuffer already holds it. */
		frame_dirty_ = true;
		Refresh(false);
		SetFocus();
	}
}

void RemoteEmulatorPanel::HandleIpcEvent(const IpcEvent &event)
{
	switch (event.type) {
	case IpcEventType::FrameReady:
		if (active_) {
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
	case IpcEventType::TitleChanged:
		/* Nothing shown for these yet in the Manager - see the class
		   comment for what is deferred. */
		break;
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

	/*
	 * ★ Kept at the guest's own size. The scaling is done by the blit.
	 *
	 * This used to wxImage::Rescale() to the panel size on every frame, which
	 * is a full bilinear resample of the whole image in software and was the
	 * single most expensive thing this panel did - most of a CPU core to show
	 * a machine doing nothing. EmulatorPanel has always scaled with
	 * StretchBlit instead, which hands the work to the toolkit; OnPaint now
	 * does the same, so there is nothing to rescale here.
	 */
	display_bitmap_ = wxBitmap(image);

	/* No Refresh() here: this is called from OnPaint, and asking for another
	   paint from inside one is how a repaint loop starts. */
}

void RemoteEmulatorPanel::OnPaint(wxPaintEvent & /*event*/)
{
	wxPaintDC dc(this);

	if (live_ && frame_dirty_) {
		RefreshFrame();
		frame_dirty_ = false;
	}

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

	if (display_bitmap_.IsOk()) {
		const wxSize client = GetClientSize();
		const int bw = display_bitmap_.GetWidth();
		const int bh = display_bitmap_.GetHeight();

		if (client.GetWidth() == bw && client.GetHeight() == bh) {
			/* Nothing to scale, so nothing to lose: straight copy. */
			wxMemoryDC memDC(display_bitmap_);

			dc.Blit(0, 0, bw, bh, &memDC, 0, 0);
		} else {
			/*
			 * ★ Scaled through a graphics context, and with the aspect
			 * ratio kept.
			 *
			 * StretchBlit is what EmulatorPanel uses, and on GTK it is a
			 * nearest-neighbour copy: every scale factor that is not a whole
			 * number drops and duplicates rows and columns, which is what
			 * made this look pixelated and grainy at any guest resolution.
			 * wxImage::Rescale would look right but resamples the whole
			 * frame in software on the way to every paint, which is what
			 * used to cost a CPU core.
			 *
			 * A graphics context is the third option: Cairo (or Direct2D, or
			 * CoreGraphics) does the filtering itself, so it looks like the
			 * bilinear version and costs like the blit.
			 *
			 * The panel is a fixed shape and the guest's screen is not, so
			 * without this the picture was also stretched out of shape.
			 * Fitted to whichever edge runs out first and centred, with the
			 * background showing through as bars.
			 */
			const double scale = std::min(
			    (double) client.GetWidth() / (double) bw,
			    (double) client.GetHeight() / (double) bh);
			const double dw = (double) bw * scale;
			const double dh = (double) bh * scale;
			const double dx = ((double) client.GetWidth() - dw) / 2.0;
			const double dy = ((double) client.GetHeight() - dh) / 2.0;

			dc.SetBackground(*wxBLACK_BRUSH);
			dc.Clear();

			wxGraphicsContext *gc = wxGraphicsContext::Create(dc);

			if (gc != nullptr) {
				gc->SetInterpolationQuality(wxINTERPOLATION_GOOD);
				gc->DrawBitmap(display_bitmap_, dx, dy, dw, dh);
				delete gc;
			} else {
				/* No graphics context available: better a hard-edged
				   picture than none. */
				wxMemoryDC memDC(display_bitmap_);

				dc.StretchBlit((int) dx, (int) dy, (int) dw, (int) dh,
				    &memDC, 0, 0, bw, bh);
			}
		}
	} else {
		dc.SetBackground(*wxBLACK_BRUSH);
		dc.Clear();
	}
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
		frame_dirty_ = true;
		Refresh(false);
	}
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

wxPoint RemoteEmulatorPanel::PanelPointToGuest(int x, int y) const
{
	const wxSize client = GetClientSize();

	if (client.GetWidth() <= 0 || client.GetHeight() <= 0 || frame_width_ <= 0 || frame_height_ <= 0) {
		return wxPoint(0, 0);
	}

	const int gx = x * frame_width_ / client.GetWidth();
	const int gy = y * frame_height_ / client.GetHeight();

	return wxPoint(std::clamp(gx, 0, frame_width_ - 1), std::clamp(gy, 0, frame_height_ - 1));
}

void RemoteEmulatorPanel::OnMouseMove(wxMouseEvent &event)
{
	if (!live_) {
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

	const int buttons = MapClickButton(event);
	if (buttons == 0) {
		return;
	}

	OnMouseMove(event);
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
