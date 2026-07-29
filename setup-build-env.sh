#!/bin/bash
# RPCEmu (Spork Edition) - Linux build environment setup
#
# Usage:
#   ./setup-build-env.sh              # Native Linux build dependencies
#   ./setup-build-env.sh --cross-arm64 # + aarch64 cross-compiler (from amd64 host)
#   ./setup-build-env.sh --podules    # + ARM binutils for podule ROM rebuilds

set -e

INSTALL_CROSS_ARM64=false
INSTALL_PODULES=false

for arg in "$@"; do
	case $arg in
		--cross-arm64) INSTALL_CROSS_ARM64=true ;;
		--podules|-p) INSTALL_PODULES=true ;;
		--help|-h)
			echo "Usage: $0 [options]"
			echo ""
			echo "  --cross-arm64   Install Linux arm64 cross-compiler (from amd64 host)"
			echo "  --podules, -p   Install ARM binutils for HostFS podule ROM rebuilds"
			exit 0
			;;
	esac
done

echo "=================================================="
echo "RPCEmu - Linux Build Environment Setup"
echo "=================================================="
echo ""

if ! command -v apt &>/dev/null; then
	echo "Error: This script supports Debian/Ubuntu only."
	exit 1
fi

echo "Detected $(uname -s) $(uname -m)"
echo ""

echo "Updating package lists..."
sudo apt update

echo ""
echo "Installing build tools, CMake, wxWidgets, SDL2, ALSA, VNC, and libusb..."
sudo apt install -y \
	build-essential \
	cmake \
	pkg-config \
	libwxgtk3.2-dev \
	libgtk-3-dev \
	libsdl2-dev \
	libasound2-dev \
	libgs-dev \
	ghostscript \
	libvncserver-dev \
	libusb-1.0-0-dev

echo ""
echo "✓ Linux build environment ready"
echo ""
echo "Build with:"
echo "  ./build.sh"
echo "  ./build.sh --deb --zip"
echo ""

if [ "$INSTALL_CROSS_ARM64" = true ]; then
	echo "Installing Linux arm64 cross-compilation tools..."
	sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
	echo ""
	echo "For cross-compiled wxWidgets builds you may also need arm64 dev libraries:"
	echo "  sudo dpkg --add-architecture arm64"
	echo "  sudo apt update"
	echo "  sudo apt install libwxgtk3.2-dev:arm64 libsdl2-dev:arm64 libasound2-dev:arm64 libvncserver-dev:arm64 libusb-1.0-0-dev:arm64"
fi

if [ "$INSTALL_PODULES" = true ]; then
	echo ""
	echo "Installing ARM binutils for podule ROM builds..."
	sudo apt install -y binutils-arm-linux-gnueabi || {
		echo "Warning: binutils-arm-linux-gnueabi not available"
	}
fi

echo ""
echo "=================================================="
echo "Setup complete!"
echo "=================================================="
