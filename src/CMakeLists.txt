set(RPCEMU_CORE_SOURCES
    superio.c
    parallel.c
    serial.c
    printer.c
    print_convert.c
    peripheral_config.c
    cdrom-iso.c
    cmos.c
    cp15.c
    fdc.c
    fpa.c
    hostfs.c
    hostfs-unix.c
    hostcmd.c
    debugcmd.c
    ide.c
    iomd.c
    keyboard.c
    mem.c
    romload.c
    rpcemu.c
    sound.c
    vidc20.c
    podules.c
    podule_config.c
    podules/aka05.c
    podules/midimax.c
    podules/aka16.c
    podules/aka12.c
    podules/lark.c
    podules/common/n16550.c
    podules/common/scc2691.c
    podules/common/6522.c
    podules/ad1848.c
    podules/am7202a.c
    podules/common/sound_out_sdl2.c
    podulerom.c
    icside.c
    rpc.c
    rpc-machdep.c
    arm_common.c
    arm_disasm.c
    i8042.c
    disc.c
    disc_adf.c
    disc_hfe.c
    disc_mfm_common.c
    network.c
    network-nat.c
    broadcast_relay.c
    slirp/bootp.c
    slirp/cksum.c
    slirp/cutils.c
    slirp/if.c
    slirp/ip_icmp.c
    slirp/ip_input.c
    slirp/ip_output.c
    slirp/mbuf.c
    slirp/misc.c
    slirp/sbuf.c
    slirp/slirp.c
    slirp/socket.c
    slirp/tcp_input.c
    slirp/tcp_output.c
    slirp/tcp_subr.c
    slirp/tcp_timer.c
    slirp/udp.c
)

if(RPCEMU_DYNAREC)
    list(APPEND RPCEMU_CORE_SOURCES arm_dynarec.c)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        list(APPEND RPCEMU_CORE_SOURCES codegen_amd64.c)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386|i686|X86")
        list(APPEND RPCEMU_CORE_SOURCES codegen_x86.c)
    else()
        message(FATAL_ERROR "No dynarec backend for processor ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    list(APPEND RPCEMU_CORE_SOURCES arm.c codegen_null.c)
endif()

# Host-platform backends. Ethernet bridging / IP tunnelling (network-tun.c) go
# through /dev/net/tun and the Linux CD-ROM ioctl backend (cdrom-ioctl.c) are
# Linux-only. On Windows and macOS use the null network platform layer (NAT
# still works via SLiRP) and the portable ISO-file CD-ROM backend (cdrom-iso.c,
# listed above).
if(WIN32 OR APPLE)
    list(APPEND RPCEMU_CORE_SOURCES network-null.c)
else()
    list(APPEND RPCEMU_CORE_SOURCES network-tun.c cdrom-ioctl.c)
endif()

add_library(rpcemu_core STATIC ${RPCEMU_CORE_SOURCES})

target_include_directories(rpcemu_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/slirp
)

# Shared podule support code (UART, MIDI interface, ...) lives in podules/common,
# so device sources can include those headers by name (e.g. "n16550.h").
target_include_directories(rpcemu_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/podules/common
)

target_compile_definitions(rpcemu_core PUBLIC
    CONFIG_SLIRP
    RPCEMU_VERSION="${PROJECT_VERSION}"
)

target_compile_definitions(rpcemu_core PRIVATE _DEFAULT_SOURCE)
target_compile_options(rpcemu_core PRIVATE -std=gnu11)

# MinGW defaults printf/scanf to the MSVCRT implementations, which lack C99
# conversions such as %zu and %lld. Select MinGW's ANSI-stdio implementations
# (public: affects headers included by GUI/tools translation units too).
if(WIN32)
    target_compile_definitions(rpcemu_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(rpcemu_core PUBLIC _DEBUG)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|ppc64|powerpc")
    target_compile_definitions(rpcemu_core PUBLIC _RPCEMU_BIG_ENDIAN)
endif()

target_link_libraries(rpcemu_core PUBLIC pthread ${CMAKE_DL_LIBS})

# Windows: Winsock (control sockets, SLiRP NAT, TCP modem, broadcast relay) and
# the IP Helper API (adapter enumeration in broadcast_relay.c).
if(WIN32)
    target_link_libraries(rpcemu_core PUBLIC ws2_32 iphlpapi)
endif()

# SDL2 is needed by the podule sound-output pathway (common/sound_out_sdl2.c).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
endif()
if(SDL2_FOUND)
    target_include_directories(rpcemu_core PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(rpcemu_core PUBLIC ${SDL2_LIBRARIES})
endif()

# Podule MIDI and sound-input backends: real ALSA pathways where available,
# stubs otherwise.
if(PkgConfig_FOUND)
    pkg_check_modules(ALSA alsa)
endif()
if(ALSA_FOUND)
    target_sources(rpcemu_core PRIVATE
        podules/common/midi_alsa.c
        podules/common/sound_alsain.c)
    target_include_directories(rpcemu_core PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(rpcemu_core PUBLIC ${ALSA_LIBRARIES})
    target_compile_definitions(rpcemu_core PRIVATE RPCEMU_MIDI_ALSA)
    message(STATUS "Podule MIDI + sound-in backend: ALSA")
else()
    target_sources(rpcemu_core PRIVATE
        podules/common/midi_stub.c
        podules/common/sound_in_null.c)
    message(STATUS "Podule MIDI + sound-in backend: stub (no ALSA)")
endif()

# Expose the JIT test hooks (codegen_test_compile/_interp) for the differential
# flag tester. amd64 dynarec only; releases can disable with RPCEMU_BUILD_TESTS=OFF.
if(RPCEMU_BUILD_TESTS AND RPCEMU_DYNAREC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_definitions(rpcemu_core PRIVATE RPCEMU_JIT_TEST)
endif()

if(RPCEMU_ENABLE_GHOSTPDL)
    if(DEFINED ENV{GHOSTPDL_PREFIX})
        set(_ghost_prefix "$ENV{GHOSTPDL_PREFIX}")
    else()
        set(_ghost_prefix "")
    endif()

    if(_ghost_prefix AND EXISTS "${_ghost_prefix}/include/ghostscript/iapi.h")
        target_include_directories(rpcemu_core PUBLIC "${_ghost_prefix}/include/ghostscript")
        target_compile_definitions(rpcemu_core PUBLIC CONFIG_GHOSTPDL)
        if(EXISTS "${_ghost_prefix}/lib/libgpdl.so" OR EXISTS "${_ghost_prefix}/lib/libgpdl.a")
            target_compile_definitions(rpcemu_core PUBLIC CONFIG_GHOSTPDL_PCL)
            target_link_directories(rpcemu_core PUBLIC "${_ghost_prefix}/lib")
            target_link_libraries(rpcemu_core PUBLIC gpdl)
        else()
            target_link_directories(rpcemu_core PUBLIC "${_ghost_prefix}/lib")
            target_link_libraries(rpcemu_core PUBLIC gs)
        endif()
    elseif(NOT CMAKE_CROSSCOMPILING AND EXISTS "/usr/include/ghostscript/iapi.h")
        target_include_directories(rpcemu_core PUBLIC /usr/include/ghostscript)
        target_compile_definitions(rpcemu_core PUBLIC CONFIG_GHOSTPDL)
        target_link_libraries(rpcemu_core PUBLIC gs)
    endif()
endif()

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(gui)

# Host-side HostCmd client (rpcemu-run / rpcemu-shell).
add_subdirectory(tools)
