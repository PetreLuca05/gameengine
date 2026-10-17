# Terrain shading

The open-world ground (`MAT_TERRAIN` chunks and far tiles) is shaded by `terrain_shade`
(`shaders/terrain/terrain.glsl`) from the terrain layer set (`assets/terrain`), a blend map and an
attribute map baked from the material raster at map load, and one parameter block. The art target and
the numbers behind the defaults are in `docs/GROUND_REFERENCE.md`. `r_terrain 0` (or a missing
`assets/terrain/terrain.json`) falls back to the older surface layer blend (`surfaces.glsl`
`surf_terrain`, docs/TEXTURES.md); roads and buildings keep that path.

| | |
|---|---|
| Data | `assets/terrain/terrain.json` (slots, macro, distances, snow line, scatter atlas), `layers/<layer>.json`, `transitions/*.json` |
| Textures | `assets/terrain/tex/<name>_{albedo.jpg,nrh.png,ao.png,macro.png}` + `scatter_ground_*`: `tools/vfxgen/groundgen.py` (recipes `assets/terrain/gen`) |
| Pure C | `src/world/terrain/terrain_params.c` (JSON -> GPU block), `terrain_maps.c` (blend / attribute maps), `terrain_blend.c` (CPU mirror of the shader math), `terrain_world.c` (load / bake / upload / reload) |
| GPU | `src/render/terrain_tex.c` (frame set bindings 16..24), `shaders/terrain/{terrain_params,terrain_noise,terrain_macro,terrain_tile,terrain_weights,terrain_ground,terrain}.glsl` |
| Game | `src/game/game_terrain.c` (level, debug views, hot reload, terrain tool), `src/game/game_ground_ref.c` (reference viewer) |
| Tests | `tests/test_terrain.c` (ctest `terrain`), `tools/tests/ground_tex_check.py` (ctest `ground_textures`), `tools/tests/terrain_strip.cfg`, `tools/tests/ground_match.cfg` + `tools/ground_match.py` |

## Data flow

1. `openworld.c` builds the material raster (1 m) and calls `terrain_world_load`.
2. `terrain_params_load` parses the JSON into `TerrainGpu` (a global block, 16 layer blocks, 64 pair blocks,
   a 16 x 16 pair table) and the texture list (main textures, variants, transition sets, rims; `surf:<name>`
   reuses an `assets/textures/surfaces` layer: asphalt, concrete, crop).
3. `render_terrain_load` loads the textures on the job system into three arrays (albedo + AO in alpha,
   normal / roughness / height at half size, 512 px macro), and the scatter atlas.
4. `terrain_maps_build` bakes the **blend map** and the **attribute map** (below); `render_terrain_maps`
   uploads them with an empty **paint map**.
5. Every frame `terrain_game_frame` feeds the level (`r_terrain_quality`), the debug view and the grass
   term's distances into the block; `terrain_tex.c` copies the block into this frame slot's buffer when it
   changed. The ground colours of the grass shading (`render_ground_set_color`) and the shared macro
   variation (`render_ground_set_macro`) come from the same parameters.

Memory (1024 px layers, 27 textures): about 216 MB (albedo 27 x 5.6 MB, nrh 27 x 1.4 MB, macro 27 x 1.4 MB)
plus the maps (3 x n^2 x 4 B: 3 MB at 513^2, 50 MB at 2049^2). `r_terrain_tex_size 2048` quadruples the layers
(about 860 MB: only for cards with the memory); the lowest texture level loads 512 px.

## Maps

**Blend map** (`terrain_blend`, RGBA8, texelFetch): per raster sample the dominant and the second material
of a (2r + 1)^2 box (r = `blend_radius_m`, 3 m) and their fractions. Across a straight border the fraction
ramps linearly, so `(fa - fb) / (fa + fb) x (r + 0.5 sample)` is the signed distance to the border in metres
(positive on the dominant side): transition widths are in metres, identical at every terrain LOD and camera
distance (they do not depend on the mesh or the pixel footprint). The shader gathers the 4 samples around
the point bilinearly, each giving its fraction to its two materials, merges and sorts (top 2, top 3 on High+).

**Attribute map** (`terrain_attr`, RGBA8, linear):

| Channel | What | How |
|---|---|---|
| R curvature | 0.5 flat, > 0.5 concave (cavities), < 0.5 convex (ridges) | Laplacian of the height box-blurred over 2 m, x `curvature_scale` |
| G moisture | 0 dry .. 1 wet | D8 flow accumulation (log, `flow_ref` cells = 1) x 0.75 + low ground (below the 8 m mean by up to 0.6 m) x 0.5; mud = 1 |
| B wear | 1 on paths, falling over `wear_m` beside them | chamfer distance to `dirt_road` samples |
| A border distance | 0..8 m to the nearest other material | chamfer distance from the border samples |

**Paint map** (`terrain_paint`): tint rgb (0.5 = unchanged) and a variant mask in alpha, painted with the
terrain tool; wear and moisture paint go into the attribute map.

## Techniques

Per layer; each runs from a level up (`r_terrain_quality`, the Engine Fidelity feature `terrain_quality`:
Off 0, Low 1, Medium 2, High 3, Modern 4). `L` = the layer JSON, `T` = terrain.json.

### Breaking the tiling (`terrain_tile.glsl`)

| Technique | Level | Parameters | Notes |
|---|---|---|---|
| Random rotation / mirror per cell ("cells") | 0+ (and beyond `hex_far_m`) | `tiling.cell` | two grids offset by half a cell, each cell rotated by k x 90 deg and mirrored, blended by the distance to the cell edges; 2 taps |
| Hex-tile stochastic bombing | 2+ (`tiling.mode` "hex") | `tiling.cell` (tiles), `rotation` (0..1 of a turn), `scale_jitter` | the 3 hex cells around the point, random offset / rotation / scale each, barycentric weights ^5, **histogram-preserving** blend mean + sum w (t - mean) / sqrt(sum w^2) (keeps mean and variance; a plain average washes the contrast out); tangent normals rotated with their cell; 3 taps |
| Multi-scale | 2+ | `multiscale.far1` (0.23), `far2` (0.07), `strength` | two albedo taps at 1 / 0.23 and 1 / 0.07 of the scale, rotated, as luminance ratios in world-noise patches |
| Distance uv scaling | all | `multiscale.halve_at_m` | the uv scale halves over [d, 1.5 d] (crossfade): far ground shows larger features, fewer repeats |
| Detail / macro split | all | `macro_scale_m`, `T.macro.texture_strength` | the layer's 512 px macro map (colour multiplier, tileable over tens of metres) under the near detail |

### Colour and tone (`terrain.glsl ter_layer_color`)

| Technique | Level | Parameters |
|---|---|---|
| Macro texture | all | `macro_scale_m`, `T.macro.texture_strength` |
| Global macro field (50..500 m fBm, hue / saturation / value; shared with the grass) | all | `T.macro`: `scale1_m`, `scale2_m`, `hue_deg`, `sat`, `val`, `strength`; layer `macro_strength` |
| Per-layer HSV jitter by world noise | 1+ | `hsv_jitter`: `hue_deg`, `sat`, `val`, `scale_m` |
| Slope tint | 2+ | `slope_tint`: `tint` (rgb multiplier), `amount`, `from` / `to` (normal y) |
| Altitude tint | 2+ | `altitude_tint`: `tint`, `amount`, `from_m`, `to_m` |
| Curvature (cavities darker, ridges lighter) | 3+ | `curvature`: `cavity`, `ridge`; `T.curvature_strength` |
| Moisture tint / darkening (flow map) | 3+ | `moisture`: `tint`, `amount`, `darken` |
| Aspect (north-facing) | 4 | `aspect_tint`: `tint`, `amount` |
| tint_by_weight (the minor material stains the major one inside the band) | 3+ | pair `tint_by_weight` |
| Paint tint | all | terrain tool |

### Blending quality

| Technique | Level | Parameters |
|---|---|---|
| Height-based blending | all | `blend`: `height_contrast`, `sharpness`, `height_offset` (a material outside its band never wins on height) |
| Weight-noise breakup | all (1 octave), 2+ (3 octaves) | pair `edge_noise_scale_m`, `edge_noise_strength_m` |
| Top-N weights | 0-2: 2, 3+: 3 | |
| Triplanar on steep ground | 2+ (normal y < 0.7, within 120 m) | the dominant layer's side projections |
| Sub-layer variants by mask | 3+ (top two layers, one variant each) | `variants`: `texture`, `mask` (noise / moisture / curvature / slope / paint / dry), `lo`, `hi`; `variant_noise` |
| Rock exposure on steep ground | 2+ | `expose_below` (normal y), `expose` |
| Scatter layer (pebbles, leaves, twigs, grass bits) | 3+ (within `distances.scatter_m`) | `scatter`: `per_m2`, `cell_m`, `sprites` (atlas cells), `size_m`; atlas `T.scatter` |

### Surface detail

| Technique | Level | Parameters |
|---|---|---|
| Parallax occlusion (dominant layer, one offset for every tap) | 3+ (within `distances.parallax_m`, faded) | `detail.parallax_m` (depth), 8..16 steps by view angle |
| Detail normals | 4 (within `distances.detail_m`) | `detail.detail_normal` |
| Micro roughness | 2+ | `detail.micro_roughness` |
| Normal strength | all | `detail.normal` |
| Tessellation | not implemented | parallax covers the near relief; a tessellated near-chunk pipeline would be needed (see "Limits") |

### Weather (`weather.glsl`, after the terrain)

The terrain sets `g_wx_porosity` (the layers' `porosity`, wet darkening), `g_wx_puddle` (puddles where the
moisture map is high and low ground, plus the pair's `puddle_bias` inside the band: puddles start at low
dirt / grass borders, also outside the level's baked puddle mask) and `g_wx_snow` (the layers'
`snow.accumulation` x the snow line: `T.snow.line_m` with a `band_m` band broken by `noise_m`, plus the
pair's `snow_bias`: snow settles first on grass). Every other surface keeps the defaults (identical
results).

### Grass term beyond the grass fade

Where grass grows (each material's grass coverage, `frame.ground_color[].w`, from the grass assets'
densities), the terrain blends toward the grass tops' colour (`frame.ground_top`, macro-varied like the
grass) from where the grass starts thinning to where it ends at the current level (`render_grass_fade`).
The grass and the ground under it read the same macro field and ground colour (docs/FOLIAGE_GRASS.md), so
the fade line does not show.

## Transitions (`terrain_weights.glsl`, per material pair)

Never a soft linear crossfade: the signed distance d (m) from the blend map, shifted by the pair's
techniques, goes through `smoothstep(-W/2, W/2, d)` (W = `width_m`), then height blending decides inside
the band. Pairs are `transitions/*.json` with `a` / `b` slot names or lists (families; later files override
earlier ones; `_default.json` is pair 0 for every pair without a file). The pair applies both ways; its
first material is `a` (directions and biases refer to it).

| Mode (`modes`) | Level | Parameters | What |
|---|---|---|---|
| `height` | all | the layers' `blend` | taller material wins inside the band |
| `noise` | all (1 octave), 2+ (3) | `edge_noise_scale_m`, `edge_noise_strength_m` | multi-octave world noise moves the border |
| `crumbs` | 2+ | `crumbs` (0..1), `T.crumb_m` | small islands of each material inside the other |
| `set` | 3+ | `set` (texture), `set_strength`, `set_tile_m` | a transition texture (sparse grass on dirt, pebble band, worn path edge, wet mud edge, sand on rock, mossy rock edge) weighted 4 t (1 - t): only inside the band |
| `rim` | 3+ | `rim` (texture), `rim_width_m` | a lighter lip on b's side past the band (crater rims) |
| `scatter` | 3+ | `scatter_sprites`, `scatter_per_m2` | border scatter: pebbles at rock / dirt, dead grass at grass / dirt |
| `directional` | 4 | `direction` (`wind` / `downhill`), `direction_m` | a drifts downwind (the weather's wind) or downhill |
| `slope` | 4 | `slope_bias_m` | a spills downhill on slopes |
| `wear` | all (+ curvature at 4) | `wear_bias_m` | paths: packed centre -> worn edge -> grass |
| `moisture` | all | `moisture_bias_m` | moisture moves the border (grass -> mud spreads where wet) |
| | 3+ | `tint_by_weight` | b's colour stains a inside the band |
| | all | `puddle_bias`, `snow_bias` | weather at the border |

Band widths are in metres and do not depend on the terrain LOD (test: within 10 % at sampling steps from
1 cm to 20 cm). The blend radius bounds the widest band: W / 2 + shifts should stay under the radius
(3 m).

### Test strip

`map terrain_yard` (`data/levels/terrain_yard.json`): twelve rows, one straight 64 m border per pair (west
`a` | east `b`), on a grass base, the rock rows on a low hill. `tools/tests/terrain_strip.cfg` captures three
overviews, the band view (`r_terrain_debug 7`) and the border distance (`11`). `terrain_pair <name> modes
<bits>` / `terrain_pair <name> <a..e> <i> <value>` toggle a pair's techniques live.

## Levels and cost

Texture fetches per pixel, worst case (the blend map's 4 fetches, the attribute and paint maps 1 each, the
layer means 2 cheap last-mip taps per layer included):

| Level | What runs | Fetches | Estimate (1080p full screen, RTX 3060 Ti class) |
|---|---|---|---|
| 0 Off | 2 layers, cells, macro texture + field, height blend, 1-octave borders | about 18 | 0.25-0.35 ms |
| 1 Low | + HSV jitter | about 18 | 0.3-0.4 ms |
| 2 Medium | + hex bombing, multi-scale, slope / altitude tint, crumbs, 3-octave borders, triplanar on steep | about 30 (34 on steep) | 0.55-0.75 ms (budget 0.8) |
| 3 High | + 3 layers, variants (top two), curvature / moisture, parallax (8-16), scatter, transition sets, rims | about 75 near, 45 beyond the parallax / scatter range | 1.2-1.6 ms |
| 4 Modern | + detail normals, directional / slope / curvature-aware borders, aspect | about 77 near | 1.4-1.9 ms (budget 2.0) |

These are estimates from the fetch counts; this branch was verified on a software renderer (lavapipe), which
gives no GPU timings. Measure on the target GPU with `tools/tests/perf_compare.cfg`-style captures at each
level (`set r_terrain_quality N`, `r_perf_overlay 1`) on `map vegetation`. The near-field cost is dominated by
parallax (lower `distances.parallax_m` or the layers' `detail.parallax_m`) and the variants.

## Debug views (`r_terrain_debug`)

1 material weights (a colour per slot), 2 bombing cells, 3 macro field, 4 curvature, 5 moisture, 6 variant
masks, 7 transition band (orange: set weight), 8 tiling frequency (tile repeats per 100 px: green few, red
many), 9 scatter, 10 grass term, 11 signed border distance (red a, blue b). `r_grass_debug` has the grass's.

## Tool and commands

| Command / cvar | |
|---|---|
| `r_terrain_quality` | 0..4, bounded by Engine Fidelity (Settings > Graphics > Terrain detail) |
| `r_terrain`, `r_terrain_tex_size`, `r_terrain_debug`, `r_terrain_watch` | on / layer size / debug view / hot reload (1 s poll of the JSON) |
| `terrain_reload`, `terrain_status` | reload now (textures only when their list changed) / slots, textures, means |
| `terrain_set <slot> <field> <i> <value>` | a layer parameter live (the fields of `TerLayerGpu`) |
| `terrain_pair ...` | a transition live |
| `terrain_paint <tint_r\|tint_g\|tint_b\|tint\|variant\|wear\|moisture> <value> [radius] [strength]`, `terrain_paint_save [file]` | paint brush at the camera, save the painted map |
| `ground_ref`, `ground_ref_cam`, `ground_sample` | reference viewer, camera presets, colour sampler (docs/GROUND_REFERENCE.md) |

## Tuning guide

1. Start at the strip (`map terrain_yard`) with `r_terrain_debug 7`: set each pair's `width_m` and edge noise
   until the band reads as in the reference (grass -> dirt: 0.3-0.8 m, crumbly); then 11 to see the shifts.
2. Colour: match the layer means first (they come from the generated textures' `meta.json`; regenerate or
   change `target_mean` in the recipe rather than tinting), then the macro field strength
   (`T.macro`: 7 deg hue, 8 % value read like the reference's green / straw patches), then HSV jitter.
3. Repetition: `r_terrain_debug 8` shows where a texture repeats densely; raise the tile size or the hex
   `scale_jitter` there, and `multiscale.strength` for large flat areas.
4. Relief: `detail.parallax_m` 0.02-0.05 m on soil and rock, 0 on mud / sand; `height_contrast` 1-1.5 on rock and
   gravel so stones poke through dirt.
5. Check the grass seam (`r_terrain_debug 10`) at each level: the term must reach full strength where the
   grass ends.
6. Edits hot-reload (`r_terrain_watch 1`) from the source tree's `assets/terrain` while running from
   `build/bin` (the data path falls back to the source tree only for missing files: edit the copy in
   `build*/bin/assets/terrain` or rebuild).

## Limits

- No tessellation: the terrain is a legacy indexed draw; a tessellated near-chunk pipeline (hull / domain
  shaders on the near LOD) is the remaining item for Modern. Parallax gives the near relief meanwhile.
- The layer arrays are uncompressed RGBA8 (no BCn encoder in the pipeline): 1024 px by default.
- One transition per pixel (the top pair); at three-material junctions the third material blends by weight
  and height only.
- The paint map is not saved with the map yet (`terrain_paint_save` writes it; loading it at map load is
  a follow-up).
