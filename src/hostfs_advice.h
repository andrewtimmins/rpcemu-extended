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

#ifndef HOSTFS_ADVICE_H
#define HOSTFS_ADVICE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the machine editor should say about a chosen HostFS folder.
 *
 * Extracted from the dialog so the decision can be tested, because the decision
 * is where it went wrong. The editor had the "there is no !Boot here" warning in
 * the branch for a folder that already exists, and a separate branch for one
 * that does not - which said only that it would be created. A folder that is
 * about to be created is guaranteed to be empty, so that is precisely the case
 * where the warning matters most, and precisely the case that did not get it.
 *
 * Reported by David Ramsden, who pointed a new machine's HostFS at C:\foo,
 * accepted the restart, and got a machine that would not reach its desktop with
 * nothing on screen to say why. Nothing was broken and nothing was lost: an
 * empty folder is an empty hard disc, and a RISC OS with no !Boot does not
 * announce the fact. See docs/hostfs.md.
 */

/** The folder is not there yet and will be created. */
#define HOSTFS_ADVICE_WILL_CREATE	0x1u

/** There is no !Boot, so a machine that boots from HostFS will not get far. */
#define HOSTFS_ADVICE_NO_BOOT		0x2u

/**
 * Decide what to warn about.
 *
 * @param folder_exists Does the resolved folder exist?
 * @param has_boot      Does it contain a !Boot? Meaningless, and ignored, when
 *                      the folder does not exist: it cannot contain anything.
 * @return A combination of HOSTFS_ADVICE_*, zero when there is nothing to say.
 */
unsigned hostfs_advice(int folder_exists, int has_boot);

/**
 * The wording for one HOSTFS_ADVICE_* bit.
 *
 * One bit at a time, so a caller decides the order and the punctuation between
 * them. Returns NULL for zero, for a combination, or for an unknown bit rather
 * than inventing something to display.
 *
 * The no-!Boot wording names BOTH symptoms on purpose. RISC OS 5.31 comes up to
 * a bare grey screen with no icon bar; 5.30 stops at the supervisor prompt. An
 * earlier version of this warning promised the supervisor prompt, so on 5.31 -
 * the version people are downloading from ROOL - it described something the user
 * would never see.
 */
const char *hostfs_advice_text(unsigned bit);

#ifdef __cplusplus
}
#endif

#endif /* HOSTFS_ADVICE_H */
