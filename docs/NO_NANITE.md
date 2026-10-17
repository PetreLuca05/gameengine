# no-nanite: the same game without virtualized geometry

Two branches compare renderers on identical content:

- **`nanite`**: the vgeo virtualized-geometry renderer (docs/VGEO.md): GPU cell / instance / BVH / cluster
  culling, two-pass Hi-Z occlusion, HW + SW visibility-buffer raster, resolve passes, page streaming,
  characters through vgeo.
- **`no-nanite`** (this branch): the same meshes, materials, placements, foliage and characters drawn by a
  traditional renderer, `src/render/vk_classic.c`. `r_vgeo` is forced to 0 (log: `vgeo: no-nanite build`)
  and no vgeo culling / raster / resolve / Hi-Z / streaming pass runs.

## What the classic renderer does

| Stage | How |
|---|---|
| Meshes | Per vgeo mesh a discrete LOD chain (the 4 exported LOD errors). Meshes with vertex colours (wind weights) or several materials: the DAG cut at each error is decoded (`vgeo_cut` + `vgeo_decode_cluster`, now with colours) and split per material; others use the exported LODs. Plain vertex / index buffers. |
| Impostors | The baked hemi-octahedral atlas of a mesh (albedo + normals) as two renderer textures; a camera-facing quad per instance blends the 3 nearest frames (`shaders/classic_impostor.glsl`, same conventions as `vg_impostor.glsl`). Meshes whose coarsest LOD has >= 256 triangles (trees) switch below `r_classic_impostor_px` (64 px, about one atlas frame); small cover (grass tufts, bushes) where vgeo does (`r_vgeo_impostor_px`). |
| Culling | CPU, per frame: placement chunk bounds, then 8 m cells (frustum, per-cell foliage fade end / shadow distance), then instances (frustum, `vgeo_foliage_alpha` density + fade hash, impostor switch, LOD pick by projected error `r_vgeo_lod_px`). Cells are culled in parallel on the job system. Main view + 3 sun cascades (cascades: LOD by texel size, `r_vgeo_shadow_error_px`, mesh `shadow_max_dist`, no impostors). |
| Batching | Instances bucketed per (mesh, LOD) into a per-frame host-visible instance stream (48 B: position + scale, quaternion, wind phase + fade). One `vkCmdDrawIndexed` per bucket and material, instanced; no per-instance draws. |
| Prepass | G-buffer with the shared `gbuffer_body.glsl`. Opaque materials: no discard (early-z). Masked materials: alpha test on the material texture (isotropic mip, like the vgeo per-triangle LOD); dithered fade (`VGM_DITHER_FADE`, same interleaved-gradient threshold rotated per frame); two-sided materials without back-face culling, normal flipped on back faces. |
| Lit | Shared `lit_body.glsl` at depth EQUAL. Masked / dithered materials and impostors repeat their test (distant cards of neighbouring trees can share a depth value). |
| WPO | The foliage wind of docs/FOLIAGE_WIND.md, the same model and inputs as vgeo: `shaders/vgeo/wpo.glsl` (`wpo_veg`) included as is in the vertex shader (`classic_vert.glsl`). Per draw (push constants): the material's `wpo_params` and `wind_params`. Per frame slot (set 4, `ClWindGpu` uniform buffer, filled by `vkc_wind_frame` after the tile upload): the field outside the tile, the tile window, time, this frame's and last frame's (velocity), the `r_foliage_wind` layers / `wind_solo`, the `wind_view` debug view, the displacement buffer window; set 4 binding 1 is the 3-image wind tile ring (`vg_wind_*`, written once per slot). Per instance: pivot, scale, phase (`VG_INST_PHASE / 256`). Bent / fluttering normals (`VGW_NORMALS`) come from the vertex shader (vgeo: the resolve); shadows displace the same way. `WPO_DISPLACE` reads the vgeo displacement buffer. The wind tile upload and the displacement compute are the only vgeo-module GPU work per frame. Impostors have no wind (as on vgeo). Culling spheres grow by `max_wpo * max(scale, 1)` (`vg_wpo_margin`). |
| Shadows | Sun cascades and point / spot light tiles: instanced, depth only, masked materials alpha-tested (vgeo's local light shadows skipped masked meshes). |
| Characters | The legacy GPU-skinned path (`vkg_route_skinned` routes nothing with `r_vgeo 0`). |

Generic content: anything that enters the vgeo scene (procedural foliage kinds, `"vgeo": true` models with a
`"foliage"` block, `.vgeo` level files, imported masked / two-sided meshes) goes through the same path: the
material flags (`VGM_MASKED`, `VGM_TWO_SIDED`, `VGM_DITHER_FADE`, `VGM_FOLIAGE`, WPO) select the pipeline,
renderer texture ids are used directly, vgeo module-owned texture slots (procedural masks, the shared
Tree It leaf / bark textures with their coverage-preserving mips) are wrapped as renderer textures (same
image and sampler, no copy), and the per-mesh impostor switch scale (`VgMesh.impostor_grid >> 16`) applies.

`r_era` still works: the eras keep their LOD pixel errors and foliage densities, vgeo is off in all of them.
`classic_stats` prints meshes, last frame's instances per view, batches, draws, triangles and the CPU cull time.

### What stays from vgeo

CPU data (scene tables, placement region, materials) and their load-time uploads, the culling / raster
pipelines and the page pool are still created at init (removing the module is a larger change); none of
it is used per frame.

### Gaps / differences

- LOD and impostor switches pop (no cross-fade); vgeo's cut is continuous. Trees turn into impostors much
  earlier (64 px instead of 16 px): softer distant forests, far fewer triangles.
- The CPU culls every live placement cell that passes the chunk tests each frame (~0.5-2 ms with the
  job system on the test maps); vgeo culls on the GPU.
- No occlusion culling (vgeo has two-pass Hi-Z): the dense devroom foliage yard draws what the frustum keeps.
- Scope picture-in-picture, phone views and the rain occlusion map do not show vgeo content (as on `nanite`).
- A multi-material model mesh whose page data is not in memory falls back to the exported LODs with its
  first material only (no such content today).

### Dense forest cost (after the realistic tree heights)

`tree_scale.cfg`'s zombieland forest stop (`setpos -100 120 600 0 6`, third person, 1600 x 900, uncapped,
RTX 3060 Ti; after the wind port): frame 10.7-11.5 ms (87-93 fps), GPU 10.3-11.0 ms, 10.6M triangles of
which the classic path draws 7.55M (3.9M of them in the sun cascades, 3017 main-view instances). The ~14 fps
seen once before the port did not reproduce. Knobs tried at that stop (GPU avg): baseline 10.34 ms;
`r_classic_impostor_px 128` 9.50 ms (softer trees from ~100 m); `r_vgeo_lod_px 4` 9.55 ms (-0.1M tris:
the tree LOD chains barely reduce, e.g. beech 10.6k -> 9.8k triangles at the coarsest cut, the DAG of
cutout cards cannot simplify); `r_vgeo_shadow_error_px 4` 10.02 ms; no sun shadows 7.05 ms. The LOD pick
and the impostor switch already use the instance scale; defaults unchanged. `classic_stats` now also
prints the triangles of the shadow views.

## How to compare

```
# on each branch: build, then from build/bin
./deadground.exe --test --nosound --log perf.log --script ../../tools/tests/perf_compare.cfg
grep perf_stop perf.log
```

`tools/tests/perf_compare.cfg` visits fixed camera stops on `map vegetation`, `map zombieland` and `map devroom`
(hall, a 200-zombie horde, the 300k-instance foliage yard from `foliage_test_scatter`), uncapped (`r_maxfps 0`,
`r_vsync 0`), 120 settle frames, then `perf_stop 300 <label>`: average / worst CPU frame time (`frame_dt`),
average / worst GPU frame time (command buffer timestamps), draws and triangles per frame. `perf_stop`
(`src/game/game_post.c`) is in its own commit so it can be cherry-picked onto `nanite`. Close other GPU
work while measuring. The numbers below are four alternating runs per branch; the machine was shared with
other GPU jobs, so the table takes each stop's least contended run.

## Results

RTX 3060 Ti, 1600 x 900 window, defaults (TAA, SSR, GTAO, volumetrics), `nanite` = 76aa780 + the `perf_stop`
commit, `no-nanite` = this branch. Frame = CPU frame time (`frame_dt`, the frame rate), GPU = the whole command
buffer. Worst frames are noisy (other agents' GPU tests ran on the same machine).

| Stop | nanite frame avg / worst (ms) | no-nanite frame avg / worst (ms) | nanite GPU avg / worst (ms) | no-nanite GPU avg / worst (ms) | draws nanite / no-nanite | triangles nanite / no-nanite |
|---|---|---|---|---|---|---|
| veg_meadow | 4.50 / 6.4 | 2.86 / 5.4 | 4.49 / 6.3 | 2.85 / 3.3 | 758 / 733 | 0.48M / 0.84M |
| veg_pine_forest | 4.74 / 13.3 | 3.09 / 4.0 | 4.71 / 5.9 | 3.07 / 3.6 | 946 / 922 | 0.48M / 0.79M |
| veg_broadleaf | 4.72 / 6.7 | 3.17 / 8.0 | 4.71 / 5.7 | 3.15 / 3.9 | 946 / 929 | 0.48M / 0.87M |
| veg_wheat | 4.86 / 7.7 | 3.36 / 10.4 | 4.85 / 5.6 | 3.33 / 4.0 | 844 / 863 | 0.43M / 1.83M |
| veg_aerial | 3.69 / 4.7 | 2.48 / 8.7 | 3.68 / 4.3 | 2.45 / 2.8 | 1061 / 988 | 0.43M / 0.46M |
| zl_city_street | 4.45 / 6.3 | 4.03 / 7.7 | 4.41 / 5.1 | 3.24 / 4.0 | 12676 / 12256 | 3.02M / 2.76M |
| zl_suburbs_grass | 7.79 / 12.0 | 4.60 / 7.9 | 4.66 / 5.3 | 2.89 / 3.4 | 14917 / 14609 | 3.44M / 3.16M |
| zl_forest | 6.66 / 10.0 | 4.41 / 6.3 | 5.19 / 6.1 | 3.84 / 4.8 | 9895 / 10030 | 2.19M / 2.80M |
| zl_overview | 10.28 / 18.9 | 5.46 / 7.9 | 4.76 / 5.8 | 2.98 / 3.8 | 15291 / 14927 | 2.89M / 2.46M |
| dev_hall | 5.34 / 8.4 | 2.88 / 4.8 | 3.05 / 3.7 | 2.87 / 3.4 | 893 / 939 | 0.30M / 0.50M |
| dev_horde_200 | 4.54 / 7.0 | 3.24 / 5.5 | 3.05 / 3.6 | 2.30 / 3.0 | 676 / 690 | 0.21M / 0.24M |
| dev_foliage_yard | 5.61 / 7.5 | 3.78 / 7.4 | 5.60 / 6.3 | 3.68 / 4.3 | 653 / 690 | 0.10M / 2.06M |
| dev_foliage_air | 5.55 / 6.8 | 3.87 / 8.1 | 5.54 / 6.4 | 3.83 / 4.4 | 659 / 680 | 0.10M / 1.72M |

Reading it:
- **GPU:** the classic path is 1.3-2 ms cheaper per frame on every foliage stop (vegetation, zombieland
  grass / forest / overview, the 300k-instance yard), even though it draws 1.5-20x the triangles: on this
  content (small foliage meshes, 60-1360 triangles, few large occluders) vgeo's fixed cost (two-pass cull,
  Hi-Z builds, visibility buffer clear + HW / SW raster, full-screen G-buffer and lit resolves, cascade culls)
  outweighs what its fine-grained LOD and occlusion save. The hall (no vgeo content) costs the same on both.
- **CPU:** `nanite` frames are CPU-bound on zombieland (overview 10.3 ms vs 4.8 ms GPU) and in the devroom;
  the classic path's CPU cull (`classic_stats`: 0.5-2 ms, job system) keeps its frames close to the GPU time.
- Triangles: the classic LOD chain draws far more (no per-cluster cut, coarse LODs only every 4x error);
  impostors replace trees beyond 64 px.
- Visuals: screenshots of the same stops (vegetation spawn / pine / broadleaf / dead forest / wheat / aerial,
  zombieland city / suburbs / forest / overview, devroom hall / foliage yard / yard at night with the
  flashlight) match closely; distant impostor forests are slightly softer, and the classic path additionally
  shows masked foliage in flashlight shadows.
