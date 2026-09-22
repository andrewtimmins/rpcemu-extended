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
 * reset_question.h - "this will reset the machine, okay?"
 *
 * A machine's own window (main_frame.cpp) asks this before Reset, and before
 * anything else that resets as a side effect (CD-ROM enable/disable, the CPU
 * Idle toggle). Shared so the Manager (manager_frame.cpp) asks the same
 * question the same way before its own Reset button, rather than each window
 * carrying its own copy to drift out of step.
 *
 * Takes the machine's name because the Manager can have several machines
 * running at once - a message that just said "this will reset RPCEmu
 * Extended" would not say which one.
 */

#ifndef RESET_QUESTION_H
#define RESET_QUESTION_H

#include <wx/string.h>

class wxWindow;

bool HostResetQuestion(wxWindow *parent, const wxString &machine_name);

#endif /* RESET_QUESTION_H */
