# Unity-style shadow controls

The knobs a Unity artist expects (Light > Shadows, Mesh Renderer > Lighting, Quality > Shadows)
drive this engine's shadows. Underneath they are the sky-driven sun (SHADOWS.md), the cascades
(`shadow_csm.c`) and the local light atlas (LIGHT_SHADOWS.md). There is no inspector: lights are
edited in level JSON or with console commands. That includes edits made by Claude Code on request,
e.g. "make the hall lamps hard".

Code:
- `src/render/shadow_settings.{h,c}`: the pure model (defaults, tier table, filters, bias, splits, names, atlas packer).
- `render.c`: quality cvars, sun uniforms, Cast / Receive.
- `vk_light_shadow.c`: local lights.
- `lighting.glsl`: the filters.
- `game_shadow_sky.c`: the `sun_shadow` / `light_shadow` commands and JSON.
- `scr_settings.c`: the Shadows group.

Tests:
- `tests/test_shadow_settings.c`
- `tests/test_shadow_csm.c`

## Mapping

| Unity knob | Here | Behaviour |
|---|---|---|
| Light Shadow Type None / Hard / Soft | `type none\|hard\|soft` | None: no map, no lookup. Hard: one bilinear compare fetch. Soft: tent / PCSS per Soft Shadow Quality. |
| Light Shadow Resolution (Use Quality Settings, Low .. Very High) | `resolution use_quality\|low\|medium\|high\|very_high` | Sun cascades 512 / 1024 / 2048 / 4096. Spot tile / point face 256 / 512 / 1024 / 2048 (URP tiers). |
| Strength | `strength 0..1` | `mix(1, shadow, strength)`: 0.5 halves the darkening. The sky still sets the sun's darkness at full strength. |
| Bias | `bias 0..2` | 1 = 0.75 texel constant depth bias (the validated default). In texels: same look at every resolution. |
| Normal Bias | `normal_bias 0..3` | 1 = 1.5 texels × sin(n, l). Both biases grow at a low sun (`csm_low_sun`, up to 3×). |
| Near Plane | `near_plane 0.01..10` (m) | Spot / point projection near plane. |
| Culling Mask | `culling_mask 0..255` | Bit per layer: renderers on a cleared layer cast no shadow for this light. Applies to the sun on vgeo, classic and skinned draws, and to local lights on draw items. |
| (URP additional light) shadow distance | `distance` (m, 0 = none) | A local light further than this from the camera is unshadowed. |
| Renderer Cast Shadows Off / On / Two Sided / Shadows Only | `"cast_shadows"` on brushes / props / vgeo placements | Off: `MAT_NO_SHADOW`. On: back faces culled unless the material is two-sided / alpha-masked. Two Sided: `PIPE_SHADOW_2S`. Shadows Only: shadow passes only. |
| Receive Shadows | `"receive_shadows": false` | `MAT_NO_RECEIVE` / `VGI_NO_RECEIVE`: lit as if unshadowed (sun, moon, local lights). |
| Layer | `"layer": 0..7` | Tested against the lights' culling masks. |
| Quality Shadows Disable / Hard Only / Hard and Soft | `r_shadows 0\|1\|2` | Hard Only renders Soft lights Hard (sun, viewmodel, local). Disable: no shadow passes. |
| Shadow Resolution | `r_shadow_resolution 0..3` | Used by lights on `use_quality`. |
| Shadow Projection Close / Stable Fit | `r_shadow_projection 0\|1` | Stable: sector spheres + texel snapping (no swimming). Close: each depth slice's light-space AABB, selection by view depth. Tighter, but it swims while turning (test_close_fit). |
| Shadow Distance | `r_shadow_max_distance` (m) | The last cascade ends here and fades over the border. |
| Shadowmask Mode / distance | `r_shadowmask_mode`, `r_shadowmask_distance` | Stub: stored, shown, no baked data yet. |
| Shadow Near Plane Offset | `r_shadow_near_offset` (m) | How far each cascade's light box reaches towards the sun beyond its receivers (towers outside the view). Unity's offset is a near-plane pull back; same purpose. |
| Shadow Cascades No / Two / Four | `r_shadow_cascades 1\|2\|4` | 3 is treated as 4. |
| Cascade splits | `r_shadow_split2`, `r_shadow_split4_1..3` (% of the distance) | Unity defaults 33.3 and 6.7 / 20 / 46.7. |
| Cascade Border | `r_shadow_cascade_border` (%) | Blend band at the end of each cascade, and the distance fade. |
| Soft Shadow Quality Low / Medium / High / Ultra | `r_soft_shadow_quality 0..3` | Low / Medium / High are URP tents of 3 / 5 / 7 texels, done as `ceil((n+1)/2)²` bilinear fetches with exact tent weights. Ultra is PCSS on the sun and viewmodel (contact hardening); on local lights it is tent 7. |

"Soft" on the sun keeps the sky model. The light size comes from haze / overcast and the shadow
fades under full overcast (SHADOWS.md). Soft Shadow Quality sets the filter, the sky sets how wide
it opens. "Hard" ignores the sky's softness, but the overcast fade still applies.

The viewmodel tile follows the sun: same type, filter and strength. Its resolution is
`csm_vm_texels(sun)` (1024 .. 2048).

## Tier defaults (Engine Fidelity bounds them; assets/render/fidelity/*.json)

| Tier | Shadows | Resolution | Cascades | Distance | Soft quality |
|---|---|---|---|---|---|
| Off | Disable | Low | 1 | 50 m (2010 era 70) | Low |
| Low | Hard Only (2015 / 2010 eras: Hard and Soft) | Low | 2 | 50 m | Low |
| Medium | Hard and Soft | Medium | 2 | 100 m | Low |
| High | Hard and Soft | High | 4 | 200 m | Medium |
| Modern | Hard and Soft | Very High | 4 | 300 m | Ultra |

All tiers use Stable Fit with the Unity splits and a 10 % border. `shs_quality_for_tier` mirrors the
JSON, and test_shadow_settings / test_shadow_csm fail if the two disagree.

Low and Medium use two cascades with a 33 % split. Their cascade 0 is coarse, about 12 tx/m, compared
with 113 tx/m on Modern. Contact shadows and the viewmodel tile cover the near field.
`test_shadow_csm` prints the layout of every tier.

## Local light atlas

Spots take one tile and point lights take 6 faces, each at the light's resolution (or the quality's).
The atlas is 8 × the `r_light_shadow_quality` tile (4096 at quality 2), capped at half the atlas per
tile.

Candidates:
- visible, type not None after the quality cap, within their `distance`
- ranked by screen size; the flashlight comes first
- `r_light_shadow_count` caps the point lights

Packing (`shs_atlas_pack`) is a buddy quadtree in priority order:
- A light's faces are all or nothing.
- A group that does not fit is retried one size smaller, down to 64 px.
- If it still does not fit, the light is unshadowed this frame. That is the budget.

## Editing lights by hand (no inspector)

Point lights in a level:

```json
{"type": "light", "pos": [-22, 3.8, 0], "radius": 10, "intensity": 90,
 "shadow": {"type": "hard", "resolution": "high", "strength": 0.8, "bias": 1, "normal_bias": 1,
            "near_plane": 0.2, "culling_mask": 255, "distance": 0}}
```

- Every key is optional. A light without `"shadow"` uses the engine default: Soft, use_quality,
  strength 1.
- The dev room's two hall lights at x = −22 are a demo pair: blue is Hard, orange is Soft.

The sun:
- `assets/render/shadows_sky.json` `"sun_shadow": {...}` is the default.
- A level's top-level `"sun_shadow": {...}` overrides it.
- Same keys as above except `near_plane` / `distance`.

Renderers (brushes, props, vgeo placements):

```json
{"type": "box", ..., "cast_shadows": "two_sided", "receive_shadows": false, "layer": 3}
```

Legacy `"shadow": false` on a brush still means Cast Shadows Off.

Console (host; persists until the level reloads, so put lasting changes in the JSON):

```
light_shadow list                         every level light with its settings
light_shadow all type hard                every light Hard
light_shadow nearest type soft strength 0.5
light_shadow 37 resolution very_high bias 0.5
sun_shadow type hard                      the sun (and viewmodel) Hard
sun_shadow strength 0.6 culling_mask 0xFE
r_shadows 1                               Hard Only, everything renders hard
r_soft_shadow_quality 2                   7x7 tent for Soft lights
```

Type and Strength replicate to clients (NetEnt `extra.z`, no protocol change). The other per-light
fields are the host's render settings only.

## Debug

- `r_shadow_debug 1`: readout. It covers the sun's settings and the quality (mode, resolution,
  projection, cascades + splits + border, soft quality, filter). It also covers the local lights
  shadowed / tiles / atlas, and draws the split bar.
- `r_shadow_debug 4`: cascade colours, the same colours as the Settings split bar.
- `r_shadow_debug 6`: texel size.
- `shadow_info`: the same as text.

## Differences from Unity

- Bias / normal bias are in texels × a per-engine constant, with defaults 1 / 1 (Unity: 0.05 / 0.4 in
  world units scaled by the texel). The scale is chosen so 1 / 1 matches the validated look of
  SHADOWS.md.
- Skinned casters are always drawn two-sided into the sun's maps (thin cloth / hair).
- vgeo placements carry Cast / Receive / layer per instance. The classic renderer (`r_vgeo 0`)
  honours Cast Shadows and layers for them but has no per-instance Receive Shadows: a placement's
  `"receive_shadows": false` applies under vgeo only. Draw items (brushes, props) honour it on both.
- Shadowmask modes are a stub until baked lighting exists.
- Ultra on local lights is tent 7: their perspective maps give no blocker distance in metres.
