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

#ifndef TOOLBAR_ICONS_H
#define TOOLBAR_ICONS_H

#include <wx/bitmap.h>
#include <wx/bmpbndl.h>
#include <wx/gdicmn.h>

wxBitmap ToolbarIconScreenshot(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconFloppy(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconCdrom(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconReset(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconMute(bool muted, const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconFullscreen(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconConfigure(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconDebugRun(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconDebugPause(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconDebugStep(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconInspector(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconAnalyser(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconResume(const wxSize &size = wxSize(24, 24));
wxBitmap ToolbarIconSuspend(const wxSize &size = wxSize(24, 24));

/* Starting and stopping a machine, which the Manager does and a running
   machine cannot. Deliberately not the debugger's run/pause icons: those now
   appear on the same toolbar meaning something quite different, and the same
   picture twice over for two different actions is worse than no picture. */
wxBitmap ToolbarIconPower(const wxSize &size = wxSize(24, 24), bool on = true);

/*
 * Whether a machine is running, in the Manager's list.
 *
 * Bundles rather than bitmaps, because the list wants them that way: handed a
 * wxImageList, wxGTK draws a 16x16 circle as a clipped square whatever size the
 * list is told and whether or not it has a mask, while wxListCtrl::SetSmallImages
 * with a bundle draws the circle and picks its own size for the display it is on.
 *
 * From the same SVG pack as the toolbar, so they carry an alpha channel. Drawn
 * with a DC onto a filled bitmap they brought their background with them, which
 * showed as a pale square on a dark theme and against the selection highlight.
 */
wxBitmapBundle StatusIconRunning();
wxBitmapBundle StatusIconStarting();
wxBitmapBundle StatusIconStopped();

#endif
