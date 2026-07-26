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

# Networking tests for the Access+/ShareFS path, where large copies drive IP
# fragment reassembly. These compile the code under test directly rather than
# linking rpcemu_core, because each replaces a symbol the core also defines: the
# SLiRP output callbacks in one case, sendto() in the other.
if(NOT WIN32)
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
    add_test(NAME test_ip_reass COMMAND test_ip_reass)

    # Relay-side reassembly of guest-originated Access+ datagrams
    add_executable(test_relay_reasm test_relay_reasm.c)
    target_include_directories(test_relay_reasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/slirp
    )
    target_compile_definitions(test_relay_reasm PRIVATE CONFIG_SLIRP _DEFAULT_SOURCE)
    add_test(NAME test_relay_reasm COMMAND test_relay_reasm)
endif()
