#!/bin/sh
# Builds the plug-in. Run from the project root:
#
#   ./scripts/build.sh
#
# Asks for the version to stamp into this build, then whether to build for
# Linux, for Windows, or both. The version becomes the text shown at the bottom
# right of the panel.
set -e

# libgit2 and libssh2 are submodules: a clone made without --recursive leaves
# those folders empty and the build cannot configure, so fetch them first.
if [ ! -f third_party/libgit2/CMakeLists.txt ] || [ ! -f third_party/libssh2/CMakeLists.txt ]; then
  echo "Fetching submodules (libgit2, libssh2) ..."
  git submodule update --init --recursive
fi

# Default version: the one declared in CMakeLists.txt.
default_version=$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\)$/\1/p' \
  CMakeLists.txt | head -n 1)
[ -n "$default_version" ] || default_version=0.0.0

ask_yes() {
  printf '%s [y/N] ' "$1"
  read -r answer
  case "$answer" in
    y | Y | yes | Yes | YES) return 0 ;;
    *) return 1 ;;
  esac
}

printf 'Version [%s]: ' "$default_version"
read -r version
[ -n "$version" ] || version="$default_version"

want_linux=0
want_windows=0
ask_yes "Build for Linux?" && want_linux=1
ask_yes "Build for Windows (MinGW cross-compile)?" && want_windows=1

if [ "$want_linux" -eq 0 ] && [ "$want_windows" -eq 0 ]; then
  echo "Nothing selected, exiting."
  exit 0
fi

if [ "$want_linux" -eq 1 ]; then
  echo "Building for Linux (version $version) ..."
  cmake --preset linux -DREAPER_GIT_VERSION="$version"
  cmake --build --preset linux
  echo "Linux build: build/linux/bin/reaper_git.so"
fi

if [ "$want_windows" -eq 1 ]; then
  echo "Building for Windows (version $version) ..."
  cmake --preset windows-mingw -DREAPER_GIT_VERSION="$version"
  cmake --build --preset windows-mingw
  echo "Windows build: build/windows/bin/reaper_git.dll"
fi
