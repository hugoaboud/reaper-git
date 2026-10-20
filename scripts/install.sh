#!/bin/sh
# Copies the built Linux module into REAPER's UserPlugins folder.
set -e

source_file=build/linux/bin/reaper_git.so
[ -f "$source_file" ] || source_file=build/bin/reaper_git.so

if [ ! -f "$source_file" ]; then
  echo "No build found. Run scripts/build.sh first." >&2
  exit 1
fi

cp "$source_file" "$HOME/.config/REAPER/UserPlugins"
echo "Installed $source_file into ~/.config/REAPER/UserPlugins"
