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

    # The defines wx's own headers need (__WXGTK__ and friends): without them
    # wx/defs.h refuses to compile at all, so the check below cannot run.
    set(_wx_defs "")
    if(CMAKE_CROSSCOMPILING)
        foreach(_f IN LISTS _wx_cxxflags_list)
            if(_f MATCHES "^-D")
                list(APPEND _wx_defs "${_f}")
            endif()
        endforeach()
    else()
        foreach(_d IN LISTS wxWidgets_DEFINITIONS)
            list(APPEND _wx_defs "-D${_d}")
        endforeach()
    endif()

    rpcemu_check_wx_webrequest("${_wx_inc_dirs}" "${_wx_defs}")
endfunction()

# Check that this wxWidgets actually has wxWebRequest in it.
#
# The package manager and the "download RISC OS" feature are built on
# wxWebRequest, which arrived in wxWidgets 3.1.5. Having a new enough wxWidgets
# is not sufficient: wx/webrequest.h wraps the whole class in
# "#if wxUSE_WEBREQUEST", and a wxWidgets configured without a backend for it -
# libcurl on Linux, WinHTTP on Windows, NSURLSession on macOS - sets that to 0.
# The header then exists and declares nothing.
#
# Left alone that surfaces as around sixty lines of "'wxWebRequest' has not been
# declared" from src/gui/http_transfer.h, 90% of the way through a build, which
# says nothing about the cause. Reported from a Raspberry Pi, 31/07/2026.
#
# The check is deliberately careful about when it fails. A configure test that
# gets a false negative would block somebody whose build works perfectly, which
# is worse than the wall of errors. So it only refuses when it can prove the
# case: the header compiles, and the same file with wxUSE_WEBREQUEST asserted
# does not. Anything else is reported and allowed through.
function(rpcemu_check_wx_webrequest inc_dirs defs)
    if(DEFINED RPCEMU_WX_WEBREQUEST_CHECKED)
        return()
    endif()
    set(RPCEMU_WX_WEBREQUEST_CHECKED TRUE CACHE INTERNAL "")

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${inc_dirs})
    set(CMAKE_REQUIRED_DEFINITIONS ${defs})
    set(CMAKE_REQUIRED_QUIET TRUE)

    # Compile only, do not link. check_cxx_source_compiles() links by default,
    # and merely including wx headers emits inline code that wants symbols from
    # the wx libraries - which this test is not given, so every answer would
    # come back "no" and the check would be useless.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

    check_cxx_source_compiles(
        "#include <wx/webrequest.h>\nint main() { return 0; }"
        RPCEMU_WX_HEADER_USABLE)

    if(NOT RPCEMU_WX_HEADER_USABLE)
        # Cannot tell: the header would not compile here for some other reason,
        # or the flags this test was given are not the ones the build uses.
        message(STATUS
            "wxWidgets: could not test for wxWebRequest support; carrying on")
        return()
    endif()

    check_cxx_source_compiles(
        "#include <wx/webrequest.h>\n#if !wxUSE_WEBREQUEST\n#error no webrequest\n#endif\nint main() { return 0; }"
        RPCEMU_WX_HAS_WEBREQUEST)

    if(NOT RPCEMU_WX_HAS_WEBREQUEST)
        message(FATAL_ERROR
            "This wxWidgets was built without wxWebRequest support "
            "(wxUSE_WEBREQUEST is 0 in its setup.h), so wx/webrequest.h "
            "declares nothing and RPCEmu's package manager and RISC OS "
            "download cannot compile.\n"
            "wxWebRequest needs a backend: libcurl on Linux, WinHTTP on "
            "Windows, NSURLSession on macOS. Either install a distribution "
            "wxWidgets built with one (on Debian and Raspberry Pi OS, "
            "libwxgtk3.2-dev), or if you built wxWidgets yourself, install "
            "libcurl4-openssl-dev and configure it again - wx turns "
            "wxUSE_WEBREQUEST off silently when it can find no backend.\n"
            "To check what you have:\n"
            "  grep 'define wxUSE_WEBREQUEST ' $(wx-config --cflags | tr ' ' "
            "'\\n' | grep '^-I' | sed 's/^-I//')/wx/setup.h")
    endif()

    message(STATUS "wxWidgets: wxWebRequest available")
endfunction()
