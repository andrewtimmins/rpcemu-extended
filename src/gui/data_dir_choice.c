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
 * data_dir_choice.c - the precedence rules for where the data directory is.
 *
 * The reasoning is in data_dir_choice.h. Plain C with no wxWidgets and no
 * filesystem access, so tests/test_data_dir.c can enumerate the table.
 */

#include "data_dir_choice.h"

#include <stddef.h>

/** Is this a usable path rather than absent or empty? */
static int
given(const char *value)
{
	return value != NULL && value[0] != '\0';
}

DataDirDecision
data_dir_decide(const DataDirInputs *in)
{
	DataDirDecision d;

	d.should_ask = 0;
	d.should_remember = 0;

	/*
	 * Explicit instructions first, strongest last-word first. Neither is
	 * remembered: both are supplied afresh on every run, so recording them
	 * would let one scripted run with an unusual --datadir silently become
	 * the default for every later interactive one.
	 */
	if (given(in->cli_datadir)) {
		d.source = DATA_DIR_FROM_CLI;
		return d;
	}
	if (given(in->env_datadir)) {
		d.source = DATA_DIR_FROM_ENV;
		return d;
	}

	/*
	 * A remembered choice outranks anything worked out by looking around, but
	 * NOT the environment above. The other way round would mean a CI job or a
	 * packaging script could be quietly redirected by whatever a developer once
	 * clicked in a dialogue, and a reproducible run is worth more than
	 * honouring a preference in the one case nobody is watching.
	 */
	if (given(in->stored_datadir)) {
		d.source = DATA_DIR_FROM_STORED;
		return d;
	}

	/*
	 * A layout that says where it wants to be. configs/ beside the binary is
	 * the portable download and the developer's build tree; in the current
	 * directory is how it is usually run from source. Never remembered: the
	 * whole point is that it follows the binary, and pinning the first one seen
	 * would break the next copy someone unpacked elsewhere. Never asked about
	 * either, because it is already an answer.
	 */
	if (in->configs_beside_binary || in->configs_in_cwd) {
		d.source = DATA_DIR_FROM_PORTABLE;
		return d;
	}

	/*
	 * A macOS .app, where the payload is read-only inside the bundle and user
	 * data has to live outside it. Checked before the install prefix because a
	 * bundle can be sitting anywhere, including on a disc image.
	 */
	if (in->configs_in_bundle) {
		d.source = DATA_DIR_FROM_BUNDLE;
		return d;
	}

	/*
	 * An installed copy: payload under the prefix, user data in the usual
	 * place. Distinguished from portable because the prefix is not writable.
	 */
	if (in->configs_in_install_dir) {
		d.source = DATA_DIR_FROM_PORTABLE;
		return d;
	}

	if (given(in->env_resource_dir)) {
		d.source = DATA_DIR_FROM_ENV_RESOURCE;
		return d;
	}

	/*
	 * ★ Already set up in the default place, so this is an existing user and
	 * the question has effectively been answered by a year of use. Asking now
	 * would be a worse bug than #67: an upgrade that interrupts to ask about
	 * data you already have. The location is recorded so that later runs take
	 * the branch above and nothing has to be inferred again.
	 */
	if (in->default_location_ready) {
		d.source = DATA_DIR_FROM_EXISTING_DEFAULT;
		d.should_remember = 1;
		return d;
	}

	/*
	 * Nothing to go on: a genuine first run. Ask if there is anything to ask
	 * with, and remember the answer.
	 */
	if (in->can_ask) {
		d.source = DATA_DIR_ASK;
		d.should_ask = 1;
		d.should_remember = 1;
		return d;
	}

	/*
	 * First run with no GUI - headless, --pkg-list, --fetch-riscos, a
	 * container. Behave exactly as before and do NOT remember, so that the
	 * first interactive run still gets to ask rather than inheriting a choice
	 * nobody made.
	 */
	d.source = DATA_DIR_DEFAULT_UNASKED;
	return d;
}

const char *
data_dir_source_name(DataDirSource source)
{
	switch (source) {
	case DATA_DIR_FROM_CLI:			return "--datadir";
	case DATA_DIR_FROM_ENV:			return "RPCEMU_DATADIR";
	case DATA_DIR_FROM_ENV_RESOURCE:	return "RPCEMU_RESOURCE_DIR";
	case DATA_DIR_FROM_STORED:		return "remembered choice";
	case DATA_DIR_FROM_PORTABLE:		return "layout beside the binary";
	case DATA_DIR_FROM_BUNDLE:		return "application bundle";
	case DATA_DIR_FROM_EXISTING_DEFAULT:	return "existing default location";
	case DATA_DIR_ASK:			return "asked the user";
	case DATA_DIR_DEFAULT_UNASKED:		return "default, nothing to ask with";
	}
	return "unknown";
}
