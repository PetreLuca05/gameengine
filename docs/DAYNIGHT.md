# Day / night cycle

A host-authoritative world clock drives a physically based sky, the sun and the moon. The
lighting follows the sky: the key and fill directional lights, cascaded shadows, IBL, fog and
exposure. Lamps switch on at dusk on a schedule.

Code:

| File | What it does |
|---|---|
| `src/world/time_of_day.c/.h` | Pure: time state, wire format, sun / moon / sidereal maths, curves, events, light schedules, config |
| `src/game/game_daynight.c` | Game side: config + hot reload, host clock, events, lamps, the per-frame environment, exposure / fog, gameplay API, commands, clock, editor, tests |
| `src/render/sky/sky.c/.h`, `shaders/sky/*` | Atmosphere LUTs, sky, clouds, cloud shadows, environment + SH |
| `src/render/render.c` | Cascaded shadows, frame bindings |
| `shaders/lit_body.glsl` | SH ambient, environment specular, cloud shadow, fill light, cascade lookup |

Data: `assets/world/daynight.json`.

## Time model and networking

- The host owns a `TimeState` (`World.tod`, 20 bytes on the wire). It holds:
  - an anchor: server tick, time of day in ms and day number;
  - the calendar day of day 0;
  - the real seconds per day (0 = paused);
  - a version.
- Time (in days) = `ref_day + ref_ms / 86 400 000 + (tick - ref_tick) / 60 / day_length`.
- Every command re-anchors at the current tick (`time_set`, `time_scale`, `time_pause`,
  `time_days`, `tod_demo`), so the time never jumps and drift never accumulates.
- **Replication.** The state goes in every snapshot header (`net.c`). Clients evaluate it at their
  interpolated server tick (`net_client_render_tick`), so every machine shows the same sky at the
  same moment. A late joiner is in sync with its first snapshot. Protocol version 8.
- The local solar time is the clock: 12:00 is solar noon (no longitude, time zone or equation of
  time).

## Sun, moon and stars

| Body | Model |
|---|---|
| **Sun** | Ecliptic longitude from the mean anomaly + equation of centre, giving declination and right ascension for the axial tilt. The hour angle comes from the solar time, then elevation and azimuth from the latitude. Within 0.17° of Spencer's declination all year (`tests/test_daynight.c`). |
| **Moon** | Lunar age (`moon_age_days` on day 0, synodic month 29.53 d) gives the elongation from the sun on the ecliptic (the 5° inclination is ignored). Illuminated fraction `(1 - cos E) / 2`, waxing / waning. The sky shader lights the disc as a sphere by the true sun direction, so the phase and the lit limb are correct by construction (the unit test checks the shaded disc against the fraction). |
| **Stars** | The celestial frame from the local sidereal time (hour angle of the sun + its right ascension), so the stars rotate about the celestial pole at the latitude's elevation. |
| **Milky way** | Placed by the galactic pole / centre coordinates. |

## Sky (`src/render/sky`)

### Atmosphere

The atmosphere follows Hillaire 2020: Earth radius 6360 km, Rayleigh, Mie (Cornette-Shanks
g 0.8), ozone. The aerosol density varies with weather and dawn mist (`mie_scale`, quantized in
0.1 steps).

| LUT | Size | When |
|---|---|---|
| Transmittance | 256 × 64 | Only when the aerosols change |
| Multiple scattering | 32 × 32 | Only when the aerosols change |
| Sky view | 192 × 108 | Every frame whose inputs changed (below); absolute azimuth, so the sun and the moon share it |
| Aerial perspective froxels | 32 × 32 × 32, quadratic slices up to `sky_aerial_max` m | Every frame whose inputs changed; `sky_aerial_scale` exaggerates the distance (2 by default) |

The aerial perspective is applied to geometry in `fog_apply`, together with the volumetric fog
froxels.

### Cached passes (PF-0026)

Every sky pass keeps a key: the exact bits of each `SkyParams` / `PostUBO` field its shader reads,
the versions of the images it samples and the pipeline generation (shader hot reload). A pass whose
key equals its last run's records no dispatch and its image keeps those texels (bit-identical by
construction); the pass node stays, empty. `perf_fix_disable PF-0026` runs everything every frame.

| Pass | Key |
|---|---|
| Sky view | the sun / moon vectors and illuminances, the camera altitude, the aerosols, the LUTs |
| Aerial perspective | the same + its range / scale and the camera rays (`inv_view_proj`, jittered under TAA: then it runs every frame) |
| Cloud map (every other frame) | the lights, the layer (cover, altitude, drift, opacity, scale, softness, darkness), the camera position and altitude, the LUTs, the sky view |
| Cloud shadow | its snapped region, the key light, the layer |
| Environment rows (per quarter) | the lights, the night floor, the ground albedo, the camera altitude, the LUTs, the sky view, the cloud map, the cloud shadow; the mips + SH projection of a cycle only when a quarter changed. The easing runs every frame |

Without cloud cover (`clamp(cover) <= 0.001`: `cloud_density` returns 0 first) the cloud map and
the cloud shadow drop the drift, the camera and the layer from their keys. What it saves depends on
what stands still: the clock running moves the sun every frame (nothing is cached), and the cloud
layer drifts with the weather wind even with the weather locked (`cloud_wind_min` 2 m/s), so with
clouds the cloud map, the cloud shadow and the environment change every frame. A paused clock with
a constant camera altitude caches the sky view; a still camera without clouds (perf scene
`sky_static`) caches everything but the aerial perspective (TAA) and the easing. Measured serially
(RTX 3060 Ti, `perf_verify PF-0026` with `r_async_compute 0`): `sky_view` 0.031 -> 0.025 ms in the
meadow (the LUT skipped, the aerial perspective kept), the four sky passes 0.086 -> 0.057 ms in
`sky_static`. A new field read by one of these shaders must join its key (`sky.c` sky_build; a note
in `sky_common.glsl`). The keys are the exact bits: nothing is quantized to get a hit.

### The sky pass (`sky_render.comp`)

It draws every pixel without geometry, in the main view and in the scope picture-in-picture:

- the sky-view LUT, plus the lit ground below the horizon;
- the sun disc: power-law limb darkening per colour (redder at the limb, normalised to the
  sun's illuminance), a small aureole, through the transmittance, HDR so it blooms;
- the moon: a sphere with the lunar albedo map (`assets/textures/moon_albedo.jpg`,
  equirectangular, longitude 0 at u = 0.5, mipmapped; LOD from the pixel footprint), near side
  towards the earth with north up the sky. It is lunar-Lambert lit by the sun, so the phases
  follow and the full moon is flat, with earthshine on the dark side. The disc radiance is scaled
  by `MOON_DISC_GAIN` to the night exposure, otherwise it is a clipped blob. It hides the stars
  behind it.
- `sky_look sun|moon` aims the view at a body. `sky_disc_zoom N` enlarges both discs at the same
  surface brightness, for inspection.
- the stars: two layers of jittered points about a pixel wide, extinguished near the horizon,
  fading out as the sky brightens;
- the milky way: baked once into a celestial equirectangular map;
- the clouds;
- an airglow / starlight floor (`night_sky_floor`), so night is never pitch black.

### Clouds

- A 2D layer at `cloud_altitude_m`: fbm noise drifting with the wind. The drift advances as if a
  day took 24 real minutes, so it is identical on every machine and becomes a time-lapse at
  higher speeds.
- **Cloud map** (`clouds.comp`): 512² in direction space, updated every other frame. The clouds
  are lit by the sun and the moon through the atmosphere (earth shadow, a short march towards
  the light, silver lining) plus sky ambient, and hazed with distance.
- **Cloud shadow map** (`cloud_shadow.comp`): 256² over 2 km around the camera, snapped to its
  texels. It is the Beer-Lambert transmittance through the layer along the key light
  (`exp(-density x tau / mu)`, tau from `assets/render/shadows_sky.json`), and multiplies the key
  light in the lit shaders. It drifts with the weather wind at the layer (docs/SHADOWS.md).
- Hooks are in place for volumetric clouds later: the cloud functions in `sky_common.glsl` and the
  cloud map interface.

### Twilight

There is no special case: the sun keeps lighting the atmosphere below the horizon, the planet
shadow is in the LUTs and the sky view, and the multiple scattering carries the blue hour. The
clock names the phases from the sun's elevation (golden hour, then civil, nautical and
astronomical twilight, then night).

## Lighting follows the sky

### Key and second lights

- Each body's light at the ground is its illuminance × the atmosphere transmittance at its
  elevation. The transmittance is computed on the CPU with the same medium as the shaders, not a
  hand-painted gradient.
- The direct light is attenuated by the cloud sheet's optical depth (`exp(-tau / mu)`, no floor:
  gone under a thick deck); `cloud_light_loss` dims only the sky (and the clouds' own light).
  Both fade in over the disc around the horizon.
- The sun is the **key light** (the cascades, the volumetrics) while any of its disc is up, then
  the moon. While the sun owns the cascades the moon is the **second light**, shadowed in atlas
  tile 3 on the tiers with moon shadows. There is no unshadowed fill light any more (it lit
  through roofs and filled every shadow). docs/SHADOWS.md has the handover.

### Stable shadows

- **Cascades.** 3 cascades in a 2 × 2 atlas of the shadow map. They are fixed-size squares
  (12 m, about 36 m, about 110 m) around the camera, each snapped to its own texels, so rotating
  the camera never changes them. The last 15 % of a cascade blends into the next.
- **Sun movement.** The light direction moves in sub-texel steps: 0.04° by day, 0.25° at night,
  where it updates less often.
- **Single-matrix users.** The volumetrics, particles and decals use the widest cascade.
- **Weather.** Softness, contrast and the overcast cutoff follow the sky and the weather
  (docs/SHADOWS.md: PCSS with the source's angular radius).

### IBL

- **Environment.** An octahedral 128² environment with mips is refiltered in quarters over 4
  frames (amortized). After each cycle, its mips and an SH9 irradiance projection are rebuilt.
- **Easing.** The map and the SH the lit shaders use ease towards each refilter, with
  k = 1 − exp(−dt / 0.35 s), so a refilter never pops. Diffuse ambient is E(n) / π from the SH;
  specular samples the environment by roughness. Both are multiplied by the `ambient_scale` curve,
  1.0 (physical: sun and sky come from one atmosphere, so shadow darkness is the real direct vs
  sky ratio; it was 1.6, which lifted every shadow). Keep it 1 unless a look needs it.
- **What the environment contains.** It holds the sky, the clouds and the lit ground, but not the
  sun or moon discs (the analytic lights cover those).
- **Reflection probes.** There are no local reflection probes yet; SSR stays the local term. A
  probe system should store sky-relative data and add this environment back at run time.

### Fog and exposure

**Fog.** The `fog_density` curve drives the volumetric fog (dawn mist around 05:45), the analytic
fog and the aerosols. The volumetric fog and the god rays use the key light: the sun by day, the
moon at night.

**Auto exposure.** It is clamped to `[night_floor_ev, day_max_ev]` (−1.5 / 7 EV100). The floor stops it from
brightening the night into day: it stays clearly night, keyed by the moonlight. The
`exposure_comp` curve adds the per-time look:

| Time | Compensation |
|---|---|
| Night | −0.7 EV |
| Golden hour | +0.25 EV |

## Lamps (`light_schedule`)

- A level light with `"schedule": true` is on in `[on, off)` from `light_schedule`, with a
  deterministic per-lamp offset of ±`jitter_minutes`.
- The host switches `EF_ON` when the schedule changes, so a light switch can override a lamp until
  the next change.
- An optional `"flicker"` (0..1) is replicated and drawn on clients only (cosmetic).
- The lamp head is drawn emissive while on. Emissive brushes are batched static geometry and are
  not scheduled.

## Weather hooks

The `weather_clouds` (0..1, −1 = the curve) and `weather_fog` (multiplier) server cvars, the cloud
cover and the aerosols drive:

| What | How |
|---|---|
| Sky brightness | `cloud_light_loss` |
| Sun / moon direct light | The cloud sheet's optical depth (shadows_sky.json `veil_tau`) |
| Shadow softness / cutoff | The source's angular radius / the direct vs sky contrast (docs/SHADOWS.md) |
| Cloud shadow strength | Beer-Lambert through the patches (`cumulus_tau`) |
| Sky LUTs | Aerosols |
| Fog | The fog density |
| Star visibility | Dimmed by the cloud cover |

A weather system sets these cvars.

## Data: `assets/world/daynight.json`

The file holds:

- `latitude`, `axial_tilt`, `day_of_year`;
- `day_length_minutes` (24 by default), `start_time`, `moon_age_days`;
- `sun_illuminance` and `moon_illuminance`, in engine units (the lit shaders' sun is about 2.4 at
  noon);
- `night_sky_floor`;
- the cloud layer: `cloud_light_loss`, `cloud_altitude_m`, `cloud_wind_mps`, `cloud_wind_dir`;
- `exposure.night_floor_ev` / `day_max_ev`;
- `light_schedule`;
- `curves`: `fog_density`, `exposure_comp`, `cloud_cover`, `star_brightness`, `moon_size`,
  `sun_size`, `ambient_scale`, as `[hour, value]` keys, piecewise linear, wrapping at 24 h;
- `levels`: per-level overrides of any key (the dev room starts at 16:45).

The file is polled every second and reloaded when it changes; `sky_reload` reloads it at once.

## Commands, cvars, UI

| | |
|---|---|
| `time_set 18:30`, `time_scale 10`, `time_pause [0/1]`, `time_days 3` | Host |
| `tod_demo [s]` | 24 h in 60 s, then back to the previous speed |
| `tod_info` | Time, sun / moon, light level at the player |
| `tod_editor` | Editor window: scrub the time, pause, demo, latitude / date / lighting, edit every curve (add / remove keys), save to the JSON |
| `cl_clock` | On-screen clock: day, time, phase, sun elevation, moon %, clouds, exposure EV |
| `sky_debug 1..6` | Transmittance, multiple scattering, sky view, cloud shadow, environment, milky way |
| `r_atmosphere` | 0 = the old gradient sky and hemispheric ambient |
| `sky_aerial_scale`, `sky_aerial_max` | Aerial perspective |
| `tod_on_sunrise` / `tod_on_sunset` / `tod_on_midnight` / `tod_on_noon` | Console scripts run on every machine at the event |
| `tod_night_ambience` / `tod_day_ambience` | Ambient track started at sunset / sunrise |

## Scripting with time

Gameplay API (`game.h`):

- `world_time_hours(w)`, `world_day_number(w)`;
- `world_is_night(w)`: sun below −6°;
- `world_sun_elevation_deg(w)`;
- `world_light_level(w, pos)`: 0..1, combining the sun or moon visible from `pos` (ray cast), the
  sky and the lamps that are on. Intended for zombie perception, spawn rules and flashlight hints;
- `daynight_add_listener(fn, user)`: sunrise / sunset / midnight / noon.

How events fire:

- The host detects them per tick: the sun's upper limb crosses −0.833°, or the day / noon boundary
  is crossed.
- It broadcasts `EV_TOD`. Every machine runs its listeners and the `tod_on_*` scripts.

## Dev room yard

The yard is south-east of the room:

- an asphalt road with five street lamps on the schedule (one flickers);
- towers at about 320, 600, 1000 and 1300 m for the aerial perspective;
- the floor is 4 km wide;
- the PBR test corner and every surface sit under the sky.

## Performance

Measured on an RTX 3060 Ti at 1600 × 900, averaged over a 24 h cycle (`tod_test cycle`):

| Pass | Average |
|---|---|
| Sky view + aerial perspective (`sky_view`) | 0.069 ms |
| Cloud map (`sky_clouds`, every other frame) | 0.069 ms |
| Environment refilter + easing (`sky_env`) | 0.054 ms |
| Sky (`sky`) | 0.158 ms |
| **Total** | **0.35 ms** (budget 0.5) |
| Cloud shadows | 0.023 ms (budget 0.2) |
| LUTs | Rare: 0.009 ms on average |
| Aerial perspective in `fog_apply` (with the volumetric fog) | 0.13 ms |

The sky passes run on the async compute queue (docs/RENDERING_POST.md "Async compute", PF-0027),
next to the shadow maps. PF-0026 numbers: docs/perf/FINDINGS.md.

## Tests

| Test | Checks |
|---|---|
| `test_daynight` (ctest `daynight`, 59 checks) | Sun elevation / azimuth against reference values: solstice noons at 40N, Greenwich and Cape Town; the 47N sunrise / sunset hour and azimuth; the equinox east point; polar day / night; declination vs Spencer over the year. Moon phases (illuminated fraction, waxing, shaded disc vs fraction, full moon opposite the sun). `TimeState` pack / unpack round trip; late join at many ticks; time_set / scale / pause / days without jumps. Curves (wrap). One sunrise / sunset / noon / midnight per day. Lamp schedules and per-lamp offsets. |
| `tools/tests/daynight_cycle.cfg` (`tod_test cycle quit`) | 24 h in 60 s from noon: exposure never below the night floor (EV −1.50 .. 0.89, floor −1.5: the clamp holds); IBL irradiance never jumps between frames (max change 3.6 % per frame at 24 h / 60 s, 909 refilters); every event exactly once; the sky budgets. PASS. |
| `.toolchain/tnet.sh` (`tod_net_*.cfg`) | Host changes the time and speed, a client joins late; `tod_test dump` shows the identical state and the identical hours at fixed ticks on both. |
| `tools/tests/daynight.cfg` | Screenshots through the day (noon, afternoon, sunset, blue hour with lamps, night with the moon and stars, dawn) and the `sky_debug` views. Also validation-clean. |
| `run_post_tests.py --only toggle` | Every pass toggled with validation (the dev room now renders the atmosphere): 0 messages. |
