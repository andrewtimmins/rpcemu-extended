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
 * package_install.h - installing and removing RISC OS packages.
 *
 * A package is a zip whose members are laid out as the paths they install to,
 * plus a RiscPkg directory of metadata. Installing one means unpacking it onto
 * the machine's disc and recording what was unpacked; removing one means reading
 * that record back and deleting it again.
 *
 * The record is the RISC OS Packaging Project's own database, in a !Packages
 * directory on the machine's disc:
 *
 *   Status                    one tab-separated line per package
 *   Info/<Package>/Control    the package's control record
 *   Info/<Package>/Copyright  its copyright and licence statement
 *   Info/<Package>/Files      every path it installed, one per line
 *   Version                   the database format version
 *
 * Keeping that format matters: it means anything else built for RISC OS
 * packages, PackMan and RiscPkg included, sees what we install as ordinary
 * installed packages rather than files that appeared from nowhere. Installing
 * without recording is what makes a later upgrade go wrong.
 */

#ifndef PACKAGE_INSTALL_H
#define PACKAGE_INSTALL_H

#include <map>
#include <vector>

#include <wx/string.h>

#include "package_index.h"

/**
 * Where a package's triggers live once installed, and how to run them.
 *
 * A package may carry runnable files in its RiscPkg.Triggers directory, to be run
 * at four points in a commit: PreInstall, PostInstall, PreRemove and PostRemove.
 * The policy manual is clear that they should be rare, and most packages have
 * none.
 *
 * Running them needs the guest, since they are RISC OS code, so the caller
 * supplies this. Given a command line it should run it in the machine and report
 * whether RISC OS said it succeeded; see guest_command.h. Returning false for
 * "could not ask" is different from a trigger that ran and failed, and the
 * distinction is kept.
 */
struct PackageTriggerRunner {
	/** Run @command in the guest. @output gets whatever it printed. */
	virtual bool Run(const wxString &command, wxString &output) = 0;

	virtual ~PackageTriggerRunner() = default;
};

/** What a machine has installed: package name to version. */
using PackageInstalledMap = std::map<wxString, wxString>;

/**
 * Read a machine's package database.
 *
 * @param hostfs_dir The machine's HostFS directory (its disc).
 * @return What is installed. Empty if the machine has no database yet, which is
 *         the normal state of a fresh HardDisc4.
 */
PackageInstalledMap PackageInstalledList(const wxString &hostfs_dir);

/** Outcome of an install or a removal. */
struct PackageActionResult {
	bool ok = false;
	bool cancelled = false;
	wxString message;	/* what went wrong, for the user */
	int files = 0;		/* files written, or removed */
};

/**
 * Download, check and install one package onto a machine's disc.
 *
 * The zip is fetched to a temporary file and its MD5 checked against the one the
 * index gave before anything is unpacked, so a truncated or substituted download
 * is refused rather than half-installed.
 *
 * @param record     The package, from the catalogue.
 * @param hostfs_dir The machine's HostFS directory.
 */
PackageActionResult PackageInstall(const PackageRecord &record,
                                   const wxString &hostfs_dir,
                                   RiscosFetchReporter &reporter,
                                   const RiscosFetchLoopFactory &make_loop,
                                   PackageTriggerRunner *triggers = nullptr);

/**
 * Remove a package, using the record of what it installed.
 *
 * Only files the database says belong to the package are deleted, and only
 * directories left empty by that are pruned. A file the user has since replaced
 * is still theirs to lose, which is the same bargain any package manager makes,
 * but nothing outside the manifest is touched.
 */
PackageActionResult PackageRemove(const wxString &package_name,
                                  const wxString &hostfs_dir,
                                  PackageTriggerRunner *triggers = nullptr);

/**
 * The packages a package needs, from its Depends field, in install order.
 *
 * Returns the names it could not find in the catalogue in @missing. Recommends
 * are not followed: they are suggestions, and pulling them in silently would
 * install more than was asked for.
 */
std::vector<PackageRecord> PackageResolveDepends(
    const PackageRecord &record,
    const std::vector<PackageRecord> &catalogue,
    const PackageInstalledMap &installed,
    std::vector<wxString> &missing);

#endif /* PACKAGE_INSTALL_H */
