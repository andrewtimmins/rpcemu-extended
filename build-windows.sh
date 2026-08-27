#!/usr/bin/env bash
#
# Build RPCEmu (Extended) Edition) for Windows (x86-64) with MinGW-w64 and stage a
# runnable release into releases/windows/amd64/ - parity with build.sh's
# releases/linux/<arch>/ layout.
#
# Two modes, auto-detected:
#   * Cross-compile from Linux (default). Prerequisite: run
#     ./setup-cross-build-env.sh once to build the mingw dependencies
#     (wxWidgets/SDL2/libvncserver/...) into /usr/x86_64-w64-mingw32.
#   * Native on MSYS2/MINGW64 ($MSYSTEM set): uses the /mingw64 toolchain and
#     packages installed via pacman. This is what the Windows CI job runs.
#
# Usage:
#   ./build-windows.sh          # build + stage releases/windows/amd64/
#   ./build-windows.sh --zip    # also create releases/windows/*.zip
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

# Which Windows to build for. Taken from the MSYS2 environment when there is
# one, because that is what actually decides which compiler and which libraries
# are on the path:
#
#   MINGW64      x86-64, GCC             -> releases/windows/amd64
#   CLANGARM64   ARM64, clang            -> releases/windows/arm64
#   (neither)    the Linux cross build, x86-64 only
#
# ARM64 is native there: clang reports aarch64-w64-windows-gnu and file(1) calls
# the result "PE32+ ... ARM64", even though MSYS2's own runtime is x86-64 and runs
# under Windows' Prism emulation, which makes the build slow but not wrong.
case "${MSYSTEM:-}" in
CLANGARM64)
	TARGET=aarch64-w64-mingw32
	WIN_ARCH=arm64
	MSYS_PREFIX=/clangarm64
	;;
*)
	TARGET=x86_64-w64-mingw32
	WIN_ARCH=amd64
	MSYS_PREFIX=/mingw64
	;;
esac

BUILD_DIR=build-win
WIN_RELEASE="releases/windows/$WIN_ARCH"
MAKE_ZIP=false
BUILD_BOTH=false
# Set by the second pass of --both, so it adds to what the first pass staged
# instead of deleting it. Not an option: nothing outside this script should be
# staging into a directory it did not create.
KEEP_STAGED="${RPCEMU_KEEP_STAGED:-0}"

# The x86-64 dynarec supports the Windows x64 ABI (codegen_amd64.c), so that
# build uses the recompiler by default for full-speed emulation.
#
# ARM64 defaults to the interpreter, and still deliberately - but for a narrower
# reason than before. The AArch64 backend now ships on macOS and Linux (1.1.15),
# so "not shipped anywhere" is no longer the argument.
#
# What is left is specific to Windows on ARM: cache maintenance there wants
# FlushInstructionCache rather than the EL0 dc cvau / ic ivau our backend falls
# back on, and whether clang's __builtin___clear_cache lowers to something
# equivalent on that target has not been checked. The other worry can be struck
# off - x18 is reserved as the TEB pointer there, and codegen_arm64.c never uses
# it.
#
# Pass --dynarec to try it anyway; that is how it will be proved. This is also
# not a build anyone downloads yet: the release publishes windows_amd64 only.
if [ "$WIN_ARCH" = arm64 ]; then
	INTERPRETER=true
else
	INTERPRETER=false
fi

for arg in "$@"; do
	case "$arg" in
		--zip|-z) MAKE_ZIP=true ;;
		--interpreter|-i) INTERPRETER=true ;;
		--dynarec) INTERPRETER=false ;;
		--both) BUILD_BOTH=true ;;
		--help|-h) echo "Usage: $0 [--zip] [--interpreter|--dynarec|--both]"; echo "Target follows \$MSYSTEM: MINGW64 -> amd64, CLANGARM64 -> arm64 (interpreter by default)"; exit 0 ;;
		*) echo "unknown option: $arg"; exit 2 ;;
	esac
done

# ★ Both emulators, which takes two passes.
#
# RPCEMU_DYNAREC is a configure-time decision, so one configure builds one
# emulator. Rather than restructure this script around that, --both runs it
# twice: the interpreter first, then the recompiler, which stages alongside
# rather than over it and makes the archive at the end. The recompiler goes
# second so that the things written once - BUILDINFO, the zip - come from it.
#
# Two full compiles. On the arm64 runner, where MSYS2's own tools run emulated,
# that is the slowest job in CI; the timeout in the workflow allows for it.
if [ "$BUILD_BOTH" = true ]; then
	"$0" --interpreter
	if [ "$MAKE_ZIP" = true ]; then
		RPCEMU_KEEP_STAGED=1 "$0" --dynarec --zip
	else
		RPCEMU_KEEP_STAGED=1 "$0" --dynarec
	fi
	exit 0
fi

if [ "$INTERPRETER" = true ]; then
	DYNAREC_ARG=-DRPCEMU_DYNAREC=OFF
	BIN=rpcemu-interpreter.exe
else
	DYNAREC_ARG=-DRPCEMU_DYNAREC=ON
	BIN=rpcemu-recompiler.exe
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

njobs() { nproc 2>/dev/null || echo 4; }

# Mode detection: native MSYS2/MINGW64 vs Linux->Windows cross.
if [ "${MSYSTEM:-}" = "MINGW64" ] || [ "${MSYSTEM:-}" = "CLANGARM64" ]; then
	MODE="native (MSYS2 $MSYSTEM, $WIN_ARCH)"
	NATIVE=true
	# CLANGARM64 ships LLVM's binutils rather than GNU's, and the DLL walk below
	# only needs "DLL Name:" out of a PE's import table, which both print.
	if command -v objdump >/dev/null 2>&1; then
		OBJDUMP=objdump
	else
		OBJDUMP=llvm-objdump
	fi
	SEARCH_DIRS=("$MSYS_PREFIX/bin")
	CMAKE_TC_ARGS=()
else
	MODE="cross ($TARGET)"
	NATIVE=false
	SYSROOT=/usr/${TARGET}
	OBJDUMP=${TARGET}-objdump
	GCCDIR=$(dirname "$(${TARGET}-gcc -print-libgcc-file-name)")
	SEARCH_DIRS=("$SYSROOT/bin" "$SYSROOT/lib" "$GCCDIR")
	CMAKE_TC_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/mingw-w64-x86_64.cmake")
	# Wine, if it is here, so ctest can run the cross-built tests. Looked for in
	# the places Debian and Ubuntu put it: the `wine` wrapper is a separate
	# package from the loader, so wine64 alone is a perfectly normal install and
	# was what this machine had.
	if [ -z "${WINE:-}" ]; then
		for candidate in wine64 wine /usr/lib/wine/wine64; do
			if command -v "$candidate" >/dev/null 2>&1; then
				WINE=$(command -v "$candidate")
				break
			elif [ -x "$candidate" ]; then
				WINE="$candidate"
				break
			fi
		done
	fi
	if [ -n "${WINE:-}" ] && [ "${RPCEMU_SKIP_WINE_TESTS:-0}" != "1" ]; then
		CMAKE_TC_ARGS+=(-DCMAKE_CROSSCOMPILING_EMULATOR="$WINE")
	fi
	command -v ${TARGET}-gcc >/dev/null || { echo "error: ${TARGET}-gcc not found (apt install mingw-w64)"; exit 1; }
	if ! [ -x "$SYSROOT/bin/wx-config" ]; then
		echo "error: cross wxWidgets not found in $SYSROOT. Run ./setup-cross-build-env.sh first."
		exit 1
	fi
fi

if command -v ninja >/dev/null; then GEN=Ninja; else GEN="Unix Makefiles"; fi

echo "==> Building - mode: $MODE, generator: $GEN"
cmake -B "$BUILD_DIR" -G "$GEN" \
	"${CMAKE_TC_ARGS[@]}" \
	-DCMAKE_BUILD_TYPE=Release \
	"$DYNAREC_ARG" \
	-DRPCEMU_BUILD_TESTS=ON \
	"-DRPCEMU_REQUIRE_LIBUSB=${RPCEMU_REQUIRE_LIBUSB:-ON}" \
	-DRPCEMU_ENABLE_GHOSTPDL=OFF
cmake --build "$BUILD_DIR" -j"$(njobs)"

[ -f "$BUILD_DIR/bin/$BIN" ] || { echo "error: $BIN not built"; exit 1; }

# Run the test suite. A failure is fatal: Windows spent a long time as the one
# platform that built without ever being tested.
#
# A cross build used to skip this entirely, on the grounds that a Linux host
# cannot execute .exe files. It can, under Wine, and the whole suite passes there
# - which turns "the Windows build is checked when CI gets to it" into something
# checkable before pushing. CMAKE_CROSSCOMPILING_EMULATOR (set at configure time,
# above) is what makes ctest run each test through it.
#
# WINEPATH is needed as well: the test executables link the same DLLs the emulator
# does, and at this point they have not been staged next to anything. Without it
# every test exits 53 - a Windows "DLL not found", with no message, which reads
# like a crash.
#
# Set RPCEMU_SKIP_WINE_TESTS=1 to opt out.
if [ "$NATIVE" = true ]; then
	bash "$SCRIPT_DIR/tests/run-ctest.sh" "$BUILD_DIR"
elif [ -n "${WINE:-}" ] && [ "${RPCEMU_SKIP_WINE_TESTS:-0}" != "1" ]; then
	echo "==> Running the Windows test suite under Wine ($WINE)"
	winepath=""
	for d in "${SEARCH_DIRS[@]}"; do
		[ -d "$d" ] || continue
		# Z: is Wine's mapping of /, so an absolute Unix path converts by
		# swapping the separators.
		winepath="${winepath:+$winepath;}Z:${d//\//\\}"
	done
	WINEPATH="$winepath" WINEDEBUG="${WINEDEBUG:--all}" \
	    bash "$SCRIPT_DIR/tests/run-ctest.sh" "$BUILD_DIR" wine
else
	if [ "${RPCEMU_SKIP_WINE_TESTS:-0}" = "1" ]; then
		echo "Note: skipping tests (RPCEMU_SKIP_WINE_TESTS=1)."
	else
		echo "Note: skipping tests - no wine found, so these .exe files cannot run"
		echo "      on this host. Install wine to test a cross build before pushing"
		echo "      (apt install wine64), or rely on the Windows CI job."
	fi
fi

echo "==> Staging $WIN_RELEASE"
if [ "$KEEP_STAGED" != "1" ]; then
	rm -rf "$WIN_RELEASE"
fi
mkdir -p "$WIN_RELEASE"
# Shared resources - identical set to the Linux release.
# The guest-side payload - poduleroms, netroms, gfxroms, usbroms, podules and
# default - is deliberately NOT staged. It is embedded in the executable and
# extracted into the data directory on first run, so shipping a second copy
# beside the program is what let somebody keep an old one and run a new
# emulator against it. See src/support_files.h.
for d in configs resources roms; do
	cp -a "$d" "$WIN_RELEASE/"
done
# Common HostFS "Shared" disc (shared across machines). Normally created on
# first launch by the emulator; pre-create it so a fresh release is complete.
mkdir -p "$WIN_RELEASE/shared"
# No machine is shipped; see the same note in build.sh. New... creates one and
# seeds it from default/, which is staged above.
mkdir -p "$WIN_RELEASE/machines"
cp -f COPYING README.md COMPILE.md "$WIN_RELEASE/" 2>/dev/null || true

# Emulator + host-side tools (.exe copies; Windows has no symlinks).
cp -f "$BUILD_DIR/bin/$BIN" "$WIN_RELEASE/"
for t in rpcemu-run.exe rpcemu-shell.exe rpcemu-debug.exe rpcemu-netcap.exe; do
	[ -f "$BUILD_DIR/bin/$t" ] && cp -f "$BUILD_DIR/bin/$t" "$WIN_RELEASE/"
done

# MCP server + docs (same as Linux).
if [ -d tools/mcp ]; then
	mkdir -p "$WIN_RELEASE/tools/mcp"
	cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
	      tools/mcp/README.md tools/mcp/mcp.json.example \
	      tools/mcp/setup-mcp-env.ps1 \
	      "$WIN_RELEASE/tools/mcp/" 2>/dev/null || true
fi
[ -d docs ] && cp -a docs "$WIN_RELEASE/" 2>/dev/null || true

# Bundle the runtime DLLs the binaries need (transitive closure), so the release
# runs on a stock Windows box. System DLLs (kernel32, user32, ...) live in
# Windows and are skipped automatically (not found in the sysroot).
echo "==> Bundling runtime DLLs"
declare -A DLL_SEEN
locate_dll() { local n="$1"; for d in "${SEARCH_DIRS[@]}"; do [ -f "$d/$n" ] && { echo "$d/$n"; return; }; done; }
walk_dlls() {
	local pe="$1" dll key path
	for dll in $("$OBJDUMP" -p "$pe" 2>/dev/null | awk '/DLL Name/{print $3}'); do
		key=$(echo "$dll" | tr 'A-Z' 'a-z')
		[ -n "${DLL_SEEN[$key]:-}" ] && continue
		path=$(locate_dll "$dll") || true
		if [ -n "$path" ]; then DLL_SEEN[$key]="$path"; walk_dlls "$path"; fi
	done
}
for exe in "$WIN_RELEASE/"*.exe; do walk_dlls "$exe"; done
for key in "${!DLL_SEEN[@]}"; do
	cp -f "${DLL_SEEN[$key]}" "$WIN_RELEASE/"
	echo "   + $(basename "${DLL_SEEN[$key]}")"
done

cat > "$WIN_RELEASE/BUILDINFO.txt" <<EOF
RPCEmu Extended $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m) (cross to $TARGET)
Binary: $BIN$([ "$KEEP_STAGED" = "1" ] && echo " (and rpcemu-interpreter.exe, the other flavour)")
Toolkit: wxWidgets (wxMSW) + CMake (MinGW-w64)
EOF

echo "✓ Staged: $WIN_RELEASE"

if [ "$MAKE_ZIP" = true ]; then
	ARCHIVE="rpcemu_${VERSION}_windows_${WIN_ARCH}.zip"
	echo "==> Packaging releases/windows/$ARCHIVE"
	( cd "$WIN_RELEASE" && cmake -E tar cf "../$ARCHIVE" --format=zip . )
	echo "✓ Windows archive: releases/windows/$ARCHIVE"
fi
