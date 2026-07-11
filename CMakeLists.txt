cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" RPCEMU_VERSION)
string(STRIP "${RPCEMU_VERSION}" RPCEMU_VERSION)

project(rpcemu VERSION ${RPCEMU_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(RPCEMU_DYNAREC "Build with ARM dynamic recompiler" ON)
option(RPCEMU_ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(RPCEMU_ENABLE_GHOSTPDL "Enable in-process Ghostscript PDF conversion" ON)
option(RPCEMU_ENABLE_VNC "Enable VNC server support (requires libvncserver)" ON)
option(RPCEMU_BUILD_TESTS "Build unit tests (amd64 dynarec only)" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "RPCEmu (Spork Edition) supports Linux, Windows (MinGW-w64), and macOS.")
endif()

if(WIN32 AND MSVC)
    message(FATAL_ERROR "RPCEmu (Spork Edition) on Windows requires MinGW-w64, not MSVC.")
endif()

if(RPCEMU_ENABLE_WARNINGS)
    # These are GCC/Clang spellings; MinGW-w64 accepts them unchanged. Guard
    # against a hypothetical MSVC configure anyway.
    if(NOT MSVC)
        # -Werror=implicit-function-declaration: catch missing declarations on
        # all toolchains. GCC 14 (e.g. current MSYS2) already errors on these by
        # default; making it explicit means older GCC/Clang catch them too.
        add_compile_options(-Wall -Wextra -Werror=switch
                            -Werror=implicit-function-declaration -fno-common)
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
        PATTERN "__pycache__" EXCLUDE)
install(FILES README.md COPYING COMPILE.md DESTINATION share/doc/rpcemu)
install(DIRECTORY docs/ DESTINATION share/doc/rpcemu)
# The .desktop launcher's Exec must match the actual binary (rpcemu-recompiler
# on x86 dynarec builds, rpcemu-interpreter on arm64/interpreter builds); it is
# configured and installed from src/gui/CMakeLists.txt where the name is known.

set(CPACK_PACKAGE_NAME "rpcemu")
set(CPACK_PACKAGE_VENDOR "Andrew Timmins")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RPCEmu (Spork Edition)")
set(CPACK_PACKAGE_DESCRIPTION "Acorn RiscPC and A7000 emulator with HostFS, networking, and Spork Edition enhancements.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "andrew@example.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/andrewtimmins/rpcemu-extended")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/COPYING")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Andrew Timmins")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# Derive the runtime dependencies automatically from the linked binary via
# dpkg-shlibdeps. This picks the exact package names for this distro (e.g. the
# libwxgtk3.2-1t64 rename on Ubuntu 24.04) and includes everything the binary
# links - wxWidgets, SDL2, libvncserver, Ghostscript (libgs), GTK 3, etc.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
