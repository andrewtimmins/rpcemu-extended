#!/usr/bin/env bash
#
# Build RPCEmu Extended for macOS as a UNIVERSAL binary and stage a
# runnable release into releases/macos/ as RPCEmu.app - parity with build.sh's
# releases/linux/<arch>/ and build-windows.sh's releases/windows/amd64/ layout.
#
# Why two slices instead of one -arch arm64 -arch x86_64 build:
#   Each slice gets the recompiler for its own architecture: codegen_amd64.c emits
#   x86-64 and codegen_arm64.c emits AArch64, so the universal binary is
#   x86_64(dynarec) + arm64(dynarec), fused by lipo.
#
#   The arm64 slice was the interpreter until 1.1.14, because the arm64 recompiler
#   did not boot (issue #30) - which meant an Apple Silicon Mac, the machine most
#   people now run this on, was downloading the slow one. That is fixed, so it
#   ships the recompiler.
#
#   On Apple Silicon the x86_64 slice can also run (slowly) under Rosetta 2.
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

# A "for arg in $@" loop cannot consume an option's value, which is why --arch
# used to be a no-op that relied on the architecture being parsed as a bare
# argument. That accepted "build-macos.sh arm64", and silently ignored a bare
# "--arch" with nothing after it.
FUSE_REQUESTED=false
# Build the interpreter as well as the recompiler, and put both in the bundle.
# Off by default: it is a second full compile of the core per slice, which a
# developer rebuilding to try one change does not want. CI asks for it, because
# a release ships both.
BUILD_BOTH=false
while [ $# -gt 0 ]; do
	case "$1" in
		--zip|-z) MAKE_ZIP=true ;;
		--arch)
			case "${2:-}" in
				x86_64|arm64) ONE_ARCH="$2"; [ "$FUSE_REQUESTED" = true ] || DO_FUSE=false; shift ;;
				"") echo "error: --arch needs an architecture (x86_64 or arm64)"; exit 2 ;;
				*)  echo "error: unknown architecture '$2' (want x86_64 or arm64)"; exit 2 ;;
			esac
			;;
		--arch=*)
			case "${1#--arch=}" in
				x86_64|arm64) ONE_ARCH="${1#--arch=}"; [ "$FUSE_REQUESTED" = true ] || DO_FUSE=false ;;
				*) echo "error: unknown architecture '${1#--arch=}' (want x86_64 or arm64)"; exit 2 ;;
			esac
			;;
		--fuse) DO_FUSE=true; FUSE_REQUESTED=true ;;
		--both) BUILD_BOTH=true ;;
		--help|-h) echo "Usage: $0 [--arch x86_64|arm64] [--fuse] [--zip]"; exit 0 ;;
		*) echo "unknown option: $1"; exit 2 ;;
	esac
	shift
done
# "--arch x --fuse" means build that slice and fuse it with the other one that
# already exists, which is what a CI job building each arch separately wants.
# Given in either order, an explicit --fuse wins over --arch's default.
#
# A BARE --fuse is the only form that skips building: "lipo what is already
# there". --fuse used to clear DO_BUILD unconditionally, so "--arch x --fuse"
# quietly did the opposite of what the line above promises - it fused a stale
# appstage from an earlier invocation, producing a bundle that did not match the
# source tree and giving no hint that it had not been rebuilt.
if [ "$FUSE_REQUESTED" = true ]; then
	DO_FUSE=true
	[ -n "$ONE_ARCH" ] || DO_BUILD=false
fi

# Bare "--fuse", with no --arch, is the only form that skips building: it means
# "assemble a bundle from slices that already exist", which is what the CI job
# that only downloads them wants.
#
# It used to skip building whenever --fuse appeared at all, whatever else was on
# the command line. "--arch arm64 --fuse" therefore re-fused a stale slice and
# produced a bundle with none of the current source in it - silently, since
# everything it printed looked like a successful build. Several rounds of "this
# is still broken" were spent on a binary that predated the fix being tested.
if [ "$FUSE_REQUESTED" = true ] && [ -z "$ONE_ARCH" ]; then
	DO_BUILD=false
fi

get_version() {
	# See build.sh: number from VERSION, commit from git unless on a tag.
	release="$([ -f VERSION ] && tr -d ' \t\r\n' < VERSION || echo 0.0.0)"
	# `git rev-parse --git-dir` rather than `[ -d .git ]`: in a git worktree
	# .git is a FILE pointing at the real one, so the directory test failed
	# there and the build id came out as a bare version number with no
	# commit in it - the one thing that says which source a bundle was built
	# from.
	if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1 \
	   && ! git describe --tags --exact-match --match 'v[0-9]*' >/dev/null 2>&1; then
		commit="$(git rev-parse --short HEAD 2>/dev/null)"
		if [ -n "$commit" ]; then
			if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
				printf '%s-g%s-dirty\n' "$release" "$commit"; return
			fi
			printf '%s-g%s\n' "$release" "$commit"; return
		fi
	fi
	printf '%s\n' "$release"
}
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
	<key>CFBundleName</key><string>RPCEmu Extended</string>
	<key>CFBundleDisplayName</key><string>RPCEmu Extended</string>
	<key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
	<key>CFBundleExecutable</key><string>rpcemu</string>
	<key>CFBundleIconFile</key><string>rpcemu</string>
	<key>CFBundleShortVersionString</key><string>${VERSION}</string>
	<key>CFBundleVersion</key><string>${VERSION}</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>LSMinimumSystemVersion</key><string>${PLIST_MIN_OS}</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
	<key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
	<key>NSHumanReadableCopyright</key><string>RPCEmu contributors. Licensed under the GNU GPL v2.</string>
	<!--
	  Required, not optional, for anything that talks to other machines on the
	  same network. Without a usage description macOS has no text to put in the
	  prompt, so it does not ask - it denies, and says nothing. The denial is
	  also partial in a way that is easy to misread: unicast to a LAN peer
	  fails with EHOSTUNREACH while broadcast still goes out. For RISC OS that
	  means Access/ShareFS discovery works and opening a share never does, so
	  it presents as "the share is found but will not open" rather than as a
	  permissions problem. Same binary run from Terminal inherits the grant and
	  behaves, which makes it look like a bundling fault too.

	  It also covers the VNC server, which listens for connections from other
	  machines on the same network.
	-->
	<key>NSLocalNetworkUsageDescription</key><string>RPCEmu needs access to your local network so the emulated RISC OS machine can reach other computers on it - for file sharing over Access/ShareFS, and to serve its screen over VNC.</string>
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

# Per-arch build knobs. Both slices are the recompiler, each with its own backend.
#
# The arm64 slice was the interpreter until the arm64 recompiler was made to boot:
# an Apple Silicon Mac, which is what most people now run this on, was downloading
# the slow one.
slice_binname() { echo rpcemu-recompiler; }
slice_dynarec() { echo ON; }
# Tests are built for both slices, and with a dynarec backend on each that now
# includes the eight JIT differential tests - so the arm64 recompiler is checked
# against the interpreter on the architecture it generates code for, which is
# where a code-generation fault would actually show.
slice_tests()   { echo ON; }
slice_deploy()  { [ "$1" = "x86_64" ] && echo 10.15 || echo 11.0; }

# Does this library carry the slice we are about to build? A library that is not
# there at all is not this check's business - CMake reports a missing dependency
# clearly enough, whereas a present-but-wrong-architecture one is exactly what it
# reports badly.
lib_has_arch() {
	local lib="$1" arch="$2"

	[ -f "$lib" ] || return 0
	lipo -archs "$lib" 2>/dev/null | tr ' ' '\n' | grep -qx "$arch"
}

# Fail before configuring if the Homebrew libraries are for the other slice.
#
# Homebrew installs bottles for one architecture: /opt/homebrew on Apple Silicon
# (arm64) and /usr/local on Intel (x86_64). Apple clang cross-compiles happily
# between the two, and the HEADERS are architecture-neutral, so configuring the
# other slice against the wrong prefix SUCCEEDS - then dies hundreds of targets
# later with a screenful of "Undefined symbols for architecture x86_64" naming wx
# symbols, which says nothing about the real cause. Building a universal binary
# therefore needs a Homebrew of each architecture; without one, build the slice
# this machine has bottles for and let CI fuse them.
check_slice_deps() {
	local arch="$1" prefix lib
	local -a bad=()

	command -v wx-config >/dev/null 2>&1 || return 0
	prefix=$(wx-config --prefix 2>/dev/null) || return 0
	[ -n "$prefix" ] && [ -d "$prefix/lib" ] || return 0

	for lib in "$prefix"/lib/libwx_baseu-*.dylib "$prefix"/lib/libSDL2-*.dylib; do
		[ -e "$lib" ] || continue
		lib_has_arch "$lib" "$arch" || bad+=("$lib")
	done

	[ ${#bad[@]} -eq 0 ] && return 0

	echo "error: [$arch] the dependencies in $prefix are not built for $arch:" >&2
	for lib in "${bad[@]}"; do
		echo "         $lib ($(lipo -archs "$lib" 2>/dev/null))" >&2
	done
	echo "       Apple clang would configure anyway and then fail at link time" >&2
	echo "       with undefined symbols, so stopping here instead." >&2
	echo "       Build --arch $(uname -m) on this machine, or install a" >&2
	echo "       Homebrew for $arch and build that slice there." >&2
	exit 1
}

build_slice() {
	local arch="$1"
	local build_dir="build-mac-$arch"
	local dyn tests deploy
	dyn=$(slice_dynarec "$arch"); tests=$(slice_tests "$arch")
	deploy=$(slice_deploy "$arch")

	local gen; command -v ninja >/dev/null && gen=Ninja || gen="Unix Makefiles"
	local -a tc_args=()

	local -a extra_args=()
	# A native build must have libusb or the slice ships without USB
	# passthrough; the cross path deliberately has no libusb to find.
	local require_libusb="${RPCEMU_REQUIRE_LIBUSB:-ON}"
	if [ "$MODE" = native ]; then
		check_slice_deps "$arch"
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
		require_libusb=OFF
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
		"-DRPCEMU_REQUIRE_LIBUSB=$require_libusb" \
		-DRPCEMU_ENABLE_GHOSTPDL=OFF
	echo "==> [$arch] building"
	cmake --build "$build_dir" -j"$(njobs)"

	if [ "$BUILD_BOTH" = true ]; then
		# ★ The other emulator, for the same slice.
		#
		# A release ships both: the recompiler as the bundle's own binary, and the
		# interpreter beside it for anyone who needs to rule the JIT out of a
		# problem. RPCEMU_DYNAREC is decided at configure time, so that is a second
		# configure and a second compile into a tree of its own - the price of
		# shipping both, and it roughly doubles this job.
		#
		# No tests here: the suite has just run against the same sources, and the
		# JIT differential tests, which are the ones that care about the backend,
		# need the backend that this build does not have.
		echo "==> [$arch] configuring the interpreter (dynarec=OFF, tests=OFF)"
		cmake -B "$build_dir-interpreter" -G "$gen" \
			${tc_args[@]+"${tc_args[@]}"} ${extra_args[@]+"${extra_args[@]}"} \
			-DCMAKE_BUILD_TYPE=Release \
			-DRPCEMU_DYNAREC=OFF \
			-DRPCEMU_BUILD_TESTS=OFF \
			"-DRPCEMU_REQUIRE_LIBUSB=$require_libusb" \
			-DRPCEMU_ENABLE_GHOSTPDL=OFF
		echo "==> [$arch] building the interpreter"
		cmake --build "$build_dir-interpreter" -j"$(njobs)"
	fi

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
			bash "$SCRIPT_DIR/tests/run-ctest.sh" "$build_dir" "$arch"
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
# Canonical path: resolve the directory AND follow a symlinked filename.
# "cd $(dirname) && pwd -P" alone resolves the directory only, so Homebrew's
# lib/libSDL3.dylib -> libSDL3.0.dylib still came back under the link's name and
# looked like a different library from the same file reached directly.
# Do the two slices' copies of a library with the same basename come from the
# same place?
#
# Prints one of:
#   same     - the same library under each slice's own Homebrew prefix
#   version  - the same formula, but a different version in each slice
#   differ   - not the same library at all
#
# WHY THE MIDDLE ANSWER EXISTS. The two macOS runners are separate images with
# separate Homebrew installations, and they drift: on 2 September 2026 the
# x86_64 runner had pcre2 10.48 while the arm64 one had 10.47_1, and every
# build on main and 1.x failed because the check compared the whole path tail -
# version directory included - and called that a different library. It is not,
# and fusing them is sound: a fat dylib is two independent binaries in one
# file, the x86_64 half built against 10.48 and the arm64 half against
# 10.47_1, and a process loads only the half matching its own architecture.
# Neither half ever sees the other's code.
#
# What must still fail is a genuinely different library sharing a name - a
# /opt/local libfoo staged against a Homebrew one - because that says the two
# halves of the bundle are not the same software. So the formula name and the
# path below the version are compared, and only the version is forgiven.
staged_library_agreement() {
	local x86_src="$1" arm_src="$2"
	local x86_tail arm_tail

	# Strip each slice's expected Homebrew prefix. What remains should be the
	# same path under both, e.g. "Cellar/webp/1.6.0/lib/libwebp.7.dylib".
	x86_tail=${x86_src#/usr/local/}
	arm_tail=${arm_src#/opt/homebrew/}

	if [ "$x86_tail" = "$arm_tail" ]; then
		echo same
		return
	fi

	# Cellar/<formula>/<version>/<rest>: compare the formula and the rest, and
	# let the version differ. Anything not shaped like a Cellar path (an
	# opt/ symlink, a non-Homebrew prefix) falls through to "differ", which is
	# the safe answer.
	case "$x86_tail" in Cellar/*) ;; *) echo differ; return ;; esac
	case "$arm_tail" in Cellar/*) ;; *) echo differ; return ;; esac

	local x86_rest=${x86_tail#Cellar/} arm_rest=${arm_tail#Cellar/}
	local x86_formula=${x86_rest%%/*} arm_formula=${arm_rest%%/*}

	if [ "$x86_formula" != "$arm_formula" ]; then
		echo differ
		return
	fi

	# Drop the formula, then the version, from each.
	x86_rest=${x86_rest#*/}; x86_rest=${x86_rest#*/}
	arm_rest=${arm_rest#*/}; arm_rest=${arm_rest#*/}

	if [ "$x86_rest" = "$arm_rest" ]; then
		echo version
	else
		echo differ
	fi
}

canon_path() {
	local p="$1" d f
	while [ -L "$p" ]; do
		d=$(cd "$(dirname "$p")" 2>/dev/null && pwd -P) || break
		f=$(readlink "$p")
		case "$f" in
			/*) p="$f" ;;
			*)  p="$d/$f" ;;
		esac
	done
	d=$(cd "$(dirname "$p")" 2>/dev/null && pwd -P) || { echo "$p"; return; }
	echo "$d/${p##*/}"
}

resolve_dep() {
	local obj="$1" dep="$2" origin="$3" entry candidate

	case "$dep" in
	@loader_path/*|@executable_path/*)
		# Both are relative to where the referring object sits, which during
		# staging is $origin. collect_deps() handles @executable_path itself,
		# but a chain reaching here through @rpath can arrive with either form,
		# so the resolver understands both.
		candidate="$origin/${dep#@*_path/}"
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
	local obj="$1" map="$2" origin="$3" dep base resolved prev

	while read -r dep; do
		case "$dep" in
			/usr/lib/*|/System/*) continue ;;	# provided by the OS
		esac

		# Checked before resolving, which also disposes of the object's own id:
		# otool lists that first for a dylib, and by the time we recurse into one
		# its name is already recorded.
		base=${dep##*/}
		# Already collected? Compare the resolved path too: two libraries with
		# the same basename from different prefixes would both flatten to
		# Contents/Frameworks/<basename>, and taking the first silently ships
		# the wrong one, so that case is reported below rather than ignored.
		prev=$(awk -F'\t' -v b="$base" '$1 == b { print $2; exit }' "$map")

		case "$dep" in
			/*) resolved="$dep" ;;
			@executable_path/*)
				# NOT "already in the bundle": no bundle exists yet. A binary
				# reaching here with this form was rewritten by an earlier
				# staging pass, and skipping it drops the dependency silently -
				# the copy never happens and rewrite_deps() then finds nothing
				# to do. Resolve it against where the executable actually is
				# now, and fall back to the library search paths if the staged
				# copy is not there yet.
				resolved="$origin/${dep#@executable_path/}"
				if [ ! -f "$resolved" ]; then
					resolved=$(resolve_dep "$obj" "@rpath/${dep##*/}" "$origin" 2>/dev/null) || resolved=""
				fi
				if [ -z "$resolved" ] || [ ! -f "$resolved" ]; then
					echo "   ! ${obj##*/} references $dep and it could not be resolved; not bundled"
					continue
				fi
				;;
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

		# Canonicalise before comparing. Homebrew reaches the same file through
		# /opt/homebrew/opt/<formula> (a symlink into Cellar/) as well as its
		# real path, and recursing accumulates "../lib/../lib/" segments, so
		# the same library arrives under several spellings. Without this the
		# collision check below reports those as different libraries.
		resolved=$(canon_path "$resolved")

		if [ -n "$prev" ]; then
			if [ "$prev" != "$resolved" ]; then
				echo "   ! two different libraries are both named $base:"
				echo "       $prev"
				echo "       $resolved"
				echo "     they cannot both be Contents/Frameworks/$base"
				echo BAD >> "${map}.collision"
			fi
			continue			# already collected
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
	local map="$stage/deps.map"
	local src base obj

	rm -rf "$stage"
	mkdir -p "$stage/bin" "$stage/libs"
	: > "$map"

	# The emulator is renamed to "rpcemu" in the bundle regardless of which
	# variant this slice built.
	cp -f "$build_dir/bin/$(slice_binname "$arch")" "$stage/bin/rpcemu"
	# And the interpreter under its own name, from its own tree. Kept as
	# rpcemu-interpreter rather than renamed: the bundle runs "rpcemu", and a
	# second binary is only useful if it can be told apart from it.
	if [ -f "$build_dir-interpreter/bin/rpcemu-interpreter" ]; then
		cp -f "$build_dir-interpreter/bin/rpcemu-interpreter" \
		    "$stage/bin/rpcemu-interpreter"
	fi
	if [ -f "$build_dir/bin/rpcemu-run" ]; then
		cp -f "$build_dir/bin/rpcemu-run" "$stage/bin/rpcemu-run"
	fi
	if [ -f "$build_dir/bin/rpcemu-debug" ]; then
		cp -f "$build_dir/bin/rpcemu-debug" "$stage/bin/rpcemu-debug"
	fi
	if [ -f "$build_dir/bin/rpcemu-netcap" ]; then
		cp -f "$build_dir/bin/rpcemu-netcap" "$stage/bin/rpcemu-netcap"
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

	# Libraries loaded at runtime rather than linked, which otool -L cannot see.
	#
	# Homebrew's "sdl2" is now sdl2-compat: a shim implementing the SDL2 ABI on
	# top of SDL3, which it dlopen()s instead of linking. So libSDL3 appears in
	# no LC_LOAD_DYLIB and the dependency walk above never finds it - the app
	# then works on the build machine (Homebrew's copy is still on disk) and
	# fails anywhere else. sdl2-compat's failure path runs in a library
	# constructor and puts up a modal alert, so dyld blocks in
	# runAllInitializersForMain() and the program never reaches main() at all:
	# no output, no headless mode, and a GUI that cannot start either.
	#
	# sdl2-compat looks for "@loader_path/libSDL3.dylib" first, and the loader
	# here is Contents/Frameworks/libSDL2-2.0.0.dylib, so placing SDL3 beside it
	# is enough - no install-name rewriting of the reference is possible anyway,
	# since there is no reference to rewrite.
	# Test the dependency map, not $stage/libs: the copy loop below is what
	# populates that directory, so checking it here always failed and this whole
	# block was silently skipped.
	if awk -F'\t' '$1 ~ /^libSDL2/ { f = 1 } END { exit !f }' "$map"; then
		local sdl3 sdl3_prev sdl3_name
		# Ask the Homebrew that matches THIS slice. On Apple Silicon the arm64
		# brew in /opt/homebrew is on PATH, so building the x86_64 slice would
		# otherwise pick up an arm64 SDL3 and lipo would later refuse to fuse
		# it (or, worse, the slice would carry the wrong architecture).
		local brew_x86=/usr/local/bin/brew brew_arm=/opt/homebrew/bin/brew
		local brew_bin=""
		if [ "$arch" = x86_64 ] && [ -x "$brew_x86" ]; then
			brew_bin="$brew_x86"
		elif [ "$arch" = arm64 ] && [ -x "$brew_arm" ]; then
			brew_bin="$brew_arm"
		fi
		for sdl3 in \
			"$([ -n "$brew_bin" ] && "$brew_bin" --prefix sdl3 2>/dev/null)/lib/libSDL3.dylib" \
			"$([ -n "$brew_bin" ] && "$brew_bin" --prefix 2>/dev/null)/lib/libSDL3.dylib" \
			"$([ "$arch" = x86_64 ] && echo /usr/local || echo /opt/homebrew)/lib/libSDL3.dylib"
		do
			# The prefix should imply the architecture, but check rather than
			# assume: fusing a mismatched slice fails much later and far less
			# clearly than saying so here.
			if [ -f "$sdl3" ] && ! lipo -archs "$sdl3" 2>/dev/null | tr ' ' '\n' | grep -qx "$arch"; then
				echo "   ! $sdl3 is not $arch ($(lipo -archs "$sdl3" 2>/dev/null)); ignoring"
				continue
			fi
			if [ -f "$sdl3" ]; then
				# Record it exactly as collect_deps() would, because the
				# recursion below reaches the same file again and the two
				# entries are compared:
				#   - under its own install name (libSDL3.0.dylib), not the
				#     symlink's name, so it is not bundled twice under two
				#     names; and
				#   - by canonical path, since $sdl3 is typically
				#     /opt/homebrew/opt/sdl3/lib/libSDL3.dylib, a symlink into
				#     Cellar. Storing the symlink made collect_deps' canonical
				#     path look like a different library and failed the build.
				sdl3=$(canon_path "$sdl3")
				sdl3_name=$("$OTOOL" -D "$sdl3" 2>/dev/null | tail -n +2 | head -1)
				sdl3_name=${sdl3_name##*/}
				[ -n "$sdl3_name" ] || sdl3_name=${sdl3##*/}
				sdl3_prev=$(awk -F'\t' -v b="$sdl3_name" '$1 == b { print $2; exit }' "$map")
				if [ -z "$sdl3_prev" ]; then
					printf '%s\t%s\n' "$sdl3_name" "$sdl3" >> "$map"
				elif [ "$sdl3_prev" != "$sdl3" ]; then
					echo "error: [$arch] two different SDL3 libraries were found:"
					echo "         $sdl3_prev"
					echo "         $sdl3"
					exit 1
				fi
				collect_deps "$sdl3" "$map" "$(dirname "$sdl3")"
				break
			fi
		done
		echo "   SDL-related staged dependencies:"
		grep -i sdl "$map" | sed 's/^/     /' || true
		if ! awk -F'\t' '$1 ~ /^libSDL3/ { f = 1 } END { exit !f }' "$map"; then
			echo "error: [$arch] this SDL2 is sdl2-compat, which needs SDL3 at runtime,"
			echo "       but no libSDL3.dylib was found to bundle. Install it (brew"
			echo "       install sdl3) or link a real SDL2 instead. Without it the"
			echo "       finished app cannot start on any machine but this one."
			exit 1
		fi
	fi

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

	if [ -f "${map}.collision" ]; then
		rm -f "${map}.collision"
		echo "error: [$arch] two different libraries share a basename (see above);"
		echo "       Contents/Frameworks is flat, so one would silently win."
		exit 1
	fi

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
	# Which slices are actually there.
	#
	# Both is the shipping case and gives a universal binary. One is enough to
	# assemble a bundle that runs on the machine that built it, which is what a
	# developer whose Homebrew is a single architecture can produce - and a
	# testable RPCEmu.app is far more use to them than refusing outright, which
	# is what this did. On an Apple Silicon Mac with only the arm64 bottles that
	# left no way to build the shipping artefact locally at all.
	STAGES=()
	SLICE_ARCHES=()
	for arch in x86_64 arm64; do
		src="build-mac-$arch/bin/$(slice_binname "$arch")"
		staged="build-mac-$arch/appstage/bin/rpcemu"

		# Re-stage when the built binary is NEWER than the staged copy, not just
		# when the staged copy is missing. Staleness was previously invisible: a
		# tree rebuilt since it was last staged fused the old binary and said
		# nothing, so the bundle under test silently was not the code under
		# test - which cost a whole test cycle chasing a bug that was already
		# fixed in the source.
		if [ -f "$src" ] && { [ ! -f "$staged" ] || [ "$src" -nt "$staged" ]; }; then
			[ -f "$staged" ] && echo "==> [$arch] staged copy is older than the build; re-staging"
			stage_slice "$arch"
		fi
		[ -f "$staged" ] || continue
		STAGES+=("build-mac-$arch/appstage")
		SLICE_ARCHES+=("$arch")
	done

	if [ ${#STAGES[@]} -eq 0 ]; then
		echo "error: no slice built for either architecture."
		echo "       Build one first (./build-macos.sh --arch $(uname -m)),"
		echo "       or use the CI per-arch jobs."
		exit 1
	fi
	if [ ${#STAGES[@]} -eq 1 ]; then
		echo "==> NOTE: only the ${SLICE_ARCHES[0]} slice is present, so RPCEmu.app will"
		echo "    not be universal - it runs on ${SLICE_ARCHES[0]} only. Install a Homebrew"
		echo "    for the other architecture, or let CI build both, for a universal bundle."
	fi

	# lipo one staged binary from every slice present. Given a single input that
	# is a thin copy, which is exactly what a one-slice bundle wants.
	fuse_bin() {
		local leaf="$1" out="$2" s
		local -a ins=()

		for s in "${STAGES[@]}"; do
			[ -f "$s/bin/$leaf" ] && ins+=("$s/bin/$leaf")
		done
		[ ${#ins[@]} -gt 0 ] || return 1

		"$LIPO" -create "${ins[@]}" -output "$out"
		chmod +x "$out"
	}

	# lipo the named binary from every slice that has it, or nothing if none do.
	# One input produces a thin binary, which is what a single-arch bundle is.
	fuse_binary() {
		local name="$1" out="$2"
		local -a inputs=()
		local stage

		for stage in "${STAGES[@]}"; do
			[ -f "$stage/bin/$name" ] && inputs+=("$stage/bin/$name")
		done
		[ ${#inputs[@]} -gt 0 ] || return 1
		"$LIPO" -create ${inputs[@]+"${inputs[@]}"} -output "$out"
		chmod +x "$out"
		return 0
	}

	# Assemble a proper macOS application bundle:
	#   RPCEmu.app/Contents/MacOS/rpcemu      (universal emulator + CLI helpers)
	#   RPCEmu.app/Contents/Resources/...     (read-only payload + rpcemu.icns)
	#   RPCEmu.app/Contents/Info.plist
	# Writable data (machines, configs, ROMs, hostfs, logs) is NOT kept inside
	# the bundle - InitRpcemuPaths() reads Contents/Resources and seeds ~/RPCEmu
	# on first run, so an app dragged into /Applications stays read-only.
	# The bundle can only claim what its slices support: x86_64 targets 10.15
	# and arm64 targets 11.0 (there is no earlier macOS on Apple Silicon), so a
	# fused bundle runs from 10.15 on Intel and 11.0 on Apple Silicon. The plist
	# carries one number, so it must be the lower - anything higher would stop
	# Intel Macs the x86_64 slice supports from launching. A single-arch arm64
	# build advertises 11.0.
	# Taken from the slices actually going in, not from --arch: the slices can
	# have been built by earlier invocations, in which case --arch says nothing
	# about what is being fused. x86_64 targets the lower of the two, so it
	# decides whenever it is present.
	case " ${SLICE_ARCHES[*]} " in
	*" x86_64 "*)	PLIST_MIN_OS=$(slice_deploy x86_64) ;;
	*)		PLIST_MIN_OS=$(slice_deploy arm64) ;;
	esac

	APP="releases/macos/RPCEmu.app"
	CONTENTS="$APP/Contents"
	MACOSD="$CONTENTS/MacOS"
	RESD="$CONTENTS/Resources"

	echo "==> Assembling $APP"
	rm -rf "$APP"
	mkdir -p "$MACOSD" "$RESD"

	# The guest-side payload - poduleroms, netroms, gfxroms, usbroms, podules
	# and default - is deliberately NOT staged. It is embedded in the executable
	# and extracted into the data directory on first run, so shipping a second
	# copy beside the program is what let somebody keep an old one and run a new
	# emulator against it. See src/support_files.h.
	for d in configs resources roms; do
		[ -e "$d" ] && cp -a "$d" "$RESD/"
	done
	# No machine is shipped; see the note in build.sh. New... creates one and
	# seeds it from default/, copied above.
	mkdir -p "$RESD/machines"
	cp -f COPYING README.md COMPILE.md "$RESD/" 2>/dev/null || true
	[ -d docs ] && cp -a docs "$RESD/" 2>/dev/null || true

	# MCP server (same set as the Linux/Windows releases).
	if [ -d tools/mcp ]; then
		mkdir -p "$RESD/tools/mcp"
		cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
		      tools/mcp/README.md tools/mcp/mcp.json.example \
		      tools/mcp/setup-mcp-env.sh \
		      "$RESD/tools/mcp/" 2>/dev/null || true
	fi

	# Fuse the emulator: x86_64(dynarec) + arm64(interpreter) -> universal.
	# The staged copies are used, so the rewritten dependency paths are carried
	# through into the bundle.
	# What to call this bundle, decided once from the slices actually found rather
	# than from a flag - which is how everything else here decides, and avoids the
	# two names disagreeing.
	#
	# A one-slice bundle called "universal" is a download that fails on half the
	# Macs that trust the name, so only a genuinely fused pair earns the word.
	if [ ${#SLICE_ARCHES[@]} -gt 1 ]; then
		BINARY_DESC="universal: ${SLICE_ARCHES[*]}, recompiler on both, interpreter beside it"
		ARCHTAG=universal
	else
		BINARY_DESC="${SLICE_ARCHES[0]} only, recompiler, interpreter beside it"
		ARCHTAG="${SLICE_ARCHES[0]}"
	fi

	echo "==> lipo emulator binary (${SLICE_ARCHES[*]})"
	fuse_bin rpcemu "$MACOSD/rpcemu" || {
		echo "error: no staged rpcemu binary to assemble"; exit 1;
	}

	# The interpreter, the same way. Not fatal if it is absent: a --fuse over
	# slices staged by an older build of this script has no such binary, and a
	# bundle without it is the bundle we shipped until now.
	fuse_bin rpcemu-interpreter "$MACOSD/rpcemu-interpreter" || true

	# Fuse the HostCmd host client from whichever slices built it.
	if fuse_bin rpcemu-run "$MACOSD/rpcemu-run"; then
		ln -sf rpcemu-run "$MACOSD/rpcemu-shell"
	fi

	# And the DebugCmd host client, the same way.
	fuse_bin rpcemu-netcap "$MACOSD/rpcemu-netcap" || true
	fuse_bin rpcemu-debug "$MACOSD/rpcemu-debug" || true

	# Make each bundled dependency universal in the same way. A library present
	# for only one architecture is copied through thin, which keeps that slice
	# working rather than failing the whole build.
	FRAMEWORKSD="$CONTENTS/Frameworks"
	bundled=0
	for stage in "${STAGES[@]}"; do
	for lib in "$stage/libs/"*; do
		[ -f "$lib" ] || continue
		base=${lib##*/}
		[ -f "$FRAMEWORKSD/$base" ] && continue
		mkdir -p "$FRAMEWORKSD"
		if [ -f "$X86_STAGE/libs/$base" ] && [ -f "$ARM_STAGE/libs/$base" ]; then
			# Same basename in both slices is not proof they are the same
			# library: each slice resolved its own Homebrew prefix, and a
			# machine with, say, /opt/local alongside could stage a different
			# libfoo for each. Comparing the files is no use - thin slices for
			# different architectures always differ - so compare where they
			# came from, which the per-slice maps record.
			# The maps are per-slice build output, so they are present for a
			# local two-arch build but not when the fusing job only downloaded
			# the staged appstage directories. Missing maps mean the check
			# cannot run, not that the libraries disagree - and awk exiting
			# non-zero on a missing file would otherwise kill the script here,
			# since a failing command substitution is fatal under set -e.
			x86_src=""; arm_src=""
			if [ -f "$X86_STAGE/deps.map" ] && [ -f "$ARM_STAGE/deps.map" ]; then
				x86_src=$(awk -F'\t' -v b="$base" '$1 == b { print $2; exit }' "$X86_STAGE/deps.map")
				arm_src=$(awk -F'\t' -v b="$base" '$1 == b { print $2; exit }' "$ARM_STAGE/deps.map")
			fi
			if [ -n "$x86_src" ] && [ -n "$arm_src" ]; then
				case "$(staged_library_agreement "$x86_src" "$arm_src")" in
				same)
					;;
				version)
					# Same formula, different version on each runner. Said out
					# loud because it explains any behaviour that differs
					# between the two halves of the bundle, but not fatal: see
					# staged_library_agreement() for why fusing them is sound.
					echo "   ! $base is a different VERSION in each slice:"
					echo "       x86_64: $x86_src"
					echo "       arm64:  $arm_src"
					echo "     fusing anyway; each architecture loads its own half"
					;;
				*)
					echo "   ! $base is a different library in each slice:"
					echo "       x86_64: $x86_src"
					echo "       arm64:  $arm_src"
					echo "     refusing to fuse them into one Contents/Frameworks/$base"
					exit 1
					;;
				esac
			fi
			"$LIPO" -create "$X86_STAGE/libs/$base" "$ARM_STAGE/libs/$base" \
				-output "$FRAMEWORKSD/$base"
		else
			# Worth saying only when the bundle is meant to be universal. In a
			# single-arch bundle every library is thin by definition, and warning
			# about each of twenty-one of them is noise that hides real problems.
			if [ ${#STAGES[@]} -gt 1 ]; then
				echo "   ! $base is single-architecture (present in only one slice)"
			fi
			cp -f "$lib" "$FRAMEWORKSD/$base"
		fi
		# The thin copies were given this id before fusing, so lipo should
		# carry it through - but the finished bundle is what matters, and a
		# wrong id here makes every dependent library unloadable. Cheap to
		# assert rather than assume.
		if [ -n "$INSTALL_NAME_TOOL" ]; then
			chmod u+w "$FRAMEWORKSD/$base"
			"$INSTALL_NAME_TOOL" -id "@executable_path/../Frameworks/$base" \
				"$FRAMEWORKSD/$base" 2>/dev/null || true
		fi
		bundled=$((bundled + 1))
	done
	done
	if [ "$bundled" -gt 0 ]; then
		echo "==> Bundled $bundled libraries into Contents/Frameworks"
	fi

	# Collapse libraries bundled more than once under different names.
	#
	# Homebrew ships one real dylib plus version symlinks pointing at it
	# (libwx_osx_cocoau_core-3.3.dylib -> ... -3.3.3.0.0.dylib), and the
	# dependency walk keys each entry on the basename of whichever name the
	# object it came from happened to reference. Two names for one library
	# therefore produced two FULL COPIES here - and dyld loaded both, because
	# they are two distinct paths.
	#
	# That is not merely wasteful. wxWidgets was then present twice, every
	# Objective-C class inside it registered twice ("Class wxNSApplication is
	# implemented in both ..."), and the Manager's OpenGL canvas never became
	# usable: a machine started from the Manager showed a permanently black
	# screen in the shipped .app while the very same code was fine in a
	# build-tree run, which links Homebrew's single copy.
	#
	# A symlink is what upstream has and what dyld expects: it resolves to an
	# image that is already loaded, so the library is mapped once however many
	# names point at it.
	#
	# The test is the CANONICAL UPSTREAM PATH the copy came from, recorded per
	# basename in each slice's deps.map. Comparing the staged files byte for byte
	# does not work: each copy has already had its own LC_ID_DYLIB written into
	# it, so two copies of one library differ in exactly those bytes and hash
	# differently while being the same library.
	#
	# The name kept is the one the real file upstream has, so what remains is the
	# layout the libraries were linked against, with the aliases pointing at it.
	if [ -d "$FRAMEWORKSD" ]; then
		dedup_map=$(mktemp)
		collapsed=0
		for lib in "$FRAMEWORKSD"/*.dylib; do
			[ -f "$lib" ] || continue	# skips existing symlinks too
			[ -L "$lib" ] && continue

			base=${lib##*/}
			src=""
			for s in "${STAGES[@]}"; do
				[ -f "$s/deps.map" ] || continue
				src=$(awk -F'\t' -v b="$base" '$1 == b { print $2; exit }' "$s/deps.map")
				[ -n "$src" ] && break
			done
			# No map entry means nothing to compare it against - leave it alone
			# rather than guess.
			[ -n "$src" ] || continue
			src=$(canon_path "$src")

			keep=$(awk -F'\t' -v p="$src" '$1 == p { print $2; exit }' "$dedup_map")
			if [ -z "$keep" ]; then
				# First name seen for this upstream file: it is the survivor.
				# Deliberately NOT renamed to the upstream file's own name - a
				# single copy under an alias is already correct and works, and
				# renaming every such library would be churn for no gain.
				printf '%s\t%s\n' "$src" "$base" >> "$dedup_map"
			elif [ "$keep" != "$base" ]; then
				echo "   collapsed $base -> $keep (one library under two names)"
				rm -f "$lib"
				( cd "$FRAMEWORKSD" && ln -sf "$keep" "$base" )
				collapsed=$((collapsed + 1))
			fi
		done
		rm -f "$dedup_map"
		[ "$collapsed" -gt 0 ] && \
			echo "==> Collapsed $collapsed duplicate librar$([ "$collapsed" = 1 ] && echo y || echo ies) into symlinks"
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
RPCEmu Extended $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m) ($MODE)
Binary: rpcemu ($BINARY_DESC)
Toolkit: wxWidgets (wxOSX/Cocoa) + CMake
EOF

	# Code signing. Apple Silicon refuses to run an unsigned binary at all, so an
	# ad-hoc signature (-s -) is the minimum for the app to launch. Without an
	# Apple Developer ID we cannot notarise, so Gatekeeper still warns on first
	# open (right-click > Open once); this is documented in the README.
	# RPCEMU_MACOS_CODESIGN=1 additionally applies the hardened runtime + JIT
	# entitlement, needed only for a future recompiler arm64 slice (MAP_JIT).
	# sdl2-compat dlopen()s "@loader_path/libSDL3.dylib" by that exact name, but
	# SDL3's install name is versioned (libSDL3.0.dylib) and that is what gets
	# bundled. Link the unversioned name to it, before signing so the signature
	# covers the finished layout. Bundling the file twice would also work but
	# wastes 5MB; leaving it out means sdl2-compat fails inside a dyld
	# initializer and the app never reaches main() at all.
	if ls "$CONTENTS/Frameworks"/libSDL3.*.dylib >/dev/null 2>&1 && \
	   [ ! -e "$CONTENTS/Frameworks/libSDL3.dylib" ]; then
		( cd "$CONTENTS/Frameworks" && ln -sf "$(ls libSDL3.*.dylib | head -1)" libSDL3.dylib )
		echo "==> linked Frameworks/libSDL3.dylib -> $(readlink "$CONTENTS/Frameworks/libSDL3.dylib")"
	fi

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

	# Prove the bundle is self-contained, rather than assuming it. Every
	# @executable_path reference must resolve to a file that is actually here,
	# and nothing may still point at Homebrew - that is the reference that works
	# on the build machine and nowhere else.
	echo "==> Verifying the bundle is self-contained"
	bundle_ok=true
	for obj in "$MACOSD"/* "$CONTENTS/Frameworks"/*; do
		# Symlinks are skipped deliberately: they alias a real file in the same
		# directory, which is examined on its own iteration, so following them
		# would only check it twice.
		[ -L "$obj" ] && continue
		[ -f "$obj" ] || continue
		file "$obj" 2>/dev/null | grep -q Mach-O || continue

		"$OTOOL" -L "$obj" 2>/dev/null | tail -n +2 | awk '{ print $1 }' | while read -r dep; do
			case "$dep" in
				@executable_path/../Frameworks/*)
					# -e, not -f: the question is "does this runtime path
					# resolve", and Frameworks holds symlinks as well as
					# regular files (libSDL3.dylib -> libSDL3.0.dylib). Both
					# tests follow links, but -e is the one that stays correct
					# if a dependency ever arrives as something other than a
					# plain file, and it fails on a dangling link either way.
					if [ ! -e "$CONTENTS/Frameworks/${dep##*/}" ]; then
						echo "   ! $(basename "$obj") needs ${dep##*/}, which is not in Contents/Frameworks"
						echo BAD >> "$CONTENTS/.deps.fail"
					fi
					;;
				/opt/homebrew/*|/usr/local/*)
					echo "   ! $(basename "$obj") still references $dep - it will not resolve elsewhere"
					echo BAD >> "$CONTENTS/.deps.fail"
					;;
				@rpath/*|@loader_path/*)
					# Everything non-system is meant to live in Frameworks under
					# an @executable_path reference. One of these means a
					# dependency was missed by the rewrite, and whether it
					# resolves then depends on the LC_RPATH entries the library
					# happens to carry - which is not something to ship on.
					echo "   ! $(basename "$obj") still references $dep - it should point into Frameworks"
					echo BAD >> "$CONTENTS/.deps.fail"
					;;
			esac
		done
	done
	if [ -f "$CONTENTS/.deps.fail" ]; then
		rm -f "$CONTENTS/.deps.fail"
		echo "error: the bundle is not self-contained (see above)."
		exit 1
	fi
	if [ -f "$CONTENTS/Frameworks/libSDL2-2.0.0.dylib" ] && \
	   [ ! -e "$CONTENTS/Frameworks/libSDL3.dylib" ]; then
		echo "error: libSDL2 is present but libSDL3 is not. If this SDL2 is"
		echo "       sdl2-compat it loads SDL3 at runtime, and the app will stop"
		echo "       in a modal alert before main() on any machine without it."
		exit 1
	fi
	echo "✓ every bundled dependency resolves inside the bundle"

	echo "==> Bundle architectures:"
	"$LIPO" -archs "$MACOSD/rpcemu" 2>/dev/null || true
	echo "✓ Bundle: $APP"

	# Disk image with a drag-to-Applications shortcut - the format Mac users
	# expect. macOS only (hdiutil); the osxcross path stops at the .app.
	if [ "$(uname -s)" = Darwin ] && command -v hdiutil >/dev/null 2>&1; then
		# Named for what it actually contains: see ARCHTAG above.
		DMG_ARCH="$ARCHTAG"
		DMG="releases/macos/rpcemu_${VERSION}_macos_${DMG_ARCH}.dmg"
		echo "==> Packaging $DMG"
		DMGSTAGE=$(mktemp -d)
		cp -a "$APP" "$DMGSTAGE/"
		ln -s /Applications "$DMGSTAGE/Applications"
		rm -f "$DMG"

		# hdiutil now and then fails with "Resource busy" on a CI runner -
		# something is still holding the staging folder, or an image from an
		# earlier run has not finished detaching - and it is transient: the
		# same command a few seconds later works. Everything up to here has
		# already succeeded, and the release job needs this artifact, so a
		# whole build is not worth losing to it. Try a few times, and if it
		# really will not go, say why rather than swallowing the reason.
		DMGLOG=$(mktemp)
		dmg_try=1
		while true; do
			if hdiutil create -volname "RPCEmu $VERSION" \
				-srcfolder "$DMGSTAGE" -fs HFS+ -format UDZO \
				-ov "$DMG" >"$DMGLOG" 2>&1
			then
				break
			fi
			if [ "$dmg_try" -ge 5 ]; then
				echo "   ! hdiutil create failed $dmg_try times, giving up:"
				cat "$DMGLOG" >&2
				rm -f "$DMGLOG"
				rm -rf "$DMGSTAGE"
				exit 1
			fi
			echo "   ! hdiutil create failed (attempt $dmg_try), retrying in 5s"
			dmg_try=$((dmg_try + 1))
			sleep 5
		done
		if [ "$dmg_try" -gt 1 ]; then
			echo "   ✓ hdiutil create succeeded on attempt $dmg_try"
		fi

		rm -f "$DMGLOG"
		rm -rf "$DMGSTAGE"
		echo "✓ macOS DMG: $DMG"
	fi

	# Portable archive of the bundle (optional; the DMG is the primary artifact).
	if [ "$MAKE_ZIP" = true ]; then
		ARCHIVE="rpcemu_${VERSION}_macos_${ARCHTAG}.tar.gz"
		echo "==> Packaging releases/macos/$ARCHIVE"
		( cd releases/macos && tar czf "$ARCHIVE" RPCEmu.app )
		echo "✓ macOS archive: releases/macos/$ARCHIVE"
	fi
fi
