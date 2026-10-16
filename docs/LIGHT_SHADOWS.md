# Point and spot light shadows

Every point and spot light casts shadows unless it sets `LIGHT_NO_SHADOW` in `PointLight.flags` / `SpotLight.flags` (`src/render/render.h`). Lights that set it:
- the muzzle-flash light (`src/gameplay/effects.c`);
- every VFX effect light whose `"light"` block has `"shadow": false` (all `assets/vfx/effects/muzzle_*.json`).

Everything else casts shadows:
- level lamps (`ET_LIGHT`);
- flashlights and vehicle headlights;
- effect and particle lights such as fire and impacts.

| File | What |
|---|---|
| `src/render/vk_light_shadow.c` | atlas, per-frame selection, tile matrices, the atlas pass |
| `src/render/render.c` | `render_draw_shadow_casters`: static + GPU-skinned casters for one tile; `vkls_select` / `vkls_record` calls |
| `shaders/common.glsl` | `shadow_pass_vp` (flags.w ≥ 4 = atlas tile), `point_light_shadow`, `spot_light_shadow`, `local_shadow_tile` |
| `shaders/lit_body.glsl` | point / spot light terms × their shadow |

## How it works

- **Atlas:** one D32 image of 8 × 8 tiles (512 px tiles = 4096², or 256 px = 2048²), frame set binding 11, compare sampler.
- **Selection** (each frame, `vkls_select`):
  - Candidates are the point lights in the camera frustum that are allowed to cast.
  - They are ranked by `intensity × radius / (1 + 0.02 d²)`, where `d` is the distance from the camera to the light sphere.
  - Last frame's picks get ×1.5, so two equally ranked lights don't flicker.
  - The top `r_light_shadow_count` (≤ 10) get 6 tiles each, then each spot light gets 1 tile (≤ 4). That is 64 tiles at most.
  - Lights that don't make the cut stay unshadowed (`light_shadow[i].x = -1`).
- **Faces:**
  - Cube faces ±X, ±Y, ±Z use a 90° perspective widened by 6 texels, so the 2 × 2 PCF taps at a face edge stay inside the tile. Near is 0.05 m, far is the light radius.
  - Spot tiles use the outer cone ×1.04 + 0.02 rad, far = range.
  - Each tile matrix maps straight into its atlas tile, as the sun cascades do. Casters draw with a full-atlas viewport, scissored to the tile.
- **Casters** (`render_draw_shadow_casters`):
  - Opaque draws without `MAT_NO_SHADOW` that touch the light sphere and the face frustum. This includes the third-person shadow-only body, so the player's shadow appears under lamps.
  - GPU-skinned characters draw their own instance record of the sun shadow pass (`g_skin_shadow_inst`), so no extra skinning work is done.
  - The CPU-skinning fallback (`r_cpu_skinning`) does not cast local shadows.
  - Lamp fixtures are `MAT_NO_SHADOW`, so they never occlude their own light.
- **Sampling:**
  - The shader offsets the point along the normal by about 1.5 texels of the face at that distance, then picks the face from the dominant axis of the light-to-point direction.
  - Each sample is 4 hardware-PCF taps. Depth bias in the pass is 1.5 constant and 2.0 slope.
  - Point lights skip the shadow lookup on surfaces facing away from them.

## Cvars

| Cvar | Default | |
|---|---|---|
| `r_light_shadows` | 1 | point / spot light shadows on |
| `r_light_shadow_count` | 10 | shadowed point lights per frame (0–10) |
| `r_light_shadow_quality` | 2 | tile size: 1 = 256 px, 2 = 512 px |

## Cost

Measured in the devroom at night on an RTX 3060 Ti:
- At the street with 5 lamps in view, the whole frame takes about 2.4 ms with shadows on.
- At the flesh range, average frame time is the same with them on or off, within noise.

The atlas is re-rendered every frame. There is no static caching yet: that is the first optimisation if a scene with many lamps and many casters gets expensive.

## Not covered

- Particles and volumetric fog are lit without local shadows.
- VFX decals are lit without them too (`vfx_decal.frag`).
