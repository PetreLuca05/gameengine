#!/bin/sh
# Bakes every item icon (docs/UI.md "Icons and preview"): runs the game headless-ish with the
# bake_icons command, which renders each item through the UI item view renderer (orthographic
# 3/4 view, studio lighting, transparent background) into assets/ui/icons/{128,256}/<id>.png of
# the source tree. Run after adding items or changing models, then commit the PNGs.
#   tools/bake_icons/bake_icons.sh [build dir (default: build)]
here=$(cd "$(dirname "$0")/../.." && pwd)
bin="$here/${1:-build}/bin"
cfg="$bin/bake_icons.cfg"
printf 'wait 30\nbake_icons quit\n' > "$cfg"
EXE=./deadground.exe; [ -x "$bin/deadground" ] && EXE=./deadground   # macOS / Linux build
cd "$bin" && "$EXE" --test --nosound --script "$cfg"
rm -f "$cfg"
ls "$here/assets/ui/icons/256" | wc -l
