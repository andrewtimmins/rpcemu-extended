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

#ifndef FOLDER_MOVE_H
#define FOLDER_MOVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Should we offer to move somebody's files for them, and if not, why not?
 *
 * Choosing a new HostFS folder, or a new data folder, used to leave the files
 * where they were: the new folder was created empty and the machine booted from a
 * blank disc. The editor warns about that now, but a warning is a poor substitute
 * for doing the thing the user obviously wanted, so this decides whether the
 * offer can be made safely.
 *
 * ★ THE DECISION IS SEPARATED FROM THE DOING ON PURPOSE. Copying a hard disc is
 * the most destructive thing this program does on the host's filesystem, and the
 * conditions that make it unsafe - a destination with files already in it, not
 * enough room, a machine still running from the folder - are exactly the things
 * that are awkward to arrange in a test. So the rules live here, pure, with the
 * filesystem answers passed in, and tests/test_folder_move.c can put any
 * combination to them.
 *
 * The transfer itself is src/gui/folder_transfer.cpp, which asks this first.
 */

/** What the caller may offer. */
typedef enum {
	/** Both a move and a copy are safe to offer. */
	FOLDER_MOVE_OFFER_BOTH,
	/**
	 * A copy is safe but a move is not, because the source cannot be removed
	 * afterwards. Offered rather than refused: getting the files there is the
	 * point, and tidying up afterwards is the user's business.
	 */
	FOLDER_MOVE_OFFER_COPY_ONLY,
	/** Nothing can be offered. There is a reason, below. */
	FOLDER_MOVE_OFFER_NOTHING
} folder_move_offer;

/** Why nothing can be offered, or why only a copy can be. */
typedef enum {
	FOLDER_MOVE_OK,
	/** The two paths resolve to the same place, so there is nothing to do. */
	FOLDER_MOVE_SAME_PLACE,
	/** Nothing in the old folder to bring across. */
	FOLDER_MOVE_SOURCE_EMPTY,
	/** The old folder is not there at all. */
	FOLDER_MOVE_SOURCE_MISSING,
	/**
	 * The new folder already has files in it. Never merged: two hard discs
	 * interleaved is not something anybody asked for, and picking which copy of
	 * a clashing file wins is not a decision this program should make silently.
	 */
	FOLDER_MOVE_DEST_NOT_EMPTY,
	/** The new folder is a file, or otherwise cannot hold anything. */
	FOLDER_MOVE_DEST_NOT_A_DIR,
	/** Not enough free space at the destination for what is being copied. */
	FOLDER_MOVE_NO_SPACE,
	/**
	 * A machine is running from one of these folders. Refused outright: HostFS
	 * opens files as the guest asks for them, so moving them underneath a live
	 * machine would corrupt whatever it touches next.
	 */
	FOLDER_MOVE_IN_USE,
	/** The destination cannot be written to. */
	FOLDER_MOVE_DEST_READ_ONLY,
	/** The source cannot be removed, so only a copy can be offered. */
	FOLDER_MOVE_SOURCE_READ_ONLY
} folder_move_reason;

/** What the filesystem says about the two folders. Gathered by the caller. */
typedef struct {
	int same_place;		/**< The two paths are the same directory. */
	int source_exists;
	int source_is_dir;
	int source_writable;	/**< Can its contents be removed afterwards? */
	int source_empty;
	int dest_exists;
	int dest_is_dir;	/**< Meaningless when dest_exists is zero. */
	int dest_empty;
	int dest_parent_writable; /**< Can the destination be created or written? */
	int in_use;		/**< A machine is running from either folder. */

	/**
	 * Bytes to bring across, and bytes free where they are going.
	 *
	 * Both zero means "not known" and the space check is skipped: a caller that
	 * cannot measure should not be forced to guess, and refusing on an unknown
	 * would stop the feature working on any filesystem we cannot size.
	 */
	unsigned long long bytes_needed;
	unsigned long long bytes_free;
} folder_move_facts;

/**
 * Decide what to offer.
 *
 * @param facts What the filesystem says.
 * @param why   Filled in with the reason. FOLDER_MOVE_OK when both are offered.
 *              May be NULL.
 * @return What may be offered.
 */
folder_move_offer folder_move_decide(const folder_move_facts *facts,
                                     folder_move_reason *why);

/**
 * A sentence explaining a reason, for putting in front of the user.
 *
 * Never NULL, so a caller cannot accidentally show nothing. Phrased as the reason
 * the offer is not being made, since that is the only time it is shown.
 */
const char *folder_move_reason_text(folder_move_reason why);

/**
 * Is there enough room, allowing for the margin?
 *
 * Exposed for the tests, and because the margin is a judgement rather than a
 * fact: filling a disc completely is its own kind of failure, so a copy that
 * would leave nothing spare is refused before it starts rather than half way
 * through.
 */
int folder_move_space_is_enough(unsigned long long bytes_needed,
                                unsigned long long bytes_free);

#ifdef __cplusplus
}
#endif

#endif /* FOLDER_MOVE_H */
