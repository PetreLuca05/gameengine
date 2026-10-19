# Day / night cycle

A host-authoritative world clock drives a physically based sky, the sun and the moon. The
lighting follows the sky: the key and fill directional lights, cascaded shadows, IBL, fog and
exposure. Lamps switch on at dusk on a schedule. Every time-of-day quantity is a continuous
function of the clock, evaluated every render frame: nothing moves in steps at any day length
([Continuity](#continuity)).

Code:

| File | What it does |
|---|---|
| `src/world/time_of_day.c/.h` | Pure: time state, wire format, the client timeline and slewed clock, sun / moon / sidereal maths, curves, events, light schedules, config |
| `src/game/game_daynight.c` | Game side: config + hot reload, host clock, the client clock, events, lamps, the per-frame environment, exposure / fog, gameplay API, commands, clock, editor, tests |
| `src/game/game_tod_probe.c` | `tod_test continuity / trace / shots`: per-frame continuity measurements ([Tests](#tests)) |
| `src/render/sky/sky.c/.h`, `shaders/sky/*` | Atmosphere LUTs, sky, clouds, cloud shadows, environment + SH |
| `src/render/render.c` | Cascaded shadows, frame bindings |
| `shaders/lit_body.glsl` | SH ambient, environment specular, cloud shadow, fill light, cascade lookup |

Data: `assets/world/daynight.json`.

## Time model and networking

- **World time** is a double: game seconds since the epoch (day 0, 00:00), a function of the
  fractional server tick. The host owns a `TimeState` (`World.tod`, 40 bytes on the wire) with one
  breakpoint, the anchor:
  - the anchor's server tick (a double: the rendered moment of the last change) and world time;
  - the real seconds per day after the anchor (0 = paused) and before it;
  - a `time_set` blend: the old time's offset from the new and the ticks it eases over;
  - the calendar day of day 0, a version (never 0 once set: it wraps to 1).
- The clock, `tod_seconds(state, tick)`:

  | Tick | World time |
  |---|---|
  | at or after the anchor | `ref_sec + (tick - ref_tick) / 60 x 86400 / day_length + blend_sec x (1 - smoothstep((tick - ref_tick) / blend_ticks))` |
  | before the anchor | `ref_sec + blend_sec + (tick - ref_tick) / 60 x 86400 / prev_length`: the clock the host showed until the change |

- **Host.** Every frame evaluates the clock at `tick + alpha` (the render frame's position between
  sim ticks, like every interpolated transform). Every command re-anchors at that same moment, the
  one on screen (`time_scale`, `time_pause`, `tod_demo`: no jump, the new rate from there on;
  `time_set`: an ease; `time_days`, presets, the editor's scrub: a step). The pause menu keeps alpha
  where it was (it no longer zeroes the accumulator).
- **Replication.** The state goes in every snapshot header (`net.c`, protocol 30). A client renders
  about `cl_interp` + the latency behind the host. It keeps the states it received
  (`TodTimeline`) and evaluates the newest whose anchor its clock has reached: before it, the new
  state's segment before the anchor is the old clock. A speed change, a pause or a blend therefore
  happens on the client's timeline at the host tick it was made at, with no jump, and every machine
  shows the same sky at the same server tick. A late joiner is in sync with its first snapshot.
- **The client clock** (`TodClock`, `daynight_client_frame`): it runs on the real time between frame
  starts and follows the interpolated server tick (`net_client_render_tick_at`, sampled at the frame
  start: the offset estimate jumps when a packet arrives early, then drifts back) through a
  first-order loop (0.5 s) whose correction never exceeds 2 % of the frame's advance (the slew). It
  snaps only when off by more than 30 ticks (0.5 s: joining, a stall). Entity interpolation keeps
  using `net_client_render_tick`.
- The local solar time is the clock: 12:00 is solar noon (no longitude, time zone or equation of
  time).

## Sun, moon and stars

| Body | Model |
|---|---|
| **Sun** | Ecliptic longitude from the mean anomaly + equation of centre, giving declination and right ascension for the axial tilt. The hour angle comes from the solar time (the double day fraction: no float hours in any angle), then elevation and azimuth from the latitude, in double. Within 0.17° of Spencer's declination all year (`tests/test_daynight.c`). |
| **Moon** | Lunar age (`moon_age_days` on day 0, synodic month 29.53 d) gives the elongation from the sun on the ecliptic (the 5° inclination is ignored). Illuminated fraction `(1 - cos E) / 2`, waxing / waning. The sky shader lights the disc as a sphere by the true sun direction, so the phase and the lit limb are correct by construction (the unit test checks the shaded disc against the fraction). |
| **Stars** | The celestial frame from the local sidereal time (hour angle of the sun + its right ascension), so the stars rotate about the celestial pole at the latitude's elevation. |
| **Milky way** | Placed by the galactic pole / centre coordinates. |

## Sky (`src/render/sky`)

### Atmosphere

The atmosphere follows Hillaire 2020: Earth radius 6360 km, Rayleigh, Mie (Cornette-Shanks
g 0.8), ozone. The aerosol density varies with weather and dawn mist (`mie_scale`, continuous:
every pass reads this frame's).

| LUT | Size | When |
|---|---|---|
| Transmittance | 256 × 64 | Once (and after a shader reload): it holds optical depths, rgb Rayleigh + ozone and a Mie at unit density, so `trans_eval` gives the exact transmittance for any aerosols |
| Multiple scattering | 32 × 32 | When the aerosols moved 0.5 % (`MS_MIE_STEP`); a workgroup per texel, one of the 64 directions per invocation (~0.2 ms measured on the async queue; the old serial build of both LUTs ~1.4 ms) |
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
| Cloud map (every other frame; every frame in a time-lapse, the clock over 4× the default day) | the lights, the layer (cover, altitude, the exact double drift, opacity, scale, softness, darkness), the camera position and altitude, the LUTs, the sky view |
| Cloud shadow | its snapped region, the key light, the layer (the double drift) |
| Environment rows (per image and quarter) | the lights, the night floor, the ground albedo, the camera altitude, the LUTs, the sky view, the cloud map, the cloud shadow; a completed map whose quarters all equal the latest's is not finished again (no mips, no SH, no rotation). The blend rests once it holds the latest |

Without cloud cover (`clamp(cover) <= 0.001`: `cloud_density` returns 0 first) the cloud map and
the cloud shadow drop the drift, the camera and the layer from their keys. What it saves depends on
what stands still: the clock running moves the sun every frame (nothing is cached), and the cloud
layer drifts with the clock (`cloud_wind_min` 2 m/s at the default day), so with clouds the cloud
map, the cloud shadow and the environment change every frame while the clock runs. A paused clock
freezes the drift too: with a constant camera altitude it caches the sky view, the cloud map and
the cloud shadow; a still camera without clouds (perf scene `sky_static`) caches everything but
the aerial perspective (TAA). Measured serially
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

- A 2D layer at `cloud_altitude_m`: fbm noise drifting with the wind. The drift advances with the
  world clock, in real seconds of the default day (`day_length_minutes`): it pauses with the clock
  and becomes a time-lapse at higher speeds; a jump of the clock (`time_set ... instant`,
  `time_days`) moves nothing. It is integrated in double without a wrap; the sky splits it per noise
  octave into an exact lattice cell (mod 2^32, where the hash wraps) and a fraction
  (`cloud_octaves`, `sk_fbm_drift`), so the noise is the same function at any drift: after 100 game
  days (~1e6 m) a frame's step is still exact. Each machine integrates its own (the weather wind is
  replicated, the integral is not).
- **Cloud map** (`clouds.comp`): 512² in direction space, updated every other frame, every frame
  in a time-lapse. The clouds are lit by the sun and the moon through the atmosphere (the earth's
  shadow fades over the disc, a short march towards the light, silver lining) plus sky ambient,
  and hazed with distance.
- **Cloud shadow map** (`cloud_shadow.comp`): 256² over 2 km around the camera, snapped to its
  texels. It is the Beer-Lambert transmittance through the layer along the key light
  (`exp(-density x tau / mu)`, tau from `assets/render/shadows_sky.json`), and multiplies the key
  light in the lit shaders. It drifts with the weather wind at the layer (docs/SHADOWS.md). It
  fades out while the key light is low (under 5°: its projection `L.xz / L.y x altitude` races over
  the ground) and within 2° of the sun / moon handover, where the key changes body (the moon as the
  second light has no cloud shadow): nothing steps when the moon takes the cascades.
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
- **Sun movement.** The light direction is the body's exact direction, every frame (sun and moon,
  key and second light; below 1.15° it is held at the horizon, continuously). The cascades stay
  stable by snapping their light-space origin to whole texels about a pivot near the camera (only
  the translation is snapped, never the direction: docs/SHADOWS.md "A turning light"); the far
  cascades PF-0028 keeps redraw once the light turned half a texel of a 10 m caster's shadow.
- **Single-matrix users.** The volumetrics, particles and decals use the widest cascade.
- **Weather.** Softness, contrast and the overcast cutoff follow the sky and the weather
  (docs/SHADOWS.md: PCSS with the source's angular radius).

### IBL

- **Environment.** An octahedral 128² environment with mips is refiltered in quarters over 4
  frames (amortized) into one of three maps. After each cycle, its mips and an SH9 irradiance
  projection are built and it becomes the latest; the latest becomes the previous.
- **Blending.** The map and the SH the lit shaders use are the previous refilter blended into the
  latest by the cycle phase (frames since the latest completed / 4): piecewise linear in time, a
  refilter never pops, and the lag is a constant ~5 frames at any day length (the exponential ease
  it replaces, k = 1 − exp(−dt / 0.35 s), trailed a time-lapse by 20 game minutes and more). Diffuse ambient is E(n) / π from the SH;
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

## Continuity

Every time-of-day quantity is a continuous function of the world clock, evaluated per render
frame: the sun / moon directions and elevations, the star rotation, the moon phase, the light
colour and intensity, the sky's inputs (lights, aerosols), the cloud drift, the curves (fog,
exposure compensation, stars, sizes, ambient), the sky shadow driver. Nothing waits for a
threshold, a tick or a timer to move.

### Root-cause report (the sun moved in steps)

Measured on the base (6de60cd) with `tod_test continuity` before any change, dev room shadow
corner, Modern, per frame against the exact clock (step / expected; "zero" = the step is under 5 %
of the expected one):

| Cause | Applied | Evidence | Fix |
|---|---|---|---|
| **Update cadence: a hysteresis on the shading / shadow direction** (`game_daynight.c`: the key light moved only once it was 0.04° (sun) / 0.25° (moon) off, the second light 0.25°) | yes: **the main cause** | default day: the key held on 444 of 490 frames at 60 fps (91 %), 1293 of 1345 at 144 (96 %), 252 of 300 at 30, then a step of up to 14× / 33× / 8× the frame's motion; moonlight at night held on 290 of 295 frames (60 fps), steps up to 66×; the fin's shadow edge in per-frame EXR captures frozen on 32 of 35 frames, then a jump. The sun disc used the exact direction: it glided while the light and every shadow (cascades, moon tile, contact shadows, god rays, cloud shadows) jumped | removed: the exact direction every frame (held at the horizon below 1.15°, continuously) |
| **Cascades redrawn at a lower rate than rendering** (PF-0028: far cascades 2-3 kept every other frame unless the light turned 0.25°) | yes | time_scale 60: cascades 2 / 3 kept on 50 % of frames with a 0.23° lag (the frame's sun motion, under 0.25°); the fin's edge in cascades 2 / 3 frozen on 11 / 12 of 23 frames (30 Hz judder); a kept tile showed the other body for one frame at the handover | kept tiles redraw once the light turned half a texel of a 10 m caster's shadow, at most 0.02° (docs/SHADOWS.md "Far cascade staggering"): the default day still keeps them above ~8°, a time-lapse redraws every frame |
| **time_scale / time_pause at tick granularity** (re-anchored at the integer tick while the frame showed tick + alpha) | yes | host: time_scale 1 → 60 stepped 13.5 game s where 0.9 .. 10.4 were possible, 60 → 1 stepped −17.4 game s (backwards), pause −0.74 game s | anchors at the rendered tick (a double) |
| **Interpolation missing between host updates: the client applied a new state at once** (evaluated behind the host, the new rate ran backwards from the new anchor) | yes | client (LAN, 144 fps): every speed change jumped ±217 .. 294 game s (1 → 60 backwards), the clock step 0.34 .. 2.61× the expected, 2559 of 4045 frames outside ±5 % | the state carries the segment before its anchor; the client keeps the states and switches at their host tick (`TodTimeline`) |
| **Client clock offset snapping forward** (the server-time estimate jumps when a packet arrives early, sampled mid-frame) | yes | part of the above: the client's per-frame clock rate 0.34 .. 2.61 | its own clock (`TodClock`): real time between frame starts, slewed ≤ 2 % to the estimate sampled at the frame start; a snap only beyond 30 ticks |
| **time_set at tick granularity, always a snap** | yes (by design) | an instant jump; `daynight_story_drive` snapped whenever the story's float clock drifted 15 game s | `time_set` eases over 1.5 s (`instant` for a step); the story drive eases its corrections over 2 s (step only beyond 30 game min) and re-anchors exact rate changes |
| **Precision: float hours in the hour angle** | partly | default day 144 fps: sun step −1.24 .. +0.66 %, a 240-min day −2.1 .. +16 % (investigation emulation) | angles from the double day fraction; directions normalised in double: default day 144 fps within 0.25 %, 240-min day 2.6 % (the float direction itself) |
| **Precision: float accumulation** (the cloud drift `+= vel x dt` in float, `fmod 1e6`) | partly | the cloud field jittered after hours (0.06 m ulp at 5e5 m) and teleported at the 1e6 m wrap; real time, per machine, running while paused | the drift integrates the world clock's advance in double without a wrap; the sky splits it per noise octave (exact) |
| **Amortized LUT / IBL / cloud passes moving the light** | no (for the light) | the key's step on LUT rebuild / refilter frames equalled the others (1.0000) | — |
| **Amortized passes stepping the sky** | partly | the aerosols quantized to 0.1 (4-8 % Mie steps, 30 LUT rebuilds a day); the IBL eased in real time (0.35 s: 20+ game minutes behind in a time-lapse); the cloud map every other frame | exact transmittance for any aerosols (an optical-depth LUT), the multiple scattering rebuilt at 0.5 % steps; the IBL blends its last two refilters by the cycle phase (a constant ~5 frames); the cloud map every frame in a time-lapse |
| **Texel snapping of a quantized direction** | yes (with the hysteresis) | the snap itself was right (translation only); the light views pivoted about the world origin, so a continuously turning light re-phased the texel grid far from the origin (0.47 texel per frame 2 km out) | translation-only snapping about a camera-near pivot (docs/SHADOWS.md "A turning light") |
| **Hard switches in the sky** | partly | the clouds lost the sun in one frame at the earth's shadow; the moon's scattering switched on at 2.9°; cloud shadows switched body at the handover | faded over the disc; a smooth moon weight; cloud shadows fade around the handover and below 5° of key elevation |
| **Render-time interpolation** | no | the host already evaluated tick + alpha; the pause menu zeroed alpha (a backward step on pausing) | the pause keeps alpha |

### The implementation

- **Clock**: world seconds as a double, anchors at fractional ticks, the segment before the anchor,
  eases (`time_of_day.c`; see [Time model](#time-model-and-networking)).
- **Lights**: `daynight_environment` passes the exact sun / moon directions every frame; the
  shadow driver's fades and source radius run `time_scale` times faster in a time-lapse (they keep
  up with the sun) (docs/SHADOWS.md "Pipeline").
- **Shadows**: matrices from the exact direction every frame; texel snapping on translation only,
  about the camera-near pivot; far tiles kept only while the light stays within half a texel of a
  10 m caster's shadow.
- **Sky**: continuous aerosols, the optical-depth transmittance LUT, the IBL blend, the cloud
  drift on the clock (exact noise), faded switches.
- **Cost** (A / B, `tod_continuity.py perf`, two alternating runs each, RTX 3060 Ti shared with
  other work, so read the spread): dev room 3.13 / 2.89 ms per frame (base / this), 3.14 / 2.96 ms in
  a time-lapse; the vegetation pine forest 16.8 / 16.5 ms, 16.3 / 14.2 ms: within the noise. What a
  time-lapse adds: the cloud map every frame (+0.02 .. 0.03 ms), the multiple scattering rebuilds
  (+0.005 .. 0.04 ms averaged during the dawn mist), the far cascades every frame (PF-0028's saving
  off while the light moves fast). The default day keeps every saving (cascades kept above ~8° of
  sun, the cloud map every other frame, the LUTs only in the mist).

### Measured after the change

| Run (`tod_continuity.py cont`, 30 / 60 / 144 fps) | Before | After |
|---|---|---|
| Default day: shading / shadow key per-frame step / expected | 0.00 .. 14.4 (60 fps), 91 % zero frames | 0.9996 .. 1.0004, none outside ±5 % |
| Default day at night: the moon key | 0.00 .. 66.4, 98 % zero | 1.0000 .. 1.0000 |
| time_scale 60, a whole day (sunset, moon, sunrise) | key ok; cascades 2 / 3 kept 50 %, 0.23° behind | key, moon, tip 1.0000; cascade 2 never kept, cascade 3 only within its allowance |
| Speed changes (1 → 60 → 1, pause, resume, 10): steps outside the old / new rate | 4 of 5 (60 fps), 6 of 6 (144) | 0 of 6 |
| Client (LAN, 144 fps, the host changing speed) | clock 0.34 .. 2.61, 2559 of 4045 outside, 4 jumps of ±217 .. 294 game s | 0.994 .. 1.012 (the correction at most 0.012 of a frame's advance, the slew 0.02), 0 outside, 3 speed changes inside the old / new rate, 0 snaps |
| A whole 10-minute day (time_scale 2.4, 60 fps, 35916 frames) | — | clock 1.0000, sun / key 0.9996 .. 1.0004, moon 0.9998 .. 1.0000, no azimuth or elevation reversal |
| 100 days later (30 / 60 / 144 fps) | — | key 0.9995 .. 1.0004 (60 fps), the clock and the cloud drift 1.0000 |
| Fin shadow edge per frame (EXR, 960 × 540, FXAA): default day, cascade 0 | frozen 32 of 35 frames, a jump of 0.59 px | 0 frozen; the largest step 0.35 px (a texel of cascade 1: the map's own quantization) |
| Fin shadow edge, time_scale 60, cascades 2 / 3 | frozen 11 / 12 of 23 frames | 0 / 0 |

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
| `time_set 18:30 [seconds\|instant]` | Host: eases the time there over 1.5 s (or the seconds) on the same day; `instant` = a step. Scripts and tests that capture right after say `instant` |
| `time_scale 10`, `time_pause [0/1]`, `time_days 3` | Host; `time_scale` / `time_pause` from the moment on screen, no jump; `time_days` is a step |
| `tod_demo [s]` | 24 h in 60 s, then back to the previous speed |
| `tod_info` | Time, sun / moon, light level at the player |
| `tod_test continuity <s> [quit] [tag]`, `tod_test trace <frames> <csv>`, `tod_test shots <frames> <dir>` | The continuity probe ([Tests](#tests)) |
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

Since the continuity work ([Continuity](#continuity); A / B on the same machine, alternating, 24 h in
60 s, other GPU work around, so the spread is wide): the cloud map runs every frame in a time-lapse
(0.032 .. 0.048 → 0.098 .. 0.105 ms); the multiple scattering follows the aerosols (0.004 .. 0.007 → 0.017
.. 0.019 ms averaged over the day, nothing outside the mist and weather changes); the environment is
unchanged (0.036 .. 0.040 → 0.038 ms); the sky view and the sky stay within the run-to-run spread. At
the default day nothing changes measurably (`tod_continuity.py perf`).
| Aerial perspective in `fog_apply` (with the volumetric fog) | 0.13 ms |

The sky passes run on the async compute queue (docs/RENDERING_POST.md "Async compute", PF-0027),
next to the shadow maps. PF-0026 numbers: docs/perf/FINDINGS.md.

## Tests

| Test | Checks |
|---|---|
| `test_daynight` (ctest `daynight`, 89 checks) | Sun elevation / azimuth against reference values: solstice noons at 40N, Greenwich and Cape Town; the 47N sunrise / sunset hour and azimuth; the equinox east point; polar day / night; declination vs Spencer over the year. Moon phases (illuminated fraction, waxing, shaded disc vs fraction, full moon opposite the sun). `TimeState` pack / unpack round trip; late join at many ticks; time_set / scale / pause / days without jumps. **Continuity**: re-anchoring at a fractional tick keeps the time (1e-9 game s) and the new rate; before the anchor the old clock; pause / resume; a time_set blend starts at the old time, never steps more than its ease allows, is monotonic forward and lands on the target; a speed change inside a blend neither jumps nor misses the target; an instant set is a step at the anchor. The sun per render frame over whole days (a 10-minute day at 30 / 60 / 144 fps, time_scale 60, the default day, the 240-minute day, after 100 and 2100 days): no azimuth or elevation reversal, every step within ±5 % of the rate (0.02 .. 0.27 %, 2.6 % at the 240-minute day / 144 fps: the float direction); one 144 fps frame after 100 days exact to 1e-6 game s; the version never 0; a non-finite state dropped. **A client** 6.3 ticks behind the host, states arriving 2 frames late, the host going 1 → 60 → 0 → 1 and a blend: the host's clock at the same tick within 1e-6 game s, every step between 0 and the fastest rate. **The client clock** against a target jittering ±5 ms, jumping 8 ms forward and drifting back for 60 s: the correction never above the 2 % slew, within 1.5 ticks, no snap; a 0.5 s stall snaps once. Curves (wrap). One sunrise / sunset / noon / midnight per day. Lamp schedules and per-lamp offsets. |
| `tod_test continuity <s> [quit] [tag]` (`game_tod_probe.c`) | Per frame against the clock's rate × the frame's real time: the clock, the sky sun (great-circle step), the shading / shadow key, the moon light (second light by day, key at night), the analytic shadow tip of a 10 m pole, the cloud drift: within ±5 %, no zero frame; the azimuth never backwards, the elevation never reversing in a half day; a rate change steps between the old and the new rate; a blend never over its ease; the far cascades' light lag within the hold rule (and in texels of a 10 m caster's shadow); on a client the clock correction within the slew and no snap; the key's steps on LUT rebuild and refilter frames reported apart. Prints the fps, the GPU frame and the sky passes. PASS / FAIL |
| `python tools/tests/tod_continuity.py [build]` | `cont`: the probe at r_maxfps 30 / 60 / 144: a whole 24 s day at time_scale 60 (sunset, the moon key, sunrise), the default day, the moon at night, 100 days later, the commands (time_scale 1 → 60 → 1, pause, resume, 10, a time_set blend). `edge`: the fin's shadow edge in per-frame EXRs (dev_fixed_fps 60, FXAA) in cascade 0 at the default day and at time_scale 60, cascades 2 / 3 at time_scale 60: no frozen frame, no step over the prediction + max(0.5 px, 1.25 texels). `net`: a host changing the speed and a client measuring its own clock. `long` (on request): one whole 10-minute day. `perf` (on request): frame time, GPU frame, sky passes with the clock running, the dev room and the vegetation forest. All PASS |
| `tools/tests/daynight_cycle.cfg` (`tod_test cycle quit`) | 24 h in 60 s from noon: exposure never below the night floor (EV −1.50 .. 1.87, floor −1.5: the clamp holds); IBL irradiance never jumps between frames (max change 3.5 % per frame at 24 h / 60 s, 910 refilters: the blend of the last two refilters, as smooth as the old ease and ~5 frames behind instead of 0.35 s); every event exactly once; the sky budgets (0.41 / 0.43 ms against 0.5, two runs; this is a time-lapse, so the cloud map runs every frame). PASS. |
| `.toolchain/tnet.sh` (`tod_net_*.cfg`) | Host changes the time and speed, a client joins late; `tod_test dump` shows the identical state and the identical hours at fixed ticks on both. |
| `tools/tests/daynight.cfg` | Screenshots through the day (noon, afternoon, sunset, blue hour with lamps, night with the moon and stars, dawn) and the `sky_debug` views. Also validation-clean. |
| `run_post_tests.py --only toggle` | Every pass toggled with validation (the dev room now renders the atmosphere): 0 messages. |
