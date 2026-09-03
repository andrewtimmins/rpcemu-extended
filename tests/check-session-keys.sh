#!/usr/bin/env bash
#
# Every setting the Machine Inspector saves, it must also load.
#
# WHY THIS EXISTS. The session file (Save session... / Load session... on the
# Trace tab) is written by one function and read by another, a hundred lines
# apart, with the key name spelled out as a literal in both. Adding a setting
# means touching both, and forgetting the reader fails silently in the worst
# way: the file looks right, it contains the setting, and loading it quietly
# does nothing. Whoever saved a session and reloaded it would conclude the
# setting does not stick and go looking in the wrong place entirely.
#
# So this compares the two lists of literals. No compiler can: they are strings.

set -u

src="${1:-src/gui/machine_inspector_window.cpp}"

if [ ! -f "$src" ]; then
	echo "check-session-keys: $src not found" >&2
	exit 1
fi

# Written: file.AddLine("<key> ...") and file.AddLine(wxString::Format("<key> ...
written=$(sed -n 's/.*file\.AddLine(\(wxString::Format(\)\{0,1\}"\([a-z_][a-z_0-9]*\) .*/\2/p' \
	"$src" | sort -u)

# Read: key == "<key>"
read_keys=$(sed -n 's/.*key == "\([a-z_][a-z_0-9]*\)".*/\1/p' "$src" | sort -u)

if [ -z "$written" ] || [ -z "$read_keys" ]; then
	echo "check-session-keys: found no keys at all - has the file been renamed?" >&2
	exit 1
fi

fail=0

for key in $written; do
	if ! echo "$read_keys" | grep -qx "$key"; then
		echo "  FAIL saved but never loaded: $key"
		fail=1
	fi
done

for key in $read_keys; do
	if ! echo "$written" | grep -qx "$key"; then
		echo "  FAIL loaded but never saved: $key"
		fail=1
	fi
done

count=$(echo "$written" | wc -w | tr -d ' ')

if [ "$fail" -ne 0 ]; then
	echo "check-session-keys: FAILED"
	exit 1
fi

echo "  $count session settings, each one both saved and loaded          ok"
echo "check-session-keys: all tests passed"
