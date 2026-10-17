# Foliage wind

Vegetation moves the way Tree It intends: the procedural vertex animation of **GPU Gems 3, chapter 16,
"Vegetation Procedural Animation and Shading in Crysis"** (Tiago Sousa, Crytek), driven by the wind
weights Tree It bakes into the vertex colour, and fed by a real wind field. The field has gust fronts
that sweep across a forest, lulls, turbulence and local sources. Everything else that cares about the
wind (particles, audio, the lens droplets, `weather_at`) samples the same field.

| Part | Where |
|---|---|
| Wind field: gust fronts, lulls, turbulence, sources, render tile + settled-bend springs, `wind_at` | `src/world/wind.c` / `.h` (pure, unit tested) |
| Vegetation model, CPU side: three layers, normals, bounds, per-asset parameters | `src/render/vgeo/foliage_wind.c` / `.h` (pure) |
| Vegetation model, GPU side (every raster path, the shadows, the resolve) | `shaders/vgeo/wpo.glsl` (`wpo_veg`), `vg_decode.glsl` (`vg_wind_at`, `vg_wpo_n`, `vg_vertex_normal_world`) |
| Tree It import: wind channels kept / inferred, channel detection, per-species wind tuning | `src/render/vgeo/vgeo_tree.c` / `.h` (the one tree import, docs/VGEO.md "Tree models": species table, heights, textures, cache) |
| Wind tile on the GPU (ring of 3 images) | `src/render/vgeo/foliage_support.c` (`vg_wind_*`) |
| Game glue: the field per frame, consumers, sources, debug, tuning panel | `src/game/game_wind.c`; hooks in `game_weather.c`, `game_render.c`, `game_vehicles.c` |
| Tests | `tests/test_wind.c` (ctest `wind`), `tests/test_vgeo_foliage.c` (ctest `vgeo_foliage`), `tools/tests/wind.cfg` |

## Tree It vertex colours

Tree It bakes no animation. Its exports carry per-vertex wind data in the vertex colour, and its
documentation points at the Crysis chapter for the shader. The layout was verified from the files
themselves. Every export in `assets/treefbx/` was read with ufbx: 11 FBX and 2 OBJ, the OBJs as
`v x y z r g b a`. For each part (bark vs leaf cards) the probe measured:
- value ranges;
- variance within each connected piece (a branch, a card);
- correlation with height, distance from the trunk axis, and the card's uv;
- whether the cards' values also appear on the bark.

| Channel | Bark | Leaf / branch cards | Meaning |
|---|---|---|---|
| **R** | exactly 1/255 everywhere | 0 at the stem end, up to 0.46–0.80 at the tip; varies within every card (within-card variance fraction 0.92–1.00) | **edge (leaf) flutter weight** |
| **G** | constant per branch piece (within-piece variance 0.00), 0 on the trunk | constant per card, and from the same set as the bark values: 12/12 to 47/47 of the leaves' G values exist on the bark (±1/255) | **per-branch phase**: a card inherits its branch's |
| **B** | 0 on the whole trunk, rising along every branch (correlation with the distance from the trunk axis +0.97/+0.98 on 6 exports) | about the value where the card is attached | **branch bend weight** |
| **A** | 1.0 (maple: a constant 0.40 / 0.46) | same | unused |

Trunk bending is not a channel: it comes from the vertex height, as in the chapter. This is the
chapter's own layout (R edge stiffness, G phase variation, B overall stiffness). It also matches the
community Godot port (x flutter, y phase, z branch).

Two things differ from what the community ports suggest:
- **G is a branch ID, not a per-vertex random phase.** The per-vertex phase comes from the vertex
  position, which is also how the chapter does it.
- **B is not normalized per tree.** Its maximum is 0.10 on pine and juniper but 1.0 on beech. The
  import scales it to 1 (`normalize_branch`, recorded as `branch_scale`), so `branch_stiffness` means
  the same thing for every species.

**Automatic detection** (`vgeo_tree_detect_channels`, run over every species by `tests/test_wind.c`)
decides from the data alone:
- **phase** is the varying channel most constant per piece;
- **flutter** is, of the others, the one that is about 0 on the bark and about 0 at one end of most cards;
- **branch** is the remaining one.

It reports R flutter, G phase, B branch for all 13 exports. When two channels both look like flutter
(a plant whose only bark is the trunk, where B is 0 too) the detector keeps the Tree It order and says
so in its report.

### Canonical layout

The engine stores one layout in its meshes: **R flutter, G phase, B branch**, with trunk bending from
height. The procedural grass and wheat (`vgeo_procgen_foliage.c`) write it directly. The tree import
(`vgeo_tree_import`, docs/VGEO.md "Tree models") keeps Tree It's colours as they are (the layout is the
same) and scales B to a maximum of 1 per tree (`VgTree.branch_scale`), so `branch_stiffness` means the
same thing for every species. A mesh in another layout (SpeedTree-style: branch in R, flutter in B)
needs a remap in the import before it is added to the species table.

**Without vertex colours** (or `VgTreeSpecies.infer_wind`, which the tests use), the import infers
every channel:
- **flutter** runs along each card from its trunk-side end, 0 to 0.8, on cards only;
- **phase** is shared by the pieces within about 1.5 m (a hash of the piece centroid);
- **branch** is the distance from the trunk axis divided by the crown radius.

Parts are bark (opaque, rigid) or leaf cards (masked, two-sided). As everywhere in the tree import, a
material is a card when its texture cuts out (more than 2 % of the texels below half alpha). Pieces are
connected file vertices (a branch, a card).

## The model (as implemented)

All three layers run per vertex inside `vg_vertex_world`. That is the one vertex path of both
rasterisers (HW mesh / vertex pulling, SW compute), the sun shadow cascades, and the resolve.
`wpo_veg` in `wpo.glsl` is the GPU side; `vgeo_wind_eval` mirrors it operation for operation. The
inputs are:
- the undisplaced world position *p*, the world normal *n*, the instance pivot *ip* and scale *s*;
- the wind weights;
- the instance phase (a position hash);
- the wind field at the pivot, *W* = mix(instant, settled, `settle`);
- the time.

**Response.** r = clamp(|W| / `full_wind`, 0, 1)^`response`. Small plants use an exponent below 1
(they saturate early); big trees use one above 1 (they need strong wind).

**1. Main bending (trunk).**
- h = clamp((p − ip).y / (H · s), 0, 1), where H is the asset height.
- The bend vector is b = d · `trunk_stiffness` · r · (0.88 + 0.12 · sway) plus a cross-wind term of
  0.08 · `trunk_stiffness` · r · sway′.
- The sways are the chapter's smoothed triangle waves at `main_freq`: two incommensurate ones along
  the wind, one across it.
- |b| is capped at tan(`max_bend_deg`).
- The offset (b.x, 0, b.y) · h² · H · s is added, then the position is **normalized back to its
  original distance from the pivot** (the chapter's rescale, so nothing stretches).
- One deliberate difference from the chapter: the profile is the requested pure (h / H)², not the
  chapter's (1 + a)⁴ − (1 + a)². The square has zero slope at the root, so the base never pivots.
- Distance to the pivot is preserved to 1e-7 for every vertex. Trunk arc length is within 0.04 % for
  trees and 0.2 % for bushes at max bend. A strongly bent grass blade (23°) stretches 2.4 % along its
  arc, because the projection keeps chord lengths, not arcs.

**2. Detail bending (branches) and 3. edge flutter.** These use the chapter's four frequencies and
smoothed triangle waves:
- Phases (in cycles):
  - object phase = ip · (0.173, 0.091, 0.137) + 7 · instance phase;
  - branch phase = object phase + 3 G;
  - vertex phase = branch phase + 1.7 · (p − ip) · (1, 1, 1) / s.
- Waves:
  - a = t · `flutter_freq` + vertex phase, b = t · `detail_freq` + branch phase;
  - w = STW(1.427a, 0.573a, 1.320b, 0.680b), where STW(x) = SmoothCurve(TriangleWave(x)) · 2 − 1;
  - the four factors are the chapter's 1.975 / 0.793 and 0.375 / 0.193, normalized so that each
    layer's frequency is the mean;
  - sum = (w.x + w.z, w.y + w.w) / 2.
- **Detail:** B · `branch_stiffness` · r · s · (d · (0.4 + 0.3 sum.y) + up · 0.6 sum.y). This is
  mostly along the wind, with a vertical bounce (the chapter's branch bend is vertical only).
- **Flutter:** R · `leaf_stiffness` · rf · s · sum.x along the normal (the chapter's edge flutter).
  rf = smoothstep(0.3, 2.5 m/s, |W|) · (0.35 + 0.65 · lin), so it damps at very low wind.

**Uniqueness.** Each instance gets ±15 % frequency variation and ±15 % trunk / branch stiffness
variation from its phase. With the position-based phases, neighbours don't move in sync:
- pooled correlation over 32 neighbour pairs (2.5 m apart) × 600 frames is 0.09 for main bending,
  0.07 for detail, 0.01 for flutter, 0.07 for everything together;
- a single pair of 0.2 Hz sways over 10 s correlates by |cos(phase difference)| whatever the model,
  so per-pair values are reported but not asserted.

**Normals.** These are evaluated in the resolve for the three vertices of the pixel's triangle
(`vg_vertex_normal_world`), High / Modern only:
- main bending rotates the normal about cross(up, b) by atan(2 h |b|), the slope of the bent stem;
- flutter tilts it towards the wind with a quarter-period-shifted wave (leaves catch the light as
  they flap);
- the GPU normals match the CPU mirror to 4e-6.

**Displacement buffer first.** Trample and pushes from players, vehicles and blasts push by h (the
bent part) and take the wind out of the pushed plant: k = 1 − 0.85 · |push| / 0.6 m scales r and rf.
A player walking through a bush is never cancelled by the wind. Grass and bushes use the buffer;
trees don't.

**Motion vectors.** The resolve evaluates the previous position with last frame's wind tile (its own
window origin), last frame's fallback wind and last frame's time; nothing is re-evaluated at the
current time. It matches the CPU mirror to 1e-6 m (test).

**Bounds.** `vgeo_wind_layer_max` gives each layer's maximum at instance scale 1:
- main ≤ f(|b|max) · |b|max · H, where |b|max = min(1.15 · 1.0032 · trunk, tan max_bend) and
  f(q) = 2 sin(asin(q) / 2) / q. This is the chord the pivot-distance projection can move a vertex.
- detail ≤ 0.922 · 1.15 · branch;
- flutter ≤ leaf.

The mesh's `max_wpo` is the sum (plus 0.67 m with the displacement buffer). Every culling sphere
grows by `max_wpo` · max(instance scale, 1) (`vg_wpo_margin`, the same on CPU and GPU, and in the
impostor switch). Measured worst use at max wind: 0.43–0.72 of the bound over every species and
preset. Zero vertices fall outside on the GPU.

## Wind field

`WindField` (`src/world/wind.h`) is built every frame on every machine from the replicated weather
(`wind_dir`, `wind`, `gust`) and the interpolated server tick. Every machine computes the same
`wind_at` with **no new network traffic and no protocol change**. Its parts:

- **Gust fronts**
  - 2-octave value noise in u = (s − U t) / 90 m, where s is the coordinate along the wind and U the
    mean speed, so a crest travels downwind at exactly U (test: the shift that best explains the
    pattern after 2 s is U ± 0.5 m/s, residual 0);
  - across the wind the fronts are 2.5× longer and wavy;
  - crests raise the speed up to about 1.9× at gust 1, troughs are lulls down to about 0.35×;
  - calm weather keeps light gusts (0.15 + 0.85 · gust).
- **Turbulence**
  - 3D value noise at 38 m, advected with the mean wind and slowly evolving;
  - ±(0.22 + 0.33 · gust) rad of direction, ±15 % of strength, and a small updraft.
- **Sources**, local, not replicated (each machine adds its own from replicated events):
  - **blast:** a ring expanding at 60 m/s, decaying quadratically. `wind_game_blast` also flattens
    the grass through the displacement buffer.
  - **downwash:** a helicopter stub; a radial push while alive.
  - **wake:** vehicles, re-added every 0.1 s while moving; a 3-radius tail behind.
  - **gust:** a triggered front band sweeping downwind (tuning, `wind_gust_now`).
- **Render tile** (`wind_tile_update`)
  - 64 × 64 texels of 4 m (256 m), toroidal around the camera, RGBA16F: instant wind x, z and
    settled bend x, z;
  - the settled bend is a damped spring per texel: period 3.2 s, damping 0.45, so it lags a gust,
    rebounds once (max 1.05–1.6× the instant value in the test) and settles;
  - texels entering the window start settled; texels that stay keep their spring across window
    moves (test).
- **The shader** samples the tile at each instance's pivot (`vg_wind_at`):
  - REPEAT addressing at uv = world / 256 m;
  - blended over the outer 2 texels into the field outside the window (the mean wind and its
    spring, `VgViewGpu.wind`);
  - `wind_tile_sample` is the CPU twin; the GPU sample matches it within texture precision
    (test, 155 904 samples).
  - Per asset, `settle_time` (0 .. 3.2 s) mixes the instant and settled values: grass follows
    gusts at once, big trees lean late and rock back.
- **GPU:** a ring of 3 images with fixed texture-table entries (`vg_wind_*`).
  - Frame k writes image k and reads k and k − 1 (velocity).
  - With 2 frames in flight, image k + 1 is never touched.
  - No descriptor changes per frame.

**Consumers** (`wind_game_at`):
- `weather_at().wind`: the local wind × exposure;
- the particle wind (`vfx_game_set_environment`, a constant before this);
- the lens droplets (the wind's component along the camera's right pushes them sideways);
- the wind ambience: the local wind, gusts audible as they pass, and `wind_game_rustle`, which is
  louder among foliage;
- `wind_game_leaf_rate`, a ready rate for leaf-shedding effects (none exist yet);
- cloth and hair can use the same query later.

**Slow motion.** `wind_timescale` scales the wind's time and the springs. It applies on this machine
only (1 = in step with everyone).

## Grass and bushes

Grass and bushes use the same model with different weights:
- **Grass (tufts, crops, daisy):** main bending on the blade's own height (H = blade height) and
  flutter towards the blade tips. R runs 0 at the root to 1 at the tip; no branches.
- **Bushes:** main bending plus detail (B = distance from the stem).

Both respond to the displacement buffer first.

## Parameters

Per species, `k_wind` in `vgeo_tree.c` (`vgeo_tree_wind`). Procedural kinds set them in
`vgeo_foliage_materials`. Values that are left out (0) take the preset's defaults (`vgeo_wind_defaults`):
grass for the flowers, tree for the trees and bushes, at the species' scale-1 height (the middle of
its height range):

| Parameter | Meaning | grass | bush | tree |
|---|---|---|---|---|
| `trunk_stiffness` | main bend gain: tan(lean) at full response. Gain, not resistance: higher moves more (the brief's / Crysis's naming) | 0.45 | 0.12 | 0.05 |
| `branch_stiffness` | branch amplitude (m, branch weight 1, full response) | 0 | 0.08 | 0.25 |
| `leaf_stiffness` | flutter amplitude (m, flutter weight 1) | 0.02 | 0.03 | 0.05 |
| `full_wind` | m/s at full response | 12 | 16 | 22 |
| `main_freq` / `detail_freq` / `flutter_freq` | Hz | 0.35 / 1.2 / 3.5 | 0.3 / 1.4 / 5 | 0.18 / 1.1 / 6 |
| `max_bend_deg` | main bend cap (lean of the tip) | 35 | 14 | 7 |
| `settle_time` | s, 0 instant .. 3.2 the settled spring | 0.6 | 1.5 | 2.6 |
| `response` | (wind / full)^response | 0.6 | 0.8 | 1.4 |
| `height` | m, the main bend's H (the species' scale-1 height; scaled by the instance) | | | |

Tuning guide:
- Lean too small in a storm: raise `trunk_stiffness`, or lower `full_wind`.
- The crown wobbles as one piece: raise `branch_stiffness`.
- Leaves look dead: raise `leaf_stiffness` and `flutter_freq`.
- The tree reacts to every puff: raise `response` and `settle_time`.

The table tunes Tree It by size and build. Old oaks are stiff (0.03–0.035, max 5°). Birch is
slender (0.07, max 9°, quick flutter). Conifers get drooping branches (0.35–0.4) and stiff needles
(flutter 4 Hz). Bamboo is very flexible (0.12, max 14°, quick settle).

Per material, the shader reads `wpo_params` (trunk, branch, leaf, full wind) and `wind_params`
(height, main / detail / flutter Hz | tan max bend, settle mix, response). `VgMaterial` is 128 B.

## Species and import

The species, their heights, textures, impostors, capsules and caches are the tree import's
(docs/VGEO.md "Tree models": `vgeo_tree.c`, `.dgtree` files built by `build_vgeo --trees`,
`assets/cache/<name>.tree.<key>.dgc` at run time). The wind adds to it:
- the vertex colours become the canonical weights (above); `VG_TREE_VERSION` 4 rebuilt the caches;
- the materials (`vgeo_tree_materials`) pack the species' wind parameters: trees and bushes are
  `WPO_TREE` (trunk capsules, no trampling), the daisies `WPO_GRASS | WPO_DISPLACE` (walked through);
- every species stays inside its declared bound at max wind (`ctest wind`, worst use 0.49-0.79).

Where they appear:
- **Devroom:** `foliage_grove [spacing]` places every imported species (trees, bushes) three times
  across the foliage yard (x 80..360, z 130..), daisies between them, with trunk capsules.
- **Maps:** the open-world scatter picks the species by role and position hash
  (`openworld_foliage.c`, docs/ZOMBIE_MAP.md "Vegetation showcase").

Not used yet: the Tree It normal, roughness and transmission maps (the vgeo material has no slots for
them), and the chapter's shading half (translucency, edge AO).

## Fidelity tiers

`r_foliage_wind` is the Engine Fidelity feature `foliage_wind`:

| Level | Default and max | Layers |
|---|---|---|
| Off, Low | 1 | main bending only |
| Medium | 2 | + detail bending |
| High, Modern | 3 | + leaf flutter, bent / fluttering normals, the gust tile everywhere |

The layers are a uniform bitmask in the view (`wind_misc.z`), so every tier is the same pipelines.

## Debug and tuning

| Control | What |
|---|---|
| `wind_debug 1` | wind field arrows on an 8 m grid around the camera (colour = gust factor), the next gust front (a line across the wind, its distance and arrival time), every foliage instance's lean within 30 m (a leaning stem, its angle) |
| `wind_view 1` / `2` | the canonical wind channels as the albedo (R flutter, G phase, B branch) / the main bend amount (black .. red at max) |
| `wind_solo 1..3` (or `r_foliage_wind_solo`) | one layer only: main, detail, flutter |
| `wind_timescale` | slow motion |
| `wind_dir`, `wind_speed`, `wind_gust` | override the weather (-1 = the weather's) |
| `wind_gust_now [m/s]`, `wind_blast [r] [m/s]`, `wind_downwash [s]` | sources |
| `wind_info` | the field at the camera |
| `wind_panel` | the tuning panel. Readouts, override, gust trigger, time scale, layer solo, views, field arrows, the tile as an overlay (32 × 32: brightness = speed, hue = settled direction). The foliage type picker shows every wind parameter as a live slider (`render_foliage_wind_set`: materials re-uploaded next frame; a bound that grows re-commits the scene). |

## Tests and results

**`ctest wind`** (`tests/test_wind.c`, CPU, 123 checks):
- **Wind field:** determinism; the direction convention and mean speed; `WIND_MAX_SPEED`; gust
  fronts travel at the wind speed; a triggered front moves at its speed.
- **Tile:** equals `wind_at` at texel centres within half precision (2e-3 relative); interpolates
  between them; falls back outside the window; starts settled; keeps its springs across window moves.
- **Spring:** lags, rebounds once, settles.
- **Sources:** the blast ring, wakes.
- **Model:**
  - main bending keeps the pivot distance (1e-7) and the trunk arc (trees and bushes 1 %, grass 3 %:
    2.4 % measured);
  - each layer's amplitude doubles with its stiffness (detail and flutter exactly; main to 2 % at
    small bends, horizontally);
  - neighbours are decorrelated (pooled correlation < 0.2);
  - the declared bounds hold for 20 000 random vertices, scales, winds (to 2× full), times and
    pushes per preset;
  - normals stay unit and tilt downwind;
  - the smoothed triangle wave.
- **Import:**
  - channel detection over all 13 Tree It exports (every species of `vgeo_tree.c`) reports R flutter,
    G phase, B branch;
  - pine imports at its species height with B normalized to 1 and no flutter on bark;
  - inference without colours (birch with `infer_wind`).
- **Species:** every species imports with its channels and a wind height equal to its species height;
  at max wind (30–60 m/s, any direction, random time / scale / phase) every vertex stays inside its
  bound (worst use 0.49–0.79).

**`ctest vgeo_foliage`** (`--validation`, 501 checks): the GPU WPO kernel (`vg_test_wpo.comp`) against
the CPU mirror. This covers 128 wind / time / displacement-buffer / tile states, every vertex of every
cluster of every level of 8 instances:
- positions 1e-6 m;
- previous-frame positions (motion vectors) 1e-6 m;
- bent normals 4e-6;
- the tile sample against `wind_tile_sample`, exact on llvmpipe (tolerance 0.02 m/s + 0.4 %), and
  checked to differ from the mean wind (the tile was really sampled);
- 0 displaced vertices outside their cluster or instance spheres;
- 0 validation messages.

**`tools/tests/wind.cfg`** (`--validation`):
- the grove and 30 000 grass / bushes;
- every `r_foliage_wind` tier, both debug views, `wind_debug` with a triggered gust, a blast, slow
  motion, the panel;
- 401 frames, 0 validation messages (llvmpipe, Linux; RTX 3060 Ti, Windows, VVL).

The `vgeo_uv_plane_lod` fix (below) keeps the `vgeo_foliage` masked-raster checks passing.

First written on Mesa llvmpipe (Vulkan 1.4, software) in a Linux container; merged onto the tree
import and run on Windows (RTX 3060 Ti): every ctest passes, `wind.cfg` and `map vegetation` in a
24 m/s wind with `--validation` give 0 messages, `tools/tests/tex_veg.cfg` / `tree_scale.cfg` keep the
tree sizes and textures, `r_vgeo 0` is unchanged (the classic path draws no foliage).

**Cost** (`map vegetation`, 1600 x 900, the same cameras, `r_foliage_wind 0` vs `3` in one run,
`post_bench` "all passes" minus "post"): about +0.4 ms (broadleaf edge 1.55 -> 2.0 ms, pine edge 1.45
-> 1.87 ms). The larger declared bounds (`max_wpo`: 0.8-2 m on the trees) also shrink the LOD distances
a little: about 15-20 % more triangles in foliage-heavy views.

### Fixed on the way

`vg_uv_plane_lod` picked the mask mip of a masked triangle at its vertices' screen centroid. For a
card crossing the camera plane, the flipped projection of a vertex behind the camera put that point
anywhere. The coarsest mip got picked, and its averaged alpha passes the cutoff, so the whole card was
drawn opaque: black slabs next to the camera under AO. Tree It's 2–3 m conifer cards hit it. Now:
- the point is the centroid of the vertices in front of the camera, clamped to the screen;
- the plane's horizon gives LOD 0;
- the LOD is capped at the 4 × 4 mip;
- the CPU mirror in the test does the same.
