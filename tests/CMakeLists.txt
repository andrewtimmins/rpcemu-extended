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

# The same, for a test that is a script rather than a compiled target. Kept
# separate only because add_test() needs an interpreter in front of the file;
# it appends to the same list, so a script test is counted like any other and
# cannot go missing quietly.
macro(rpcemu_add_python_test name script)
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/${script} ${ARGN})
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

    # 26-bit program configuration: R15 carries the PSR, and no other jit test
    # runs that configuration or compares R15 at all. Matrix plus a sweep; more
    # by hand, e.g. `test_jit_26bit <seed> <sweep>`.
    add_executable(test_jit_26bit test_jit_26bit.c test_stubs.c)
    target_link_libraries(test_jit_26bit PRIVATE rpcemu_core m)
    rpcemu_add_test(test_jit_26bit 1 50000)

    # 26-bit sequences: whole programs through arm_exec() under both engines, so
    # block linking and mid-run mode changes are covered. Code runs from VRAM
    # because a 26-bit PC cannot reach the RAM at 0x10000000.
    add_executable(test_jit_26bit_seq test_jit_26bit_seq.c test_stubs.c)
    target_link_libraries(test_jit_26bit_seq PRIVATE rpcemu_core m)
    rpcemu_add_test(test_jit_26bit_seq 1 20000)
else()
    set(RPCEMU_JIT_TESTS FALSE)
    # An interpreter build has no recompiler to differentially test, which is
    # ordinary - the arm64 releases are built that way. Said at every configure
    # rather than only in the log, because eight of the tests are the ones being
    # given up.
    message(STATUS
        "RPCEmu: no dynarec backend here, so the 10 JIT differential tests are "
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

# Where a host-side sprite plot lands: the coordinate flip between RISC OS, which
# counts y up from the bottom, and a framebuffer, which counts rows down from the
# top, plus inclusive graphics-window edges and a movable origin. Links the core
# because the geometry lives with the accelerators; needs no machine.
add_executable(test_accel_plot test_accel_plot.c test_stubs.c)
target_link_libraries(test_accel_plot PRIVATE rpcemu_core m)
rpcemu_add_test(test_accel_plot)

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

# What the CPU hands an exception handler: the mode entered, the return address
# in R14, the saved status register, and whether interrupts end up disabled.
#
# Get any of those wrong and the handler runs but returns to the wrong place,
# which does not present as a CPU bug - it presents as the guest going mad some
# time later, usually a wild branch a long way from the cause. None of it was
# covered until an ADFFS game did exactly that.
#
# The case that earns this test is 26-bit code interrupted on a 32-bit-configured
# CPU: what every Archimedes game under ADFFS does thousands of times a second,
# and the least exercised path in the emulator, since RISC OS 5 is 32-bit
# throughout and RISC OS 3.1x is 26-bit throughout. Expected values come from
# DDI 0100, not from the implementation.
#
# Links rpcemu_core for the real exception(), updatemode() and register banks.
# Note the interpreter and the recompiler have separate exception() bodies, so
# whichever this build links is the one covered.
add_executable(test_exception test_exception.c test_stubs.c)
target_link_libraries(test_exception PRIVATE rpcemu_core m)
rpcemu_add_test(test_exception)

# The disassembler, and the instruction description the debugger acts on. Needs
# nothing but arm_disasm.c - it is pure decoding, no machine state.
#
# A wrong mnemonic used to cost a double-take in the inspector window and nothing
# more. It costs more now that arm_decode() answers "is this a call", "is this a
# return" and "where does this branch go", because step-over and step-out are
# built on those answers: a decoding mistake stops looking like an odd line and
# starts looking like a debugger that runs away when asked to step. This also
# pins down the FPA10 encodings, hand-assembled from the operation tables in
# fpa.c, which is the only written record of what those bit patterns mean.
add_executable(test_arm_disasm test_arm_disasm.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/arm_disasm.c)
target_include_directories(test_arm_disasm PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_arm_disasm)

# The debugger's per-instruction fast gate. Links rpcemu_core because the
# debugger state is file-static in rpcemu.c and the public API is the only way
# to reach it - which is the thing worth testing.
#
# The interpreter used to call the debugger hook on every instruction whether or
# not anything was attached. It is now gated on a cached flag, and caching it
# introduces exactly one silent failure: a state change that forgets to
# recompute it. Breakpoints then never fire, with no error and no log line, and
# the person debugging blames their breakpoint address. This checks the flag is
# raised and lowered again on every route in and out.
add_executable(test_debugger_gate test_debugger_gate.c test_stubs.c)
target_link_libraries(test_debugger_gate PRIVATE rpcemu_core m)
rpcemu_add_test(test_debugger_gate)

# Breakpoint condition expressions. Compiles debugexpr.c directly - it reads the
# machine only through a caller-supplied environment, so a test substitutes its
# own registers and memory and needs no emulator.
#
# A conditional breakpoint is trusted differently from a plain one. "Stop at
# 0x8000" visibly works or visibly does not; "stop at 0x8000 when R0 is 5" that
# evaluates wrongly stops at the wrong moment, or never, and the person
# debugging draws a conclusion about their own program from an answer the tool
# got wrong. The cases lean on how small parsers are usually wrong: operator
# precedence, operators that are prefixes of other operators (& vs &&, < vs <=),
# and - the one that matters most - malformed input being accepted rather than
# refused.
add_executable(test_debugexpr test_debugexpr.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/debugexpr.c)
target_include_directories(test_debugexpr PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_debugexpr)

# Walking the APCS frame-pointer chain. Links rpcemu_core and builds frames in
# real emulated RAM with the MMU off, so addresses are physical.
#
# A backtrace is the one debugger output people read without checking, which
# makes a wrong one uniquely expensive: it does not look wrong, it looks like an
# explanation. The chain is a compiler convention and much of RISC OS is
# assembler that does not keep it, so the walk is regularly handed a register
# that is not a frame pointer - it must not loop, must not invent frames, and
# must say when it gave up. The 26-bit case is here because forgetting to strip
# the PSR bits from a saved link register does not fail, it silently reports
# addresses that are all wrong.
add_executable(test_backtrace test_backtrace.c test_stubs.c)
target_link_libraries(test_backtrace PRIVATE rpcemu_core m)
rpcemu_add_test(test_backtrace)

# Step over, step out and run to. Links rpcemu_core and drives the debugger's
# state machine through its public API with the MMU off, so a fake instruction
# can be put where the CPU will look for it.
#
# These are the only debugger operations whose failure mode is to RUN THE
# MACHINE. A breakpoint that is wrong stops in the wrong place; a step-over that
# is wrong does not stop at all, and whatever state was being examined is gone -
# recovering it means rebooting and reproducing. So the rules pinned down here
# are the ones that keep that from happening: an unrecognised instruction
# degrades to a single step rather than a guess, the temporary target never
# outlives the operation that armed it, and it is kept out of the breakpoint
# table so stepping over an address the user has breakpointed cannot disturb
# their breakpoint.
add_executable(test_stepping test_stepping.c test_stubs.c)
target_link_libraries(test_stepping PRIVATE rpcemu_core m)
rpcemu_add_test(test_stepping)

# The guest symbol table. Compiles debugsym.c directly - it reads a text file
# and searches an array, and needs no machine. Runs in the build directory,
# since it writes the files it then loads.
#
# The value of a symbol table is entirely that the name beside an address can be
# trusted: a backtrace reading "main+0x24" gets acted on, and if the attribution
# is wrong the person reads the wrong function with nothing to warn them. Almost
# all of that risk is in deciding what NOT to name, so the cases lean on the
# boundaries - below the first symbol, between two, and past the end of the last
# - where the naive "nearest symbol at or below" would label an address in the
# RISC OS kernel as main+0x3f21c. The rest covers refusing a file that is not a
# symbol file outright, rather than loading whichever lines happened to parse.
add_executable(test_debugsym test_debugsym.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/debugsym.c)
target_include_directories(test_debugsym PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_debugsym ${CMAKE_CURRENT_BINARY_DIR})

# The debugger's wire protocol, over a real socket. Links rpcemu_core and calls
# debugcmd_poll() from the test in place of the emulator thread, which is the
# same arrangement the real code uses while the CPU is paused.
#
# Runs on every platform. The transport differs - AF_UNIX on Unix, a loopback
# TCP port on Windows - because that is what debugcmd.c itself does; the parser
# under test never sees the difference, and it is the parser that a client on
# any platform depends on.
#
# Every other layer of the debugger is reached through C functions with types on
# them. This one is a hand-written parser over strtok, and it is what every
# external client actually touches - the MCP server, rpcemu-debug, and anything
# a person writes. A mistake here is invisible from inside: the command is
# understood as something other than what was sent. "bp add 8000 if r0 == 0"
# has to survive being split on spaces, having its options recognised in any
# order, and keeping the condition - which contains spaces - whole to the end of
# the line; a parser that gets that wrong sets an unconditional breakpoint and
# reports success.
add_executable(test_debugcmd test_debugcmd.c test_stubs.c)
target_link_libraries(test_debugcmd PRIVATE rpcemu_core m)
if(WIN32)
    target_link_libraries(test_debugcmd PRIVATE ws2_32)
endif()
rpcemu_add_test(test_debugcmd ${CMAKE_CURRENT_BINARY_DIR})

# HostCmd: a command nobody is there to collect has to be answered rather than
# left waiting, and - the part that is easy to get wrong in the other direction
# - a guest module that is merely busy must not be given up on. Drives the real
# socket and calls the SWI handler in place of the guest. See src/hostcmd.c.
add_executable(test_hostcmd test_hostcmd.c test_stubs.c)
target_link_libraries(test_hostcmd PRIVATE rpcemu_core m)
if(WIN32)
    target_link_libraries(test_hostcmd PRIVATE ws2_32)
endif()
rpcemu_add_test(test_hostcmd ${CMAKE_CURRENT_BINARY_DIR})

# The CI abort watcher's classification and verdict (tests/abort_watch.py, used
# by tests/boot_smoke.py). Pure Python, no emulator.
#
# That module decides whether the boot job goes red, and everything it needs to
# do its real work - a ROM, a real boot, a machine taking real aborts - is
# exactly what the unit suite does not have. Without this, the only thing that
# ever exercises the logic is the thing it is meant to be checking, and a
# mistake would show up as a boot test that passes forever. The cases pull in
# both directions: a healthy RISC OS boot takes aborts on purpose while probing
# for hardware and must not fail, while an abort once the machine is idle must.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(RPCEMU_PYTHON_TESTS TRUE)
    rpcemu_add_python_test(test_abort_watch test_abort_watch.py)
else()
    set(RPCEMU_PYTHON_TESTS FALSE)
    message(STATUS
        "RPCEmu: no Python 3 interpreter here, so the abort-watcher test is "
        "not registered (tests/boot_smoke.py needs one too)")
endif()

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

# Where the data directory comes from, and above all when the user is asked for
# it. Issue #67 was that RPCEmu made a folder in the user's home directory
# without asking; asking on first run fixes that and introduces a worse bug if
# the conditions are wrong, because the ways to get it wrong are all real people
# being interrupted - an existing user upgrading, a developer with configs/ beside
# the binary, a headless container blocked on a dialogue nobody can see. The
# precedence chain is a pure function so those cases can be enumerated rather
# than hoped about, all 512 combinations included. See docs/paths.md.
add_executable(test_data_dir test_data_dir.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/data_dir_choice.c)
target_include_directories(test_data_dir PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_data_dir)

# The file that records the chosen data directory. Separate from test_data_dir
# because that one is the pure decision and this one is the storage: a plain file
# rather than wxConfig, because it has to be readable from the entry points that
# run before wxWidgets is started. The path rules are pure over their environment
# inputs, so all three platforms' conventions are checked from whichever is
# running; the read and write cases use a real file, hence the directory argument.
add_executable(test_data_dir_store test_data_dir_store.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/data_dir_store.c)
target_include_directories(test_data_dir_store PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_data_dir_store ${CMAKE_CURRENT_BINARY_DIR}/datadir-store-test)

# Where a machine's HostFS drive lives. Discussion #77 made this configurable so
# several machines can share one folder; the convention is the part that needs
# holding down, because it decides whether a configuration survives its data
# folder being moved - empty means the default and stores nothing, relative moves
# with the machine, absolute is the only form that can go stale. debug_socket
# stored a resolved absolute path and broke exactly that way. Dependency-free, so
# the Windows and UNC forms are checked from any platform. See docs/hostfs.md.
add_executable(test_hostfs_path test_hostfs_path.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/hostfs_path.c)
target_include_directories(test_hostfs_path PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_hostfs_path)

# Typing a host file from its extension: the numbers, and - more important -
# everything the table must not claim. Source extensions are Text on RISC OS
# and are deliberately absent. See src/hostfs_filetype.c.
add_executable(test_hostfs_filetype test_hostfs_filetype.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/hostfs_filetype.c)
target_include_directories(test_hostfs_filetype PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_hostfs_filetype)

# Ejecting a CD-ROM image has to forget the handle as well as close it, or the
# next eject closes a FILE that has already been freed - two menu clicks apart.
# See src/cdrom-iso.c.
add_executable(test_cdrom_iso test_cdrom_iso.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/cdrom-iso.c)
target_include_directories(test_cdrom_iso PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_cdrom_iso)

# Whether moving somebody's files can be offered at all. Copying a hard disc is the
# most destructive thing RPCEmu does to the host filesystem, and every condition
# that makes it unsafe - a destination with files in it, a full disc, a machine
# still running from the folder - is awkward to arrange for real. So the rules are
# pure over facts the caller gathers, and this puts the combinations to them. The
# doing is src/gui/folder_transfer.cpp. See docs/hostfs.md.
add_executable(test_folder_move test_folder_move.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/folder_move.c)
target_include_directories(test_folder_move PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_folder_move)

# C++ for the tests that need it. Enabled here as well as in src/CMakeLists.txt
# because enable_language() sets its variables in the calling directory's scope, and
# tests/ is a sibling of src/ rather than a child - without this, configuring fails
# with "CMAKE_CXX_LINK_EXECUTABLE not set" while confusingly still compiling.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The doing, on real files: copy, verify, delete, and above all what happens when a
# transfer fails half way through. Needs wx (wxBase, for file and directory work)
# but NOT a display: FolderTransferRun() creates its progress dialogue only when
# there is a GUI, which is what lets this run in CI. rpcemu_core comes along for
# md5_file_hex(), which is how a copy is verified rather than merely sized.
add_executable(test_folder_transfer test_folder_transfer.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/folder_transfer.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/folder_move.c
               test_stubs.c)
target_include_directories(test_folder_transfer PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
target_link_libraries(test_folder_transfer PRIVATE rpcemu_core m)
rpcemu_setup_wxwidgets(test_folder_transfer)
rpcemu_add_test(test_folder_transfer)

# Where a machine's package database goes and what happens to one already there.
# Needs wx (wxBase) for its file and directory work but nothing else: rpclog()
# and rpcemu_get_resourcedir() are stubbed in the test, so no emulator, no
# configuration and no ROMs. The source tree is passed in because the code copies
# the bundled !Packages template out of resources/ro5/packages.
add_executable(test_package_layout test_package_layout.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/package_layout.cpp)
target_include_directories(test_package_layout PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_setup_wxwidgets(test_package_layout)
rpcemu_add_test(test_package_layout ${CMAKE_CURRENT_SOURCE_DIR}/..)

# machine_ipc: the shared-memory framebuffer and local control-channel
# transport the Manager window and a --managed machine talk over (see
# src/gui/machine_ipc.h). Pure std::thread + BSD/Winsock sockets, so this
# exercises the real transport without needing wxWidgets, a display, ROMs or
# a running emulator.
find_package(Threads REQUIRED)
add_executable(test_machine_ipc test_machine_ipc.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/machine_ipc.cpp)
target_include_directories(test_machine_ipc PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
target_link_libraries(test_machine_ipc PRIVATE Threads::Threads)
if(WIN32)
    # machine_ipc.cpp uses Winsock (WSAPoll, socket, bind, connect, ...) via
    # socket-compat.h. rpcemu_core PUBLIC-links ws2_32 for the same reason
    # (see src/CMakeLists.txt), but this target compiles machine_ipc.cpp
    # directly rather than linking that library, so it needs its own copy -
    # missed the first time round, which linux-amd64/macos passed silently
    # since neither needs it, and only windows-amd64 CI caught it.
    target_link_libraries(test_machine_ipc PRIVATE ws2_32)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_machine_ipc PRIVATE rt)
endif()
rpcemu_add_test(test_machine_ipc)

# What the machine editor warns about when a HostFS folder is chosen. Split out of
# the dialog because the decision was wrong in a way that read correctly: the
# "no !Boot" warning lived in the branch for a folder that already existed, so a
# folder about to be created - necessarily empty - was the one case that never got
# it. Reported by David Ramsden. See src/hostfs_advice.h.
add_executable(test_hostfs_advice test_hostfs_advice.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/hostfs_advice.c)
target_include_directories(test_hostfs_advice PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_hostfs_advice)

# The Risc PC second processor bus (OPEN Bus), and the stub card that proves it.
# Built before any real card on purpose: a second CPU is the least testable thing
# in the machine, so the bus takes its host services through a struct of function
# pointers and this substitutes a fake memory and a fake interrupt controller.
# Everything about an OPEN Bus card except the processor is therefore checkable on
# any platform with no emulator and no machine - the register window, bus
# mastering, nPIRQ, reset and the timeslice. When an x86 or another CPU arrives,
# the plumbing under it is already known good. See docs/openbus.md.
add_executable(test_openbus test_openbus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/openbus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/openbus_stub.c)
target_include_directories(test_openbus PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_openbus)

# Does the machine's own memory decode actually reach a fitted card? Links
# rpcemu_core for the real physical accessors, as test_mmu_perms does. Separate
# from test_openbus because that checks the bus rules against a fake host and this
# checks the wiring into mem.c - and a mistake there would be silent, since an
# unhooked window reads exactly like an empty slot. It exists because the obvious
# way to poke the card, the debugger socket, cannot: the debugger's accessor is
# deliberately side-effect-free and handles only ROM, VRAM and RAM.
add_executable(test_openbus_decode test_openbus_decode.c test_stubs.c)
target_link_libraries(test_openbus_decode PRIVATE rpcemu_core m)
rpcemu_add_test(test_openbus_decode)

# The three co-processor cores that can go in the OPEN Bus card. Each executes
# out of a plain byte array and links nothing at all - not the bus, not the core
# emulator, not wxWidgets - so all three run on every platform with no machine
# and no display. That is the whole reason the cores were written as separate
# files rather than inside the card.
#
# What each one pins down is in its own header comment. Worth knowing here: the
# RV32I test checks its own assembler against known instruction words before it
# executes anything, because an encoder and a decoder that are wrong the same way
# agree with each other.
add_executable(test_cpu_rv32i test_cpu_rv32i.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_rv32i.c)
target_include_directories(test_cpu_rv32i PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_rv32i)

add_executable(test_cpu_6502 test_cpu_6502.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6502.c)
target_include_directories(test_cpu_6502 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_6502)

add_executable(test_cpu_z80 test_cpu_z80.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_z80.c)
target_include_directories(test_cpu_z80 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_z80)

# The 6809's own test is the largest of the four, and deliberately: its indexed
# addressing is fourteen modes in a postbyte, the core decodes three quarters of
# its opcode map as a grid rather than as cases, and carry means borrow rather
# than the 6502's carry. Each of those has a failure mode that a test only
# looking at results would pass.
add_executable(test_cpu_6809 test_cpu_6809.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6809.c)
target_include_directories(test_cpu_6809 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_6809)

# The 6800, and with it the 6802 and the 6808, which are the same instruction
# set. Almost none of what this test checks is the instruction set: it is the
# quirks, because a 6800 that gets those wrong is not the part anybody's software
# was written against. CPX not touching the carry, INX affecting only Z, TST
# clearing the carry, V after a shift being N exclusive-or C, and a stack pointer
# that addresses the next free byte. It also checks the opcodes this part and the
# 6809 disagree about, which is what would catch somebody later deciding the two
# could share a decoder.
add_executable(test_cpu_6800 test_cpu_6800.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6800.c)
target_include_directories(test_cpu_6800 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_6800)

# The 68000. What this checks that a published vector suite structurally cannot:
# the generated dispatch table's own integrity - that no encoding was claimed by
# two instructions, and that the encodings which are not instructions have no
# handler - plus privilege, the exception model, and the resumable MOVEM.
add_executable(test_cpu_68000 test_cpu_68000.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_ea.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_exc.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_ops.c)
target_include_directories(test_cpu_68000 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_68000)

# The co-processor card itself: the register window, the aperture a guest loads a
# program through, the timeslice, the interrupt on halt and bus mastering. Links
# the bus and all three cores against the same fake host memory and fake
# interrupt controller test_openbus.c uses, so no emulator is involved.
#
# Separate from the core tests above because it covers everything a card does
# AROUND a processor, and it runs a program on ALL THREE cores through the same
# sequence: a register that works only for the core its author happened to be
# testing is a failure this project has had more than once.
add_executable(test_openbus_coproc test_openbus_coproc.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/openbus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/openbus_coproc.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/copro_bus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/openbus_stub.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_rv32i.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6502.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6800.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_ea.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_exc.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_68000_ops.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6809.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_z80.c)
target_include_directories(test_openbus_coproc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro
)
rpcemu_add_test(test_openbus_coproc)

# The card's address decode: what a core's loads and stores mean once the guest
# has said which parts of the address space are ROM, a screen, a latch the guest
# presents, a register that has to stop the core, or nothing at all. Links
# nothing - no card, no bus, no emulator - because the decode is a function of a
# region table and two byte arrays. That is why it is its own unit.
add_executable(test_copro_bus test_copro_bus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/copro_bus.c)
target_include_directories(test_copro_bus PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_copro_bus)

# A core running against that decode: the two halves have to agree, and the part
# most likely to be wrong is a stalling access, because a core cannot suspend
# mid-instruction so the instruction is abandoned whole and retried. This checks
# that the retry leaves no trace - registers, program counter, instruction count
# and the store that follows - on the 6502 and on the Z80, which reaches the same
# decode through its separate port space.
add_executable(test_copro_core_bus test_copro_core_bus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/copro_bus.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6502.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6809.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_z80.c)
target_include_directories(test_copro_core_bus PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_copro_core_bus)

# Instruction timings against published values. The cores' tables were derived
# mechanically - the 6502's from each opcode's addressing mode, the Z80's from the
# memory accesses the core makes - and this is the independent half: figures from
# the documentation, checked against what the core charges. Both polarities of
# every conditional, because the Z80 table was once derived from a probe whose
# flags came from a reset, where they are all ones, and a sample testing one
# polarity of each agreed with a table that was wrong for the other.
add_executable(test_cpu_cycles test_cpu_cycles.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6502.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6800.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_6809.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro/cpu_z80.c)
target_include_directories(test_cpu_cycles PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/copro)
rpcemu_add_test(test_cpu_cycles)

# The guest module images this fork assembles itself: header offsets inside the
# image, the 32-bit flag present, no 26-bit return idiom anywhere in the code,
# and every command table entry's minimum parameter count no greater than its
# maximum. Reads the committed images and needs no emulator, no ARM assembler
# and no guest, so it runs on all three platforms.
#
# It exists because every one of those mistakes is silent until a guest boots,
# and one of them is not silent in proportion to its size: a module whose
# initialisation returns to a corrupted address makes RISC OS abandon the entire
# expansion-card scan, so a single bad word in one module took four other
# modules and the graphics card's driver off the machine with it.
add_executable(test_module_headers test_module_headers.c)
rpcemu_add_test(test_module_headers ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Physical-address decode for the DMA fetch paths (sound, VIDC cursor). Links
# rpcemu_core for the real physical accessors, as test_mmu_perms does, and writes
# markers through them so a decode that lands in the wrong region is caught
# rather than reading a plausible zero. Exists because both callers used to
# decode the address themselves and both got SIMM 1 and the Kinetic SDRAM wrong,
# silently: the sound is noise and the pointer is rubbish, with nothing logged.
add_executable(test_dma_region test_dma_region.c test_stubs.c)
target_link_libraries(test_dma_region PRIVATE rpcemu_core m)
rpcemu_add_test(test_dma_region)

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
add_executable(test_relay_reasm test_relay_reasm.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c)
target_include_directories(test_relay_reasm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
)
target_compile_definitions(test_relay_reasm PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
target_link_libraries(test_relay_reasm PRIVATE ${NET_TEST_LIBS})
rpcemu_add_test(test_relay_reasm)

# The transmit path against mbuf chains a guest can build: an over-long segment
# used to wrap the 32-bit length accumulator past the size check and be copied
# into a fixed buffer regardless, and a circular chain was followed for ever.
# Links network-nat.c against a stubbed emulator and a plain array standing in
# for guest RAM, so no SLiRP is needed. See src/network-nat.c.
# Capturing the frames a machine sends and receives: the pcap headers another
# program has to be able to open, and the ring the analyser and the control
# socket read from a different thread than the one filling it. Compiles
# netcapture.c directly - it needs nothing from the rest of the emulator.
add_executable(test_netcapture test_netcapture.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/netcapture.c
               test_log_stub.c)
target_include_directories(test_netcapture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_netcapture PRIVATE ${NET_TEST_LIBS})
rpcemu_add_test(test_netcapture)

add_executable(test_net_tx test_net_tx.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/network-nat.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/netcapture.c)
target_include_directories(test_net_tx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
)
target_compile_definitions(test_net_tx PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
target_link_libraries(test_net_tx PRIVATE ${NET_TEST_LIBS})
rpcemu_add_test(test_net_tx)

# The emulated graphics card: its register file, and the checks that stop a
# guest-supplied mode from making the video code read outside the card's own
# framestore. Compiles gfxcard.c directly and stubs the backplane, because
# linking podules.c would drag in every bundled expansion card and the plugin
# loader with them - and rpcemu_core would drag in SDL, which is what broke the
# macOS arm64 build once already (see test_mode_fit above).
# edid.c comes along because the card serves the monitor's EDID over DDC; like
# display_mode.c it is dependency-free, so this still links nothing.
# Vertical sync must be produced by the display tick, not by pixels being
# delivered. It used to ride on a frame reaching the host, so a screen that was
# not changing produced no vertical sync at all - which deadlocks any guest that
# waits for one before drawing, and is what made every ADFFS game hang with a
# black screen and no fault of any kind. Links rpcemu_core for the real IOMD and
# VIDC; test_stubs.c makes vidctrymutex() fail, so drawscr() draws nothing here,
# which is precisely the case that used to be silent.
add_executable(test_vsync test_vsync.c test_stubs.c)
target_link_libraries(test_vsync PRIVATE rpcemu_core m)
rpcemu_add_test(test_vsync)

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
# Which ROMs may be offered the graphics card. Compiles romload.c directly and
# stubs the handful of symbols it wants from the rest of the emulator, so it
# links nothing and can build ROM images of its own to probe.
add_executable(test_rom_gfxcard test_rom_gfxcard.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/romload.c)
target_include_directories(test_rom_gfxcard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_rom_gfxcard)

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
# The JSON wire format, which is somebody else's
# format: if we disagree with it there is nobody on our side to notice. Compiles
# net_json.c directly with stubs for the emulator it would otherwise reach.
add_executable(test_net_json test_net_json.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/net_json.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/net_switch.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/net_slot.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c
               test_log_stub.c)
target_include_directories(test_net_json PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
    target_link_libraries(test_net_json PRIVATE ws2_32)
endif()
rpcemu_add_test(test_net_json)

# The wire between machines on this host: what a machine keeps off it, and that
# a frame put in one end comes out of the others. Compiles net_switch.c directly
# rather than linking rpcemu_core, supplying its own network_hwaddr and inject
# function - the unit needs neither an emulator nor a guest.
add_executable(test_net_switch test_net_switch.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/net_switch.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/net_slot.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c
               test_log_stub.c)
target_include_directories(test_net_switch PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
    target_link_libraries(test_net_switch PRIVATE ws2_32)
endif()
rpcemu_add_test(test_net_switch)

# The Access relay's inbound datagram path: the receive buffer that silently
# truncated every ShareFS data datagram, and the forged source address on the
# fragmenting path. broadcast_relay.c is included by the test rather than
# compiled here, because both are static.
# What a cloned machine's configuration must stop sharing with its original.
# config_paths.cpp is compiled in; it needs wx for wxFileConfig but no display.
add_executable(test_clone_config test_clone_config.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui/config_paths.cpp
               test_stubs.c)
target_include_directories(test_clone_config PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
target_link_libraries(test_clone_config PRIVATE rpcemu_core m)
rpcemu_setup_wxwidgets(test_clone_config)
rpcemu_add_test(test_clone_config)

# The MAC address field's typing rules and the generator behind them. No wx:
# mac_address_input.h is deliberately free of it so it can be tested like this.
add_executable(test_mac_address test_mac_address.cpp test_stubs.c)
target_include_directories(test_mac_address PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
target_link_libraries(test_mac_address PRIVATE rpcemu_core m)
rpcemu_add_test(test_mac_address)

# Which addresses the guests live on. The relay derives the same ones
# independently, so the arithmetic is pinned down here.
add_executable(test_guest_subnet test_guest_subnet.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c
               test_log_stub.c)
target_include_directories(test_guest_subnet PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_guest_subnet)

add_executable(test_net_slot test_net_slot.c)
target_include_directories(test_net_slot PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
    target_link_libraries(test_net_slot PRIVATE ws2_32)
endif()
rpcemu_add_test(test_net_slot)

# The Access relay's inbound datagram path: the receive buffer that silently
# truncated every ShareFS data datagram, and the forged source address on the
# fragmenting path. broadcast_relay.c is included by the test rather than
# compiled here, because both are static.
add_executable(test_relay_datagram test_relay_datagram.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/guest_subnet.c)
target_include_directories(test_relay_datagram PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
    target_link_libraries(test_relay_datagram PRIVATE ws2_32 iphlpapi)
endif()
rpcemu_add_test(test_relay_datagram)

# The structure offsets the guest network driver hardcodes. It reads the equates
# out of riscos-progs/EtherRPCEm/etherrpcem.s and checks them against the host's
# own declarations in src/network.h and against models of Acorn's DCI4 headers,
# so the source path is passed as an argument. Nothing is compiled from the
# module: the assembler source is the input, and it does not need the ARM tools
# to be installed for this test to run.
add_executable(test_etherrpcem_layout test_etherrpcem_layout.c)
target_include_directories(test_etherrpcem_layout PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_etherrpcem_layout
                ${CMAKE_CURRENT_SOURCE_DIR}/../riscos-progs/EtherRPCEm/etherrpcem.s)

# Where the Manager draws a machine's screen inside its panel, and where a point
# in that panel lands on that screen. Arithmetic only - the geometry lives in a
# header of its own (src/gui/remote_display_geometry.h) precisely so that it can
# be checked without a display, a machine or a running Manager. The two answers
# disagreeing is what made the guest pointer move more slowly than the hand.
add_executable(test_remote_display test_remote_display.c)
target_include_directories(test_remote_display PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/gui)
rpcemu_add_test(test_remote_display)

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
# Which shipped guest-side files to bring into the data directory. The decision
# is the whole feature - copy too little and a new emulator runs old guest
# modules, copy too much and somebody's own module is overwritten - and both
# failures are silent, so the logic is kept free of the filesystem and tested
# here on its own.
add_executable(test_support_files test_support_files.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/support_files.c)
target_include_directories(test_support_files PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
rpcemu_add_test(test_support_files)

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
set(RPCEMU_EXPECTED_TESTS 65)	# tests that build on every platform
if(RPCEMU_JIT_TESTS)
    math(EXPR RPCEMU_EXPECTED_TESTS "${RPCEMU_EXPECTED_TESTS} + 10")	# JIT differential
endif()
if(RPCEMU_PYTHON_TESTS)
    math(EXPR RPCEMU_EXPECTED_TESTS "${RPCEMU_EXPECTED_TESTS} + 1")	# abort watcher
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
