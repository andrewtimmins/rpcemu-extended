# Standalone host-side client for the DebugCmd channel - the counterpart of
# rpcemu-run below. That one drives RISC OS's command line; this one drives the
# processor underneath it, which is a different socket speaking a different
# protocol. Like rpcemu-run it does not link the emulator core.
# machine_lock.c comes along so the client can read where a running machine
# recorded its debugger socket - the only way to find one whose configuration
# named a path of its own.
add_executable(rpcemu-debug rpcemu_debug.c ${CMAKE_CURRENT_SOURCE_DIR}/../machine_lock.c)
target_include_directories(rpcemu-debug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(rpcemu-debug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(WIN32)
    target_link_libraries(rpcemu-debug PRIVATE ws2_32)
endif()
install(TARGETS rpcemu-debug RUNTIME DESTINATION bin)

# Standalone host-side client for the NetCapCmd channel: the network the
# machine is on, rather than its command line or its processor. Carries
# machine_lock.c for the same reason rpcemu-debug does - a running machine
# records the socket it bound, and that is the only way to find one whose
# configuration named a path of its own.
add_executable(rpcemu-netcap rpcemu_netcap.c ${CMAKE_CURRENT_SOURCE_DIR}/../machine_lock.c)
target_include_directories(rpcemu-netcap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(rpcemu-netcap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(WIN32)
    target_link_libraries(rpcemu-netcap PRIVATE ws2_32)
endif()
install(TARGETS rpcemu-netcap RUNTIME DESTINATION bin)

# Standalone host-side client for the HostCmd channel. Does not link the
# emulator core - it is just a small socket client.

# machine_lock.c for the same reason rpcemu-debug and rpcemu-netcap need it:
# a running machine records where its channel is, and a socket file left
# behind by one that was killed must not be mistaken for a live machine.
add_executable(rpcemu-run rpcemu_run.c vdu_filter.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../machine_lock.c)
set_target_properties(rpcemu-run PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Winsock for the socket client (Linux gets sockets from libc).
if(WIN32)
    target_link_libraries(rpcemu-run PRIVATE ws2_32)
endif()
install(TARGETS rpcemu-run RUNTIME DESTINATION bin)

# rpcemu-shell is the same binary; its interactive mode is selected by argv[0].
# Ship it under both names. Windows has no reliable symlinks, so copy the .exe;
# elsewhere a symlink keeps it a single file.
if(WIN32)
    add_custom_command(TARGET rpcemu-run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:rpcemu-run>
                ${CMAKE_BINARY_DIR}/bin/rpcemu-shell.exe
        COMMENT "Copying rpcemu-run to rpcemu-shell.exe")
    install(PROGRAMS ${CMAKE_BINARY_DIR}/bin/rpcemu-shell.exe DESTINATION bin)
else()
    add_custom_command(TARGET rpcemu-run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink rpcemu-run
                ${CMAKE_BINARY_DIR}/bin/rpcemu-shell
        COMMENT "Creating rpcemu-shell symlink")
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink \
        rpcemu-run \$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rpcemu-shell)")
endif()
