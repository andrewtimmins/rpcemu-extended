# wxWidgets helper.
#
# The "net" component is wxWebRequest, used to fetch RISC OS from RISC OS Open
# (src/gui/riscos_fetch.cpp). It wraps whichever HTTP stack the platform already
# has - WinHTTP, NSURLSession or libcurl - so it costs no new dependency.
#
# Native builds (Linux/wxGTK, and Windows/MSYS2 in CI) use CMake's standard
# find_package(wxWidgets). Cross builds (Linux -> Windows with MinGW-w64) can't:
# CMake's FindwxWidgets switches to its win32 directory-layout lookup when the
# target is Windows and does not understand the Unix-style install a `--host`
# wxWidgets build produces. For that case query the cross wx-config directly.
function(rpcemu_setup_wxwidgets target)
    if(CMAKE_CROSSCOMPILING)
        if(NOT wxWidgets_CONFIG_EXECUTABLE)
            find_program(wxWidgets_CONFIG_EXECUTABLE
                NAMES wx-config x86_64-w64-mingw32-wx-config
                PATHS ${CMAKE_FIND_ROOT_PATH}/bin
                NO_DEFAULT_PATH)
        endif()
        if(NOT wxWidgets_CONFIG_EXECUTABLE)
            message(FATAL_ERROR
                "Cross-compiling but no wx-config found. Build wxWidgets for the "
                "target, or pass -DwxWidgets_CONFIG_EXECUTABLE=<path>.")
        endif()
        message(STATUS "wxWidgets (cross) via ${wxWidgets_CONFIG_EXECUTABLE}")

        execute_process(
            COMMAND ${wxWidgets_CONFIG_EXECUTABLE} --cxxflags core base net
            OUTPUT_VARIABLE _wx_cxxflags OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _wx_rc)
        if(NOT _wx_rc EQUAL 0)
            message(FATAL_ERROR "wx-config --cxxflags failed (rc=${_wx_rc})")
        endif()
        execute_process(
            COMMAND ${wxWidgets_CONFIG_EXECUTABLE} --libs core base net
            OUTPUT_VARIABLE _wx_libs OUTPUT_STRIP_TRAILING_WHITESPACE)

        separate_arguments(_wx_cxxflags_list NATIVE_COMMAND "${_wx_cxxflags}")
        separate_arguments(_wx_libs_list NATIVE_COMMAND "${_wx_libs}")

        # wx C++ flags carry -I/-D that are harmless for the target's few C files,
        # but scope them to C++ anyway.
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${_wx_cxxflags_list}>)
        target_link_libraries(${target} PRIVATE ${_wx_libs_list})

        # Include dirs the resource compiler needs (for wx/msw/wx.rc).
        set(_wx_inc_dirs "")
        foreach(_f IN LISTS _wx_cxxflags_list)
            if(_f MATCHES "^-I(.+)$")
                list(APPEND _wx_inc_dirs "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    else()
        find_package(wxWidgets REQUIRED COMPONENTS core base net)
        include(${wxWidgets_USE_FILE})
        target_link_libraries(${target} PRIVATE ${wxWidgets_LIBRARIES})
        set(_wx_inc_dirs ${wxWidgets_INCLUDE_DIRS})
    endif()

    # On Windows, wxMSW apps MUST compile in wx/msw/wx.rc: it provides the
    # standard cursors/icons and, critically, the application manifest (comctl32
    # v6 for themed common controls). Without it wxWidgets asserts at startup
    # ("Loading a cursor defined by wxWidgets failed ... include wx/msw/wx.rc").
    if(WIN32)
        enable_language(RC)
        set(_wx_rc_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_wx.rc")
        # Application icon for the .exe (Explorer / taskbar). Explorer uses the
        # alphabetically-first icon resource; wx/msw/wx.rc defines "wxICON_AAA"
        # (its own std.ico) specifically to be early, so ours must be defined
        # BEFORE the include AND sort ahead of it ("APPICON" < "WXICON_AAA").
        set(_app_ico "${CMAKE_SOURCE_DIR}/resources/rpcemu.ico")
        if(EXISTS "${_app_ico}")
            file(WRITE "${_wx_rc_file}" "APPICON ICON \"${_app_ico}\"\n")
            file(APPEND "${_wx_rc_file}" "#include \"wx/msw/wx.rc\"\n")
        else()
            file(WRITE "${_wx_rc_file}" "#include \"wx/msw/wx.rc\"\n")
        endif()
        set(_wx_rc_flags "")
        foreach(_d IN LISTS _wx_inc_dirs)
            string(APPEND _wx_rc_flags " -I\"${_d}\"")
        endforeach()
        set_source_files_properties("${_wx_rc_file}" PROPERTIES
            LANGUAGE RC COMPILE_FLAGS "${_wx_rc_flags}")
        target_sources(${target} PRIVATE "${_wx_rc_file}")
    endif()
endfunction()
