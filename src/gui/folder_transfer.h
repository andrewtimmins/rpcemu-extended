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

#ifndef FOLDER_TRANSFER_H
#define FOLDER_TRANSFER_H

#include <wx/string.h>

class wxWindow;

extern "C" {
#include "folder_move.h"
}

/*
 * Moving somebody's files when they point a folder somewhere new.
 *
 * Pointing HostFS, or the data folder, at a new place used to leave the files
 * behind: the new folder was created empty and the machine booted from a blank
 * disc. This offers to bring them across.
 *
 * ★ THE RULE THE WHOLE THING RESTS ON: the configuration is only changed after a
 * transfer has completely succeeded. If anything fails part way, both copies are
 * still on disc and the machine still boots from the old folder. A failed move is
 * then an inconvenience rather than a lost hard disc - which matters more than any
 * warning text, because people click through warnings.
 *
 * Two more properties worth stating, because they are what make the destructive
 * parts safe:
 *
 *   - Nothing is deleted from the old folder until every file has been copied AND
 *     its content verified (MD5 both sides, not just a size). A move is a verified
 *     copy followed by a delete, never an unchecked one.
 *   - A partial transfer is cleaned up, and that is safe to do because the
 *     destination was checked to be empty (or absent) before anything started, so
 *     everything in it is ours. folder_move_decide() is what guarantees that, and
 *     it refuses rather than merging into a folder with files in it.
 *
 * The decision about whether to offer at all is folder_move.c, kept separate and
 * unit tested. This is the doing.
 */

/** What the user chose. */
enum class FolderTransferChoice {
	Move,		/**< Bring the files across and empty the old folder. */
	Copy,		/**< Bring the files across and leave the old folder alone. */
	Manual,		/**< Change the setting; the user will move files themselves. */
	Cancel		/**< Change nothing at all. */
};

/** How a transfer went. */
struct FolderTransferResult {
	bool ok = false;		/**< Are the files at the destination? */
	bool source_left_behind = false; /**< A move that could not empty the source. */
	unsigned long files = 0;	/**< Files brought across. */
	wxString message;		/**< For showing; never empty. */
};

/**
 * Ask the filesystem the questions folder_move_decide() needs.
 *
 * @param from   The folder in use now.
 * @param to     The folder being chosen.
 * @param in_use Is a machine running from either? The caller knows; this cannot.
 * @param bytes  Filled with the size of @from, for the space check and the
 *               dialogue. May be NULL.
 */
folder_move_facts FolderTransferGatherFacts(const wxString &from,
                                            const wxString &to, bool in_use,
                                            unsigned long long *bytes);

/**
 * Put the choice to the user.
 *
 * @param what  What is being moved, in the user's terms: "hard disc",
 *              "data folder". Used in the wording.
 * @param offer What folder_move_decide() allows.
 * @param why   Its reason, shown when a move cannot be offered.
 * @return The choice. FolderTransferChoice::Manual when nothing can be offered
 *         and the user accepted that, Cancel when they backed out.
 */
FolderTransferChoice FolderTransferAsk(wxWindow *parent, const wxString &what,
                                       const wxString &from, const wxString &to,
                                       folder_move_offer offer,
                                       folder_move_reason why,
                                       unsigned long long bytes);

/**
 * Bring the files across, with a progress dialogue.
 *
 * A move on one filesystem is a rename, which is instant whatever the size. Across
 * filesystems, and for every copy, each file is copied and then verified before
 * anything is removed.
 *
 * @return Whether the files are now at the destination. On failure the source is
 *         untouched and any partial destination has been cleaned up, so the caller
 *         must leave the configuration alone.
 */
FolderTransferResult FolderTransferRun(wxWindow *parent, const wxString &from,
                                       const wxString &to,
                                       FolderTransferChoice choice);

#endif /* FOLDER_TRANSFER_H */
