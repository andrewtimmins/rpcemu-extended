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

#ifndef SUPPORT_BUNDLE_H
#define SUPPORT_BUNDLE_H

#include <vector>

#include <wx/filefn.h>
#include <wx/string.h>

/** One file that went into the bundle, for telling the user what was sent. */
struct SupportBundleMember {
	wxString name;		/**< Path as stored in the archive */
	wxFileOffset size;	/**< Bytes, as written */
};

struct SupportBundleResult {
	bool ok = false;
	wxString message;	/**< Why it failed, when it did */
	std::vector<SupportBundleMember> members;
};

/**
 * Collect what a bug report needs into a zip at @dest_path.
 *
 * The log, the machine's configuration, its CMOS and the emulator metadata:
 * small files that describe how the machine is set up and what it last did.
 * The hard disc is not among them, and neither is anything else large.
 *
 * The configuration is redacted on the way in - see the implementation - so
 * this can be attached to a public issue without handing over a password or
 * the reporter's home directory.
 *
 * The machine is named rather than taken from the loaded configuration, so the
 * Manager can bundle any machine it knows of. Every file here is a file on
 * disk: a machine does not have to be running, and one that crashed is exactly
 * the machine somebody wants a bundle for.
 *
 * @param machine_name    Which machine, for the archive's configuration member.
 *                        Empty leaves the configuration out.
 * @param machine_dir     That machine's data directory, holding its CMOS and
 *                        emulator metadata.
 * @param log_path        Its rpclog.txt.
 * @param screenshot_path A PNG of the guest's screen to include, or empty for
 *                        none. Asked for rather than taken: it is whatever the
 *                        machine happens to be showing, which is the one thing
 *                        here that cannot be redacted - and a machine that is
 *                        not running has no screen to show.
 * @param manager_log_path The data directory's own rpclog.txt, written by a
 *                        process that has not chosen a machine - which is the
 *                        Manager, for its whole life. It records starting
 *                        machines and attaching to them, so it is the log that
 *                        says why a machine never got far enough to write one
 *                        of its own. Left out when the file is not there.
 */
SupportBundleResult SupportBundleWrite(const wxString &dest_path,
                                       const wxString &machine_name,
                                       const wxString &machine_dir,
                                       const wxString &log_path,
                                       const wxString &screenshot_path = wxString(),
                                       const wxString &manager_log_path = wxString());

/** Suggested leafname, e.g. "rpcemu-support-Test1-20260803.zip". */
wxString SupportBundleSuggestedName(const wxString &machine_name);

#endif /* SUPPORT_BUNDLE_H */
