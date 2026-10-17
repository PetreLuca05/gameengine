# Sky-driven shadows

Sun and moon shadows follow the sky: crisp and dark under a clear sun, soft and faint in haze,
gone under overcast, bluish from the skylight, with a penumbra that widens with the distance from
the caster. Nobody sets a shadow strength or softness. Both come from the sky and weather state
every frame.

| File | Role |
|---|---|
| `src/render/shadow_sky.c/.h` | Pure driver: config, curves, contrast, cutoff with hysteresis, source radius, wide mode, PCF steps, moon handover, cloud drift, exposure, ground albedo |
| `assets/render/shadows_sky.json` | Every tuning value; hot reload (saved changes apply within a second, `shadows_sky_reload`) |
| `src/game/game_shadow_sky.c` | Steps the driver from `daynight_environment`; fidelity inputs, `shadow_preset`, `shadow_info`, the `r_shadow_debug 1` readout |
| `src/game/game_daynight.c` | Fills the driver input (sun / moon through the atmosphere, sky SH, clouds, fog, rain, snow, wind) |
| `src/render/render.c` `compute_matrices` | Tiles drawn this frame, the moon tile matrix, the filter's units (`ssky0..2`, `shadow_mpt`, `shadow_span`) |
| `shaders/lit_body.glsl` `sh_tile` | PCSS / PCF steps, receiver-plane depth, wide mode, cutoff fade, the moon tile, debug views |
| `shaders/sky/cloud_shadow.comp`, `env_render.comp` | Beer-Lambert cloud shadows, the sky probe's ground (snow / wet albedo, cosine quadrature) |
| `shaders/post/contact.comp` | Contact shadows: ray length limited by the penumbra |
| `data/levels/devroom.json` | The shadow reference corner |
| `tests/test_shadow_sky.c`, `tools/tests/shadow_ref.py` | Unit tests (including the PCSS kernel on the CPU) and in-game tests |

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

Shadow-map resolution sets a floor. A cascade 0 texel is 2.3 cm at 2048 and 1.17 cm at 4096, so
the clear sun's 1 m penumbra (4.6 mm) is below one texel; contact shadows carry that range. The
tests check widths ≥ 3 texels within 15 % and smaller ones to stay under 2 texels.

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
   - **Wide mode** (full tier): above `wide_deg`, the cascade 0 weight fades to 0 and the pass stops.
   - **PCF steps** (filter model 0): 1 tap, 3×3 or 5×5 by radius, with hysteresis. The fractional
     outer ring blends each change.
   - **Also:** cloud drift (integrated), exposure compensation, ground albedo.
3. `compute_matrices` (render.c) prepares the shadow pass:
   - `shadow_active[0..3]`: no tiles under the cutoff, no cascade 0 in wide mode, tile 3 for the
     moon.
   - The moon matrix: the middle cascade's size, snapped, along the moon's stepped direction.
   - The uniforms (see the table after this list).
   - The shadow pass and the vgeo culling (view slot 4 for the moon, `VG_MAX_VIEWS` 5) skip the
     inactive tiles.
4. `lit_body.glsl` computes `shadow_factor` × `contact` × `cloud_shadow_at` for the key light, and
   `moon_shadow` for the second light. Both are `mix(1, s, fade)`; the contact shadows take the
   same fade.

| Uniform | Contents |
|---|---|
| `ssky0` | tan r, fade, cascade 0 weight, filter model |
| `ssky1` | PCF radius, PCSS max texels, search metres, moon fade |
| `ssky2` | tan r of the moon, debug view, contact scale, moon radius |
| `shadow_mpt` | metres per texel |
| `shadow_span` | metres of light-space depth per NDC unit |

### The filter (`sh_tile`)

- **Blocker search:** a Vogel disc of `search_m × tan r / texel_m` texels, clamped to
  [1, 1.5 × max]. It reads raw depth through binding 15 (`shadow_depth`, nearest sampler).
- **Filter:** half width `d_block × tan r / texel_m`, clamped to [0.5, max], with hardware-compare
  taps. Taps per tier:
  - limited tier: 8 + 8 taps, max 6 texels
  - full tier: 16 + 16 taps, max 32 texels
- **Rotation:** interleaved gradient noise per pixel and frame, which TAA averages.
- **Receiver-plane depth.** Every tap compares against the receiver's plane at the tap. The depth
  gradient comes from the normal under the cascade matrix, clamped at about 80°, plus one texel of
  slope bias. Without this a wide kernel on ground sloped to the light shadows itself: the unit
  test measured the hazy 1 m penumbra 7× too wide before.
- **Units.** Everything is in metres and converted per cascade, so the penumbra is the same across
  cascade blends.
- **Known limit (all PCSS):** the map stores the light-facing surface. A thick caster's penumbra is
  estimated from its front face, which is farther than its silhouette edge, so it comes out
  slightly too wide. The test caster is a 4 cm thin fin.

### Contact shadows

They stay sharp at contact. The ray only runs as far as the penumbra stays under `contact_px`
pixels: the length is `contact_px × pixel_m / (2 tan r)`, capped at the clear-sun 0.35 m. A hit is
`1 - 0.5 t²`: full occlusion at contact (the old 0.4 floor is gone). They fade with the cast
shadows.

### Tiers and eras

`r_shadow_filter` is the filter model:

| Level | Model |
|---|---|
| Off / Low | 0: PCF in 3 steps by the source radius |
| Medium | 1: PCSS, limited |
| High / Modern | 2: full PCSS + wide mode |

- `r_moon_shadows` (High / Modern) gives the moon tile 3.
- Cloud shadows: Medium and up.
- The 2010 era (`r_era 3`) keeps fixed-hardness PCF (`era_pcf`). Its contrast and cutoff are still
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

## Debug views and the reference corner

**Debug views.**
- `r_shadow_debug 1`: the readout, which `shadow_info` also prints:
  - key light and elevation
  - direct and sky irradiance, contrast, predicted lit / shade ratio of the reference surface
  - source radius, mode, fade, cascade 0 weight, PCF radius
  - veil and cumulus tau
  - moon tile, cloud drift, EV compensation, ground albedo
- `r_shadow_debug 2`: penumbra half width (0 .. 0.5 m heat).
- `r_shadow_debug 3`: blocker distance (0 .. 30 m).
- `r_shadow_debug 4`: cascade (red / green / blue), wide mode (yellow tint), off (grey).
- `r_shadow_debug 5`: the cloud shadow map.

**Reference corner** (dev room, (−50, 0, −11), about 24 × 20 m):
- 18 % grey ground
- a 15 m pole
- a 1 m cube
- a 20 m × 2 m fin, 4 cm thin, at z = −2.5: penumbra width at 1 / 5 / 20 m
- colour checkers in the sun and under a canopy

Eight switches on the post at (−40, 0, −4) run `shadow_preset clear_noon | hazy | light_cloud |
overcast | sunset | night_moon | fog | rain`. A switch's `"preset"` key does this; nothing else
runs from level data. The preset sets the weather (locked) and the time: 12:30, the evening sun at
5°, or the next full moon at midnight. It also snaps the driver state.

## Tuning

- **Softness:** `source.aureole_*`. Keep the disc radii physical.
- **When overcast kills the shadows:** `clouds.veil_tau` (the cover at which tau passes ~3 is where
  shadows start fading), and `cutoff.off` / `on`.
- **Cost against quality:** `filter.wide_deg` and `pcss_max_texels`.
- **Drift speed:** `clouds.wind_aloft`.

Change one curve, watch `r_shadow_debug 1` in the corner, then run the tests.

## Tests

**Unit tests (`test_shadow_sky`):**
- the JSON equals the C defaults
- curve validation, monotonic source radius
- the PCSS kernel on a rasterised fin vs the uniform disc: clear and hazy at 1 / 5 / 20 m, within
  1 – 10 % at ≥ 3 texels
- shading = predicted ratio; ordering clear ≫ hazy > light cloud > overcast ≈ 1
- the 2 min clear→overcast transition:
  - one toggle, exactly under the cutoff
  - the shaded luminance step stays under 0.5 % per frame
  - re-enable only above the hysteresis
  - no chatter
- the moon handover: continuous fade and intensity; dawn cut; new moon
- cloud drift = wind × aloft
- wide mode hysteresis; PCF steps and their hysteresis; the era keeps contrast
- exposure, ground albedo
- TAA on a softening edge

**In-game tests (`python tools/tests/shadow_ref.py [build]`):**
- penumbra at 1 / 5 / 20 m on the fin, from linear EXR captures (the scale calibrated from the
  fin's 2 m shadow)
- lit / shade ratio against the prediction, and its ordering
- shade chromaticity
- the 2 min clear→overcast transition: smooth, ends < 1.05, cut under the cutoff
- moon at night and at dawn
- every tier × era: plausible frames, the era's fixed hardness, 0 validation messages
- TAA temporal std on a softening edge

Known gaps:
- Cloud-shadow motion on the GPU is not measured: the drift is tested on the CPU, and the shader
  only offsets the map.
- The cloud offset is integrated per machine, not replicated, so co-op players may see the patches
  in slightly different places.
- With moon shadows off (Off .. Medium), the moon is unshadowed while the sun owns the cascades.
  Its contrast against a sky lit by the sun is far under the cutoff, so this is invisible.
