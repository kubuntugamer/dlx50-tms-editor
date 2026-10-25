# ---------------------------------------------------------------------------
# CMake toolchain file: Linux (build) -> 64-bit Windows (target)
# Compiler : MinGW-w64 (x86_64-w64-mingw32-g++)
# Qt       : Qt 6.8.3 for Windows (win64_mingw), installed via aqtinstall
#
# Usage:
#   cmake -S . -B build-win \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win --parallel
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# --- MinGW-w64 cross compilers -------------------------------------------
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Static-link libstdc++/libgcc/libpthread into the .exe so the binary has
# fewer external DLL dependencies. Qt DLLs are still dynamic (bundled by
# windeployqt later).
set(CMAKE_CXX_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive")

# --- Qt for Windows (target) ---------------------------------------------
set(QT_WIN_ROOT "/home/chazz/qt-sdk/6.8.3/mingw_64" CACHE PATH "Qt 6.8.3 win64_mingw root")
set(CMAKE_PREFIX_PATH "${QT_WIN_ROOT}")

# --- Host Qt (Linux) used to run moc / rcc / uic natively -----------------
set(QT_HOST_ROOT "/home/chazz/qt-sdk/6.8.3/gcc_64" CACHE PATH "Qt 6.8.3 linux_gcc_64 root (host tools)")

# --- Search-path behaviour: only look in target dirs for libs/headers ------
set(CMAKE_FIND_ROOT_PATH "${QT_WIN_ROOT}" "/usr/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # host programs (moc, etc.)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)    # target libs
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)    # target headers
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)    # target CMake packages

# Let Qt's CMake run the target's tools (if any) through Wine.
set(CMAKE_CROSSCOMPILING_EMULATOR "/usr/bin/wine")

# Qt6 needs to know the host machine's tools (moc/rcc/uic) when cross-building.
set(QT_HOST_PATH "${QT_HOST_ROOT}" CACHE PATH "Host Qt for moc/rcc/uic")
