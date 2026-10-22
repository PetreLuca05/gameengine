#!/bin/bash
# Item hot reload (docs/ITEM_PIPELINE.md "Edit items in Blender"): a .dgim that changes under the running game
# (an export from Blender) reloads by itself. The game inspects the smg; the pistol's file is copied over the
# build's smg.dgim; the log must say it reloaded, and the screenshots show the swap. The file is restored.
#   tools/tests/item_hot_reload.sh [build dir, default build]   -> <build>/bin/item_hot_reload_{before,after}.png
set -u
BUILD="${1:-build}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/$BUILD/bin"
MESHES="$BIN/assets/items/meshes"
LOG="$BIN/item_hot_reload.log"
CFG="$BIN/item_hot_reload.cfg"
cp "$MESHES/smg.dgim" "$MESHES/smg.dgim.keep" || exit 1
cat > "$CFG" <<'EOF'
r_maxfps 60
wait 30
item_inspect smg
item_inspect_spin 0
item_inspect_orbit 90 -10 1
wait 30
echo hot_reload: inspecting
screenshot item_hot_reload_before.png
wait 2
wait 600
screenshot item_hot_reload_after.png
wait 2
quit
EOF
rm -f "$LOG"
(cd "$BIN" && ./deadground.exe --test --nosound --log "$LOG" --script "$CFG" >/dev/null 2>&1) &
GAME=$!
for _ in $(seq 1 600); do grep -q "hot_reload: inspecting" "$LOG" 2>/dev/null && break; sleep 0.2; done
sleep 1
cp "$MESHES/pistol.dgim" "$MESHES/smg.dgim.tmp" && mv -f "$MESHES/smg.dgim.tmp" "$MESHES/smg.dgim"
wait $GAME
mv -f "$MESHES/smg.dgim.keep" "$MESHES/smg.dgim"
rm -f "$CFG"
if grep -q "item_mesh: assets/items/meshes/smg.dgim changed on disk: reloaded" "$LOG"; then
    echo "item_hot_reload: PASS ($(grep -c 'changed on disk' "$LOG") reload line(s); $BIN/item_hot_reload_{before,after}.png)"
    exit 0
fi
echo "item_hot_reload: FAIL (no reload line in $LOG)"
exit 1
