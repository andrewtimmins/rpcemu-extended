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

#ifndef GL_DISPLAY_CANVAS_H
#define GL_DISPLAY_CANVAS_H

#include <wx/wx.h>

#if wxUSE_GLCANVAS

#include <wx/glcanvas.h>

#include <cstdint>
#include <functional>
#include <memory>

/*
 * A machine's screen as a GPU texture.
 *
 * The Manager's alternative to scaling a guest screen on the CPU. Where the
 * platform's own renderer is hardware accelerated - Direct2D on Windows - that
 * is used instead and this is not needed; where it is not, the choice was
 * between an expensive software resample and a grainy one, and neither is
 * necessary when there is a GPU sitting idle.
 *
 * What it buys, beyond the scaling being free:
 *
 *   - No pixel conversion. The guest's framebuffer is 0x00RRGGBB, which on a
 *     little-endian host is the bytes B,G,R,0 - exactly GL_BGRA with
 *     GL_UNSIGNED_BYTE. The whole RGB24 pass disappears rather than moving.
 *   - No copy out of shared memory. SharedFramebuffer::AcquireFront() hands over
 *     a pointer, and glTexSubImage2D reads it there and then, which the triple
 *     buffer makes safe.
 *   - Only the rows the guest redrew are uploaded, the same dirty range the CPU
 *     path uses.
 *
 * So a frame costs one upload of a band and one draw call, and the filtering is
 * full quality whatever the scale factor - including while the user is dragging
 * something, where the CPU path has to drop to nearest neighbour to keep up.
 *
 * Minification is trilinear off mipmaps, not plain GL_LINEAR. Plain linear
 * samples four texels however many are really being averaged away, which thins
 * and breaks single-pixel text as the guest screen shrinks into the panel - the
 * same fault that made wxINTERPOLATION_GOOD unusable here, and the same
 * complaint nearest-neighbour scaling drew. Where mipmaps cannot be generated
 * the filter drops to linear and says so in the log rather than quietly looking
 * wrong.
 *
 * Every part of this can fail: no GLX at all on a remote display, a context
 * that will not create, a driver without the pieces it needs. IsUsable() is the
 * only thing the caller should trust, and a false answer means the caller keeps
 * its own drawing. Nothing here is allowed to leave a black window behind.
 */
class GlDisplayCanvas : public wxGLCanvas {
public:
	explicit GlDisplayCanvas(wxWindow *parent);
	~GlDisplayCanvas() override;

	/*
	 * Whether this canvas can actually draw. False after a failed context or a
	 * failed first render, in which case the caller must fall back; it is
	 * checked again after the first paint, because on GLX a context can be
	 * created and only then fail to become current.
	 */
	bool IsUsable() const { return usable_; }

	/* Why it is not usable, for the log. */
	const wxString &Failure() const { return failure_; }

	/*
	 * Hand over the newest frame. `pixels` is read during this call and not
	 * kept. `dirty_top`/`dirty_bottom` are the half-open range of guest rows
	 * that changed; passing the whole height is always correct and is what a
	 * first frame or a mode change wants.
	 *
	 * ★ MUST be called from the supplier set by SetFrameSupplier(), so that a
	 * buffer swap always follows. See that comment: calling it anywhere else
	 * livelocks the driver.
	 */
	void UpdateFrame(const uint32_t *pixels, int width, int height,
	                 int dirty_top, int dirty_bottom);

	/*
	 * ★ Who to ask for the newest frame, called from the paint immediately
	 * before the picture is drawn. The supplier is expected to call
	 * UpdateFrame() if it has anything new.
	 *
	 * This exists because an upload has to be followed by a swap, and only the
	 * paint swaps.
	 *
	 * The frame used to be uploaded the moment it arrived, on the grounds that a
	 * band is a transfer rather than a pass over the picture, so there was
	 * nothing left worth coalescing. That was wrong in a way that took a hung
	 * Manager and its backtrace to see. glTexSubImage2D does not merely cost what
	 * it transfers: it puts work in the driver's queue, and the queue is drained
	 * by presenting a frame. Uploading on arrival at sixty frames a second while
	 * the window was not being painted - the user had switched to another
	 * application, or the guest had just changed screen mode - filled that queue,
	 * and NVIDIA's driver then busy-waits in userspace for room. The GUI thread
	 * span at 100% of a core inside glTexSubImage2D, so it never reached the paint
	 * that would have swapped and made room: a livelock that cannot recover, with
	 * no syscalls to show for it.
	 *
	 * Asking at paint time also means no work at all happens for a window nobody
	 * is looking at, and wx's coalescing of Refresh() gives back the batching that
	 * uploading on arrival was trying to avoid needing: a burst of frames becomes
	 * one upload of the union of their rows, and one swap.
	 */
	using FrameSupplier = std::function<void()>;
	void SetFrameSupplier(FrameSupplier supplier) { supply_frame_ = std::move(supplier); }

	/* Where in the canvas the picture goes, aspect-fitted by the caller, which
	   already works this out for the pointer mapping. */
	void SetDisplayRect(const wxRect &rect);

private:
	void OnPaint(wxPaintEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnEraseBackground(wxEraseEvent &event);

	bool EnsureContext();
	void Render();

	std::unique_ptr<wxGLContext> context_;
	bool usable_ = false;
	bool context_tried_ = false;
	wxString failure_;

	unsigned texture_ = 0;
	int texture_width_ = 0;
	int texture_height_ = 0;
	bool have_frame_ = false;
	bool mipmaps_ = false;
	bool mipmaps_checked_ = false;
	/* Both track GL state so it is only changed when the answer changes. */
	bool generating_mipmaps_ = false;
	bool mipmap_filter_set_ = false;

	/* The frame handed over but not yet drawn, and the rows of it that are new.
	   Held as a pointer only for the length of UpdateFrame(). */
	int frame_width_ = 0;
	int frame_height_ = 0;
	wxRect display_rect_;

	FrameSupplier supply_frame_;

	wxDECLARE_EVENT_TABLE();
};

#endif /* wxUSE_GLCANVAS */

#endif /* GL_DISPLAY_CANVAS_H */
