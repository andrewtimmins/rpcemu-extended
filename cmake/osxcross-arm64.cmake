# CMake toolchain: cross-compile the arm64 macOS slice from Linux via osxcross.
#
# UNTESTED local-iteration path (see cmake/osxcross-x86_64.cmake for the full
# note). The arm64 slice is interpreter-only - there is no ARM dynarec backend,
# so build-macos.sh configures this slice with RPCEMU_DYNAREC=OFF.
#
# Reads (exported by ./setup-macos-cross-build-env.sh):
#   OSXCROSS_ROOT       - osxcross "target" dir (contains bin/oa64-clang, ...)
#   OSXCROSS_DEPS_ARM64 - prefix holding the arm64 cross-built wxWidgets/SDL2

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)

if("$ENV{OSXCROSS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "OSXCROSS_ROOT not set - run ./setup-macos-cross-build-env.sh first")
endif()
set(_ox "$ENV{OSXCROSS_ROOT}")

# osxcross arm64 wrappers.
set(CMAKE_C_COMPILER   "${_ox}/bin/oa64-clang")
set(CMAKE_CXX_COMPILER "${_ox}/bin/oa64-clang++")

set(CMAKE_FIND_ROOT_PATH "${_ox}/SDK" "$ENV{OSXCROSS_DEPS_ARM64}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM SEARCH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT "$ENV{OSXCROSS_DEPS_ARM64}" STREQUAL "")
    set(wxWidgets_CONFIG_EXECUTABLE "$ENV{OSXCROSS_DEPS_ARM64}/bin/wx-config"
        CACHE FILEPATH "cross wx-config (arm64)")
    set(ENV{PKG_CONFIG_LIBDIR} "$ENV{OSXCROSS_DEPS_ARM64}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "$ENV{OSXCROSS_DEPS_ARM64}/lib/pkgconfig")
endif()
