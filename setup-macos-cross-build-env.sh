#!/usr/bin/env bash
#
# One-time setup for cross-compiling RPCEmu (Spork Edition) for macOS FROM LINUX
# via osxcross. This builds the osxcross toolchain and cross-builds the two
# dependencies the emulator links (wxWidgets + SDL2) for BOTH macOS arches
# (x86_64 + arm64) into per-arch prefixes.
#
# ############################################################################
# # EXPERIMENTAL / UNVERIFIED path.                                          #
# #   * A Linux host cannot execute the Mach-O binaries this produces, so    #
# #     nothing here is runtime-tested. Real verification is the GitHub      #
# #     macOS CI jobs (native runners). Use this only for fast local         #
# #     compile iteration.                                                   #
# #   * It WILL likely need tweaking for your exact SDK / osxcross version.  #
# ############################################################################
#
# YOU MUST PROVIDE A macOS SDK. It cannot be downloaded automatically (it comes
# from Xcode, behind an Apple ID). On a Mac (or from an Xcode.xip) produce e.g.
# MacOSX14.sdk.tar.xz - see osxcross' README "packaging the SDK" - and drop it
# into:   ./macos-sdk/
# An SDK >= 11.x is required for the arm64 slice.
#
# Usage:
#   1.  mkdir -p macos-sdk && cp /path/to/MacOSX14.sdk.tar.xz macos-sdk/
#   2.  ./setup-macos-cross-build-env.sh
#   3.  source ./macos-cross-env.sh      # exports OSXCROSS_ROOT + deps prefixes
#   4.  ./build-macos.sh                 # cross-build both slices + lipo
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

WORK="$SCRIPT_DIR/.macos-cross"
OSXCROSS_SRC="$WORK/osxcross"
SDK_DIR="$SCRIPT_DIR/macos-sdk"
WX_VERSION="3.2.6"
SDL2_VERSION="2.30.9"

msg() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# --- host prerequisites --------------------------------------------------
for tool in git clang cmake make patch python3 xz; do
	command -v "$tool" >/dev/null || die "missing host tool '$tool' (apt install clang cmake git make patch python3 xz-utils libssl-dev liblzma-dev)"
done

# --- locate the user-provided SDK ---------------------------------------
shopt -s nullglob
SDKS=("$SDK_DIR"/MacOSX*.sdk.tar.* )
shopt -u nullglob
[ "${#SDKS[@]}" -ge 1 ] || die "no macOS SDK found in $SDK_DIR (see header: drop MacOSX<NN>.sdk.tar.xz there)"
SDK_TARBALL="${SDKS[0]}"
msg "Using macOS SDK: $(basename "$SDK_TARBALL")"

# --- clone + build osxcross ---------------------------------------------
mkdir -p "$WORK"
if [ ! -d "$OSXCROSS_SRC" ]; then
	msg "Cloning osxcross"
	git clone --depth 1 https://github.com/tpoechtrager/osxcross "$OSXCROSS_SRC"
fi
cp -f "$SDK_TARBALL" "$OSXCROSS_SRC/tarballs/"

msg "Building osxcross toolchain (this takes a while)"
( cd "$OSXCROSS_SRC" && UNATTENDED=1 ./build.sh )

export OSXCROSS_ROOT="$OSXCROSS_SRC/target"
export PATH="$OSXCROSS_ROOT/bin:$PATH"
[ -x "$OSXCROSS_ROOT/bin/o64-clang" ]  || die "osxcross build did not produce o64-clang"
[ -x "$OSXCROSS_ROOT/bin/oa64-clang" ] || die "osxcross build did not produce oa64-clang (SDK too old for arm64?)"

# darwinNN target triple suffix (e.g. darwin23), from osxcross-conf.
eval "$("$OSXCROSS_ROOT/bin/osxcross-conf")"
DARWIN="${OSXCROSS_TARGET:?}"

DEPS_X8664="$WORK/deps-x86_64"
DEPS_ARM64="$WORK/deps-arm64"

# --- fetch dependency sources -------------------------------------------
fetch() { # url dest
	local url="$1" dest="$2"
	[ -f "$dest" ] || { msg "Downloading $(basename "$dest")"; curl -fL "$url" -o "$dest"; }
}
mkdir -p "$WORK/src"
fetch "https://github.com/wxWidgets/wxWidgets/releases/download/v${WX_VERSION}/wxWidgets-${WX_VERSION}.tar.bz2" \
      "$WORK/src/wxWidgets-${WX_VERSION}.tar.bz2"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz" \
      "$WORK/src/SDL2-${SDL2_VERSION}.tar.gz"

# --- per-arch dependency cross-build ------------------------------------
build_deps_for_arch() {
	local arch="$1" prefix cc cxx triple
	if [ "$arch" = x86_64 ]; then
		prefix="$DEPS_X8664"; cc=o64-clang; cxx=o64-clang++; triple="x86_64-apple-${DARWIN}"
	else
		prefix="$DEPS_ARM64"; cc=oa64-clang; cxx=oa64-clang++; triple="arm64-apple-${DARWIN}"
	fi
	mkdir -p "$prefix"

	# SDL2 (cmake, static) --------------------------------------------------
	msg "[$arch] cross-building SDL2 $SDL2_VERSION"
	rm -rf "$WORK/build-sdl2-$arch" "$WORK/src/SDL2-${SDL2_VERSION}"
	tar -C "$WORK/src" -xzf "$WORK/src/SDL2-${SDL2_VERSION}.tar.gz"
	cmake -S "$WORK/src/SDL2-${SDL2_VERSION}" -B "$WORK/build-sdl2-$arch" \
		-DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/osxcross-$arch.cmake" \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		-DSDL_SHARED=OFF -DSDL_STATIC=ON -DCMAKE_BUILD_TYPE=Release
	cmake --build "$WORK/build-sdl2-$arch" --target install -j"$(nproc)"

	# wxWidgets (autotools, static, Cocoa) ---------------------------------
	msg "[$arch] cross-building wxWidgets $WX_VERSION (wxOSX/Cocoa)"
	rm -rf "$WORK/wx-${WX_VERSION}-$arch"
	mkdir -p "$WORK/wx-${WX_VERSION}-$arch"
	tar -C "$WORK/wx-${WX_VERSION}-$arch" --strip-components=1 \
		-xjf "$WORK/src/wxWidgets-${WX_VERSION}.tar.bz2"
	(
		cd "$WORK/wx-${WX_VERSION}-$arch"
		mkdir -p build-cross && cd build-cross
		CC="$cc" CXX="$cxx" \
		../configure --host="$triple" --build="$(gcc -dumpmachine)" \
			--with-osx_cocoa --disable-shared --disable-sys-libs \
			--without-liblzma --prefix="$prefix"
		make -j"$(nproc)"
		make install
	)
}

build_deps_for_arch x86_64
build_deps_for_arch arm64

# --- write the env file the build reads ---------------------------------
cat > "$SCRIPT_DIR/macos-cross-env.sh" <<EOF
# Source this before ./build-macos.sh to cross-build the macOS slices.
export OSXCROSS_ROOT="$OSXCROSS_ROOT"
export OSXCROSS_DEPS_X8664="$DEPS_X8664"
export OSXCROSS_DEPS_ARM64="$DEPS_ARM64"
export PATH="\$OSXCROSS_ROOT/bin:$DEPS_X8664/bin:$DEPS_ARM64/bin:\$PATH"
EOF

msg "Done. Next:"
echo "    source ./macos-cross-env.sh"
echo "    ./build-macos.sh          # cross-build both slices + lipo -> releases/macos/universal/"
echo
echo "Reminder: this cross build is UNVERIFIED (can't run Mach-O on Linux)."
echo "Push to trigger the GitHub macOS CI jobs for real build+test coverage."
