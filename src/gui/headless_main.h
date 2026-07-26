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

#ifndef HEADLESS_MAIN_H
#define HEADLESS_MAIN_H

#include <string>

/*
 * Console entry points for headless operation.
 *
 * These deliberately avoid ALL wxWidgets code: they are reached from main()
 * before any wxApp is constructed, so the toolkit never opens a connection to a
 * display server (gtk_init() under wxGTK, and the equivalent on wxMSW/wxOSX).
 * They therefore work with no display or desktop session at all - no X11/Wayland
 * session on Linux, no interactive desktop on Windows, no window server on
 * macOS. Each returns a process exit code.
 */
/*
 * Sink for the output of HeadlessPrintUsage()/HeadlessListMachines(). Both are
 * reachable from a normal GUI launch (--help, --list-machines), and on Windows
 * a GUI-subsystem binary has no console to print to, so main() redirects them
 * into a message box. Left null, they print to stdout as usual.
 *
 * Only these two use it. The genuinely headless paths keep printing to the
 * console: --headless is driven from a terminal or a service manager, where a
 * dialog would be useless or invisible.
 */
void HeadlessSetOutputSink(void (*sink)(const char *text));

void HeadlessPrintUsage(const char *argv0);
int HeadlessListMachines(void);
/*
 * Run a machine headlessly. machine_name may be NULL (reported as an error).
 * `resume` loads the machine's own suspend snapshot, consuming it to .bak;
 * `state_file` (may be NULL) loads an explicit state file and leaves it in
 * place. The two are mutually exclusive; main() rejects the combination.
 */
int RunHeadless(const char *machine_name, bool resume, const char *state_file);

/*
 * Resolve a machine name to an existing config path, or return an empty string
 * if there is no such config. Also used by the GUI path, where --machine
 * bypasses the machine selector: validating before wxEntry() means a bad name
 * is reported on the console rather than needing an initialised toolkit.
 *
 * Requires the data paths to have been set up first (see HeadlessInitPaths).
 * Matching is exact, so on a case-sensitive filesystem the name must match the
 * .cfg file's case; only the ".cfg" suffix itself is matched case-insensitively.
 */
std::string HeadlessResolveMachineConfig(const char *machine_name);

/*
 * Locate the data/resource directories without wxWidgets. Returns false if no
 * directory containing a 'configs/' folder could be found; call
 * HeadlessPrintNoDataError() to report that.
 */
bool HeadlessInitPaths(void);
void HeadlessPrintNoDataError(void);

#endif
