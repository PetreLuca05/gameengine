#!/bin/sh
# vgeo vs the same meshes on the legacy path (docs/VGEO.md "Tests"): the devroom rock test scene
# rendered by vgeo (r_era 0) and by the discrete-LOD fallback at LOD 0 detail (r_era 2,
# r_vgeo_lod_px 0.1), TAA / motion blur / HUD text off; at most 1 % of the pixels may differ by more
# than 24 / 255. Run from the build output directory (build/bin).
OUT=${OUT:-.}
EXE=./deadground; [ -x ./deadground.exe ] && EXE=./deadground.exe
cat > vgeo_legacy_diff.cfg <<CFG
r_maxfps 60
r_aa 0
r_motion_blur 0
cl_showfps 0
time_set 15:00
time_pause 1
setpos 0 0 -400 0 -8
wait 20
vgeo_test_scene 400 8 2
wait 60
screenshot $OUT/vgeo_diff_vgeo.png
wait 2
r_era 2
r_vgeo_lod_px 0.1
wait 60
screenshot $OUT/vgeo_diff_legacy.png
wait 2
quit
CFG
"$EXE" --test --nosound --log vgeo_legacy_diff.log --script vgeo_legacy_diff.cfg || exit 1
IMG=./img_diff; [ -x ./img_diff.exe ] && IMG=./img_diff.exe
"$IMG" "$OUT/vgeo_diff_vgeo.png" "$OUT/vgeo_diff_legacy.png" 24 1.0
