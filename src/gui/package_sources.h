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
 * package_sources.h - where the package catalogue is fetched from.
 *
 * The repositories were a fixed list in the code, which meant a new one could
 * only arrive in a new release. They now live in a file the user owns,
 * "pkgsources" in the data directory, so a repository can be added, renamed,
 * turned off or removed without waiting for us.
 *
 * The file is deliberately in the same shape as the indexes themselves:
 * "Name: value" lines, a blank line between records, # for a comment. Somebody
 * who has looked at a package index can edit it without being told how, and it
 * survives being hand-edited badly, because a record that does not make sense
 * is reported and skipped rather than taking the rest of the file with it.
 */

#ifndef PACKAGE_SOURCES_H
#define PACKAGE_SOURCES_H

#include <vector>

#include <wx/string.h>

/** Where an index came from, what it is called, and whether to read it. */
struct PackageSource {
	wxString name;		/* short name, e.g. "rool" */
	wxString url;
	wxString description;
	bool enabled = true;
};

/**
 * The sources shipped as the starting point.
 *
 * Deliberately not every index that exists: RISC OS Open also publish
 * programs-armv5 and two Raspberry Pi indexes, which hold code for machines
 * this is not, so offering them would only hand somebody software that cannot
 * run here.
 */
std::vector<PackageSource> PackageSourcesDefaults();

/** The file the sources are read from and written to. */
wxString PackageSourcesPath();

/**
 * Read the sources.
 *
 * With no file, the defaults are returned and written out, so that the first
 * thing a curious user finds is a working file to edit rather than an absence.
 * Any complaint about the file's contents is appended to @warnings, which may
 * be null; a bad record is skipped, never fatal.
 */
std::vector<PackageSource> PackageSourcesLoad(std::vector<wxString> *warnings = nullptr);

/** Write the sources. False on failure, with why in @error. */
bool PackageSourcesSave(const std::vector<PackageSource> &sources, wxString *error);

/**
 * Is this a usable source name?
 *
 * It has to be, because the name becomes a filename: the catalogue cache is
 * one file per source. Anything with a separator or a dot-dot in it could
 * write outside the cache directory, so the allowed set is narrow and checked
 * here rather than trusted from a file the user edits by hand.
 */
bool PackageSourceNameValid(const wxString &name, wxString *why = nullptr);

/** Is this a usable index URL? http or https, and nothing else. */
bool PackageSourceUrlValid(const wxString &url, wxString *why = nullptr);

#endif /* PACKAGE_SOURCES_H */
