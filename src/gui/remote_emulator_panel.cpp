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

#include "input_helpers.h"

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
		ipc_client_.Disconnect();
		shared_fb_.Close();
	}
}

RemoteEmulatorPanel::~RemoteEmulatorPanel()
{
	ipc_client_.Disconnect();
}

void RemoteEmulatorPanel::SetActive(bool active)
{
	active_ = active;
	if (active_ && live_) {
		/* Show whatever the machine last drew immediately, rather than
		   waiting for the guest to draw something new after the switch -
		   the shared framebuffer already holds it. */
		RefreshFrame();
		SetFocus();
	}
}

void RemoteEmulatorPanel::HandleIpcEvent(const IpcEvent &event)
{
	switch (event.type) {
	case IpcEventType::FrameReady:
		if (active_) {
			RefreshFrame();
		}
		break;
	case IpcEventType::Fatal:
	case IpcEventType::Quit:
		live_ = false;
		if (on_gone_) {
			on_gone_();
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

	const wxSize client = GetClientSize();
	if (client.GetWidth() > 0 && client.GetHeight() > 0) {
		image.Rescale(client.GetWidth(), client.GetHeight(), wxIMAGE_QUALITY_BILINEAR);
	}
	display_bitmap_ = wxBitmap(image);
	Refresh(false);
}

void RemoteEmulatorPanel::OnPaint(wxPaintEvent & /*event*/)
{
	wxPaintDC dc(this);

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
		dc.DrawBitmap(display_bitmap_, 0, 0);
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
	if (live_) {
		RefreshFrame();
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
