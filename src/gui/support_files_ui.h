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
 */

#ifndef SUPPORT_FILES_UI_H
#define SUPPORT_FILES_UI_H

class wxWindow;

/**
 * Bring the data directory's guest-side files up to date with this build,
 * saying so on screen if there is enough to be worth mentioning.
 *
 * Called once at startup, after the data directory is known and before
 * anything reads a module out of it - the machine selector cannot offer a
 * machine whose expansion cards have nothing to load.
 *
 * Does nothing, and shows nothing, when the files are already current, which is
 * every start but the first after an upgrade.
 *
 * @param parent Window to be modal to, or nullptr when there is not one yet
 * @return Number of files brought over
 */
int SupportFilesEnsure(wxWindow *parent);

#endif /* SUPPORT_FILES_UI_H */
