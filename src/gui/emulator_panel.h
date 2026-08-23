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
#include <vector>

#include <wx/wx.h>

#include "gl_display_canvas.h"

#include "emulator_host.h"
#include "host_types.h"

class EmulatorPanel : public wxPanel {
public:
	EmulatorPanel(wxWindow *parent, EmulatorHost &emulator);

	void ApplyVideoUpdate(const VideoUpdate &update);
	void HandleMoveHostMouse(const MouseMoveUpdate &update);
	void ReleaseMouseCapture();
	void UpdateMouseCursor();
	wxCursor ChooseMouseCursor() const;
	void ApplyMouseCursor(const wxCursor &cursor);
	void FocusPanel();
	void SetFullScreen(bool full_screen);

	/**
	 * The size of the guest's desktop in guest pixels, or (0, 0) before the
	 * first frame has arrived.
	 *
	 * Already doubled where VIDC is doubling, so it is the number of pixels a
	 * 1:1 window would have to be, not the number RISC OS thinks it has.
	 */
	wxSize GuestScreenSize() const { return wxSize(host_xsize_, host_ysize_); }
	/**
	 * Set how the guest's screen is drawn: a DisplayScaling.
	 *
	 * The panel's SIZE is not set here. It comes from the guest's own screen mode
	 * (see SizeToGuest()), because a window smaller than the desktop clips it -
	 * losing the icon bar off the bottom - and a window larger than it wastes the
	 * difference on a border.
	 */
	void SetDisplayMode(int scaling);

	/**
	 * Size this panel to the guest's current screen mode.
	 *
	 * Called by the frame, never from a video update. That is the difference that
	 * matters: RISC OS changes mode two or three times while it boots, and on a
	 * machine with the graphics card the display is handed over part way through
	 * as well. Resizing on each one made the window jump through three sizes and
	 * positions before settling, so the frame waits for the changes to stop and
	 * then calls this once. See MainFrame::NoteGuestFrame().
	 */
	void SizeToGuest();
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
#if wxUSE_GLCANVAS
	void TryCreateGlCanvas();
	void DestroyGlCanvas(const wxString &why);
	bool GlActive() const;
	void StoreFrameForGl(const VideoUpdate &update);
	void SupplyFrameToGl();
#endif
	void ResizeToHostDisplay();
	void SyncMousePosition(int x, int y);
	bool IsMouseOverPanel() const;
	bool IsPointerButtonDown() const;
	bool IsUserPointerActive() const;
	int MapClickButton(const wxMouseEvent &event) const;
	wxPoint HostPointToPanel(int host_x, int host_y) const;
	wxPoint PanelPointToHost(int x, int y) const;
	wxPoint CaptureCentre() const;
	void MarkUserPointerActivity();

	EmulatorHost &emulator_;
	/*
	 * The GPU path. When this is up it covers the panel and draws the guest's
	 * screen as a texture, so the wxImage/wxBitmap conversion and the
	 * full-frame CoreGraphics rescale below are skipped entirely. Null when the
	 * setting is off, when the platform has no GL, or when a canvas was tried
	 * and did not come up.
	 */
#if wxUSE_GLCANVAS
	GlDisplayCanvas *gl_canvas_ = nullptr;
	bool gl_tried_ = false;
	int gl_undecided_paints_ = 0;

	/*
	 * The newest guest frame, and which of its rows have changed since the
	 * canvas last took one.
	 *
	 * A copy, because the canvas must be handed its pixels from inside its own
	 * paint (see SetFrameSupplier in gl_display_canvas.h) and the emulator's
	 * buffer is not ours to read at that moment. Copying the dirty rows is far
	 * cheaper than the wxImage conversion this replaces, and it is what lets a
	 * burst of frames become one upload of the union of their rows.
	 */
	std::vector<uint32_t> gl_frame_;
	int gl_frame_w_ = 0;
	int gl_frame_h_ = 0;
	int gl_dirty_yl_ = -1;
	int gl_dirty_yh_ = -1;
#endif

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
	std::chrono::steady_clock::time_point last_press_time_{};
	int display_scaling_ = DisplayScaling_ActualSize;


	bool full_screen_ = false;
	std::chrono::steady_clock::time_point user_pointer_until_{};

	wxDECLARE_EVENT_TABLE();
};

#endif
