#!/usr/bin/env bash
#
# Build RPCEmu (Spork Edition) for Windows (x86-64) with MinGW-w64 and stage a
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

TARGET=x86_64-w64-mingw32
BUILD_DIR=build-win
WIN_ARCH=amd64
WIN_RELEASE="releases/windows/$WIN_ARCH"
MAKE_ZIP=false
# The x86-64 dynarec now supports the Windows x64 ABI (codegen_amd64.c), so the
# Windows build uses the recompiler by default for full-speed emulation. Pass
# --interpreter to force the (slower, ABI-agnostic) interpreter build.
INTERPRETER=false

for arg in "$@"; do
	case "$arg" in
		--zip|-z) MAKE_ZIP=true ;;
		--interpreter|-i) INTERPRETER=true ;;
		--dynarec) INTERPRETER=false ;;
		--help|-h) echo "Usage: $0 [--zip] [--interpreter|--dynarec]"; exit 0 ;;
		*) echo "unknown option: $arg"; exit 2 ;;
	esac
done

if [ "$INTERPRETER" = true ]; then
	DYNAREC_ARG=-DRPCEMU_DYNAREC=OFF
	BIN=rpcemu-interpreter.exe
else
	DYNAREC_ARG=-DRPCEMU_DYNAREC=ON
	BIN=rpcemu-recompiler.exe
fi

get_version() { [ -f VERSION ] && tr -d ' \t\r\n' < VERSION || echo "0.0.0"; }
VERSION=$(get_version)

njobs() { nproc 2>/dev/null || echo 4; }

# Mode detection: native MSYS2/MINGW64 vs Linux->Windows cross.
if [ "${MSYSTEM:-}" = "MINGW64" ]; then
	MODE="native (MSYS2 $MSYSTEM)"
	NATIVE=true
	OBJDUMP=objdump
	SEARCH_DIRS=(/mingw64/bin)
	CMAKE_TC_ARGS=()
else
	MODE="cross ($TARGET)"
	NATIVE=false
	SYSROOT=/usr/${TARGET}
	OBJDUMP=${TARGET}-objdump
	GCCDIR=$(dirname "$(${TARGET}-gcc -print-libgcc-file-name)")
	SEARCH_DIRS=("$SYSROOT/bin" "$SYSROOT/lib" "$GCCDIR")
	CMAKE_TC_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/mingw-w64-x86_64.cmake")
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

# Run the test suite. Only possible on a native MSYS2 build, since a cross build
# produces .exe files this host cannot execute. A failure is fatal: Windows spent
# a long time as the one platform that built without ever being tested.
if [ "$NATIVE" = true ]; then
	if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
		echo "==> ctest"
		( cd "$BUILD_DIR" && ctest --output-on-failure )
	fi
else
	echo "Note: skipping tests (cross-compiled binaries cannot run on this host)."
fi

echo "==> Staging $WIN_RELEASE"
rm -rf "$WIN_RELEASE"
mkdir -p "$WIN_RELEASE"
# Shared resources - identical set to the Linux release.
for d in configs poduleroms netroms gfxroms usbroms resources roms podules default; do
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
for t in rpcemu-run.exe rpcemu-shell.exe; do
	[ -f "$BUILD_DIR/bin/$t" ] && cp -f "$BUILD_DIR/bin/$t" "$WIN_RELEASE/"
done

# MCP server + docs (same as Linux).
if [ -d tools/mcp ]; then
	mkdir -p "$WIN_RELEASE/tools/mcp"
	cp -f tools/mcp/rpcemu_mcp.py tools/mcp/requirements.txt \
	      tools/mcp/README.md tools/mcp/mcp.json.example \
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
RPCEmu (Spork Edition) $VERSION
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Host:  $(uname -s) $(uname -m) (cross to $TARGET)
Binary: $BIN
Toolkit: wxWidgets (wxMSW) + CMake (MinGW-w64)
EOF

echo "✓ Staged: $WIN_RELEASE"

if [ "$MAKE_ZIP" = true ]; then
	ARCHIVE="rpcemu_${VERSION}_windows_${WIN_ARCH}.zip"
	echo "==> Packaging releases/windows/$ARCHIVE"
	( cd "$WIN_RELEASE" && cmake -E tar cf "../$ARCHIVE" --format=zip . )
	echo "✓ Windows archive: releases/windows/$ARCHIVE"
fi
