#!/usr/bin/env bash
#
# staged_library_agreement() in build-macos.sh, which decides whether the two
# slices of a universal build may be fused.
#
# WHY THIS EXISTS. The judgement is a few lines of string handling with no
# dependencies, and it can fail in two directions that both matter: too strict
# stops every release, too lax puts two unrelated libraries into one file and
# ships it. So it is tested rather than eyeballed.
#
# CI gives both slices one prefix, so the usual answer is the trivial one. The
# rest of the cases are a local build against a package manager with a prefix
# per architecture, where two runners can hold different revisions of a package
# and the two paths then differ in ways that do not mean different software.
# Getting that wrong once failed every build on main and 1.x with nothing in
# the tree changed, which is why those cases are kept.
#
# Run: tests/test_fuse_libs.sh

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

# Pull the function out of the build script and define it here. Sourcing the
# whole script would run it; this takes the one function under test, which also
# means the test fails loudly if it is renamed or removed.
fn=$(awk '/^staged_library_agreement\(\) \{/,/^\}/' "$root/build-macos.sh")
if [ -z "$fn" ]; then
	echo "error: staged_library_agreement() not found in build-macos.sh" >&2
	exit 2
fi
eval "$fn"

failures=0

check() {
	local what="$1" x86="$2" arm="$3" want="$4" got

	got=$(staged_library_agreement "$x86" "$arm")
	if [ "$got" = "$want" ]; then
		printf '  %-58s ok\n' "$what"
	else
		printf '  %-58s FAIL (got %s, want %s)\n' "$what" "$got" "$want"
		failures=$((failures + 1))
	fi
}

# Both slices take their libraries from MacPorts, which installs to /opt/local
# whatever the architecture, so the two source paths are identical and the
# first comparison answers outright. This is the shipping configuration and so
# the first thing tested.
echo "One prefix for both slices, which is how CI builds"
check "identical /opt/local paths" \
	/opt/local/lib/libwebp.7.dylib \
	/opt/local/lib/libwebp.7.dylib \
	same
check "a framework layout, as wxWidgets installs" \
	/opt/local/Library/Frameworks/wxWidgets.framework/Versions/wxWidgets/3.1/lib/libwx_baseu-3.2.dylib \
	/opt/local/Library/Frameworks/wxWidgets.framework/Versions/wxWidgets/3.1/lib/libwx_baseu-3.2.dylib \
	same
check "one prefix, genuinely different paths" \
	/opt/local/lib/libfoo.dylib \
	/opt/local/lib/other/libfoo.dylib \
	differ

echo "The same library under each prefix"
check "identical tails" \
	/usr/local/Cellar/webp/1.6.0/lib/libwebp.7.dylib \
	/opt/homebrew/Cellar/webp/1.6.0/lib/libwebp.7.dylib \
	same

echo "The runners drifting apart, which is what broke CI"
check "pcre2 10.48 against 10.47_1" \
	/usr/local/Cellar/pcre2/10.48/lib/libpcre2-32.0.dylib \
	/opt/homebrew/Cellar/pcre2/10.47_1/lib/libpcre2-32.0.dylib \
	version
check "a plain version bump" \
	/usr/local/Cellar/webp/1.6.1/lib/libwebp.7.dylib \
	/opt/homebrew/Cellar/webp/1.6.0/lib/libwebp.7.dylib \
	version
check "a revision suffix on one side only" \
	/usr/local/Cellar/glib/2.84.0/lib/libglib-2.0.0.dylib \
	/opt/homebrew/Cellar/glib/2.84.0_1/lib/libglib-2.0.0.dylib \
	version

# Not every Homebrew library sits under a Cellar directory, and one at the
# prefix root with the same relative path in both slices is the same library by
# the only definition available here. Kept as a case because it is the one place
# the version-forgiving path must NOT be reached: there is no version to forgive,
# and answering "version" would mean skipping the comparison that matters.
echo "At the prefix root rather than under a Cellar"
check "the same relative path under each prefix" \
	/usr/local/lib/libfoo.dylib \
	/opt/homebrew/lib/libfoo.dylib \
	same
check "different relative paths, neither a Cellar" \
	/usr/local/lib/libfoo.dylib \
	/opt/homebrew/lib/other/libfoo.dylib \
	differ

echo "What must still be refused"
check "different formulae with one basename" \
	/usr/local/Cellar/pcre2/10.48/lib/libpcre2-32.0.dylib \
	/opt/homebrew/Cellar/pcre/8.45/lib/libpcre2-32.0.dylib \
	differ
check "same formula, different file below the version" \
	/usr/local/Cellar/webp/1.6.0/lib/libwebp.7.dylib \
	/opt/homebrew/Cellar/webp/1.6.0/lib32/libwebp.7.dylib \
	differ
# Two package managers, one basename. Whether they are the same library cannot
# be told from the paths, so it is refused rather than guessed at.
check "a different package manager on each side" \
	/opt/local/lib/libpcre2-32.0.dylib \
	/opt/homebrew/Cellar/pcre2/10.47_1/lib/libpcre2-32.0.dylib \
	differ
# An opt/ symlink is not a Cellar path and its version cannot be read from it,
# so it is refused rather than guessed at.
check "an opt symlink rather than a Cellar path" \
	/usr/local/opt/pcre2/lib/libpcre2-32.0.dylib \
	/opt/homebrew/Cellar/pcre2/10.47_1/lib/libpcre2-32.0.dylib \
	differ

echo
if [ "$failures" -eq 0 ]; then
	echo "all ok"
	exit 0
fi
echo "$failures FAILED"
exit 1
