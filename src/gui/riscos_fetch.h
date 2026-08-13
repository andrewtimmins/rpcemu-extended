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
 * riscos_fetch.h - fetching RISC OS from RISC OS Open.
 *
 * A Risc PC needs a ROM image, and is not much use without a hard disc, and
 * RPCEmu can ship neither. RISC OS Open publish both, so this fetches them and
 * assembles a machine around them, which is otherwise a long-winded manual job
 * for somebody who has not used RISC OS in thirty years.
 *
 * The work is the same whether it was asked for from a dialog or from the
 * command line, so it lives here and reports progress through an interface the
 * caller implements.
 */

#ifndef RISCOS_FETCH_H
#define RISCOS_FETCH_H

#include <functional>

#include <wx/progdlg.h>
#include <wx/string.h>

class wxEventLoopBase;

/** Which build to fetch. Both come from the same page and cost the same. */
enum class RiscosRelease {
	Stable,		/**< The current stable release */
	Nightly		/**< The development build, rebuilt as changes land */
};

/*
 * What to fetch, and what to do with it.
 *
 * Three shapes, from the three places this is asked for:
 *   create_machine                    build a new machine around the download
 *   !create_machine, include_disc      put the disc on machine_name
 *   !create_machine, !include_disc     the ROM and nothing else
 *
 * A hard disc always belongs to some machine, so include_disc without
 * create_machine needs machine_name to say which.
 *
 * Clearing include_rom asks for the disc on its own, for a machine that has a
 * ROM already and only wants something to boot. Nothing about the machine
 * needs to be known to do that: the disc carries a boot sequence that works
 * with any version, reading the one it is running on and configuring itself
 * to match the first time it starts.
 */
struct RiscosFetchRequest {
	RiscosRelease release = RiscosRelease::Stable;

	/** Fetch the ROM. Cleared to fetch only the hard disc. */
	bool include_rom = true;

	/** Also fetch HardDisc4 and unpack it onto the machine's HostFS. */
	bool include_disc = true;

	/** Merge the bundled network configuration into the disc's !Boot. */
	bool configure_network = false;

	/**
	 * Create a machine around what was fetched. When false the ROM lands in
	 * roms/ and any disc goes to the machine named below, which is what the
	 * machine editor wants: it has a machine already.
	 */
	bool create_machine = true;

	/**
	 * Which machine. Creating one, this names it, and an empty string derives
	 * a name from the release. Not creating one, this is the existing machine
	 * the disc is unpacked onto.
	 */
	wxString machine_name;
};

struct RiscosFetchOutcome {
	bool ok = false;

	/** Why it failed, ready to show. Empty when it did not. */
	wxString message;

	/** True when the user cancelled, so the caller can stay quiet about it. */
	bool cancelled = false;

	wxString rom_name;	/**< Leafname written into roms/ */
	wxString version;	/**< "5.30", say, as read from the download */
	wxString machine_name;	/**< Machine created, empty if none was */
	wxString config_path;	/**< Its configuration file */
	int disc_files = 0;	/**< Files unpacked onto the HostFS */
};

/**
 * Progress reporting, implemented by the caller. Every method returns false to
 * abandon the fetch, which is how Cancel is delivered.
 */
class RiscosFetchReporter {
public:
	virtual ~RiscosFetchReporter() = default;

	/** A new step is starting, e.g. "Fetching RISC OS 5.30". */
	virtual bool Stage(const wxString &description) = 0;

	/** Progress within the step. @total is 0 when the size is not known. */
	virtual bool Progress(long long done, long long total) = 0;
};

/**
 * Acknowledge who RISC OS comes from, and its licensing, before downloading.
 *
 * Shown before anything is fetched. RISC OS Open Limited publish the ROM and the
 * hard disc we download, and RISC OS Developments Limited own RISC OS itself;
 * neither is us, and a program that helps itself to their files should say so
 * and get the user's agreement first.
 *
 * @return true if the user agreed and the download may proceed
 */
bool RiscosFetchConfirmLicence(wxWindow *parent);

/**
 * The same notice as plain text, for the command line.
 *
 * Ends without a trailing blank line.
 */
wxString RiscosFetchLicenceText();

/**
 * A reporter that drives a progress box, and takes Cancel from it.
 *
 * Shared by every windowed caller so the download looks the same wherever it
 * was started from.
 */
class RiscosFetchProgressReporter : public RiscosFetchReporter {
public:
	explicit RiscosFetchProgressReporter(wxWindow *parent);

	bool Stage(const wxString &description) override;
	bool Progress(long long done, long long total) override;

private:
	wxProgressDialog dialog_;
	wxString stage_;
};

/**
 * Supplies a fresh event loop to wait on a transfer with.
 *
 * The kind of loop needed depends on the application object: the graphical
 * one wants the toolkit's loop, whereas --fetch-riscos runs under a console
 * application with no display connection, where that loop cannot be used.
 * Only the application knows which it is, and the call that would ask it is
 * not public, so the caller passes this in.
 */
using RiscosFetchLoopFactory = std::function<wxEventLoopBase *()>;

/**
 * Fetch, unpack and install, reporting as it goes.
 *
 * Nothing is written into place until the download and unpacking have both
 * succeeded, so a cancelled or failed fetch leaves no half-built machine.
 *
 * @param make_loop Optional; defaults to the toolkit's own event loop, which
 *                  is what a windowed caller wants
 */
RiscosFetchOutcome RiscosFetchPerform(const RiscosFetchRequest &request,
                                      RiscosFetchReporter &reporter,
                                      RiscosFetchLoopFactory make_loop = {});

/**
 * True when roms/ already holds something that could be a ROM.
 *
 * Used at startup to decide whether to offer this at all: with no ROM the
 * emulator cannot start a machine, and the error a new user meets instead is
 * a dead end.
 */
bool RiscosFetchHaveRom();

/** Approximate download size in bytes, for telling the user before they commit. */
long long RiscosFetchApproximateSize(const RiscosFetchRequest &request);

/**
 * True when a machine's HostFS holds nothing but what a new machine is seeded
 * with, so a hard disc could be unpacked onto it without destroying anything.
 *
 * A machine's disc is the user's, and this is the test that keeps a download
 * from overwriting one. Offered publicly so the option can be shown as
 * unavailable rather than failing once it has been chosen.
 */
bool RiscosFetchMachineDiscIsEmpty(const wxString &machine_name);

#endif /* RISCOS_FETCH_H */
