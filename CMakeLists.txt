cmake_minimum_required(VERSION 3.16)

project(reaper_git
  VERSION 0.3.1
  DESCRIPTION "REAPER extension: git client panel"
  LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Version shown at the bottom right of the panel. The build script asks for it
# and passes it as -DREAPER_GIT_VERSION=..., so it can be changed per build
# without touching the sources; it defaults to the project version above.
set(REAPER_GIT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version displayed in the panel")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The extension is a module loaded into REAPER: everything it needs is linked
# statically (libgit2 + libssh2 themselves are built below).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

if(MSVC)
  # Same static CRT for the extension and every vendored static library.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Vendored SDK pieces, see third_party/README.md.
set(REAPER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reaper-sdk")
set(WDL_SWELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/swell")

# ---------------------------------------------------------------------------
# libssh2 (static) - SSH transport for libgit2
# ---------------------------------------------------------------------------
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(WIN32)
  # OpenSSL is needed for the key exchanges modern servers offer: the CNG
  # backend has neither ECDH (LIBSSH2_ECDSA) nor curve25519, so SSH fails with
  # "Unable to exchange encryption keys". Fall back to CNG only when OpenSSL is
  # unavailable for the target, and say so loudly.
  find_package(OpenSSL QUIET)
  if(OPENSSL_FOUND)
    set(CRYPTO_BACKEND "OpenSSL" CACHE STRING "libssh2 crypto backend" FORCE)
    set(REAPER_GIT_WIN_OPENSSL TRUE)
  else()
    message(WARNING
      "OpenSSL was not found for the Windows target: libssh2 falls back to WinCNG, "
      "which supports neither ECDH nor curve25519. SSH to modern servers will fail "
      "with 'Unable to exchange encryption keys'. Install OpenSSL development files "
      "for the target to enable SSH.")
    set(CRYPTO_BACKEND "WinCNG" CACHE STRING "libssh2 crypto backend" FORCE)
  endif()
else()
  set(CRYPTO_BACKEND "OpenSSL" CACHE STRING "libssh2 crypto backend" FORCE)
endif()

add_subdirectory(third_party/libssh2 EXCLUDE_FROM_ALL)

# libssh2 is compiled as C90 (__STRICT_ANSI__), which hides the BSD types
# (u_int/u_char) its chacha/poly1305 code uses. Ask glibc for them explicitly.
target_compile_definitions(libssh2_static PRIVATE _DEFAULT_SOURCE)

# Predictable location/name so libgit2's find_package(LibSSH2) works even
# though libssh2 is built as part of this project.
set_target_properties(libssh2_static PROPERTIES
  OUTPUT_NAME "ssh2"
  PREFIX "${CMAKE_STATIC_LIBRARY_PREFIX}"
  SUFFIX "${CMAKE_STATIC_LIBRARY_SUFFIX}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  POSITION_INDEPENDENT_CODE ON
)

set(LIBSSH2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libssh2/include"
    CACHE PATH "libssh2 headers" FORCE)
set(LIBSSH2_LIBRARY "${CMAKE_BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssh2${CMAKE_STATIC_LIBRARY_SUFFIX}"
    CACHE FILEPATH "libssh2 library" FORCE)

# ---------------------------------------------------------------------------
# libgit2 (static, with SSH + HTTPS)
# ---------------------------------------------------------------------------
set(USE_SSH "libssh2" CACHE STRING "SSH transport provider" FORCE)
if(WIN32)
  # No OpenSSL needed on Windows: Schannel + WinCNG come with the OS.
  set(USE_HTTPS "Schannel" CACHE STRING "HTTPS transport provider" FORCE)
else()
  set(USE_HTTPS "OpenSSL" CACHE STRING "HTTPS transport provider" FORCE)
endif()
set(USE_BUNDLED_ZLIB "Bundled" CACHE STRING "zlib provider" FORCE)
# "builtin" keeps libgit2's own regex engine, so the extension does not depend
# on the system PCRE2 library at runtime.
set(REGEX_BACKEND "builtin" CACHE STRING "regex provider" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/libgit2 EXCLUDE_FROM_ALL)
add_dependencies(libgit2package libssh2_static)

# ---------------------------------------------------------------------------
# The extension itself
# ---------------------------------------------------------------------------
set(REAPER_GIT_SOURCES
  src/extension.cpp
  src/git_logall.cpp
  src/git_service.cpp
  src/panel.cpp
  src/settings.cpp
  src/ui.cpp
)

# A REAPER extension is a plain module: .dll on Windows, .so on Linux.
add_library(reaper_git MODULE ${REAPER_GIT_SOURCES})

target_include_directories(reaper_git PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Third-party headers live in SYSTEM include dirs so their warnings stay quiet.
target_include_directories(reaper_git SYSTEM PRIVATE
  "${REAPER_SDK_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libgit2/include"
)

target_link_libraries(reaper_git PRIVATE libgit2package libssh2_static)

# SWELL (and windows.h) define min/max macros unless this is set; they break
# <algorithm>.
target_compile_definitions(reaper_git PRIVATE NOMINMAX)
target_compile_definitions(reaper_git PRIVATE REAPER_GIT_VERSION="${REAPER_GIT_VERSION}")

if(WIN32)
  # Windows: REAPER's own Win32 API is used directly, no SWELL needed.
  target_compile_definitions(reaper_git PRIVATE WIN32_LEAN_AND_MEAN)
  target_link_libraries(reaper_git PRIVATE user32 crypt32 secur32 ws2_32 shell32 ole32)
  if(REAPER_GIT_WIN_OPENSSL)
    # libssh2 is static and built against OpenSSL here, so its symbols have to be
    # resolved in the module itself.
    target_link_libraries(reaper_git PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()
  if(MINGW)
    # A MinGW build otherwise imports libstdc++-6.dll, libgcc_s_seh-1.dll and
    # libwinpthread-1.dll, which do not exist on a normal Windows machine: REAPER
    # then silently fails to load the file. Link the runtime into the module.
    target_link_options(reaper_git PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  # Linux: REAPER provides the SWELL (Win32 emulation) API to the plug-in.
  # swell-modstub-generic.cpp turns every SWELL call into a function pointer
  # that REAPER fills in when it loads the .so.
  target_sources(reaper_git PRIVATE "${WDL_SWELL_DIR}/swell-modstub-generic.cpp")
  target_compile_definitions(reaper_git PRIVATE SWELL_PROVIDED_BY_APP)
  target_include_directories(reaper_git SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL")
  # Vendored code: keep its warnings out of our build log.
  set_source_files_properties("${WDL_SWELL_DIR}/swell-modstub-generic.cpp" PROPERTIES COMPILE_OPTIONS "-w")

  find_package(Threads REQUIRED)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(reaper_git PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(reaper_git PRIVATE /W4)
else()
  target_compile_options(reaper_git PRIVATE -Wall -Wextra)
endif()

set_target_properties(reaper_git PROPERTIES
  PREFIX ""
  OUTPUT_NAME "reaper_git"
  # Windows wants <name>.dll in REAPER/Plugins, Linux <name>.so in
  # ~/.config/REAPER/UserPlugins. CMake picks the right suffix per platform.
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
# Keep multi-config generators (Visual Studio, Xcode) out of per-config subdirs.
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(reaper_git PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin"
  )
endforeach()
