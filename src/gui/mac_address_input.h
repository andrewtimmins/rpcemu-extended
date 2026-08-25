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

#ifndef MAC_ADDRESS_INPUT_H
#define MAC_ADDRESS_INPUT_H

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <random>
#include <string>

/*
 * The MAC address field's typing rules, apart from wxWidgets so they can be
 * tested without a window.
 */
namespace MacAddressInput {

/** Is this a hex digit, in either case? */
inline bool IsHexDigit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * Reduce whatever is in the field to the address it describes.
 *
 * Everything that is not a hex digit is dropped - including the separators,
 * which Format() puts back where they belong - so a pasted "06-02-03-04-05-06"
 * or "060203040506" is accepted as readily as a typed one. Case is preserved
 * for Format() to settle.
 */
inline std::string Digits(const std::string &text)
{
	std::string digits;

	for (const char c : text) {
		if (IsHexDigit(c) && digits.size() < 12) {
			digits.push_back(c);
		}
	}
	return digits;
}

/**
 * Lay hex digits out as an address, colons and all.
 *
 * A partial address formats as far as it goes, so the field reads correctly
 * while it is still being typed.
 *
 * `trailing_separator` adds the colon after a complete pair. It is what the
 * typist gets for pressing ':' themselves: without it that keypress appeared to
 * do nothing at all - the colon was stripped and only came back when the next
 * digit arrived - and a field that ignores a key you were right to press reads
 * as broken rather than as helpful.
 */
inline std::string Format(const std::string &digits, bool trailing_separator = false)
{
	std::string out;

	for (size_t i = 0; i < digits.size(); i++) {
		if (i != 0 && (i % 2) == 0) {
			out.push_back(':');
		}
		out.push_back((char) tolower((unsigned char) digits[i]));
	}

	/* Not after the sixth pair: the address is complete and a seventh could
	   never follow. */
	if (trailing_separator && !digits.empty() && (digits.size() % 2) == 0 &&
	    digits.size() < 12) {
		out.push_back(':');
	}
	return out;
}

/** Does this text end in a separator the typist put there? */
inline bool EndsWithSeparator(const std::string &text)
{
	if (text.empty()) {
		return false;
	}

	const char last = text[text.size() - 1];

	return last == ':' || last == '-' || last == '.' || last == ' ';
}

/** What the field should read, given what was typed into it. */
inline std::string Normalise(const std::string &text)
{
	return Format(Digits(text), EndsWithSeparator(text));
}

/** Is this a complete address, ready to be saved? */
inline bool IsComplete(const std::string &text)
{
	return Digits(text).size() == 12;
}

/**
 * Make up an address for a machine that has never had one.
 *
 * Locally administered and not multicast - bit 1 of the first byte set, bit 0
 * clear - so it cannot collide with real hardware.
 *
 * std::random_device rather than rand(): rand() is the same sequence in every
 * process unless it is seeded, so every machine on every computer would be
 * handed one identical address, which is the collision this exists to avoid.
 * The caller writes the result back to the machine's configuration, since one
 * generated afresh on every start would change identity on every boot.
 */
inline std::string Generate()
{
	std::random_device rd;
	std::uniform_int_distribution<unsigned> byte(0, 255);
	unsigned char hwaddr[6];
	char out[18];

	for (unsigned char &b : hwaddr) {
		b = (unsigned char) byte(rd);
	}
	hwaddr[0] = (unsigned char) ((hwaddr[0] | 0x02u) & ~0x01u);

	std::snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x",
	              hwaddr[0], hwaddr[1], hwaddr[2],
	              hwaddr[3], hwaddr[4], hwaddr[5]);
	return std::string(out);
}

} /* namespace MacAddressInput */

#endif /* MAC_ADDRESS_INPUT_H */
