/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef DATA_PATHS_H
#define DATA_PATHS_H

#include <functional>

#include <wx/string.h>

/**
 * Asked to let the user choose a data directory, on a first run.
 *
 * Supplied by the caller rather than built here, so that this file stays free of
 * dialogue code and the console entry points cannot accidentally raise one.
 *
 * @param suggested Where RPCEmu would have put it, to be offered as the default
 *                  so that agreeing costs one click and behaves as it always did.
 * @param chosen    Set to the directory the user picked.
 * @return true if the user chose; false to fall back to @suggested.
 */
using DataDirPrompt = std::function<bool(const wxString &suggested, wxString *chosen)>;

/**
 * Resolve install prefix / portable layout and seed per-user data when needed.
 *
 * @param cli_datadir --datadir from the command line, or empty. Outranks
 *                    everything, including the remembered choice.
 * @param prompt      How to ask on a first run, or nullptr for "do not ask",
 *                    which is what every console entry point passes. Passing
 *                    nullptr guarantees no dialogue, whatever else is true.
 *
 * The precedence rules, and which situations do and do not ask, are in
 * data_dir_choice.h and enumerated in tests/test_data_dir.c.
 */
void InitRpcemuPaths(const wxString &cli_datadir = wxEmptyString,
                     DataDirPrompt prompt = nullptr);

#endif
