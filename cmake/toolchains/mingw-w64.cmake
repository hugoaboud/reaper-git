# Cross-compile the extension for 64-bit Windows from Linux with MinGW-w64.
#
#   sudo apt install mingw-w64
#   cmake -S . -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
#   cmake --build build/windows
#
# Output: build/windows/bin/reaper_git.dll

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# BOTH, not ONLY: prebuilt dependencies for the target (OpenSSL for libssh2, for
# example) live outside the sysroot and are pointed at explicitly.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
