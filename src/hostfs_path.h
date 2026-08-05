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

#ifndef HOSTFS_PATH_H
#define HOSTFS_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a machine's HostFS drive lives on the host.
 *
 * Discussion #77 asked for this to be configurable so that several machines can
 * share one folder, which is much easier when testing the same software across
 * different machine configurations. Until now it was hardwired to
 * <machine directory>/hostfs.
 *
 * ★ THE THREE-WAY CONVENTION MATTERS MORE THAN THE FEATURE. hd4_path and rom_dir
 * already work this way, and it is what makes a configuration survive having its
 * data folder moved:
 *
 *   empty     the default, <machine directory>/hostfs. Nothing is stored, so
 *             there is nothing to go stale.
 *   relative  resolved under the machine directory. Moves with the machine.
 *   absolute  taken as given. The only form that breaks when things move, and
 *             the only form the user has to type deliberately.
 *
 * Storing the resolved default instead of an empty string is exactly how
 * debug_socket ended up holding a path to a folder that no longer existed, so
 * this one keeps out of that trap by construction: a caller that has not been
 * asked for anything unusual stores nothing.
 *
 * Pure, with no filesystem access, so tests/test_hostfs_path.c can check the
 * Windows forms from a Linux runner too.
 */

/**
 * Is this path absolute, on any platform we support?
 *
 * Accepts Unix, UNC and drive-letter forms whatever the host, so a configuration
 * copied from a Windows machine to a Mac is understood rather than silently
 * treated as a relative path and resolved somewhere unexpected.
 */
int hostfs_path_is_absolute(const char *path);

/**
 * Resolve the HostFS root for a machine.
 *
 * @param configured  The `hostfs_path` setting: empty, relative or absolute.
 * @param machine_dir The machine's own directory, with or without a trailing
 *                    separator.
 * @param out         Filled with the resolved root, using forward slashes and
 *                    with no trailing separator.
 * @param len         Capacity of @out.
 * @return Non-zero on success. Zero if the result would not fit, in which case
 *         @out is left empty rather than truncated: a truncated path is a
 *         different directory, and for HostFS that means writing the guest's
 *         files somewhere nobody asked for. Also zero when @configured contains a
 *         control character, which is what a hand-edited Windows path turns into
 *         once wxFileConfig has unescaped it - see has_control_char() in
 *         hostfs_path.c for how "C:\foo" becomes "C:<form feed>oo".
 */
int hostfs_path_resolve(const char *configured, const char *machine_dir,
                        char *out, size_t len);

/**
 * The directory one level above @path.
 *
 * For telling "you asked for a folder that is not there yet" apart from "this
 * whole path is stale". A machine whose absolute hostfs_path pointed inside its
 * data folder, after that folder has moved, names a directory whose PARENT has
 * gone too - and HostFS creates the lot, so the old tree reappears empty and the
 * guest boots to a blank disc with nothing to explain it. Only the parent test
 * separates that from somebody legitimately asking for a new folder on a drive
 * that does exist.
 *
 * Pure, like the rest of this file: the caller does the asking-the-filesystem.
 *
 * @return Non-zero on success. Zero when @path has no parent worth testing - a
 *         bare leaf such as "hostfs", an empty path, or one too long to handle.
 *         A drive-relative parent ("C:") is returned as that drive's root
 *         ("C:/"), since "C:" on Windows means the current directory of the
 *         drive and a caller testing for a directory would get the wrong answer.
 */
int hostfs_path_parent(const char *path, char *out, size_t len);

/**
 * Would two machines be sharing one HostFS folder?
 *
 * Compares resolved roots, ignoring trailing separators and separator style, so
 * that "/data/hostfs", "/data/hostfs/" and "\\data\\hostfs" are recognised as
 * the same place. Case is significant: it is on the filing systems where this
 * matters most, and treating distinct directories as identical would suppress a
 * warning that ought to be given.
 */
int hostfs_path_same_root(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* HOSTFS_PATH_H */
