#!/usr/bin/env bash
#
# Every Mach-O file staged for a slice must be the slice's own architecture.
#
# A successful link is weaker evidence than it looks. A wrong-architecture
# library usually fails at link time, but a library that is simply absent does
# not: the binary links against what it found, the staging step bundles what the
# binary references, and the slice comes out quietly missing something. lipo
# then fuses two slices that are not the same software.
#
# So this asserts the finished staged tree rather than trusting the build, and
# it runs per slice, before the two ever meet.
#
# Usage: check-slice-arch.sh <appstage-dir> <arch>
set -euo pipefail

STAGE="${1:?usage: check-slice-arch.sh <appstage-dir> <arch>}"
WANT="${2:?usage: check-slice-arch.sh <appstage-dir> <arch>}"

[ -d "$STAGE" ] || { echo "error: no such directory: $STAGE" >&2; exit 1; }

bad=0
checked=0

for f in "$STAGE"/bin/* "$STAGE"/libs/*; do
	[ -f "$f" ] || continue
	# deps.map and anything else non-Mach-O lives here too.
	file -b "$f" | grep -q 'Mach-O' || continue

	got=$(lipo -archs "$f" 2>/dev/null || echo '?')
	checked=$((checked + 1))

	if [ "$got" != "$WANT" ]; then
		echo "::error::$(basename "$f") is '$got', not $WANT"
		bad=$((bad + 1))
	else
		printf '  %-44s %s\n' "$(basename "$f")" "$got"
	fi
done

if [ "$checked" -eq 0 ]; then
	echo "error: no Mach-O files found under $STAGE - nothing was staged" >&2
	exit 1
fi

if [ "$bad" -gt 0 ]; then
	echo "error: $bad of $checked staged files are not $WANT" >&2
	exit 1
fi

echo "✓ all $checked staged files are $WANT"

# The minimum macOS each file declares. Reported, not enforced: the x86_64
# slice is configured for 10.15, but MacPorts builds its ports against the OS
# it runs on, so a bundled library may require more than the app claims. That
# is a runtime failure on an old Mac and nothing in the build would catch it.
# This is the evidence for setting the real figure.
echo "==> Minimum macOS declared by each staged file"
for f in "$STAGE"/bin/* "$STAGE"/libs/*; do
	[ -f "$f" ] || continue
	file -b "$f" | grep -q 'Mach-O' || continue
	minos=$(otool -l "$f" 2>/dev/null \
		| awk '/LC_BUILD_VERSION/ {v=1} v && /minos/ {print $2; exit}')
	[ -n "$minos" ] || minos=$(otool -l "$f" 2>/dev/null \
		| awk '/LC_VERSION_MIN_MACOSX/ {v=1} v && /version/ {print $2; exit}')
	printf '  %-44s %s\n' "$(basename "$f")" "${minos:-unknown}"
done
