# Virtualized geometry (vgeo): plan and data layout

Status: **Phases 1-2 are implemented**; `docs/VGEO.md` documents the built system, and *As built* notes below
mark where it deviates from this plan. Reference design: Karis et al., "A Deep Dive into
Nanite Virtualized Geometry" (SIGGRAPH 2021), and the UE 5.5+ Nanite skinning and foliage designs. `docs/VGEO.md`
also lists every deliberate simplification.

## 0. Where the engine is today (what the plan is built on)

| Area | Today | Consequence for vgeo |
|---|---|---|
| Frame | Hand-written shadow pass, then the frame graph: `prepass` (depth + normal / material / velocity G-buffer) → screen-space passes (GTAO, SSR, SSGI, contact, froxels) → **`opaque`, a forward lit pass that re-draws every object** → sky, transparents, post (render.c:1346-1498). | The G-buffer has no albedo and lighting is forward. Vgeo pixels need their own **lit resolve**: a full-screen deferred shade of vgeo pixels only, using the same `lit_body.glsl` lighting code. Everything after `prepass` stays unchanged. |
| Static draws | One `vkCmdDrawIndexed` per object, push-constant materials, a set-1 texture rebound per draw, CPU sphere culling. Every mesh has its own VB/IB. | Vgeo adds a material table SSBO plus a bindless texture array. The legacy path is untouched. |
| Device features | Vulkan 1.3: only dynamic rendering, sync2 and demote. Nothing from Vulkan 1.2 is enabled, and neither are int64 or multi-draw-indirect. | Enable, when supported: `shaderInt64`, `shaderBufferInt64Atomics`, `drawIndirectCount`, `multiDrawIndirect`, `drawIndirectFirstInstance`, descriptor indexing (partially-bound, runtime arrays) and optionally `VK_EXT_mesh_shader`. **If buffer int64 atomics are missing, vgeo is off** and the era fallback (discrete DAG LODs) is used. |
| Hi-Z | The `hiz` pass is a MIN (closest) pyramid for SSR, built only when SSR is on. Depth is standard (0 near, clear 1, `LESS_OR_EQUAL`). | Occlusion needs its own **MAX (farthest) pyramid**, built from the visibility buffer and kept as a frame-graph history. |
| Frame graph | Texture resources only, 96 passes. Shadows and occlusion are outside the graph and not timed. | Buffers get manual barriers inside the exec callbacks. Vgeo passes go through `fg_pass` so they get timestamps. Shadow vgeo work gets its own timestamp pair. |
| Skinning | One shared canonical skeleton (`ANIM_MAX_BONES 128`, about 65 used). The CPU builds `palette = bone_world * inv_bind` and uploads it **once per draw**, 2–3 copies per character (main, fade, shadow). Previous palettes are appended to the same SSBO, with an `ObjSlot` hash keyed by `object_id`. `SkinVertex` joints index the canonical skeleton directly. **There are no modular parts:** one whole-body mesh per skin. | Phase 6 adds a **per-character palette slot**, uploaded once and referenced by legacy draws and vgeo instances alike, with the previous-frame slot kept beside it. A "part" is simply another vgeo skinned instance on the same slot, so modular parts work when they arrive. No new part system is invented. |
| Character LOD | 4 discrete mesh LODs (pixel thresholds 260/120/50, cross-fade), animation update interval per LOD, ragdolls at LOD ≤ 2, hitboxes from the sim pose. | Modern/2020 replace only the **mesh** LOD with the DAG cut. Animation, IK and ragdoll tiers stay as they are. |
| Static content | Level brushes are merged into ≤ 32 buckets (identity transforms). glTF props are dynamic entities. **Chunks hold only a heightfield** plus 4 terrain LODs and one heightfield body. There is no static-mesh cache. | Vgeo adds a **static instance registry** (level + chunk placements → GPU buffer). glTF models opt in with `"vgeo": true` in `data/assets.json` and get a `.vgeo` cache next to the existing `.dgc` files (same FNV key and versioned-header convention). |
| Wind | `wx2 = (wind.x, wind.z, |wind|, …)`. Gust is folded into strength on the CPU. | Add a `wx5 = (gust, gust phase, time, wind dir angle)` vec4 to `FrameData` for WPO. |
| Presets | **No era presets exist.** There are only post Low/Medium/High/Ultra plus separate quality cvars. | Add `r_era` (`modern`, `2020`, `2015`, `2010`) as a new preset that sets vgeo / foliage cvars (see §6). It does not replace `r_post_preset`. |
| meshoptimizer | v0.22, with only `simplifier.cpp` and `allocator.cpp` built. | Add `clusterizer.cpp` from the **same v0.22 tag**, so existing character-LOD output cannot change. The group partitioner is our own greedy one (no METIS dependency). |
| Test hardware | The container has lavapipe (CPU Vulkan 1.4): int64 buffer/image atomics, `VK_EXT_mesh_shader`, `drawIndirectCount` and validation layers. The whole game runs headless under Xvfb with 0 validation messages. | Every correctness test runs here on a real Vulkan implementation, both raster paths included. **GPU timings cannot be measured here.** `bench_vgeo` prints per-pass ms and must be run on your AMD/NVIDIA machine to sign off budgets. Here I gate on hardware-independent counters instead (triangles, clusters, pages, instances surviving). |

## 1. Data layout

All GPU structs are std430, mirrored in `src/render/vgeo/vgeo_gpu.h` and `shaders/vgeo/vgeo_common.glsl`, with
`_Static_assert` on every size.

### 1.1 `.vgeo` file (`assets/cache/<stem>.vgeo.<key %08x>.dgc`)

```
VgFileHeader (192 B) { u32 magic 'VGEO', version, flags (SKINNED | FOLIAGE | HAS_COLOR | HAS_IMPOSTOR), key;
                       u32 cluster_count, group_count, node_count, page_count, root_page_count, lod_count, material_count;
                       u32 bone_count; f32 sphere[4]; f32 aabb_min[3], q_step; f32 aabb_max[3]; u32 level_count;
                       u32 root_node, pad[3];
                       section table: { u32 offset, size } x VGS_COUNT (10) }   // 112 + 80
sections are 16-byte aligned, in this order:
  [hierarchy]  VgNode[node_count]       always resident (CPU + GPU)
  [groups]     VgGroup[group_count]     always resident
  [pages]      VgPageDesc[page_count]   file offset, size, first group, dependency
  [materials]  VgMaterialRef[]          name + slot (resolved against the material table at load)
  [lods]       VgDiscreteLod[4]         exported discrete LODs for the 2015/2010 eras
  [lod idx]    u32[]                    their index buffers
  [lod verts]  VgLodVertex[]            their vertices (pos, normal, uv as f32: CPU-side, uploaded as-is)
  [impostor]   optional octahedral atlas (Phase 7)
  [page data]  pages of <= 128 KiB stored compactly (a page fills one 128 KiB pool slot at load), DAG order
               coarse → fine; the root pages are a prefix
  [stats]      build report (per level: clusters, triangles, error min/max); the per-stage ms and the thread
               count are zeroed in the file so the bytes depend only on the input
```

The key is FNV-1a over the source file, `VGEO_BUILD_VERSION` and the build options (same scheme as `anim_cache.c`).
The build is deterministic: there is no hash-map iteration order anywhere, all sorts are stable, and per-group jobs
run in parallel with their results written in a fixed order. The test checks that the bytes are identical across
runs and across thread counts.

### 1.2 Resident structures (GPU, per mesh, uploaded at load)

```c
typedef struct VgNode {          // 32 B: BVH over groups (4-wide), built per LOD level then merged (Nanite-style)
    vec3 center; f32 radius;     // culling sphere (mesh space)
    f32 max_parent_error;        // max over the subtree: node skipped if this <= threshold
    u32 first;                   // first child node, or first group if leaf
    u16 count; u16 flags;        // child count; LEAF
    u32 pad;
} VgNode;

typedef struct VgGroup {         // 32 B
    vec3 lod_center; f32 lod_radius;   // sphere enclosing every child cluster's lod sphere (monotonic)
    f32 parent_error;            // error of simplifying this group (>= every member error, enforced)
    u32 page;                    // all clusters of a group live in one page
    u16 first_cluster, cluster_count;  // index within the page
    u32 level;                   // DAG level (0 = leaves; debug views, stats)
} VgGroup;

typedef struct VgMesh {          // 64 B: the mesh table, one entry per loaded vgeo mesh
    vec3 center; f32 radius;
    u32 root_node, node_base, group_base, page_base;   // offsets into the global tables
    u32 page_count, flags, material_base, impostor;    // impostor: atlas id or ~0
    f32 max_wpo;                 // m: max world-position-offset over its materials (0 = static)
    f32 q_step;                  // position grid step (m), one grid per mesh (see VgCluster)
    f32 pad[2];                  // (skinned bone-scale bounds live in the palette slot table, §1.4)
} VgMesh;
```

### 1.3 Page payload (streamed; a page is one 128 KiB slot of the GPU page pool)

```c
typedef struct VgPageHeader { u32 cluster_count, vertex_bytes, index_bytes, skin_bytes; } VgPageHeader;  // 16 B

typedef struct VgCluster {       // 64 B
    vec3 center; f32 radius;     // culling bounds (mesh space; skinned: bind pose, unused at runtime)
    vec3 lod_center; f32 lod_radius;   // sphere of the group this cluster was generated FROM
    f32 error;                   // that group's simplification error (0 at leaves)
    u32 cone;                    // oct axis 2 x 8 bit | cutoff snorm8 | flags 8 (TWO_SIDED, NO_CONE)
    u32 counts;                  // verts 8 | tris 8 | material 16
    u32 offsets;                 // vertex data offset / 4 (15 bits) | triangle data offset / 4 (15 bits) << 15
    u32 child_group;             // group this cluster was simplified from (~0 = leaf)
    i32 base[3];                 // grid base: pos = (base + u16x3 offset) * VgMesh.q_step
} VgCluster;                     // parent error / sphere come from VgGroup (the group the cluster belongs to)
```

*As built (Phase 1):* the first draft of this struct (per-cluster `q_origin` / `q_step`, separate offsets,
`skin_offset`) was 84 B and, worse, per-cluster float quantisation decodes a vertex shared by two clusters to
different bits, which cracks the LOD boundaries the DAG is built to keep. Positions are instead on **one integer
grid per mesh** (`q_step = extent / 65000`): the builder snaps every input vertex to it before welding, a cluster
stores its integer `base`, each vertex a u16 offset, so shared vertices are bit-identical everywhere. The two data
offsets are page-relative and pack into one word (a page is ≤ 128 KiB). The skinned bone-set block follows the
cluster's triangle data, so it needs no offset of its own (§7).

- **Vertex: 12 B static** = u16×3 position, oct-encoded normal 8+8 bits, half2 UV. **+4 B** with vertex colour
  (wind weights). **+8 B skinned** = u8×4 joints (canonical skeleton index, max 128 bones) and u8×4 weights (unorm,
  sum 255).
- **Triangle: 4 B** = 3× u8 local indices plus 8 spare bits, used for the edge flags of the SW rasteriser's
  top-left rule.
- **Skinned cluster bone set:** `u8 count`, then per bone 16 B = `{ u8 bone; u8 pad; half3 min; half3 max }`, a
  bind-space AABB of the cluster vertices with a non-zero weight on that bone. Capped at 16 bones per cluster: the
  builder splits clusters that exceed the cap.
- A typical cluster (128 tris, about 80 verts) is about 1.6 KiB static or 2.3 KiB skinned, so a 128 KiB page holds
  about 60–80 clusters.

### 1.4 Instances

```c
typedef struct VgInstance {      // 32 B, the only instance format (props, chunk placements, foliage)
    vec3 pos;                    // world (f32: no visible error at the 4 km floor)
    u32 rot;                     // quaternion, smallest-three 10:10:10:2
    u16 scale_h;                 // half uniform scale
    u16 mesh;                    // index into the VgMesh table (65k meshes)
    u32 flags_phase;             // phase u8 (wind) | era/density hash u8 | flags u16 (XFORM, SKINNED, CAST_SHADOW, …)
    u32 extra;                   // XFORM: index into VgXform[] (non-uniform mat3x4); SKINNED: palette slot
    u32 cell;                    // owning placement cell (hierarchical culling + unload)
} VgInstance;
```

Placement cells are 8 m, so a 32 m chunk holds 4×4 cells. Each cell is `{ AABB (incl. max WPO); u32 first, count }`
(32 B). Cells are the top level of hierarchical instance culling (§3).

Skinned instances use `extra` = palette slot. The slot table holds `{ u32 cur_first_bone, prev_first_bone,
bone_count, max_scale_f32 }`. Previous-frame palettes are the existing `prev_bones` copy, addressed through the slot.

### 1.5 Material table (resident, `VgMaterial`, 72 B as listed; finalised with the resolve pass in Phase 3)

`{ vec4 color; u32 albedo_tex, mask_tex /* bindless indices */; f32 roughness, metallic, emissive, tex_scale;
u32 flags /* existing MaterialFlags + VG_MASKED | VG_TWO_SIDED | VG_DITHER_FADE */; u32 wpo /* snippet id 8 |
params */; f32 alpha_cutoff, wpo_max, fade_start, fade_end, shadow_max_dist, pad }`

### 1.6 Per-frame buffers

| Buffer | Format | 1080p size |
|---|---|---|
| Visibility buffer | SSBO `u64[w*h]`: hi 32 = `floatBitsToUint(1 - depth)` (so `atomicMax` keeps the nearest), lo 32 = visible-cluster index 25 bits \| triangle 7 bits | 16.6 MB |
| Visible cluster list | `{ u32 instance, u32 page_slot<<8 \| cluster }` × 2^21 (cap, cvar) | 16 MB |
| Work queues | node / cluster candidate queues, ping-pong, plus indirect args | ≈ 24 MB |
| HW / SW raster lists | indirect draw / dispatch args | small |
| Hi-Z (MAX) | R32F full mip chain, frame-graph history | 11 MB |
| Page feedback | `u32 page_id, u32 priority` × 4096, per frame in flight, host-readable | 64 KB |
| Page pool | `r_vgeo_pool_mb` (default 512 Modern / 256 2020) of 128 KiB slots, plus the page table (`u32` slot per mesh page) | budget |

The 64-bit packing follows Nanite: the instance id is recovered through the visible-cluster entry rather than
stored per pixel. Buffer (not image) int64 atomics are used because they are core Vulkan 1.2 and available on
every AMD/NVIDIA desktop driver. The image variant would need `VK_EXT_shader_image_atomic_int64`.

*As built (Phase 2):* a visible entry packs `page slot << 12 | cluster` (12 bits, not 8): a 128 KiB page of small
clusters can hold more than 256. The Hi-Z mip 0 is at half resolution (the 2×2 max of the depth buffer), about
a quarter of the size listed.

## 2. Phase 1: offline cluster builder (CPU only)

`src/render/vgeo/vgeo_build.{c,h}` is pure C with no renderer dependency, linked into the game (import), into
`tools/build_vgeo` and into the tests.

1. **Weld and preprocess:** snap positions to the mesh grid (§1.3), then a stable sort-based weld on
   grid position + normal + uv + colour, then a position-only weld id for adjacency and locking; degenerate
   triangles are dropped and missing normals smoothed. (*As built:* our own weld rather than
   `meshopt_generateVertexRemap`, because it must weld on the snapped grid and the vendored meshoptimizer subset
   does not include `indexgenerator.cpp`.)
2. **Leaf clusters:** `meshopt_buildMeshlets` (max 128 tris, 128 verts, cone weight 0.25), then
   `meshopt_optimizeMeshlet`.
3. **DAG loop** (per level, until one cluster remains):
   - Build cluster adjacency: shared-edge counts via a sorted edge list (deterministic).
   - **Greedy partition** into groups of 8 clusters (allowed 4–16). Seed with the lowest-index unassigned cluster,
     grow by the strongest shared-edge neighbour, break ties by spatial proximity, and if no neighbour is left take
     the nearest cluster by centroid (handles disconnected parts).
   - For each group (in parallel on the job system): merge its triangles, **lock every vertex shared with a cluster
     outside the group** (`vertex_lock` in `meshopt_simplifyWithAttributes`), and simplify to 50% with
     `ErrorAbsolute`. If it fails to reach ≤ 75% of the input, retry with a higher error. If that also fails, mark
     the group as not reducible and carry it through unchanged with error raised (a terminal guard for degenerate
     topology).
   - Error: `group_err = max(simplify_err, max member error)`. The lod sphere encloses the member lod spheres.
     **Monotonicity is enforced by construction and asserted.**
   - Re-cluster the simplified group into ≤ 128-tri clusters; their `error` and `lod_sphere` are the group's.
4. **Pack:** order groups coarse → fine. Pack groups into pages (a group never spans pages) and mark root pages:
   the page prefix that holds every group of the top three levels (always resident). Quantise on the mesh grid. Build the per-LOD-level BVH (4-wide, SAH-free median split on group centres) and merge the
   levels under one root.
5. **Discrete LOD export:** for the 2015/2010 eras, take the DAG cut at four fixed object-space errors
   (0, 0.25%, 1%, 4% of the radius), each as a normal index/vertex buffer.
6. **Stats:** per level clusters, triangles and error range, plus pages, bytes and ms per stage, printed by
   `build_vgeo --stats` and stored in the file.

**Deliverables:** `tools/build_vgeo <mesh.gltf> [--out] [--stats] [--threads N]`; the `"vgeo": true` import path
in `assets.c`; a procedural mesh generator (displaced rock / sphere, N triangles) in `src/render/vgeo/vgeo_procgen.c`.

**Tests** (`tests/test_vgeo_build.c`, ctest):
- DAG error monotonic, sphere containment.
- Every group's locked boundary is bit-identical to its children's (crack-free: the edge multiset on group
  boundaries is the same before and after).
- ≤ 128 tris and ≤ 128 verts per cluster.
- Deterministic bytes across runs and thread counts.
- Every cut at a sweep of thresholds is watertight on a closed input. The CPU reference cut is an Euler check plus
  "no open edges". *As built:* "no open edges" means every directed edge is matched by its reverse (crack- and
  hole-free). Near the root, `meshopt_simplify` can occasionally pinch two sheets into a balanced 4-use edge. That
  is still closed, and is allowed at coarse cuts only: ≤ 0.1% of edges, never in the five finest cuts. The Euler
  check runs on the cuts with no pinch.
- 10M-triangle build completes, with time and peak memory reported.

**Gate:** 10M tris builds in < 3 min on 4 cores with < 6 GB peak memory, and the 500k statue builds in < 10 s.

## 3. Phase 2: GPU culling and LOD selection

`src/render/vgeo/vgeo_cull.c`, `shaders/vgeo/*.comp`. The vgeo module takes a small `VgContext` (device, VMA,
pipeline layout), so it runs both inside the renderer and in a **window-less GPU test harness**
(`tests/vgeo_gpu_harness.c`, which creates a device with no surface on lavapipe).

Per view (main camera and each shadow cascade):

1. `vg_cull_cells`: frustum + previous Hi-Z per placement cell → candidate instance ranges.
2. `vg_cull_instances`: one thread per instance. Frustum + prev Hi-Z (bounds reprojected with
   `prev_view_proj_nj`); density/fade (Phase 7); impostor select (Phase 7). Survivors seed the node queue with
   (instance, root). Occluded instances go to the pass-2 list.
3. `vg_traverse` (**level-synchronous**: one `vkCmdDispatchIndirect` per BVH depth, ≤ 16, ping-pong queues).
   - A node is visited if it is in the frustum and not Hi-Z occluded, and if
     `project(node.max_parent_error) > threshold`.
   - At leaves, a group's clusters are emitted when the group's page is resident. Otherwise a page request is
     recorded with priority = projected error.
4. `vg_cull_clusters`:
   - Selected if `project(group.parent_error, group sphere) > t` **and**
     (`project(cluster.error, cluster.lod_sphere) <= t` **or** the cluster's `child_group` page is non-resident).
   - Then frustum, cone (skipped for two-sided / skinned) and prev Hi-Z.
   - Classify **SW** if the projected cluster bounds are < `r_vgeo_sw_px` (default 32 px across). Otherwise **HW**.
   - Hi-Z rejects go to the pass-2 cluster list.
   - Where Nanite uses persistent-threads traversal, the level-synchronous variant needs no forward-progress
     guarantee and is deterministic, which the CPU-reference test needs.
5. Raster pass 1 (Phase 3) → build the **new Hi-Z (MAX)** from the visibility buffer → **pass 2**: re-test the
   occluded instances and clusters against the new Hi-Z and raster the newly visible ones. The pass-2 Hi-Z becomes
   next frame's history.

Projection: `px = error * proj_scale / max(dist - radius, znear)`, where `proj_scale = render_h / (2 tan(fov/2))`.

Cvars:
- `r_vgeo` (0/1).
- `r_vgeo_error_px` (1.0).
- `r_vgeo_shadow_error_px` (2.0).
- `r_vgeo_sw_px`.
- `r_vgeo_occlusion` (0/1/2: off, 1 pass, 2 pass).
- `r_vgeo_freeze` (freeze culling camera).
- `r_vgeo_debug`: 1 cluster id, 2 triangle id, 3 LOD level, 4 error heat, 5 SW/HW, 6 residency, 7 overdraw
  (atomic count), 8 instance.
- `vgeo_stats` (console + HUD line: instances / clusters / tris visible per pass, pages resident / requested).

**Tests:**
- GPU cut equals the CPU reference for 8 fixed cameras (identical selected cluster sets).
- No cluster is drawn with its ancestor (the CPU walks `child_group` chains of the GPU list).
- No instance or cluster is lost by the frustum test against brute force.

**Gate:** zero CPU work per instance per frame (asserted by counting `render_draw` calls and timing the CPU vgeo
update, which stays O(1) in instance count).

*As built (Phase 2):*
- **Queues:** the node queue has 3 regions (0 / 1 ping-pong, 2 = pass 2 seeds: nodes pass 1 found occluded, and
  the children of pass 2 instances). Leaves emit **cluster candidates** `(instance, group << 4 | k)`, one per
  cluster, instead of group items (16 lanes per group idled on the many 1-cluster groups near the root). This
  assumes groups of ≤ 16 clusters, warned at registration. Cells with ≤ 32 instances are tested inline by the
  cell kernel, and the instance step expands the BVH root, so traversal runs `max_depth - 1` levels.
- **Hi-Z:** half resolution, mip 0 = 2×2 max of depth, odd tails folded into the last row / column. The test
  reads ≤ 2×2 texels and keeps the item visible when the rect needs a mip beyond the chain. In game, Phase 2
  builds it from the prepass depth after the prepass (legacy occluders, no vgeo yet), and only pass 1 runs.
  The mid-frame build from the visibility buffer for pass 2 comes with Phase 3.
- **Struct sizes:** `VgViewGpu` 352 B, `VgCounters` 256 B (8 indirect args + 32 counts), `VgPush` 48 B.
- **Harness:** runs on the local GPU (discrete preferred), not lavapipe in a container. Timings come from the same
  run (RTX 3060 Ti); no AMD run yet.
- **Device features**, enabled when present: `scalarBlockLayout` (every vgeo shader uses scalar layout),
  `shaderInt64` + `shaderBufferInt64Atomics`, `drawIndirectCount` + `multiDrawIndirect` (+
  `drawIndirectFirstInstance`), descriptor indexing (runtime arrays, partially bound, non-uniform sampled image
  indexing). Vgeo is off without scalar layout + int64 atomics. `VK_EXT_mesh_shader` is detected but not enabled
  until Phase 3.

## 4. Phase 3: visibility buffer rasterisation and resolve

Two rasterisers write the same 64-bit value with `atomicMax`:
- **HW:** `vkCmdDrawMeshTasksIndirectCountEXT`, one mesh workgroup per cluster, when `VK_EXT_mesh_shader` is
  present. Otherwise `vkCmdDrawIndirectCount`, with the vertex shader pulling `gl_VertexIndex → (cluster, tri,
  corner)` from the cluster buffer (non-indexed, 384 verts per cluster instance). The fragment shader does
  `atomicMax` on the SSBO. There are no colour attachments, and the depth test is off: the atomic is the depth
  test.
- **SW:** compute, one workgroup per cluster and one thread per triangle. The threads share the vertex transform
  via shared memory. Edge functions use fixed-point 16.8 sub-pixel precision and the **same top-left rule and
  pixel-centre convention as the HW rasteriser**. The loop is a bounding-box scan for tiny triangles and a scanline
  loop above 4×4 px.
- **Depth export + G-buffer** (frame-graph pass `vgeo_resolve`, before `prepass`): a full-screen fragment pass that,
  for vgeo pixels, decodes the triangle, re-transforms its 3 vertices (re-skins / re-applies WPO in later phases)
  and computes barycentrics and analytic derivatives. It writes:
  - `gl_FragDepth`;
  - normal / material / velocity into the existing G-buffer targets;
  - albedo into a new transient `vg_albedo` (RGBA8).

  The legacy `prepass` then draws on top with its normal depth test, so mixed scenes composite correctly and GTAO,
  SSR and SSGI see one depth buffer.
- **Lit resolve** (`vgeo_lit`, inside `opaque`): a full-screen pass that shades only vgeo pixels with
  `lit_body.glsl` (shadows, clustered point/spot lights, sky SH, weather terms), reading `vg_albedo` and the
  G-buffer.
  - Materials run as one table-driven uber-shader. **Per-material tile classification** (8×8 tiles binned by
    shader kind: default / triplanar / grid / textured / masked) picks specialised pipelines through indirect
    dispatches, so each pixel runs its own material code once.
  - This replaces Nanite's material-depth trick; the trick is not needed because materials here are parameters
    plus a texture, not separate shaders.
- **Shadows:** each cascade is a vgeo view (frustum only, no Hi-Z in v1) with `r_vgeo_shadow_error_px`.
  - HW clusters draw depth-only straight into the atlas tile.
  - SW clusters do `atomicMin` into an R32 buffer that a per-tile full-screen depth write merges into the atlas.
  - Virtual shadow maps are out of scope. `vg_view` is the hook: a view is `{ view_proj, viewport, error_px,
    flags }`, and a VSM page would just be another view.
- **Motion vectors:** from the reconstructed position, current and previous (unjittered) view-proj; static
  instances do not move.

**Tests:**
- SW vs HW visibility equality on a fixed triangle soup (≤ 0.5% differing pixels, all on edges).
- Depth export matches the raster depth.
- Velocity matches a CPU reference.
- Headless devroom render with 0 validation messages, plus a screenshot diff of vgeo against the same meshes on
  the legacy path (≤ 1% pixels over threshold).

*As built (Phase 3 / 5):*
- **No `vg_albedo` target:** the lit resolve decodes the pixel's triangle and evaluates the material again
  (`vgeo_lit.frag` = the shared `lit_body.glsl` on decoded inputs) instead of reading an albedo written by the
  G-buffer export. One fewer target; the decode runs twice.
- The export and the lit resolve are draws **inside** the existing `prepass` and `opaque` passes (before the
  legacy draws / depth EQUAL), not separate frame-graph passes.
- **No per-material tile classification** yet: one uber resolve (materials are parameters; textures not bound).
- **Shadows:** every shadow cluster is HW (no SW `atomicMin` path into the atlas); `vgeo_shadow.vert` reads the
  shared (writable) set, so the game enables `vertexPipelineStoresAndAtomics`.
- Hi-Z: power-of-two half resolution, 6 levels per dispatch (see VGEO.md "Occlusion Hi-Z"); this frame's Hi-Z
  for pass 2 is built from the visibility buffer only (legacy occluders enter through next frame's history).
- Phase 5: placements are render-only (no collision); the 2015 / 2010 fallback picks discrete LODs on the CPU;
  the bench writes per-pass ms from the frame graph (`vgeo_bench`), not a separate executable.

## 5. Phase 4: streaming

- Page requests are appended in `vg_traverse` / `vg_cull_clusters` and read back `FRAMES_IN_FLIGHT` frames later.
- The CPU sorts the requests by priority and issues `jobs_submit` file reads (the `platform_read_file` range
  variant added for this: fseek / fread on a worker).
- Completions are uploaded through the staging ring, and the page table is patched with a small compute scatter.
- **LRU eviction:** every page used this frame is stamped `last_used = frame` by the cull pass (an atomic store into
  a per-slot u32). The CPU never evicts a slot stamped within the last `FRAMES_IN_FLIGHT + 1` frames, and root
  pages are pinned.
- If the pool is full, requests wait. Correctness holds because a cluster renders its coarser parent while its
  child page is missing.
- Cvars: `r_vgeo_pool_mb` and `r_vgeo_stream_pages_per_frame` (default 64 ≈ 8 MB/frame).
- Debug view: residency colouring, plus a HUD line with resident / requested / loaded / evicted per frame.

**Tests:**
- Never evict a page referenced by this frame's visible list (CPU audit of readback).
- A pool that is too small degrades detail only (no missing geometry).
- A cold start draws every mesh on frame 1 from root pages.

## 6. Phase 5: integration, era presets, test scene

- `data/assets.json` model entries take `"vgeo": true`. The level loader registers vgeo props as static instances
  instead of entities when the prop is static. Brushes stay on the legacy path: they are low-poly boxes.
- **`r_era`:**

  | | Modern | 2020 | 2015 | 2010 |
  |---|---|---|---|---|
  | Geometry | vgeo, 1 px | vgeo, 2 px | discrete DAG LODs (4), CPU LOD by screen size | 3 LODs, more aggressive distances |
  | Foliage density | 1.0 | 0.7 | 0.4 | 0.2 |
  | Shadow error | 2 px | 4 px | n/a | n/a |

  Every value is a plain cvar that the preset sets. A user override marks the preset Custom, like `r_post_preset`.
- **Test scene** `data/levels/vgeo_test.json`, also reachable from the devroom with `vgeo_test_scene`:
  - a 10M-tri displaced rock field;
  - 1000 instances of a 500k-tri procedural statue (displaced, layered torus/sphere composition);
  - a detailed building (procedural: walls with bevelled bricks, about 2M tris).

  The content is generated by `tools/build_vgeo --gen …` into `assets/vgeo/`. Generated files are git-ignored and
  built at configure time, so no binary assets are committed.
- `bench_vgeo`: a script flythrough (fixed spline, 600 frames). It writes CSV and a summary of per-pass ms (frame
  graph timestamps), counters and CPU ms.

## 7. Phase 6: skinned vgeo

- **Builder:** works in the bind pose. Weights are carried as simplification attributes (4 weight channels at low
  weight). A **bone-set penalty** scales the collapse cost by bone-set dissimilarity, implemented by locking
  vertices whose dominant bone differs from all their neighbours' at a 50% simplification ratio. Weights are
  re-normalised with ≤ 4 influences, and clusters are split above 16 bones. Each cluster stores its bone set and
  per-bone bind-space boxes.
- **Palette slots:** `characters.c` uploads each character's palette once into a slot (`render_palette_slot(object_id,
  palette, n)`), and legacy `SkinnedDraw` gains a `palette_slot` alternative to copying. The previous palette is
  found by `object_id` as today. This also removes the 2–3 duplicate palette copies per character.
- **Culling:**
  - The animated bounds are the union over the cluster's bones of `AABB(palette[b] * box_b)`. This is
    conservative for LBS: a skinned vertex is a convex combination of points that each lie in one of the
    transformed boxes.
  - For DQS (`r_vgeo_dqs`) the box is inflated by `r_vgeo_dqs_margin` × the box diagonal, verified by test.
  - Instance bounds come from the root bone plus the mesh radius × max bone scale.
  - The error threshold is scaled by the slot's max bone scale.
  - Cone culling is off for skinned clusters.
- **Raster and resolve:** both rasterisers skin 4 influences from the slot's palette. Resolve re-skins the 3
  vertices for barycentrics, and re-skins them with the previous palette for velocity.
- **Integration:**
  - Modern/2020 characters use the DAG cut. Animation, IK and ragdoll LODs are unchanged.
  - The first-person `noarm` body and the `arm` viewmodel mesh are two vgeo skinned instances (`MAT_VIEWMODEL` uses
    `vm_proj` in the HW/SW vertex stage). The held item is a static vgeo instance with an `XFORM`, updated per
    frame as a moving instance.
  - Hitboxes and ragdolls are untouched.
  - 2015/2010 keep today's discrete LODs, now exported from the DAG.

**Tests:**
- Animated bounds contain every skinned vertex over 600 ticks of locomotion (CPU, all clips).
- Crack-free cut on the animated mesh (CPU cut + skin, no open edges).
- Resolve re-skin within 1e-4 of the raster position (GPU harness).
- Velocity matches the CPU reference.
- 500-zombie horde headless render, 0 validation messages, and no LOD popping: the per-frame image delta while
  moving slowly stays below the threshold.

## 8. Phase 7: foliage support (no content)

`src/render/vgeo/foliage_support.{c,h}`:
- **Masked / two-sided:**
  - Material flags. SW: sample `mask_tex` at the barycentric UV (explicit LOD from the triangle's UV derivatives)
    and discard before `atomicMax`. HW: discard in the fragment stage. Dithered alpha-to-coverage is a blue-noise
    threshold, rotated per TAA frame.
  - Two-sided turns off cone culling and flips the normal in resolve when back-facing.
  - Masked clusters are always classified so that SW and HW agree on the same mask (test).
- **Density:** 32 B instances, placement cells as the top culling level (an instance grid rather than a BVH:
  static placements on a heightfield world map naturally to 2D cells), up to 512k instances per frame.
  - A **placement chunk** (`.vgp`, per 32 m chunk) holds `{ header; cells[16]; VgInstance[] }`. It is loaded by
    the chunk job alongside the heightfield and freed with the chunk.
  - GPU instances are sub-allocated from a free-list allocator in the instance buffer. Unload returns the range,
    and a test checks for leaks.
- **WPO snippets** (`shaders/vgeo/wpo.glsl`): `WPO_NONE`, `WPO_GRASS` (bend by height, vertex-colour R weight),
  `WPO_TREE` (trunk sway (R) + branch (G) + leaf flutter (B), per-instance phase) and `WPO_DISPLACE` (reads the
  displacement buffer, combinable with the others).
  - Inputs: world position, vertex colour, phase, `wx2` / `wx5` wind, time.
  - Each snippet declares its max displacement: `wpo_max = f(material params, max wind)`. Bounds are inflated by
    it in every culling stage and in the LOD error.
  - Velocity evaluates the snippet at the previous time and wind as well.
- **Impostors:** `build_vgeo --impostor` renders the root cluster from 8×8 hemi-octahedral directions on the CPU
  (our own small rasteriser, deterministic and needing no GPU), into a 1024² albedo+alpha and normal atlas.
  - At runtime, an instance whose projected sphere is `< r_vgeo_impostor_px` draws one camera-facing quad with the
    3 nearest frames blended. Otherwise it goes down the DAG.
  - The same comparison in one function decides both sides, so there is no gap and no overlap at the threshold
    (test). The placeholder mesh is the test tuft.
- **Distance fade and density:**
  - Per-material `fade_start` / `fade_end` dither the instance out.
  - Density keeps an instance when `hash(instance_id) < density(dist) * r_foliage_density`. The hash is fixed per
    instance, so membership is stable for a static camera and changes monotonically with distance.
- **Shadows:** masked clusters cast masked shadows (the same mask test in the depth-only paths), plus a
  `shadow_max_dist` cull per material.
- **Collision:** the asset flag `"colliders": "capsule"` registers trunk capsules into the chunk's static physics
  when the placement chunk loads. `foliage_query(pos, radius, out[])` returns the instances in the cells, for
  footstep surfaces and later crushing or burning.
- **Displacement buffer:** a 128×128 RG16F world-space grid at 0.5 m around the camera (64 m), toroidally
  scrolled and decaying each frame. `foliage_push(pos, radius, strength)` is the writer API (stubbed). The console
  command `foliage_push_test` drives it in the dev room.
- **Test content:** procedural grass tuft (about 60 tris) and bush (about 600 tris) meshes, vertex-colour wind
  weights and a noise alpha mask. `foliage_test_scatter [count=300000]` scatters them over the dev yard as
  placement chunks.

**Tests:**
- Masked GPU visibility matches the CPU alpha-tested reference.
- WPO bounds contain every vertex at max wind.
- The density hash is stable across 600 frames.
- Cell culling matches brute force.
- Impostor switch exactly at the threshold.
- Placement load/unload leaks nothing over 2000 m of streaming flythrough.
- Hi-Z rejects ≥ 90% of instances behind the dev-room walls (counter).

## 9. GPU budgets (1080p, mid-range: RTX 3060 / RX 6600 class)

| Phase | Work | Budget |
|---|---|---|
| 2 | cells + instances + traversal + clusters, both passes | 0.5 ms |
| 3 | HW + SW raster, both passes | 1.2 ms |
| 3 | Hi-Z build ×2 | 0.1 ms |
| 3 | depth export + G-buffer | 0.3 ms |
| 3 | lit resolve (replaces forward shading of those pixels) | 0.8 ms |
| **5** | **static test scene: cull + raster + resolve** | **≤ 3.0 ms** |
| 3/5 | 3 shadow cascades (vgeo part) | ≤ 1.0 ms, reported separately |
| 4 | streaming: page-table patch + uploads | ≤ 0.1 ms GPU; CPU I/O on jobs |
| **6** | **+500 skinned instances (static scene present)** | **≤ 4.0 ms total** |
| **7** | **300k masked foliage instances incl. shadows** | **≤ 2.5 ms** |

Memory: page pool 512 MB (Modern); visibility buffer, queues and Hi-Z about 70 MB at 1080p; instances 32 B each
(500k = 16 MB).

Hardware-independent gates checked here on lavapipe for the static test scene (they keep the ms budgets
plausible):
- ≤ 12M rasterised triangles per frame;
- ≤ 150k visible clusters;
- > 95% of clusters rejected before raster on the flythrough.

## 10. Order of work and checkpoints

Each phase is one or more commits on `claude/wonderful-einstein-omk8y7`. A phase is closed only when:
- its ctest tests pass;
- the headless lavapipe runs show 0 validation messages;
- `bench_vgeo` counters meet the gate.

After each phase I report the counters and you run `bench_vgeo` on Windows for the ms numbers. The Linux build fix
already committed (`hud.c`) is the only change outside this plan so far.
