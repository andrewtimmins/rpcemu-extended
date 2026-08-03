#!/usr/bin/env bash
#
# Set up a Linux host to cross-compile RPCEmu (Extended Edition) for Windows
# (x86-64) with MinGW-w64.
#
# The native Windows build (see CI) uses MSYS2, which ships wxWidgets/SDL2/
# libvncserver as packages. Debian/Ubuntu/Mint do NOT package those for the
# mingw target, so this script builds them from source and installs them into
# the mingw sysroot (/usr/x86_64-w64-mingw32). After running it:
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DRPCEMU_BUILD_TESTS=OFF -DRPCEMU_ENABLE_GHOSTPDL=OFF
#   cmake --build build-win -j
#
# Prerequisites (install via apt): the MinGW-w64 toolchain, cmake, curl.
#   sudo apt install mingw-w64 cmake curl
#
# Requires sudo (installs into a system directory). Idempotent-ish: re-running
# rebuilds and reinstalls.
set -euo pipefail

TARGET=x86_64-w64-mingw32
PREFIX=/usr/${TARGET}
JOBS=$(nproc)
HERE=$(cd "$(dirname "$0")" && pwd)
TC="${HERE}/cmake/mingw-w64-x86_64.cmake"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Versions (match the project's Linux/MSYS2 builds where it matters).
ZLIB=1.3.1
LIBPNG=1.6.43
JPEG=3.0.3
LIBVNC=0.9.14
SDL2=2.30.9
WX=3.2.6
LIBUSB=1.0.28

echo "==> Cross-build prefix: ${PREFIX}"
command -v ${TARGET}-gcc >/dev/null || { echo "error: ${TARGET}-gcc not found (apt install mingw-w64)"; exit 1; }
command -v cmake >/dev/null || { echo "error: cmake not found"; exit 1; }

cd "$WORK"
fetch() { echo "==> fetch $1"; curl -fsSL "$1" -o "$2"; }

fetch "https://github.com/madler/zlib/releases/download/v${ZLIB}/zlib-${ZLIB}.tar.gz" zlib.tar.gz
fetch "https://download.sourceforge.net/libpng/libpng-${LIBPNG}.tar.gz" libpng.tar.gz
fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${JPEG}/libjpeg-turbo-${JPEG}.tar.gz" jpeg.tar.gz
fetch "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-${LIBVNC}.tar.gz" vnc.tar.gz
fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2}/SDL2-devel-${SDL2}-mingw.tar.gz" sdl2.tar.gz
fetch "https://github.com/wxWidgets/wxWidgets/releases/download/v${WX}/wxWidgets-${WX}.tar.bz2" wx.tar.bz2
fetch "https://github.com/libusb/libusb/releases/download/v${LIBUSB}/libusb-${LIBUSB}.tar.bz2" libusb.tar.bz2
for t in *.tar.gz; do tar xzf "$t"; done
for t in *.tar.bz2; do tar xjf "$t"; done

cmake_dep() { # <srcdir> <extra cmake args...>
	local src="$1"; shift
	cmake -S "$src" -B "$src/b" -G "Unix Makefiles" \
		-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" "$@"
	cmake --build "$src/b" -j"$JOBS"
	sudo cmake --install "$src/b"
}

echo "==> SDL2 (prebuilt mingw dev package)"
sudo make -C "SDL2-${SDL2}" install-package arch=${TARGET} prefix="$PREFIX"

echo "==> zlib"
make -C "zlib-${ZLIB}" -f win32/Makefile.gcc PREFIX=${TARGET}- -j"$JOBS"
sudo make -C "zlib-${ZLIB}" -f win32/Makefile.gcc install PREFIX=${TARGET}- \
	BINARY_PATH="$PREFIX/bin" INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" SHARED_MODE=1
# zlib's win32 makefile ships no pkg-config file; write one.
sudo tee "$PREFIX/lib/pkgconfig/zlib.pc" >/dev/null <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library
Version: ${ZLIB}
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF

echo "==> libjpeg-turbo"
cmake_dep "libjpeg-turbo-${JPEG}" -DENABLE_SHARED=ON -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF

echo "==> libpng"
cmake_dep "libpng-${LIBPNG}" -DPNG_TESTS=OFF -DPNG_SHARED=ON -DPNG_STATIC=ON -DZLIB_ROOT="$PREFIX"

echo "==> libvncserver"
cmake_dep "libvncserver-LibVNCServer-${LIBVNC}" \
	-DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF -DWITH_GNUTLS=OFF -DWITH_OPENSSL=OFF \
	-DWITH_SDL=OFF -DWITH_GTK=OFF -DWITH_FFMPEG=OFF -DWITH_SYSTEMD=OFF \
	-DWITH_LZO=OFF -DWITH_SASL=OFF -DBUILD_SHARED_LIBS=ON

# libusb gives USB passthrough (src/usb_host.c). Without it the cross-built
# Windows binary still runs, but the emulated USB card has nothing to plug into
# it, and build-windows.sh now fails rather than shipping that quietly.
# Autotools rather than cmake_dep: libusb's own configure emits the
# libusb-1.0.pc that pkg_check_modules looks for.
echo "==> libusb"
( cd "libusb-${LIBUSB}" && ./configure --host=${TARGET} --prefix="$PREFIX" \
	--enable-shared --disable-static --disable-udev &&
  make -j"$JOBS" && sudo make install )

echo "==> wxWidgets (wxMSW, static, bundled image/zlib/expat libs)"
( cd "wxWidgets-${WX}" && mkdir -p build-mingw && cd build-mingw &&
  ../configure --host=${TARGET} --build="$(../config.guess)" --prefix="$PREFIX" \
	--disable-shared --enable-unicode \
	--with-libpng=builtin --with-zlib=builtin --with-libjpeg=builtin \
	--with-expat=builtin --with-regex=builtin --without-libtiff \
	--disable-tests --disable-precomp-headers &&
  make -j"$JOBS" && sudo make install )

echo
echo "All cross dependencies installed into ${PREFIX}."
echo "Now configure + build:"
echo "  cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DRPCEMU_BUILD_TESTS=OFF -DRPCEMU_ENABLE_GHOSTPDL=OFF"
echo "  cmake --build build-win -j"
