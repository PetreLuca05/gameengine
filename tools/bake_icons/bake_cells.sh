#!/bin/sh
# Bakes every item's inventory cell image (docs/INVENTORY.md "Cell images"): runs the game with the
# bake_cells command, which renders each item through the UI item view renderer at its grid aspect
# (orthographic, transparent) into assets/ui/cells/<id>.png of the source tree, plus <id>_r.png
# turned a quarter for rotatable items. Run after adding items or changing grid sizes / models,
# then commit the PNGs. (Without a GPU, tools/bake_icons/placeholder_cells.py fills gaps.)
#   tools/bake_icons/bake_cells.sh [build dir (default: build)]
here=$(cd "$(dirname "$0")/../.." && pwd)
bin="$here/${1:-build}/bin"
cfg="$bin/bake_cells.cfg"
printf 'wait 30\nbake_cells quit\n' > "$cfg"
EXE=./deadground.exe; [ -x "$bin/deadground" ] && EXE=./deadground   # macOS / Linux build
cd "$bin" && "$EXE" --test --nosound --script "$cfg"
rm -f "$cfg"
ls "$here/assets/ui/cells" | wc -l
