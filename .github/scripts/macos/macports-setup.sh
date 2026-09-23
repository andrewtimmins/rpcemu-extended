#!/usr/bin/env bash
#
# Install MacPorts and the libraries a macOS slice links against.
#
# MacPorts publishes binary archives for both architectures, and installs to
# /opt/local on either - so both slices link against libraries from one place,
# which is what lets the fuse step in build-macos.sh compare them by path.
#
# It is not in the runner images, so it is installed here.
#
# ★ The environment is published to GITHUB_PATH and GITHUB_ENV, not just
# exported. Each step of a job is its own shell, so an export here reaches
# nothing: the build step would find Homebrew's pkg-config first, which knows
# nothing of /opt/local/lib/pkgconfig, and configuring would fail on a package
# that is installed.
#
# Usage: macports-setup.sh <os-tag>      e.g. 15-Sequoia
set -euo pipefail

OS_TAG="${1:?usage: macports-setup.sh <os-tag>, e.g. 15-Sequoia}"
MACPORTS_VERSION="${MACPORTS_VERSION:-2.12.6}"

PKG="MacPorts-${MACPORTS_VERSION}-${OS_TAG}.pkg"
URL="https://github.com/macports/macports-base/releases/download/v${MACPORTS_VERSION}/${PKG}"

export PATH="/opt/local/bin:/opt/local/sbin:$PATH"

if [ ! -x /opt/local/bin/port ]; then
	echo "==> Installing MacPorts ${MACPORTS_VERSION} (${OS_TAG})"
	# Retried: this is a GitHub release download, and a failure here fails the
	# whole job rather than one library.
	for attempt in 1 2 3; do
		curl -fsSL --retry 3 -o "/tmp/${PKG}" "$URL" && break
		echo "download failed (attempt $attempt of 3)"
		[ "$attempt" = 3 ] && exit 1
		sleep $((attempt * 10))
	done
	sudo installer -pkg "/tmp/${PKG}" -target /
fi

port version

# Bring the port tree up to date. Not fatal: the worst a stale tree does is
# install slightly older ports, which are the ones most likely to have archives
# built for them anyway.
for attempt in 1 2 3; do
	sudo port -q selfupdate && break
	echo "port selfupdate failed (attempt $attempt of 3)"
	if [ "$attempt" = 3 ]; then
		echo "::warning::port selfupdate failed three times; using the tree as shipped"
		break
	fi
	sleep $((attempt * 15))
done

# -N: never prompt. Binary archives are used where they exist, which is the
# point of using MacPorts here; a port without one is built from source and the
# job simply takes longer.
echo "==> Installing ports"
sudo port -N install \
	wxWidgets-3.2 \
	libsdl2 \
	LibVNCServer \
	libusb \
	cmake \
	ninja \
	pkgconfig

# wxWidgets installs as a framework and its wx-config is reached through
# port select; without this there is no wx-config on PATH at all.
sudo port select --set wxWidgets wxWidgets-3.2

echo "==> Installed versions"
port installed wxWidgets-3.2 libsdl2 LibVNCServer libusb cmake ninja pkgconfig

# Hand the environment to the rest of the job. PATH first, so /opt/local/bin
# wins over the Homebrew binaries in the runner image - cmake and pkg-config
# both exist in each, and the two prefixes know nothing of each other's
# packages.
if [ -n "${GITHUB_PATH:-}" ]; then
	printf '%s\n' /opt/local/bin /opt/local/sbin >> "$GITHUB_PATH"
fi
if [ -n "${GITHUB_ENV:-}" ]; then
	printf 'PKG_CONFIG_PATH=%s\n' /opt/local/lib/pkgconfig >> "$GITHUB_ENV"
fi

# Recorded because wxWidgets installs as a framework and the build script finds
# it through these. Worth having in the log when a build fails.
echo "==> wx-config"
command -v wx-config || echo "   ! wx-config is not on PATH"
wx-config --prefix || true
wx-config --version || true
wx-config --libs || true

# The libraries have to be findable by the tools the build will actually use,
# which is not the same as having installed them: the runner has a Homebrew
# pkg-config too, and asking that one about a MacPorts package gets "not
# found" from a package that is plainly installed. Ask here, where the answer
# names the cause, rather than letting CMake fail three steps later.
echo "==> Checking the MacPorts tools answer for the MacPorts packages"
export PKG_CONFIG_PATH=/opt/local/lib/pkgconfig
missing=0
for mod in sdl2 libvncserver libusb-1.0; do
	if /opt/local/bin/pkg-config --exists "$mod"; then
		printf '  %-14s %s\n' "$mod" "$(/opt/local/bin/pkg-config --modversion "$mod")"
	else
		echo "::error::pkg-config cannot find '$mod' in /opt/local/lib/pkgconfig"
		missing=1
	fi
done
[ "$missing" -eq 0 ] || exit 1
