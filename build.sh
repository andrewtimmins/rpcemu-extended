#!/bin/bash
# RPCEmu (Spork Edition) - Linux build script
#
# Usage:
#   ./build.sh                         # Linux release build (dynarec) for host arch
#   ./build.sh --arch arm64            # Linux arm64 (native on Pi, cross from x86)
#   ./build.sh --deb                   # Linux + .deb for selected arch
#   ./build.sh --zip                   # Linux build + .tar.gz in releases/linux/
#   ./build.sh --interpreter           # Interpreter build (no dynarec)
#   ./build.sh --debug                 # Debug build (-debug suffix on binary name)
#   ./build.sh --podules               # Rebuild HostFS podule ROMs (optional)
#   ./build.sh --clean                 # Remove build directories and releases
#
# Environment:
#   GHOSTPDL_PREFIX=/opt/ghostpdl      # Optional full GhostPDL for PCL print jobs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

get_version() {
	if [ -f VERSION ]; then
		tr -d ' \t\r\n' < VERSION
		return
	fi
	echo "0.0.0"
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
BUILD_DEBUG=false
BUILD_PODULES=false
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
		--debug|-g) BUILD_DEBUG=true ;;
		--deb|-d) BUILD_DEB=true ;;
		--zip|-z) BUILD_ZIP=true ;;
		--cross-arm64) FORCE_CROSS_ARM64=true; LINUX_ARCH=arm64 ;;
		--podules|-p) BUILD_PODULES=true ;;
		--clean|-c) CLEAN_ONLY=true ;;
		--help|-h)
			echo "Usage: $0 [options]"
			echo ""
			echo "  --arch ARCH         Linux target: amd64 or arm64 (default: host)"
			echo "  --cross-arm64       Cross-compile Linux arm64 from x86_64"
			echo "  --interpreter, -i   Build interpreter instead of dynarec"
			echo "  --debug, -g         Debug build"
			echo "  --deb, -d           Create .deb package"
			echo "  --zip, -z           Create .tar.gz in releases/linux/"
			echo "  --podules, -p       Rebuild HostFS podule ROMs"
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
	echo -DCMAKE_INSTALL_PREFIX=/usr
}

stage_linux_release() {
	local binary_name="$1"
	local release_binary="$LINUX_RELEASE/$binary_name"

	mkdir -p "$LINUX_RELEASE"
	cp -a configs "$LINUX_RELEASE/"
	cp -a poduleroms "$LINUX_RELEASE/"
	cp -a netroms "$LINUX_RELEASE/"
	cp -a resources "$LINUX_RELEASE/"
	cp -a roms "$LINUX_RELEASE/"
	cp -a podules "$LINUX_RELEASE/"
	cp -a default "$LINUX_RELEASE/"
	# Common HostFS "Shared" disc (shared across machines). Normally created on
	# first launch by the emulator; pre-create it so a fresh release is complete.
	mkdir -p "$LINUX_RELEASE/shared"
	rm -rf "$LINUX_RELEASE/machines/Default"
	mkdir -p "$LINUX_RELEASE/machines/Default"
	if [ -d machines/Default ]; then
		cp -a machines/Default/. "$LINUX_RELEASE/machines/Default/"
	fi
	# Seed any machine files that aren't already present from the default/ seed.
	# Only cmos.ram is tracked under machines/Default/; the HostFS contents
	# (e.g. HardDisc4.5.30.util, the first-boot hard-disc installer) live in
	# default/hostfs/. On a fresh clone / CI the copy above yields only cmos.ram,
	# so hostfs/ must be seeded here or the shipped machine has no HostFS.
	[ -f default/cmos.ram ] && [ ! -f "$LINUX_RELEASE/machines/Default/cmos.ram" ] && \
		cp -a default/cmos.ram "$LINUX_RELEASE/machines/Default/"
	[ -d default/hostfs ] && [ ! -d "$LINUX_RELEASE/machines/Default/hostfs" ] && \
		cp -a default/hostfs "$LINUX_RELEASE/machines/Default/"
	cp -f COPYING README.md COMPILE.md "$LINUX_RELEASE/" 2>/dev/null || true
	cp -f setup-runtime-env.sh "$LINUX_RELEASE/" 2>/dev/null || true
	if [ -f packaging/rpcemu.desktop ]; then
		cp -f packaging/rpcemu.desktop "$LINUX_RELEASE/"
		# Point the launcher at the actual binary for this build (recompiler or
		# interpreter); packaging/rpcemu.desktop uses an @RPCEMU_GUI_TARGET@ token.
		sed -i "s/@RPCEMU_GUI_TARGET@/$binary_name/" "$LINUX_RELEASE/rpcemu.desktop"
	fi

	cp -f "build/bin/$binary_name" "$release_binary"
	chmod +x "$release_binary"
	cp -f "$release_binary" "$binary_name"

	# HostCmd host-side client (rpcemu-run + rpcemu-shell symlink). These let
	# the host drive the guest RISC OS command line; ship them alongside the
	# emulator binary. See docs/hostcmd.md.
	if [ -f build/bin/rpcemu-run ]; then
		cp -f build/bin/rpcemu-run "$LINUX_RELEASE/rpcemu-run"
		chmod +x "$LINUX_RELEASE/rpcemu-run"
		ln -sf rpcemu-run "$LINUX_RELEASE/rpcemu-shell"
	fi

	# MCP server: drive a RISC OS machine (HostCmd + HostFS + VNC + the debugger
	# control socket) from an MCP client. Python; ships with requirements.txt +
	# README + config example. See tools/mcp/README.md and docs/debugcmd.md.
	if [ -d tools/mcp ]; then
		mkdir -p "$LINUX_RELEASE/tools/mcp"
		cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
		      tools/mcp/README.md tools/mcp/mcp.json.example \
		      "$LINUX_RELEASE/tools/mcp/" 2>/dev/null || true
	fi

	# Ship the full docs/ set so the README and MCP/HostCmd/debugger docs resolve.
	[ -d docs ] && cp -a docs "$LINUX_RELEASE/" 2>/dev/null || true

	cat > "$LINUX_RELEASE/BUILDINFO.txt" <<EOF
RPCEmu (Spork Edition) $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m)
Binary: $binary_name
Toolkit: wxWidgets + CMake (Linux)
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
	if [ ! -d "$hostfs_dir" ]; then
		echo "Error: $hostfs_dir not found"
		exit 1
	fi
	if ! command -v arm-linux-gnueabi-as &>/dev/null; then
		echo "Error: arm-linux-gnueabi-as not found."
		echo "Install with: ./setup-build-env.sh --podules"
		exit 1
	fi
	echo "Building HostFS podule ROMs..."
	(
		cd "$hostfs_dir"
		make clean
		make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
		cp -f hostfs,ffa hostfsfiler,ffa "$SCRIPT_DIR/poduleroms/"
	)

	local hostcmd_dir="riscos-progs/HostCmd"
	if [ -d "$hostcmd_dir" ]; then
		echo "Building HostCmd podule ROM..."
		(
			cd "$hostcmd_dir"
			make clean
			make AS=arm-linux-gnueabi-as LD=arm-linux-gnueabi-ld OBJCOPY=arm-linux-gnueabi-objcopy
			cp -f hostcmd,ffa "$SCRIPT_DIR/poduleroms/"
		)
	fi
	echo "✓ Podule ROMs copied to poduleroms/"
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

	rm -rf build
	mkdir -p "$LINUX_RELEASE"

	local cmake_args=(-S . -B build)
	mapfile -t common_args < <(cmake_common_args)
	cmake_args+=("${common_args[@]}")

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
	cmake --build build -j"$NPROC"

	if [ "$LINUX_CROSS" = false ]; then
		if [ -f build/CTestTestfile.cmake ]; then
			echo ""
			echo "Running tests..."
			(cd build && ctest --output-on-failure)
		fi
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
			cd build
			cpack -G DEB > /dev/null 2>&1
		)
		shopt -s nullglob
		local debs=(build/*.deb)
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
build_linux

echo ""
echo "=================================================="
echo "Build complete!"
echo "=================================================="
echo ""
ls -la "$LINUX_RELEASE/"
