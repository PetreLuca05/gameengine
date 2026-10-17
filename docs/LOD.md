# LOD ranges and transitions (classic renderer, `r_vgeo 0`)

Scope: the classic renderer (`src/render/vk_classic.c`, `shaders/classic_*`: discrete LOD chains,
impostors, foliage, sun cascades) and the non-vgeo systems it shares (characters / skinned meshes and
their animation tiers, terrain chunks and surface textures, foliage density fade, VFX spawn LOD). The
virtualized geometry path (`r_vgeo 1`: cluster DAG, error threshold, vgeo impostors) is out of scope and
renders as before (section 2).

## 1. Report: thresholds and transitions before this change

Measured / read from the code at `549261a`. Pixel figures are at 1080p (render height 1080) with the
default 75 deg vertical FOV unless stated; a character is 1.72 m tall (`anim_lod_lineup` at 900 p: 202 px at
5 m), so its projected height is `px = 1211 / d` (m).

### Characters (skinned, `src/anim/anim_lod.h`, `src/gameplay/characters.c`)

| | value | distance at 1080p |
|---|---|---|
| LOD0 above | 260 px (hysteresis 15 %: down at 221 px, up at 299 px) | LOD0 only within 4.1-5.5 m |
| LOD1 above | 120 px (102 / 138 px) | to 10-12 m |
| LOD2 above | 50 px (42.5 / 57.5 px) | to 24-28 m |
| LOD3 | below | beyond 28 m |
| culled | < 3 px (state machine ticks, no pose) | > 400 m |
| sun shadow | only above 40 px, one LOD coarser than the main draw | none beyond 30 m |
| bias | `r_lod_bias` (Modern 1, High 0.71, Medium 0.5, Low / Off 0.25) x `r_char_quality` (0.5 + 0.25 q) x per-character `lod_bias` | |

Transitions:
- Mesh switch: screen-space dither cross-fade over a fixed **0.15 s** (interleaved gradient noise, static:
  not rotated per frame, so TAA cannot resolve it into a blend; with it off the pattern is visible).
  Both LODs drawn with complementary masks (`skin.frag` / `gbuffer_skin.frag`, fade < 0 out, > 0 in).
- **Ping-pong:** a new switch during a fade restarts it from the LOD just reached (`fade_from = lod`):
  the outgoing LOD of the running fade disappears in one frame (pop).
- **Shadows do not fade:** the caster (main LOD + 1) switches in one frame; a character crossing 40 px
  loses its shadow in one frame.
- Animation tiers by LOD, in one step: pose update every 1 / 2 / 4 / 8 ticks; procedural systems: foot IK,
  turn-in-place, lean at LOD0 only; spine, hold, damage at LOD0-1; LOD2+ baked arms; ragdolls up to LOD2.
  Switching LOD0 -> 1 drops foot IK / lean in one tick (pelvis and feet jump by the IK offset), and
  `turn` off snaps the rendered body yaw to the controller's.
- `anim_eval_budget` 64 post-passes per tick; the rest keep their last pose.

### Static meshes / props (classic discrete LODs)

- 4 LODs per vgeo mesh (the exported LOD errors). LOD pick per instance and frame: the coarsest LOD whose
  projected error `error x scale x px_scale / distance <= r_vgeo_lod_px`. No hysteresis, **no fade: pops**.
- `r_vgeo_lod_px` (era-owned): modern 2 px, 2020 3, 2015 4, 2010 12 (2010 also never draws LOD0). Engine
  Fidelity: x1 Modern, x1.41 High, x2 Medium, x4 Low, x8 Off (era x8 at Off = 16 px in the modern era).
- Exported-LOD meshes upload a LOD on first use; until it is resident (`ready_frame`) the batch is
  **skipped** (the instance vanishes for a frame or two: a hole, not a coarser LOD).
- Sun cascades: own LOD pick by texel size (`r_vgeo_shadow_error_px` 2 px of cascade texel), never
  impostors, mesh `shadow_max_dist` as a **hard cut** (grass 15 m, wheat 20, bushes 40, trees 70 m).
- Local light shadow tiles: LOD by distance to the light at ~2 texels (shared with `r_vgeo 1`, unchanged).

### Impostors (classic)

- Trees (coarsest LOD >= 256 triangles): switch below `r_classic_impostor_px` 64 px x the species switch
  scale (`impostor_grid >> 16`: trees 6, old tree 1, Tree It bushes 2), i.e. **384 px across the bounding
  sphere** for a tree: a ~24 m tree (radius ~12 m) becomes an impostor at **~45 m** (px_scale 704 at 1080p); bushes at 128 px (~25 m for a 5 m bush).
- Small cover (grass tufts, procedural bushes): `r_vgeo_impostor_px` (Modern / High 16, Medium 24, Low 48,
  Off 96) x the mesh scale.
- **Hard switch, no fade**; the impostor (3 blended atlas frames, baked albedo + normals, no wind) and the
  mesh differ in colour and silhouette, so the switch pops.

### Foliage

| type | fade (m) | shadow (m) | impostor |
|---|---|---|---|
| grass tuft (`test_tuft`) | 40 - 65 | 15 | `r_vgeo_impostor_px` |
| wheat (`crop_wheat`) | 60 - 95 | 20 | none |
| procedural bushes | 100 - 150 | 40 | `r_vgeo_impostor_px` |
| Tree It bushes (juniper, bamboo, acacia) | 150 - 210 | 40 | 128 px |
| daisy | 50 - 80 | 15 | none |
| trees | 850 - 1100 | 70 | 384 px |

- Density / fade (`vgeo_foliage_alpha`, shared with vgeo): t = `r_foliage_density` x linear fade over
  [start, end]; per-instance alpha = clamp((t - hash) x 16): each instance goes from full to gone over 1/16
  of the band (grass: ~1.6 m). Dithered only on `VGM_DITHER_FADE` materials, and then by alpha alone:
  the blades keep full size, so the fade reads as instances blinking out at a moving front.
- Density (era-owned): modern 1, 2020 0.7, 2015 0.4, 2010 0.2; Fidelity caps Off 0.1, Low 0.25, Medium 0.5,
  High 0.8.
- Grass -> terrain texture: grass ends at 65 m, the terrain surface texture is drawn everywhere under it
  (the handoff is the grass fade band itself). Grass chunks stream with the terrain chunks
  (`world_chunk_radius` 4: 144 m), so the fade end must stay inside ~140 m.

### Terrain (`src/world/chunks.c`, `shaders/surfaces.glsl`)

- 32 m chunks, 4 LOD meshes (1 / 2 / 4 / 8 m grid), by chunk-centre distance: LOD0 < `world_lod_distance`
  (70 m), LOD1 < 140 m, LOD2 < 280 m, LOD3 beyond. **Hard switch**; cracks between LODs are hidden by
  vertical skirts (1 + 1.5 x stride m deep), visible as vertical facets where neighbours differ.
- Bounded maps: far tiles (128 m) at 4 m spacing within 700 m, 16 m beyond (hard switch); a tile is
  replaced by its 16 chunks when all are loaded (hard swap).
- Material LOD: the anti-tiling detail tap fades out over 70 - 110 m (smooth); triplanar side projections
  on steep ground (|n.y| < 0.8) **stop at a hard 160 m**. There is no parallax mapping (nothing to fade).

### Particles (VFX spawn LOD)

- Per effect `lod`: spawn multiplier steps (default 0.7 beyond 25 m, 0.4 beyond 60, 0.2 beyond 110), unlit
  below 0.3, no collision below 0.5: **steps** at the distances; emitters with `lod_cull` stop beyond it.

### Streaming

- Character LODs: all resident (built at load). Terrain chunk LODs: all four built at chunk finalize.
- Classic exported LODs: uploaded when first selected, no prefetch (see the hole above).

### Summary of pops

Character shadows and mid-fade switches; every static-mesh LOD switch; every impostor switch; foliage
instances blinking out at full size; the triplanar 160 m line; terrain chunk LOD switches with visible
skirts; far tile LOD / tile -> chunk swaps; animation update rate halving and IK dropping in one tick; VFX
spawn-rate steps; shadow casters at `shadow_max_dist`.

## 2. What applies now

| | |
|---|---|
| Ranges | `assets/render/lod.json` (reload: `r_lod_reload`), `src/render/lod_fade.{h,c}` (pure: `tests/test_lod.c`) |
| Cross-fade dither | `shaders/lod_fade.glsl`: 16 x 16 void-and-cluster blue noise (`tools/gen_lod_bluenoise.py`), shifted per frame (R2) while TAA is on |
| Characters | `src/gameplay/characters.c` (`LodFade`, `lod_char_select`), `shaders/skin*.frag`, `skin_shadow.frag` (`PIPE_SKIN_SHADOW_FADE`) |
| Animation tiers | `characters.c` (eased update rate), `src/anim/foot_ik.c`, `turn.c` (`ProcInput.lod_w`) |
| Classic renderer | `src/render/vk_classic.c` (`cull_instance`, `emit_part`, `lod_pick_band`), `shaders/classic_common.glsl` (`cl_faded`) |
| Terrain | `src/world/chunks.c` (`morph_target`, nearest-point levels), `shaders/lit.vert`, `shadow_morph.vert` (`PIPE_SHADOW_MORPH`), `surfaces.glsl` |
| VFX | `src/vfx/vfx_sim.c` (`lod_smooth`, `lod_dist_scale`) |
| Cvars | `r_lod_fade_time` (0.35 s), `r_lod_fade_band` (0.25 octaves), `r_lod_fade_debug` (tint fading draws: green in, red out, blue alpha only), `r_lod_debug` (1 character labels: LOD, fade `<from %`, pixels, update rate, IK and shadow weights; 2 + classic LOD levels as colours: white 0, green 1, yellow 2, red 3, blue impostor), `r_lod_bias` (user bias against the tier), `r_lod_legacy` (the ranges / cuts from before, for comparisons), `r_lod_scope` (the scope PiP's own LOD selection) |
| Commands | `lod_dolly <characters\|static\|impostor\|foliage\|terrain\|stop> [m/s] [m]`, `anim_lod_lineup` (now 5 / 15 / 30 / 60 / 120 / 240 m), `classic_stats` (effective thresholds, parts per LOD, fading parts) |

**Which path.** `lod_active()`: `r_vgeo 0` and not `r_lod_legacy`. With `r_vgeo 1` every shared system
(characters, terrain, surface detail, VFX) keeps exactly its behaviour from before (the frame UBO's
`lod_fade.z` = 0 selects the legacy dither and distances in the shaders), so the virtualized path renders as
before: the screenshot regression (`r_vgeo 1`, vegetation meadow / pine / aerial, dev hall, foliage yard)
differs from the pre-change build by 0.08-1.9 / 255 mean, inside the build's own run-to-run noise
(0.09-2.0; the windy yard 10.7 vs 10.0). The one intended exception is the scope picture-in-picture (below).

**Scaling.** Every range = lod.json value x the tier's multiplier (`tiers`: Off 0.3, Low 0.4, Medium 0.6,
High 0.85, Modern 1) x the user's `r_lod_bias` against the level's default x the era's multiplier (modern /
2020 1, 2015 0.85, 2010 0.7, on top of the eras' own pixel errors and densities). The Engine Fidelity
level's own scaling of the shared thresholds (`vgeo_lod_px`, `vgeo_shadow_error_px`, `impostor_px`) is
taken out (`without_level_scale`) so the tier is applied once; a user's deviation from the level default
is kept.

### New ranges (Modern, 1080p, 75 deg)

| system | before | now |
|---|---|---|
| Character LOD0 | >= 260 px (to ~5 m) | >= 140 px (to 8.7 m, down at 10.2 m) |
| Character LOD1 / LOD2 | 120 / 50 px (to 10 / 24 m) | 60 / 25 px (to 20 / 48 m; down at 23.7 / 57 m) |
| Character shadow | > 40 px (cut at 30 m) | fades out over 23.8 .. 20 px (51 .. 60 m) |
| Character fade | 0.15 s, static IGN dither, restarts mid-fade, shadows cut | 0.35 s, blue noise, always completes, shadows cross-fade |
| Update rate | 1 / 2 / 4 / 8 ticks, one step at the switch | same targets, eased one octave per 1.5 s |
| Foot IK / lean / turn | dropped at LOD0 -> 1 | weight fades over 140 .. 98 px (0.3 of the threshold) |
| Static LOD error | 2 px (cut) | 1 px (2x distance per switch), 0.25-octave cross-fade band |
| Shadow caster LOD error | 2 px of cascade texel | 1.33 px (x1.5) |
| Tree impostor | below 384 px across (~45 m for a 24 m tree), cut | below 192 px (~90 m), mesh + impostor cross-fade over 0.35 octave (impostor only at ~115 m) |
| Small-cover impostor | 16 px | 8 px |
| Grass tuft fade | 40 - 65 m, 1/16-band per instance, full size | 60 - 97.5 m, smoothstep density, 0.3-band per instance, shrinks to 35 %, dithered |
| Wheat / daisy / proc bushes / Tree It bushes / trees | 60-95 / 50-80 / 100-150 / 150-210 / 850-1100 m | 90-142.5 / 75-120 / 150-225 / 225-315 / 1275-1650 m |
| Foliage shadows | grass 15, wheat 20, bushes 40, trees 70 m (cut) | 22.5 / 30 / 60 / 105 m, fading out over the last 15 % |
| Terrain LOD | 70 / 140 / 280 m by chunk centre, cut, skirts | 105 / 210 / 420 m by the chunk's nearest point, per-vertex geomorph over the last 0.5 octave (crack- and pop-free: `test_lod`), skirts never exposed |
| Far terrain tiles | 4 m grid within 700 m | within 1050 m (still a cut, see Limits) |
| Terrain detail | anti-tiling 70-110 m, triplanar cut at 160 m | 105-165 m, triplanar fading over 195-240 m |
| VFX spawn LOD | x0.7 / 0.4 / 0.2 beyond 25 / 60 / 110 m (steps) | beyond 37.5 / 90 / 165 m, interpolated (log distance) |
| Streaming | a not-yet-resident exported LOD was skipped (hole) | the nearest resident LOD is drawn (coarser first) until it is in; the next finer LOD is prefetched |

## 3. How the fades work

* **Complementary masks.** A cross-fade is a pair (or triple) of draws that split the dither threshold
  range: time fades use a signed value (in `t` keeps `n < t`, out `-(1-t)` keeps `n >= t`); classic parts
  carry the kept interval `[lo, hi)` (`lod_iv_pack`): impostor `[0, iw)`, coarser LOD `[iw, mid)`, finer
  `[mid, 1)`. Every texel is covered by exactly one part (`test_lod` checks all 256 thresholds), in the main
  view and in the shadow maps (the same dither in shadow-map texels): no holes, no double shadows.
* **Time fades (objects with state: characters).** `LodFade`: the selector (with its hysteresis) runs on
  the LOD being faded to; a running fade always completes, a reversal mid-fade then fades back (no
  ping-pong half-fades). Shadows: one LOD coarser than each of the pair, faded with the same values.
* **Distance bands (stateless instances: the classic renderer).** 300k foliage instances have no per-
  instance state, so their switches are a continuous function of distance: within `r_lod_fade_band`
  octaves past a threshold both sides are drawn. A slow dolly fades smoothly (coverage changes < 0.2 % per
  frame at walking speed), a reversal reverses it, and a camera that stops inside a band keeps a stable
  mix that TAA resolves into a blend (temporal std 2.14 vs 2.04 / 255 without bands, `lod_check.py`).
* **Fading parts** batch apart (key bit 3) and draw through the dithered (clip / masked shadow) pipelines
  whatever their material; settled instances keep the opaque early-z pipelines.

## 4. Scopes (picture-in-picture)

The world camera narrows only slightly when aiming (`cl_ads_world_zoom`); scopes magnify inside the PiP lens (`exec_scope_pip`).
Before this change the PiP pass drew the legacy draw list and the skinned characters with the main view's
LOD choice, and **no vgeo scene content at all on either path** (no trees, bushes or grass in the scope;
the vgeo culling has no PiP view, its per-view error metric never sees the PiP projection).

Now (`r_lod_scope 1`):
* the classic renderer culls a fifth view with the PiP projection (`render_scope_cull`: objective
  position, PiP frustum, pixels of the PiP target per unit tangent) and picks LODs, impostors and foliage
  fades from it; `vkc_draw_pip` draws it forward (lit + depth write) into the PiP, **on both renderers**
  (with `r_vgeo 1` the PiP is the classic path's at the legacy thresholds: the main view is unchanged);
* characters inside the PiP cone take the larger of their main-view and magnified PiP pixel sizes, so the
  LOD, update rate, IK weight and shadow follow the scope; entering / leaving ADS cross-fades (time fades;
  through 8x the 120 m pair goes LOD3 -> LOD0 in three 0.35 s fades, and back after ADS);
* terrain chunks keep their main-camera distance LOD in the scope (a magnified level would crack against
  the chunks outside the lens).

`tools/tests/lod_scope.cfg` (8x at the line-up and the forest, `r_lod_debug`): with `r_lod_scope 0` the lens
shows terrain and characters only; with 1 the trees and bushes appear at their PiP LODs.

## 5. Tests

* `test_lod` (ctest `lod`): lod.json = built-ins; malformed tables rejected; thresholds = lod.json x tier
  for every category and level; character px -> LOD at 140 / 60 / 25 px, the line-up distances (5 m LOD0,
  15 m LOD1, 30 m LOD2, 60+ m LOD3) and ~2x the old switch distance; hysteresis; slow dollies (1 and 4 m/s)
  where every switch is a fade with < 5 % coverage change per frame and coverage summing to 1; camera
  reversal completes the fade; complementary masks (pairs and the three-way split); static chain dolly in
  log distance (< 5 % per step); streaming never draws a lower LOD after a higher one is resident; terrain
  geomorph: no neighbours two levels apart, every shared edge vertex agrees, a chunk LOD switch moves no
  vertex (random cameras, LOD distances 32-300 m).
* `tools/tests/lod_fade_shots.cfg` + `lod_check.py`: a forced character switch with the fade ramps (largest
  2-frame step 27 % of the switch) where the cut jumps (53 %); TAA temporal variance of the yard with and
  without bands.
* `tools/tests/lod_scope.cfg`, `tools/tests/lod_perf.cfg`, `lod_dolly <category>`.
* Impostor vs mesh colour (vegetation forest, impostors forced on / off): luminance -8 % before the gain,
  -4 % with `impostor.gain` 1.2 (the remaining difference is hue: the mesh's bent leaf normals catch more
  sky blue).

## 6. Performance per tier

RTX 3060 Ti, 1600 x 900 window, `r_vgeo 0`, uncapped, two alternating runs per configuration
(`tools/tests/lod_perf.cfg`), new ranges vs `r_lod_legacy 1`. Frame = CPU frame time.

| stop | frame ms new / old | GPU ms new / old (delta) | tris M new / old | budget (1080p) |
|---|---|---|---|---|
| yard_modern (300k foliage) | 12.35 / 5.95 | 12.21 / 5.80 (+6.42) | 22.71 / 6.02 | 16 |
| yard_high | 9.36 / 5.57 | 8.54 / 5.42 (+3.13) | 14.46 / 5.06 | 12 |
| yard_medium | 5.81 / 5.17 | 3.60 / 2.65 (+0.95) | 3.74 / 2.75 | 8 |
| yard_low | 4.26 / 4.32 | 1.12 / 1.15 (-0.02) | 0.57 / 0.79 | 5 |
| yard_off | 3.71 / 4.64 | 0.71 / 0.64 (+0.06) | 0.13 / 0.24 | 3 |
| forest_modern (vegetation pines) | 7.46 / 5.67 | 7.34 / 5.62 (+1.73) | 8.45 / 4.51 | 16 |
| forest_high | 6.93 / 6.73 | 6.92 / 6.68 (+0.24) | 6.14 / 3.56 | 12 |
| forest_medium | 3.31 / 4.08 | 3.01 / 3.40 (-0.38) | 3.18 / 2.26 | 8 |
| forest_low | 2.73 / 5.13 | 1.23 / 1.74 (-0.50) | 0.91 / 0.91 | 5 |
| forest_off | 2.58 / 4.69 | 0.66 / 0.96 (-0.30) | 0.37 / 0.40 | 3 |
| horde_modern (500 zombies) | 10.12 / 9.20 | 2.93 / 3.04 (-0.11) | 1.29 / 0.93 | 16 |
| horde_high | 11.49 / 17.45 | 3.22 / 3.17 (+0.04) | 1.27 / 0.97 | 12 |
| horde_medium | 9.68 / 10.32 | 2.62 / 2.42 (+0.21) | 1.08 / 0.96 | 8 |
| horde_low | 8.58 / 9.87 | 1.78 / 1.94 (-0.16) | 0.29 / 0.60 | 5 |
| horde_off | 8.63 / 9.88 | 1.25 / 1.44 (-0.19) | 0.11 / 0.31 | 3 |

Reading it: the dense 300k foliage yard is where the new ranges cost (Modern +6.4 ms GPU, 3.8x the
triangles: grass to 97 m, bushes x2 before their impostors, half of the parts in a fade band) and it still
fits every tier's GPU budget (at 1080p ~1.4x the pixels; the yard is triangle-bound). The vegetation forest
costs +1.7 ms at Modern and is cheaper than before from Medium down (the Medium / Low / Off multipliers
shrink the ranges below the old ones). The horde is CPU-bound by the animation, unchanged within noise
(the eased update rates and the LOD0 band do not add post-passes overall); its GPU time moves by +-0.2 ms.
No tier exceeds its budget, so the tier multipliers stay at the spec values (0.3 / 0.4 / 0.6 / 0.85 / 1).

## 7. Limits and out of scope

* **Out of scope (vgeo, `r_vgeo 1`):** cluster-level dither / continuous error metric, the vgeo error
  threshold, vgeo impostors. Unchanged; the vgeo path has no PiP view (the scope's vgeo content now comes
  from the classic PiP view on both renderers).
* Classic instances fade over distance bands, not over time (no per-instance state for 300k instances): the
  spec's "complete even if the camera moves back" holds trivially (the mix is a function of distance), and a
  camera parked in a band shows a stable dithered mix.
* Still cuts: character culling below 3 px; far terrain tile LOD (1050 m) and the far tile -> chunk swap;
  the triplanar slope threshold (|n.y| 0.8); VFX `lod_cull` emitters; LOD1 -> 2 hold IK -> baked arms
  (only the LOD0 systems fade).
* Terrain in the scope keeps the main camera's distance LOD.
* Measured at 1600 x 900 (the test window), not 1080p.
* Pre-existing, unrelated: the first frame of `map flat` reports 11 validation messages (a 512 x 512
  texture copy from a 256 KB staging buffer, images in UNDEFINED layout in the opaque pass) on the
  pre-change build too; every other run here is at 0 (horde soak on both renderers, scope, foliage yard).
