include(${CMAKE_SOURCE_DIR}/cmake/FindWxWidgets.cmake)

set(GUI_SOURCES
    main.cpp
    data_paths.cpp
    main_frame.cpp
    main_frame_menus.cpp
    headless_bridge.cpp
    headless_main.cpp
    headless_selector.cpp
    vnc_app.cpp
    vnc_overlay.cpp
    machine_selector.c
    text_screen.c
    console_font.c
    emulator_panel.cpp
    emulator_host.cpp
    emulator_snapshot.cpp
    gui_preferences.cpp
    input_helpers.cpp
    config_selector_dialog.cpp
    config_paths.cpp
    folder_transfer.cpp
    machine_edit_dialog.cpp
    new_machine_dialog.cpp
    guest_command.cpp
    package_dialog.cpp
    usb_dialog.cpp
    package_index.cpp
    package_install.cpp
    package_sources.cpp
    package_sources_dialog.cpp
    riscos_fetch.cpp
    riscos_setup_dialog.cpp
    help_actions.cpp
    support_bundle.cpp
    podule_config_dialog.cpp
    machine_inspector_window.cpp
    about_dialog.cpp
    nat_edit_dialog.cpp
    nat_list_dialog.cpp
    serial_dialog.cpp
    parallel_dialog.cpp
    settings.cpp
    plt_sound.cpp
    toolbar_icons.cpp
    keyboard_x.c
    keymap_platform.c
    held_keys.c
    data_dir_choice.c
    data_dir_store.c
    data_dir_dialog.cpp
)

if(RPCEMU_ENABLE_VNC)
    list(APPEND GUI_SOURCES vnc_server.cpp vnc_dialog.cpp)
endif()

if(RPCEMU_DYNAREC)
    set(RPCEMU_GUI_TARGET rpcemu-recompiler)
else()
    set(RPCEMU_GUI_TARGET rpcemu-interpreter)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RPCEMU_GUI_TARGET "${RPCEMU_GUI_TARGET}-debug")
endif()

# Embed the toolbar SVG icon pack (src/gui/icons/*.svg) into the binary as
# NUL-terminated byte arrays, so the icons need no runtime files and ship in
# every build. Pure-CMake generation (file(READ HEX)) - no external tool, works
# on Linux and MSYS2. Regenerated on configure; re-run cmake after editing an SVG.
set(RPCEMU_TOOLBAR_SVGS
    screenshot floppy cdrom reset mute unmute
    fullscreen settings start pause step inspector)
set(_svg_header "${CMAKE_CURRENT_BINARY_DIR}/toolbar_icons_svg.h")
file(WRITE "${_svg_header}"
     "// Generated from src/gui/icons/*.svg by CMake - do not edit.\n#pragma once\n\n")
foreach(_svg ${RPCEMU_TOOLBAR_SVGS})
    set(_svg_path "${CMAKE_CURRENT_SOURCE_DIR}/icons/${_svg}.svg")
    if(NOT EXISTS "${_svg_path}")
        message(FATAL_ERROR "Toolbar icon missing: ${_svg_path}")
    endif()
    file(READ "${_svg_path}" _hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
    file(APPEND "${_svg_header}"
         "static const unsigned char ${_svg}_svg[] = { ${_bytes} 0x00 };\n")
endforeach()

add_executable(${RPCEMU_GUI_TARGET} ${GUI_SOURCES})

target_include_directories(${RPCEMU_GUI_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(${RPCEMU_GUI_TARGET} PRIVATE
    RPCEMU_VERSION="${RPCEMU_VERSION}"
    RPCEMU_INSTALL_DATADIR="${CMAKE_INSTALL_PREFIX}/share/rpcemu"
)

if(RPCEMU_DYNAREC)
    target_compile_definitions(${RPCEMU_GUI_TARGET} PRIVATE RPCEMU_BUILD_DYNAREC=1)
endif()

if(RPCEMU_ENABLE_VNC)
    target_compile_definitions(${RPCEMU_GUI_TARGET} PRIVATE RPCEMU_VNC)
endif()

target_link_libraries(${RPCEMU_GUI_TARGET} PRIVATE
    rpcemu_core
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
target_include_directories(${RPCEMU_GUI_TARGET} PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(${RPCEMU_GUI_TARGET} PRIVATE ${SDL2_LIBRARIES})

# GTK3 is only used for the wxGTK relative-mouse capture path in
# emulator_panel.cpp (guarded by __WXGTK__). wxMSW (Windows) and wxOSX (macOS)
# do not use it, so require it on Linux only.
if(NOT WIN32 AND NOT APPLE)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(${RPCEMU_GUI_TARGET} PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(${RPCEMU_GUI_TARGET} PRIVATE ${GTK3_LIBRARIES})
endif()

if(RPCEMU_ENABLE_VNC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBVNCSERVER REQUIRED libvncserver)
    target_include_directories(${RPCEMU_GUI_TARGET} PRIVATE ${LIBVNCSERVER_INCLUDE_DIRS})
    target_link_libraries(${RPCEMU_GUI_TARGET} PRIVATE ${LIBVNCSERVER_LIBRARIES})
endif()

rpcemu_setup_wxwidgets(${RPCEMU_GUI_TARGET})

set_target_properties(${RPCEMU_GUI_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# On Windows, build as a GUI-subsystem app so no console window appears. MinGW
# still enters through the app's main() (libmingw32 provides a WinMain shim), so
# no code change is needed. (The cross build got this implicitly via wx-config's
# -mwindows; the MSYS2 find_package path does not, hence set it explicitly.)
if(WIN32)
    set_target_properties(${RPCEMU_GUI_TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

install(TARGETS ${RPCEMU_GUI_TARGET} RUNTIME DESTINATION bin)

# Desktop launcher + icon (freedesktop.org / Linux only).
if(NOT WIN32)
    # Desktop launcher with Exec= pointing at the actual binary name.
    configure_file(${CMAKE_SOURCE_DIR}/packaging/rpcemu.desktop
                   ${CMAKE_BINARY_DIR}/rpcemu.desktop @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/rpcemu.desktop DESTINATION share/applications)
    # Icon the .desktop's "Icon=rpcemu" resolves to via the hicolor theme.
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/rpcemu.png")
        install(FILES ${CMAKE_SOURCE_DIR}/resources/rpcemu.png
                DESTINATION share/icons/hicolor/256x256/apps)
    endif()
endif()
