#!/usr/bin/env bash
#
# Rebuild every guest module from source and check the result against the binary
# committed alongside it.
#
# The modules under riscos-progs/ are RISC OS code that runs inside the emulated
# machine: HostFS and its filer, RPCEmuSupport, SyncClock, the USB and PCI
# support modules, the graphics card's display driver and the network driver.
# They are assembled with the ARM binutils and the assembled images are
# committed, because a user building RPCEmu is not asked to install a
# cross-assembler.
#
# That arrangement has one failure mode, and it is a quiet one: someone edits a
# module's source, does not rebuild it, and the emulator carries on loading the
# stale committed image. Nothing fails. The source and the shipped module simply
# disagree, and the next person to read the source is reading something the
# machine has never run. Worse, CI never built these at all until this script
# existed - build.sh skips them when the ARM tools are absent, which they were,
# so every one of them shipped untouched by the build.
#
# So: rebuild them, then compare. A difference is a hard error naming the file.
#
# Usage: tests/check-guest-modules.sh [--rebuild-only]
#
#   --rebuild-only   leave the rebuilt images in place and skip the comparison,
#                    for a working tree where the source has deliberately moved
#                    ahead of what is committed.
#
# Requires ARM binutils: arm-linux-gnueabi-* (setup-build-env.sh --podules) or
# arm-none-eabi-* (brew install arm-none-eabi-binutils on macOS).
#
# See docs/testing.md and COMPILE.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

COMPARE=true
if [ "${1:-}" = "--rebuild-only" ]; then
	COMPARE=false
fi

# Which ARM binutils to use.
#
# arm-linux-gnueabi- is what setup-build-env.sh --podules installs and what CI
# has, so it is tried first and remains the reference. arm-none-eabi- is the
# fallback because that is what Homebrew ships on macOS, where the Linux triple
# is not packaged at all - and without a fallback this check simply could not be
# run on a Mac, which is where a fair amount of the work happens.
#
# The two are interchangeable here. These modules are position-independent ARM
# assembly linked to a bare binary with no libc, no start files and no relocation
# beyond --section-start, so nothing the two toolchains disagree about is in
# play: both were verified to produce byte-identical images for every module in
# the list below.
PREFIX=""
for candidate in arm-linux-gnueabi- arm-none-eabi-; do
	if command -v "${candidate}as" >/dev/null 2>&1 &&
	   command -v "${candidate}ld" >/dev/null 2>&1 &&
	   command -v "${candidate}objcopy" >/dev/null 2>&1; then
		PREFIX="$candidate"
		break
	fi
done

if [ -z "$PREFIX" ]; then
	echo "error: no ARM binutils found (tried arm-linux-gnueabi-, arm-none-eabi-)." >&2
	echo "Install the ARM tools with: ./setup-build-env.sh --podules" >&2
	echo "On macOS: brew install arm-none-eabi-binutils" >&2
	exit 1
fi

AS="${PREFIX}as"
LD="${PREFIX}ld"
OBJCOPY="${PREFIX}objcopy"
echo "Using ${PREFIX}* binutils"

# Each entry is: source directory : the images it produces : where they are kept.
# Kept as a list rather than discovered, so a module that stops being built is a
# mismatch rather than something that silently drops out of the check.
#
# An image may be written "built=installed" where the two names differ. Only
# MultiFS needs it: it is its own project and builds under its own names, and
# the numeric prefix that orders the USB card's ROM is applied here, on the way
# in. Everything else installs under the name it builds under.
MODULES=(
	"riscos-progs/HostFS:hostfs,ffa hostfsfiler,ffa:poduleroms"
	"riscos-progs/RPCEmuSupport:rpcemusupport,ffa:poduleroms"
	"riscos-progs/SyncClock:syncclock,ffa:poduleroms"
	"riscos-progs/RPCEmuUSBSupport:rpcemuusbsupport,ffa:poduleroms"
	"riscos-progs/RPCEmuPCIEmulator:rpcemupciemulator,ffa:poduleroms"
	"riscos-progs/RPCEmuGfx:RPCEmuGfx,ffa:gfxroms"
	"riscos-progs/EtherRPCEm:EtherRPCEm,ffa:netroms"
	"riscos-progs/MultiFS:MultiFS,ffa=70-multifs,ffa MultiFSFiler,ffa=80-multifsfiler,ffa:usbroms"
)

# ScrollWheel is deliberately absent: its makefile wants clang, which is not part
# of this toolchain, so build.sh does not build it either and its binary is
# committed as it stands. Saying so here is the point - an unexplained gap in a
# list like this reads as an oversight.

# MultiFS was missing from this list until FAT12 was added to it, which is
# exactly the failure the script was written to catch: a module whose committed
# image nothing compared against its source. It goes in usbroms/ rather than
# poduleroms/ because it is carried in the USB card's own ROM.
#
# MultiFS is also the one entry whose source is a SUBMODULE
# (github.com/andrewtimmins/riscos-multifs). An uninitialised submodule leaves
# an empty directory behind, so it is skipped with a message rather than
# treated as a missing module - but see the note at the end about CI, because a
# check that skips itself is worth less than no check at all if nobody notices.

failures=0
checked=0
skipped=0

for entry in "${MODULES[@]}"; do
	dir="${entry%%:*}"
	rest="${entry#*:}"
	images="${rest%%:*}"
	dest="${rest##*:}"

	if [ ! -d "$dir" ]; then
		echo "error: $dir is missing, so its module cannot be checked" >&2
		failures=$((failures + 1))
		continue
	fi

	# A submodule that has not been checked out: the directory is there and
	# empty. Not an error - a user building the emulator has no reason to
	# fetch it - but say so, loudly enough that a CI run missing it is
	# noticed rather than read as a pass.
	if [ ! -f "$dir/Makefile" ]; then
		echo "==> $dir"
		echo "    SKIPPED: no Makefile here."
		echo "    If this is a submodule, fetch it with:"
		echo "        git submodule update --init $dir"
		skipped=$((skipped + 1))
		continue
	fi

	echo "==> $dir"
	(
		cd "$dir"
		make clean >/dev/null
		make AS="$AS" LD="$LD" OBJCOPY="$OBJCOPY" >/dev/null
	)

	for image in $images; do
		# "built=installed", or just the one name when they agree.
		built_name="${image%%=*}"
		installed_name="${image#*=}"
		built="$dir/$built_name"
		committed="$dest/$installed_name"

		if [ ! -f "$built" ]; then
			echo "error: $dir did not produce $built_name" >&2
			failures=$((failures + 1))
			continue
		fi
		if [ ! -f "$committed" ]; then
			echo "error: $committed is missing" >&2
			failures=$((failures + 1))
			continue
		fi

		checked=$((checked + 1))

		if [ "$COMPARE" = false ]; then
			cp -f "$built" "$committed"
			echo "    $built_name rebuilt into $dest/$installed_name"
			continue
		fi

		if cmp -s "$built" "$committed"; then
			echo "    $installed_name matches $dir/$built_name"
		else
			echo "error: $committed does not match a rebuild of $dir" >&2
			echo "       built:     $(wc -c < "$built") bytes" >&2
			echo "       committed: $(wc -c < "$committed") bytes" >&2
			echo "       Rebuild with ./build.sh --podules and commit the result," >&2
			echo "       or with tests/check-guest-modules.sh --rebuild-only." >&2
			failures=$((failures + 1))
		fi
	done
done

echo ""
if [ "$skipped" -ne 0 ]; then
	echo "guest modules: $skipped module(s) skipped for want of their source."
	echo "CI must NOT skip any: the workflow checks out submodules, and a skip"
	echo "there means that has broken and nothing is checking those images."
fi

if [ "$failures" -ne 0 ]; then
	echo "guest modules: $failures problem(s) across $checked image(s)" >&2
	exit 1
fi

if [ "$COMPARE" = false ]; then
	echo "guest modules: $checked image(s) rebuilt"
else
	echo "guest modules: $checked image(s) match their sources"
fi
