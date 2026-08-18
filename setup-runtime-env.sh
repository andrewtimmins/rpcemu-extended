#!/bin/bash
# RPCEmu Extended - runtime dependency setup
#
# Run this ONCE if you downloaded the portable .tar.gz release and get an error
# like "error while loading shared libraries: libwx_gtk3u_core-3.2.so.0" when
# launching rpcemu-recompiler. It installs the shared libraries the prebuilt
# binary needs to run (NOT the build tools - use setup-build-env.sh for those).
#
# If you installed the .deb instead, apt already pulled these in automatically.

set -e

if ! command -v apt &>/dev/null; then
	echo "This helper supports Debian/Ubuntu."
	echo ""
	echo "On other distributions, install the runtime equivalents of:"
	echo "  - wxWidgets 3.2 (GTK 3 build)"
	echo "  - SDL2"
	echo "  - libvncserver"
	echo "  - Ghostscript (libgs)"
	echo "  - GTK 3"
	exit 1
fi

echo "=================================================="
echo "RPCEmu - Runtime Library Setup"
echo "=================================================="
echo ""
echo "Updating package lists..."
sudo apt update

echo ""
echo "Installing runtime libraries..."

# wxWidgets runtime. The package was renamed for the 64-bit time_t transition
# (Ubuntu 24.04+), so try the new name first and fall back to the old one.
sudo apt install -y libwxgtk3.2-1t64 || sudo apt install -y libwxgtk3.2-1

sudo apt install -y \
	libsdl2-2.0-0 \
	libvncserver1 \
	libgtk-3-0

# Ghostscript runtime (used for parallel-port "print to PDF"). Try the library
# package, then fall back to the full ghostscript package.
sudo apt install -y libgs10 || sudo apt install -y ghostscript || true

echo ""
echo "=================================================="
echo "Done. Launch the emulator with:"
echo "  ./rpcemu-recompiler"
echo "=================================================="
