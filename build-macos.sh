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

# Per-arch build knobs. The x86_64 slice is the recompiler, so it is the one that
# exercises the JIT differential tests; the arm64 slice is the interpreter.
slice_binname() { [ "$1" = "x86_64" ] && echo rpcemu-recompiler || echo rpcemu-interpreter; }
slice_dynarec() { [ "$1" = "x86_64" ] && echo ON || echo OFF; }
# Tests are built for both slices. The JIT differential tests need a native
# dynarec backend and CMake skips them by itself when RPCEMU_DYNAREC is off, so
# on the arm64 (interpreter) slice this builds the architecture-neutral tests -
# which is better than the slice having no test coverage at all.
slice_tests()   { echo ON; }
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

	# Run the unit tests where we can: a native build, of an architecture this
	# machine can actually execute. Decide that up front so a real failure can be
	# fatal. Previously the result was piped into an "or echo", which meant a
	# genuine test failure was indistinguishable from "could not run here" and
	# macOS could go green with tests failing.
	if [ "$tests" = ON ] && [ "$MODE" = native ]; then
		local can_run=false

		if [ "$(uname -m)" = "$arch" ]; then
			can_run=true
		elif [ "$arch" = x86_64 ] && arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
			can_run=true	# Apple Silicon host with Rosetta 2 installed
		fi

		if [ "$can_run" = true ]; then
			echo "==> [$arch] ctest"
			( cd "$build_dir" && ctest --output-on-failure )
		else
			echo "Note: [$arch] skipping tests - cannot execute $arch binaries on $(uname -m) (Rosetta 2 not available)."
		fi
	fi

	# Stage this slice and its dependencies now, while we are on the machine that
	# has this architecture's libraries installed. The CI job that fuses the
	# slices installs no dependencies of its own, so it relies on this.
	stage_slice "$arch"
}

# Discovered before any staging: stage_slice() uses these to walk and rewrite
# dependencies. Absent on an osxcross host, where the app cannot be run anyway,
# so bundling is skipped there rather than treated as fatal.
OTOOL=$(command -v otool || command -v x86_64-apple-darwin*-otool || true)
INSTALL_NAME_TOOL=$(command -v install_name_tool || \
                    command -v x86_64-apple-darwin*-install_name_tool || true)

#
# Dependency bundling.
#
# The slices link against Homebrew, so the emulator records absolute paths such
# as /opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib. Those exist only on a
# machine with the same formulae installed, so an app built this way aborts on
# launch anywhere else:
#
#   dyld: Library not loaded: /opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib
#
# Copy each non-system dependency into Contents/Frameworks and repoint every
# reference at it, which is the macOS counterpart of the DLL bundling that
# build-windows.sh does.
#
# The complication is that this is a universal binary fused from two slices built
# against different prefixes (/usr/local for x86_64, /opt/homebrew for arm64), so
# there is no single old path to rewrite and no single-architecture library to
# copy. Each slice is therefore staged and rewritten while it is still thin, with
# its own prefix, and the collected libraries are made universal with lipo
# afterwards. Signing has to follow all of this, since editing a load command
# invalidates a signature.
#
# Note: no associative arrays or other bash 4 features here, because macOS still
# ships bash 3.2.

# Turn a dependency recorded through a runtime search path into a real file, so
# there is something to copy into the bundle. @loader_path is relative to the
# object holding the reference; @rpath is searched through that object's LC_RPATH
# entries, which Homebrew sets to @loader_path/../lib.
#
# These have to be followed rather than skipped. Homebrew's webp libraries refer
# to each other this way, and leaving them out produced an app that still would
# not start, with dyld looking for Contents/Frameworks/../lib.
resolve_dep() {
	local obj="$1" dep="$2" origin="$3" entry candidate

	case "$dep" in
	@loader_path/*)
		candidate="$origin/${dep#@loader_path/}"
		if [ -f "$candidate" ]; then
			echo "$candidate"
			return 0
		fi
		;;
	@rpath/*)
		while read -r entry; do
			case "$entry" in
				@loader_path/*)     entry="$origin/${entry#@loader_path/}" ;;
				@executable_path/*) entry="$origin/${entry#@executable_path/}" ;;
			esac
			candidate="$entry/${dep#@rpath/}"
			if [ -f "$candidate" ]; then
				echo "$candidate"
				return 0
			fi
		done < <("$OTOOL" -l "$obj" | \
		         awk '/LC_RPATH/ { rp = 1; next } rp && $1 == "path" { print $2; rp = 0 }')
		;;
	esac

	return 1
}

# Recursively record a thin object's bundle-able dependencies into a map file
# holding "basename<tab>path" lines. "origin" is the directory that
# @loader_path refers to for this object.
collect_deps() {
	local obj="$1" map="$2" origin="$3" dep base resolved

	while read -r dep; do
		case "$dep" in
			/usr/lib/*|/System/*) continue ;;	# provided by the OS
			@executable_path/*) continue ;;		# already pointed into the bundle
		esac

		# Checked before resolving, which also disposes of the object's own id:
		# otool lists that first for a dylib, and by the time we recurse into one
		# its name is already recorded.
		base=${dep##*/}
		if awk -F'\t' -v b="$base" '$1 == b { found = 1 } END { exit !found }' "$map"; then
			continue				# already collected
		fi

		case "$dep" in
			/*) resolved="$dep" ;;
			@rpath/*|@loader_path/*)
				if ! resolved=$(resolve_dep "$obj" "$dep" "$origin"); then
					echo "   ! ${obj##*/} references $dep and it could not be resolved; not bundled"
					continue
				fi
				;;
			*)
				echo "   ! ${obj##*/} references $dep in a form we do not handle; not bundled"
				continue ;;
		esac

		if [ ! -f "$resolved" ]; then
			echo "   ! dependency not found, leaving reference alone: $resolved"
			continue
		fi

		printf '%s\t%s\n' "$base" "$resolved" >> "$map"
		collect_deps "$resolved" "$map" "$(dirname "$resolved")"
	done < <("$OTOOL" -L "$obj" | tail -n +2 | awk '{ print $1 }')
}

# Repoint every bundled dependency of a thin object at Contents/Frameworks.
# Driven by what the object actually records, and matched on basename, so a
# reference through a Cellar path, an opt path or an @rpath is rewritten alike.
rewrite_deps() {
	local obj="$1" libdir="$2" dep base

	"$OTOOL" -L "$obj" | tail -n +2 | awk '{ print $1 }' | while read -r dep; do
		case "$dep" in
			/usr/lib/*|/System/*) continue ;;
			@executable_path/*) continue ;;		# already done
		esac
		base=${dep##*/}
		if [ -f "$libdir/$base" ]; then
			"$INSTALL_NAME_TOOL" -change "$dep" "@executable_path/../Frameworks/$base" "$obj"
		fi
	done
}

# Stage one slice's binaries and their dependencies, rewritten and ready to fuse.
# Leaves build-mac-<arch>/appstage/{bin,libs} populated.
stage_slice() {
	local arch="$1"
	local build_dir="build-mac-$arch"
	local stage="$build_dir/appstage"
	local map="$build_dir/appstage-deps.map"
	local src base obj

	rm -rf "$stage"
	mkdir -p "$stage/bin" "$stage/libs"
	: > "$map"

	# The emulator is renamed to "rpcemu" in the bundle regardless of which
	# variant this slice built.
	cp -f "$build_dir/bin/$(slice_binname "$arch")" "$stage/bin/rpcemu"
	if [ -f "$build_dir/bin/rpcemu-run" ]; then
		cp -f "$build_dir/bin/rpcemu-run" "$stage/bin/rpcemu-run"
	fi
	chmod u+w "$stage/bin/"*

	if [ -z "$OTOOL" ] || [ -z "$INSTALL_NAME_TOOL" ]; then
		echo "Note: [$arch] otool/install_name_tool unavailable, skipping dependency bundling."
		return
	fi

	# The executables' own @loader_path is their directory in the finished bundle,
	# Contents/MacOS, alongside which nothing is installed; their dependencies are
	# recorded as absolute paths in practice.
	for obj in "$stage/bin/"*; do
		collect_deps "$obj" "$map" "$build_dir/bin"
	done

	while IFS=$'\t' read -r base src; do
		[ -n "$base" ] || continue
		cp -f "$src" "$stage/libs/$base"
		chmod u+w "$stage/libs/$base"
		"$INSTALL_NAME_TOOL" -id "@executable_path/../Frameworks/$base" "$stage/libs/$base"
	done < "$map"

	for obj in "$stage/bin/"* "$stage/libs/"*; do
		[ -f "$obj" ] || continue
		rewrite_deps "$obj" "$stage/libs"
	done

	echo "==> [$arch] bundling $(wc -l < "$map" | tr -d ' ') dependencies"
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
	X86_STAGE="$X86_DIR/appstage"
	ARM_STAGE="$ARM_DIR/appstage"

	LIPO=$(command -v lipo || command -v x86_64-apple-darwin*-lipo || true)
	[ -n "$LIPO" ] || { echo "error: lipo not found (need Apple cctools or osxcross)"; exit 1; }

	# Staging normally happens in build_slice, on the machine that has that
	# architecture's libraries installed - which matters because the CI job doing
	# the fuse has no Homebrew dependencies of its own, only the staged slices.
	# Stage here only if it has not been done, for a tree where the slices were
	# built by an earlier invocation.
	for arch in x86_64 arm64; do
		if [ ! -f "build-mac-$arch/appstage/bin/rpcemu" ]; then
			[ -f "build-mac-$arch/bin/$(slice_binname "$arch")" ] || {
				echo "error: no slice for $arch (build it first, or use the CI per-arch jobs)"
				exit 1
			}
			stage_slice "$arch"
		fi
	done

	for f in "$X86_STAGE/bin/rpcemu" "$ARM_STAGE/bin/rpcemu"; do
		[ -f "$f" ] || { echo "error: missing staged slice '$f'"; exit 1; }
	done

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

	for d in configs poduleroms netroms gfxroms resources roms podules default; do
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
	# The staged copies are used, so the rewritten dependency paths are carried
	# through into the bundle.
	echo "==> lipo universal emulator binary"
	"$LIPO" -create "$X86_STAGE/bin/rpcemu" "$ARM_STAGE/bin/rpcemu" -output "$MACOSD/rpcemu"
	chmod +x "$MACOSD/rpcemu"

	# Fuse the HostCmd host client if both slices built it.
	if [ -f "$X86_STAGE/bin/rpcemu-run" ] && [ -f "$ARM_STAGE/bin/rpcemu-run" ]; then
		"$LIPO" -create "$X86_STAGE/bin/rpcemu-run" "$ARM_STAGE/bin/rpcemu-run" \
			-output "$MACOSD/rpcemu-run"
		chmod +x "$MACOSD/rpcemu-run"
		ln -sf rpcemu-run "$MACOSD/rpcemu-shell"
	fi

	# Make each bundled dependency universal in the same way. A library present
	# for only one architecture is copied through thin, which keeps that slice
	# working rather than failing the whole build.
	FRAMEWORKSD="$CONTENTS/Frameworks"
	bundled=0
	for lib in "$X86_STAGE/libs/"* "$ARM_STAGE/libs/"*; do
		[ -f "$lib" ] || continue
		base=${lib##*/}
		[ -f "$FRAMEWORKSD/$base" ] && continue
		mkdir -p "$FRAMEWORKSD"
		if [ -f "$X86_STAGE/libs/$base" ] && [ -f "$ARM_STAGE/libs/$base" ]; then
			"$LIPO" -create "$X86_STAGE/libs/$base" "$ARM_STAGE/libs/$base" \
				-output "$FRAMEWORKSD/$base"
		else
			echo "   ! $base is single-architecture (present in only one slice)"
			cp -f "$lib" "$FRAMEWORKSD/$base"
		fi
		bundled=$((bundled + 1))
	done
	if [ "$bundled" -gt 0 ]; then
		echo "==> Bundled $bundled libraries into Contents/Frameworks"
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
