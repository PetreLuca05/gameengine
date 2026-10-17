# Classic renderer (`r_vgeo 0`)

One exe, two renderers for the vgeo scene content (meshes, materials, instances, placement chunks, foliage):

- **`r_vgeo 1`**: virtualized geometry (docs/VGEO.md): GPU cell / instance / BVH / cluster culling, two-pass
  Hi-Z occlusion, HW + SW visibility-buffer raster, resolve passes, page streaming, characters through vgeo.
- **`r_vgeo 0`**: the same content drawn the traditional way by `src/render/vk_classic.c` (shaders
  `shaders/classic_*`): discrete LOD chains, CPU culling, instanced draws, impostors.

The 2015 / 2010 eras (`r_era`) use the classic renderer; Modern / 2020 use vgeo. `r_vgeo` switches at any
frame: console (`set r_vgeo 0`), Settings > Graphics > Geometry "Virtualized geometry (Nanite)" (locked in
the eras that lack vgeo), Engine Fidelity (`vgeo` feature, available at every level), the era presets.
Switching by hand makes `r_era` read back as Custom, as any other era-owned value does. The classic renderer
replaced the old discrete-LOD fallback of `vk_vgeo.c` (which skipped masked foliage). It started as the
`no-nanite` comparison branch, where vgeo was forced off.

## How the two paths coexist

- **Registration.** Every mesh that enters the vgeo scene (`fallback_register` in `vk_vgeo.c`: level meshes,
  `.vgeo` files, procedural / Tree It foliage, test content) also goes to `vkc_register_mesh`, whatever
  `r_vgeo` is, so a switch never waits on a rebuild. Meshes with vertex colours (wind weights) or several
  materials have their DAG cuts decoded and uploaded at registration (the transient build is gone later);
  meshes with one material and no colours keep a CPU copy of the exported LODs and upload a LOD on first use.
  Impostor atlases become two renderer textures at registration. Skinned meshes are not registered
  (characters use the legacy skinned path with `r_vgeo 0`: `vkg_route_skinned` routes nothing).
- **Per frame.** `vkg_submit_fallback` (render.c, after the camera) calls `vkc_frame` on both paths:
  `r_vgeo 0` culls and batches the main view and the sun cascades; `r_vgeo 1` only resets the batches (the
  classic prepass / lit / cascade calls in render.c then draw nothing) and hands over the scene for the local
  light tiles. Before the shadow pass `vkg_record_shadow_cull` runs, with `r_vgeo 0`, the vgeo work the
  classic path still needs: the wind of the frame, the wind tile upload, the displacement buffer, live wind
  materials and the placement region upload (so the vgeo GPU copy of the placements is current when
  `r_vgeo` goes back to 1). With `r_vgeo 1` the vgeo frame runs as before. Both then write the classic wind
  set (`vkc_wind_frame`).
- **Point / spot light shadows** of vgeo content are drawn by the classic renderer on both paths
  (`vkg_draw_local_shadow` -> `vkc_draw_local_shadow`): instanced, masked materials alpha-tested. (Before,
  `r_vgeo 1` drew them as discrete LODs without alpha test and `r_vgeo 0` drew none.)
- **What stays allocated** with `r_vgeo 0`: the whole vgeo module (scene tables, page pool, culling /
  raster pipelines, visibility buffer), as before the merge. With `r_vgeo 1`: the classic LOD buffers,
  impostor textures and pipelines (below). Nothing is created or freed by a switch, so switching cannot
  leak; the instance stream is per frame slot and grows only.

### Memory cost

`classic_stats` prints the classic renderer's own memory (LOD vertex / index buffers + impostor atlases on
the GPU, exported-LOD copies on the CPU); it is held on both paths.

Measured (`classic_stats`, one session: devroom, `foliage_test_scatter 300000`, `map vegetation`,
`map zombieland`; meshes stay registered across maps like the vgeo mesh table):

| After | Classic GPU | Classic CPU | For reference (vgeo) |
|---|---|---|---|
| devroom (pbr_ball: exported LODs, not drawn yet) | 0.0 MB | 0.0 MB | page pool 10 MB |
| + foliage test types (tufts, 4 bushes, daisy) | 20.6 MB | 0.0 MB | |
| + vegetation / zombieland (9 tree species, wheat) | 55.6 MB | 0.0 MB | page pool 15 MB, tree textures 70 MB |

Nearly all of it is the decoded tree LOD chains (cards: the DAG barely simplifies, 4 LODs of 1-18k triangles
at 36 B / vertex) and the 8 x 8 impostor atlases (albedo + normals with mips). Exported-LOD meshes cost nothing
until first drawn (CPU copies only for transient builds; the level / file meshes are persistent). The cost
is paid with `r_vgeo 1` too: the price of switching without a rebuild.

## What the classic renderer does

| Stage | How |
|---|---|
| Meshes | Per vgeo mesh a discrete LOD chain (the 4 exported LOD errors). Meshes with vertex colours (wind weights) or several materials: the DAG cut at each error is decoded (`vgeo_cut` + `vgeo_decode_cluster`, with colours) and split per material; others use the exported LODs. Plain vertex / index buffers. |
| Impostors | The baked hemi-octahedral atlas of a mesh (albedo + normals) as two renderer textures; a camera-facing quad per instance blends the 3 nearest frames (`shaders/classic_impostor.glsl`, same conventions as `vg_impostor.glsl`). Meshes whose coarsest LOD has >= 256 triangles (trees) switch below `r_classic_impostor_px` (64 px, about one atlas frame); small cover (grass tufts, bushes) where vgeo does (`r_vgeo_impostor_px`). |
| Culling | CPU, per frame: placement chunk bounds, then 8 m cells (frustum, per-cell foliage fade end / shadow distance), then instances (frustum, `vgeo_foliage_alpha` density + fade hash, impostor switch, LOD pick by projected error `r_vgeo_lod_px`). Cells are culled in parallel on the job system. Main view + 3 sun cascades (cascades: LOD by texel size, `r_vgeo_shadow_error_px`, mesh `shadow_max_dist`, no impostors). |
| Batching | Instances bucketed per (mesh, LOD) into a per-frame host-visible instance stream (48 B: position + scale, quaternion, wind phase + fade). One `vkCmdDrawIndexed` per bucket and material, instanced; no per-instance draws. |
| Prepass | G-buffer with the shared `gbuffer_body.glsl`. Opaque materials: no discard (early-z). Masked materials: alpha test on the material texture (isotropic mip, like the vgeo per-triangle LOD); dithered fade (`VGM_DITHER_FADE`, same interleaved-gradient threshold rotated per frame); two-sided materials without back-face culling, normal flipped on back faces. |
| Lit | Shared `lit_body.glsl` at depth EQUAL. Masked / dithered materials and impostors repeat their test (distant cards of neighbouring trees can share a depth value). |
| WPO | The foliage wind of docs/FOLIAGE_WIND.md, the same model and inputs as vgeo: `shaders/vgeo/wpo.glsl` (`wpo_veg`) included as is in the vertex shader (`classic_vert.glsl`). Per draw (push constants): the material's `wpo_params` and `wind_params`. Per frame slot (set 4, `ClWindGpu` uniform buffer, filled by `vkc_wind_frame` after the tile upload): the field outside the tile, the tile window, time, this frame's and last frame's (velocity), the `r_foliage_wind` layers / `wind_solo`, the `wind_view` debug view, the displacement buffer window; set 4 binding 1 is the 3-image wind tile ring (`vg_wind_*`). Per instance: pivot, scale, phase (`VG_INST_PHASE / 256`). Bent / fluttering normals (`VGW_NORMALS`) come from the vertex shader (vgeo: the resolve); shadows displace the same way. `WPO_DISPLACE` reads the vgeo displacement buffer. Impostors have no wind (as on vgeo). Culling spheres grow by `max_wpo * max(scale, 1)` (`vg_wpo_margin`). |
| Shadows | Sun cascades and point / spot light tiles: instanced, depth only, masked materials alpha-tested. |
| Characters | The legacy GPU-skinned path. |
| glTF maps | docs/PBR.md: the prepass and lit shaders define `PBR_MAPS` and sample `pbr_maps.glsl` from set 5 (`R->mat_layout`, the legacy lit layout's set 3; `vkr_material_set` per map combination, bound per material draw), with the screen-derivative cotangent frame as on the legacy path; `PushLit.pbr` carries normal scale, occlusion strength, emissive colour. `mat_maps` resolves each map index of the `VgMaterial` (renderer id or wrapped module slot); a map that does not resolve (not ready, bad index) drops its `MAT_*_TEX` flag, so every sampled slot is a valid texture. glTF MASK / doubleSided come in as `VGM_MASKED` / `VGM_TWO_SIDED` (the clip / 2S pipelines). Impostors and shadows ignore the maps. |

Generic content: anything that enters the vgeo scene (procedural foliage kinds, `"vgeo": true` models with a
`"foliage"` block, `.vgeo` level files, imported masked / two-sided meshes) goes through the same path: the
material flags (`VGM_MASKED`, `VGM_TWO_SIDED`, `VGM_DITHER_FADE`, `VGM_FOLIAGE`, WPO) select the pipeline,
renderer texture ids are used directly, vgeo module-owned texture slots (procedural masks, the shared
Tree It leaf / bark textures with their coverage-preserving mips) are wrapped as renderer textures (same
image and sampler, no copy), and the per-mesh impostor switch scale (`VgMesh.impostor_grid >> 16`) applies.

`r_era` keeps its LOD pixel errors and foliage densities on both paths. `classic_stats` (command category
Rendering) prints memory, meshes, last frame's instances per view, batches, draws, triangles and the CPU
cull time.

### Gaps / differences

- LOD and impostor switches pop (no cross-fade); vgeo's cut is continuous. Trees turn into impostors much
  earlier (64 px instead of 16 px): softer distant forests, far fewer triangles.
- The CPU culls every live placement cell that passes the chunk tests each frame (~0.5-2 ms with the
  job system on the test maps); vgeo culls on the GPU.
- No occlusion culling (vgeo has two-pass Hi-Z): the dense devroom foliage yard draws what the frustum keeps.
- Scope picture-in-picture, phone views and the rain occlusion map do not show vgeo content (as with vgeo).
- A multi-material model mesh whose page data is not in memory falls back to the exported LODs with its
  first material only (no such content today).

### Dense forest cost (after the realistic tree heights)

`tree_scale.cfg`'s zombieland forest stop (`setpos -100 120 600 0 6`, third person, 1600 x 900, uncapped,
RTX 3060 Ti): frame 10.7-11.5 ms (87-93 fps), GPU 10.3-11.0 ms, 10.6M triangles of which the classic path
draws 7.55M (3.9M of them in the sun cascades, 3017 main-view instances). Knobs tried at that stop (GPU
avg): baseline 10.34 ms; `r_classic_impostor_px 128` 9.50 ms (softer trees from ~100 m); `r_vgeo_lod_px 4`
9.55 ms (-0.1M tris: the tree LOD chains barely reduce, e.g. beech 10.6k -> 9.8k triangles at the coarsest
cut, the DAG of cutout cards cannot simplify); `r_vgeo_shadow_error_px 4` 10.02 ms; no sun shadows 7.05 ms.

## How to compare

```
# from build/bin, one run per path
./deadground.exe --test --nosound --log perf_vgeo.log --script ../../tools/tests/perf_compare.cfg
./deadground.exe --test --nosound --log perf_classic.log +set r_vgeo 0 --script ../../tools/tests/perf_compare.cfg
grep perf_stop perf_*.log
```

`tools/tests/perf_compare.cfg` visits fixed camera stops on `map vegetation`, `map zombieland` and `map devroom`
(hall, a 200-zombie horde, the 300k-instance foliage yard from `foliage_test_scatter`), uncapped (`r_maxfps 0`,
`r_vsync 0`), 120 settle frames, then `perf_stop 300 <label>`: average / worst CPU frame time (`frame_dt`),
average / worst GPU frame time (command buffer timestamps), draws and triangles per frame.

## Results

RTX 3060 Ti, 1600 x 900 window, defaults (TAA, SSR, GTAO, volumetrics), two alternating runs per
configuration, mean of the two: the one exe (`r_vgeo 1` / `r_vgeo 0`) against the two former builds (`main`
1b352c4 with the old fallback, `no-nanite` 086540c with vgeo forced off). Frame = CPU frame time
(`frame_dt`), GPU = the whole command buffer.

| Stop | frame avg ms: one exe r_vgeo 1 / old main / one exe r_vgeo 0 / old no-nanite | GPU avg ms: same order | draws (same order) | tris M (same order) |
|---|---|---|---|---|
| veg_meadow | 7.89 / 7.82 / 3.56 / 3.88 | 7.87 / 7.78 / 3.52 / 3.25 | 758 / 758 / 803 / 803 | 0.48 / 0.48 / 1.55 / 1.55 |
| veg_pine_forest | 8.49 / 8.44 / 4.54 / 4.63 | 8.48 / 8.43 / 4.49 / 4.43 | 790 / 792 / 1077 / 1077 | 0.49 / 0.49 / 4.47 / 4.47 |
| veg_broadleaf | 8.98 / 9.02 / 4.91 / 5.02 | 8.96 / 9.00 / 4.90 / 4.94 | 778 / 779 / 1039 / 1037 | 0.49 / 0.49 / 5.02 / 5.02 |
| veg_wheat | 6.17 / 6.11 / 3.87 / 4.05 | 6.14 / 6.10 / 3.84 / 3.60 | 788 / 786 / 895 / 896 | 0.43 / 0.43 / 2.02 / 2.02 |
| veg_aerial | 4.71 / 4.72 / 2.93 / 3.54 | 4.66 / 4.69 / 2.88 / 2.61 | 1059 / 1049 / 907 / 907 | 0.43 / 0.43 / 0.61 / 0.61 |
| zl_city_street | 6.72 / 5.03 / 4.34 / 4.56 | 4.43 / 4.57 / 3.13 / 3.11 | 12612 / 12713 / 12313 / 12311 | 3.04 / 3.03 / 2.76 / 2.76 |
| zl_suburbs_grass | 7.99 / 10.53 / 5.54 / 5.87 | 5.88 / 5.87 / 3.19 / 3.19 | 14967 / 14808 / 14558 / 14510 | 3.45 / 3.45 / 3.26 / 3.26 |
| zl_forest | 11.04 / 11.27 / 5.90 / 6.05 | 11.02 / 10.95 / 5.80 / 5.80 | 9797 / 9788 / 10099 / 10091 | 2.19 / 2.19 / 9.80 / 9.80 |
| zl_overview | 7.76 / 12.33 / 5.67 / 5.87 | 4.97 / 4.93 / 2.96 / 2.97 | 15647 / 15245 / 14968 / 14941 | 2.89 / 2.90 / 2.47 / 2.47 |
| dev_hall | 4.84 / 7.25 / 2.81 / 2.91 | 3.44 / 3.26 / 2.49 / 2.48 | 1047 / 1050 / 1116 / 1116 | 0.49 / 0.49 / 0.69 / 0.69 |
| dev_horde_200 | 7.96 / 6.42 / 4.63 / 4.31 | 3.32 / 3.14 / 2.06 / 2.06 | 795 / 793 / 798 / 799 | 0.37 / 0.35 / 0.34 / 0.34 |
| dev_foliage_yard | 12.04 / 11.91 / 5.41 / 5.27 | 12.03 / 11.90 / 4.74 / 4.77 | 756 / 756 / 860 / 860 | 0.20 / 0.20 / 6.01 / 6.01 |
| dev_foliage_air | 10.66 / 10.35 / 5.26 / 5.27 | 10.65 / 10.33 / 5.20 / 5.25 | 762 / 762 / 834 / 834 | 0.20 / 0.20 / 7.69 / 7.69 |

Reading it:
- **The one exe matches both old builds**: GPU time within run-to-run noise (+-0.3 ms; `no-nanite`'s second
  run had a few lower GPU / higher CPU vegetation stops, its first matched `r_vgeo 0` to 0.05 ms), identical
  draws and triangles per path. CPU frame times on the CPU-bound vgeo stops (zombieland, devroom) vary run to
  run on both builds.
- **Classic vs vgeo**: on this content the classic path is 0.9-7.3 ms cheaper on the GPU at every stop, even
  drawing up to 38x the triangles (small foliage meshes, few large occluders: vgeo's fixed cost of two-pass
  cull, Hi-Z, visibility buffer raster and full-screen resolves outweighs what its LOD and occlusion save).
- Visuals (`tools/tests/tex_veg.cfg` / `tree_scale.cfg` / `pbr_shelf.cfg` stops): `r_vgeo 0` and the
  `no-nanite` build differ by 0.2-1.5 / 255 mean (wind phase, TAA), 3.7 in the zombieland forest and 12 in
  the wheat field (thin stalks swaying), with identical draw / triangle counts.
