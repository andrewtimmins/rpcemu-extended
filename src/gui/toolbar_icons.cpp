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

#include "toolbar_icons.h"

#include <wx/bmpbndl.h>

// SVG icon pack, embedded at build time from src/gui/icons/*.svg. Each entry is
// a NUL-terminated byte array named <basename>_svg (e.g. floppy_svg). See the
// generator in src/gui/CMakeLists.txt.
#include "toolbar_icons_svg.h"

namespace {

// Render an embedded SVG to a bitmap at the requested toolbar size. The SVG is
// authored at a 24x24 reference size; wxBitmapBundle rasterises it cleanly to
// whatever size the toolbar asks for. Used on all platforms for a consistent
// look (wxMSW has no matching wxArtProvider bitmaps; wxGTK theme icons varied).
wxBitmap SvgIcon(const unsigned char *svg, const wxSize &size)
{
	wxBitmapBundle bundle =
	    wxBitmapBundle::FromSVG(reinterpret_cast<const char *>(svg),
	                            wxSize(24, 24));
	return bundle.GetBitmap(size);
}

} // namespace

wxBitmap ToolbarIconScreenshot(const wxSize &size)
{
	return SvgIcon(screenshot_svg, size);
}

wxBitmap ToolbarIconFloppy(const wxSize &size)
{
	return SvgIcon(floppy_svg, size);
}

wxBitmap ToolbarIconCdrom(const wxSize &size)
{
	return SvgIcon(cdrom_svg, size);
}

wxBitmap ToolbarIconReset(const wxSize &size)
{
	return SvgIcon(reset_svg, size);
}

wxBitmap ToolbarIconMute(bool muted, const wxSize &size)
{
	return SvgIcon(muted ? mute_svg : unmute_svg, size);
}

wxBitmap ToolbarIconFullscreen(const wxSize &size)
{
	return SvgIcon(fullscreen_svg, size);
}

wxBitmap ToolbarIconConfigure(const wxSize &size)
{
	return SvgIcon(settings_svg, size);
}

wxBitmap ToolbarIconDebugRun(const wxSize &size)
{
	return SvgIcon(start_svg, size);
}

wxBitmap ToolbarIconDebugPause(const wxSize &size)
{
	return SvgIcon(pause_svg, size);
}

wxBitmap ToolbarIconDebugStep(const wxSize &size)
{
	return SvgIcon(step_svg, size);
}

wxBitmap ToolbarIconInspector(const wxSize &size)
{
	return SvgIcon(inspector_svg, size);
}

wxBitmap ToolbarIconAnalyser(const wxSize &size)
{
	return SvgIcon(analyser_svg, size);
}

wxBitmap ToolbarIconPower(const wxSize &size, bool on)
{
	return SvgIcon(on ? power_svg : stop_svg, size);
}

namespace {

/* The reference size the whole pack is authored at; the bundle scales from it. */
wxBitmapBundle SvgBundle(const unsigned char *svg)
{
	return wxBitmapBundle::FromSVG(reinterpret_cast<const char *>(svg),
	                               wxSize(24, 24));
}

} // namespace

wxBitmapBundle StatusIconRunning()
{
	return SvgBundle(running_svg);
}

wxBitmapBundle StatusIconStarting()
{
	return SvgBundle(starting_svg);
}

wxBitmapBundle StatusIconStopped()
{
	return SvgBundle(stopped_svg);
}
