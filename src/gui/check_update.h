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

#ifndef CHECK_UPDATE_H
#define CHECK_UPDATE_H

#include <wx/window.h>

/*
 * Ask GitHub whether there is a newer release, and say so.
 *
 * Shared rather than a method of either window, for the reason AboutDialog is:
 * it is about RPCEmu and not about any machine, so the Manager answers it
 * itself - a managed machine has no window for these dialogues to appear over.
 *
 * `parent` owns the dialogues and the progress window.
 */
void CheckForUpdate(wxWindow *parent);

/* Silent unless there is a newer release: somebody who did not ask is not owed
   a dialogue about a check that failed. At most once a day. */
void CheckForUpdateInBackground(wxWindow *parent);

#endif /* CHECK_UPDATE_H */
