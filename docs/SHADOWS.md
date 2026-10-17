# Sky-driven shadows

Sun and moon shadows follow the sky: crisp and dark under a clear sun, soft and faint in haze,
gone under overcast, bluish from the skylight, with a penumbra that widens with the distance from
the caster. Nobody sets a shadow strength or softness. Both come from the sky and weather state
every frame. One filter and one bias formula serve every preset and every sky: there is no
discrete "shadow type" to switch between (see [The continuous model](#the-continuous-model)).

| File | Role |
|---|---|
| `src/render/shadow_sky.c/.h` | Pure driver: config, curves, contrast, cutoff with hysteresis, source radius, wide mode, the filter's PCSS weight / max width (tier crossfade), moon handover, cloud drift, exposure, ground albedo |
| `src/render/shadow_csm.c/.h` | Pure cascade layout: practical splits, sector bounding spheres, texel snapping, the light-space reach, bias and kernel from the texel size, the viewmodel tile, the preset-table check |
| `assets/render/shadows_sky.json` | Every tuning value; hot reload (saved changes apply within a second, `shadows_sky_reload`) |
| `src/game/game_shadow_sky.c` | Steps the driver from `daynight_environment`; fidelity inputs, `shadow_preset` (and the test corner's switches), `shadow_info`, `r_shadow_debug` readout / fit drawing, `shadow_switch_stress`, `shadow_orbit` |
| `src/game/game_daynight.c` | Fills the driver input (sun / moon through the atmosphere, sky SH, clouds, fog, rain, snow, wind) |
| `src/render/render.c` `compute_matrices` | Cascade fit (shadow_csm), tiles drawn this frame, the moon and viewmodel tiles, every shadow uniform; the shadow passes (per-tile caster culling, masked casters, the viewmodel map) and the unrendered-tile check |
| `shaders/lighting.glsl` `sh_filter` | The one filter (PCF / PCSS by weight), receiver-plane depth, texel-scaled bias, cascade selection + blend + distance fade, wide mode, the moon and viewmodel tiles, debug views |
| `shaders/shadow_mask.vert/.frag` | Alpha-masked casters in the raster shadow passes (`PIPE_SHADOW_MASK`) |
| `shaders/sky/cloud_shadow.comp`, `env_render.comp` | Beer-Lambert cloud shadows, the sky probe's ground (snow / wet albedo, cosine quadrature) |
| `shaders/post/contact.comp` | Contact shadows: ray length limited by the penumbra |
| `data/levels/devroom.json` | The shadow reference corner and test corner |
| `tests/test_shadow_sky.c`, `tests/test_shadow_csm.c` | Unit tests (the filter on the CPU, tier crossfades, cloud sweeps, the cascade layout) |
| `tools/tests/shadow_ref.py`, `tools/tests/shadow_stability.py` | In-game tests |

## Root causes (October 2026 fix)

The reported symptoms (jagged edges, shimmer / swimming, shadows cut off at some distances and
angles, acne / peter-panning / seams / flicker, worst when the shadow type changes, poor
first-person shadows) traced to these causes in the code before the fix. They were found by
reading the code and checking the numbers against the formulas, then confirmed with before / after
captures of the same scripted scenes (dev room corner, noon / sunset, looking down, far views,
cascade view, viewmodel) rendered headless on Linux (Xvfb + Mesa lavapipe), both builds under the
Vulkan validation layer.

| # | Symptom | Cause (before) | Fix |
|---|---|---|---|
| 1 | Peter-panning everywhere, worst at sunset; small casters lose their shadow | The constant depth bias was `0.0012 × k × 2 × depth_range` metres: **0.29 m** in cascade 0, ~1 m in cascade 1, ~10 m in cascade 2 (`k` 1 / 2.5 / 7, `depth_range` ≥ 120 m). At a 5° sun 0.29 m of depth bias moves the shadow ~3.3 m along the ground | Bias in metres from each cascade's texel size (0.75 texel constant, 1.5 texel × sin(n, l) normal offset, raised ×1 .. 3 under 15° of sun elevation), plus one texel of receiver-plane slope (`csm_bias`) |
| 2 | Acne on some presets, peter-panning on others; the bias jumps when the preset changes | The normal offset was 3 cm × `k` with `k` hard-coded 1 / 2.5 / 7 "texel size relative to cascade 0"; the real ratios depended on the level extent, `r_shadow_distance` and the cascade count (1 cascade = the middle radius with `k` = 1: under-biased); resolution changes changed the texel without touching the bias | Bias derived from the texel size of the tile actually sampled (`shadow_nbias`, `shadow_bias`), the same formula on every preset |
| 3 | Shadows cut off at a random distance, depending on where the player stands | The far cascade's centre was pulled halfway to the level's `shadow_center`: away from it, cascade 2 left the camera behind | Every cascade is centred on its view sector (`csm_layout`), nothing else moves it |
| 4 | Shadows cut off at some view angles; the cut line turns with the camera | Square tiles centred 0.35 r ahead (yaw only) with distance-based selection: points behind / beside the camera within the split fell outside the last tile (returned lit); with 3 cascades at full reach there was no distance fade at all, so the shadow ended at the tile's square edge | Each tile is the bounding sphere of the camera frustum cut at its split (3D view direction), padded by 6 texels; the shadow always ends at the shadow distance and fades over its last 10 % |
| 5 | Shadow type switch (preset change): jagged ↔ soft jumps | `r_shadow_filter` selected discrete filter models (3 PCF steps in *texels* vs PCSS in metres); the step model changed width per cascade (texels) and stepped at the radius thresholds | One filter: half width = d × tan r in metres, d measured (PCSS weight 1) or assumed (`pcf_ref_m`, weight 0), clamped to [`min_texels`, max] texels; a tier change glides the weight and the max over `mode_s` (0.12 s) |
| 6 | Flicker of soft edges on Off / Low / Medium | The kernel rotation changed every frame regardless of the AA; only TAA averages that pattern | The rotation advances per frame only while TAA / TAA-U jitter is on (`shadow_cfg.x`); otherwise a fixed per-pixel pattern |
| 7 | Jagged edges at Off .. Medium; low texel density everywhere | `r_shadow_quality` sized the whole 2 × 2 atlas: Modern / High had 1024² per cascade (2.3 cm texels at 12 m), Low 512² (4.7 cm); PCF radius 0 = a single hardware tap | `r_shadow_quality` is the tile size per cascade (1024 / 2048 / 4096); High 1.0 cm, Modern 1.2 cm (0.6 cm at quality 3), Low 2.4 cm; every tier filters at least 1 texel |
| 8 | Light leaking through foliage / fence cards (solid card shadows) | The raster shadow pass had no fragment stage: alpha-masked glTF casters cast their full quad | `PIPE_SHADOW_MASK` (`shadow_mask.frag`) for `MAT_ALPHA_MASK` casters |
| 9 | Cost / caster culling | Raster casters were drawn into every tile without culling | Per-tile culling against the tile's light box (which reaches `CSM_REACH` towards the light, so off-screen casters stay) |
| 10 | Decal / particle shadows sampling the wrong tile | The last cascade was range-checked against the whole atlas [0, 1], so samples bled into neighbouring tiles | Range check against the last cascade's own tile |
| 11 | First-person hands: detached / offset / wrong-shaped shadows; hands shadowed by the player's own (third-person) body | The viewmodel was not a shadow caster at all; its only shadow came from the owner's `MAT_SHADOW_ONLY` third-person body copy, whose pose differs from the first-person arms, sampled at the viewmodel's FOV-space position | The viewmodel tile (below): the rig's own map at ~1 mm texels; world cascades sampled at the rig's apparent world position and skipping casters within 0.6 m along the light (the body copy) |
| 12 | Dawn / dusk: the fit looking up from under the horizon | The light view was built from the raw sun direction, which dips below 0° while the disc still owns the cascades | The fit uses at least 1° of elevation |
| 13 | Looking down: the feet's shadows do not touch the feet | Cause 1: the 0.29 m constant bias moved every contact shadow ~13 cm away at noon (more at a low sun). The first-person body and the shadow copy are posed identically (`fp_shadow_check`: 0 mm) | Cause 1's fix; verified on before / after captures looking down |

Captures (`docs/shadow_screenshots/`): `feet_before_after.png` (looking down at noon: the leg
shadows now reach the boots, smoother edges), `cascades_before_after.png` (`r_shadow_debug 4`: the
sector-sphere cascades, the distance fade, the viewmodel tile in magenta),
`sunset_corner_before_after.png`, `viewmodel_bias.png` (the sleeve at the bare texel bias, with the
skin / cloth floor, with the tile off).

What was checked and found sound:
- **Matrix / map timing.** `apply_setting_changes` (resolution) runs at the frame boundary before
  `compute_matrices`; matrices, the vgeo / classic shadow culls and the uniforms are all built in the
  frame that renders and samples them; the atlas is cleared every frame, so a skipped tile reads
  lit, never stale. The shadow pass renders before every pass that samples it. A runtime check now
  enforces it (`shadow_check_sampled`, below).
- **Jitter.** The shadow matrices never contain the TAA jitter or a previous frame's matrix.
- **Cutoff.** The overcast cutoff already had hysteresis (`reenable`) and a 1 s fade; the passes
  keep running until the fade ends. The cloud sweeps in the tests toggle it at most once per sweep.
- **Skinned casters** use this frame's palette; character shadows use one LOD coarser than the main
  draw (never more).

## The physical model

**Contrast.** A shadow's darkness is the ratio of direct irradiance to sky irradiance. On a
surface lit at incidence θ:

    lit / shade = (E_sky + E_dir cos θ) / E_sky = 1 + C cos θ,   C = E_dir / E_sky

The lit shader already computes exactly this: `albedo / π × (E_sky(n) × AO + E_dir × n·l × shadow)`.
It does so with `ambient_scale` 1 and no unshadowed fill light. The driver only decides when a cast
shadow is worth drawing.

- **Clear noon:** C is large, so shadows are dark.
- **Haze / thin cloud:** E_dir drops and the sky brightens, so shadows are faint.
- **Overcast:** E_dir is about 0. Only AO and the sky remain.

**Softness.** The penumbra comes from the source's angular radius r:

    penumbra half width = d × tan(r)    (d: blocker to receiver distance; full width 2 d tan r)

- **Clear sun:** the disc, 0.265°.
- **Haze, thin cloud, fog, rain:** they widen the effective source (the bright aureole) to several
  degrees.
- **Overcast:** the source is the whole sky, so there is no cast shadow at all.

The driver combines the terms in quadrature: `r = sqrt(disc² + haze² + veil² + fog² + rain²)`.

**Colour.** The shade is lit by the sky only: `E_sky(n)` from the sky SH.

- **Clear sky:** blue shade.
- **Overcast:** neutral grey.
- **Sunset:** warm direct light against a cool shade.
- **Moon:** dim and blue.

No tint term exists anywhere. This is what correct diffuse lighting gives.

**Clouds.**
- A uniform sheet (the veil) attenuates the direct light by `exp(-tau / mu)`, with tau from
  `veil_tau(cover) × (1 - k + k × density)` and mu = sin(elevation), clamped at `min_mu`.
- The cloud shadow map adds the patches: `exp(-density × cumulus_tau / mu)`. They drift with the
  weather wind at the layer (`wind_aloft`).
- `cloud_light_loss` (daynight.json) dims only the sky and the clouds' own light. It no longer
  touches the direct light, and the old 8 % floor is gone.

**Air between caster and receiver.** `fog_apply` already applies `L × T + S_ap` to both lit and
shaded pixels, so far-field shadows lift on their own (`(L_lit T + S) / (L_shade T + S) → 1`). A
separate shadow-to-fog blend would count the effect twice.

**Wet and snow.**
- Wetness lowers roughness and raises reflectance (`weather_surface`). Lit wet areas gain specular,
  so the shadows read darker.
- Snow raises the albedo of the surface and of the sky probe's ground (`ground.snow_albedo`), so
  the bounce brightens the shadows. This works on every tier, not only with SSGI.

## Per-condition targets

These are real-world values for an 18 % grey horizontal surface. They are checked in the game by
`tools/tests/shadow_ref.py`.

| Preset | r | Penumbra half width at 1 / 5 / 20 m | Lit / shade (ground) | Shade colour | Cast shadows |
|---|---|---|---|---|---|
| clear_noon | 0.265° | 0.46 / 2.3 / 9.3 cm | 6 – 9 | very blue (b/r ≥ 1.4) | full PCSS |
| hazy | ~1° | 1.7 / 8.7 / 35 cm | 2.5 – 4 | blue | PCSS |
| light_cloud | ~3° | 5 / 26 / 105 cm | 1.6 – 2.5 | slightly cool | wide mode |
| overcast / rain | – | – | 1.00 – 1.05 (AO only) | neutral | off |
| sunset (5°) | ~0.3° | ≈ clear | ground 1.5 – 2.5, sun-facing wall 5 – 10 | warm vs cool | PCSS, long |
| fog | 2 – 4° | wide | 1.3 – 1.8 near, → 1 far | grey | wide → off |
| night_moon | 0.26° | ≈ clear | 4 – 8, about 1/150 of noon | blue | PCSS, dim |

Shadow-map resolution sets a floor. A cascade 0 texel is 1.0 cm on High, 1.2 cm on Modern and
0.6 cm at shadow quality 3 ([Per-preset tables](#per-preset-tables)), so the clear sun's 1 m
penumbra (4.6 mm) is below one texel; the 1-texel minimum filter keeps that edge anti-aliased and
contact shadows carry the range. The tests check widths ≥ 3 texels within 15 % and smaller ones to
stay under 2 texels.

## The continuous model

A "shadow type" (hard / soft, PCF / PCSS, wide, off) is never a switch. Every frame the same
pipeline runs with continuous parameters:

| Parameter | Meaning | Comes from | How it changes |
|---|---|---|---|
| source radius r | angular radius of the light (sun 0.265°, haze / cloud / fog / rain widen it) | the driver (sky + weather curves) | smoothed, `smooth_s` 1 s |
| d | blocker distance: `mix(pcf_ref_m, measured, pcss_weight)` | PCSS search (weight 1) or the assumed 2 m (weight 0) | the weight glides over `mode_s` 0.12 s on a tier change |
| half width | `clamp(d × tan r / texel, min_texels, max)` texels: hard → soft is this radius | the formula, per cascade in metres | follows r and d; the max glides with the tier |
| fade | the cast shadow's weight; 0 = no shadow (the end of a fade, not a mode) | contrast C vs the cutoff, with hysteresis | smoothed 1 s; the passes stop only once it reaches 0 |
| cascade 0 weight | wide mode: cascade 0 fades into cascade 1 when r passes `wide_deg` | driver, with hysteresis | smoothed 1 s |
| bias | normal offset / constant / slope, from the texel size | `csm_bias` (texel, sun elevation) | follows the texel size; the low-sun factor is smooth |
| distance | where the last cascade ends | `r_shadow_distance` × the level reach | glides over ~0.2 s |

The tier (`r_shadow_filter`) only sets the tap budget: 12 taps (PCF), 16 taps (PCF), 16 + 16 (PCSS,
the blocker search runs while its weight is above 0). The 2010 era fixes the half width
(`era_pcf` cascade-0 texels, kept in metres on every cascade) and keeps everything else physical.

What still changes in one frame, by design: the shadow resolution (`r_shadow_quality`, reallocated
on the frame boundary and rendered before anything samples it) and the cascade count. The bias
and the kernel follow the new texel size in that same frame, so the change is one of detail only
(no bias or softness jump), and `shadow_switch_stress` allows one texel of slack on those frames.

## Cascades

`compute_matrices` (render.c) with `shadow_csm.c`:

- **Splits.** Practical scheme between 1 m and the shadow distance D:
  `s_k = 0.8 × (D / 1)^(k / N) + 0.2 × (1 + (D - 1) k / N)`, the last split exactly D. Fixed per preset
  (D = `r_shadow_distance` × max(3.5 × level extent, 110 m): 140 m in the dev room at 1.0).
- **Fit.** Cascade k is the bounding sphere of the view sector of radius s_k (the frustum cut at
  that distance; corner half angle θ: radius s sin θ ahead s cos θ for θ ≥ 45°, else s / (2 cos θ)),
  computed at a vertical FOV of at least 90° (`CSM_FIT_FOV_MIN`), so ADS zoom never resizes a
  cascade, and padded by 6 texels (the shader's 4-texel border + the snap). Camera rotation never
  changes a tile's size; the tile follows the 3D view direction.
- **Snap.** The sphere centre is snapped to whole texels in light space; camera motion moves the
  grid by whole texels (the unit test: < 0.02 texel of drift over 10 frames).
- **Reach.** The light box reaches `CSM_REACH` (3 km) towards the light and r + 2 m behind the centre,
  so towers outside the view at a low sun cast (D32 depth: ~0.2 mm precision at that span).
- **Selection.** By the distance from the camera; the last 12 % of each cascade blends into the
  next one; the last fades out over the last 10 % of D (`shadow_cfg2.z`); in wide mode cascade 1's
  sphere covers the near field.
- **Low sun.** The fit uses at least 1° of elevation. The low-sun factor (×1 at 15°, ×3 at 3°)
  raises the bias and the receiver-plane slope limit (80° → 85°).
- **Casters.** Raster casters are culled per tile against its light box; vgeo / classic cull with
  the same matrices and pick the LOD by the tile's texel size (`R->shadow_radius`). Masked casters
  alpha-test (`PIPE_SHADOW_MASK`). Characters cast with this frame's palette, one LOD coarser.

### Bias

Per cascade, in metres, from its texel size t (`csm_bias`), × the low-sun factor:

| Term | Value | Shader |
|---|---|---|
| normal offset | 1.5 t × sin(n, l) | `shadow_nbias[c]` |
| constant | 0.75 t (NDC: / span) | `shadow_bias[c]` |
| slope | one texel × the receiver-plane gradient (clamped 80° .. 85°) | `sh_receiver_gradient` |
| hardware | constant 1.25, slope 1.75 (sun tiles); 1.0 / 1.5 (viewmodel) | `vkCmdSetDepthBias` |

At noon on High (t = 1.0 cm) that is 7.5 mm constant and up to 15 mm normal offset, against 290 mm
before. Every tap of the filter compares against the receiver's plane at the tap, so wide kernels
on sloped receivers do not shadow themselves.

### Per-preset tables

One formula for all: tiles of `csm_tile_texels(r_shadow_quality)`, the splits above, the sphere
radii (16:9). `tests/test_shadow_csm.c` checks every preset's default and max values and its era
overrides: the splits end at D, cascade 0 has at least the preset's texels per metre (Off 6, Low 30,
Medium 40, High / Modern 80; eras 80 % of that), neighbouring cascades within 6× of each other, the
bias from the texel size. Default values (dev room):

| Preset | Tile | Cascades | D | Cascade 0 (split, texel) | Cascade 1 | Cascade 2 | Filter (taps, max half width) | Viewmodel tile |
|---|---|---|---|---|---|---|---|---|
| Off | 1024 | 1 | 70 m | 70 m, 12.3 cm | – | – | PCF 12, 4 texels | 1024 |
| Low | 1024 | 2 | 70 m | 13.8 m, 2.4 cm | 70 m, 12.3 cm | – | PCF 12, 4 texels | 1024 |
| Medium | 1024 | 3 | 105 m | 10.9 m, 1.9 cm | 31.9 m, 5.6 cm | 105 m, 18.5 cm | PCF 16, 8 texels | 1024 (PCSS) |
| High | 2048 | 3 | 105 m | 10.9 m, 0.96 cm | 31.9 m, 2.8 cm | 105 m, 9.3 cm | PCSS 16 + 16, 32 texels | 2048 (PCSS) |
| Modern | 2048 (max 4096) | 3 | 140 m | 13.6 m, 1.2 cm (0.6 cm) | 40.3 m, 3.6 cm | 140 m, 12.3 cm | PCSS 16 + 16, 32 texels | 2048 (PCSS) |

Before the fix, High and Modern had 1024 texels per cascade (cascade 0: 12 m, 2.3 cm texels), Low
512 (4.7 cm). The atlas is 2 × 2 tiles: 4096² D32 (64 MB) at quality 2, 8192² (256 MB) at quality 3,
clamped to the device's `maxImageDimension2D`.

## The viewmodel tile

The first-person rig (arms, held item, attachments: the `MAT_VIEWMODEL` draws) has its own D32 map
(`R->vm_shadow`, bindings 16 / 17), the best shadows in the game:

- **Size.** 2048² (shadow quality 2 and up), 1024² at quality 1; `r_vm_shadow 0` turns it off.
- **Fit.** Every frame around the rig's drawn bounds, a fixed 0.9 m half extent (0.88 mm texels at
  2048), snapped in light space, along the same light view as the cascades, no jitter. It is
  cleared every frame and drawn with this frame's matrices and palettes (GPU and CPU skinning).
- **Space.** The rig is drawn at its world transforms with its own FOV (`r_viewmodel_fov`); casters
  and receivers in the tile use those same transforms, so self-shadowing lines up. The world
  cascades are sampled where the rig appears in the world (`sh_vm_apparent`: view x / y scaled by
  `tan(fov / 2) / tan(vm_fov / 2)`).
- **The body copy.** The owner's third-person body (`MAT_SHADOW_ONLY`) casts the player's shadow on
  the world, but its arms are not where the first-person arms are. Viewmodel receivers sample the
  world cascades 0.6 m towards the light (`CSM_VM_SKIP`), past that copy; world casters further
  away (walls, trees, the tower) still shadow the hands.
- **Filter.** PCSS from Medium up with the driver's light size (crossfading on a tier change), PCF at
  Off / Low; bias `csm_bias_vm`: the texel formula with floors at skin / cloth scale (2 mm constant,
  4 mm normal offset; the meshes interpenetrate by millimetres at cuffs and grips, which showed as
  black spots on the sleeve at the bare texel bias).
- **Shading.** `shadow_factor_vm` = world cascades × the viewmodel tile, × the sky fade.

Not done (known gaps): world receivers near the player (ground / wall within ~2 m) do not sample the
viewmodel tile: they already get the arms / item from the body copy in the world cascades, and
sampling both would draw them twice. Player-attached lights (flashlight, muzzle light) do not yet
shadow the rig: the local light shadow tiles (docs/LIGHT_SHADOWS.md) contain world casters only.

## Pipeline

Each frame:

1. `daynight_environment` builds the driver input:
   - sun and moon light through the atmosphere (`transmittance_cpu`), without clouds
   - `sky_up` = SH irradiance(+Y) × `ambient_scale`
   - cloud cover and density, fog, aerosols (`mie_scale`)
   - rain, snow and wetness (`World.weather`)
   - wind
2. `shadow_sky_frame` adds the fidelity inputs and calls `ssky_update`:
   - **Ownership:** the sun owns the cascades while its disc is up (`sun_light > 0`, horizon
     smoothstep), then the moon. While the sun owns them, the moon is the second light in atlas
     tile 3 (`second_shadowed`, tiers with moon shadows).
   - **State:** one state per body (fade, radius). The handover continues the moon's history, so it
     never pops: when the sun's light reaches 0, nothing of the sun is left to change.
   - **Per light:** contrast C, cutoff state with hysteresis, smoothed fade, smoothed source radius
     (time constant `smooth_s`).
   - **Wide mode** (PCSS tier): above `wide_deg`, the cascade 0 weight fades to 0 and the pass stops.
   - **Filter:** the PCSS weight and the max half width approach the tier's targets over `mode_s`.
   - **Also:** cloud drift (integrated), exposure compensation, ground albedo.
3. `compute_matrices` (render.c):
   - the cascade layout and fit (above), the moon tile (cascade 1's sphere along the moon), the
     viewmodel tile
   - `shadow_active[0..3]`: no tiles once the cutoff fade has ended, no cascade 0 in wide mode, tile
     3 for the moon
   - the uniforms (see the table after this list)
4. The shadow passes: the atlas tiles (raster casters culled per tile, masked casters alpha-tested,
   vgeo / classic, GPU-skinned), then the viewmodel tile. `shadow_check_sampled` then compares the
   tiles the lit shaders will sample this frame (from the uniforms, by lighting.glsl's rules) with
   the tiles drawn; a miss is logged once and counted (`shadow_info`: "unrendered samples"; the
   tests require 0).
5. `lit_body.glsl` computes `shadow_factor` (or `shadow_factor_vm` on the viewmodel) × `contact` ×
   `cloud_shadow_at` for the key light, and `moon_shadow` for the second light. Both are
   `mix(1, s, fade)`; the contact shadows take the same fade.

| Uniform | Contents |
|---|---|
| `shadow_vps[3]`, `shadow_vp_moon` | world → atlas tile |
| `shadow_splits` | selection distances, w: cascade count |
| `shadow_bias` | constant bias per cascade (NDC), w: the moon tile |
| `shadow_nbias` | normal offset (m) per cascade, w: the moon tile |
| `shadow_mpt`, `shadow_span` | metres per texel, metres of depth per NDC unit (w: the moon tile) |
| `ssky0` | tan r, fade, cascade 0 weight, tap budget (0 / 1 / 2) |
| `ssky1` | –, max half width (texels), search metres, moon fade |
| `ssky2` | tan r of the moon, debug view, contact scale, moon radius |
| `shadow_cfg` | kernel rotation per frame (TAA on), PCSS weight, PCF blocker distance (m), min half width (texels) |
| `shadow_cfg2` | fixed half width (m, era 2010), low-sun factor, shadow distance (m), viewmodel FOV scale |
| `shadow_vp_vm`, `shadow_vm`, `shadow_vm2` | the viewmodel tile: matrix; texel m, span, on, normal offset; 1 / size, constant bias, world skip (m), PCSS weight |

### The filter (`sh_filter`)

- **Blocker search** (PCSS weight > 0): 16 Vogel taps over `search_m × tan r / texel` texels,
  clamped to [1, 1.5 × max], raw depth through binding 15 (17 for the viewmodel tile). No blocker
  inside the search (always wider than the kernel): lit.
- **Filter:** half width `clamp(mix(pcf_ref_m, d_block, w) × tan r / texel, min_texels, max)` texels
  (`era` fixed: `fixed_texels` × the cascade 0 texel, in metres); 12 (PCF 12) or 16 hardware-compare
  taps.
- **Rotation:** interleaved gradient noise per pixel; a new pattern per frame only under TAA / TAA-U
  (`shadow_cfg.x`), so the noise is averaged; without TAA the pattern is fixed (no flicker).
- **Receiver-plane depth.** Every tap compares against the receiver's plane at the tap. The depth
  gradient comes from the normal under the cascade matrix, clamped at 80° (85° at a low sun), plus
  one texel of slope bias. Without this a wide kernel on ground sloped to the light shadows itself.
- **Units.** Everything is in metres and converted per cascade, so the penumbra is the same across
  cascade blends (until the min / max clamps).
- **Known limit (all PCSS):** the map stores the light-facing surface. A thick caster's penumbra is
  estimated from its front face, which is farther than its silhouette edge, so it comes out
  slightly too wide. The test caster is a 4 cm thin fin.

### Contact shadows

They stay sharp at contact. The ray only runs as far as the penumbra stays under `contact_px`
pixels: the length is `contact_px × pixel_m / (2 tan r)`, capped at the clear-sun 0.35 m. A hit is
`1 - 0.5 t²`: full occlusion at contact. They fade with the cast shadows. The viewmodel is excluded
from the screen-space passes; its sub-millimetre tile covers its contact detail.

### Tiers and eras

`r_shadow_filter` is the tap budget of the one filter:

| Level | Taps | Max half width | Blocker distance |
|---|---|---|---|
| Off / Low (0) | 12 | 4 texels | assumed (`pcf_ref_m` 2 m) |
| Medium (1) | 16 | 8 texels | assumed |
| High / Modern (2) | 16 + 16 | 32 texels | measured (PCSS) + wide mode |

- `r_moon_shadows` (High / Modern) gives the moon tile 3 (PCF, a twilight fill).
- Cloud shadows: Medium and up.
- The 2010 era (`r_era 3`) keeps a fixed half width (`era_pcf`). Its contrast and cutoff are still
  physical, so 2010 shadows lighten and vanish with the weather.

### Exposure

`exposure.cover` / `exposure.snow` add EV compensation after `TOD_EXPOSURE_COMP`. Overcast scenes
metered to mid grey read flat but readable, and snow is not pulled down to grey. The values are
small; the metering does most of the work.

## Energy corrections in the lighting

| Before | Now |
|---|---|
| `ambient_scale` 1.6 (sky light and reflections) | 1.0: a documented global multiplier, physical at 1 |
| Unshadowed fill light (moon by day, sun at dusk) | Removed: the moon is shadowed in tile 3 |
| Direct light × `cloud_light_loss`, floor 8 % | Direct × `exp(-tau / mu)` (no floor); the loss dims the sky only |
| Cloud shadow `1 - d × 0.75` | Beer-Lambert `exp(-d × cumulus_tau / mu)` |
| `pcf++` above 65 % cloud cover | Source radius from the sky |
| Contact shadow floor 0.4 | Full occlusion at contact |
| Sky probe ground: 3 samples × π/3 × 1.6, albedo 0.25 fixed | 12-sample cosine quadrature (weights sum to π); albedo from snow / wetness |
| Sun / moon swap at equal brightness (a shadow direction jump) | Sun owns the cascades while its disc is up; continuous moon state |

What stays:
- the airglow floor (`night_sky_floor`: night is never pitch black; about 25× brighter relative to
  the moon than in reality, a readability choice)
- the lightning flash (`ambient_scale × (1 + 8 × flash)`, an event)
- the compressed moon (0.02 vs the sun's 3: about 1/150 instead of 1/400000, so night is playable)

## Debug views, the reference corner and the test corner

**Debug views** (`r_shadow_debug`):
- `1`: the readout, which `shadow_info` also prints:
  - key light and elevation; direct and sky irradiance, contrast, predicted lit / shade ratio
  - source radius, mode (PCF / PCF ↔ PCSS / PCSS / wide / fixed / off), fade, cascade 0 weight,
    PCSS weight, max half width; veil and cumulus tau
  - moon tile, cloud drift, EV compensation, ground albedo
  - cascades: count × tile, shadow distance, per cascade split / sphere radius / texel size
  - bias of cascade 0 (constant, normal, low-sun factor), kernel range, jitter state (TAA: the
    kernel rotates; otherwise fixed)
  - the viewmodel tile (size, texel, radius, PCSS weight) and the unrendered-sample count
- `2`: penumbra half width (0 .. 0.5 m heat). `3`: blocker distance (0 .. 30 m).
- `4`: cascade (red / green / blue), the viewmodel tile (magenta), wide mode (yellow tint), off
  (grey), darkening over the distance fade.
- `5`: the cloud shadow map. `6`: texel size of the sampled tile (0 .. 10 cm heat).
- `7`: viewmodel tile coverage (green inside, red a viewmodel pixel outside it).
- `8`: lines: each tile's fitting sphere and its square with edges towards the light, the viewmodel
  tile's sphere.

**Reference corner** (dev room, (−50, 0, −11), about 24 × 20 m): 18 % grey ground, a 15 m pole, a
1 m cube, a 20 m × 2 m fin, 4 cm thin, at z = −2.5 (penumbra width at 1 / 5 / 20 m), colour checkers
in the sun and under a canopy. Eight switches on the post at (−40, 0, −4) run `shadow_preset
clear_noon | hazy | light_cloud | overcast | sunset | night_moon | fog | rain`. A switch's `"preset"`
key does this; nothing else runs from level data. The preset sets the weather (locked) and the time:
12:30, the evening sun at 5°, or the next full moon at midnight. It also snaps the driver state.

**Test corner** (same area):
- a 40 m, 6 cm pole at (−61, 0, −20): a long thin shadow across every cascade boundary (seams)
- sloped planes at 15 / 35 / 60° at x −41 / −43 / −45, z −9 (acne at noon, peter-panning at sunset)
- a slatted fence at z −6 (aliasing, shimmer under `shadow_orbit`)
- a 60 m tower at (−95, 0, −11), 45 m west: a caster outside the view
- a wall at (−39, 0, −13) for the first-person rig: stand 1 m east of it (`r_shadow_debug 7`)
- the second post at (−37.5, 0, −4), bottom to top: `sunrise`, `noon`, `sunset`, `night`;
  `fid_off` .. `fid_modern` (Engine Fidelity); `cover_0`, `cover_0.5`, `cover_0.8`, `cover_1`
  (cloud cover over 4 s through the weather, the driver's slow path); `stress`
  (`shadow_switch_stress`); `orbit` (`shadow_orbit`)

**Commands:**
- `shadow_switch_stress [sweep s] [quit]`: the presets Off → Modern → Off → Modern (1 s each) at a
  fixed cover, then on each preset cover 0 → 1 → 0 (the weather's own transition). Every frame it
  measures the reference edge (a blocker 1 m over flat ground in cascade 0, as the shader computes
  it: penumbra half width, the edge offset the bias adds, the fade, the shade ratio) and fails on a
  frame-to-frame jump over 15 % / one texel (penumbra), 25 % / 1.5 texels (offset), 0.02 (fade),
  2 % (ratio), on more than one cutoff toggle per half sweep, or on an unrendered tile. Prints
  `shadow_switch_stress: ... -> PASS | FAIL`.
- `shadow_orbit [deg/s | stop]`: turns the view slowly (default 6°/s) to show swimming.
- `fp_shadow_check`: the drawn first-person body's pelvis / feet against the owner's shadow copy
  (world positions, mm apart; they must coincide).

## Tuning

- **Softness:** `source.aureole_*`. Keep the disc radii physical.
- **When overcast kills the shadows:** `clouds.veil_tau` (the cover at which tau passes ~3 is where
  shadows start fading), and `cutoff.off` / `on`.
- **Cost against quality:** `filter.wide_deg`, `pcss_max_texels`, `r_shadow_quality`.
- **PCF tiers:** `filter.pcf_ref_m` (the assumed blocker distance), `min_texels`.
- **Tier crossfade:** `filter.mode_s`.
- **Drift speed:** `clouds.wind_aloft`.
- **Cascades / bias:** `CSM_*` and `csm_bias` in `shadow_csm.h/.c` (then `test_shadow_csm`).

Change one curve, watch `r_shadow_debug 1` in the corner, then run the tests.

## Tests

**Unit tests (`test_shadow_sky`):**
- the JSON equals the C defaults
- curve validation, monotonic source radius
- the filter on a rasterised fin vs the uniform disc: clear and hazy at 1 / 5 / 20 m, within
  1 – 10 % at ≥ 3 texels
- shading = predicted ratio; ordering clear ≫ hazy > light cloud > overcast ≈ 1
- the 2 min clear→overcast transition: one toggle, exactly under the cutoff; the shaded luminance
  step stays under 0.5 % per frame; re-enable only above the hysteresis; no chatter
- the moon handover: continuous fade and intensity; dawn cut; new moon
- cloud drift = wind × aloft; wide mode hysteresis
- the tiers: one filter (PCSS weight 1 / 0, max width per tier); tier changes every 0.4 s never
  move the effective penumbra more than 15 % in a frame, the search runs while PCSS fades
- cloud sweeps 0 → 1 → 0 (60 s per half) on every tier: at most one cutoff toggle per half, the
  fade steps < 2 % per frame, the shadows come back
- the era keeps contrast; exposure, ground albedo; TAA on a softening edge

**Unit tests (`test_shadow_csm`):**
- every preset table (default, max, era overrides) satisfies the formula assertion (splits end at
  D, cascade 0 texel density per preset, neighbours within 6×, bias from the texel size); prints
  the layout
- every visible point up to a split lies in its tile (with the shader's border) for 1800 random
  cameras × 9 FOV / aspect pairs; the texel size never depends on the rotation; ADS zoom never
  resizes a cascade
- texel snapping: 10 frames of 0.37-texel translation (half with a 0.3°/frame turn): < 0.02 texel
  of drift of a fixed point
- casters 5 m .. 1 km towards the light at 1 .. 89.5° elevation, every heading: inside the depth
  range
- the shadow is full to 90 % of D and gone at D; the cascade index + blend is continuous (no seam
  step) for 1 .. 3 cascades; wide mode uses cascade 1 near
- bias proportional to the texel, the low-sun factor monotonic in 1 .. 3; a penumbra in metres is
  the same on two cascades; the viewmodel tile: < 1 mm texels, the rig inside, snapped

**In-game tests (`python tools/tests/shadow_ref.py [build]`):**
- penumbra at 1 / 5 / 20 m on the fin, from linear EXR captures (the scale calibrated from the
  fin's 2 m shadow)
- lit / shade ratio against the prediction, and its ordering; shade chromaticity
- the 2 min clear→overcast transition: smooth, ends < 1.05, cut under the cutoff
- moon at night and at dawn
- every tier × era: plausible frames, the era's fixed hardness, 0 validation messages
- TAA temporal std on a softening edge

**In-game tests (`python tools/tests/shadow_stability.py [build] [--only <group>]`):**
- `switch`: `shadow_switch_stress` passes, 0 validation messages
- `matrix`: sunrise / noon / sunset / night × the 5 presets × cloud 0 / 0.5 / 1: plausible frames,
  0 validation messages, no unrendered tile
- `translate`: the fin's shadow edge within 1 px over 10 frames of sub-texel translation
- `seams`: across cascade 0's blend band the shade level and edge width never step between rows
- `acne`: the 15 / 35 / 60° planes at noon and sunset: lit faces smooth (< 4 % spread)
- `taa`: a static edge: TAA temporal std < 2 % of the step; FXAA frames identical within 0.5 %
- `viewmodel`: the tile reported on with sub-millimetre texels, the coverage view renders

**Run so far** (Linux, Xvfb + lavapipe, this change): the three unit suites (0 failures), every
shader compiled with glslangValidator, the game built and linked, before / after captures of the
scripted scenes, `fp_shadow_check`, and a validation run over the scenes plus every Engine Fidelity
level, quality 3 and the 2010 era: no shadow-related validation messages. The 40
`VUID-vkCmdDrawIndexed-None-08114` errors in that run (a texture sampler destroyed by the
texture-quality / anisotropy recreation during the fidelity switches) appear identically on the
build before this change. `shadow_ref.py` / `shadow_stability.py` have not been run end to end
(lavapipe renders ~1 frame per second; they are meant for the Windows GPU build).

Known gaps:
- Cloud-shadow motion on the GPU is not measured: the drift is tested on the CPU, and the shader
  only offsets the map.
- The cloud offset is integrated per machine, not replicated, so co-op players may see the patches
  in slightly different places.
- With moon shadows off (Off .. Medium), the moon is unshadowed while the sun owns the cascades.
  Its contrast against a sky lit by the sun is far under the cutoff, so this is invisible.
- A shadow resolution or cascade count change applies in one frame (the bias and kernel follow the
  new texel size in that frame, so only the detail changes); the old maps are not kept for a
  crossfade.
- The volumetric fog (`vol_inject.comp`) still range-checks the last cascade against the whole
  atlas; at its blur this is not visible.
- The viewmodel tile is sampled by the viewmodel only, and player-attached lights do not shadow
  the rig (see [The viewmodel tile](#the-viewmodel-tile)).
