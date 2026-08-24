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

/*
 * window_owner - keeping a machine's windows in front of the Manager's.
 *
 * A machine started from the Manager runs in its own process, and that process's
 * own window is never shown. So the Machine Inspector, the Package Manager and
 * the rest are parented to an invisible frame in a different process from the
 * window the user is looking at: the window manager has nothing to stack them
 * against and no reason to put them above an unrelated application, so they open
 * behind the Manager and nothing says they opened at all.
 *
 * The Manager therefore tells each machine its own native window id, and the
 * machine names it as the owner of the windows it opens. That is a real
 * relationship the window manager honours - the window is kept above its owner,
 * moves with it and minimises with it - and it is not the same thing as "always
 * on top", which would also sit above the user's browser and be resented within
 * a minute.
 *
 * ★ It is only possible on some platforms, and this says so rather than
 * pretending:
 *
 *   X11      XSetTransientForHint is a property any process may set on any
 *            window, so this works across processes. Verifiable with xprop.
 *   Windows  GWLP_HWNDPARENT takes another process's window as the owner.
 *   Wayland  No cross-process parent/child exists. Nothing to do.
 *   macOS    Nor here: NSWindow's parent/child is within one application.
 *
 * Where it cannot be done, window_show_in_front() is the floor, and it is worth
 * having everywhere: Show(), Raise(), and then RequestUserAttention() for the
 * window managers that decline to raise a window on another application's behalf
 * but will mark its taskbar entry.
 */

#ifndef WINDOW_OWNER_H
#define WINDOW_OWNER_H

#include <cstdint>

class wxWindow;

/*
 * This window's native id, for handing to another process, or 0 where there is
 * no such thing to hand over - which includes Wayland and macOS, and any window
 * that has not been realised yet.
 */
uint64_t window_native_id(const wxWindow *window);

/*
 * Name `owner_id` as the owner of this window. Does nothing if the id is 0 or
 * the platform has no cross-process equivalent. Safe to call before showing: the
 * X11 side realises the widget itself, because a GTK widget has no X window to
 * carry the property until it is realised, and Windows wants the owner set while
 * the window is still unmapped so it is placed correctly first time.
 */
void window_set_owner(wxWindow *window, uint64_t owner_id);

/*
 * Show a window and do what can be done to put it in front: for a window whose
 * parent is invisible, Raise() alone is frequently ignored.
 */
void window_show_in_front(wxWindow *window);

/*
 * Bring THIS process to the front, for the moment it is about to show a window
 * the user has just asked for.
 *
 * macOS raises windows a whole application at a time, and a managed machine is
 * never the active one: the click that asked for the window went to the
 * Manager's menu or toolbar, so the Manager is active and the machine is not.
 * Raise() then reorders the window within an application that is itself behind,
 * which puts it exactly nowhere the user can see. Every window a managed machine
 * opens came up behind the Manager, with nothing on screen to say anything had
 * happened at all.
 *
 * Called by the process that owns the window, not by the Manager, and only when
 * a window is actually being shown. The Manager could activate the machine by
 * pid instead, but it forwards every menu command through one path and cannot
 * tell which of them open a window - so it would steal focus for a checkbox.
 *
 * Nothing to do anywhere else: X11 and Windows raise per window, and both are
 * handled by window_set_owner() and window_allow_foreground().
 */
void window_activate_self(void);

/*
 * Let another process raise a window of its own, once.
 *
 * ★ Windows restricts this and the restriction is the whole difficulty: a
 * process may bring a window to the front only if it is already the foreground
 * process or received the last input event. A machine started by the Manager is
 * neither - the click that asks for its window went to the Manager's menu - so
 * its Raise(), which is SetForegroundWindow() for a top-level window, is refused
 * and Windows flashes the taskbar instead.
 *
 * Called by the process that DOES have the right, at the moment it is passing on
 * a request to open a window. The grant is for one use and lapses at the next
 * input not directed at that process, so it cannot become a standing licence to
 * interrupt.
 *
 * Nothing to do anywhere else: X11 has no such restriction, and on Wayland and
 * macOS a machine cannot raise its window across processes whatever we do.
 */
void window_allow_foreground(long pid);

#endif /* WINDOW_OWNER_H */
