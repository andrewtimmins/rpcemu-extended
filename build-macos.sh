#!/usr/bin/env bash
#
# Build RPCEmu (Spork Edition) for macOS as a UNIVERSAL binary and stage a
# runnable release into releases/macos/universal/ - parity with build.sh's
# releases/linux/<arch>/ and build-windows.sh's releases/windows/amd64/ layout.
#
# Why two slices instead of one -arch arm64 -arch x86_64 build:
#   The dynarec (codegen_amd64.c) emits x86-64 machine code, so it can only be
#   compiled into the x86_64 slice. The arm64 slice therefore uses the
#   interpreter (RPCEMU_DYNAREC=OFF), exactly as the Linux arm64 build does.
#   The universal binary = x86_64(dynarec) + arm64(interpreter), fused by lipo.
#   On Apple Silicon the x86_64 slice can also run (fast) under Rosetta 2.
#
# Two toolchain modes, auto-detected:
#   * Native on macOS (uname = Darwin). Apple clang cross-compiles between its
#     own two arches (the SDK is universal), so ONE mac can build either slice.
#     Dependencies via Homebrew (per-arch bottles). This is what CI runs.
#   * Cross-compile from Linux via osxcross. Prerequisite: run
#     ./setup-macos-cross-build-env.sh once (needs a user-provided macOS SDK)
#     and cross-build wxWidgets/SDL2/libvncserver for each arch. UNTESTED path
#     (a Linux host cannot execute the resulting Mach-O), for iteration only.
#
# Usage:
#   ./build-macos.sh                 # build both slices + fuse + stage (local)
#   ./build-macos.sh --arch x86_64   # build just the x86_64 (dynarec) slice
#   ./build-macos.sh --arch arm64    # build just the arm64 (interpreter) slice
#   ./build-macos.sh --fuse          # lipo already-built slices + stage release
#   ./build-macos.sh --zip           # also create releases/macos/*.tar.gz
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

MAC_RELEASE="releases/macos/universal"
MAKE_ZIP=false
DO_BUILD=true
DO_FUSE=true
ONE_ARCH=""

for arg in "$@"; do
	case "$arg" in
		--zip|-z) MAKE_ZIP=true ;;
		--arch) : ;;                       # value handled below
		x86_64|arm64) ONE_ARCH="$arg"; DO_FUSE=false ;;
		--fuse) DO_BUILD=false; DO_FUSE=true ;;
		--help|-h) echo "Usage: $0 [--arch x86_64|arm64] [--fuse] [--zip]"; exit 0 ;;
		*) echo "unknown option: $arg"; exit 2 ;;
	esac
done

get_version() { [ -f VERSION ] && tr -d ' \t\r\n' < VERSION || echo "0.0.0"; }
VERSION=$(get_version)
njobs() { sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4; }

# Toolchain mode.
if [ "$(uname -s)" = "Darwin" ]; then
	MODE=native
else
	MODE=cross
fi

# Per-arch build knobs. The x86_64 slice is the recompiler and carries the
# unit tests (they exercise the x86 JIT); the arm64 slice is the interpreter.
slice_binname() { [ "$1" = "x86_64" ] && echo rpcemu-recompiler || echo rpcemu-interpreter; }
slice_dynarec() { [ "$1" = "x86_64" ] && echo ON || echo OFF; }
slice_tests()   { [ "$1" = "x86_64" ] && echo ON || echo OFF; }
slice_deploy()  { [ "$1" = "x86_64" ] && echo 10.15 || echo 11.0; }

build_slice() {
	local arch="$1"
	local build_dir="build-mac-$arch"
	local dyn tests deploy
	dyn=$(slice_dynarec "$arch"); tests=$(slice_tests "$arch")
	deploy=$(slice_deploy "$arch")

	local gen; command -v ninja >/dev/null && gen=Ninja || gen="Unix Makefiles"
	local -a tc_args=()

	local -a extra_args=()
	if [ "$MODE" = native ]; then
		tc_args+=(-DCMAKE_OSX_ARCHITECTURES="$arch"
		          -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploy")
	else
		# osxcross: a per-arch toolchain file selects the clang wrapper + sysroot,
		# and points CMake at the cross-built wxWidgets wx-config for this arch.
		# The cross path only builds wxWidgets + SDL2 (VNC/GhostPDL need extra
		# cross-built libs and are dropped here; the native CI build keeps them).
		local tc="$SCRIPT_DIR/cmake/osxcross-$arch.cmake"
		[ -f "$tc" ] || { echo "error: $tc not found. Run ./setup-macos-cross-build-env.sh"; exit 1; }
		tc_args+=(-DCMAKE_TOOLCHAIN_FILE="$tc")
		extra_args+=(-DRPCEMU_ENABLE_VNC=OFF)
	fi

	echo "==> [$arch] configuring ($MODE, dynarec=$dyn, tests=$tests)"
	cmake -B "$build_dir" -G "$gen" \
		"${tc_args[@]}" "${extra_args[@]}" \
		-DCMAKE_BUILD_TYPE=Release \
		-DRPCEMU_DYNAREC="$dyn" \
		-DRPCEMU_BUILD_TESTS="$tests" \
		-DRPCEMU_ENABLE_GHOSTPDL=OFF
	echo "==> [$arch] building"
	cmake --build "$build_dir" -j"$(njobs)"

	# Run the JIT unit test where we can (native host of a matching arch).
	if [ "$tests" = ON ] && [ "$MODE" = native ]; then
		if [ "$(uname -m)" = "$arch" ] || [ "$arch" = x86_64 ]; then
			echo "==> [$arch] ctest"
			( cd "$build_dir" && ctest --output-on-failure ) || \
				echo "!! [$arch] ctest failed or could not run (arch mismatch / no Rosetta)"
		fi
	fi
}

if [ "$DO_BUILD" = true ]; then
	if [ -n "$ONE_ARCH" ]; then
		build_slice "$ONE_ARCH"
	else
		build_slice x86_64
		build_slice arm64
	fi
fi

# Fuse + stage only when we have (or expect) both slices.
if [ "$DO_FUSE" = true ]; then
	X86_DIR=build-mac-x86_64
	ARM_DIR=build-mac-arm64
	x86_bin="$X86_DIR/bin/$(slice_binname x86_64)"
	arm_bin="$ARM_DIR/bin/$(slice_binname arm64)"
	for f in "$x86_bin" "$arm_bin"; do
		[ -f "$f" ] || { echo "error: missing slice '$f' (build it first, or use CI per-arch jobs)"; exit 1; }
	done

	LIPO=$(command -v lipo || command -v x86_64-apple-darwin*-lipo || true)
	[ -n "$LIPO" ] || { echo "error: lipo not found (need Apple cctools or osxcross)"; exit 1; }

	echo "==> Staging $MAC_RELEASE"
	rm -rf "$MAC_RELEASE"
	mkdir -p "$MAC_RELEASE"
	for d in configs poduleroms netroms resources roms podules default; do
		cp -a "$d" "$MAC_RELEASE/"
	done
	mkdir -p "$MAC_RELEASE/shared"
	mkdir -p "$MAC_RELEASE/machines/Default"
	if [ -d machines/Default ]; then
		cp -a machines/Default/. "$MAC_RELEASE/machines/Default/"
	fi
	# Seed missing machine files from the default/ seed (fresh clone / CI).
	[ -f default/cmos.ram ] && [ ! -f "$MAC_RELEASE/machines/Default/cmos.ram" ] && \
		cp -a default/cmos.ram "$MAC_RELEASE/machines/Default/"
	[ -d default/hostfs ] && [ ! -d "$MAC_RELEASE/machines/Default/hostfs" ] && \
		cp -a default/hostfs "$MAC_RELEASE/machines/Default/"
	cp -f COPYING README.md COMPILE.md "$MAC_RELEASE/" 2>/dev/null || true

	# Fuse the emulator: x86_64(dynarec) + arm64(interpreter) -> universal.
	echo "==> lipo universal emulator binary"
	"$LIPO" -create "$x86_bin" "$arm_bin" -output "$MAC_RELEASE/rpcemu"
	chmod +x "$MAC_RELEASE/rpcemu"

	# Fuse the HostCmd host client if both slices built it.
	if [ -f "$X86_DIR/bin/rpcemu-run" ] && [ -f "$ARM_DIR/bin/rpcemu-run" ]; then
		"$LIPO" -create "$X86_DIR/bin/rpcemu-run" "$ARM_DIR/bin/rpcemu-run" \
			-output "$MAC_RELEASE/rpcemu-run"
		chmod +x "$MAC_RELEASE/rpcemu-run"
		ln -sf rpcemu-run "$MAC_RELEASE/rpcemu-shell"
	fi

	# MCP server + docs (same set as the Linux/Windows releases).
	if [ -d tools/mcp ]; then
		mkdir -p "$MAC_RELEASE/tools/mcp"
		cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
		      tools/mcp/README.md tools/mcp/mcp.json.example \
		      "$MAC_RELEASE/tools/mcp/" 2>/dev/null || true
	fi
	[ -d docs ] && cp -a docs "$MAC_RELEASE/" 2>/dev/null || true

	cat > "$MAC_RELEASE/BUILDINFO.txt" <<EOF
RPCEmu (Spork Edition) $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m) ($MODE)
Binary: rpcemu (universal: x86_64 recompiler + arm64 interpreter)
Toolkit: wxWidgets (wxOSX/Cocoa) + CMake
EOF

	echo "==> Universal binary architectures:"
	"$LIPO" -archs "$MAC_RELEASE/rpcemu" 2>/dev/null || true
	echo "✓ Staged: $MAC_RELEASE"

	if [ "$MAKE_ZIP" = true ]; then
		ARCHIVE="rpcemu_${VERSION}_macos_universal.tar.gz"
		echo "==> Packaging releases/macos/$ARCHIVE"
		( cd "$MAC_RELEASE" && tar czf "../$ARCHIVE" . )
		echo "✓ macOS archive: releases/macos/$ARCHIVE"
	fi
fi
