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
 * http_reachable - does a connection to this URL get refused outright?
 *
 * ★ THIS EXISTS TO AVOID A CRASH IN wxWidgets, NOT TO TEST THE NETWORK.
 *
 * wxWidgets 3.2 under GTK dereferences freed memory when a connection is
 * refused. GLib reports G_IO_IN and G_IO_ERR together on the refused socket;
 * wx_on_channel_event() calls OnReadWaiting(), curl learns the connect failed
 * and tears the transfer down - which deletes the handler - and then, because
 * G_IO_ERR is still set, the same function calls OnExceptionWaiting() through
 * the object it has just freed. Every HTTP request RPCEmu makes to a refused
 * port takes the process with it.
 *
 * Fixed upstream by wxWidgets PR 27041, which is on master and not in any
 * release. The patch is inside the library, so there is nothing to carry here:
 * we link whatever wxWidgets the distribution ships, and every 3.2.x has this.
 *
 * So the only move left is not to hand wxWebRequest a connection that will be
 * refused. This connects first, and reports only that one condition. It is a
 * mitigation and not a fix: a server that stops listening between this call and
 * the real request still crashes, and nothing here can close that gap.
 *
 * Only ECONNREFUSED counts. Everything else - a timeout, a name that does not
 * resolve, no route - is a failure wxWebRequest survives and describes far
 * better than this could, so those are passed through untouched rather than
 * turned into a worse error message.
 */

#ifndef HTTP_REACHABLE_H
#define HTTP_REACHABLE_H

#include <wx/string.h>

/**
 * True if connecting to this URL's host and port is refused outright.
 *
 * False for every other outcome, including success, a timeout, a name that
 * does not resolve, and anything this cannot work out - the caller should go
 * ahead and make the real request, which reports failure properly.
 *
 * Costs a TCP connect to the host, so it is only worth calling where the
 * alternative is the crash described above.
 */
bool HttpConnectionRefused(const wxString &url);

#endif /* HTTP_REACHABLE_H */
