#!/bin/bash
# RPCEmu (Extended Edition) - Linux build script
#
# Usage:
#   ./build.sh                         # Linux release build (dynarec) for host arch
#   ./build.sh --arch arm64            # Linux arm64 (native on Pi, cross from x86)
#   ./build.sh --deb                   # Linux + .deb for selected arch
#   ./build.sh --zip                   # Linux build + .tar.gz in releases/linux/
#   ./build.sh --interpreter           # Interpreter build (no dynarec)
#   ./build.sh --debug                 # Debug build (-debug suffix on binary name)
#   ./build.sh --no-podules            # Skip the guest ROMs (podules, display driver)
#   ./build.sh --podules               # Insist on the guest ROMs (fail if the tools are absent)
#   ./build.sh --clean                 # Remove build directories and releases
#
# The guest ROMs are built by default, so a change to anything under riscos-progs/
# is picked up without having to remember a flag. They need the ARM cross-assembler
# (./setup-build-env.sh --podules); without it they are skipped with a note and the
# committed ROMs are used, which is what a machine that only builds the emulator
# wants.
#
# Environment:
#   GHOSTPDL_PREFIX=/opt/ghostpdl      # Optional full GhostPDL for PCL print jobs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

get_version() {
	# The number always comes from VERSION; git only says which commit this is,
	# so a build from anywhere but a release tag cannot be mistaken for the
	# release. Matches the logic in CMakeLists.txt.
	local release
	if [ -f VERSION ]; then
		release="$(tr -d ' \t\r\n' < VERSION)"
	else
		echo "0.0.0"
		return
	fi
	if command -v git >/dev/null 2>&1 && [ -d .git ] \
	   && ! git describe --tags --exact-match --match 'v[0-9]*' >/dev/null 2>&1; then
		local commit
		commit="$(git rev-parse --short HEAD 2>/dev/null)"
		if [ -n "$commit" ]; then
			if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
				printf '%s-g%s-dirty\n' "$release" "$commit"
			else
				printf '%s-g%s\n' "$release" "$commit"
			fi
			return
		fi
	fi
	printf '%s\n' "$release"
}

normalize_linux_arch() {
	case "$1" in
		x86_64|amd64) echo amd64 ;;
		aarch64|arm64) echo arm64 ;;
		*) echo "" ;;
	esac
}

host_linux_arch() {
	normalize_linux_arch "$(uname -m)"
}

VERSION="$(get_version)"
BUILD_DEB=false
BUILD_ZIP=false
BUILD_INTERPRETER=false
BUILD_BOTH=false
BUILD_DIR=build
EXTRA_BINARY=
BUILD_DEBUG=false
BUILD_PODULES=true
PODULES_EXPLICIT=false
CLEAN_ONLY=false
FORCE_CROSS_ARM64=false
LINUX_ARCH=""

prev=""
for arg in "$@"; do
	if [ -n "$prev" ]; then
		case "$prev" in
			--arch)
				LINUX_ARCH=$(normalize_linux_arch "$arg")
				if [ -z "$LINUX_ARCH" ]; then
					echo "Error: unsupported Linux architecture '$arg' (use amd64 or arm64)"
					exit 1
				fi
				;;
		esac
		prev=""
		continue
	fi

	case $arg in
		--interpreter|-i) BUILD_INTERPRETER=true ;;
		--both) BUILD_BOTH=true ;;
		--debug|-g) BUILD_DEBUG=true ;;
		--deb|-d) BUILD_DEB=true ;;
		--zip|-z) BUILD_ZIP=true ;;
		--cross-arm64) FORCE_CROSS_ARM64=true; LINUX_ARCH=arm64 ;;
		--podules|-p) BUILD_PODULES=true; PODULES_EXPLICIT=true ;;
		--no-podules) BUILD_PODULES=false ;;
		--clean|-c) CLEAN_ONLY=true ;;
		--help|-h)
			echo "Usage: $0 [options]"
			echo ""
			echo "  --arch ARCH         Linux target: amd64 or arm64 (default: host)"
			echo "  --cross-arm64       Cross-compile Linux arm64 from x86_64"
			echo "  --interpreter, -i   Build interpreter instead of dynarec"
			echo "  --both              Build and ship both: recompiler and interpreter"
			echo "  --debug, -g         Debug build"
			echo "  --deb, -d           Create .deb package"
			echo "  --zip, -z           Create .tar.gz in releases/linux/"
			echo "  --no-podules        Skip the guest ROMs under riscos-progs/"
			echo "  --podules, -p       Insist on the guest ROMs (fail if the ARM tools are absent)"
			echo "  --clean, -c         Remove build trees and releases/"
			exit 0
			;;
		--arch) prev="--arch" ;;
		-*)
			echo "Error: unknown option '$arg' (try --help)"
			exit 1
			;;
		*)
			echo "Error: unexpected argument '$arg' (try --help)"
			exit 1
			;;
	esac
done

if [ -n "$prev" ]; then
	echo "Error: option '$prev' requires a value"
	exit 1
fi

if [ "$FORCE_CROSS_ARM64" = true ]; then
	LINUX_ARCH=arm64
fi

if [ -z "$LINUX_ARCH" ]; then
	LINUX_ARCH=$(host_linux_arch)
fi

HOST_LINUX_ARCH=$(host_linux_arch)
LINUX_CROSS=false
if [ "$LINUX_ARCH" != "$HOST_LINUX_ARCH" ]; then
	if [ "$LINUX_ARCH" = "arm64" ] && [ "$HOST_LINUX_ARCH" = "amd64" ]; then
		LINUX_CROSS=true
	else
		echo "Error: cannot build linux/$LINUX_ARCH on a $HOST_LINUX_ARCH host"
		exit 1
	fi
fi

if [ "$FORCE_CROSS_ARM64" = true ] && [ "$HOST_LINUX_ARCH" != "amd64" ]; then
	echo "Error: --cross-arm64 is only for cross-compiling from x86_64"
	exit 1
fi

LINUX_RELEASE="releases/linux/$LINUX_ARCH"
NPROC=$(nproc 2>/dev/null || echo 4)

clean_build() {
	echo "Cleaning build directories and releases..."
	# Only build DIRECTORIES - a bare "build-*" glob also matches the tracked
	# build-windows.sh / build-macos.sh scripts and would delete them.
	rm -rf build build-win build-mac-x86_64 build-mac-arm64 releases
	rm -f rpcemu-recompiler rpcemu-interpreter
	rm -f rpcemu-recompiler-debug rpcemu-interpreter-debug
	rm -f rpclog.txt
	echo "✓ Clean complete"
}

if [ "$CLEAN_ONLY" = true ]; then
	clean_build
	exit 0
fi

binary_basename() {
	if [ "$BUILD_INTERPRETER" = true ]; then
		if [ "$BUILD_DEBUG" = true ]; then
			echo "rpcemu-interpreter-debug"
		else
			echo "rpcemu-interpreter"
		fi
		return
	fi
	if [ "$BUILD_DEBUG" = true ]; then
		echo "rpcemu-recompiler-debug"
	else
		echo "rpcemu-recompiler"
	fi
}

cmake_common_args() {
	if [ "$BUILD_INTERPRETER" = true ]; then
		echo -DRPCEMU_DYNAREC=OFF
	else
		echo -DRPCEMU_DYNAREC=ON
	fi
	if [ "$BUILD_DEBUG" = true ]; then
		echo -DCMAKE_BUILD_TYPE=Debug
	else
		echo -DCMAKE_BUILD_TYPE=Release
	fi
	# This is the release script, so a build that quietly cannot reach a USB
	# device is a broken release rather than a lesser one. Set
	# RPCEMU_REQUIRE_LIBUSB=OFF in the environment to build without it anyway.
	echo "-DRPCEMU_REQUIRE_LIBUSB=${RPCEMU_REQUIRE_LIBUSB:-ON}"
	echo -DCMAKE_INSTALL_PREFIX=/usr
}

stage_linux_release() {
	local binary_name="$1"
	local release_binary="$LINUX_RELEASE/$binary_name"

	mkdir -p "$LINUX_RELEASE"
	cp -a configs "$LINUX_RELEASE/"
	cp -a resources "$LINUX_RELEASE/"
	cp -a roms "$LINUX_RELEASE/"
	# The guest-side payload - poduleroms, netroms, gfxroms, usbroms, podules and
	# default - is deliberately NOT staged. It is embedded in the executable and
	# extracted into the data directory on first run, so shipping a second copy
	# beside the program is what let somebody keep an old one and run a new
	# emulator against it. See src/support_files.h.
	# Common HostFS "Shared" disc (shared across machines). Normally created on
	# first launch by the emulator; pre-create it so a fresh release is complete.
	mkdir -p "$LINUX_RELEASE/shared"
	# No machine is shipped. A machine RPCEmu made up has no ROM and an empty
	# disc, so it cannot start, and one appearing in the list only teaches a new
	# user that starting a machine does not work. The New... button creates one
	# properly, fetching RISC OS from RISC OS Open, and seeds it from default/
	# (staged above), which is why that directory ships and this one does not.
	mkdir -p "$LINUX_RELEASE/machines"
	cp -f COPYING README.md COMPILE.md "$LINUX_RELEASE/" 2>/dev/null || true
	cp -f setup-runtime-env.sh "$LINUX_RELEASE/" 2>/dev/null || true
	if [ -f packaging/rpcemu.desktop ]; then
		cp -f packaging/rpcemu.desktop "$LINUX_RELEASE/"
		# Point the launcher at the actual binary for this build (recompiler or
		# interpreter); packaging/rpcemu.desktop uses an @RPCEMU_GUI_TARGET@ token.
		sed -i "s/@RPCEMU_GUI_TARGET@/$binary_name/" "$LINUX_RELEASE/rpcemu.desktop"
	fi

	cp -f "$BUILD_DIR/bin/$binary_name" "$release_binary"
	chmod +x "$release_binary"
	cp -f "$release_binary" "$binary_name"

	# HostCmd host-side client (rpcemu-run + rpcemu-shell symlink). These let
	# the host drive the guest RISC OS command line; ship them alongside the
	# emulator binary. See docs/hostcmd.md.
	if [ -f "$BUILD_DIR/bin/rpcemu-run" ]; then
		cp -f "$BUILD_DIR/bin/rpcemu-run" "$LINUX_RELEASE/rpcemu-run"
		chmod +x "$LINUX_RELEASE/rpcemu-run"
		ln -sf rpcemu-run "$LINUX_RELEASE/rpcemu-shell"
	fi

	# DebugCmd host-side client. The counterpart of rpcemu-run: that one drives
	# RISC OS's command line, this one drives the processor underneath it.
	# See docs/debugcmd.md.
	if [ -f "$BUILD_DIR/bin/rpcemu-debug" ]; then
		cp -f "$BUILD_DIR/bin/rpcemu-debug" "$LINUX_RELEASE/rpcemu-debug"
		chmod +x "$LINUX_RELEASE/rpcemu-debug"
	fi

	# MCP server: drive a RISC OS machine (HostCmd + HostFS + VNC + the debugger
	# control socket) from an MCP client. Python; ships with requirements.txt +
	# README + config example. See tools/mcp/README.md and docs/debugcmd.md.
	if [ -d tools/mcp ]; then
		mkdir -p "$LINUX_RELEASE/tools/mcp"
		cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
		      tools/mcp/README.md tools/mcp/mcp.json.example \
		      tools/mcp/setup-mcp-env.sh \
		      "$LINUX_RELEASE/tools/mcp/" 2>/dev/null || true
	fi

	# Ship the full docs/ set so the README and MCP/HostCmd/debugger docs resolve.
	[ -d docs ] && cp -a docs "$LINUX_RELEASE/" 2>/dev/null || true

	# Whether USB passthrough is in this build, read from the binary rather than
	# assumed: it is the one feature that silently disappears if a build host is
	# missing a library, and "the USB dialogue is empty" is otherwise a puzzle.
	local usb_state="no (built without libusb)"
	if ldd "$release_binary" 2>/dev/null | grep -q "libusb-1.0"; then
		usb_state="yes (libusb)"
	fi

	cat > "$LINUX_RELEASE/BUILDINFO.txt" <<EOF
RPCEmu (Extended Edition) $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m)
Binary: $binary_name$([ -n "$EXTRA_BINARY" ] && echo " (and $(basename "$EXTRA_BINARY"), the other flavour)")
Toolkit: wxWidgets + CMake (Linux)
USB passthrough: $usb_state
EOF
}

create_linux_tarball() {
	local archive_name="rpcemu_${VERSION}_linux_${LINUX_ARCH}.tar.gz"
	local archive_path="releases/linux/$archive_name"
	mkdir -p releases/linux
	tar -czf "$archive_path" -C "$LINUX_RELEASE" .
	echo "✓ Linux archive: $archive_path"
}

build_podules() {
	local hostfs_dir="riscos-progs/HostFS"

	# Built by default, so a missing toolchain must not stop the emulator being
	# built: say so and leave the committed ROMs in place. Only --podules, where
	# the guest ROMs are the point of the run, treats it as an error.
	if [ ! -d "$hostfs_dir" ] || ! command -v arm-linux-gnueabi-as &>/dev/null; then
		local why hint=""
		if [ ! -d "$hostfs_dir" ]; then
			why="$hostfs_dir not found"
		else
			why="arm-linux-gnueabi-as not found"
			hint="./setup-build-env.sh --podules"
		fi
		if [ "$PODULES_EXPLICIT" = true ]; then
			echo "Error: $why."
			[ -n "$hint" ] && echo "Install the ARM tools with: $hint"
			exit 1
		fi
		echo "Guest ROMs: skipped ($why) - using the committed ones."
		[ -n "$hint" ] && echo "  To rebuild them: $hint"
		echo ""
		return 0
	fi
	echo "Building HostFS podule ROMs..."
	(
		cd "$hostfs_dir"
		make clean
		make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
		cp -f hostfs,ffa hostfsfiler,ffa "$SCRIPT_DIR/poduleroms/"
	)

	# SharedClipboard is not built here: it is C, built inside the emulator with
	# the RISC OS DDE and OSLib, and committed as poduleroms/sharedclipboard,ffa.
	# See riscos-progs/SharedClipboard/README.md.

	local support_dir="riscos-progs/RPCEmuSupport"
	if [ -d "$support_dir" ]; then
		echo "Building RPCEmuSupport podule ROM..."
		(
			cd "$support_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f rpcemusupport,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi

	# SyncClock is DEEJ Technology's module, translated from their BBC BASIC
	# assembler source so it builds here. It re-reads the emulated RTC every ten
	# seconds, which is what puts the guest clock right again after a suspend or
	# a snapshot resumed later.
	local syncclock_dir="riscos-progs/SyncClock"
	if [ -d "$syncclock_dir" ]; then
		echo "Building SyncClock podule ROM..."
		(
			cd "$syncclock_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f syncclock,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi
	# RPCEmuMonitor carries the monitor definition the RISC OS versions without
	# EDID support need. Its mode list is generated from src/display_mode.c by
	# the module's own Makefile, so it has to be rebuilt whenever that table
	# changes or the definition will describe a different set of modes from the
	# one the emulator offers.
	local rpcemumonitor_dir="riscos-progs/RPCEmuMonitor"
	if [ -d "$rpcemumonitor_dir" ]; then
		echo "Building RPCEmuMonitor podule ROM..."
		(
			cd "$rpcemumonitor_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f rpcemumonitor,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi
	# RPCEmuUSBSupport is the harness for the emulated USB host controller. Nothing
	# in a stock guest touches that card, there being no USB stack in the IOMD ROM,
	# so this module is how the emulation is exercised. See docs/usb.md.
	local usb_dir="riscos-progs/RPCEmuUSBSupport"
	if [ -d "$usb_dir" ]; then
		echo "Building RPCEmuUSBSupport podule ROM..."
		(
			cd "$usb_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f rpcemuusbsupport,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi
	# RPCEmuPCIEmulator supplies the PCI SWIs that RISC OS's USB stack uses to
	# get DMA-capable memory. It has to be in poduleroms/, not usbroms/: the
	# support card's modules start before the USB card's, and OHCIDriver needs
	# these SWIs during its own initialisation. See docs/usb.md.
	local pci_dir="riscos-progs/RPCEmuPCIEmulator"
	if [ -d "$pci_dir" ]; then
		echo "Building RPCEmuPCIEmulator podule ROM..."
		(
			cd "$pci_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f rpcemupciemulator,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi
	echo "✓ Podule ROMs copied to poduleroms/"

	# EtherRPCEm is the guest half of the emulated network card. It goes in
	# netroms/, not poduleroms/: the emulator builds that card's ROM around it
	# at run time (src/network.c) and loads it by name, while poduleroms/ is
	# scanned into the general-purpose expansion card. Until this module was
	# converted to assembler it could only be built with the Acorn DDE inside a
	# running guest, so build.sh could not touch it at all.
	local ether_dir="riscos-progs/EtherRPCEm"
	if [ -d "$ether_dir" ]; then
		echo "Building EtherRPCEm network driver..."
		(
			cd "$ether_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f "EtherRPCEm,ffa" "$SCRIPT_DIR/netroms/"
		)
		echo "✓ Network driver copied to netroms/"
	fi

	# The graphics card's display driver goes in gfxroms/, NOT poduleroms/: it
	# is carried in that card's own ROM, and poduleroms/ is scanned into the
	# general-purpose expansion card, which would present the module twice.
	local gfx_dir="riscos-progs/RPCEmuGfx"
	if [ -d "$gfx_dir" ]; then
		echo "Building RPCEmuGfx display driver..."
		mkdir -p gfxroms
		(
			cd "$gfx_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f "RPCEmuGfx,ffa" "$SCRIPT_DIR/gfxroms/"
		)
		echo "✓ Display driver copied to gfxroms/"
	fi
}

build_linux() {
	local binary_name
	binary_name="$(binary_basename)"

	if ! command -v cmake &>/dev/null; then
		echo "Error: cmake not found. Run ./setup-build-env.sh first."
		exit 1
	fi

	echo "Building RPCEmu $VERSION for Linux ($LINUX_ARCH)..."
	echo "  Target: $binary_name"
	if [ -n "${GHOSTPDL_PREFIX:-}" ]; then
		echo "  GhostPDL: $GHOSTPDL_PREFIX"
		export GHOSTPDL_PREFIX
	fi
	echo ""

	rm -rf "$BUILD_DIR"
	mkdir -p "$LINUX_RELEASE"

	local cmake_args=(-S . -B "$BUILD_DIR")
	mapfile -t common_args < <(cmake_common_args)
	cmake_args+=("${common_args[@]}")

	if [ -n "$EXTRA_BINARY" ]; then
		cmake_args+=(-DRPCEMU_EXTRA_BINARY="$EXTRA_BINARY")
	fi

	if [ "$LINUX_CROSS" = true ]; then
		if ! command -v aarch64-linux-gnu-gcc &>/dev/null; then
			echo "Error: aarch64-linux-gnu-gcc not found."
			echo "Install with: ./setup-build-env.sh --cross-arm64"
			exit 1
		fi
		cmake_args+=(-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-gnu.cmake)
		export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
		export PKG_CONFIG_LIBDIR="/usr/lib/aarch64-linux-gnu/pkgconfig"
		export PKG_CONFIG_SYSROOT_DIR="/usr/aarch64-linux-gnu"
		echo "Note: cross-compiling for arm64 (requires multiarch dev packages)."
	fi

	cmake "${cmake_args[@]}"
	cmake --build "$BUILD_DIR" -j"$NPROC"

	if [ "$LINUX_CROSS" = false ]; then
		echo ""
		# Whether the tests can run here is decided above; whether they passed is
		# decided by the script, which treats a suite that is absent or the wrong
		# size as a failure rather than as nothing to do. See docs/testing.md.
		bash "$SCRIPT_DIR/tests/run-ctest.sh" "$BUILD_DIR"
	else
		echo "Note: skipping tests (cross-compiled binaries cannot run on this host)."
	fi

	stage_linux_release "$binary_name"

	echo "✓ Linux build complete ($LINUX_ARCH)"
	echo "  Binary:  $LINUX_RELEASE/$binary_name"

	if [ "$BUILD_ZIP" = true ]; then
		echo ""
		create_linux_tarball
	fi

	if [ "$BUILD_DEB" = true ]; then
		echo ""
		echo "Creating .deb package ($LINUX_ARCH)..."
		(
			cd "$BUILD_DIR"
			cpack -G DEB > /dev/null 2>&1
		)
		shopt -s nullglob
		local debs=("$BUILD_DIR"/*.deb)
		shopt -u nullglob
		if [ ${#debs[@]} -eq 0 ]; then
			echo "Error: cpack did not produce a .deb file"
			exit 1
		fi
		cp "${debs[@]}" "$LINUX_RELEASE/"
		echo "✓ Debian package created"
		echo "  Package: $LINUX_RELEASE/$(basename "${debs[0]}")"
	fi
}

echo "=================================================="
echo "RPCEmu Build  v$VERSION (Linux)"
echo "=================================================="
echo ""
echo "Linux arch: $LINUX_ARCH$([ "$LINUX_CROSS" = true ] && echo " (cross-compiled)" || echo " (native)")"
echo ""

if [ "$BUILD_PODULES" = true ]; then
	build_podules
fi

mkdir -p "$LINUX_RELEASE"

if [ "$BUILD_BOTH" = true ]; then
	# ★ Two passes, and the order matters.
	#
	# RPCEMU_DYNAREC is decided at configure time, so one configure builds one
	# emulator. The interpreter goes first, into a build tree of its own, and
	# stages its binary; the recompiler then builds second, so that everything
	# written once and overwritten by the later pass - the .desktop Exec line,
	# BUILDINFO.txt, the .deb - describes the recompiler, which is the one a
	# user should reach for. The interpreter is handed to the second configure
	# as RPCEMU_EXTRA_BINARY so it is inside the .deb as well as the tarball.
	#
	# Two full compiles, with no compiler cache in CI, so this roughly doubles
	# the build. That is the price of shipping both and it is paid on purpose.
	want_zip=$BUILD_ZIP
	want_deb=$BUILD_DEB

	BUILD_DIR=build-interpreter
	BUILD_INTERPRETER=true
	BUILD_ZIP=false
	BUILD_DEB=false
	build_linux

	# Staged, so it is beside the recompiler in the archive already; named here
	# so the second configure can put it in the .deb too.
	EXTRA_BINARY="$PWD/$LINUX_RELEASE/$(binary_basename)"

	BUILD_DIR=build
	BUILD_INTERPRETER=false
	BUILD_ZIP=$want_zip
	BUILD_DEB=$want_deb
	build_linux
else
	build_linux
fi

echo ""
echo "=================================================="
echo "Build complete!"
echo "=================================================="
echo ""
ls -la "$LINUX_RELEASE/"
