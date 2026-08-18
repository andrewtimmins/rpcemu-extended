cmake_minimum_required(VERSION 3.16)

# The release number. project() and the Debian package need a bare x.y.z, so
# this stays exactly what the file says.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" RPCEMU_RELEASE)
string(STRIP "${RPCEMU_RELEASE}" RPCEMU_RELEASE)

project(rpcemu VERSION ${RPCEMU_RELEASE} LANGUAGES C)

# What the emulator reports and what the archives are named. The number is
# always the one in VERSION; git only says which commit this is, so that a build
# from anywhere but a release tag cannot be mistaken for the release itself.
# Nothing is inferred from the tags, so a clone that is missing some cannot
# quietly report an older release than the source says it is.
#
# Released builds are the exception: on a release tag the number stands alone,
# and the workflow refuses to publish a tag that disagrees with VERSION.
set(RPCEMU_VERSION "${RPCEMU_RELEASE}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _rpcemu_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rpcemu_commit_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_QUIET ERROR_QUIET
        RESULT_VARIABLE _rpcemu_on_tag)
    if(_rpcemu_commit_result EQUAL 0 AND _rpcemu_commit AND NOT _rpcemu_on_tag EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _rpcemu_dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_rpcemu_dirty)
            set(RPCEMU_VERSION "${RPCEMU_RELEASE}-g${_rpcemu_commit}-dirty")
        else()
            set(RPCEMU_VERSION "${RPCEMU_RELEASE}-g${_rpcemu_commit}")
        endif()
    endif()
endif()
message(STATUS "RPCEmu version: ${RPCEMU_VERSION} (release ${RPCEMU_RELEASE})")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(RPCEMU_DYNAREC "Build with ARM dynamic recompiler" ON)
option(RPCEMU_ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(RPCEMU_ENABLE_GHOSTPDL "Enable in-process Ghostscript PDF conversion" ON)
option(RPCEMU_PROFILE "Sample the emulator thread and log where its time goes (development tool)" OFF)
if(RPCEMU_PROFILE)
    # It classifies a sample by whether it landed in the recompiler's code cache,
    # so there has to be one. Said here rather than as a link error later.
    if(NOT RPCEMU_DYNAREC)
        message(FATAL_ERROR
            "RPCEMU_PROFILE needs the recompiler: it tells recompiled code from C "
            "by the code cache's address range, which an interpreter build has "
            "not got. Configure without --interpreter, or with "
            "-DRPCEMU_PROFILE=OFF.")
    endif()
    add_compile_definitions(RPCEMU_PROFILE)
    # Debug info, without changing the optimiser: the profiler reports file
    # offsets and they are resolved with addr2line afterwards. Without -g that
    # resolution falls back to the nearest preceding symbol, which silently names
    # the wrong function - so a profiling build always carries it.
    add_compile_options(-g)
endif()

option(RPCEMU_ENABLE_VNC "Enable VNC server support (requires libvncserver)" ON)
option(RPCEMU_BUILD_TESTS "Build unit tests (amd64 dynarec only)" ON)
# Off by default so a casual build still works without libusb. Release builds
# turn it on, so a platform losing USB passthrough fails the build rather than
# shipping quietly without it.
option(RPCEMU_REQUIRE_LIBUSB "Fail the build if libusb-1.0 is missing" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "RPCEmu Extended supports Linux, Windows (MinGW-w64), and macOS.")
endif()

if(WIN32 AND MSVC)
    message(FATAL_ERROR "RPCEmu Extended on Windows requires MinGW-w64, not MSVC.")
endif()

if(RPCEMU_ENABLE_WARNINGS)
    # These are GCC/Clang spellings; MinGW-w64 accepts them unchanged. Guard
    # against a hypothetical MSVC configure anyway.
    if(NOT MSVC)
        # -Werror=implicit-function-declaration: catch missing declarations on
        # all toolchains. GCC 14 (e.g. current MSYS2) already errors on these by
        # default; making it explicit means older GCC/Clang catch them too. C
        # only: GCC warns once per C++ source file that the flag does not apply.
        add_compile_options(-Wall -Wextra -Werror=switch -fno-common
                            $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
    endif()
endif()

if(RPCEMU_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(RPCEMU_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(DIRECTORY configs/ DESTINATION share/rpcemu/configs)
install(DIRECTORY resources/ DESTINATION share/rpcemu/resources)
install(DIRECTORY poduleroms/ DESTINATION share/rpcemu/poduleroms)
install(DIRECTORY netroms/ DESTINATION share/rpcemu/netroms)
# The graphics card's display driver, carried in that card's own ROM. Kept out of
# poduleroms/, which is scanned into the general-purpose expansion card and would
# present the module a second time.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gfxroms)
    install(DIRECTORY gfxroms/ DESTINATION share/rpcemu/gfxroms)
endif()
# RISC OS's own USB stack, carried in the USB card's ROM for the same reason.
install(DIRECTORY usbroms/ DESTINATION share/rpcemu/usbroms)
install(DIRECTORY roms/ DESTINATION share/rpcemu/roms)
# Expansion-card (podule) ROMs are shipped system components, read from the
# shared resource dir; users select which podules to use per machine config.
install(DIRECTORY podules/ DESTINATION share/rpcemu/podules)
# Factory seed used to populate new machines (cmos.ram + hostfs).
install(DIRECTORY default/ DESTINATION share/rpcemu/default)
# Ship a ready-to-run Default machine. Use the tracked machines/Default if present
# (local dev), otherwise derive it from the default/ seed (fresh clone / CI).
if(EXISTS "${CMAKE_SOURCE_DIR}/machines/Default")
    install(DIRECTORY machines/Default/ DESTINATION share/rpcemu/machines/Default)
else()
    install(DIRECTORY default/ DESTINATION share/rpcemu/machines/Default)
endif()
# MCP server (Python): drive a RISC OS machine from an MCP client, including
# the debugger control socket. See tools/mcp/README.md.
install(DIRECTORY tools/mcp/ DESTINATION share/rpcemu/mcp
        PATTERN "__pycache__" EXCLUDE
        PATTERN "setup-mcp-env.ps1" EXCLUDE)
install(FILES README.md COPYING COMPILE.md DESTINATION share/doc/rpcemu)
install(DIRECTORY docs/ DESTINATION share/doc/rpcemu)
# The .desktop launcher's Exec must match the actual binary (rpcemu-recompiler
# on x86 dynarec builds, rpcemu-interpreter on arm64/interpreter builds); it is
# configured and installed from src/gui/CMakeLists.txt where the name is known.

set(CPACK_PACKAGE_NAME "rpcemu")
set(CPACK_PACKAGE_VENDOR "Andy Timmins")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RPCEmu Extended")
set(CPACK_PACKAGE_DESCRIPTION "Acorn RiscPC and A7000 emulator with HostFS, networking and a good deal more.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "andrew@example.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/andrewtimmins/rpcemu-extended")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/COPYING")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Andy Timmins")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# Derive the runtime dependencies automatically from the linked binary via
# dpkg-shlibdeps. This picks the exact package names for this distro (e.g. the
# libwxgtk3.2-1t64 rename on Ubuntu 24.04) and includes everything the binary
# links - wxWidgets, SDL2, libvncserver, Ghostscript (libgs), GTK 3, etc.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
