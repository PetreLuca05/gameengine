# LOD ranges and transitions (classic renderer, `r_vgeo 0`)

Scope: the classic renderer (`src/render/vk_classic.c`, `shaders/classic_*`: discrete LOD chains,
impostors, foliage, sun cascades) and the non-vgeo systems it shares (characters / skinned meshes and
their animation tiers, terrain chunks and surface textures, foliage density fade, VFX spawn LOD). The
virtualized geometry path (`r_vgeo 1`: cluster DAG, error threshold, vgeo impostors) is out of scope and
renders as before.

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
