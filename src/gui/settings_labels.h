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
 * settings_labels.h - the Settings menu entries that are not display options.
 *
 * A machine's Settings menu is built twice: by the machine's own window
 * (main_frame_menus.cpp) and by the Manager, for the machine it is showing
 * (manager_frame.cpp). Both offer the same settings and each used to spell them
 * out for itself, so they drifted - "Two-button Mouse Mode" against "Two-Button
 * Mouse", "Share Clipboard with RISC OS" against "Shared Clipboard", "Open This
 * Machine Automatically" against "Default Machine". Three settings reading as
 * six, and the Manager set no help text at all, so the same menu explained
 * itself in one window and said nothing in the other.
 *
 * Naming each one once makes the two menus mirror each other by construction
 * rather than by somebody remembering to change both. The help text is here for
 * the same reason: both windows show it in the status bar.
 *
 * The display choices are named in display_options.h, which the machine editor
 * reads as well - they are kept separate because that file also holds the mode
 * arithmetic, which has nothing to do with menus.
 */

#ifndef SETTINGS_LABELS_H
#define SETTINGS_LABELS_H

namespace SettingsLabels {

inline const char *MuteSound() { return "Mute Sound"; }
inline const char *MuteSoundHelp()
{
	return "Silence this machine. RISC OS carries on as though it were making "
	       "the sound.";
}

inline const char *MouseFollows() { return "Mouse Follows Host Pointer"; }
inline const char *MouseFollowsHelp()
{
	return "On, the RISC OS pointer goes wherever the host one is. Off, a click "
	       "captures the mouse and RISC OS is sent movements instead, which is "
	       "what games that drive the pointer themselves need. Alt+Enter gives "
	       "it back.";
}

inline const char *TwoButtonMouse() { return "Two-button Mouse Mode"; }
inline const char *TwoButtonMouseHelp()
{
	return "Treat the middle button as the RISC OS menu button, for a mouse that "
	       "has only two.";
}

inline const char *SharedClipboard() { return "Share Clipboard with RISC OS"; }
inline const char *SharedClipboardHelp()
{
	return "Copy and paste text between the host and RISC OS. Needs the "
	       "SharedClipboard module, which loads itself in the guest.";
}

inline const char *ReduceCpu() { return "Reduce CPU Usage"; }
inline const char *ReduceCpuHelp()
{
	return "Let the host processor idle when RISC OS is idle, instead of running "
	       "flat out.";
}

inline const char *DefaultMachine() { return "Open This Machine Automatically"; }
inline const char *DefaultMachineHelp()
{
	return "Start RPCEmu straight into this machine, without showing the machine "
	       "list. Hold Shift while starting to get the list back.";
}

} /* namespace SettingsLabels */

#endif /* SETTINGS_LABELS_H */
