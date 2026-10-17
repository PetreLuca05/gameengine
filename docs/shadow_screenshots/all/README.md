# Shadow fix: every capture

All captures from the shadow fix (docs/SHADOWS.md), rendered headless on Linux (Xvfb + Mesa
lavapipe) in the dev room. "Before" is the build before commit 18a248b, "after" is that commit.
Full resolution, stored as JPEG (quality 90).

| Folder | What |
|---|---|
| `01_before_after_side_by_side` | Each scene, before (left) / after (right); `boots` = the feet looking down (zoomed), `hands` = the first-person hands in three scenes |
| `02_scenes_before`, `03_scenes_after` | The scripted scenes: `a` noon fence, `b` noon shadow corner, `c` / `c2` sunset, `d` far views at two headings, `e` cascade view (`r_shadow_debug 4`), `f` two FXAA frames (flicker check), `g` viewmodel by the wall. The fence and test-corner objects exist only in the after build's level |
| `04_feet_looking_down_before`, `05_..._after` | Looking down (pitch -80 / -75) at noon and sunset, two headings: the leg shadows reach the boots after the fix |
| `06_feet_diagnosis` | The feet investigation: third person vs first person, blocker distance (`r_shadow_debug 3`), penumbra / cascade views, `cl_fp_body 0`, filter tiers |
| `07_viewmodel_isolation_bias_before` | Viewmodel tile at the bare texel bias (black spot on the sleeve), tile off, shadows off, coverage (`r_shadow_debug 7`), texel size (`6`) |
| `08_viewmodel_isolation_bias_after` | The same with the skin / cloth bias floor (spot gone) |
| `09_viewmodel_self_shadow_sunset` | Sunset at four headings, viewmodel tile on / off (`vm_grid` = the crops side by side) |
| `10_first_run` | The first headless frame of the new build (noon fence) and a crop of the rail's shadow edge |
| `11_validation_run_after`, `12_..._before` | The validation-layer runs (same scenes, then every Engine Fidelity level) |
