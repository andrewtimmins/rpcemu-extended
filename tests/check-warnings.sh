#!/usr/bin/env bash
#
# Fail when the code we maintain compiles with warnings.
#
# Not -Werror in the build system, deliberately. -Werror turns a new compiler
# release into a build that will not build at all, on somebody else's machine,
# for a warning that has nothing to do with them - which is a poor trade for a
# project people build from source on three platforms and several distributions.
# The place to be strict is here, where we control the compiler and a failure
# costs a maintainer ten minutes rather than costing a user their afternoon.
#
# Files listed in tests/warnings-exempt.txt are allowed to warn: inherited code,
# and the one case where the fix would change behaviour we cannot test here.
# Everything else must be clean. See that file for the rule about adding to it.
#
# Usage: tests/check-warnings.sh <build-log> [exempt-file]
#
# The build log is whatever the compiler printed - capture it with
#   cmake --build build 2>&1 | tee build.log
# and note that a full build is needed, since an incremental one only recompiles
# what changed and will report a clean tree that is nothing of the sort.
#
# See docs/testing.md.

set -uo pipefail

LOG="${1:-}"
EXEMPT="${2:-}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
[ -n "$EXEMPT" ] || EXEMPT="$here/warnings-exempt.txt"

if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
	echo "usage: $0 <build-log> [exempt-file]" >&2
	exit 2
fi
if [ ! -f "$EXEMPT" ]; then
	echo "error: no exemption list at $EXEMPT" >&2
	exit 2
fi

# Every "<file>:<line>:<col>: warning: ..." line, with the path made relative to
# the repository so the exemption list can be written in repository terms.
warnings="$(grep -E "^[^ ]+:[0-9]+:[0-9]+: warning:" "$LOG" \
            | sed "s|^${root}/||" \
            | sort -u)"

if [ -z "$warnings" ]; then
	echo "==> no compiler warnings at all"
	exit 0
fi

# Anything the compiler found outside the repository is somebody else's header
# being included by us; there is nothing to fix in this repository for it.
mapfile -t patterns < <(grep -vE "^\s*(#|$)" "$EXEMPT" | sed 's/[[:space:]]*$//')

exempt_count=0
offending=""
while IFS= read -r line; do
	[ -n "$line" ] || continue
	file="${line%%:*}"

	# Outside the tree (an absolute path survived the strip above).
	case "$file" in
	/*)
		exempt_count=$((exempt_count + 1))
		continue
		;;
	esac

	skip=0
	for p in "${patterns[@]}"; do
		case "$p" in
		*/) [ "${file#"$p"}" != "$file" ] && skip=1 ;;
		*)  [ "$file" = "$p" ] && skip=1 ;;
		esac
		[ "$skip" = 1 ] && break
	done

	if [ "$skip" = 1 ]; then
		exempt_count=$((exempt_count + 1))
	else
		offending="$offending$line"$'\n'
	fi
done <<< "$warnings"

total=$(printf '%s\n' "$warnings" | grep -c .)
offending_count=$(printf '%s' "$offending" | grep -c . || true)

echo "==> $total distinct warnings: $exempt_count from exempt or external files, $offending_count from ours"

if [ "$offending_count" -eq 0 ]; then
	echo "==> the code we maintain compiled clean"
	exit 0
fi

echo
echo "::error::$offending_count compiler warnings in code this project maintains"
printf '%s' "$offending"
echo
echo "Fix them, or - if it is inherited code, or the fix would change behaviour"
echo "that cannot be tested on all three platforms - add the file to"
echo "tests/warnings-exempt.txt with the reason. Do not add a line to quieten"
echo "something that ought to be fixed; that list is meant to get shorter."
exit 1
