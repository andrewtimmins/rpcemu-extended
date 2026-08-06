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
 * folder_move.c - whether moving somebody's files can be offered.
 *
 * The reasoning is in folder_move.h.
 */

#include "folder_move.h"

#include <stddef.h>

/*
 * Spare room to leave behind, as a fraction of what is being copied plus a
 * fixed floor.
 *
 * A copy that exactly fits leaves the destination completely full, which breaks
 * the next thing to write there - quite possibly the guest, a moment later,
 * having just been pointed at it. The floor matters for small discs, where a
 * percentage of a few megabytes is no margin at all.
 */
#define SPACE_MARGIN_DIVISOR	20		/* 5% */
#define SPACE_MARGIN_FLOOR	(16u * 1024u * 1024u)	/* 16 MB */

int
folder_move_space_is_enough(unsigned long long bytes_needed,
                            unsigned long long bytes_free)
{
	unsigned long long margin;
	unsigned long long required;

	/* Nothing known: the caller could not measure, so this is not the place to
	   refuse. Said in the header so nobody reads it as an oversight. */
	if (bytes_needed == 0 && bytes_free == 0) {
		return 1;
	}

	margin = bytes_needed / SPACE_MARGIN_DIVISOR;
	if (margin < SPACE_MARGIN_FLOOR) {
		margin = SPACE_MARGIN_FLOOR;
	}

	required = bytes_needed + margin;
	/* Overflow would wrap and turn "far too big" into "plenty of room". */
	if (required < bytes_needed) {
		return 0;
	}
	return bytes_free >= required;
}

folder_move_offer
folder_move_decide(const folder_move_facts *facts, folder_move_reason *why)
{
	folder_move_reason reason = FOLDER_MOVE_OK;
	folder_move_offer offer = FOLDER_MOVE_OFFER_NOTHING;

	if (facts == NULL) {
		if (why != NULL) {
			*why = FOLDER_MOVE_SOURCE_MISSING;
		}
		return FOLDER_MOVE_OFFER_NOTHING;
	}

	/*
	 * Order matters, because the first thing that is true is what the user gets
	 * told. In use comes first: it is the only one that could destroy a running
	 * machine's files, and it is worth saying even when something else is also
	 * wrong.
	 */
	if (facts->in_use) {
		reason = FOLDER_MOVE_IN_USE;
	} else if (facts->same_place) {
		reason = FOLDER_MOVE_SAME_PLACE;
	} else if (!facts->source_exists || !facts->source_is_dir) {
		reason = FOLDER_MOVE_SOURCE_MISSING;
	} else if (facts->source_empty) {
		/* Nothing to bring across. Not a failure - this is the ordinary case of
		   somebody pointing a brand new machine somewhere. */
		reason = FOLDER_MOVE_SOURCE_EMPTY;
	} else if (facts->dest_exists && !facts->dest_is_dir) {
		reason = FOLDER_MOVE_DEST_NOT_A_DIR;
	} else if (facts->dest_exists && !facts->dest_empty) {
		reason = FOLDER_MOVE_DEST_NOT_EMPTY;
	} else if (!facts->dest_parent_writable) {
		reason = FOLDER_MOVE_DEST_READ_ONLY;
	} else if (!folder_move_space_is_enough(facts->bytes_needed,
	                                       facts->bytes_free)) {
		reason = FOLDER_MOVE_NO_SPACE;
	} else if (!facts->source_writable) {
		/*
		 * The files can be copied but the originals cannot be removed. Offering
		 * the copy is still worth it: the machine ends up working, which is what
		 * was wanted, and the leftovers are visible rather than lost.
		 */
		reason = FOLDER_MOVE_SOURCE_READ_ONLY;
		offer = FOLDER_MOVE_OFFER_COPY_ONLY;
	} else {
		offer = FOLDER_MOVE_OFFER_BOTH;
	}

	if (why != NULL) {
		*why = reason;
	}
	return offer;
}

const char *
folder_move_reason_text(folder_move_reason why)
{
	switch (why) {
	case FOLDER_MOVE_OK:
		return "The files can be moved or copied.";

	case FOLDER_MOVE_SAME_PLACE:
		return "That is the folder already in use, so there is nothing to move.";

	case FOLDER_MOVE_SOURCE_EMPTY:
		return "The current folder is empty, so there is nothing to move.";

	case FOLDER_MOVE_SOURCE_MISSING:
		return "The current folder is not there, so there is nothing to move.";

	case FOLDER_MOVE_DEST_NOT_EMPTY:
		return "The new folder already has files in it. RPCEmu will not mix two "
		       "sets of files together, because deciding which copy of a clashing "
		       "file wins is not a choice it should make for you. Either empty "
		       "the new folder or move the files across yourself.";

	case FOLDER_MOVE_DEST_NOT_A_DIR:
		return "The new location is a file, not a folder.";

	case FOLDER_MOVE_NO_SPACE:
		return "There is not enough free space where the files are going, once a "
		       "small margin is allowed for. A copy that exactly fills the disc "
		       "leaves nothing for the machine to write afterwards.";

	case FOLDER_MOVE_IN_USE:
		return "A machine is still running from one of these folders. Close it "
		       "first: RPCEmu opens files as the guest asks for them, so moving "
		       "them now would damage whatever it reads next.";

	case FOLDER_MOVE_DEST_READ_ONLY:
		return "The new folder cannot be written to.";

	case FOLDER_MOVE_SOURCE_READ_ONLY:
		return "The files can be copied, but the originals cannot be removed "
		       "afterwards, so they will be left where they are.";
	}

	/* Not reachable for a value from the enumeration, and deliberately not a
	   NULL: a caller showing this to somebody should never end up showing
	   nothing at all. */
	return "The files cannot be moved automatically.";
}
