# Vendored SDL2 Java sources

These nine files are SDL2's own Android support, copied unmodified from
`android-project/app/src/main/java/org/libsdl/app/` in the **SDL2 2.32.10**
release tarball. They are under SDL's zlib licence, not RPCEmu's GPL.

They are vendored rather than fetched because they are needed at *Java compile*
time, while the native SDL2 is fetched by CMake during the native build - so
pointing Gradle at the fetched copy would be a build-ordering race. Copying
`android-project` is also the documented way to build an SDL2 Android app.

**They must stay in step with the version CMake fetches**, which is
`RPCEMU_SDL2_VERSION` in `src/gui-android/CMakeLists.txt`. To update:

    tar xzf SDL2-<version>.tar.gz
    cp SDL2-<version>/android-project/app/src/main/java/org/libsdl/app/*.java \
       src/gui-android/android/app/src/main/java/org/libsdl/app/

and change `RPCEMU_SDL2_VERSION` and the `URL_HASH` to match.
