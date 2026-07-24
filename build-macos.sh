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

# Reverse-DNS bundle identifier. Change BUNDLE_ID to a domain you control before
# distributing through the App Store or with a Developer ID; it is cosmetic for
# an ad-hoc signed build.
BUNDLE_ID="com.github.andrewtimmins.rpcemu"

write_info_plist() {
	cat > "$1" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>RPCEmu</string>
	<key>CFBundleDisplayName</key><string>RPCEmu</string>
	<key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
	<key>CFBundleExecutable</key><string>rpcemu</string>
	<key>CFBundleIconFile</key><string>rpcemu</string>
	<key>CFBundleShortVersionString</key><string>${VERSION}</string>
	<key>CFBundleVersion</key><string>${VERSION}</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>LSMinimumSystemVersion</key><string>10.15</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
	<key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
	<key>NSHumanReadableCopyright</key><string>RPCEmu contributors. Licensed under the GNU GPL v2.</string>
</dict>
</plist>
PLIST
}

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
	# Expand possibly-empty arrays safely: macOS ships bash 3.2, where
	# "${arr[@]}" on an empty array under `set -u` is an "unbound variable"
	# error. The ${arr[@]+...} guard expands to nothing when empty.
	cmake -B "$build_dir" -G "$gen" \
		${tc_args[@]+"${tc_args[@]}"} ${extra_args[@]+"${extra_args[@]}"} \
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

	# Assemble a proper macOS application bundle:
	#   RPCEmu.app/Contents/MacOS/rpcemu      (universal emulator + CLI helpers)
	#   RPCEmu.app/Contents/Resources/...     (read-only payload + rpcemu.icns)
	#   RPCEmu.app/Contents/Info.plist
	# Writable data (machines, configs, ROMs, hostfs, logs) is NOT kept inside
	# the bundle - InitRpcemuPaths() reads Contents/Resources and seeds ~/RPCEmu
	# on first run, so an app dragged into /Applications stays read-only.
	APP="releases/macos/RPCEmu.app"
	CONTENTS="$APP/Contents"
	MACOSD="$CONTENTS/MacOS"
	RESD="$CONTENTS/Resources"

	echo "==> Assembling $APP"
	rm -rf "$APP"
	mkdir -p "$MACOSD" "$RESD"

	for d in configs poduleroms netroms resources roms podules default; do
		[ -e "$d" ] && cp -a "$d" "$RESD/"
	done
	mkdir -p "$RESD/machines/Default"
	if [ -d machines/Default ]; then
		cp -a machines/Default/. "$RESD/machines/Default/"
	fi
	# Seed missing machine files from the default/ seed (fresh clone / CI).
	[ -f default/cmos.ram ] && [ ! -f "$RESD/machines/Default/cmos.ram" ] && \
		cp -a default/cmos.ram "$RESD/machines/Default/"
	[ -d default/hostfs ] && [ ! -d "$RESD/machines/Default/hostfs" ] && \
		cp -a default/hostfs "$RESD/machines/Default/"
	cp -f COPYING README.md COMPILE.md "$RESD/" 2>/dev/null || true
	[ -d docs ] && cp -a docs "$RESD/" 2>/dev/null || true

	# MCP server (same set as the Linux/Windows releases).
	if [ -d tools/mcp ]; then
		mkdir -p "$RESD/tools/mcp"
		cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
		      tools/mcp/README.md tools/mcp/mcp.json.example \
		      "$RESD/tools/mcp/" 2>/dev/null || true
	fi

	# Fuse the emulator: x86_64(dynarec) + arm64(interpreter) -> universal.
	echo "==> lipo universal emulator binary"
	"$LIPO" -create "$x86_bin" "$arm_bin" -output "$MACOSD/rpcemu"
	chmod +x "$MACOSD/rpcemu"

	# Fuse the HostCmd host client if both slices built it.
	if [ -f "$X86_DIR/bin/rpcemu-run" ] && [ -f "$ARM_DIR/bin/rpcemu-run" ]; then
		"$LIPO" -create "$X86_DIR/bin/rpcemu-run" "$ARM_DIR/bin/rpcemu-run" \
			-output "$MACOSD/rpcemu-run"
		chmod +x "$MACOSD/rpcemu-run"
		ln -sf rpcemu-run "$MACOSD/rpcemu-shell"
	fi

	write_info_plist "$CONTENTS/Info.plist"

	# App icon: build rpcemu.icns from resources/rpcemu.png. Needs macOS sips +
	# iconutil, so skipped on the osxcross path (the app then uses a generic
	# icon). Source is 256x256, so the 512/1024 variants are upscaled - swap in a
	# 1024x1024 master for crisp Retina rendering.
	if [ "$(uname -s)" = Darwin ] && command -v sips >/dev/null 2>&1 && \
	   command -v iconutil >/dev/null 2>&1 && [ -f resources/rpcemu.png ]; then
		echo "==> building app icon (rpcemu.icns)"
		ICONROOT=$(mktemp -d)
		ICONSET="$ICONROOT/rpcemu.iconset"
		mkdir -p "$ICONSET"
		for sz in 16 32 128 256 512; do
			sips -z "$sz" "$sz" resources/rpcemu.png \
				--out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null 2>&1
			sips -z "$((sz * 2))" "$((sz * 2))" resources/rpcemu.png \
				--out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null 2>&1
		done
		iconutil -c icns "$ICONSET" -o "$RESD/rpcemu.icns"
		rm -rf "$ICONROOT"
	else
		echo "!! no macOS icon tooling - bundle will use a generic icon"
	fi

	cat > "$RESD/BUILDINFO.txt" <<EOF
RPCEmu (Spork Edition) $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m) ($MODE)
Binary: rpcemu (universal: x86_64 recompiler + arm64 interpreter)
Toolkit: wxWidgets (wxOSX/Cocoa) + CMake
EOF

	# Code signing. Apple Silicon refuses to run an unsigned binary at all, so an
	# ad-hoc signature (-s -) is the minimum for the app to launch. Without an
	# Apple Developer ID we cannot notarise, so Gatekeeper still warns on first
	# open (right-click > Open once); this is documented in the README.
	# RPCEMU_MACOS_CODESIGN=1 additionally applies the hardened runtime + JIT
	# entitlement, needed only for a future recompiler arm64 slice (MAP_JIT).
	if [ "$(uname -s)" = Darwin ] && command -v codesign >/dev/null 2>&1; then
		if [ "${RPCEMU_MACOS_CODESIGN:-0}" = 1 ] && [ -f resources/rpcemu-jit.entitlements ]; then
			echo "==> codesign (ad-hoc, hardened runtime + JIT entitlement)"
			codesign --force --deep --options runtime \
				--entitlements resources/rpcemu-jit.entitlements \
				-s - "$APP"
		else
			echo "==> codesign (ad-hoc)"
			codesign --force --deep -s - "$APP"
		fi
		codesign --verify --deep --strict "$APP" 2>/dev/null && echo "✓ signature verified"
	fi

	echo "==> Universal binary architectures:"
	"$LIPO" -archs "$MACOSD/rpcemu" 2>/dev/null || true
	echo "✓ Bundle: $APP"

	# Disk image with a drag-to-Applications shortcut - the format Mac users
	# expect. macOS only (hdiutil); the osxcross path stops at the .app.
	if [ "$(uname -s)" = Darwin ] && command -v hdiutil >/dev/null 2>&1; then
		DMG="releases/macos/rpcemu_${VERSION}_macos_universal.dmg"
		echo "==> Packaging $DMG"
		DMGSTAGE=$(mktemp -d)
		cp -a "$APP" "$DMGSTAGE/"
		ln -s /Applications "$DMGSTAGE/Applications"
		rm -f "$DMG"
		hdiutil create -volname "RPCEmu $VERSION" -srcfolder "$DMGSTAGE" \
			-fs HFS+ -format UDZO -ov "$DMG" >/dev/null
		rm -rf "$DMGSTAGE"
		echo "✓ macOS DMG: $DMG"
	fi

	# Portable archive of the bundle (optional; the DMG is the primary artifact).
	if [ "$MAKE_ZIP" = true ]; then
		ARCHIVE="rpcemu_${VERSION}_macos_universal.tar.gz"
		echo "==> Packaging releases/macos/$ARCHIVE"
		( cd releases/macos && tar czf "$ARCHIVE" RPCEmu.app )
		echo "✓ macOS archive: releases/macos/$ARCHIVE"
	fi
fi
