# Unit tests. The JIT differential testers use the recompiler's test hooks
# (compiled into rpcemu_core when RPCEMU_JIT_TEST is set, see src/CMakeLists.txt).
# They compare each recompiled instruction class against the interpreter and are
# architecture-generic: they validate whichever native backend is built
# (amd64/x86 or arm64).
if(RPCEMU_DYNAREC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|aarch64|arm64|ARM64")
    foreach(jit_test
            test_jit_flags   # data-processing NZCV flags
            test_jit_mem     # LDR/STR/LDRB/STRB addressing modes
            test_jit_branch  # B / BL
            test_jit_mul     # MUL / MLA / UMULL
            test_jit_ldmstm  # LDM / STM
            test_jit_e2e)    # block linking + full-program integration
        add_executable(${jit_test} ${jit_test}.c test_stubs.c)
        target_link_libraries(${jit_test} PRIVATE rpcemu_core m)
        add_test(NAME ${jit_test} COMMAND ${jit_test})
    endforeach()

    # Randomised differential fuzzer (interpreter vs recompiler). Run a modest
    # count under ctest; run more by hand, e.g. `test_jit_fuzz <seed> <iters>`.
    add_executable(test_jit_fuzz test_jit_fuzz.c test_stubs.c)
    target_link_libraries(test_jit_fuzz PRIVATE rpcemu_core m)
    add_test(NAME test_jit_fuzz COMMAND test_jit_fuzz 1 200000)

    # Sequence fuzzer: random programs through arm_exec (branches + block
    # linking), interpreter vs recompiler. Also runnable at scale by hand.
    add_executable(test_jit_seqfuzz test_jit_seqfuzz.c test_stubs.c)
    target_link_libraries(test_jit_seqfuzz PRIVATE rpcemu_core m)
    add_test(NAME test_jit_seqfuzz COMMAND test_jit_seqfuzz 1 50000)
else()
    message(STATUS "Skipping JIT differential tests (requires a native dynarec backend)")
endif()

# boot_trace: boots a real machine with no GUI and reports how far it got. Not a
# ctest - it needs a ROM in RPCEMU_DATADIR, which a unit test cannot assume - but
# it builds everywhere the core does, including cross-compiled for aarch64 so the
# arm64 backend can be examined under qemu on a machine that has no arm64 in it.
# See the comment at the top of boot_trace.c, and issue #30.
add_executable(boot_trace EXCLUDE_FROM_ALL boot_trace.c test_stubs.c)
target_link_libraries(boot_trace PRIVATE rpcemu_core m)

# The VDU-stream filter rpcemu-run uses to turn guest output into host text.
# Compiles the unit directly rather than linking anything: it has no dependencies,
# which is why it is a separate file. Covers the cases that make a naive filter
# wrong - VDU parameters that look like line endings, and sequences split across
# a read boundary. See docs/hostcmd.md.
add_executable(test_vdu_filter test_vdu_filter.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/vdu_filter.c)
target_include_directories(test_vdu_filter PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/tools)
add_test(NAME test_vdu_filter COMMAND test_vdu_filter)

# MMU access permissions. Builds real page tables in emulated RAM and checks the
# ARM DDI 0100B Table 7-7 rules are enforced, including - the point of it - after
# a translation has been cached. Needs no dynarec: the caches it exercises live in
# the interpreter paths, and the recompiler reads the same maps.
#
# This is the regression test for the defect ADFFS reported from 2013 to 2026: a
# cached translation was reused without rechecking permissions, so an illegal
# access following a legal one to the same page was allowed. It covers cold
# permissions, cached translations, LDRT/STRT and instruction fetch. See
# docs/mmu-permissions.md.
add_executable(test_mmu_perms test_mmu_perms.c test_stubs.c)
target_link_libraries(test_mmu_perms PRIVATE rpcemu_core m)
add_test(NAME test_mmu_perms COMMAND test_mmu_perms)

# Display mode selection against a VRAM budget. Architecture-neutral and needs
# no dynarec, so unlike the JIT tests it builds everywhere.
# Compiles the unit under test directly rather than linking rpcemu_core: the
# core drags in SDL, and arm64 Homebrew has no separate SDL2main, so linking it
# for a pure arithmetic test broke the macOS arm64 build. display_mode.c has no
# dependencies of its own, which is the point of it being separate.
# edid.c is included so the test can decode what the EDID actually advertises
# and check the chooser cannot offer a mode the guest would refuse. Both files
# are dependency-free, so this still links nothing.
add_executable(test_mode_fit test_mode_fit.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/display_mode.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/edid.c)
target_include_directories(test_mode_fit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test_mode_fit COMMAND test_mode_fit)

# Networking tests for the Access+/ShareFS path, where large copies drive IP
# fragment reassembly. These compile the code under test directly rather than
# linking rpcemu_core, because each replaces a symbol the core also defines: the
# SLiRP output callbacks in one case, sendto() in the other. That also means the
# Windows socket libraries have to be named here rather than inherited.
#
# These run on Windows too, deliberately: broadcast_relay.c has a whole separate
# _WIN32 branch, and the Access+/ShareFS reports that prompted this code came
# from Windows users, so it is the last platform that should go untested.
# pthread is needed for MinGW's clock_gettime (used by slirp/misc.c), which
# rpcemu_core otherwise supplies for the rest of the build.
if(WIN32)
    set(NET_TEST_LIBS ws2_32 iphlpapi pthread)
else()
    set(NET_TEST_LIBS "")
endif()

set(SLIRP_TEST_SOURCES "")
foreach(slirp_src
        bootp cksum cutils if ip_icmp ip_input ip_output mbuf misc sbuf
        slirp socket tcp_input tcp_output tcp_subr tcp_timer udp)
    list(APPEND SLIRP_TEST_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp/${slirp_src}.c)
endforeach()

# SLiRP fragment reassembly, covering the two backported upstream fixes
add_executable(test_ip_reass test_ip_reass.c ${SLIRP_TEST_SOURCES})
target_include_directories(test_ip_reass PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
)
target_compile_definitions(test_ip_reass PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
target_link_libraries(test_ip_reass PRIVATE ${NET_TEST_LIBS})
add_test(NAME test_ip_reass COMMAND test_ip_reass)

# Relay-side reassembly of guest-originated Access+ datagrams
add_executable(test_relay_reasm test_relay_reasm.c)
target_include_directories(test_relay_reasm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
)
target_compile_definitions(test_relay_reasm PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
target_link_libraries(test_relay_reasm PRIVATE ${NET_TEST_LIBS})
add_test(NAME test_relay_reasm COMMAND test_relay_reasm)

# The emulated graphics card: its register file, and the checks that stop a
# guest-supplied mode from making the video code read outside the card's own
# framestore. Compiles gfxcard.c directly and stubs the backplane, because
# linking podules.c would drag in every bundled expansion card and the plugin
# loader with them - and rpcemu_core would drag in SDL, which is what broke the
# macOS arm64 build once already (see test_mode_fit above).
# edid.c comes along because the card serves the monitor's EDID over DDC; like
# display_mode.c it is dependency-free, so this still links nothing.
add_executable(test_gfxcard test_gfxcard.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/edid.c)
target_include_directories(test_gfxcard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test_gfxcard COMMAND test_gfxcard)

# The shared clipboard's text conversion. hostclipboard.c is included by the test
# and the emulated memory stubbed, so this links nothing.
add_executable(test_clipboard test_clipboard.c)
target_include_directories(test_clipboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test_clipboard COMMAND test_clipboard)

# The graphics card's driver module, as RISC OS will read it. The image is
# assembled by hand and committed (build.sh --podules), so nothing in an ordinary
# build would notice it going stale or truncated; this reads it the way the module
# system does. Guarded on the file existing so a tree without it still configures.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../gfxroms/RPCEmuGfx,ffa")
    add_executable(test_gfxrom test_gfxrom.c)
    target_include_directories(test_gfxrom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(NAME test_gfxrom COMMAND test_gfxrom
             "${CMAKE_CURRENT_SOURCE_DIR}/../gfxroms/RPCEmuGfx,ffa")
else()
    message(STATUS "Skipping test_gfxrom (gfxroms/RPCEmuGfx,ffa not built)")
endif()

# The OHCI controller's isochronous transfer descriptors: the one place the
# emulator writes a length back that a driver reads, so a mistake there is a
# stream that appears to work and produces rubbish rather than a failure anybody
# notices. Compiles usb_ohci.c directly and stubs guest memory and the
# passthrough, so it links nothing - no libusb, and no SDL through rpcemu_core.
add_executable(test_ohci_iso test_ohci_iso.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/usb_ohci.c)
target_include_directories(test_ohci_iso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test_ohci_iso COMMAND test_ohci_iso)

# The zip reader used to unpack downloaded RISC OS archives. Compiles unzip.c
# directly rather than linking rpcemu_core, for the same reason as the tests
# above: the core drags in SDL. unzip.c needs only the logger, which the test
# supplies. Runs everywhere, including Windows, since that is where the
# filename and directory handling is most likely to differ.
add_executable(test_unzip test_unzip.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/unzip.c)
target_include_directories(test_unzip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test_unzip COMMAND test_unzip)
