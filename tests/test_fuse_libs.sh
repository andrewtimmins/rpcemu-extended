#!/usr/bin/env bash
#
# staged_library_agreement() in build-macos.sh, which decides whether the two
# slices of a universal build may be fused.
#
# WHY THIS EXISTS. On 2 September 2026 every build on main and 1.x failed, and
# nothing in the tree had changed: the two macOS runners are separate images with
# separate Homebrew installations, and they had drifted to pcre2 10.48 on x86_64
# and 10.47_1 on arm64. The check compared the whole path below each slice's
# Homebrew prefix - version directory included - so the same formula at two
# versions read as "a different library in each slice" and the fuse step refused.
#
# The judgement is four lines of string handling with no dependencies, and it can
# fail in two directions that both matter: too strict stops every release, too
# lax puts two unrelated libraries into one file and ships it. So it is tested
# rather than eyeballed, and the case that broke CI is one of the cases.
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
check "a non-Homebrew prefix on one side" \
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
