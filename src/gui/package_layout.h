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
 * package_layout.h - where a machine's package database lives, and the logical
 * paths its manifests are written against.
 *
 * ★ The database is shared with PackMan, so its location is not ours to choose.
 *
 * On a real RISC OS disc the database is "!Boot.Resources.!Packages", and both
 * PackMan and RiscPkg find it there through the Packages$Dir system variable
 * that its own !Boot file sets. A machine can therefore already have one before
 * RPCEmu touches it, with packages recorded in it that we did not install. So
 * the rule is to join that database rather than keep a second one beside it: two
 * databases on one disc means each manager overwrites files the other believes
 * it owns, and neither can uninstall what the other put down.
 *
 * Early versions kept it at "$.!Packages", which no RISC OS tool looks in. That
 * location survives as the fallback for a disc with no !Boot at all, and a
 * database found there is migrated on sight.
 *
 * Split out of package_install.cpp so it can be tested: deciding where the
 * database goes, merging two of them, and resolving a logical path are all pure
 * over the filesystem, and getting any of them wrong loses packages or deletes
 * the wrong files. tests/test_package_layout.cpp builds this file alone.
 */

#ifndef PACKAGE_LAYOUT_H
#define PACKAGE_LAYOUT_H

#include <map>
#include <set>

#include <wx/string.h>

/** Which of the two locations a machine's database is in. */
enum class PackagesLayout {
	/** !Boot.Resources.!Packages: where RISC OS tools look. */
	BootResources,
	/** $.!Packages: no !Boot on this disc, so nothing else is looking. */
	Legacy
};

struct PackagesLocation {
	wxString dir;			/**< absolute host path to !Packages */
	PackagesLayout layout = PackagesLayout::Legacy;
	bool existed = false;		/**< the directory was already there */
};

/**
 * Decide where the database belongs on this disc. Creates nothing.
 *
 * Prefers an existing !Boot.Resources.!Packages, then !Boot.Resources if the
 * disc has one, then the legacy location.
 */
PackagesLocation PackagesResolve(const wxString &hostfs_dir);

/**
 * Resolve, create from the bundled template if absent, and merge a legacy
 * database into it. Idempotent, and safe to call before a read.
 *
 * A database whose Version file we do not recognise is left completely alone and
 * the legacy location returned instead: appending to a format from the future is
 * how a working PackMan installation gets corrupted.
 */
PackagesLocation PackagesPrepare(const wxString &hostfs_dir);

/**
 * The logical paths a manifest is written against, read from the database's
 * Paths file, or a built-in copy of the standard set when it has none.
 *
 * Keys are RISC OS logical names, dot separated and possibly multi-component
 * ("Apps", "System", "Apps.Admin.!PackMan"). Values are paths relative to the
 * disc's root, '/' separated, already expanded and with any "^" collapsed. The
 * separator stays '/' rather than the host's so that the resolution is the same
 * on every platform and the tests do not have to know which one they are on.
 */
using PackagesPathMap = std::map<wxString, wxString>;

PackagesPathMap PackagesReadPaths(const wxString &hostfs_dir,
                                  const wxString &packages_dir);

/**
 * Apply that map to a zip member's stored path.
 *
 * @stored A member name as the archive holds it, '/' separated
 *         ("Apps/Games/!ADFFS/!Run").
 * @return The same path with its logical root replaced by where that root
 *         actually is, still '/' separated and still relative to the disc's
 *         root ("Apps/Games/!ADFFS/!Run", or for a system module
 *         "!Boot/Resources/!System/310/Modules/...").
 *
 * The longest logical name wins, so "Apps.Admin.!PackMan" beats "Apps". A root
 * that is not in the map is returned untouched, which is what keeps a package
 * using some name we have never seen installing where it always did.
 */
wxString PackagesApplyPaths(const PackagesPathMap &paths,
                            const wxString &stored);

/** Parse one Status file into name -> whole line. Exposed for the merge. */
std::map<wxString, wxString> PackagesReadStatus(const wxString &path);

/**
 * The directories the DISC owns, as absolute host paths.
 *
 * Every logical name names a directory that exists because the disc is laid out
 * that way, not because a package created it: "$.Apps", "$.!Boot.Resources", the
 * PreDesk directory. So this is the floor for tidying empty directories away
 * after a removal.
 *
 * Without it the tidy-up walks up out of the package and into the boot structure:
 * a package with a ToBeLoaded component installs into
 * "!Boot.Choices.Boot.PreDesk", and removing the last such package would take
 * PreDesk with it, a directory the OS reads at every boot.
 */
std::set<wxString> PackagesDiscOwnedDirs(const wxString &hostfs_dir,
                                         const PackagesPathMap &logical);

/**
 * A host path somewhere under the disc, as RISC OS names it.
 *
 * "<disc>/!Boot/Resources/!Packages/Info/Zool" becomes
 * "$.!Boot.Resources.!Packages.Info.Zool".
 *
 * Needed because some paths have to be handed to the guest as text - a trigger is
 * an Obey file run inside RISC OS and told where it lives. Deriving it rather than
 * writing "$.!Packages" down is the difference between a trigger that runs and one
 * that is pointed at a directory that moved.
 *
 * Deliberately not "<Packages$Dir>", which is only set when the boot sequence has
 * run the database's own !Boot, and so is unset in the legacy location.
 */
wxString PackagesRiscosPath(const wxString &hostfs_dir,
                            const wxString &host_path);

#endif /* PACKAGE_LAYOUT_H */
