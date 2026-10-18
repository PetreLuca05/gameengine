# vgeo content: builder, scene tables, foliage, trees

**The virtualized geometry renderer ("nanite": GPU cell / instance / BVH / cluster culling, two-pass Hi-Z,
the HW / SW visibility-buffer rasterisers, the resolve passes, page streaming, vgeo shadows, characters
through vgeo, `r_vgeo`, `bench_vgeo`, the GPU harness tests) lives on branch `nanite`, with the full version
of this file and docs/VGEO_PLAN.md.** On this branch the classic instanced renderer
(docs/CLASSIC_RENDERER.md, `src/render/vk_classic.c`) is the only renderer of the vgeo content. This file
documents what it still uses: the cluster builder and its DAG (the classic LOD chains are cuts of it), the
`.vgeo` file, the scene tables, the foliage support (materials, WPO, density / fade, impostors, placement
chunks, displacement buffer) and the imported trees.

The builder follows Nanite (Brian Karis, Rune Stubbe, Graham Wihlidal, *"A Deep Dive into Nanite Virtualized
Geometry"*, SIGGRAPH 2021): triangle clusters of ≤ 128 triangles, a DAG of cluster groups simplified with
locked group boundaries, view-dependent cuts from the pair (own error, parent error), a BVH over groups.
Simplification and clustering use meshoptimizer v0.22 (`simplifier.cpp`, `clusterizer.cpp`, vendored in
`third_party/meshoptimizer`).

## Files

| File | What |
|---|---|
| `src/render/vgeo/vgeo_gpu.h` | GPU / file structs (std430 mirrors, size asserts): meshes, instances, cells, materials, view fields |
| `src/render/vgeo/vgeo_build.{c,h}` | builder: weld, clusters, DAG, BVH, pages, discrete LODs, (de)serialize, decode, CPU cut |
| `src/render/vgeo/vgeo_import.{c,h}` | glTF → builder, `assets/cache/<stem>.vgeo.<key>.dgc` cache; `.vgeo` level files |
| `src/render/vgeo/vgeo_procgen.{c,h}` | procedural test meshes: displaced rock, torus, rock field, statue, building |
| `tools/build_vgeo/build_vgeo.c` | offline tool (also builds the trees at build time) |
| `tests/test_vgeo_build.c` | ctest `vgeo_build` (+ `vgeo_build_long`, label `long`: the 10M gate) |
| `src/render/vgeo/vgeo_ctx.{c,h}` | `VgContext` (device, VMA, shader loader), buffers, uploads, one-shot commands |
| `src/render/vgeo/vgeo_scene.{c,h}` | `VgScene`: mesh / material / instance / cell tables, placement region, commit, CPU reference cut |
| `src/render/vgeo/vgeo_skin.{c,h}` | CPU mirror of the skinned bounds / skinning (palette slots: only with a GPU scene) |
| `src/render/vgeo/vgeo_procgen_skinned.{c,h}` | procedural skinned creature (body + tentacles, 31 bones) and its analytic animation |
| `src/render/vgeo/vgeo_tex.{c,h}` | `VgTextures`: the module textures (masks, tree textures, impostor atlases, the displacement buffer) |
| `src/render/vgeo/foliage_support.{c,h}` | WPO CPU mirror + declared bounds, fade / density / impostor rules, `.vgp` chunks, scatter, `foliage_query`, impostor upload, displacement buffer, wind field tile ring |
| `src/render/vgeo/foliage_wind.{c,h}` | vegetation wind model, CPU mirror of `wpo_veg` (docs/FOLIAGE_WIND.md) |
| `src/render/vgeo/vgeo_impostor.{c,h}` | CPU hemi-octahedral impostor baker, file section |
| `src/render/vgeo/vgeo_procgen_foliage.{c,h}` | grass tuft, wheat, the test bush, alpha masks, their materials |
| `src/render/vgeo/vgeo_tree.{c,h}` | imported tree models: species table, FBX / OBJ import, wind weights, capsules, cache, coverage mips |
| `src/render/vgeo/grass_asset.{c,h}` | the grass assets of docs/FOLIAGE_GRASS.md |
| `shaders/vgeo/wpo.glsl`, `vg_displace.comp` | WPO snippets (included by `classic_common.glsl`); displacement buffer update |
| `src/render/vk_vgeo.c` | game glue: scene, level placements, foliage types, grass, trees, wind / displacement per frame, era presets |
| `src/game/game_foliage.c` | `foliage_test_scatter`, trunk capsules, player pushes |

The builder is pure C with no renderer dependency. It is compiled into the game (through the `src/` glob) and
the `dg_vgeo` static library (tool and tests).

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

**Cut rule** (`vgeo_cut`, which the classic LOD chains use): cluster `c` in group `g` is drawn at
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
on `Model.vgeo`; a level `"vgeo"` entity places it ("Levels").

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
## Scene tables (`VgScene`)

The tables the classic renderer draws, registered at load time. In the game the scene has **no GPU context**
(`vgeo_scene_init(&scene, NULL)`): `vgeo_scene_commit` bins and derives, nothing is uploaded and no page pool
exists (the GPU upload path of the module is kept for a context that has one).
- `vgeo_scene_add_mesh` appends a built mesh: `VgMesh` (`node_base`, `group_base`, `page_base` into the global
  tables), its BVH nodes, groups and pages, and keeps a copy of the page data; `vgeo_scene_add_mesh_file` keeps
  only the path of the `.vgeo` file and reads pages back with `platform_read_file_range`. The classic renderer
  takes its LOD chain from the build at registration (`vkc_register_mesh`: decoded DAG cuts or the exported LODs).
- `vgeo_scene_add_instance` appends a 32 B `VgInstance` (smallest-three rotation, half scale).
- `vgeo_scene_commit` bins instances into **8 m cells** on x / z (stable sort, instances of a cell contiguous,
  cell AABB = union of the instance mesh spheres) and derives every mesh's foliage fields from its materials.
- The CPU keeps every page's header and cluster records ("meta") for the CPU reference cut
  (`vgeo_scene_cpu_select`).

### Levels and eras

- Level entity `"vgeo"` (`src/world/level.c`): `mesh` (a `.vgeo` file or a model name with `"vgeo": true`), `pos`,
  `yaw`, `scale`, `scale_jitter`, `grid: [nx, nz]`, `spacing`, `color`, `roughness`, `seed`, `collision`. The game
  hands the placements to `render_vgeo_set_level` after the level loads; both sides expand grids with
  `vgeo_placement_expand` (same instances) and share the loaded file (`vgeo_file_get`, once per process).
- Collision (static, `.vgeo` files): `"mesh"` (default for single placements) = the coarsest discrete LOD as a
  triangle mesh per instance; `"sphere"` (default for grids) = the bounding sphere x 0.8; `"none"`.
- `build_vgeo --gen field|statue|building` still writes the stress meshes of the old `map vgeo_test` (that map
  lives on branch `nanite`).
- `r_era` (0 modern, 1 2020, 2 2015, 3 2010, 4 custom) sets `r_vgeo_lod_px` (2 / 3 / 4 / 12 px; 2010 never draws
  LOD 0), `r_vgeo_shadow_error_px` (2 / 4 / 2 / 2 px) and `r_foliage_density` (1.0 / 0.7 / 0.4 / 0.2); any other
  combination reads back as Custom. Engine Fidelity (docs/ENGINE_FIDELITY.md) bounds these: its levels scale the
  era's error thresholds and cap its foliage density; the detection compares against the bounded values.

## Skinned meshes (builder)

The builder takes skinned sources too (characters through vgeo were part of the nanite renderer; the build
stays: `vgeo_procgen_skinned`, `test_skin_reference`).

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
- **Discrete LODs** (`VgLodVertex`) carry no skin data. Characters draw through the legacy skinned path
  (docs/CHARACTERS.md).

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

## Foliage support

Foliage is ordinary vgeo content plus a few material, instance and placement features, drawn by the classic
renderer (docs/CLASSIC_RENDERER.md: masked / two-sided pipelines, dithered fades, impostor quads, WPO in the
vertex shader). The content is the imported tree models ("Tree models") plus procedural grass tufts and wheat
and the grass assets (docs/FOLIAGE_GRASS.md). The wind that moves all of it is docs/FOLIAGE_WIND.md.

### Materials (`VgMaterial`, 144 B)

| Field | Meaning |
|---|---|
| `flags` | `MAT_*` plus `VGM_MASKED`, `VGM_TWO_SIDED`, `VGM_DITHER_FADE`, `VGM_FOLIAGE` (bits 24-27) |
| `mask_tex`, `alpha_cutoff` | alpha mask (0: `albedo_tex`), its threshold |
| `wpo`, `wpo_params[4]`, `wind_params[8]`, `wpo_max` | vegetation wind preset (`WPO_GRASS`, `WPO_BUSH`, `WPO_TREE`, `\| WPO_DISPLACE`), its parameters (docs/FOLIAGE_WIND.md), declared bound (the commit computes it with `vgeo_wpo_max`) |
| `fade_start`, `fade_end` | distance fade of `VGM_FOLIAGE` instances (m) |
| `shadow_max_dist` | no sun-cascade shadow beyond this distance from the camera (m, 0 = always) |
| `mr_tex`, `normal_tex`, `occlusion_tex`, `emissive_tex`, `normal_scale`, `occlusion_strength`, `emissive_rgb` | glTF maps (docs/PBR.md): renderer texture ids or module slots (0 = none, with the `MAT_*_TEX` flags); the classic prepass / lit shaders sample them |

At commit every mesh gets its foliage fields from its materials (`VgMesh` 80 B: `max_wpo`, fades, shadow distance,
`fflags`, impostor atlas). Model meshes take them from a `"foliage": {...}` block in `data/assets.json`
(`masked`, `two_sided`, `dither`, `alpha_cutoff`, `wpo`, `displace`, `wpo_params`, `fade`, `shadow_max_dist`)
and `"colliders": "capsule"`, `"capsule": [r, h]` on the model entry.
### Textures

`VgTextures` (`vgeo_tex.h`): indices below 1024 are renderer texture ids, 1024-1151 the module's own (masks, tree
textures, impostor atlases, the displacement buffer). The classic renderer uses renderer ids directly and wraps
module slots as renderer textures (same image and sampler, no copy).

### WPO (`shaders/vgeo/wpo.glsl`)

- One vegetation model for every snippet (docs/FOLIAGE_WIND.md): the GPU Gems 3 chapter 16 (Crysis)
  layers driven by the canonical wind channels of the vertex colour (R flutter, G branch phase, B
  branch bend; trunk bending from the height), sampling the wind field's tile at the instance pivot.
  CPU mirror `vgeo_wind_eval` / `vgeo_wpo_eval`. The classic vertex shaders include it as is.
- `WPO_DISPLACE` adds the displacement buffer's push first (weighted by the height) and takes the wind
  out of the pushed plant.
- Bounds: `vgeo_wpo_max` sums the layers' declared maxima (`vgeo_wind_layer_max`) + `1.118 ·
  VG_DISPLACE_MAX` (0.6 m) with the displacement buffer. Culling spheres and cell bounds grow by the mesh's
  `max_wpo` x max(instance scale, 1) (`vg_wpo_margin`).
- WPO materials use the vertex colour as wind weights, not as albedo.

### Instances: density, fade, shadow distance, impostors

- **Density / fade:** `vgeo_foliage_alpha` keeps a `VGMF_FOLIAGE` instance while its fixed hash
  `h = (hash + 0.5) / 256 · 15/16 < r_foliage_density · fade(distance)`. The last 1/16 of that threshold
  is the dither band. The hash is byte 1 of `flags_phase`, set from the position bits by
  `vgeo_foliage_instance`, so it does not depend on load order.
- **Shadow distance:** shadow views drop instances past `shadow_max_dist` (from the camera).
- **Impostor:** a mesh with an atlas switches to its impostor below a projected size (`vgeo_use_impostor`,
  the per-mesh scale in `VgMesh.impostor_grid >> 16`); the classic renderer's thresholds and cross-fades are
  docs/CLASSIC_RENDERER.md / docs/LOD.md. Never in shadow views.
- **All-foliage cells** are skipped whole once their nearest point is beyond the largest foliage fade end
  (main view) or shadow distance (shadow views).

### Impostors (`vgeo_impostor.c`)

- **Bake.** `vgeo_impostor_bake` (CPU, deterministic) renders the leaf clusters of a built mesh:
  - 8 × 8 hemi-octahedral directions;
  - orthographic over the mesh sphere, 2× supersampled, masked materials alpha-tested;
  - albedo (sRGB, coverage alpha) and mesh-space normals, empty texels dilated.

  The result goes in the `.vgeo` impostor section (`VGF_HAS_IMPOSTOR`): `build_vgeo --impostor [size=1024]
  [grid=8]`. `vgeo_foliage_impostor_upload` makes the two module textures (mips down to 4 px tiles).
- **Runtime.** The classic renderer draws a camera-facing quad at the mesh sphere centre that blends the 3
  nearest atlas frames (`shaders/classic_impostor.glsl`).

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
  - The classic renderer culls the region's cells on the CPU (the GPU copy and its per-frame upload,
    `vgeo_scene_placement_record`, only exist with a GPU scene).
  - `vgeo_scene_placement_audit` checks that live chunks plus free lists cover the region exactly once.
- **Query.** `vgeo_foliage_query(pos, radius)` returns the placement instances within a horizontal radius,
  nearest first.

### Displacement buffer

- `VgDisplace`: a 128 × 128 RG16F image of 0.5 m texels around the camera, with horizontal pushes in metres.
  It is toroidal: texel `t` holds world texel `X ≡ t (mod 128)` of the window.
- `vg_displace_record`, per frame before the shadow pass (`vg_displace.comp`, `vkg_record_foliage`):
  - clears texels whose world texel changed with the window;
  - decays (1.5 s time constant);
  - applies up to 64 queued pushes (`vg_displace_push(pos, radius, strength m/s)`, falling off to the radius);
  - clamps to `VG_DISPLACE_MAX`.
- `WPO_DISPLACE` samples it (the classic renderer's displacement set).
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
- **Commands:** `foliage_stats` (chunks, region, allocator audit), `foliage_query [r]`, `foliage_push_test [s]`,
  `classic_stats`.

### Tree models (`vgeo_tree.c`, `assets/treefbx`)

The game's trees, bushes and flowers are Tree It exports (CC BY 4.0, docs/CREDITS.md): FBX 6.1 / OBJ
meshes and PNG textures in `assets/treefbx`. There are no procedural trees any more; the procedural
bush is test content only (`build_vgeo --gen bush`).

- **Import** (`vgeo_tree_import`, ufbx for both formats). Every triangle of every mesh node; one material
  per texture (the FBX model name or the OBJ `usemtl` minus Tree It's `_N` suffix, + `.png`).
  - A material whose texture cuts out (> 2 % of the texels below half alpha) is a masked, two-sided card
    with the albedo alpha as the mask (cutoff 0.5). Bark, stumps and stems are opaque.
  - The files are centimetres, Y up, ground at y = 0. The mesh is scaled to the middle of the species
    height range ("Heights" below) and re-centred on its trunk base (the bark between 0 and 1.5 m).
  - Tree It card normals point up / outwards whatever the winding (and are not unit length). Each card
    triangle is re-wound towards its normal, so the front face is the lit side (the shaders flip only
    back faces). Without this, half of the leaves turned black.
  - Wind weights in the vertex colour: Tree It's own channels, which are the canonical layout of
    docs/FOLIAGE_WIND.md (R leaf flutter, G branch phase, B branch bend, normalised to a maximum of 1 per
    tree; trunk bending comes from the height). Without colours they are inferred from the geometry.
  - Trunk capsule: the 25th percentile of the bark's distance from the axis between 0.4 and 1.6 m
    (scaled down for bushes), + 10 %; height min(H / 2, 5 m).
- **Build.** The ordinary vgeo builder: its DAG is the LOD chain (the files' own `_LODn` meshes are not
  used). Card-only groups do not coarsen (see "Test content"), so a tree's coarsest cut stays at about
  30-90 % of its triangles. Hence the impostors.
- **Impostors.** 512 px atlas, 8 × 8 frames, baked with the textures. Trees switch at
  `impostor_scale` × `r_vgeo_impostor_px`: 6× (96 px across, about 180 m for a 25 m tree) for the
  leafy trees, 2× for bushes, 1× for the bare old tree (its twigs thin out in a 64 px frame).
  The per-mesh scale lives in the high 16 bits of `VgMesh.impostor_grid` (`vgeo_impostor_scale`, 1/16
  steps, 0 = 1×), mirrored by `vgeo_use_impostor`.
- **Textures.** Albedo (RGBA8, sRGB) for every material. Loaded with stb_image and box-filtered
  (alpha-weighted) to 512 px. Masked textures get coverage-preserving mips (`vgeo_tree_mip_coverage`: each
  level's alpha is scaled so the share of texels above the cutoff matches level 0); with plain box mips,
  cards thinned out and distant crowns turned into bare sticks. Shared by file across species (27
  textures). They are module textures, so `VG_TEX_OWN` is now 128 (the table holds 1152 entries; 84 used
  on zombieland).
- **Bark maps.** The opaque materials (bark, stumps, the bamboo / daisy stems: 13 textures) also get a
  tangent normal and an ORM map from `assets/treepbr/<texture>_n.png` / `_orm.png` (linear module
  textures; `tree_load_maps`, after the cache read or the build, so they are not part of the cache and
  `VG_TREE_VERSION` stays). `vgeo_tree_materials` sets `MAT_NORMAL_TEX | MAT_MR_TEX | MAT_OCCLUSION_TEX`,
  roughness factor 1 (the ORM's g is the roughness), occlusion strength 0.8; `foliage_build_trees` enters
  `normal_tex` and `mr_tex = occlusion_tex` (the same ORM). The classic renderer shades them with the
  screen-derivative tangent frame (docs/PBR.md). Cards (leaves, needles, branches) stay albedo-only, impostors
  keep their baked albedo / normal.
  Missing files: logged, the material stays albedo-only. Generator `tools/vfxgen/tree_pbr.py` (local,
  deterministic): Tree It's own `_normal` (its DirectX y flipped to the glTF convention: every bark map
  correlates at −0.7 .. −0.8 with the albedo relief in y, +0.7 .. +0.85 in x) and `_roughness` maps, box-
  halved to the albedo's game size; AO = horizon AO (`surface_pbr.horizon_ao`) of the height
  integrated from the normal map (Frankot-Chellappa, ~8 texels relief) at half size. `Stump03_normal.png`
  (two iCCP chunks) is read without its ancillary chunks. Memory: normals 16 MB, ORM 4 MB (the 27 albedo
  textures ≈ 35 MB). Cost (`tools/tests/pbr_perf.cfg`, opaque pass, maps on vs off): +0.10 ms at a pine
  trunk close-up, +0.07 ms broadleaf, +0.06 ms zombieland forest.
- **Cache.** `build_vgeo --trees <dir/> <out/>` builds every species in parallel at build time (CMake
  `dg_vgeo_content`, about 1.4 s) into `bin/assets/vgeo/trees/<name>.dgtree`: a `VgTreeCacheHeader`
  (key, materials, masks, capsule, scale) + the `.vgeo` bytes. The scale is baked into the meshes, the
  capsule and the impostor. The key hashes the source file, the species height range and atlas size, `VG_TREE_VERSION` and `VGEO_BUILD_VERSION`. When a file is missing or
  stale the game builds the species on the job system (about 1.1 s for all) and writes
  `assets/cache/<name>.tree.<key>.dgc`. Texture edits do not change the key: bump `VG_TREE_VERSION`.
- **Registration** (`foliage_build_trees` in `vk_vgeo.c`): `render_foliage_test_types` loads the bushes
  and flowers, `render_foliage_map_types` the trees. Each species is a foliage type with a role
  (`render_foliage_role_types(role, names, weights, max)`), in table order on every machine.

| Species (type) | File | Role | Height range | Triangles (leaf / coarsest cut) | Materials |
|---|---|---|---|---|---|
| `tree_pine` | pine.fbx | conifer | 18-32 m | 7 027 / 2 265 | bark09, stump01, pine branch (masked) |
| `tree_fir` | fir.fbx | conifer | 20-36 m | 4 973 / 3 697 | bark07, fir branch |
| `tree_spruce` | spruce.fbx | conifer | 20-36 m | 9 010 / 5 641 | bark08, spruce branch, branch06 |
| `tree_oak` | oak.fbx | broadleaf | 16-28 m | 8 990 / 5 570 | mossybark02, stump03, oak leaf |
| `tree_beech` | beech.fbx | broadleaf | 20-34 m | 10 584 / 9 755 | bark01, beech leaf |
| `tree_birch` | birch.fbx | broadleaf (0.8) | 14-24 m | 6 944 / 2 683 | bark11, birch leaf |
| `tree_elm` | elm.fbx | broadleaf (0.8) | 16-27 m | 4 013 / 984 | bark07, elm leaf |
| `tree_maple` | maple.fbx | broadleaf | 14-24 m | 17 962 / 4 071 | bark02, leaf maple |
| `tree_old` | old.fbx | dead | 10-17 m | 5 873 / 2 597 | bark04, stump01, branch06 (a rooted snag with a few twig cards) |
| `bush_juniper` | juniper.fbx | bush | 3-7 m | 2 906 / 1 752 | bark05, juniper branch |
| `bush_bamboo` | bamboo.obj | bush (0.4) | 2.5-5 m | 3 324 / 1 660 | plant02, bamboo branch |
| `bush_acacia` | tree1.obj | bush (0.4) | 4-8 m | 8 957 / 4 088 | bark05, acacia branch |
| `flower_daisy` | daisy.fbx | flower | 0.35-0.75 m | 632 / 632 | plant05, flower daisy, daisy leaf (no impostor) |

**Heights.** Each species has a target height range (`height_min` / `height_max` in the `vgeo_tree.c`
table). The mesh is scaled to the middle of the range (instance scale 1), and the open-world scatter
picks every instance height uniformly in the range (`vgeo_tree_scale_range`, `openworld_foliage.c`;
the forest underbrush and the meadow bushes use the same range). Trunk capsules (radius and height),
cell / shadow bounds and impostor switch distances all follow the mesh and the instance scale. The
ranges come from published mature heights. Forest trees sit in the lower-middle part of the mature
range: a stand, not record trees, and the crowns stay readable at the forest densities
(docs/ZOMBIE_MAP.md, up to 3.6 trees / 100 m², 280-360 stems / ha, typical for mature stands, so the
densities were kept).

| Species | Native model height (file) | Old in-game range | Published mature height | Source |
|---|---|---|---|---|
| Scots pine | 11.9 m | 12.2-21.6 m | 15-25 m (nursery), up to 35-36 m (Scotland) | [Forestry and Land Scotland](https://forestryandland.gov.scot/learn/trees/scots-pine), [Westmuir](https://westmuir.org.uk/woodland/flora/scots-pine/) |
| Silver fir | 10.6 m | 10.8-19.1 m | 30-50 m, to 45 m (RHS) | [RHS](https://www.rhs.org.uk/plants/11/Abies-alba/Details), [van den Berk](https://www.vdberk.co.uk/trees/abies-alba/) |
| Norway spruce | 10.6 m | 13.0-22.9 m | 30-40 m (native range) | [dimensions.com](https://dimensions.com/element/norway-spruce-picea-abies), [KPU plant database](https://plantdatabase.kpu.ca/PlantPrint/piab) |
| English oak | 14.3 m | 11.5-20.3 m | 20-40 m | [Woodland Trust shop](https://shop.woodlandtrust.org.uk/english-oak), [Ashridge](https://www.ashridgetrees.co.uk/p/garden-trees/oak-quercus/robur-common-native) |
| Common beech | 9.7 m | 12.2-21.6 m | over 40 m (UK record 45 m) | [Woodland Trust](https://www.woodlandtrust.org.uk/trees-woods-and-wildlife/british-trees/a-z-of-british-trees/common-beech), [Deepdale Trees](https://www.deepdale-trees.co.uk/trees/2016/12-Fagus-sylvatica.html) (to 30 m) |
| Silver birch | 7.8 m | 10.1-17.8 m | 25 m, up to 30 m | [Woodland Trust shop](https://shop.woodlandtrust.org.uk/silver-birch), [Nature's Calendar](https://naturescalendar.woodlandtrust.org.uk/what-we-record-and-why/species-we-record/trees/silver-birch/) |
| Field elm | 8.5 m | 10.8-19.1 m | 20-30 m | [RHS](https://www.rhs.org.uk/plants/164598/wd/details), [van den Berk](https://www.vdberk.co.uk/trees/ulmus-minor/) |
| Norway maple | 9.1 m | 10.1-17.8 m | 15-25 m | [Gedling tree trail](https://www.gedling.gov.uk/resident/parksandgreenspaces/digbyarboretum/treetrail/10norwaymaple), [Ashridge](https://ashridgetrees.co.uk/p/garden-trees/maple-acer/platanoides-norway) |
| Old snag | 8.9 m | 7.9-14.0 m | a retrenched veteran: e.g. the Rumskulla oak, 14 m | [woodlands.co.uk](https://www.woodlands.co.uk/?p=38655) |
| Common juniper | 8.6 m | 1.9-3.7 m | 4-8 m (RHS), prostrate shrub forms lower | [RHS](https://rhs.org.uk/plants/9557/juniperus-communis/details), [Naturblick](https://naturblick.museumfuernaturkunde.berlin/species/portrait/204?lang=en) |
| Bamboo (Phyllostachys) | 4.8 m | 2.2-4.1 m | clumps 2.5-4 m (P. aurea, P. nigra), 4-8 m vigorous | [RHS P. aurea](https://www.rhs.org.uk/plants/75274/phyllostachys-aurea/details), [RHS P. nigra](https://www.rhs.org.uk/plants/12869/wd/details) |
| Umbrella thorn acacia | 7.9 m | 2.7-5.2 m | 5-20 m wild, 3-5 m cultivated | [SANBI](https://pza.sanbi.org/node/9642), [Wikipedia](https://en.wikipedia.org/wiki/Vachellia_tortilis) |
| Oxeye daisy | 0.85 m | 0.53-0.95 m | 25-70 cm (60-80 cm typical) | [Naturblick](https://naturblick.museumfuernaturkunde.berlin/species/portrait/383?lang=en), [Burren flora](https://flora-burren.linnaeus.naturalis.nl/linnaeus_ng/app/views/species/taxon.php?id=38130) |

The old ranges came from one fixed height per species (8-18 m trees) times a shared scatter scale
(0.72-1.27 for trees, 0.65-1.15 / 0.6-1.05 for forest / meadow bushes, 0.75-1.35 for flowers): about
half the real heights, and the columnar juniper and the acacia tree read as miniature trees. Native
heights are logged by `build_vgeo --trees` (`scale ... (<file units> -> <m>)`); `tools/tests/tree_scale.cfg`
takes the scale screenshots (third-person player beside each forest, a zombieland forest).

Wind: per-species tuning of the vegetation model (`k_wind`, docs/FOLIAGE_WIND.md "Parameters"); the
main bend's height is the species' scale-1 height. Trees fade at
850-1100 m with shadows to 70 m; bushes 150-210 m / 40 m; daisies 50-80 m / 15 m.

**Budget** (+ 20 MB GPU for the bark maps, above). 13 species, 91k source triangles, 2.2k clusters; .dgtree files 40 MB on disk (mostly the
impostor atlases and the pages). GPU: 27 textures at ≤ 512 px with mips ≈ 35 MB, 12 impostor atlas
pairs ≈ 34 MB, the classic LOD chains (`classic_stats`).

### Test content (`vgeo_procgen_foliage.c`)

- **Tuft:** 5 crossed cards, 60 triangles. Grass masks of 14 tapered, noisy blades. Masked, two-sided,
  `WPO_GRASS | WPO_DISPLACE`. Fade 40-65 m, shadows to 15 m.
- **Bush:** a 48-triangle tapered trunk plus 46 leaf cards, 600 triangles. Leaf-blob masks. `WPO_TREE`. Fade
  100-150 m, shadows to 40 m, trunk capsule.
- Vertex colours hold the wind weights. `build_vgeo --gen tuft|bush [--impostor]`.
- The builder now carries a material subset that simplifies to nothing (tiny disconnected cards) through
  unchanged, instead of meshletizing zero triangles. Earlier outputs are byte-identical.

### Simplifications (foliage)

- **Impostor depth is the quad's plane.** No per-pixel depth offset. Impostors cast no shadows.
- **Displacement velocity** uses this frame's buffer for the previous position too.
- **WPO and the LOD error.** The WPO bound grows the spheres. The simplification error is not increased.
- **Shadow distance and fades are per mesh** (the max over its materials), not per cluster.
- **Capsules** are hexagonal prisms in one static mesh per chunk, not capsule shapes.
- **Displacement format.** RG16F needs `shaderStorageImageExtendedFormats` (the game warns if missing).

## Builder simplifications vs Nanite

- **Partitioner:** a greedy, shared-edge-weighted grower over a Morton order instead of METIS graph partitioning.
  Groups are slightly less compact, so a few more boundary vertices end up locked. Reduction is still ~50 % per
  level on all test meshes, with 0 unreducible groups.
- **One grid per mesh** for positions (u16 offsets from an i32 cluster base); simpler and crack-free by
  construction.
- **No per-cluster bit packing** of attributes: fixed 12 B vertices (u16×3 position, oct normal 8+8, half2 UV).
- **Pinched edges:** `meshopt_simplify` can, near the root, collapse two sheets onto one edge. The tests allow it
  only at coarse cuts (≤ 0.1 % of edges).
- **Locking:** whole shared positions are locked, including material seams inside a group, instead of only the
  group's outer boundary edges.
- **Discrete LOD export:** plain cuts of the DAG at fixed errors, the classic renderer's LOD chains.


## Adding a foliage system on top

A foliage system is a content and placement layer on the vgeo content and the classic renderer, not a
renderer fork ("Foliage support"); it needs only:

1. **Meshes.** Author or generate grass, bush and tree meshes as normal glTF with `"vgeo": true` and a
   `"foliage": { "masked": true, "two_sided": true, "wpo": "grass" | "tree", "wpo_params": [...], "fade": [s, e],
   "shadow_max_dist": d }` block in `data/assets.json`. Bake wind weights into vertex colour in the canonical
   layout of docs/FOLIAGE_WIND.md: R = leaf flutter, G = branch phase, B = branch bend (`VgSource.color`, 4 extra bytes per vertex; WPO materials do not use it as
   albedo). For distant instances, `build_vgeo --impostor` bakes the hemi-octahedral atlas into the `.vgeo`,
   `vgeo_foliage_impostor_upload` puts it on the GPU, and the classic renderer's impostor thresholds set the switch.
2. **Placement.** Write per-chunk placement files (`.vgp`, `vgp_write`): ≤ 16 cells of 8 m per 32 m chunk, 32 B
   `VgInstance`s made with `vgeo_foliage_instance` (rotation, scale, and wind phase + density hash fixed by the
   position). Generate them from the heightfield and biome masks with your own scatter rules (slope, altitude,
   distance to roads; `vgeo_foliage_scatter_chunk` is the test example). Load and free them with the chunk
   (`render_foliage_chunk_load` / `_unload`; `chunks.c` does it for `world_foliage_test`). Cells are the first
   culling level, so a chunk adds no per-instance CPU work.
3. **Materials.** Pick a WPO snippet per material (`WPO_GRASS`, `WPO_TREE`, `| WPO_DISPLACE` for trampling) and
   its parameters, with `prm.w` the full wind. The declared bound (`vgeo_wpo_max`) inflates every culling
   bound automatically. New snippets go in `wpo.glsl` with their bound in `vgeo_wpo_max` and the mirror in
   `vgeo_wpo_eval`.
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

What stays out of the foliage system: culling, LOD selection, drawing and shadows. They come from the classic
renderer unchanged.
