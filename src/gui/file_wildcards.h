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
 * file_wildcards.h - file dialogue filters that ignore case
 *
 * GTK matches a file dialogue's filter patterns case-sensitively, so a
 * FLOPPY.IMG or a CD.ISO is hidden by a *.img or *.iso filter and looks as
 * though it is not there. The Windows and macOS dialogues match without regard
 * to case already, and neither understands the character classes this produces,
 * so the rewrite is GTK-only and this is the identity elsewhere.
 *
 * Takes and returns a wx wildcard string ("Label (*.ext)|*.ext|..."), rewriting
 * only the pattern halves. The labels are left as they are, so what the user
 * reads stays "DOS Disc Image (*.img)".
 *
 * Image formats are decided by content or by strcasecmp (see fdc.c), so the
 * filter is the only thing that ever cared about case. Any dialogue whose files
 * come from elsewhere - a CD pressed on another system, an archive, a Windows
 * share - wants this.
 */

#ifndef FILE_WILDCARDS_H
#define FILE_WILDCARDS_H

#include <wx/string.h>

wxString HostCaseInsensitiveWildcard(const wxString &wildcard);

#endif /* FILE_WILDCARDS_H */
