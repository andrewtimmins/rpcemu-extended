# Unit tests.
#
# Every test is registered through rpcemu_add_test() rather than add_test()
# directly, so the suite can be counted. A test that stops being registered -
# because a file it is guarded on was renamed, or a condition stopped matching -
# takes its coverage away without failing anything, and a build that quietly
# tests less than it did reads exactly like one that passed. The count is
# declared at the bottom of this file and checked at configure time, and it is
# written out for tests/run-ctest.sh to check again after the run.
#
# Adding a test therefore means raising RPCEMU_EXPECTED_TESTS. That is the point:
# the number has to be changed deliberately, so it cannot drift downwards on its
# own.
set(RPCEMU_REGISTERED_TESTS "")

# A macro, not a function: it appends to a list in the caller's scope.
macro(rpcemu_add_test name)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    list(APPEND RPCEMU_REGISTERED_TESTS ${name})
endmacro()

# The JIT differential testers use the recompiler's test hooks
# (compiled into rpcemu_core when RPCEMU_JIT_TEST is set, see src/CMakeLists.txt).
# They compare each recompiled instruction class against the interpreter and are
# architecture-generic: they validate whichever native backend is built
# (amd64/x86 or arm64).
#
# The condition has to stay identical to the one guarding RPCEMU_JIT_TEST in
# src/CMakeLists.txt: these tests call hooks that only exist when that is defined,
# so the two disagreeing is a link error rather than a skip. Note that i386/i686
# has a backend (codegen_x86.c) but is excluded from both - see docs/testing.md.
if(RPCEMU_DYNAREC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|aarch64|arm64|ARM64")
    set(RPCEMU_JIT_TESTS TRUE)
    foreach(jit_test
            test_jit_flags   # data-processing NZCV flags
            test_jit_mem     # LDR/STR/LDRB/STRB addressing modes
            test_jit_branch  # B / BL
            test_jit_mul     # MUL / MLA / UMULL
            test_jit_ldmstm  # LDM / STM
            test_jit_e2e)    # block linking + full-program integration
        add_executable(${jit_test} ${jit_test}.c test_stubs.c)
        target_link_libraries(${jit_test} PRIVATE rpcemu_core m)
        rpcemu_add_test(${jit_test})
    endforeach()

    # Randomised differential fuzzer (interpreter vs recompiler). Run a modest
    # count under ctest; run more by hand, e.g. `test_jit_fuzz <seed> <iters>`.
    add_executable(test_jit_fuzz test_jit_fuzz.c test_stubs.c)
    target_link_libraries(test_jit_fuzz PRIVATE rpcemu_core m)
    rpcemu_add_test(test_jit_fuzz 1 200000)

    # Sequence fuzzer: random programs through arm_exec (branches + block
    # linking), interpreter vs recompiler. Also runnable at scale by hand.
    add_executable(test_jit_seqfuzz test_jit_seqfuzz.c test_stubs.c)
    target_link_libraries(test_jit_seqfuzz PRIVATE rpcemu_core m)
    rpcemu_add_test(test_jit_seqfuzz 1 50000)
else()
    set(RPCEMU_JIT_TESTS FALSE)
    # An interpreter build has no recompiler to differentially test, which is
    # ordinary - the arm64 releases are built that way. Said at every configure
    # rather than only in the log, because eight of the tests are the ones being
    # given up.
    message(STATUS
        "RPCEmu: no dynarec backend here, so the 8 JIT differential tests are "
        "not built (RPCEMU_DYNAREC=${RPCEMU_DYNAREC}, "
        "processor ${CMAKE_SYSTEM_PROCESSOR})")
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
rpcemu_add_test(test_vdu_filter)

# The VNC machine selector: navigation as a state machine, plus the drawing.
# Compiles the units directly; they deliberately depend on nothing but the font,
# so a test needs neither wx nor libvncserver. Given a path it also writes a PPM,
# which is how the layout gets looked at rather than merely asserted.
add_executable(test_machine_selector test_machine_selector.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/machine_selector.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/text_screen.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/console_font.c)
target_include_directories(test_machine_selector PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_machine_selector)

# App-level settings: the ways in to the process (VNC, HostCmd), which are not
# properties of an emulated machine. The migration is the part worth testing - a
# key the file omits must leave the legacy per-machine value alone - so the cases
# lean on that. Runs in the build directory, since it writes a settings file.
add_executable(test_app_settings test_app_settings.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/app_settings.c test_stubs.c)
target_link_libraries(test_app_settings PRIVATE rpcemu_core m)
rpcemu_add_test(test_app_settings
         ${CMAKE_CURRENT_BINARY_DIR})

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
rpcemu_add_test(test_mmu_perms)

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
rpcemu_add_test(test_mode_fit)

# Host keys to RISC OS keys. Compiles the two units directly, which is the whole
# point of them being separate: they are integer lookup tables with no wxWidgets
# and no platform calls, so THE WINDOWS AND macOS MAPPINGS ARE BOTH TESTED FROM
# WHICHEVER PLATFORM HAPPENS TO BE RUNNING. The keyboard had no test at all until
# three faults arrived that all came from mapping characters instead of physical
# key positions - #88 (German layout), #91 (third mouse button on macOS) and part
# of #70 (left and right modifiers). See docs/keyboard.md.
add_executable(test_keymap test_keymap.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/keymap_platform.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/keyboard_x.c)
target_include_directories(test_keymap PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_keymap)

# Which keys the guest believes are held. The regression test for issue #70,
# where letting go of one Shift released the modifier while the other was still
# down, because held keys were tracked as a set of scancodes and several physical
# keys can map to one scancode. That is not a tidied-away accident either: macOS
# maps both Command and Control onto the guest's Ctrl on purpose, so the rule
# needs holding down by something. Dependency-free, like the unit itself.
add_executable(test_held_keys test_held_keys.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/held_keys.c)
target_include_directories(test_held_keys PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_held_keys)

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
rpcemu_add_test(test_ip_reass)

# Relay-side reassembly of guest-originated Access+ datagrams
add_executable(test_relay_reasm test_relay_reasm.c)
target_include_directories(test_relay_reasm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
)
target_compile_definitions(test_relay_reasm PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
target_link_libraries(test_relay_reasm PRIVATE ${NET_TEST_LIBS})
rpcemu_add_test(test_relay_reasm)

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
rpcemu_add_test(test_gfxcard)

# The shared clipboard's text conversion. hostclipboard.c is included by the test
# and the emulated memory stubbed, so this links nothing.
add_executable(test_clipboard test_clipboard.c)
target_include_directories(test_clipboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_clipboard)

# The graphics card's driver module, as RISC OS will read it. The image is
# assembled by hand and committed (build.sh --podules), so nothing in an ordinary
# build would notice it going stale or truncated; this reads it the way the module
# system does.
#
# The file is in the repository, so a tree without it is a broken checkout rather
# than a configuration to accommodate. This used to skip quietly if it was
# missing, which meant renaming the ROM would have taken the test with it and said
# nothing.
set(RPCEMU_GFXROM "${CMAKE_CURRENT_SOURCE_DIR}/../gfxroms/RPCEmuGfx,ffa")
if(NOT EXISTS "${RPCEMU_GFXROM}")
    message(FATAL_ERROR
        "gfxroms/RPCEmuGfx,ffa is missing, so the graphics card's ROM cannot be "
        "tested. It is committed to the repository - restore it with "
        "'git checkout -- gfxroms', or rebuild it with './build.sh --podules'. "
        "Configure with -DRPCEMU_BUILD_TESTS=OFF to build without any tests.")
endif()
add_executable(test_gfxrom test_gfxrom.c)
target_include_directories(test_gfxrom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_gfxrom "${RPCEMU_GFXROM}")

# The OHCI controller's isochronous transfer descriptors: the one place the
# emulator writes a length back that a driver reads, so a mistake there is a
# stream that appears to work and produces rubbish rather than a failure anybody
# notices. Compiles usb_ohci.c directly and stubs guest memory and the
# passthrough, so it links nothing - no libusb, and no SDL through rpcemu_core.
add_executable(test_ohci_iso test_ohci_iso.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/usb_ohci.c)
target_include_directories(test_ohci_iso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_ohci_iso)

# The lock that stops one machine being run twice, and the owner file that says
# which process has it. Compiles machine_lock.c directly and supplies rpclog(),
# its only dependency; the test uses the platform's own locking calls to stand in
# for a second emulator, so it needs no second process and behaves the same on all
# three platforms. Runs in the build directory, since it creates machine
# directories and locks them.
add_executable(test_machine_lock test_machine_lock.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/machine_lock.c
               test_log_stub.c)
target_include_directories(test_machine_lock PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_machine_lock ${CMAKE_CURRENT_BINARY_DIR})

# The per-instance network slot. net_slot.c is #included by the test rather than
# linked, so the test can bind the same loopback ports the unit does and check the
# claim is really exclusive. Needs the socket libraries on Windows, which
# rpcemu_core would otherwise have supplied.
add_executable(test_net_slot test_net_slot.c)
target_include_directories(test_net_slot PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
    target_link_libraries(test_net_slot PRIVATE ws2_32)
endif()
rpcemu_add_test(test_net_slot)

# The serialisation primitives snapshots are built from, and the run-length codec
# that carries RAM and VRAM. Links rpcemu_core: savestate.c calls into every
# module's own save/load function, so unlike most of the tests here it cannot be
# compiled in isolation. Runs in the build directory, since it writes files.
add_executable(test_savestate test_savestate.c test_stubs.c)
target_link_libraries(test_savestate PRIVATE rpcemu_core m)
rpcemu_add_test(test_savestate ${CMAKE_CURRENT_BINARY_DIR})

# MD5, which decides whether a downloaded package is the file its index promised.
# Known-answer tests against RFC 1321, because a subtly wrong MD5 produces
# sixteen plausible bytes and nothing else would notice. Compiles md5.c directly;
# it depends on nothing. Runs in the build directory, since it hashes a file it
# writes.
add_executable(test_md5 test_md5.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/md5.c)
target_include_directories(test_md5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_md5 ${CMAKE_CURRENT_BINARY_DIR})

# The zip reader used to unpack downloaded RISC OS archives. Compiles unzip.c
# directly rather than linking rpcemu_core, for the same reason as the tests
# above: the core drags in SDL. unzip.c needs only the logger, which the test
# supplies. Runs everywhere, including Windows, since that is where the
# filename and directory handling is most likely to differ.
add_executable(test_unzip test_unzip.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/unzip.c)
target_include_directories(test_unzip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_unzip)

# ---------------------------------------------------------------------------
# How big the suite is meant to be.
#
# Everything above registers through rpcemu_add_test(), so the tests can be
# counted and the count compared against what this file says it should be. The
# reason for doing it at all: every other outcome here is loud - a test that fails
# fails the build - but a test that is never registered is silent, and a run that
# tested twelve things looks exactly like the run that tested twenty. This turns
# that one silent failure into a configure error.
#
# Raise RPCEMU_EXPECTED_TESTS when adding a test. If that feels like a chore, it
# is the only line in the file that cannot go stale without being noticed.
set(RPCEMU_EXPECTED_TESTS 18)	# tests that build on every platform
if(RPCEMU_JIT_TESTS)
    math(EXPR RPCEMU_EXPECTED_TESTS "${RPCEMU_EXPECTED_TESTS} + 8")	# JIT differential
endif()

list(LENGTH RPCEMU_REGISTERED_TESTS RPCEMU_ACTUAL_TESTS)
if(NOT RPCEMU_ACTUAL_TESTS EQUAL RPCEMU_EXPECTED_TESTS)
    string(REPLACE ";" "\n  " RPCEMU_TEST_LIST "${RPCEMU_REGISTERED_TESTS}")
    message(FATAL_ERROR
        "The test suite is the wrong size: ${RPCEMU_ACTUAL_TESTS} tests were "
        "registered, ${RPCEMU_EXPECTED_TESTS} expected.\n"
        "If a test was added, raise RPCEMU_EXPECTED_TESTS in "
        "tests/CMakeLists.txt. If one has gone missing, find out why before "
        "lowering it - the number exists to make that impossible to do by "
        "accident.\nRegistered:\n  ${RPCEMU_TEST_LIST}")
endif()

# Left for tests/run-ctest.sh, which checks the same number again after the run.
# The check above cannot fire when the tests are never configured at all, which is
# the case the shell side catches.
file(WRITE "${CMAKE_BINARY_DIR}/rpcemu-expected-tests.txt"
     "${RPCEMU_EXPECTED_TESTS}\n")

message(STATUS "RPCEmu: ${RPCEMU_ACTUAL_TESTS} tests registered")
