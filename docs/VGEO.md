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
| 2. GPU culling / LOD selection / Hi-Z | — |
| 3. Visibility buffer raster + resolve | — |
| 4. Streaming | — |
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

The builder is pure C with no renderer dependency. It is compiled into the game (through the `src/` glob), the
`dg_vgeo` static library (tool and tests), and nothing else.

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
on `Model.vgeo`, and the GPU path uses it from Phase 2.

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
