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
# Requires arm-linux-gnueabi-as / -ld / -objcopy (setup-build-env.sh --podules).
#
# See docs/testing.md and COMPILE.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

COMPARE=true
if [ "${1:-}" = "--rebuild-only" ]; then
	COMPARE=false
fi

AS=arm-linux-gnueabi-as
LD=arm-linux-gnueabi-ld
OBJCOPY=arm-linux-gnueabi-objcopy

for tool in "$AS" "$LD" "$OBJCOPY"; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "error: $tool not found." >&2
		echo "Install the ARM tools with: ./setup-build-env.sh --podules" >&2
		exit 1
	fi
done

# Each entry is: source directory : the images it produces : where they are kept.
# Kept as a list rather than discovered, so a module that stops being built is a
# mismatch rather than something that silently drops out of the check.
MODULES=(
	"riscos-progs/HostFS:hostfs,ffa hostfsfiler,ffa:poduleroms"
	"riscos-progs/RPCEmuSupport:rpcemusupport,ffa:poduleroms"
	"riscos-progs/SyncClock:syncclock,ffa:poduleroms"
	"riscos-progs/RPCEmuUSBSupport:rpcemuusbsupport,ffa:poduleroms"
	"riscos-progs/RPCEmuPCIEmulator:rpcemupciemulator,ffa:poduleroms"
	"riscos-progs/RPCEmuCoPro:rpcemucopro,ffa:poduleroms"
	"riscos-progs/RPCEmuGfx:RPCEmuGfx,ffa:gfxroms"
	"riscos-progs/EtherRPCEm:EtherRPCEm,ffa:netroms"
	"riscos-progs/MultiFS:70-multifs,ffa 80-multifsfiler,ffa:usbroms"
)

# ScrollWheel is deliberately absent: its makefile wants clang, which is not part
# of this toolchain, so build.sh does not build it either and its binary is
# committed as it stands. Saying so here is the point - an unexplained gap in a
# list like this reads as an oversight.

failures=0
checked=0

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

	echo "==> $dir"
	(
		cd "$dir"
		make clean >/dev/null
		make AS="$AS" LD="$LD" OBJCOPY="$OBJCOPY" >/dev/null
	)

	for image in $images; do
		built="$dir/$image"
		committed="$dest/$image"

		if [ ! -f "$built" ]; then
			echo "error: $dir did not produce $image" >&2
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
			echo "    $image rebuilt into $dest/"
			continue
		fi

		if cmp -s "$built" "$committed"; then
			echo "    $image matches $dest/"
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
if [ "$failures" -ne 0 ]; then
	echo "guest modules: $failures problem(s) across $checked image(s)" >&2
	exit 1
fi

if [ "$COMPARE" = false ]; then
	echo "guest modules: $checked image(s) rebuilt"
else
	echo "guest modules: $checked image(s) match their sources"
fi
