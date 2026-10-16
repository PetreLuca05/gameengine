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
| 3. Visibility buffer raster + resolve | — |
| 4. Streaming | **done** (module + tests; renderer glue pending) |
| 5. Integration, `r_era`, test scene, `bench_vgeo` | — |
| 6. Skinned | — |
| 7. Foliage support | — |

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
| `tools/bench_vgeo/bench_vgeo.c` | GPU timings of the cull passes and the Hi-Z build |

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

- MAX (farthest) R32F pyramid, depth 0 = near, at **half resolution**: mip 0 is the max of each 2×2 depth
  block, each next mip the 2×2 max of the previous one. The last row / column of a mip also folds in the odd
  tail, so texture mip `j` bounds every depth pixel of its 2^(j+1) footprint. `vg_hiz_record`: one 8×8 dispatch
  per mip.
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

- Frame graph: `vgeo_cull` before the prepass (main view, slot 0, reads the `vgeo_hiz` history), `vgeo_hiz` after
  the prepass (MAX Hi-Z of this frame's prepass depth, `FG_SIZE_HALF`, full chain; next frame's history). The
  prepass depth holds the legacy geometry only: vgeo is not rasterised until Phase 3.
- **One pass only until Phase 3:** pass 2 needs this frame's Hi-Z from the visibility buffer. `r_vgeo_occlusion
  2` behaves as 1 until then.
- Active when `r_vgeo` is on and the scene has instances.
- Cvars:
  - `r_vgeo` (1).
  - `r_vgeo_error_px` (1.0).
  - `r_vgeo_shadow_error_px` (2.0, Phase 3).
  - `r_vgeo_sw_px` (32).
  - `r_vgeo_occlusion` (0 off, 1 last frame's Hi-Z, 2 two-pass; default 2).
  - `r_vgeo_freeze` (keeps the last view constants).
  - `r_vgeo_debug` (registered; the views come with Phase 3).
  - `r_vgeo_debug_spheres`: culling spheres of up to 4096 HW (green) + 4096 SW (cyan) visible clusters, read
    back one frame slot later.
- Commands:
  - `vgeo_stats`: scene, last frame's counters, `vgeo_*` pass ms.
  - `vgeo_test_scene [count=20000] [spacing=6] [meshes=4]`: procedural rocks scattered around the camera.
  - `vgeo_clear`: removes every instance.
- Registering `Model.vgeo` meshes and level / chunk placements comes with Phase 5.

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
lands the support pieces described in `VGEO_PLAN.md` §8; a foliage system then needs only:

1. **Meshes.** Author or generate grass, bush and tree meshes as normal glTF with `"vgeo": true` and the material
   flags `masked` / `two_sided`. Bake wind weights into vertex colour: R = trunk / bend, G = branch, B = leaf
   flutter. The builder carries vertex colour when present (`VgSource.color`, 4 extra bytes per vertex). For far
   trees, `build_vgeo --impostor` bakes an octahedral atlas.
2. **Placement.** Write per-chunk placement files (`.vgp`): 16 cells of 8 m per 32 m chunk, each a list of 32 B
   `VgInstance`s with a per-instance wind phase and density hash. Generate them from the heightfield and biome
   masks with your own scatter rules (slope, altitude, distance to roads). The chunk job loads and frees them with
   the heightfield; cells are the first culling level, so a chunk adds no per-instance CPU work.
3. **Materials.** Pick a WPO snippet per material (`WPO_GRASS`, `WPO_TREE`, and `WPO_DISPLACE` for trampling) and
   its parameters. The snippet's declared max displacement inflates every culling bound and the LOD error
   automatically.
4. **Era scaling.** `r_foliage_density` (set by `r_era`: 1.0 / 0.7 / 0.4 / 0.2) thins instances by their fixed
   hash, so the thinning is stable and monotonic with distance. Per-material `fade_start` / `fade_end` dither out
   distant instances, and `shadow_max_dist` caps their shadow cost.
5. **Gameplay hooks.**
   - `foliage_query(pos, radius, out[])` serves footstep surfaces and crushing / burning.
   - `foliage_push(pos, radius, strength)` writes the displacement buffer: players, vehicles and explosions
     flatten grass.
   - Trunks get physics capsules with `"colliders": "capsule"`.

What stays out of the foliage system:
- culling;
- LOD selection;
- rasterisation;
- shadows;
- streaming.

These all come from the vgeo core unchanged.
