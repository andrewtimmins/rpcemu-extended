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
 * The MAC address field's typing rules, and the generator that fills it in for
 * a machine that has never had an address.
 */

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "mac_address_input.h"

extern "C" {
#include "network.h"
}

static int failures = 0;

static void
check(const char *what, bool ok)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

int
main(void)
{
	using namespace MacAddressInput;

	std::printf("typing an address\n");
	check("a complete address is left as it is",
	    Normalise("06:02:03:04:05:06") == "06:02:03:04:05:06");
	check("colons are inserted as the digits arrive",
	    Normalise("060203") == "06:02:03");
	check("a lone digit stands on its own",
	    Normalise("0") == "0");
	check("no trailing colon after a complete pair",
	    Normalise("0602") == "06:02");

	/*
	 * ★ Pressing ':' must visibly do something.
	 *
	 * It used to be stripped and only reappear when the next digit arrived, so
	 * the keypress looked ignored and the field looked broken - the one thing a
	 * typist is most likely to try.
	 */
	std::printf("typing a separator\n");
	check("a colon after a pair is kept",
	    Normalise("06:") == "06:");
	check("so is a hyphen",
	    Normalise("06-") == "06:");
	check("and it is not doubled when the next digit arrives",
	    Normalise("06:0") == "06:0");
	check("a colon mid-pair waits for the pair to finish",
	    Normalise("0:") == "0");
	check("no seventh separator after a complete address",
	    Normalise("06:02:03:04:05:06:") == "06:02:03:04:05:06");
	check("a trailing separator still counts as complete",
	    IsComplete("06:02:03:04:05:06:"));

	std::printf("what the field refuses\n");
	check("letters beyond f are dropped",
	    Normalise("zz06gg02") == "06:02");
	check("spaces and punctuation are dropped",
	    Normalise("06 02-03") == "06:02:03");
	check("more than six pairs cannot be typed",
	    Normalise("060203040506070809") == "06:02:03:04:05:06");

	std::printf("case\n");
	check("upper case is folded to lower, matching the generator",
	    Normalise("AA:BB:CC:DD:EE:FF") == "aa:bb:cc:dd:ee:ff");
	check("mixed case likewise",
	    Normalise("aA:Bb:cC:Dd:eE:Ff") == "aa:bb:cc:dd:ee:ff");

	std::printf("other separators, as pasted\n");
	check("hyphens are accepted",
	    Normalise("06-02-03-04-05-06") == "06:02:03:04:05:06");
	check("no separator at all is accepted",
	    Normalise("060203040506") == "06:02:03:04:05:06");

	std::printf("completeness\n");
	check("a whole address is complete", IsComplete("06:02:03:04:05:06"));
	check("a partial address is not", !IsComplete("06:02:03"));
	check("an empty field is not", !IsComplete(""));
	check("eleven digits are not enough", !IsComplete("06:02:03:04:05:0"));

	/*
	 * What a configuration can say that is not an address. Networking refuses to
	 * start on any of these rather than inventing one to carry on with, so the
	 * parser saying no is what makes that guard work.
	 */
	std::printf("what the parser refuses\n");
	{
		uint8_t parsed[6];
		static const char *const bad[] = {
			"", "not-a-mac-addr", "06:02:03:04:05", "06:02:03:04:05:06:07",
			"zz:zz:zz:zz:zz:zz", "06-02-03-04-05-06", "060203040506",
			"06:02:03:04:05:6", " 06:02:03:04:05:06"
		};

		for (const char *s : bad) {
			char what[80];

			std::snprintf(what, sizeof(what), "\"%s\" is refused", s);
			check(what, network_macaddress_parse(s, parsed) == 0);
		}
		check("a good one is still accepted",
		    network_macaddress_parse("06:02:03:04:05:06", parsed) == 1);
	}

	std::printf("a generated address\n");
	{
		const std::string generated = Generate();
		uint8_t parsed[6];

		check("is 17 characters", generated.size() == 17);
		check("parses as a MAC address",
		    network_macaddress_parse(generated.c_str(), parsed) == 1);
		/*
		 * Bit 1 of the first byte set and bit 0 clear: the range set aside for
		 * addresses nobody registered, which cannot collide with real hardware.
		 * A multicast address (bit 0) would be accepted by the parser and then
		 * behave very strangely as a source address.
		 */
		check("is locally administered", (parsed[0] & 0x02) != 0);
		check("is not multicast", (parsed[0] & 0x01) == 0);
		check("reads back as it was written",
		    Normalise(generated) == generated);
	}

	{
		/*
		 * ★ The addresses must differ, and this is the check that would have
		 * caught the first version: it used an unseeded rand(), so every machine
		 * on every computer was handed one identical address - exactly the
		 * collision the whole feature exists to prevent.
		 */
		std::set<std::string> seen;
		bool all_local = true;
		bool all_unicast = true;
		bool all_parse = true;

		for (int i = 0; i < 1000; i++) {
			const std::string mac = Generate();
			uint8_t bytes[6];

			seen.insert(mac);
			if (network_macaddress_parse(mac.c_str(), bytes) != 1) {
				all_parse = false;
				continue;
			}
			if ((bytes[0] & 0x02) == 0) {
				all_local = false;
			}
			if ((bytes[0] & 0x01) != 0) {
				all_unicast = false;
			}
		}
		check("1000 generated addresses are all different", seen.size() == 1000);
		check("every one parses", all_parse);
		check("every one is locally administered", all_local);
		check("every one is unicast", all_unicast);
	}

	if (failures != 0) {
		std::printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("\nall checks passed\n");
	return 0;
}
