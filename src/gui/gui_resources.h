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

#ifndef GUI_RESOURCES_H
#define GUI_RESOURCES_H

#include <wx/string.h>

class wxFrame;

/* The application logo, <resourcedir>/resources/rpcemu.png. Not every caller
   wants it the same way - one rescales it, another falls back to a drawn one -
   so this is the path rather than the picture. */
wxString AppLogoPath();

/* The window, taskbar and Alt-Tab icon. Does nothing where the logo cannot be
   read, leaving the platform's default rather than a blank space. */
void SetFrameIcon(wxFrame *frame);

#endif /* GUI_RESOURCES_H */
