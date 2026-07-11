# CMake toolchain: cross-compile the x86_64 macOS slice from Linux via osxcross.
#
# This is the UNTESTED local-iteration path (a Linux host cannot execute the
# resulting Mach-O; real verification happens on the GitHub macOS runners).
# Prerequisite: run ./setup-macos-cross-build-env.sh, which builds osxcross and
# the per-arch dependencies, then exports the env vars this file reads.
#
#   OSXCROSS_ROOT       - osxcross "target" dir (contains bin/o64-clang, ...)
#   OSXCROSS_DEPS_X8664 - prefix holding the x86_64 cross-built wxWidgets/SDL2
#                         (its bin/wx-config, lib/, include/)

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if("$ENV{OSXCROSS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "OSXCROSS_ROOT not set - run ./setup-macos-cross-build-env.sh first")
endif()
set(_ox "$ENV{OSXCROSS_ROOT}")

# osxcross ships stable, darwin-version-independent wrapper names.
set(CMAKE_C_COMPILER   "${_ox}/bin/o64-clang")
set(CMAKE_CXX_COMPILER "${_ox}/bin/o64-clang++")

# Search cross libraries/headers under the osxcross SDK and our deps prefix,
# but always take programs (wx-config, etc.) from the host toolchain dirs.
set(CMAKE_FIND_ROOT_PATH "${_ox}/SDK" "$ENV{OSXCROSS_DEPS_X8664}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM SEARCH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Let the emulator's FindWxWidgets.cmake cross path pick up the cross wx-config,
# and point pkg-config (SDL2) at the cross deps prefix.
if(NOT "$ENV{OSXCROSS_DEPS_X8664}" STREQUAL "")
    set(wxWidgets_CONFIG_EXECUTABLE "$ENV{OSXCROSS_DEPS_X8664}/bin/wx-config"
        CACHE FILEPATH "cross wx-config (x86_64)")
    set(ENV{PKG_CONFIG_LIBDIR} "$ENV{OSXCROSS_DEPS_X8664}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "$ENV{OSXCROSS_DEPS_X8664}/lib/pkgconfig")
endif()
