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
 * hostfs_advice.c - what to say about a chosen HostFS folder.
 *
 * The reasoning is in hostfs_advice.h.
 */

#include "hostfs_advice.h"

unsigned
hostfs_advice(int folder_exists, int has_boot)
{
	unsigned flags = 0;

	if (!folder_exists) {
		flags |= HOSTFS_ADVICE_WILL_CREATE;
		/* A folder that does not exist yet cannot hold a !Boot, whatever the
		   caller passed. Deciding it here rather than trusting the caller is the
		   whole point: the dialog only tested for !Boot in the branch where the
		   folder already existed, so the case that always needs the warning was
		   the one case that never got it. */
		flags |= HOSTFS_ADVICE_NO_BOOT;
		return flags;
	}

	if (!has_boot) {
		flags |= HOSTFS_ADVICE_NO_BOOT;
	}
	return flags;
}

const char *
hostfs_advice_text(unsigned bit)
{
	switch (bit) {
	case HOSTFS_ADVICE_WILL_CREATE:
		return "This folder does not exist yet. It will be created, empty.";

	case HOSTFS_ADVICE_NO_BOOT:
		/* Both symptoms, because they look nothing like each other and neither
		   mentions a folder. See the header. */
		return "There is no !Boot here, so this is an empty hard disc. If this "
		       "machine boots from HostFS it will not reach its desktop: RISC OS "
		       "5.31 comes up to a bare grey screen with no icon bar, and 5.30 "
		       "stops at the supervisor prompt. Nothing is lost - point this "
		       "back at the old folder to get the machine back.";

	default:
		/* Zero, a combination, or a bit that does not exist. Nothing invented. */
		return NULL;
	}
}
