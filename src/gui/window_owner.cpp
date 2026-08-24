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

#include "window_owner.h"

#include <wx/wx.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#endif
#endif

#ifdef __WXMSW__
#include <windows.h>
#endif

uint64_t
window_native_id(const wxWindow *window)
{
	if (window == nullptr) {
		return 0;
	}

#if defined(__WXMSW__)
	return (uint64_t) (uintptr_t) window->GetHandle();
#elif defined(__WXGTK__) && defined(GDK_WINDOWING_X11)
	GtkWidget *const widget = GTK_WIDGET(window->GetHandle());

	if (widget == nullptr) {
		return 0;
	}

	GdkWindow *const gdk = gtk_widget_get_window(widget);

	/* Not realised yet, or a Wayland session, where there is no X window and
	   nothing another process could name. */
	if (gdk == nullptr || !GDK_IS_X11_WINDOW(gdk)) {
		return 0;
	}
	return (uint64_t) GDK_WINDOW_XID(gdk);
#else
	(void) window;
	return 0;
#endif
}

void
window_set_owner(wxWindow *window, uint64_t owner_id)
{
	if (window == nullptr || owner_id == 0) {
		return;
	}

#if defined(__WXMSW__)
	/*
	 * The owner, not the parent: an owned window stays above its owner and
	 * minimises with it, while remaining a top-level window of its own. Windows
	 * accepts an owner belonging to another process, which is the whole point
	 * here.
	 */
	SetWindowLongPtr((HWND) window->GetHandle(), GWLP_HWNDPARENT,
	    (LONG_PTR) (uintptr_t) owner_id);
#elif defined(__WXGTK__) && defined(GDK_WINDOWING_X11)
	GtkWidget *const widget = GTK_WIDGET(window->GetHandle());

	if (widget == nullptr) {
		return;
	}

	/*
	 * Realise it first. A GTK widget has no X window until it is realised, which
	 * normally waits for it to be shown - so setting the hint "before showing",
	 * which is what Windows wants, found nothing here to set it on and did
	 * nothing at all. Realising is not showing: the window is created and can
	 * carry properties while still unmapped.
	 */
	if (!gtk_widget_get_realized(widget)) {
		gtk_widget_realize(widget);
	}

	GdkWindow *const gdk = gtk_widget_get_window(widget);

	if (gdk == nullptr || !GDK_IS_X11_WINDOW(gdk)) {
		return;
	}

	/*
	 * WM_TRANSIENT_FOR names a window this one is transient for, and the
	 * specification does not require the two to belong to the same client - so
	 * this is set on the Manager's window in another process, and the window
	 * manager keeps ours above it. Set through Xlib rather than
	 * gtk_window_set_transient_for(), which only takes a GtkWindow and so cannot
	 * express a window this process does not own.
	 */
	XSetTransientForHint(GDK_WINDOW_XDISPLAY(gdk), GDK_WINDOW_XID(gdk),
	    (Window) owner_id);
#else
	(void) window;
	(void) owner_id;
#endif
}

void
window_show_in_front(wxWindow *window)
{
	if (window == nullptr) {
		return;
	}

	if (!window->IsShown()) {
		window->Show();
	}
	window->Raise();

	/*
	 * For the window managers that will not raise one application's window
	 * because another asked them to. This marks the taskbar entry instead, which
	 * is the difference between a window that opened somewhere and a window the
	 * user cannot tell opened at all.
	 */
	if (auto *top = wxDynamicCast(window, wxTopLevelWindow)) {
		top->RequestUserAttention();
	}
}

void
window_allow_foreground(long pid)
{
#if defined(__WXMSW__)
	if (pid > 0) {
		AllowSetForegroundWindow((DWORD) pid);
	}
#else
	(void) pid;
#endif
}
