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

#ifndef EMULATOR_PANEL_H
#define EMULATOR_PANEL_H

#include <chrono>

#include <wx/wx.h>

#include "captured_pointer.h"
#include "emulator_host.h"
#include "host_types.h"

class EmulatorPanel : public wxPanel {
public:
	EmulatorPanel(wxWindow *parent, EmulatorHost &emulator);

	void ApplyVideoUpdate(const VideoUpdate &update);
	void HandleMoveHostMouse(const MouseMoveUpdate &update);
	void ReleaseMouseCapture();
	void UpdateMouseCursor();
	void FocusPanel();
	void SetFullScreen(bool full_screen);
	void SetIntegerScaling(bool integer_scaling);
	void SetFitToWindow(bool fit_to_window);
	void ForceRedraw();
	bool SaveScreenshot(const wxString &path);

private:
	void OnPaint(wxPaintEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnMouseMove(wxMouseEvent &event);
	void OnMouseDown(wxMouseEvent &event);
	void OnMouseUp(wxMouseEvent &event);
	void OnMouseDoubleClick(wxMouseEvent &event);
	void ForwardMousePress(const wxMouseEvent &event);
	void OnMouseWheel(wxMouseEvent &event);
	void OnEnterWindow(wxMouseEvent &event);
	void OnLeaveWindow(wxMouseEvent &event);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent &event);
	void CapturePointerForDrag();
	void ReleasePointerAfterDrag();

	void CalculateScaling();
	void ResizeToHostDisplay();
	void SyncMousePosition(int x, int y);
	bool IsMouseOverPanel() const;
	bool ShouldSuppressHostMouseWarp() const;
	int MapClickButton(const wxMouseEvent &event) const;
	wxPoint HostPointToPanel(int host_x, int host_y) const;
	wxPoint PanelPointToHost(int x, int y) const;
	wxPoint CaptureCentre() const;
	void MarkUserPointerActivity();

	/* Movements and re-centres, logged every few seconds while the pointer is
	   captured and moving. See the definition. */
	void ReportCapturedPointerRate();

	EmulatorHost &emulator_;
	wxImage display_image_;
	wxBitmap display_bitmap_;	/**< Cached bitmap of display_image_, rebuilt only when the frame changes */
	int image_width_ = 640;
	int image_height_ = 480;
	int double_size_ = 0;
	int host_xsize_ = 640;
	int host_ysize_ = 480;
	int scaled_x_ = 640;
	int scaled_y_ = 480;
	int offset_x_ = 0;
	int offset_y_ = 0;
	int last_mouse_x_ = -1;
	int last_mouse_y_ = -1;
	int last_press_button_ = 0;
	int held_buttons_ = 0;		/**< Bitmask of buttons currently forwarded as pressed */
	bool pointer_captured_ = false;	/**< True while we hold the wx mouse capture for a drag */

	/*
	 * Captured-pointer movement: where the pointer was last seen, and how often
	 * it has had to be put back in the middle. See captured_pointer.h for why
	 * that is not every event.
	 */
	struct captured_pointer captured_pointer_{};
	std::chrono::steady_clock::time_point last_press_time_{};
	bool integer_scaling_ = false;
	bool fit_to_window_ = false;
	bool full_screen_ = false;
	std::chrono::steady_clock::time_point user_pointer_until_{};

	wxDECLARE_EVENT_TABLE();
};

#endif
