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

#ifndef MACHINE_SHORTCUT_H
#define MACHINE_SHORTCUT_H

#include <wx/string.h>

/*
 * A shortcut that opens one machine, in whatever form the desktop understands:
 * a .lnk on Windows, a .desktop on Linux, a small application bundle on macOS.
 * wxWidgets has nothing for this, so there is a writer per platform and they
 * share nothing but this declaration.
 *
 * Each carries the emulator's icon, so the shortcut looks like the thing it
 * starts rather than like a script.
 *
 * Answers false if it could not be written, which the caller reports.
 */
bool WriteShortcut(const wxString &path, const wxString &exe,
                   const wxString &args, const wxString &working_dir,
                   const wxString &description);

#endif /* MACHINE_SHORTCUT_H */
