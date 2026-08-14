# The Android front end: one machine, one screen, SDL2.
#
# Built only for Android, and reached from src/CMakeLists.txt, which skips gui/
# and tools/ for this target. See docs/android.md.
#
# SDL2 is fetched rather than found. pkg-config cannot be used here - it ignores
# the cross-compile root and answers with the host's packages, which is why
# src/CMakeLists.txt skips it for Android entirely - and there is no Android SDL2
# to find in any case. Pinned by URL and hash rather than a git tag: a tag can be
# moved and a hash cannot, the tarball is a fraction of SDL's git history, and no
# git is needed at configure time, which matters on a CI runner.
#
# SDL2 2.32.x is the current SDL2 line. SDL3 is NOT interchangeable here: the core
# already contains an SDL2 audio path (podules/common/sound_out_sdl2.c) written
# against SDL_AudioDeviceID and SDL_CloseAudioDevice, which SDL3 changed.

include(FetchContent)

set(RPCEMU_SDL2_VERSION 2.32.10)

# Shared, not static: the Java side loads libSDL2.so by name, and SDL's own
# Android activity expects to find it in the APK.
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL2
    URL      https://github.com/libsdl-org/SDL/releases/download/release-${RPCEMU_SDL2_VERSION}/SDL2-${RPCEMU_SDL2_VERSION}.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
)
FetchContent_MakeAvailable(SDL2)

# The library has to be called "main". SDL's SDLActivity loads "SDL2" and then
# "main" by those names (SDLActivity.getLibraries()), so renaming this breaks the
# app with nothing more informative than a missing library.
# keyboard_x.c is the authoritative X11-keycode to PS/2 Set 2 table. It is a pure
# table with no X11 dependency (stddef.h and stdint.h only), so it is compiled in
# here rather than the mapping being duplicated for Android.
add_library(main SHARED
    rpcemu_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../gui/keyboard_x.c
)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# log for __android_log_print, android for the NDK's own bits, and SDL2main
# because SDL renames our main() to SDL_main and supplies the real entry point.
target_link_libraries(main PRIVATE
    rpcemu_core
    SDL2::SDL2
    SDL2::SDL2main
    log
    android
    m
)

message(STATUS "Android front end: SDL2 ${RPCEMU_SDL2_VERSION}, target libmain.so")
