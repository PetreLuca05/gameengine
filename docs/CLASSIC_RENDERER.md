# Classic renderer

The renderer of the vgeo scene content (meshes, materials, instances, placement chunks, foliage):
`src/render/vk_classic.c` (shaders `shaders/classic_*`) draws it the traditional way: discrete LOD chains, CPU
culling, instanced draws, impostors. It is the only renderer of that content.

**Nanite removed.** The virtualized geometry renderer (`r_vgeo 1`: GPU cell / instance / BVH / cluster culling,
two-pass Hi-Z, HW + SW visibility-buffer raster, resolve passes, page streaming, vgeo sun shadows, characters
through vgeo) lives on branch `nanite` (2cd1d958, docs/VGEO.md and docs/VGEO_PLAN.md there); the runtime switch
`r_vgeo`, its Settings row "Virtualized geometry (Nanite)" and the Engine Fidelity features `vgeo` /
`vgeo_error_px` are gone, and so are the launchers' nanite / classic question (`dg n` / `dg c` still work and
mean the same as `dg`). What the classic path shares stayed (docs/VGEO.md on this branch): the builder and its
DAG (the LOD chains are cuts of it), `.vgeo` files, the scene tables, foliage support, impostors, trees, grass,
the wind tile and the displacement buffer. Differences from `r_vgeo 0` before the removal:
- the scene tables are CPU only (no GPU context: no page pool, scene buffers or placement region copy on the
  GPU, nothing uploaded at a commit or per frame);
- the device no longer asks for the vgeo features (mesh shaders, 64-bit atomics, scalar layout, descriptor
  indexing); the classic renderer runs without them;
- `lod_active()` (docs/LOD.md) is on unless `r_lod_legacy 1` (before: also off with `r_vgeo 1`, the default);
- `r_era` sets `r_vgeo_lod_px`, `r_vgeo_shadow_error_px` and `r_foliage_density` (no `r_vgeo` / `r_vgeo_error_px`).
The cvars the classic path uses keep their names: `r_vgeo_lod_px`, `r_vgeo_shadow_error_px`,
`r_vgeo_impostor_px`, `r_vgeo_shadows`.

## How it fits in the frame

- **Registration.** Every mesh that enters the vgeo scene (`fallback_register` in `vk_vgeo.c`: level meshes,
  `.vgeo` files, procedural / Tree It foliage, grass assets) goes to `vkc_register_mesh`. Meshes with vertex
  colours (wind weights) or several materials have their DAG cuts decoded and uploaded at registration (the
  transient build is gone later); meshes with one material and no colours keep a CPU copy of the exported LODs
  and upload a LOD on first use. Impostor atlases become two renderer textures at registration. Skinned meshes
  are not registered (characters use the legacy skinned path).
- **Per frame.** `vkg_classic_frame_begin` (render.c, after `compute_matrices`) starts `vkc_frame`'s culling,
  LOD selection and instance batches for the main view, the sun cascades and the scope PiP as a job
  (PF-0041); `vkg_classic_frame_end` joins it before the frame graph and uploads the LODs it asked for. Before the shadow pass
  `vkg_record_foliage` records the foliage inputs: the frame's wind, the displacement buffer, the wind tile
  upload, then the classic wind set (`vkc_wind_frame`).
- **Point / spot light shadows** of the content: `vkg_draw_local_shadow` -> `vkc_draw_local_shadow`
  (instanced, masked materials alpha-tested).

### Memory cost

`classic_stats` prints the classic renderer's own memory (LOD vertex / index buffers + impostor atlases on
the GPU, exported-LOD copies on the CPU).

Measured (`classic_stats`, one session: devroom, `foliage_test_scatter 300000`, `map vegetation`,
`map zombieland`; meshes stay registered across maps like the vgeo mesh table):

| After | Classic GPU | Classic CPU | For reference (the old vgeo page pool) |
|---|---|---|---|
| devroom (pbr_ball: exported LODs, not drawn yet) | 0.0 MB | 0.0 MB | page pool 10 MB |
| + foliage test types (tufts, 4 bushes, daisy) | 20.6 MB | 0.0 MB | |
| + vegetation / zombieland (9 tree species, wheat) | 55.6 MB | 0.0 MB | page pool 15 MB, tree textures 70 MB |

Nearly all of it is the decoded tree LOD chains (cards: the DAG barely simplifies, 4 LODs of 1-18k triangles
at 36 B / vertex) and the 8 x 8 impostor atlases (albedo + normals with mips). Exported-LOD meshes cost nothing
until first drawn (CPU copies only for transient builds; the level / file meshes are persistent). The vgeo page
pool, scene buffers and placement region copy no longer exist (CPU-only scene).

## What the classic renderer does

| Stage | How |
|---|---|
| Meshes | Per vgeo mesh a discrete LOD chain (the 4 exported LOD errors). Meshes with vertex colours (wind weights) or several materials: the DAG cut at each error is decoded (`vgeo_cut` + `vgeo_decode_cluster`, with colours) and split per material; others use the exported LODs. Plain vertex / index buffers. |
| Impostors | The baked hemi-octahedral atlas of a mesh (albedo + normals) as two renderer textures; a camera-facing quad per instance blends the 3 nearest frames (`shaders/classic_impostor.glsl`, the conventions of the atlas bake, `vgeo_impostor.c`). Meshes whose coarsest LOD has >= 256 triangles (trees) switch below `r_classic_impostor_px` (64 px, about one atlas frame); small cover (grass tufts, bushes) below `r_vgeo_impostor_px`. |
| Culling | CPU, per frame: placement chunk bounds, then 8 m cells (frustum, per-cell foliage fade end / shadow distance), then instances (frustum, `vgeo_foliage_alpha` density + fade hash, impostor switch, LOD pick by projected error `r_vgeo_lod_px`). Cells are culled in parallel on the job system. Main view + 3 sun cascades (cascades: LOD by texel size, `r_vgeo_shadow_error_px`, mesh `shadow_max_dist`, no impostors, the caster class's cascade limit: grass in cascade 0, small plants in 0 - 1, docs/SHADOWS.md "Caster cascade limits"; cells / chunks skipped by their class mask). |
| Batching | Instances bucketed per (mesh, LOD) into a per-frame host-visible instance stream (48 B: position + scale, quaternion, wind phase + fade). One `vkCmdDrawIndexed` per bucket and material, instanced; no per-instance draws. |
| Prepass | G-buffer with the shared `gbuffer_body.glsl`. Opaque materials: no discard (early-z). Masked materials: alpha test on the material texture (isotropic mip); dithered fade (`VGM_DITHER_FADE`, same interleaved-gradient threshold rotated per frame); two-sided materials without back-face culling, normal flipped on back faces. |
| Lit | Deferred (PF-0039, "Deferred lighting" below): the prepass of a deferrable material also stores the lit pass's surface terms and one full-screen pass lights them. Other materials and impostors: shared `lit_body.glsl` at depth EQUAL; masked / dithered ones repeat their test (distant cards of neighbouring trees can share a depth value). |
| WPO | The foliage wind of docs/FOLIAGE_WIND.md, `shaders/vgeo/wpo.glsl` (`wpo_veg`) included as is in the vertex shader (`classic_vert.glsl`). Per draw (push constants): the material's `wpo_params` and `wind_params`. Per frame slot (set 4, `ClWindGpu` uniform buffer, filled by `vkc_wind_frame` after the tile upload): the field outside the tile, the tile window, time, this frame's and last frame's (velocity), the `r_foliage_wind` layers / `wind_solo`, the `wind_view` debug view, the displacement buffer window; set 4 binding 1 is the 3-image wind tile ring (`vg_wind_*`). Per instance: pivot, scale, phase (`VG_INST_PHASE / 256`). Bent / fluttering normals (`VGW_NORMALS`) come from the vertex shader; shadows displace the same way. `WPO_DISPLACE` reads the displacement buffer (`VgDisplace`). Impostors have no wind. Culling spheres grow by `max_wpo * max(scale, 1)` (`vg_wpo_margin`). |
| Shadows | Sun cascades and point / spot light tiles: instanced, depth only, masked materials alpha-tested. |
| Characters | The legacy GPU-skinned path. |
| glTF maps | docs/PBR.md: the prepass and lit shaders define `PBR_MAPS` and sample `pbr_maps.glsl` from set 5 (`R->mat_layout`, the legacy lit layout's set 3; `vkr_material_set` per map combination, bound per material draw), with the screen-derivative cotangent frame as on the legacy path; `PushLit.pbr` carries normal scale, occlusion strength, emissive colour. `mat_maps` resolves each map index of the `VgMaterial` (renderer id or wrapped module slot); a map that does not resolve (not ready, bad index) drops its `MAT_*_TEX` flag, so every sampled slot is a valid texture. glTF MASK / doubleSided come in as `VGM_MASKED` / `VGM_TWO_SIDED` (the clip / 2S pipelines). Impostors and shadows ignore the maps. |

Generic content: anything that enters the vgeo scene (procedural foliage kinds, `"vgeo": true` models with a
`"foliage"` block, `.vgeo` level files, imported masked / two-sided meshes) goes through the same path: the
material flags (`VGM_MASKED`, `VGM_TWO_SIDED`, `VGM_DITHER_FADE`, `VGM_FOLIAGE`, WPO) select the pipeline,
renderer texture ids are used directly, vgeo module-owned texture slots (procedural masks, the shared
Tree It leaf / bark textures with their coverage-preserving mips) are wrapped as renderer textures (same
image and sampler, no copy), and the per-mesh impostor switch scale (`VgMesh.impostor_grid >> 16`) applies.

`r_era` sets the LOD pixel errors and foliage densities. `classic_stats` (command category
Rendering) prints memory, meshes, last frame's instances per view, batches, draws, triangles and the CPU
cull time.

### Gaps / differences

- LOD, impostor, foliage density and shadow-distance switches cross-fade over distance bands with
  complementary dithers, at the ranges of `assets/render/lod.json` x the Engine Fidelity tier
  (docs/LOD.md); `r_lod_legacy 1` brings back the cuts. (The nanite cut was continuous.) Trees turn into
  impostors at 192 px x the species scale at Modern: softer distant forests, far fewer triangles.
- The scope picture-in-picture draws the vgeo content through a classic view culled with the PiP projection
  (docs/LOD.md "Scopes").
- The CPU culls every live placement cell that passes the chunk tests each frame (~0.5-2 ms with the
  job system on the test maps). Every step runs on the job system (PF-0040: the views' cell walks, the
  instance cull, the packing and emit in parallel; `classic_cull_check` compares it with the serial path)
  and the whole culling overlaps the rest of the frame's preparation (PF-0041).
- No occlusion culling: the dense devroom foliage yard draws what the frustum keeps. `classic_occlusion_probe`
  measures what one could take (docs/PERF_AUDIT.md "What was measured and left out": 1 - 18 % of the main
  view's triangles in the vegetation map).
- Phone views and the rain occlusion map do not show vgeo content.
- A multi-material model mesh whose page data is not in memory falls back to the exported LODs with its
  first material only (no such content today).

### Deferred lighting (PF-0039)

The lit pass drew the main view's geometry a second time (the full wind deformation again) to shade about one
fragment per pixel: in the forests that pass is geometry bound (broadleaf at Low: 5.0 ms for 634 k fragments).
Classic content is now lit from its prepass instead:

- **Deferrable** (`mat_deferrable`): every material but those with something the lit body adds after the
  lighting (emissive or an emissive map, highlight, glass, caustics, the facade window grid). Those keep the
  lit pass, as do impostors.
- **Prepass** (`classic_gbuffer_def(_clip).frag`): the shared G-buffer, then the lit pass's surface stage
  (`lit_surface.glsl`, the code `lit_body.glsl` includes), material and weather terms of the fragment
  (`classic_deferred_gbuffer.glsl`) into two more targets (`shaders/classic_deferred.glsl`): RGBA16F albedo +
  roughness after the weather, RGBA16UI baked AO (vertex alpha x occlusion map x ground cover), snow cover,
  ground cover thickness and the flags + AO byte. Every prepass pipeline binds the two targets (the others do
  not write them; null views while the frame has no deferred content).
- **Lighting** (`classic_deferred.frag`, graph pass `classic_deferred` before `opaque`): a full-screen
  triangle; pixels whose flags say a deferred material kept them run `lit_body.glsl`'s lighting on the stored
  terms (`LIT_DEFERRED_MATERIAL`), the world position from the depth and the inverse of the prepass's jittered
  view-projection; ground cover facing down is turned up and gets its puddle normal, as the lit pass does
  (the G-buffer keeps the card's normal for the screen-space passes). Other pixels are discarded. The
  opaque pass then loads HDR (no clear) and draws everything else, also over any pixel a later prepass draw
  took from classic content (its flags are stale there; the forward draw wins).
- **Precision**: albedo and roughness in half floats, metallic and normal in the G-buffer's formats, the
  position from the 32 bit depth. Dev room A / B (lavapipe, `perf_fix_disable PF-0039`): PSNR 48 - 50 dB,
  scattered card edge pixels.
- `perf_fix_disable PF-0039` restores the lit pass for every classic material.

### Depth-only prepass for masked parts (PF-0052, `r_classic_zprepass`, off by default)

With the deferred lighting, a masked part's G-buffer fragment shader (the alpha test, then the surface,
material and weather terms into five targets) runs for every leaf card that covers a pixel and passes the
test: late depth testing, about 12 per pixel in the forests. `r_classic_zprepass 1` splits the deferred
masked groups (one- and two-sided) in two: first a depth-only pass with only their alpha / dither test
(`classic_depth_clip.frag`, the same texture and sampler as `classic_gbuffer_def_clip.frag`, no target
written), then their G-buffer (`classic_gbuffer_def_clip.frag`) at EQUAL depth with no depth writes: the
depth test runs early, about one G-buffer shading per pixel. The alpha test runs again in that pass:
distant cards of neighbouring trees can share a depth value, and a transparent texel of one would
otherwise take the pixel (without it, 0.8 % of a pine forest frame's pixels changed). With it the frame is
bit-identical to the zprepass-off one. On the Linux container (lavapipe, pine forest at Low) it is slower:
the prepass pass +19 %, fragment invocations 1.13 M -> 1.58 M, vertex invocations doubled: at ~1.8
G-buffer fragments per pixel there is little overdraw to save. The geometry is drawn twice (the vertex stage, its wind, again).
The opaque and lit-pass groups are unchanged, and so is the image (the same coverage; coplanar cards keep
the draw order's winner as before). Which is faster depends on the GPU (vertex vs fragment / bandwidth
bound): an A/B for the target machine, e.g. `perf_bench levels=current` with `set r_classic_zprepass 0` and
`1`. `perf_fix_disable PF-0052` also turns it off.

### Alpha masks (PF-0014)

The alpha tests of masked materials in the sun cascades, the local light shadow tiles and the prepass only
read alpha, so they bind an R8 copy of the texture's alpha instead of the RGBA albedo
(`src/render/vk_alpha_mask.c`, `shaders/alpha_mask.comp`): every resident level copied texel for texel on the
GPU at the start of the frame after a draw first asks for it (until then the albedo is bound), rebuilt when
the texture's image changes (`GpuTexture.serial`) or its sampler does. The sampled view swizzles R into A,
so `cl_mask_alpha` is unchanged. The lit pass and the scope PiP keep the albedo. Block-compressed
sources (BC7, PF-0035, the default) get no mask: they already read 1 byte per texel; neither do MoltenVK
devices without view swizzles. Bit-identical results
(perf_verify: images and state pass); in the forest interiors the shadow pass changed by only -1.7 %
(not significant): it is bound by fragment / raster work, not texture bandwidth.

### Dense forest cost (after the realistic tree heights)

`tree_scale.cfg`'s zombieland forest stop (`setpos -100 120 600 0 6`, third person, 1600 x 900, uncapped,
RTX 3060 Ti): frame 10.7-11.5 ms (87-93 fps), GPU 10.3-11.0 ms, 10.6M triangles of which the classic path
draws 7.55M (3.9M of them in the sun cascades, 3017 main-view instances). Knobs tried at that stop (GPU
avg): baseline 10.34 ms; `r_classic_impostor_px 128` 9.50 ms (softer trees from ~100 m); `r_vgeo_lod_px 4`
9.55 ms (-0.1M tris: the tree LOD chains barely reduce, e.g. beech 10.6k -> 9.8k triangles at the coarsest
cut, the DAG of cutout cards cannot simplify); `r_vgeo_shadow_error_px 4` 10.02 ms; no sun shadows 7.05 ms.

## How to compare

```
# from build/bin, one run per build (or change)
./deadground.exe --test --nosound --log perf.log --script ../../tools/tests/perf_compare.cfg
grep perf_stop perf.log
```

`tools/tests/perf_compare.cfg` visits fixed camera stops on `map vegetation` and `map devroom` (hall, a
200-zombie horde, the 300k-instance foliage yard from `foliage_test_scatter`), uncapped (`r_maxfps 0`,
`r_vsync 0`), 120 settle frames, then `perf_stop 300 <label>`: average / worst CPU frame time (`frame_dt`),
average / worst GPU frame time (command buffer timestamps), draws and triangles per frame.

## The nanite removal

RTX 3060 Ti, 1600 x 900 window, defaults, `perf_compare.cfg`: 2cd1d958 (branch `nanite`) with `+set r_vgeo 0`
against this branch, one run each. Frame = CPU frame time, GPU = the whole command buffer.

| Stop | frame avg ms (before / after) | GPU avg ms (before / after) | draws (before / after) | tris M |
|---|---|---|---|---|
| veg_meadow | 7.74 / 7.69 | 7.67 / 7.64 | 923 / 923 | 6.35 |
| veg_pine_forest | 14.58 / 14.60 | 14.57 / 14.58 | 1039 / 1039 | 11.99 |
| veg_broadleaf | 19.82 / 19.92 | 19.81 / 19.91 | 1103 / 1103 | 15.04 |
| veg_wheat | 7.66 / 7.86 | 7.63 / 7.73 | 712 / 712 | 3.10 |
| veg_aerial | 6.94 / 7.01 | 6.93 / 6.99 | 596 / 594 | 4.66 |
| dev_hall | 3.98 / 3.71 | 3.16 / 3.20 | 1688 / 1688 | 1.15 |
| dev_horde_200 | 5.57 / 5.50 | 2.67 / 2.65 | 1107 / 1107 | 0.84 |
| dev_foliage_yard | 13.50 / 13.68 | 13.49 / 13.67 | 608 / 608 | 27.59 |
| dev_foliage_air | 15.27 / 15.34 | 15.26 / 15.33 | 293 / 293 | 31.88 |

The same draws and triangles (veg_aerial: 2 draws fewer), frame and GPU times within run-to-run noise
(+-0.2 ms). Screenshots (vegetation close / wheat / forest / far, terrain_yard, dev hall and PBR shelf):
before vs after differ by 0.4-3.6 / 255 mean, less than two runs of the same build (1.2-7.4: wind,
wandering zombies, TAA).

## Results of the merge (historical: both renderers in one exe)

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
