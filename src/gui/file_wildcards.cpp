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

#include "file_wildcards.h"

wxString HostCaseInsensitiveWildcard(const wxString &wildcard)
{
#ifndef __WXGTK__
	return wildcard;
#else
	wxString out;
	bool pattern = false;	/* halves alternate label, patterns, label, ... */

	for (size_t i = 0; i < wildcard.length(); i++) {
		const wxUniChar c = wildcard[i];

		if (c == '|') {
			pattern = !pattern;
			out += c;
			continue;
		}

		/*
		 * [aA] rather than a;A so that a mixed-case extension matches
		 * too - .Iso and .iMg turn up in the wild, and spelling out
		 * every combination as its own pattern would not.
		 *
		 * ASCII arithmetic rather than tolower(): the letters here are
		 * literals in this source, and a Turkish locale turning 'I'
		 * into a dotless 'ı' would quietly stop .ISO matching.
		 */
		const wxUint32 u = c.GetValue();
		const bool letter = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');

		if (pattern && letter) {
			const char lower = (char)(u | 0x20);

			out += '[';
			out += lower;
			out += (char)(lower - 0x20);
			out += ']';
		} else {
			out += c;
		}
	}

	return out;
#endif
}
