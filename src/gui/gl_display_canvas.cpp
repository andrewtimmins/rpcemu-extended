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

#include "gl_display_canvas.h"

#if wxUSE_GLCANVAS

#include <cstring>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

extern "C" {
#include "rpcemu.h"
}

/*
 * Constants that may be absent from an old <GL/gl.h> - Windows ships 1.1
 * headers. The values are from the specification and are not negotiable.
 */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

wxBEGIN_EVENT_TABLE(GlDisplayCanvas, wxGLCanvas)
	EVT_PAINT(GlDisplayCanvas::OnPaint)
	EVT_SIZE(GlDisplayCanvas::OnSize)
	EVT_ERASE_BACKGROUND(GlDisplayCanvas::OnEraseBackground)
wxEND_EVENT_TABLE()

/*
 * Asks for the plainest thing that can work: RGBA, double buffered, a depth
 * buffer we do not use but which some drivers are happier having. No core
 * profile, because this draws one textured quad with fixed-function calls that
 * every driver from 1.1 onwards has, and a core profile would demand shaders and
 * a VAO for no gain.
 */
GlDisplayCanvas::GlDisplayCanvas(wxWindow *parent)
	: wxGLCanvas(parent, wxID_ANY, nullptr, wxDefaultPosition, wxDefaultSize,
	    wxFULL_REPAINT_ON_RESIZE)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
}

GlDisplayCanvas::~GlDisplayCanvas()
{
	if (texture_ != 0 && context_ && IsShownOnScreen() && SetCurrent(*context_)) {
		glDeleteTextures(1, &texture_);
	}
}

/*
 * The context cannot be made in the constructor: on GTK the window has no
 * native peer until it is realised, and SetCurrent() on an unrealised window
 * fails. So this runs from the first paint, when the window definitely exists.
 */
bool GlDisplayCanvas::EnsureContext()
{
	if (context_tried_) {
		return usable_;
	}
	context_tried_ = true;

	if (!IsShownOnScreen()) {
		/* Not an error yet - try again on the next paint. */
		context_tried_ = false;
		return false;
	}

	context_.reset(new wxGLContext(this));
	if (!context_->IsOK()) {
		failure_ = "the OpenGL context would not create";
		context_.reset();
		return false;
	}

	if (!SetCurrent(*context_)) {
		failure_ = "the OpenGL context would not become current";
		context_.reset();
		return false;
	}

	const char *version = (const char *) glGetString(GL_VERSION);
	const char *renderer = (const char *) glGetString(GL_RENDERER);

	glGenTextures(1, &texture_);
	if (texture_ == 0) {
		failure_ = "no texture could be created";
		context_.reset();
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, texture_);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	/*
	 * Mipmaps the GL 1.4 way, turned on only while the picture is shrunk - see
	 * UpdateFrame. Deliberately not glGenerateMipmap, which would need an
	 * extension loader on three platforms. A mipmap filter on a texture with no
	 * mipmaps is incomplete and draws NOTHING AT ALL, so the filter stays linear
	 * until an upload has proved a second level exists.
	 */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	usable_ = true;

	rpclog("Manager: OpenGL display in use - %s, %s\n",
	    renderer != nullptr ? renderer : "unknown renderer",
	    version != nullptr ? version : "unknown version");

	return true;
}

void GlDisplayCanvas::SetDisplayRect(const wxRect &rect)
{
	display_rect_ = rect;
}

/*
 * Upload, and only the rows that changed.
 *
 * A new size means a new texture: glTexImage2D with a null pointer allocates
 * without transferring anything, and the rows are then filled by the same
 * glTexSubImage2D path as any other frame, so there is one code path for the
 * upload rather than two.
 */
void GlDisplayCanvas::UpdateFrame(const uint32_t *pixels, int width, int height,
                                  int dirty_top, int dirty_bottom)
{
	if (!usable_ || pixels == nullptr || width <= 0 || height <= 0) {
		return;
	}
	if (!SetCurrent(*context_)) {
		return;
	}

	glBindTexture(GL_TEXTURE_2D, texture_);

	if (width != texture_width_ || height != texture_height_) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
		    GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
		texture_width_ = width;
		texture_height_ = height;
		have_frame_ = false;
		dirty_top = 0;
		dirty_bottom = height;

		/* The new texture has no levels, so the filter must not ask for them
		   until an upload has rebuilt them. */
		if (mipmap_filter_set_) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			mipmap_filter_set_ = false;
		}
		generating_mipmaps_ = false;
	}

	if (!have_frame_) {
		/* Nothing in the texture yet, so a partial upload would leave the rest
		   of it undefined - whatever the driver last had in that memory. */
		dirty_top = 0;
		dirty_bottom = height;
	}

	if (dirty_top < 0) {
		dirty_top = 0;
	}
	if (dirty_bottom > height) {
		dirty_bottom = height;
	}
	if (dirty_bottom <= dirty_top) {
		return;
	}

	/* GL_GENERATE_MIPMAP rebuilds the whole pyramid on every upload however few
	   rows changed, which throws the dirty range away and costs real time on a
	   software renderer. The levels are only sampled when minifying. */
	const bool minified = display_rect_.GetWidth() > 0 &&
	    display_rect_.GetHeight() > 0 &&
	    (display_rect_.GetWidth() < width || display_rect_.GetHeight() < height);

	if (minified != generating_mipmaps_) {
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP,
		    minified ? GL_TRUE : GL_FALSE);
		glGetError();	/* discarded: a core profile rejects the line above */
		generating_mipmaps_ = minified;
	}

	/* Rows of the source are contiguous and full width, so one call does the
	   band. GL_BGRA + GL_UNSIGNED_BYTE is the guest's own 0x00RRGGBB word on a
	   little-endian host, so nothing is converted on the way. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty_top, width,
	    dirty_bottom - dirty_top, GL_BGRA, GL_UNSIGNED_BYTE,
	    pixels + (size_t) dirty_top * (size_t) width);

	frame_width_ = width;
	frame_height_ = height;
	have_frame_ = true;

	/*
	 * Now that something has been uploaded, find out whether the driver really
	 * built the mipmap chain, and only then ask for trilinear minification.
	 * Asking first would risk an incomplete texture, which draws nothing.
	 *
	 * Why bother at all: plain GL_LINEAR samples four texels however many are
	 * really being averaged away, so a guest screen shrunk into a smaller panel
	 * loses single-pixel text - the same fault that ruled out
	 * wxINTERPOLATION_GOOD on the CPU path, and the same complaint
	 * nearest-neighbour drew.
	 */
	if (generating_mipmaps_ && !mipmaps_checked_) {
		int level1 = 0;

		mipmaps_checked_ = true;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &level1);

		if (level1 > 0) {
			mipmaps_ = true;
			rpclog("Manager: OpenGL display has mipmaps, so a scaled-down "
			       "screen is filtered trilinearly\n");
		} else {
			rpclog("Manager: OpenGL display has no automatic mipmaps, so a "
			       "scaled-down screen is filtered linearly\n");
		}
	}

	/* Trilinear only while the levels are being kept up to date; sampling a
	   pyramid that stopped being rebuilt would show a stale, blurred picture. */
	const bool want_mipmap_filter = mipmaps_ && generating_mipmaps_;

	if (want_mipmap_filter != mipmap_filter_set_) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		    want_mipmap_filter ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		mipmap_filter_set_ = want_mipmap_filter;
	}
}

void GlDisplayCanvas::OnSize(wxSizeEvent &event)
{
	Refresh(false);
	event.Skip();
}

void GlDisplayCanvas::OnEraseBackground(wxEraseEvent & /*event*/)
{
	/* All painting happens in OnPaint; erasing first only flickers. */
}

void GlDisplayCanvas::OnPaint(wxPaintEvent & /*event*/)
{
	/* Required even though nothing is drawn through it: on some platforms the
	   paint event must be consumed by a wxPaintDC or it is re-sent for ever. */
	wxPaintDC dc(this);

	if (!EnsureContext()) {
		return;
	}
	if (!SetCurrent(*context_)) {
		usable_ = false;
		failure_ = "the OpenGL context stopped being current";
		return;
	}

	/*
	 * ★ Upload here, and only here, because the SwapBuffers() below is what
	 * lets the driver retire it. See SetFrameSupplier() for what uploading
	 * anywhere else did.
	 */
	if (supply_frame_) {
		supply_frame_();
	}

	Render();
	SwapBuffers();
}

/*
 * One textured quad, in pixel coordinates.
 *
 * An orthographic projection matching the canvas means the quad's corners are
 * the destination rectangle the caller worked out, with no arithmetic here to
 * disagree with the pointer mapping - which is exactly the fault that made the
 * guest pointer travel at the wrong speed on the CPU path.
 */
void GlDisplayCanvas::Render()
{
	const wxSize size = GetClientSize();

	if (size.GetWidth() <= 0 || size.GetHeight() <= 0) {
		return;
	}

	/*
	 * The viewport is in PIXELS; everything else here is in points.
	 *
	 * GetClientSize() answers in logical points, which on a Retina display is
	 * half the drawable's real size. Passing those straight to glViewport left
	 * the picture in the bottom-left quarter of a black panel - the quadrant a
	 * GL viewport starts from - while the projection, the destination rectangle
	 * and the pointer mapping all still agreed with each other in points.
	 *
	 * So the viewport is scaled up to the drawable and the projection is left in
	 * points: the quad's corners stay the destination rectangle the caller
	 * worked out, and no other coordinate here has to know about the scale.
	 * GetContentScaleFactor() is 1.0 where that is the truth, so this is the
	 * same arithmetic everywhere.
	 */
	const double scale = GetContentScaleFactor();
	const int pixel_w = (int) (size.GetWidth() * scale + 0.5);
	const int pixel_h = (int) (size.GetHeight() * scale + 0.5);

	glViewport(0, 0, pixel_w, pixel_h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, size.GetWidth(), size.GetHeight(), 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	/* Black, so the bars either side of a letterboxed picture match what the
	   CPU path draws. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (!have_frame_ || display_rect_.GetWidth() <= 0 || display_rect_.GetHeight() <= 0) {
		return;
	}

	const double x0 = display_rect_.GetX();
	const double y0 = display_rect_.GetY();
	const double x1 = x0 + display_rect_.GetWidth();
	const double y1 = y0 + display_rect_.GetHeight();

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture_);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f); glVertex2d(x0, y0);
	glTexCoord2f(1.0f, 0.0f); glVertex2d(x1, y0);
	glTexCoord2f(1.0f, 1.0f); glVertex2d(x1, y1);
	glTexCoord2f(0.0f, 1.0f); glVertex2d(x0, y1);
	glEnd();

	glDisable(GL_TEXTURE_2D);
}

#endif /* wxUSE_GLCANVAS */
