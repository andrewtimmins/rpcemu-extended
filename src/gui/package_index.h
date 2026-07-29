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
 * package_index.h - the catalogue of RISC OS packages.
 *
 * RISC OS Open host indexes of packaged software at packages.riscosopen.org.
 * Each is a text file of control records, one per package, in the format the
 * RISC OS Packaging Project's policy manual defines: fields as "Name: value",
 * continuation lines indented by a space, records separated by a blank line.
 * Every record carries the URL of the package's zip and an MD5 to check it by.
 *
 * This reads those indexes. It installs nothing and knows nothing about a
 * machine; see package_db.h for what is installed and package_install.h for
 * putting it there.
 */

#ifndef PACKAGE_INDEX_H
#define PACKAGE_INDEX_H

#include <map>
#include <vector>

#include <wx/string.h>

#include "riscos_fetch.h"	/* RiscosFetchReporter, RiscosFetchLoopFactory */

/**
 * One package, as the index describes it.
 *
 * The named members are the fields worth having in a struct; everything else
 * stays in @fields, because the format is open and a package may carry fields
 * this does not know about. Reading them by name rather than position means a
 * new field is somebody else's business, not a parse failure.
 */
struct PackageRecord {
	wxString name;			/* Package */
	wxString version;		/* Version */
	wxString section;		/* Section, e.g. "Graphics" */
	wxString description;		/* Description, first line */
	wxString description_long;	/* Description, continuation lines */
	wxString url;			/* URL of the zip, made absolute */
	wxString md5;			/* MD5Sum of the zip */
	wxString environment;		/* Environment: any, arm, arm32 */
	wxString licence;		/* Licence */
	wxString depends;		/* Depends */
	wxString recommends;		/* Recommends */
	wxString osdepends;		/* OSDepends */
	wxString components;		/* Components */
	wxString homepage;		/* Homepage */
	wxString maintainer;		/* Maintainer */
	long long size = 0;		/* Size, in bytes */

	std::map<wxString, wxString> fields;	/* every field, by name */

	/** Value of any field, or empty. Case-insensitive, as the format is. */
	wxString Field(const wxString &field_name) const;

	/**
	 * True if this package can run on the machine RPCEmu emulates.
	 *
	 * The Risc PC's StrongARM is ARMv4, so an index built for ARMv5 or for
	 * the Raspberry Pi holds code that will not run here. "any" means
	 * architecture-independent and "arm"/"arm32" mean 32-bit ARM, which is
	 * what we are.
	 */
	bool RunsHere() const;
};

/** Where an index came from, and what it is called. */
struct PackageSource {
	wxString name;		/* short name, e.g. "rool" */
	wxString url;
	wxString description;
};

/**
 * The sources worth offering on an emulated Risc PC.
 *
 * Deliberately not all of the ones RISC OS Open publish: the ARMv5 and
 * Raspberry Pi indexes are for machines we are not.
 */
std::vector<PackageSource> PackageIndexSources();

/**
 * Parse an index, appending to @out.
 *
 * Returns the number of records read. A malformed record is skipped rather than
 * failing the file: these are third-party indexes of 194 packages and growing,
 * and one bad record should not cost the user the catalogue.
 */
int PackageIndexParse(const wxString &text, const wxString &source_name,
                      const wxString &index_url,
                      std::vector<PackageRecord> &out);

/** Outcome of refreshing the catalogue. */
struct PackageIndexResult {
	bool ok = false;
	bool cancelled = false;
	wxString message;		/* why it failed, for the user */
	int sources_read = 0;
	int packages = 0;
};

/**
 * Fetch every source and return what they hold, newest version of each package.
 *
 * The result is cached under the data directory so browsing the catalogue does
 * not re-fetch it, and so that ROOL's server is not asked for the same file
 * repeatedly. Pass @force to fetch regardless of the cache.
 */
PackageIndexResult PackageIndexRefresh(std::vector<PackageRecord> &out,
                                       RiscosFetchReporter &reporter,
                                       const RiscosFetchLoopFactory &make_loop,
                                       bool force = false);

/** Load the cached catalogue without going near the network. */
bool PackageIndexLoadCached(std::vector<PackageRecord> &out);

/** Age of the cache in seconds, or -1 if there is none. */
long PackageIndexCacheAge();

#endif /* PACKAGE_INDEX_H */
