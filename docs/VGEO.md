# Virtualized geometry (vgeo)

Cluster-based virtualized geometry for static props, terrain decoration, and (later) skinned characters and
foliage. `docs/VGEO_PLAN.md` is the full design and phase plan. This file documents what is **built**, and is
updated phase by phase.

**Prior art.** The design follows Nanite (Brian Karis, Rune Stubbe, Graham Wihlidal, *"A Deep Dive into Nanite
Virtualized Geometry"*, SIGGRAPH 2021 Advances in Real-Time Rendering in Games). It uses the same ideas:
- triangle clusters of ≤ 128 triangles;
- a DAG of cluster groups simplified with locked group boundaries;
- view-dependent cuts selected independently per cluster from the pair (own error, parent error);
- a BVH over groups;
- a 64-bit visibility buffer;
- page streaming.

Simplification and clustering use meshoptimizer v0.22 (`simplifier.cpp`, `clusterizer.cpp`, vendored in
`third_party/meshoptimizer`).

| Phase | Status |
|---|---|
| 1. Offline builder + DAG (CPU) | **done** |
| 2. GPU culling / LOD selection / Hi-Z | **done** |
| 3. Visibility buffer raster + resolve | **done** |
| 4. Streaming | **done** |
| 5. Integration, `r_era`, test scene, `bench_vgeo` | **done** |
| 6. Skinned | **done** (main view; shadows / scope / phone views keep the legacy skinned path) |
| 7. Foliage support | **done** (masked / two-sided, WPO, placement chunks, impostors, displacement; budget slightly over: see "Bench") |

## Files

| File | What |
|---|---|
| `src/render/vgeo/vgeo_gpu.h` | GPU / file structs (std430 mirrors, size asserts) |
| `src/render/vgeo/vgeo_build.{c,h}` | builder: weld, clusters, DAG, BVH, pages, discrete LODs, (de)serialize, decode, CPU cut |
| `src/render/vgeo/vgeo_import.{c,h}` | glTF → builder, `assets/cache/<stem>.vgeo.<key>.dgc` cache |
| `src/render/vgeo/vgeo_procgen.{c,h}` | procedural test meshes: displaced rock, torus, rock field, statue |
| `tools/build_vgeo/build_vgeo.c` | offline tool |
| `tests/test_vgeo_build.c` | ctest `vgeo_build` (+ `vgeo_build_long`, label `long`: the 10M gate) |
| `src/render/vgeo/vgeo_ctx.{c,h}` | `VgContext` (device, VMA, shader loader, caps), buffers, uploads, one-shot commands |
| `src/render/vgeo/vgeo_scene.{c,h}` | `VgScene`: global mesh / node / group / page / instance / cell tables, commit, CPU reference cut |
| `src/render/vgeo/vgeo_cull.{c,h}` | `VgCuller`: per-view culling record, Hi-Z build, counters, timestamps |
| `src/render/vgeo/vgeo_stream.{c,h}` | `VgStream`: page streaming (request readback, loads, page-table scatter, LRU eviction) |
| `shaders/vgeo/vg_stream_patch.comp` | page-table scatter |
| `tests/test_vgeo_stream.c` | ctest `vgeo_stream` (`--validation`) |
| `shaders/vgeo/vgeo_common.glsl` | GLSL mirror of `vgeo_gpu.h`, bindings, frustum / projection / Hi-Z helpers |
| `shaders/vgeo/vg_expand.glsl` | instance test and node expansion (shared by cells / instances / traverse) |
| `shaders/vgeo/vg_{args,cells,instances,traverse,clusters,hiz}.comp` | the cull kernels and the Hi-Z build |
| `src/render/vk_vgeo.c` | game glue: context, frame graph passes, cvars, console commands |
| `tests/vgeo_gpu_harness.{c,h}` | window-less Vulkan device + synthetic Hi-Z images |
| `tests/test_vgeo_gpu.c` | ctest `vgeo_gpu` (`--validation`) |
| `tools/bench_vgeo/bench_vgeo.c` | GPU timings of the cull passes, the Hi-Z build and the rasterisers |
| `src/render/vgeo/vgeo_raster.{c,h}` | `VgRaster`: visibility buffer, HW / SW raster per pass, timestamps |
| `shaders/vgeo/vg_raster*.{glsl,comp,vert,mesh,frag}` | raster args, HW mesh / vertex pulling + fragment, SW compute |
| `tests/test_vgeo_raster.c` | ctest `vgeo_raster` (`--validation`) |
| `src/render/vgeo/vgeo_skin.{c,h}` | `VgPalettes` (palette slots, per-frame upload), CPU mirror of the skinned bounds / skinning |
| `src/render/vgeo/vgeo_procgen_skinned.{c,h}` | procedural skinned creature (body + tentacles, 31 bones) and its analytic animation |
| `shaders/vgeo/vg_test_skin.comp` | test-only kernel: the shared skin helper on a vertex list |
| `tests/test_vgeo_skinned.c` | ctest `vgeo_skinned` (`--validation`) |
| `src/render/vgeo/vgeo_tex.{c,h}` | `VgTextures`: the texture table at binding 23 of every cull set (renderer ids + module textures) |
| `src/render/vgeo/foliage_support.{c,h}` | WPO CPU mirror + declared bounds, fade / density / impostor mirrors, `.vgp` chunks, scatter, `foliage_query`, impostor upload, displacement buffer |
| `src/render/vgeo/vgeo_impostor.{c,h}` | CPU hemi-octahedral impostor baker, file section |
| `src/render/vgeo/vgeo_procgen_foliage.{c,h}` | grass tuft, wheat, the test bush, alpha masks, their materials |
| `src/render/vgeo/vgeo_tree.{c,h}` | imported tree models: species table, FBX / OBJ import, wind weights, capsules, cache, coverage mips |
| `shaders/vgeo/wpo.glsl`, `vg_mask.glsl`, `vg_impostor.glsl` | WPO snippets, the shared per-pixel mask test, impostor quads / atlas |
| `shaders/vgeo/vg_raster_sw*.comp`, `vg_raster_hw*` | raster variants: plain / masked / impostor (SW), plain / masked (HW), bodies in `*.glsl` |
| `shaders/vgeo/vg_displace.comp`, `vg_test_wpo.comp` | displacement buffer update; test-only WPO kernel |
| `src/game/game_foliage.c` | `foliage_test_scatter`, trunk capsules, player pushes |
| `tests/test_vgeo_foliage.c` | ctest `vgeo_foliage` (`--validation`) |

The builder is pure C with no renderer dependency. It is compiled into the game (through the `src/` glob), the
`dg_vgeo` static library (tool and tests), and nothing else. The GPU module is the `dg_vgeo_gpu` library (see
"GPU module").

## Builder

```
source triangles
  -> weld        positions snapped to one integer grid per mesh (q_step = extent / 65000), stable sort weld on
                 grid pos + normal + uv + colour; position-only weld ids for adjacency / locking;
                 degenerate triangles dropped, missing normals smoothed
  -> leaves      meshopt_buildMeshlets (128 verts / 128 tris, cone weight 0.25) + optimizeMeshlet, per material
  -> DAG         per level: Morton-ordered clusters, shared-edge adjacency (radix-sorted edge keys), greedy groups
                 of ~8 (min 4; nearest-in-Morton-window fallback for disconnected parts);
                 per group, in parallel: lock every vertex whose position is shared with another group or material,
                 meshopt_simplifyWithAttributes to 50 % (ErrorAbsolute; retry at 25 %; else carried unchanged),
                 re-cluster. Stops at one cluster, 32 levels, or < 5 % reduction; the last level is the root
                 group(s) with parent_error = FLT_MAX.
  -> pack        per-level 4-wide BVH over groups (median split, widest axis) under one root; groups coarse ->
                 fine into pages <= 128 KiB (a group never spans pages); root pages = prefix holding the top 3
                 levels; cluster payloads: 12 B vertices, 4 B triangles
  -> LODs        discrete cuts at 0 / 0.25 % / 1 % / 4 % of the radius, as plain index + vertex buffers
  -> stats       per level clusters / tris / groups / error range; per-stage ms
```

**Cut rule** (CPU reference `vgeo_cut`, the GPU uses the same test): cluster `c` in group `g` is drawn at
object-space error `t` when `g.parent_error > t && c.error <= t`. Errors are monotonic up the DAG by construction:
- a cluster's error is the error of the group it was simplified from;
- a group's parent error is ≥ every member's error.

So exactly one of every ancestor chain passes the test.

**Determinism.** There is no hash-map iteration and no `qsort`: all sorts are stable merge sorts or LSD radix
sorts. Group jobs write into per-group slots that are concatenated in group order. The file has no timings or
thread counts. The bytes are identical across runs and thread counts (tested with 1 thread vs 8, and two 8-thread
runs).

### Usage

```
build_vgeo assets/models/barrel.gltf --stats [--out barrel.vgeo] [--threads N]
build_vgeo --gen rock|field|statue|torus [--tris N] [--seed S] --stats
```

In `data/assets.json`, `"vgeo": true` on a model builds it at load. A cache hit is about 0.1 ms. The build is kept
on `Model.vgeo`. Registering it with the GPU scene comes with Phase 5; until then the GPU content is
`vgeo_test_scene`.

Generated meshes are never committed. Tests and benches generate them at run time (deterministic for a seed).

### Phase 1 results (i5-11600K, 6C/12T)

| Input | Result | Time | Peak memory | Gate |
|---|---|---|---|---|
| 10.0M tris rock field, 4 threads | 157k clusters, 19.9k groups, 2077 pages, 18 levels | 30.9 s | 1.38 GB | < 3 min, < 6 GB |
| 504k tris statue, 12 threads | 7.9k clusters, 106 pages, 13 levels | 1.0 s | 86 MB | < 10 s |
| 52k tris torus | 817 clusters, 11 pages, 10 levels | 0.10 s | 19 MB | — |

The 10M stage split is: weld 4.2 s, leaves 10.3 s, DAG 8.5 s, pack 6.2 s, LODs 0.7 s. Leaf meshletization is
single-threaded (one call per material) and is the obvious next speed-up if it is ever needed.

### Tests (`ctest -R vgeo_build`)

- Cluster limits (≤ 128 / 128, non-empty).
- Pages ≤ 128 KiB.
- Culling spheres contain the **decoded** vertices.
- Errors are monotonic and LOD spheres are nested.
- A root group exists.
- **Locked boundaries:** for every group, the boundary edge multiset (directed, in position-weld ids) of its
  clusters equals that of the clusters simplified from it.
- **Cut sweep:** 25 thresholds over 0 to 160× the radius on a rock (genus 0) and a two-material torus (genus 1).
  The decoded cut's grid edges must all be matched by a reverse edge (no crack or hole), the Euler characteristic
  must equal 2 or 0, the triangle count must be non-increasing, and no cluster may appear in the same cut as one of
  its DAG ancestors.
- Discrete LOD triangle counts strictly decrease, and LOD0 is the full mesh.
- Serialize round trip is byte-identical, and truncated or bad-magic files are rejected.
- Determinism across thread counts.
- glTF import + cache: build then hit, identical pages.
- Gates: 500k statue < 10 s; `--long`: 10M on 4 threads < 3 min and < 6 GB.

## GPU module

The GPU side runs on a small `VgContext` (device, queue, VMA, frames in flight, a shader loader and the
capability bits) and touches no renderer globals. The game fills it from its device (`src/render/vk_vgeo.c`);
the window-less harness (`tests/vgeo_gpu_harness.c`: instance + device, no surface or swapchain, the same vgeo
feature set as `vk_device.c`) fills it from its own, and so do `test_vgeo_gpu` and `bench_vgeo`. The module is the
`dg_vgeo_gpu` static library (`vgeo_ctx.c`, `vgeo_scene.c`, `vgeo_cull.c`, `vgeo_stream.c`) for those two; the game compiles the
same files through the `src/` glob.

**Requirements.** `scalarBlockLayout` (every vgeo shader uses scalar layout) and `shaderInt64` +
`shaderBufferInt64Atomics` (the Phase 3 visibility buffer). Without them vgeo is off and the 2015 / 2010 era LODs
are used. `drawIndirectCount` + `multiDrawIndirect`, descriptor indexing and `VK_EXT_mesh_shader` are optional:
the first two are enabled when present, the mesh shader extension is only detected until Phase 3.

### Scene (`VgScene`)

Global tables every view culls against, registered and uploaded at load time; nothing runs per frame.
- `vgeo_scene_add_mesh` appends a built mesh: `VgMesh` (`node_base`, `group_base`, `page_base` into the global
  tables), its BVH nodes, groups and pages. Groups with > 16 clusters are warned about (the cluster candidate
  encoding has 4 bits for the cluster within the group).
- `vgeo_scene_add_instance` appends a 32 B `VgInstance` (smallest-three rotation, half scale).
- `vgeo_scene_commit` bins instances into **8 m cells** on x / z (stable sort, instances of a cell contiguous,
  cell AABB = union of the instance mesh spheres), uploads every table and bumps `generation` so cullers rewrite
  their descriptor sets.
- **Page pool:** without a budget every page is resident and **slot = global page index**. With one
  (`vgeo_scene_set_pool`, set by the streamer) the pool has that many 128 KiB slots and a commit makes only the
  **root pages** resident (the builder's root prefix per mesh, `VgFileHeader.root_page_count`); see "Streaming".
  The page table (global page → slot, `~0` = missing, CPU mirror `page_table`) and `slot_page` (slot → page) are
  the residency.
- **Page sources:** `vgeo_scene_add_mesh` keeps a copy of the page data; `vgeo_scene_add_mesh_file` keeps only
  the path of the `.vgeo` file (the `vgeo_deserialize` of it locates the page data section) and reads pages back
  with `platform_read_file_range`. `vgeo_scene_read_page` reads either.
- The CPU keeps every page's header and cluster records ("meta", ~5 % of the page bytes) for the CPU reference
  cut (`vgeo_scene_cpu_select`, `_ex` with `all_resident`), `vgeo_scene_cluster` (by slot; a zeroed record for a
  free slot) and the streamer's page dependencies.

### Culling (`vg_cull_record`, `shaders/vgeo/*.comp`)

Per view, in two passes. Pass 1 tests against last frame's Hi-Z, reprojected with `prev_view_proj`. Pass 2
(`VGV_TWO_PASS`) re-tests whatever pass 1 found occluded against this frame's Hi-Z.

```
pass 1
  vg_cells        1 thread per cell: frustum + Hi-Z on the cell sphere. <= 32 instances: tested inline
                  (most cells); larger: cell list region 0; occluded: cell list region 1
  vg_instances    1 workgroup per large cell, its instances strided over 64 lanes
    vg_instance   (shared) frustum + Hi-Z on the mesh sphere; the root's error test and expansion inline,
                  so traversal starts at the root's children; occluded: instance list
  vg_traverse     x (max_depth - 1): one indirect dispatch per BVH level (level-synchronous), node queue
                  region 0 <-> 1; visit = frustum, project(max_parent_error) > t, not Hi-Z occluded;
                  occluded nodes: region 2 (pass 2 seeds). Leaves, per group with project(parent_error) > t:
                  page resident -> one cluster candidate per cluster (instance, group << 4 | k) and
                  the slot's usage stamp, else a page request (priority = projected error)
  vg_clusters     1 thread per candidate: the cut rule's other half (own error <= t, or the child group's
                  page missing), then frustum, cone, Hi-Z; occluded: occluded list;
                  SW if the projected diameter < r_vgeo_sw_px, else HW
pass 2
  occluded cells (region 1) -> inline / region 0 -> occluded instances -> traversal from region 2 ->
  candidates (region 1) -> pass 1's occluded clusters (Hi-Z only: frustum and cone already passed)
```

- **Cut rule** as on the CPU: the group test (`parent_error`) is done at the leaf, the cluster test in
  `vg_clusters`. Projection: `px = error * proj_scale / max(dist - radius, znear)`, ortho views
  `error * proj_scale`.
- **Cone:** 8-bit oct axis and snorm8 cutoff, plus a 0.02 margin for the axis quantisation. Skipped for
  `TWO_SIDED` / `NO_CONE` clusters and for `VGV_NO_CONE` / `VGV_ORTHO` views.
- **Visible lists:** one buffer, HW entries at `[0, cap)`, SW at `[cap, 2 cap)`, entry `(instance, page slot << 12 |
  cluster)`. The pass 1 ends are copied to `VGN_HW_P1` / `VGN_SW_P1`, so pass 2's additions are the tail.
- **Indirect args:** `vg_args` (one thread) turns a counter into dispatch args (split over y past 65535 groups),
  clamps it to its cap and resets the next queue's counter.
- **Buffers per view:** counters, cell list (2 regions), instance list, node queue (3 regions), cluster
  candidates (2 regions), visible, occluded, page requests. Every append is bounded and counts `VGN_OVERFLOW`.
  Game caps: 512k nodes per region, 1M candidates per region, 1M visible per list, 512k occluded, 4096 requests.

### Occlusion Hi-Z

- MAX (farthest) R32F pyramid, depth 0 = near, at **half resolution, power-of-two sized** (`vg_hiz_size`): mip 0
  is the max of each 2×2 depth block, each next mip the 2×2 max of the previous one, so texel `x` of mip `j`
  covers depth pixels `[x << (j+1), (x+1) << (j+1))` exactly; pixels off the screen count as 0 (never occlude).
- `vg_hiz_record` writes 6 levels per dispatch (SPD-style: one 16×16 workgroup per 32×32 tile of the first
  level, the rest through shared memory), so a 1080p chain is 2 dispatches; level 0 from the depth buffer uses
  `textureGather`. The source is a depth view or the visibility buffer (depth = 1 - its high 32 bits; empty =
  far) for this frame's pass 2 Hi-Z. 1080p: 0.055 ms per build (budget 0.1 ms for two: slightly over).
- Test (`vg_occluded_vp`): the sphere's box corners are projected; a corner behind the camera plane or at
  depth ≤ 0 means visible. The screen rect (depth pixels) is grown by 1 px for TAA jitter (the matrices are the
  unjittered ones). The mip is chosen so the rect covers ≤ 2×2 texels; occluded when the nearest corner is behind
  the max of those texels. If the rect needs a mip beyond the chain, the test refuses (keeps it visible).
- No history (first frame, camera cut, `r_vgeo_occlusion 0`): no occlusion in pass 1.

### Counters and CPU cost

- `VgCounters` (256 B) holds the indirect args and the counts (cells, instances, nodes visited / occluded,
  candidates, selected, HW / SW, occluded clusters, triangles, page requests, overflow, pass 2 counts).
- Nothing is read back in the frame. The counters are copied to a per-frame host buffer and collected
  (`vg_cull_collect`) when that frame slot is recorded again. Timestamps: 5 per (frame, view), giving ms for
  instances (cells + instances), traversal, clusters and pass 2.
- The CPU uploads one `VgViewGpu` (`vkCmdUpdateBuffer`) and records a dispatch count that depends only on the
  deepest BVH, never on the instance count: pass 1 is `5 + 2 (max_depth - 1)` dispatches (each level is args +
  traverse), e.g. 15 for the test scenes (max_depth 6: 5 traversal levels); pass 2 adds `10 + 2 (max_depth - 1)` (35 in all).

### In the renderer (`src/render/vk_vgeo.c`)

Per frame, main view (slot 0), all before the legacy prepass:

```
vgeo_cull      pass 1 cull against last frame's Hi-Z (vgeo_hiz history)    vg_cull_record_pass1
vgeo_raster    clear the visibility buffer, rasterise pass 1                  vg_raster_record(pass 1)
vgeo_hiz_cur   this frame's Hi-Z from the visibility buffer (r_vgeo_occlusion 2)
vgeo_pass2     re-cull pass 1's occluders against it, rasterise them          vg_cull_record_pass2 + raster(pass 2)
prepass        vgeo depth / G-buffer export first (vgeo_gbuffer.frag), then the legacy draws on top
vgeo_hiz       next frame's history: MAX Hi-Z of the prepass depth (vgeo + legacy occluders)
opaque         legacy forward lighting, then the vgeo lit resolve (vgeo_lit.frag, depth EQUAL)
```

- Rasterised with this frame's **jittered** projection (`VgViewGpu.raster_view_proj`, as the legacy prepass),
  culled and velocities with the unjittered ones.
- Sun shadow cascades: before the shadow pass, each cascade is a vgeo view (slots 1-3: `VGV_ORTHO`, frustum +
  LOD only, `r_vgeo_shadow_error_px`, every cluster HW) culled with `vg_cull_record`; a 1-thread kernel writes
  the draw args and `vgeo_shadow.vert` (vertex pulling, depth only) draws the cascade's HW list into its atlas
  tile right after the legacy shadow draws. Point / spot light shadows (docs/LIGHT_SHADOWS.md) do not include
  vgeo yet.
- Active when `r_vgeo` is on and the scene has instances. Cvars: `r_vgeo` (1), `r_vgeo_error_px` (1.0),
  `r_vgeo_shadow_error_px` (2.0), `r_vgeo_sw_px` (32), `r_vgeo_occlusion` (0 off, 1 last frame's Hi-Z, 2 two-pass;
  default 2), `r_vgeo_freeze`, `r_vgeo_shadows` (1), `r_vgeo_debug_spheres`, `r_vgeo_lod_px` (fallback),
  `r_foliage_density` (1.0), `r_vgeo_impostor_px` (16), `r_era`.
- Commands: `vgeo_stats`, `vgeo_test_scene [count] [spacing] [meshes]` (procedural rocks around the camera),
  `vgeo_clear`, `vgeo_bench [frames=600] [csv] [vgeo_test|foliage]`; foliage: see "Foliage support".

### Streaming in the renderer

- `vg_stream_init` after the culler (pool budget `r_vgeo_pool_mb`, default 512; `r_vgeo_stream_pages_per_frame`
  64); every frame, before the first cull (the sun cascades), the frame slot's counters / debug readbacks are
  collected and `vg_stream_update` runs; the budget applies at the next scene upload (`render_vgeo_set_level`
  sets it first). Level `.vgeo` meshes stream their pages from the file (`vgeo_scene_add_mesh_file`);
  procedural test meshes from memory. `vgeo_stats` prints resident / free / loaded / evicted / waiting.
- `map vgeo_test`, `vgeo_bench 300`, 96 MB pool (768 slots for 2599 pages): 695 pages loaded, 235 evicted, stream
  CPU 0.015 ms mean / 0.30 max per frame, the vgeo passes unchanged (worst frame 1.87 ms). 512 MB: everything
  needed fits (1056 resident).

### Characters in the renderer (Phase 6 glue, `vk_vgeo.c` "skinned characters")

- Every renderer skinned mesh drawn in the world is built to vgeo on a worker thread (`SkinVertex` joints /
  weights straight from the mesh's CPU copy) and enters the scene with a **pool of instances** (identity
  transform: the game's palettes are world space), one palette slot each, starting at 4 per mesh and doubling
  to what a frame needs (each growth re-commits the scene; `vg_cull_prepare` then resizes the views before the
  frame records anything).
- Per frame `vkg_route_skinned` (render.c, before the legacy skinned passes are built) gives each eligible
  draw (not viewmodel / shadow-only / highlighted / fading, has an object id, <= 128 bones) a stable slot of its
  mesh's pool (by object id: the previous palette gives the velocity), uploads its palette (`vg_palettes_set`)
  and its material (per-slot material entries: tint, roughness, the skin texture through the bindless array),
  and removes it from the legacy main pass. Slots not used this frame are cleared (culled), freed after 2.
- Not routed: the sun cascades (vgeo cascade views carry `VGV_NO_SKINNED`; characters cast their legacy shadows,
  also into point / spot light tiles), the scope picture-in-picture (no routing while a PiP scope is up) and the
  phone camera views. `r_vgeo_characters 0` switches it off.
- devroom `spawn_horde 500`: 27 character meshes (parts x LODs), 1326 pool instances, 327 characters in view
  drawn by vgeo, 0 validation messages; frame 11.7 ms with the horde AI / animation (CPU-bound).

### Resolve and lighting (`shaders/vgeo_gbuffer.frag`, `vgeo_lit.frag`, `vgeo/vg_resolve.glsl`)

- Both are full-screen triangles (`vgeo_fullscreen.vert`) that decode the pixel's visibility entry: visible list
  entry → instance + cluster → triangle → its 3 vertices (`vg_decode.glsl`), transformed and projected with
  `raster_view_proj`; perspective-correct barycentrics of the pixel centre interpolate world position, normal,
  uv and colour. Depth is the visibility buffer's own (`gl_FragDepth = 1 - hi`), so the export, the Hi-Z and the
  lit pass agree to the bit.
- The **G-buffer export** runs the shared `gbuffer_body.glsl` and the **lit resolve** the shared `lit_body.glsl`
  (`GB_CUSTOM_INPUTS` / `LIT_CUSTOM_INPUTS`: the includer supplies the varyings), so every lighting feature
  (cascaded + local light shadows, sky SH / environment, SSR / GTAO / SSGI, weather, fog, grids) applies to vgeo
  pixels unchanged. The lit pass tests depth EQUAL: pixels a legacy draw covered keep their forward shading.
- Material: `VgMesh.material_base` + the cluster's material indexes the material table (binding 19): colour,
  roughness, metallic, emissive, flags. Textures are not bound yet (bindless table with Phase 5 glTF content:
  `MAT_TEXTURED` is masked off).
- Velocity: current / previous unjittered view-projection of the reconstructed position (static instances).
- The vgeo pipelines use the renderer's sets 0-2 and push range (compatible with `layout_lit`) plus the vgeo
  set as set 3 (`VG_SET 3`, `VG_NO_PUSH`).

### Levels, test scene and eras

- Level entity `"vgeo"` (`src/world/level.c`): `mesh` (a `.vgeo` file or a model name with `"vgeo": true`), `pos`,
  `yaw`, `scale`, `scale_jitter`, `grid: [nx, nz]`, `spacing`, `color`, `roughness`, `seed`, `collision`. The game
  hands the placements to `render_vgeo_set_level` after the level loads; both sides expand grids with
  `vgeo_placement_expand` (same instances) and share the loaded file (`vgeo_file_get`, once per process).
- Collision (static, `.vgeo` files): `"mesh"` (default for single placements) = the coarsest discrete LOD as a
  triangle mesh per instance; `"sphere"` (default for grids) = the bounding sphere x 0.8; `"none"`.
- `map vgeo_test` (`data/levels/vgeo_test.json`): the 10M-triangle rock field (north), 1024 instances of the
  500k-triangle statue (south), the 2M-triangle building (east: four 16 × 8 m continuous walls with a bevelled
  brick relief, `vgeo_gen_building`). The content is generated at build time by `build_vgeo --gen` into
  `bin/assets/vgeo/` (only when missing; never committed). File sizes: rock field 575 MB, building 109 MB, statue
  28 MB — most of it the discrete LODs stored as f32 vertices (Phase 4 streaming loads pages on demand).
- `r_era` (0 modern, 1 2020, 2 2015, 3 2010, 4 custom) sets `r_vgeo`, `r_vgeo_error_px`, `r_vgeo_shadow_error_px`,
  `r_vgeo_lod_px` and `r_foliage_density` (1.0 / 0.7 / 0.4 / 0.2); any other combination reads back as Custom.
  Engine Fidelity (docs/ENGINE_FIDELITY.md) bounds these: its levels scale the era's error thresholds (Low x3)
  and cap its foliage density; the detection compares against the bounded values, so the era still reads back.
- **2015 / 2010 fallback** (`r_vgeo 0`): each mesh's exported discrete LODs are uploaded as legacy meshes on
  first use and drawn through the legacy draw list; the CPU picks per instance the coarsest LOD whose projected
  error is under `r_vgeo_lod_px` (4 px 2015, 12 px 2010; 2010 never draws LOD 0). This per-instance CPU work
  exists only in the fallback.
- `vgeo_bench [frames] [csv]`: a fixed Catmull-Rom flythrough of `vgeo_test` (camera override in
  `render_set_camera`), CSV of every frame-graph pass's GPU ms + visible clusters / triangles, and a summary.

### Phase 3 / 5 results in game (RTX 3060 Ti, 1600×900 window)

- `map vgeo_test`, `vgeo_bench 600` (frames 10+, GPU ms per pass): vgeo_cull 0.220, vgeo_raster 0.114,
  vgeo_hiz_cur 0.038, vgeo_pass2 0.149, vgeo_hiz 0.036; worst frame vgeo + prepass + opaque 1.83 ms; CPU 4.1 ms per
  frame. 1026 instances, 2599 pages resident (325 MB).
- devroom `vgeo_test_scene 400 8 2`, validation on: 0 messages; the rocks are lit, shadowed (sun + local
  lights) and composite with the legacy scene (`docs` screenshots in the session log).
- vgeo vs legacy (`tools/tests/vgeo_legacy_diff.sh`): the same rock scene through vgeo and through the
  fallback at LOD 0 detail: 0.95 % of the pixels differ by more than 24 / 255 (gate 1 %).
- `vgeo_check_resolve` (console; reads one frame back): every pixel the visibility buffer covers holds exactly
  its depth or a nearer legacy depth, and its velocity equals the reprojection of that depth through the
  current / previous unjittered views. During a slow `vgeo_test` flythrough with validation: 23 475 + 31 025 exact
  pixels (4958 / 6274 under nearer legacy surfaces), 0 missing, velocity error max 7.0e-7 UV for a mean velocity of
  2.5e-4 / 3.8e-4 UV, 0 validation messages.

### Tests (`ctest -R vgeo_gpu`, runs `--validation`)

Scene: a rock, a two-material torus and a 60k statue, 400 instances.
- **LOD cut:** for 8 cameras (LOD only), the GPU set equals `vgeo_scene_cpu_select`: nothing missing, extras only
  within 1e-4 of the threshold.
- **No cluster is drawn with a DAG ancestor** (walking `child_group` chains of the GPU list).
- **Frustum:** no cluster inside is lost, none outside is kept (borderline band excluded); visible instance count
  vs brute force.
- **Occlusion:** a wall at 30 m over the left half of a synthetic depth buffer. Nothing in front of or beside it
  may be culled, and most clusters behind it must be. Pass 2 against an empty current Hi-Z must restore exactly the
  set without occlusion.
- **Hi-Z:** on 333×217, every texel bounds its whole footprint (odd tails).
- **CPU cost:** same dispatch count for 100 and 10 000 instances, record time does not grow.
- Zero validation messages.

### Phase 2 results (RTX 3060 Ti, 1920x1080)

- `test_vgeo_gpu`: 65/65 checks, 0 validation messages.
- GPU cut identical to the CPU reference on all 8 cameras (0 borderline, 0 extra).
- Occlusion: all 998 must-stay clusters kept, all 597 clusters behind the wall culled. Two-pass restores the full
  set exactly.

`bench_vgeo` (8 rocks × 79k tris, 100k instances on a 1.9 km field, error 1 px; 2-pass = synthetic walls over 60 %
of the screen, the same pyramid as both the previous and the current Hi-Z, so pass 2 measures the re-test cost):

| Camera | Occlusion | inst ms | trav ms | clus ms | pass2 ms | total | instances | nodes | clusters | tris |
|---|---|---|---|---|---|---|---|---|---|---|
| eye level, across | off | 0.077 | 0.242 | 0.041 | — | 0.361 | 75132 | 228381 | 80585 | 6.41M |
| eye level, across | 2-pass | 0.046 | 0.127 | 0.025 | 0.091 | 0.289 | 26444 | 81163 | 29764 | 2.44M |
| eye level, inside | off | 0.045 | 0.127 | 0.029 | — | 0.201 | 28144 | 87587 | 33986 | 2.86M |
| eye level, inside | 2-pass | 0.033 | 0.088 | 0.022 | 0.083 | 0.226 | 10177 | 32632 | 13903 | 1.24M |
| elevated 60 m | off | 0.083 | 0.266 | 0.051 | — | 0.401 | 99848 | 300531 | 101162 | 7.79M |
| elevated 60 m | 2-pass | 0.047 | 0.107 | 0.020 | 0.087 | 0.261 | 30375 | 91554 | 30601 | 2.36M |
| top down 400 m | off | 0.032 | 0.065 | 0.015 | — | 0.113 | 15993 | 47979 | 15712 | 1.20M |
| top down 400 m | 2-pass | 0.027 | 0.047 | 0.015 | 0.070 | 0.159 | 6507 | 19521 | 6321 | 0.48M |

- Worst camera: 0.401 ms against the 0.5 ms culling budget.
- Hi-Z build at 1080p: 0.079 ms per build. The Phase 3 budget is 0.1 ms for **two** builds, so the build must get
  faster in Phase 3.
- In game (devroom, 20k rocks from `vgeo_test_scene`): `vgeo_cull` 0.22 ms, `vgeo_hiz` 0.10 ms, 0 validation
  messages.
- Only NVIDIA hardware was available: there is no AMD run yet.

How it got there (the first version's worst camera was 0.77 ms):
- One workgroup per cell idled on cells of a few instances → small cells are processed inline.
- The BVH root is expanded in the instance step, saving a traversal level.
- Group items with 16 lanes idled on the 1-cluster root groups → one candidate per cluster.
- Hi-Z mip 0 at half resolution.

## Rasterisation (Phase 3, `vgeo_raster.c`)

Two rasterisers write the same 64-bit value per pixel into the visibility buffer, an SSBO `u64[w * h]` (binding 18
of the cull set, cleared to 0 every frame):

```
hi 32  floatBitsToUint(1 - z)          z = Vulkan NDC depth at the pixel centre (0 near): atomicMax keeps the nearest
lo 32  visible index << 7 | triangle   visible index: HW list k -> k, SW list k -> cap_visible + k (25 bits)
```

- **Per pass.** `vg_raster_record(r, cmd, frame, slot, pass)` draws the pass range of both lists: pass 1 the entries
  `[0, *_P1)`, pass 2 `[*_P1, total)`. `vg_raster_args` (one thread) turns the counters into the indirect args
  (`VGA_DRAW_HW`, `VGA_MESH`, `VGA_SW`). The raster shares the culler's descriptor sets (it uses its own pipeline
  layout: same set layout, `VgPush` for vertex / mesh / fragment / compute). Timestamps: HW and SW ms per (frame,
  view, pass) (`vg_raster_timings`). `serialize` puts a barrier between HW and SW for exact per-rasteriser
  timings; by default they overlap.
- **HW** (clusters ≥ `sw_px` across, or near the camera): no attachments, no depth test (the atomic is the depth
  test), dynamic rendering over the view size, back faces culled with CCW front faces (as `vk_pipelines.c`).
  - `VK_EXT_mesh_shader`: one 128-thread mesh workgroup per cluster (≤ 128 vertices / 128 triangles), the payload
    as a per-primitive output, `vkCmdDrawMeshTasksIndirectEXT`.
  - Fallback: vertex pulling. One `vkCmdDrawIndirect` of 384 vertices per instance; `firstInstance` = the pass
    range start, so `gl_InstanceIndex` is the visible index. Triangles past the cluster's count collapse outside
    the frustum.
  - The fragment shader does the `atomicMax` at `gl_FragCoord`.
- **SW** (compute, one 128-thread workgroup per cluster):
  - The threads transform the cluster's vertices once into shared memory: screen space in 16.8 fixed point
    (`floor(x * 256 + 0.5)`, the HW's 8 sub-pixel bits) and NDC depth.
  - Then one thread per triangle: back-face cull with the HW convention (Vulkan's signed area is `-cross` with y
    down), bounding box scan over the pixel centres `(x + 0.5, y + 0.5)` with integer edge functions. 32-bit when
    the triangle spans < 128 px (every real SW triangle), 64-bit otherwise.
  - **Fill rule:** top-left, as D3D / Vulkan hardware (y down): with the vertices in screen-clockwise order, a top
    edge runs exactly horizontal to the right and a left edge runs up. Pixel centres exactly on any other edge are
    excluded (bias -1 on the integer edge function).
  - Depth from the plane through the snapped vertices at the pixel centre (unbiased barycentrics), outside
    [0, 1] discarded.
- **Near plane.** The SW path never clips: `vg_clusters` sends a cluster to HW when its culling sphere comes
  within `znear` of the near plane (every vertex then has w ≥ 2 znear in SW). The plane is `planes[4]`, the view
  depth, not the Euclidean distance, which does not bound w.
- **Winding.** The procedural meshes (rock, torus, statue) are CCW outward for this convention (positive signed
  volume, checked by the test), so the generator was left as it was.

### Device features (game device: `vk_device.c`)

On top of the Phase 2 set, the rasterisers need:
- `fragmentStoresAndAtomics` (the HW fragment atomic; already enabled by `vk_device.c`);
- `drawIndirectFirstInstance` (vertex pulling; already enabled when present);
- for the mesh shader path, when present: the device extension `VK_EXT_mesh_shader`, its feature
  `VkPhysicalDeviceMeshShaderFeaturesEXT::meshShader` (task shaders are not used), and the Vulkan 1.3 feature
  `maintenance4`: glslang emits `LocalSizeId` for mesh shaders. Then set `VgContext.mesh_shader`.

`vertexPipelineStoresAndAtomics` is **not** needed: the vertex shader reads the visible list through a read-only
alias of binding 13.

### Tests (`ctest -R vgeo_raster`, runs `--validation`)

Same scene as `vgeo_gpu` (rock, two-material torus, 60k statue, 400 instances), 1920×1080, 4 cameras. The last one
is 0.15 m from a rock's surface, so the near plane cuts clusters.
- **Winding:** positive signed volume of every procedural mesh.
- **HW vs SW:** the same view all-HW (`sw_px` 0) and all-SW (`sw_px` 1e9, near-plane clusters still go to HW).
  The decoded (instance, cluster, triangle) per pixel is compared: ≤ 0.5 % of the covered pixels may differ, and
  every differing pixel must have a 3×3 neighbour of another triangle.
- **Mesh shader vs vertex pulling:** the same tolerance.
- **Depth:** about 3000 pixels per image against a CPU recomputation. The CPU decodes the triangle, transforms it
  with `raster_view_proj` in f64, snaps it to 1/256 px and evaluates the plane at the pixel centre. It must agree
  within 1e-5 relative, plus one snapping step of the depth slope (an f32 vertex can round to the other side of a
  1/256 step than the f64 reference).
- **Mixed (`sw_px` 32) vs all-HW:** no holes between the two rasterisers, same tolerance.
- **Two-pass:** pass 1 is occluded by a synthetic wall, pass 2 against an empty Hi-Z. Passes 1 + 2 rasterise the
  identical image to one pass.
- Zero validation messages.

### Phase 3 raster results (RTX 3060 Ti, 1920×1080)

`test_vgeo_raster`: 118/118 checks, 0 validation messages, 1.0 s.

| Camera | Covered px | HW vs SW differing | mixed vs HW | mesh vs vertex pulling | depth worst rel. error |
|---|---|---|---|---|---|
| edge of the field | 500 540 | 17 (0.003 %), all on edges | 1 | 0 | 9.5e-7 |
| eye level, inside | 335 371 | 23 (0.007 %) | 2 | 0 | 1.6e-7 |
| close to instances | 631 639 | 27 (0.004 %) | 0 | 0 | 2.7e-7 |
| 0.15 m from a surface (5 near-plane HW clusters) | 1 821 598 | 3 (0.000 %) | 0 | 0 | 3.2e-5 (4 samples over 1e-5, all within one snapping step) |

Two-pass: 902 HW + 95 SW clusters in pass 1, 516 + 103 in pass 2, identical to one pass (0 pixels differ).

The 3.2e-5 on the last camera comes from the vertex transform, not the rasteriser: HW and SW agree to the bit
there. The shaders transform world-space positions with an f32 `raster_view_proj`. Near the camera (0.3 m) this
costs about 1e-5 m, and 100 m from the origin it alone reaches ~5e-5 relative depth, which is why that camera
uses the rock nearest the origin. A camera-relative raster matrix (translation applied in f64 on the CPU) would
remove it. That is a `VgViewGpu` change for the integration.

`bench_vgeo` (scene and cameras as in Phase 2: 100k instances of 8 rocks; clear + raster after the cull of the
same view; HW and SW serialised; median of 300 frames):

| Camera | Occlusion | HW p1 | SW p1 | HW p2 | SW p2 | total ms | tris | HW / SW clusters |
|---|---|---|---|---|---|---|---|---|
| eye level, across | off | 0.068 | 0.669 | — | — | 0.736 | 6.41M | 3230 / 77355 |
| eye level, across | 2-pass | 0.058 | 0.177 | 0.002 | 0.003 | 0.241 | 2.44M | 2512 / 27252 |
| eye level, inside | off | 0.092 | 0.226 | — | — | 0.318 | 2.86M | 3566 / 30420 |
| eye level, inside | 2-pass | 0.083 | 0.101 | 0.002 | 0.003 | 0.189 | 1.24M | 2856 / 11047 |
| elevated 60 m | off | 0.011 | 0.654 | — | — | 0.666 | 7.79M | 102 / 101060 |
| elevated 60 m | 2-pass | 0.009 | 0.179 | 0.002 | 0.003 | 0.194 | 2.36M | 20 / 30581 |
| top down 400 m | off | 0.002 | 0.122 | — | — | 0.124 | 1.20M | 0 / 15712 |
| top down 400 m | 2-pass | 0.002 | 0.055 | 0.002 | 0.003 | 0.062 | 0.48M | 0 / 6321 |

- Worst camera: 0.736 ms, without any occlusion (6.4M triangles), against the 1.2 ms budget for HW + SW raster in
  both passes. With occlusion the worst is 0.24 ms.
- The bench's pass 2 is nearly empty: the synthetic pyramid serves as both the previous and the current Hi-Z.
- Vertex pulling (`--vertex-pulling`) measures within noise of the mesh shader here: HW is fill-bound on these
  scenes.
- The GPU was shared with other work during development. Single runs varied up to 3×, hence the medians; the
  numbers above are a clean run, reproduced on three runs within 5 %.
- The SW raster went from 1.4 ms to 0.67 ms on the worst camera by using 32-bit edge functions.
- A plain load that skips a losing `atomicMax` measured no faster, and was dropped.

### Simplifications (raster)

- **Bounding box scan only:** no scanline path for larger triangles. SW triangles are a few pixels, and the bbox
  loop is per thread (a large triangle only stalls its own warp). Triangles of any size are still correct (64-bit
  path), just slow.
- **No clipping in SW:** the near-plane classification guarantees w ≥ 2 znear. A vertex outside a ±16k px guard
  band drops its triangles; the classification never produces one.
- **One thread per triangle** in SW (Nanite's SW raster is the same); there is no work redistribution for long
  triangles.
- **The fill rule is assumed to be the HW's:** top-left with y down. Exact ties are rare at 8 sub-pixel bits, and
  measured mismatches stay on edges.
- **Equal-depth ties** go to the larger payload, so which of two coplanar triangles wins depends on the visible
  index. It is stable within a frame, but not across HW / SW reclassification.
- **The cull record still runs both passes at once:** pass 2 culling needs this frame's Hi-Z built from the pass 1
  visibility buffer. The in-frame split (cull pass 1 → raster 1 → Hi-Z → cull pass 2 → raster 2) is part of the
  renderer integration. The raster API is already per pass.
## Streaming (`src/render/vgeo/vgeo_stream.c`)

The pool is a budget of 128 KiB slots (`VgStreamConfig.pool_mb`). Root pages are resident from the commit and
pinned; every other page is loaded on demand and evicted LRU.

```
cull, frame N        leaves stamp page_stamp[slot] = N (binding 20, VgViewGpu.frame) for every group they emit,
                     and append (page, projected error) for every group whose page is missing (4096 per view)
update, frame N + 1  copies the stamps and every view's request count + list into the host buffer of its frame
                     slot (the cull of N wrote them; the copy runs before this frame's culls)
update, N + 1 + F    (F = frames in flight: that slot's fence has signalled) reads it back:
                     stamps -> CPU mirror (last_used per slot), requests -> dedupe (highest priority), sort by
                     priority (descending, then page)
                     loads, up to pages_per_frame: a page's missing parent pages first (recursively), then a
                     free slot; memory source: memcpy into the staging ring now; file source: read on the
                     streamer's worker thread, copied by the next update
                     staging -> pool slot copies, then a compute scatter of (page, slot | ~0) pairs patches the
                     page table; barriers before the culls of the frame
                     eviction, when requests are left and the free + quarantined slots cannot take them
```

- **Dependencies.** Page P depends on page Q when a cluster in Q was simplified from a group in P (Q holds coarser
  parents of P's clusters). Residency is kept closed under it: a page is loaded only after (or with) its parents,
  and a page with a resident / loading dependent is never evicted. A missing group's whole subtree is then missing,
  so the cut's fallback (a cluster whose child group's page is missing is drawn) never overlaps finer geometry, and
  the cut stays watertight (tested).
- **Eviction (LRU).** Candidates: resident, not root, no dependents, `last_used < frame - (F + 1)`. The CPU
  mirror is F + 1 frames behind, so this means "not used in the newest frame read back"; a slot's load frame
  counts as a use. Oldest first, as many as the waiting requests need (capped at `pages_per_frame`). The page
  table entry is cleared in the same frame, so no later frame can select it.
- **Quarantine.** An evicted slot is reused F frames later at the earliest: by then every frame that could
  reference the old page has passed its fence. The pool copies therefore need no barrier against earlier frames
  (only the page-table scatter waits for all stages), and no frame in flight sees a page change under it.
- **Pool full.** Requests wait; the coarser parents keep drawing. With a pool smaller than the working set the LRU
  churns (bench: about 80 % of loads are reloads within 30 frames at half the working set, none once it fits).
- **Staging ring.** `F + 1` regions of `pages_per_frame` pages plus the patch entries (`4 pages_per_frame + 16`);
  a file batch lands within one frame (the next update waits for it if needed).
- **Stats** (`vg_stream_stats`): pool slots, resident, free, requested (distinct missing pages read this frame),
  loaded / evicted this frame and in total, waiting, pending reads, CPU ms of the update and GPU ms of its copies +
  scatter (timestamps of the frame slot read back).
- **CPU cost.** O(slots + requests) per frame; no per-instance work.

### Renderer glue (to do in `src/render/vk_vgeo.c`)

- Init: `vg_stream_init(&st, &scene, &cull, &(VgStreamConfig){ r_vgeo_pool_mb, r_vgeo_stream_pages_per_frame })`
  after `vg_cull_init` (it sets the pool budget and re-commits the scene if it is already committed).
- Every frame, once the frame slot's fence has been waited (where `vg_cull_collect` runs) and **before the first
  `vg_cull_record` / `vg_cull_record_pass1` of the frame** (main view and shadow cascades):
  `vg_stream_update(&st, cmd, frame_slot)`. Then the culls, raster and resolve as before.
- Cvar changes: `vg_stream_set_config` (pages per frame now, pool size at the next commit).
- `vg_stream_shutdown` before `vg_cull_shutdown` / `vgeo_scene_free`.
- Cvars: `r_vgeo_pool_mb` (512 Modern / 256 2020), `r_vgeo_stream_pages_per_frame` (64); HUD / `vgeo_stats` line
  from `vg_stream_stats`. The residency debug view (`r_vgeo_debug 6`) can read `slot_page` / `page_root`.

### Tests (`ctest -R vgeo_stream`, runs `--validation`)

Scene: a rock, a torus, a 120k statue and a rock streamed from a `.vgeo` file (51 pages, 4 root), 400 instances in
5 × 5 patches of one mesh. Frames are recorded as the renderer does (update + cull, frame slot = frame mod 2).
- **Cold start:** frame 1 selects only root-page clusters, equals the residency-aware CPU cut, draws all 400
  instances (LOD only), and every instance inside the frustum (with culling).
- **Convergence:** 3 fixed cameras converge in 11-13 frames to the all-resident CPU cut (0 missing, 0 extra).
- **Small pool** (pool = root + 1/4 of the rest, camera changed every 15 frames, 60 frames): every instance draws
  every frame, no cluster with a DAG ancestor, residency closed, 460 instance selections of closed meshes
  watertight (every directed edge matched, decoded on the mesh grid), GPU = residency-aware CPU cut; pages evicted.
- **Eviction audit** (200 frame flythrough, pool = root + 1/6 of the rest): every visible cluster's slot is mapped;
  no page evicted in a frame is in that frame's visible list; no slot is rewritten within F frames of a frame that
  referenced it; the GPU pool bytes of every referenced slot equal the CPU page data (memory and file source).
- Zero validation messages.

### Phase 4 results (RTX 3060 Ti)

- `test_vgeo_stream`: 38/38 checks, 0 validation messages (~5 s).
- `bench_vgeo` streaming flythrough (100k instances of 8 rocks, 136 pages, 8 root, eye level, 600 frames,
  64 pages per frame):

  | Pool | Loaded | Evicted | Quick reloads | Stream GPU ms mean / max | Stream CPU ms mean / max |
  |---|---|---|---|---|---|
  | 72 slots (9 MB) | 1584 | 1523 | 1259 | 0.017 / 0.383 | 0.061 / 1.05 |
  | 96 slots (12 MB) | 778 | 692 | 490 | 0.012 / 0.167 | 0.033 / 1.05 |
  | 112 slots (14 MB) | 313 | 209 | 118 | 0.008 / 0.166 | 0.016 / 1.06 |
  | 136 slots (17 MB) | 128 | 0 | 0 | 0.006 / 0.166 | 0.009 / 1.10 |

  The largest frames load 59 pages (7.4 MB, cold start); the mean is well inside the 0.1 ms budget.

## Skinned meshes (Phase 6, `vgeo_skin.c`)

Skinned meshes use the same DAG, pages, culling and rasterisers as static ones. A skinned instance points at a
**palette slot**: one character's palette (mat4 per canonical-skeleton bone) plus last frame's. The cut is selected
and the clusters are culled against the animated pose. Both rasterisers, the shadow pass and the resolve skin 4
influences per vertex with one shared GLSL helper.

### Builder

- **Input.** `VgSource.joints` (u8×4, canonical skeleton index < 128), plus `weights_u8` (u8×4) or `weights`
  (float×4). With `joints == NULL` the mesh is static. The build runs in the bind pose.
- **Skin per position.** Each source vertex's skin is normalised once: duplicate joints merged, ≤ 4 influences
  sorted by weight (ties: lower joint), quantised to u8 summing to 255 (largest remainder). Unused slots repeat the
  dominant joint with weight 0, so joint 0 is the dominant bone. Every welded vertex at one grid position takes
  the skin of the first (lowest index) source vertex there. A vertex shared by two clusters therefore skins to
  the same bits in both, and animated cuts stay crack-free exactly as bind-pose ones.
- **Simplification.**
  - The 4 sorted weights are attributes of `meshopt_simplifyWithAttributes`, at the UV weight.
  - **Bone-set penalty, kept simple:** a vertex whose dominant bone differs from that of every one of its
    neighbours is locked, so a lone tip vertex is not dragged across a bone boundary. This applies **only to
    the first simplification** (the source mesh). Applied at every level, it also locked vertices that earlier
    collapses had isolated, which pinched the thin tentacles: 90 pinched cut edges on the test creature, against
    0 now.
  - The simplifier only collapses onto existing vertices, so every output vertex keeps its normalised source
    skin. No re-normalisation is needed after simplification.
- **≤ 16 bones per cluster.** Each meshlet (leaf, or re-clustered after a group simplification) is split into
  runs of triangles, in meshlet order, that touch ≤ 16 bones with a non-zero weight. A coarse cluster can
  therefore have fewer than 128 triangles. The test creature has 31 bones and still ends in a single 28-triangle
  root cluster.
- **Flags.** `VGF_SKINNED`, header `bone_count` (max weighted joint + 1). `VGC_NO_CONE` is set on every skinned
  cluster.
- **Versions.** `VGEO_BUILD_VERSION` 2 (the cache key). `VGEO_FILE_VERSION` stays 1: static meshes are
  **byte-identical** to the previous builder (`build_vgeo --gen statue / torus` compared with `cmp`), and the
  skinned layout is flagged by `VGF_SKINNED`.
- **Discrete LODs** (`VgLodVertex`) carry no skin data. Characters keep their legacy discrete LODs in the
  2015 / 2010 eras.

### Page data

| | static | skinned |
|---|---|---|
| vertex | 12 B (+4 colour) | + 8 B after the colour: joints u8×4, weights unorm8×4 (sum 255) |
| triangle block, per cluster | triangles (4 B each), 16-byte aligned | triangles, then the **bone set**, 16-byte aligned |
| bone set | — | `u32 count` (≤ 16), then 16 B per bone: `{ u8 bone, u8 pad, half3 min, half3 max }`, ascending bone ids |
| `VgPageHeader.skin_bytes` | 0 | bone-set bytes, included in `index_bytes` |

The bone box is the bind-space AABB of the cluster's vertices with a non-zero weight on that bone. It is rounded
outwards to half precision, after a 1e-6 relative margin (the GPU's f32 position decode can differ from the CPU's
by an ulp). The bone set needs no offset: its word is `tri_word + tri_count`. `vgeo_cluster_bones` decodes it.
`vgeo_bone_bounds` unions the leaf clusters' boxes into one bind-space influence sphere per bone; coarser clusters
only keep a subset of the leaf vertices, so those spheres cover them too.

### Palette slots (`VgPalettes`)

- **Buffers.** Binding 20 of the cull set holds the `mat4` palettes (column-major, bind space → instance space).
  Binding 21 holds the slot table `VgPaletteSlot { first, prev_first, bone_count, -; max_scale, k0, k1, - }`. The
  palettes come from the game's `bone_world * inverse_bind`. With an identity instance at the origin, the
  game's world-space palette is used as-is.
- **Regions.** Slot `s` owns two regions of 128 matrices, at `(2s + h) × 128`. Each `vg_palettes_set` writes the
  region the previous frame did not use; `prev_first` keeps pointing at last frame's region. A slot not set in a
  frame keeps its palette, with previous = current (no motion).
- **Per frame.**
  - `vg_palettes_begin(frame)`.
  - `vg_palettes_set(slot, palette, n)` for each animated slot. This copies into the host staging buffer of
    that frame in flight and the CPU mirror, and computes the slot bounds.
  - `vg_palettes_record(cmd)` before the frame's first cull: a barrier against every earlier shader read (the
    previous frame may still read the region or table being overwritten), one copy per set slot plus the slot
    table, then a barrier to all shader stages.
- **Memory.** 16 KiB device + 8 KiB staging per frame in flight + 2 KiB of bone bounds per slot.
- **Empty slots.** `vg_palettes_clear(slot)`, or a slot that was never set, culls its instances. A pool of
  instances can be registered once and switched on and off per frame.
- **Slot bounds (`vgeo_palette_bounds`).** `max_scale` = max over bones of `sqrt(|AᵀA|∞)` (≥ the spectral norm of
  the linear part, exact for rotation × uniform scale). `k0` / `k1` bound how far a bone moves a vertex away from
  the root bone's transform, `|P_b v − P_0 v| ≤ k0 + k1 |v|`:
  - **generic:** `k0 = max |t_b − t_0|`, `k1 = max |A_b − A_0|`. Valid for any mesh, but loose for long limbs
    (the test creature's LOD spheres grow by 5.0 m on average).
  - **per bone** (`vg_palettes_set_bone_bounds(slot, vgeo_bone_bounds of the slot's meshes)`):
    `k0 = max_b |(P_b − P_0) c_b| + |A_b − A_0| r_b`, `k1 = 0` (1.3 m on the creature). Every mesh drawn with
    the slot must be covered (`vgeo_bone_bounds_merge`).

### Instances and culling

- **Registration.** `vgeo_scene_add_skinned_instance(scene, mesh, pos, rot, scale, slot)` sets
  `VGI_SKINNED << 16` in `flags_phase` and `extra = slot`. The vertices are skinned into instance space, then
  transformed by the instance. The mesh must be `VGF_SKINNED` (otherwise the instance draws static).
- **Cells.** Skinned instances move, so `vgeo_scene_commit` bins them into cells of their own (≤ 32, the inline
  path of `vg_cells`) with an unbounded AABB. Their test is the instance test, on the slot's bounds.
- **Sphere transform.** `vg_world_sphere` of a skinned instance first maps a mesh-space sphere to
  `(P_0 c, r · max_scale + k)` in instance space, with `k = k0 + k1 (|mesh centre| + mesh radius)`. This
  transform is used for:
  - the instance (mesh sphere);
  - the BVH nodes;
  - the group and cluster LOD spheres.

  Because `|v' − P_0 v| ≤ k` for every skinned vertex `v'` (a convex combination of the `P_b v`), the result
  contains the skinned geometry of anything the bind-space sphere contains. It is also the same function of
  the sphere wherever that sphere is tested, and it keeps nested spheres nested. The DAG cut therefore stays
  consistent and monotonic, and the "never an ancestor and a descendant" property holds. CPU mirror:
  `vgeo_xf_sphere`.
- **Error scale.** Errors are scaled by `instance scale × max_scale` (`VgXf.scale`). `VgXf.pscale` is the
  instance's own scale, for positions.
- **Cluster culling sphere.** Frustum, Hi-Z, SW / HW size and the near plane test the box around
  `∪_b AABB(palette[b] · box_b)`, from the cluster's bone set (`vg_skin_bounds`, mirror
  `vgeo_skin_cluster_sphere`). This is conservative for linear blend skinning: a skinned vertex is a convex
  combination of points that each lie in one of the transformed boxes.
- **Cone culling** is off (`VGC_NO_CONE` from the builder).
- **CPU reference.** `vgeo_scene_cpu_select` does all of the above with the slot's CPU mirror (`VgXfCpu`).

### Raster, resolve, shadows

`vg_decode.glsl` holds the only skinning code:
- `vg_vertex_skin`: the joint and weight words.
- `vg_skin_matrix`: `Σ (w_k / 255) palette[first + j_k]`, as the game's `skin_matrix`.
- `vg_vertex_skin_matrix`: the current or previous palette.
- `vg_vertex_world`: skin, then the instance transform.

Its users:
- **Rasterisers.** `vg_raster_clip` uses it, so the HW mesh / vertex-pulling and SW paths see bit-identical
  clip positions.
- **Shadows.** The shadow vertex pulling (`vgeo_shadow.vert`) uses it.
- **Resolve.** `vg_resolve.glsl` re-skins the 3 vertices for the barycentrics (positions exactly as rasterised),
  skins the normals with each vertex's `mat3(skin matrix)`, and interpolates the previous palette's skinned
  positions with the same barycentrics into `v_prev_clip` (per-bone velocity). Static instances take the
  unchanged path.

### Tests (`ctest -R vgeo_skinned`, runs `--validation`)

The test mesh is the procedural creature: a body sphere plus 5 tapered tentacles of 6 bones (31 bones, 29k
triangles, 6 closed shells). It is built into 470 clusters over 11 levels, with up to 16 bones per cluster.
- **Builder.** `VGF_SKINNED` and the bone count; ≤ 16 bones per cluster; sorted bone sets; every weighted
  joint is in its cluster's set and its bind box contains the vertex; weights sum to 255; `NO_CONE`.
- **Animated bounds.** Over 600 ticks of the analytic animation (root motion, per-bone swing, pulsing scale),
  with both slot bounds, every skinned vertex of every cluster of every level must lie inside:
  - its bone-box sphere;
  - its transformed LOD and bind spheres;
  - the instance sphere.

  That is 24M vertex tests per bound: 0 outside.
- **Crack-free animated cuts.** 25 errors × 3 poses. The CPU cut plus the CPU skin, with edges keyed by the
  skinned float bits: no open edge. The topology is identical to the bind-pose cut (edges, pinched edges, Euler
  characteristic): 0 pinched edges, Euler 12 while the error is below the tentacles' size.
- **GPU cut vs the CPU reference.** 25 skinned creatures (one rotated and scaled 1.4), one on an empty slot, and
  40 static rocks; per-bone bounds on even slots, generic bounds on odd ones. For 3 poses × 4 cameras, LOD only
  and then with the frustum: the GPU set equals `vgeo_scene_cpu_select` (0 missing, 0 extra, 0 borderline). No
  cluster is lost inside or kept outside the frustum, and the empty slot draws nothing.
- **Raster.** On the animated herd at 1920×1080, 3 cameras:
  - all-HW vs all-SW: 10 / 21 / 16 pixels differ (≤ 0.012 %), all on triangle edges;
  - mixed vs HW: ≤ 2 pixels;
  - mesh shader vs vertex pulling: ≤ 1 pixel.
- **Skin helper.** `vg_test_skin.comp` runs the helper on 52k vertices of 4 instances. Against the CPU skin it is
  within 2.0e-6 m (current palette) and 2.1e-6 m (previous palette, motion up to 0.049 m); normals within
  1 − cos = 2.4e-7, raster clip within 2.4e-7.
- 0 validation messages. The run takes 812/812 checks, 2.4 s.

### Bench (`bench_vgeo --skinned 500`, RTX 3060 Ti, 1920×1080, medians of 200 frames)

The scene is 500 creatures (29k tris, 468 clusters, 31 bones each) on a 69 m square, one slot each, animated
every frame. It is compared with the same instances static. The mode column is the slot bound: per-bone or
generic, or the static instances.

| Camera | Mode | pal CPU ms | pal GPU ms | cull ms | HW ms | SW ms | GPU total | clusters | tris |
|---|---|---|---|---|---|---|---|---|---|
| in front | per-bone | 1.674 | 0.115 | 0.176 | 0.077 | 0.026 | 0.394 | 1776 | 171k |
| in front | generic | 1.900 | 0.115 | 0.267 | 0.116 | 0.042 | 0.539 | 2523 | 267k |
| in front | static | — | — | 0.133 | 0.067 | 0.061 | 0.262 | 1354 | 114k |
| inside | per-bone | 1.669 | 0.112 | 0.173 | 0.075 | 0.003 | 0.363 | 1047 | 123k |
| inside | static | — | — | 0.129 | 0.055 | 0.065 | 0.250 | 766 | 84k |
| above 25 m | per-bone | 1.643 | 0.111 | 0.157 | 0.030 | 0.030 | 0.328 | 1126 | 71k |
| above 25 m | static | — | — | 0.109 | 0.030 | 0.043 | 0.182 | 1041 | 57k |

- **pal CPU** is 500 `vg_palettes_set` (slot bounds included) plus the record, with the animation excluded:
  about 3.3 µs per slot.
- **pal GPU** is the upload copies.
- **Overhead.** A skinned horde costs about 1.4× the static GPU time with per-bone bounds. The extra
  triangles come from the conservative LOD spheres, which grow by the bones' displacement from the root.
  Vertex pulling measures within noise of the mesh shader.

### For the renderer glue (characters)

1. **Init.**
   - `vg_palettes_init(&pal, ctx, max_slots)`, then `vg_palettes_attach(&pal, scene)`.
   - Build each character mesh with `VgSource.joints` / `weights_u8`. `SkinVertex.joints` index the
     `SkeletalMesh`'s own bone list (`skm_gather_palette` order), so there are two options:
     - **Simplest:** one slot per part mesh, with the raw joints and the `skm_gather_palette` output
       (≤ 128 bones per mesh).
     - **One slot per character** (the plan): remap the joints through `SkmBone.skel` to the canonical
       skeleton and upload `si->skin[]`. Mesh-only leaf bones (`skel == -1`) then need indices of their own
       after the skeleton's (≤ 128 in all), filled with their `skm_gather_palette` matrices.
2. **Per character.**
   - Take a slot. For each part mesh, `vgeo_scene_add_skinned_instance(scene, mesh, pos, rot, 1, slot)`
     (simplest: identity at the origin, with the world-space palette).
   - `vgeo_bone_bounds` of each part, merged (`vgeo_bone_bounds_merge`), then
     `vg_palettes_set_bone_bounds(slot, merged)`.
   - Commit at load or spawn time, or keep a pool and use `vg_palettes_clear`.
3. **Per frame.** Before the vgeo cull: `vg_palettes_begin(frame)`, then `vg_palettes_set(slot, palette,
   ANIM_MAX_BONES or the bones used)` with the same `bone_world * inverse_bind` the legacy path uploads, then
   `vg_palettes_record(cmd)`.

Nothing else changes: the cull, raster, shadow and resolve passes read bindings 20 / 21 from the cull set the
renderer already binds as set 3.

### Simplifications (skinned)

- **The LOD spheres** use the root bone's transform plus a slot-wide deviation bound, not a per-cluster pose.
  This is conservative (finer near the camera) and consistent across the DAG. A per-group bone set would be
  tighter but needs more data.
- **No dual-quaternion skinning:** linear blend only (`r_vgeo_dqs` of the plan is not implemented).
- **One reference bone** (bone 0, the canonical root) for the sphere transform.
- **The bone-set penalty** is the lone-vertex lock on the source mesh plus the weight attributes. There is no
  collapse-cost scaling inside meshoptimizer.
- **The palette upload** waits on every earlier shader read (one barrier). With 3 regions per slot it could
  overlap the previous frame.

## Foliage support (Phase 7)

Foliage is ordinary vgeo content plus a few material, instance and placement features. Culling, LOD,
both rasterisers, the shadows and streaming are the core's. The content is the imported tree models
("Tree models") plus procedural grass tufts and wheat; procedural test types exercise the GPU paths.

### Materials (`VgMaterial`, 112 B)

| Field | Meaning |
|---|---|
| `flags` | `MAT_*` plus `VGM_MASKED`, `VGM_TWO_SIDED`, `VGM_DITHER_FADE`, `VGM_FOLIAGE` (bits 24-27) |
| `mask_tex`, `alpha_cutoff` | alpha mask (0: `albedo_tex`), its threshold |
| `wpo`, `wpo_params[4]`, `wpo_max` | snippet (`WPO_GRASS`, `WPO_TREE`, `\| WPO_DISPLACE`), parameters, declared bound (the commit computes it with `vgeo_wpo_max`) |
| `fade_start`, `fade_end` | distance fade of `VGM_FOLIAGE` instances (m) |
| `shadow_max_dist` | no sun-cascade shadow beyond this distance from the camera (m, 0 = always) |
| `mr_tex`, `normal_tex`, `occlusion_tex`, `emissive_tex`, `normal_scale`, `occlusion_strength`, `emissive_rgb` | glTF maps (docs/PBR.md): texture table indices (0 = none, with the `MAT_*_TEX` flags), the resolve shades them with the triangle's tangent frame |

At commit every mesh gets its foliage fields from its materials (`VgMesh` 80 B: `max_wpo`, fades, shadow distance,
`fflags`, impostor atlas). Model meshes take them from a `"foliage": {...}` block in `data/assets.json`
(`masked`, `two_sided`, `dither`, `alpha_cutoff`, `wpo`, `displace`, `wpo_params`, `fade`, `shadow_max_dist`)
and `"colliders": "capsule"`, `"capsule": [r, h]` on the model entry.

### Textures

One array of 1152 combined image samplers at binding 23 of the cull set (`vgeo_tex.h`). Entries 0-1023 are the
renderer's texture ids, 1024-1151 the module's own (masks, tree textures, impostor atlases, the displacement buffer).
Every cull kernel, both rasterisers, the shadow passes and the resolve can sample it. A change reaches a cull
set when that set is next recorded, so no descriptor changes under a frame in flight. The lit resolve's old
set 4 is gone.

### Masked / two-sided raster (the same pixels in SW and HW)

- Per triangle, `vg_uv_plane` (`vg_raster.glsl`) turns the clip positions and UVs into a rational UV function of
  the pixel: `uv(p) = (u·P, v·P) / (d·P)`. It also gives one mip LOD per triangle: the UV footprint of a pixel
  at the triangle's screen centroid.
- The mesh shader computes it from shared memory and passes it per primitive. Vertex pulling computes it in
  every corner and passes it flat. The SW variant computes it per triangle thread.
- Per pixel, `vg_mask_pass` evaluates the plane at the pixel centre and compares the mask's `textureLod`
  alpha with the cutoff. The fade dither compares the instance's fade alpha with an interleaved-gradient
  threshold, rotated per frame (TAA).
- Both rasterisers call the same functions on the same values, so their results differ only at triangle
  edges (test).
- Two-sided: no cone culling. The HW list is drawn up to three times:
  - plain clusters, back faces culled, Phase 3 shaders;
  - two-sided clusters, no culling, masked variant;
  - one-sided masked clusters, back faces culled, masked variant.

  Each draw skips the clusters of the other kinds (`vg_hw_kind`). The SW rasteriser keeps the back faces of
  two-sided triangles. The resolve flips the normal of back faces (screen-space winding).
- The SW range is dispatched once per variant: plain, masked, impostor. Each kernel takes its kind of entry, so
  the plain kernel keeps its registers and its Phase 3 speed.
- Masked clusters go SW only below `sw_px · sqrt(tris / 128) / 2` across. Their few, large card triangles
  rasterise better in HW.
- WPO materials use the vertex colour as wind weights: the resolve does not multiply it into the albedo.

### WPO (`shaders/vgeo/wpo.glsl`)

- Snippets (pure functions; CPU mirror `vgeo_wpo_eval`):
  - `WPO_GRASS`: bend along the wind by R, with the bent tip dropping.
  - `WPO_TREE`: trunk sway (R), branch bounce (G), leaf flutter (B), per-instance phase.
  - `WPO_DISPLACE`: adds the displacement buffer's push, weighted by R.
- Every snippet clamps the wind to the full wind it was declared for (`prm.w`). So `vgeo_wpo_max` bounds it for
  any weather:
  - grass: `√2 · bend`;
  - tree: `sway + branch + 1.5 · flutter`;
  - displacement: `+ 1.118 · VG_DISPLACE_MAX` (0.6 m).
- `vg_vertex_world` applies the snippet in every stage: both rasterisers, the shadows, the resolve, the test
  kernel.
- Every world sphere (`vg_world_sphere`) grows by the mesh's `max_wpo`: instance, BVH node, group and cluster
  LOD spheres, culling spheres, and the cell bounds. The LOD distance therefore shrinks by it too. The error
  itself is unchanged.
- The SW/HW size test and the impostor switch use the size without that margin.
- Velocity: the resolve evaluates the snippet with last frame's wind and time (`VgViewGpu.wind_prev`,
  `foliage.y`). The game feeds the views from `RenderWeather` (wind, gust, time). `FrameData.wx5 = (gust, gust
  phase, time, wind direction angle)`.

### Instances: density, fade, shadow distance, impostors

- The instance test (`vg_instance`, after the frustum):
  - **Density / fade:** `vg_foliage_alpha` keeps a `VGMF_FOLIAGE` instance while its fixed hash
    `h = (hash + 0.5) / 256 · 15/16 < r_foliage_density · fade(distance)`. The last 1/16 of that threshold
    is the dither band. The hash is byte 1 of `flags_phase`, set from the position bits by
    `vgeo_foliage_instance`, so it does not depend on load order.
  - **Shadow distance:** cascade views (cam_pos = the camera) drop instances past `shadow_max_dist`.
  - Instances culled these ways count in `VGN_INST_FADED`.
  - **Impostor:** `vg_use_impostor` sends the instance down the impostor path when its mesh has an atlas and its
    sphere projects below `r_vgeo_impostor_px` across. Never in cascades. The instance then appends one SW
    entry `(instance, VG_IMPOSTOR_ENTRY)` instead of expanding its BVH. That single comparison decides both
    sides, so there is no gap and no overlap.
- **All-foliage cells** (`VG_CELL_FOLIAGE` in the GPU count) are culled whole once their nearest point is beyond
  the largest foliage fade end (main view) or shadow distance (cascades).
- CPU mirrors: `vgeo_foliage_alpha`, `vgeo_use_impostor`. `vgeo_scene_cpu_select` applies all of it.

### Impostors (`vgeo_impostor.c`, `vg_impostor.glsl`)

- **Bake.** `vgeo_impostor_bake` (CPU, deterministic) renders the leaf clusters of a built mesh:
  - 8 × 8 hemi-octahedral directions;
  - orthographic over the mesh sphere, 2× supersampled, masked materials alpha-tested;
  - albedo (sRGB, coverage alpha) and mesh-space normals, empty texels dilated.

  The result goes in the `.vgeo` impostor section (`VGF_HAS_IMPOSTOR`): `build_vgeo --impostor [size=1024]
  [grid=8]`. `vgeo_foliage_impostor_upload` makes the two module textures (mips down to 4 px tiles).
- **Runtime.** A quad faces the camera at the mesh sphere centre.
  - **Raster:** SW impostor variant, 64 threads per triangle sharing its rows. Coverage is the 3 nearest frames'
    alpha blended ≥ 0.5, each frame's tile coordinates a UV plane of the triangle.
  - **Resolve:** the blended albedo and normals at the pixel's point on the quad.

### Placement chunks (`.vgp`) and the placement region

- **File.** `VgpHeader` (48 B: chunk x / z, counts, origin), mesh names (48 B each), ≤ 16 `VgCell`s (4 × 4 cells of
  8 m), then 32 B `VgInstance`s grouped by cell, with mesh = name index.
  - `vgp_write` bins a chunk's instances; `vgp_parse` validates.
  - `vgeo_foliage_chunk_load` resolves the names to scene meshes; unknown ones are dropped.
- **Region.** `vgeo_scene_reserve_placements(instances, cells)` puts a placement region after the committed
  instance and cell tables. The game reserves 1M instances (32 MB) and 8192 cells on first use.
  - `vgeo_scene_placement_add` sub-allocates a chunk: first-fit ranges, a free-cell stack. Bounds come from the
    mesh spheres + max WPO + positions.
  - `vgeo_scene_placement_remove` frees the cells first (no later frame sees the instances), then merges the
    instance range back into the sorted free list.
  - `vgeo_scene_placement_record` uploads the changed ranges and cells through a staging buffer per frame in
    flight. A barrier after every earlier read orders it after the frames in flight on the same queue.
  - A commit re-uploads the region whole (rebased past the static tables).
  - `vgeo_scene_placement_audit` checks that live chunks plus free lists cover the region exactly once.
- **Query.** `vgeo_foliage_query(pos, radius)` returns the placement instances within a horizontal radius,
  nearest first.

### Displacement buffer

- `VgDisplace`: a 128 × 128 RG16F image of 0.5 m texels around the camera, with horizontal pushes in metres.
  It is toroidal: texel `t` holds world texel `X ≡ t (mod 128)` of the window.
- `vg_displace_record`, per frame before the culls (`vg_displace.comp`):
  - clears texels whose world texel changed with the window;
  - decays (1.5 s time constant);
  - applies up to 64 queued pushes (`vg_displace_push(pos, radius, strength m/s)`, falling off to the radius);
  - clamps to `VG_DISPLACE_MAX`.
- `WPO_DISPLACE` samples it through the texture table.
- Game hooks: `render_foliage_push`. Every grounded player pushes at its feet. `foliage_push_test` circles
  pushes around the camera.

### In the game (`vk_vgeo.c`, `game_foliage.c`, `chunks.c`)

- **API (render.h):**
  - `render_foliage_test_types`: builds the procedural tuft (mask, 1024 px impostor) and loads the imported
    bushes and daisies ("Tree models").
  - `render_foliage_map_types`: the imported trees and the procedural wheat; `render_foliage_role_types`.
  - `render_foliage_test_chunk` (thread-safe .vgp of a chunk), `render_foliage_chunk_load` / `_unload`,
    `render_foliage_chunk_capsules`, `render_foliage_query`, `render_foliage_push`, `render_foliage_clear`.
- **Devroom.** `foliage_test_scatter [count=300000]` fills the foliage yard (x 60..380, z 80..400): 90 % tufts, the
  rest juniper / bamboo / acacia bushes and daisies, 121 chunks. The bushes' trunk capsules become one static triangle-mesh body per chunk: hexagonal
  prisms (`chunks_capsule_body`). A body per capsule would exhaust the physics world's 16k bodies.
  `foliage_test_clear` removes it all; a map change clears the placements.
- **Streamed terrain.** `world_foliage_test 1` makes the chunk jobs generate 1.5 instances per m² on the
  heightfield. Chunk finalize loads them and their capsules; chunk release frees both.
- **Occluders.** The Hi-Z of this frame (pass 2) merges the depth of the large static legacy draws
  (`vgeo_occluders`, before `vgeo_hiz_cur`). Before that change, walls only occluded in pass 1 and pass 2 brought
  everything behind them back.
- **Shadows.** Cascade views list masked clusters in their (otherwise unused) SW half. `vgeo_shadow_masked.vert` /
  `.frag` draws them alpha-tested, the HW half depth only. vgeo shadow cull and draw are timed:
  `vgeo_stats`, `foliage_stats`, `vgeo_bench`.
- **Fallback.** The 2015 / 2010 fallback and the local light shadows apply the same density / fade per
  instance and skip masked meshes (the legacy pipelines have no alpha test).
- **Commands:** `foliage_stats` (chunks, region, audit, last frame's counters), `foliage_query [r]`,
  `foliage_push_test [s]`, `foliage_check` (scripted, below).

### Tree models (`vgeo_tree.c`, `assets/treefbx`)

The game's trees, bushes and flowers are Tree It exports (CC BY 4.0, docs/CREDITS.md): FBX 6.1 / OBJ
meshes and PNG textures in `assets/treefbx`. There are no procedural trees any more; the procedural
bush is GPU test / bench content only (`vgeo_foliage`, `bench_vgeo`, `build_vgeo --gen bush`).

- **Import** (`vgeo_tree_import`, ufbx for both formats). Every triangle of every mesh node; one material
  per texture (the FBX model name or the OBJ `usemtl` minus Tree It's `_N` suffix, + `.png`).
  - A material whose texture cuts out (> 2 % of the texels below half alpha) is a masked, two-sided card
    with the albedo alpha as the mask (cutoff 0.5). Bark, stumps and stems are opaque.
  - The files are centimetres, Y up, ground at y = 0. The mesh is scaled to the species height and
    re-centred on its trunk base (the bark between 0 and 1.5 m).
  - Tree It card normals point up / outwards whatever the winding (and are not unit length). Each card
    triangle is re-wound towards its normal, so the front face is the lit side (the resolve flips only
    back faces). Without this, half of the leaves turned black.
  - Wind weights in the vertex colour: R = (y / H)^1.5 (trunk sway), G = (r / crown radius)^1.3,
    faded in over the lowest quarter of the height (branch bounce), B = 1 on card-only vertices (flutter).
    Shared positions get the same R / G, so bark and cards bend together.
  - Trunk capsule: the 25th percentile of the bark's distance from the axis between 0.4 and 1.6 m
    (scaled down for bushes), + 10 %; height min(H / 2, 5 m).
- **Build.** The ordinary vgeo builder: its DAG is the LOD chain (the files' own `_LODn` meshes are not
  used). Card-only groups do not coarsen (see "Test content"), so a tree's coarsest cut stays at about
  30-90 % of its triangles. Hence the impostors.
- **Impostors.** 512 px atlas, 8 × 8 frames, baked with the textures. Trees switch at
  `impostor_scale` × `r_vgeo_impostor_px`: 6× (96 px across, about 120 m for a 17 m tree) for the
  leafy trees, 2× for bushes, 1× for the bare old tree (its twigs thin out in a 64 px frame).
  The per-mesh scale lives in the high 16 bits of `VgMesh.impostor_grid` (`vgeo_impostor_scale`, 1/16
  steps, 0 = 1×), mirrored by `vgeo_use_impostor`.
- **Textures.** Albedo only (RGBA8, sRGB). Normal, roughness and transmission maps are not used: the
  vgeo vertex format has no tangents. Loaded with stb_image and box-filtered (alpha-weighted) to 512 px.
  Masked textures get coverage-preserving mips (`vgeo_tree_mip_coverage`: each level's alpha is scaled so
  the share of texels above the cutoff matches level 0); with plain box mips, cards thinned out and
  distant crowns turned into bare sticks. Shared by file across species (27 textures). They are module
  textures, so `VG_TEX_OWN` is now 128 (the table holds 1152 entries).
- **Cache.** `build_vgeo --trees <dir/> <out/>` builds every species in parallel at build time (CMake
  `dg_vgeo_content`, about 1.4 s) into `bin/assets/vgeo/trees/<name>.dgtree`: a `VgTreeCacheHeader`
  (key, materials, masks, capsule, scale) + the `.vgeo` bytes. The key hashes the source file, the
  species height and atlas size, `VG_TREE_VERSION` and `VGEO_BUILD_VERSION`. When a file is missing or
  stale the game builds the species on the job system (about 1.1 s for all) and writes
  `assets/cache/<name>.tree.<key>.dgc`. Texture edits do not change the key: bump `VG_TREE_VERSION`.
- **Registration** (`foliage_build_trees` in `vk_vgeo.c`): `render_foliage_test_types` loads the bushes
  and flowers, `render_foliage_map_types` the trees. Each species is a foliage type with a role
  (`render_foliage_role_types(role, names, weights, max)`), in table order on every machine.

| Species (type) | File | Role | Height | Triangles (leaf / coarsest cut) | Materials |
|---|---|---|---|---|---|
| `tree_pine` | pine.fbx | conifer | 17 m | 7 027 / 1 628 | bark09, stump01, pine branch (masked) |
| `tree_fir` | fir.fbx | conifer | 15 m | 4 973 / 3 925 | bark07, fir branch |
| `tree_spruce` | spruce.fbx | conifer | 18 m | 9 010 / 5 642 | bark08, spruce branch, branch06 |
| `tree_oak` | oak.fbx | broadleaf | 16 m | 8 990 / 6 072 | mossybark02, stump03, oak leaf |
| `tree_beech` | beech.fbx | broadleaf | 17 m | 10 584 / 9 755 | bark01, beech leaf |
| `tree_birch` | birch.fbx | broadleaf (0.8) | 14 m | 6 944 / 2 579 | bark11, birch leaf |
| `tree_elm` | elm.fbx | broadleaf (0.8) | 15 m | 4 013 / 1 508 | bark07, elm leaf |
| `tree_maple` | maple.fbx | broadleaf | 14 m | 17 962 / 4 043 | bark02, leaf maple |
| `tree_old` | old.fbx | dead | 11 m | 5 873 / 2 606 | bark04, stump01, branch06 (a rooted snag with a few twig cards) |
| `bush_juniper` | juniper.fbx | bush | 3.2 m | 2 906 / 1 944 | bark05, juniper branch |
| `bush_bamboo` | bamboo.obj | bush (0.4) | 3.6 m | 3 324 / 827 | plant02, bamboo branch |
| `bush_acacia` | tree1.obj | bush (0.4) | 4.5 m | 8 957 / 4 082 | bark05, acacia branch |
| `flower_daisy` | daisy.fbx | flower | 0.7 m | 632 / 632 | plant05, flower daisy, daisy leaf (no impostor) |

Wind (m at full wind, sway / branch / flutter): conifers 0.18-0.2 / 0.12 / 0.02, broadleaves
0.18-0.26 / 0.16-0.18 / 0.04-0.05, the old tree 0.08 / 0.05 / 0.02, bushes 0.05-0.08. Trees fade at
850-1100 m with shadows to 70 m; bushes 150-210 m / 40 m; daisies 50-80 m / 15 m.

**Budget.** 13 species, 91k source triangles, 2.2k clusters; .dgtree files 40 MB on disk (mostly the
impostor atlases and the pages). GPU: 27 textures at ≤ 512 px with mips ≈ 35 MB, 12 impostor atlas
pairs ≈ 34 MB, cluster pages a few MB.

### Test content (`vgeo_procgen_foliage.c`)

- **Tuft:** 5 crossed cards, 60 triangles. Grass masks of 14 tapered, noisy blades. Masked, two-sided,
  `WPO_GRASS | WPO_DISPLACE`. Fade 40-65 m, shadows to 15 m.
- **Bush:** a 48-triangle tapered trunk plus 46 leaf cards, 600 triangles. Leaf-blob masks. `WPO_TREE`. Fade
  100-150 m, shadows to 40 m, trunk capsule.
- Vertex colours hold the wind weights. `build_vgeo --gen tuft|bush [--impostor]`.
- The builder now carries a material subset that simplifies to nothing (tiny disconnected cards) through
  unchanged, instead of meshletizing zero triangles. Earlier outputs are byte-identical.

### Tests (`ctest -R vgeo_foliage`, runs `--validation`; 495 checks)

- **Masked visibility.** 2880 masked, two-sided tufts and bushes in wind, 3 cameras at 1920 × 1080:
  - all-HW vs all-SW: 1 / 2 / 11 differing pixels out of 1.3-2.0M covered, all on triangle edges;
  - mesh shader vs vertex pulling: 0 / 0 / 0;
  - **CPU reference** (sample pixels, no wind; the bush leaves still flutter, so the CPU applies the snippet
    mirror; borderline ±0.06): 52k-78k masked winners, 0 failing the CPU alpha test; 32k-78k pixels whose
    unmasked winner fails it, 0 still showing it; masking never brings a surface nearer.
- **WPO bounds.** All vertices of all clusters of all levels of 8 instances, 96 states (max wind, 2 × max wind,
  max wind + a saturated displacement buffer), 467 712 samples: 0 outside their cluster's or instance's
  culling sphere (worst 6.4 cm inside). GPU snippet vs CPU mirror: 1.2e-6 m.
- **Density hash.** 60k instances, LOD only, the GPU kept set equals the CPU mirror at densities 0.7 / 0.4 / 1.0
  (11 540 / 6 629 / 15 778, 0 missing, 0 extra) and is monotonic (0.4 ⊂ 0.7 ⊂ 1.0). A static camera over 600
  frames (alternating frame slots) keeps the identical set and counters every frame.
- **Cell culling vs brute force.** 69k instances, 4 cameras: GPU visible instances = brute-force frustum + fade
  count within the borderline (±2), 0 drawn instances failing the brute force test.
- **Impostor switch.** Bushes on a line, 21 of them within ±1e-5 of the switch distance. Every instance draws
  its impostor xor its DAG cut (0 both, 0 neither), exactly where `vgeo_use_impostor` says (except 3 within
  f32 noise). Impostor quads rasterise (2650 px).
- **Placement streaming.** A 2135 m winding flythrough, 7 × 7 chunks loaded around it: 688 loads / 639 unloads
  through `.vgp` write / parse / load, up to 50k live instances.
  - Allocator audit every step: 0 failures.
  - 27 GPU checks of the tables against the CPU mirror and of the culled instance counts: 0 mismatches.
  - At the end: 1 free range of the whole region, all 4096 cells free, 0 live chunks.
- **Hi-Z.** A wall 3 m in front of the camera: 99.3 % of the 5277 in-frustum instances rejected.
- 0 validation messages.

In game (devroom, `--validation`: 0 messages):
- `foliage_check` behind the foliage yard wall: 11 790 foliage instances in the frustum, 1 visible: **99.99 %
  rejected**. The pass-1 Hi-Z alone did this before the occluder merge; pass 2 brought them back.
- `foliage_check` density, occlusion off, 600 frames of a static camera: 0 frames differ.
- **Streaming:** `map streaming` with `world_foliage_test 1`, a 2080 m noclip flight: 77 chunks / 118k instances
  live, audit PASS; after `map devroom`: 0 chunks, 1 free range. The 21 validation messages of that run are
  `map streaming`'s own: the base commit shows the same ones.

### Bench (300k instances, RTX 3060 Ti, 1920 × 1080; the GPU was shared with other jobs during the run, so these
are the least-contended of several runs)

`bench_vgeo --foliage 300000`: main view in the renderer's order (pass 1 cull, raster, Hi-Z from the visibility buffer,
pass 2) plus the 3 cascade culls, medians of 60 frames:

| Camera | cull | raster HW / SW | Hi-Z | shadow cull | total | visible inst. | impostors | clusters HW / SW | tris |
|---|---|---|---|---|---|---|---|---|---|
| eye level, in the yard | 0.231 | 1.547 / 0.546 | 0.059 | 0.210 | **2.593** | 10 823 | 2 192 | 10 182 / 5 484 | 0.77M |
| eye level, at the edge | 0.207 | 1.349 / 0.350 | 0.064 | 0.188 | **2.296** | 7 909 | 1 471 | 7 199 / 3 303 | 0.53M |
| elevated 15 m | 0.205 | 0.654 / 0.757 | 0.053 | 0.167 | **2.205** | 9 277 | 2 402 | 8 605 / 8 367 | 0.76M |
| in the grass, looking down | 0.210 | 0.990 / 0.291 | 0.065 | 0.196 | **1.970** | 9 731 | 1 817 | 7 588 / 2 127 | 0.48M |

In game, `vgeo_bench 600 f.csv foliage` over the yard (frame-graph passes + the timed vgeo shadows, averages):
- `vgeo_cull` 0.25, `vgeo_raster` 1.52, `vgeo_occluders` 0.04, `vgeo_hiz_cur` 0.09, `vgeo_pass2` 0.21, `vgeo_hiz`
  0.04;
- shadows: cascade cull 0.58, cascade draws 0.50;
- about **3.2 ms** in all against the **2.5 ms** budget, before the shared prepass / opaque resolve.

These yard figures predate the tree models: the yard then held the 600-triangle procedural bush. Its 10 %
of bushes are now juniper / bamboo / acacia models and daisies (thousands of triangles each), so the yard
draws about 4M triangles at eye level and has not been re-benched.

The harness total (main view + cascade culls) is 2.0-2.6 ms; adding the cascade draws puts every camera over
the budget, by up to about 0.6-1 ms. What it costs:
- the near grass's masked overdraw in HW (fill: each tuft is 5 cards of mostly transparent texels);
- tufts being a single 60-triangle cluster, which the DAG cannot coarsen, so a tuft is 60 triangles until it
  turns into an impostor;
- the vertex-pulled masked shadow draws.

How it got there, from 6-10 ms:
- splitting the SW kernel into variants;
- the HW plain / masked split;
- the per-triangle UV plane and LOD;
- sending masked cards to HW sooner;
- whole-cell fade culling;
- dropping the visibility-buffer pre-read;
- the content's fade / shadow distances (grass 40-65 m / 15 m).

### Simplifications (foliage)

- **Mask LOD per triangle** (at its centroid), not per pixel. Identical in both rasterisers by construction.
- **Impostor depth is the quad's plane.** No per-pixel depth offset. Impostors cast no shadows: cascades never
  switch.
- **Displacement velocity** uses this frame's buffer for the previous position too.
- **WPO and the LOD error.** The WPO bound grows the spheres. The simplification error is not increased.
- **Shadow distance and fades are per mesh** (the max over its materials), not per cluster.
- **Fallback.** No masked foliage in the 2015 / 2010 fallback or in local light shadows.
- **Capsules** are hexagonal prisms in one static mesh per chunk, not capsule shapes.
- **Displacement format.** RG16F needs `shaderStorageImageExtendedFormats` (the game warns if missing).

## Simplifications vs Nanite

- **Partitioner:** a greedy, shared-edge-weighted grower over a Morton order instead of METIS graph partitioning.
  Groups are slightly less compact, so a few more boundary vertices end up locked. Reduction is still ~50 % per
  level on all test meshes, with 0 unreducible groups.
- **One grid per mesh** for positions (u16 offsets from an i32 cluster base). Nanite uses per-cluster bit widths,
  which compress better; ours is simpler and crack-free by construction.
- **No per-cluster bit packing** of attributes: fixed 12 B vertices (u16×3 position, oct normal 8+8, half2 UV).
- **Pinched edges:** `meshopt_simplify` can, near the root, collapse two sheets onto one edge (used 4 times, still
  closed). Nanite's own simplifier has the same class of artefact. The tests allow it only at coarse cuts (≤ 0.1 %
  of edges).
- **Locking:** whole shared positions are locked, including material seams inside a group, instead of only the
  group's outer boundary edges. This is simpler and slightly more conservative.
- **Discrete LOD export** for the 2015 / 2010 era presets: plain cuts of the DAG at fixed errors (Nanite has no
  equivalent; its fallback mesh is a single coarse LOD).
- **Level-synchronous traversal** instead of persistent threads: one indirect dispatch per BVH depth. It is
  deterministic (the CPU-reference test depends on it) and assumes no forward progress between workgroups.
- **8 m cell grid** instead of an instance BVH as the first culling level.
- **Groups of ≤ 16 clusters** are assumed by the cluster candidate encoding (`group << 4 | k`); larger groups
  are warned about at registration.
- **Streaming:** whole 128 KiB pages are the only unit (no partial pages or separate fixup data), priorities are
  the projected error of the requesting group (no distance or screen-area weighting), usage is stamped per group
  emitted at a leaf (not per rendered cluster), and there is no eviction hysteresis.

## Adding a foliage system on top

The vgeo core is built so that a foliage system is a content and placement layer, not a renderer fork. Phase 7
landed the support pieces ("Foliage support"); a foliage system then needs only:

1. **Meshes.** Author or generate grass, bush and tree meshes as normal glTF with `"vgeo": true` and a
   `"foliage": { "masked": true, "two_sided": true, "wpo": "grass" | "tree", "wpo_params": [...], "fade": [s, e],
   "shadow_max_dist": d }` block in `data/assets.json`. Bake wind weights into vertex colour: R = trunk / bend,
   G = branch, B = leaf flutter (`VgSource.color`, 4 extra bytes per vertex; WPO materials do not use it as
   albedo). For distant instances, `build_vgeo --impostor` bakes the hemi-octahedral atlas into the `.vgeo`,
   `vgeo_foliage_impostor_upload` puts it on the GPU, and `r_vgeo_impostor_px` sets the switch.
2. **Placement.** Write per-chunk placement files (`.vgp`, `vgp_write`): ≤ 16 cells of 8 m per 32 m chunk, 32 B
   `VgInstance`s made with `vgeo_foliage_instance` (rotation, scale, and wind phase + density hash fixed by the
   position). Generate them from the heightfield and biome masks with your own scatter rules (slope, altitude,
   distance to roads; `vgeo_foliage_scatter_chunk` is the test example). Load and free them with the chunk
   (`render_foliage_chunk_load` / `_unload`; `chunks.c` does it for `world_foliage_test`). Cells are the first
   culling level, so a chunk adds no per-instance CPU work.
3. **Materials.** Pick a WPO snippet per material (`WPO_GRASS`, `WPO_TREE`, `| WPO_DISPLACE` for trampling) and
   its parameters, with `prm.w` the full wind. The declared bound (`vgeo_wpo_max`) inflates every culling
   bound automatically. New snippets go in `wpo.glsl` with their bound in `vgeo_wpo_max` and the mirror in
   `vgeo_wpo_eval` (the `vgeo_foliage` WPO test checks the bound).
4. **Era scaling.** `r_foliage_density` (set by `r_era`: 1.0 / 0.7 / 0.4 / 0.2) thins instances by their fixed
   hash, so the thinning is stable and monotonic with distance. Per-material `fade_start` / `fade_end` thin and
   dither out distant instances, all-foliage cells past the largest fade end are skipped whole, and
   `shadow_max_dist` caps their shadow cost.
5. **Gameplay hooks.**
   - `render_foliage_query(pos, radius, out[])` serves footstep surfaces and crushing / burning.
   - `render_foliage_push(pos, radius, strength)` writes the displacement buffer: players already do; vehicles
     and explosions can.
   - Trunks get static colliders with `"colliders": "capsule"` (`render_foliage_chunk_capsules`,
     `chunks_capsule_body`).

What stays out of the foliage system:
- culling;
- LOD selection;
- rasterisation;
- shadows;
- streaming.

These all come from the vgeo core unchanged.
