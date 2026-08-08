# Standalone host-side client for the DebugCmd channel - the counterpart of
# rpcemu-run below. That one drives RISC OS's command line; this one drives the
# processor underneath it, which is a different socket speaking a different
# protocol. Like rpcemu-run it does not link the emulator core.
add_executable(rpcemu-debug rpcemu_debug.c)
set_target_properties(rpcemu-debug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(WIN32)
    target_link_libraries(rpcemu-debug PRIVATE ws2_32)
endif()
install(TARGETS rpcemu-debug RUNTIME DESTINATION bin)

# Standalone host-side client for the HostCmd channel. Does not link the
# emulator core - it is just a small socket client.

add_executable(rpcemu-run rpcemu_run.c vdu_filter.c)
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
