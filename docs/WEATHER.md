# Dynamic weather

Weather is a set of continuous parameters, never an enum, so any state blends smoothly into any other. The host owns the state and replicates it. Every machine evaluates the same blend from the server tick, so everyone sees the same sky, the same rain and the same lightning.

| Part | Where |
|---|---|
| Model: parameters, presets, transitions, surfaces, lightning, scheduler, pack, lens droplets | `src/world/weather.c` / `.h` (pure, unit tested) |
| Game glue: replication, commands, events, `weather_at()`, puddle mask, renderer / sky / post / audio hooks, debug overlay, demo, in-game tests | `src/game/game_weather.c` |
| Surface terms: wetness, puddles, ripples, snow | `shaders/weather.glsl`, used by `lit_body.glsl` and `gbuffer_body.glsl` |
| Precipitation: streaks, flakes, splashes | `shaders/precip.vert` / `.frag`, `draw_precip()` in `src/render/render.c` |
| Rain occlusion map: a top-down depth of the static world | `src/render/render.c` (`PIPE_SHADOW`, cascade 3), frame set binding 9 |
| Puddle mask: per level | `bake_puddles()` in `game_weather.c`, frame set binding 10 |
| Lens droplets | CPU simulation in `weather.c`; post passes `src/render/post/post_weather.c`, `shaders/post/lens_drops.comp`, `lens_apply.comp` |
| Clouds: coverage, density / opacity, softness, storm darkness | `src/render/sky/sky.c` (`clouds2.x`, `clouds3`), `shaders/sky/sky_common.glsl`, `clouds.comp` |
| Fog | the froxel volumetrics: weather fog density / height, ambient-lit in-scattering (`PostSettings.fog_ambient`) |
| Presets | `assets/world/weather/*.json` |
| Tests | `tests/test_weather.c`, `tools/tests/weather.cfg`, `tools/tests/weather_net.sh` |

## Parameters

| Parameter | Range | Notes |
|---|---|---|
| `cloud_cover` | 0..1 | Sky coverage. Also drives sun / moon light loss, star visibility, softer shadows when overcast. |
| `cloud_density` | 0..1 | Layer opacity and darkness. A thin veil is low; storm decks are high. |
| `precipitation` | 0..1 | Intensity. Rain or snow comes from `temperature`: rain above +2 °C, snow below −1 °C, a smooth mix between. |
| `fog_density` | 1/m, 0..0.2 | Fed into the volumetric fog (the larger of the weather and time-of-day values). |
| `fog_height` | m | Height of the fog layer. |
| `temperature` | °C | Rain ↔ snow, melting, drying speed. |
| `wind_dir` | deg | The direction the wind blows towards (0 = north / −Z, 90 = east / +X). |
| `wind` | m/s | Mean wind speed. |
| `gust` | 0..1 | Gust amplitude. `weather_wind()` returns direction, strength and gust; a later wind system can override it. |
| `lightning_rate` | strikes/min | Rate of lightning strikes. |

Accumulated surface state is integrated on every machine and resynced with every pack:

- **`wetness`:** rain soaks surfaces in about 40 s. They dry over 2–6 minutes: faster when warm, sunny or clear.
- **`puddles`:** fill once surfaces are soaked (full after about 3 minutes of rain) and drain slower than surfaces dry.
- **`snow`:** settles below 0 °C (about 3 minutes to full cover) and melts above 0 °C, faster in rain.

## Presets and composition

Each file is `assets/world/weather/<name>.json`. A file may give any parameter key (as named above) plus `transition`, `min_stay` and `max_stay` in seconds. A missing file falls back to the built-in values. A key left out of a file keeps the current value when presets are composed.

| Preset | Character |
|---|---|
| `clear`, `little_cloudy`, `cloudy`, `overcast` | Increasing cloud cover, minutes-long transitions |
| `fog` | Dense ground fog, still air |
| `light_rain`, `rain`, `storm` | Storm adds strong gusts and lightning (4 strikes per minute) |
| `light_snow`, `snow`, `blizzard` | Blizzard adds low visibility and 16 m/s wind |

- **Composition:** `weather_set overcast+rain` applies the presets left to right on top of the current target.
- **Transitions:** each parameter has its own duration and curve (`weather_param_duration`): clouds take the whole transition (up to 15 minutes), rain arrives in 20–60 s, fog in about a minute, temperature slowly.
- **Retargeting:** a new target set mid-transition starts from the current blend, so nothing pops.

## Scheduler

The scheduler is host-only and off by default. Turn it on with `weather_random on`, or in a level with `"weather": {"preset": "...", "random": true}`.

- It picks the next preset from a Markov-style table: clear → cloudy, cloudy → rain or fog, rain → storm or back, snow only when cold, and so on.
- Each pick stays between the preset's `min_stay` and `max_stay`, plus the transition time. The wind veers a little with every change.
- `weather_lock 1` freezes it.
- All randomness comes from the state's seed and the tick, so a run replays identically.

## Replication

- **Snapshots:** each snapshot carries a flag byte, optionally followed by the 120-byte pack (`weather_pack`). The host sends the pack for 3 snapshots after every change (`version` bump) and then every 10th snapshot. Its first snapshot to a new client always has it, so late joiners sync at once.
- **Client side:** the client unpacks the pack and evaluates the same `weather_eval(tick)`. It integrates the surfaces between packs.
- **Lightning:** strikes are a deterministic function of the seed and the tick, so host and clients flash on the same tick. Thunder is delayed by distance / 343 m/s.
- **Protocol:** version 10.

## Gameplay API

Declared in `game.h`:

| Function | Purpose |
|---|---|
| `weather_at(world, pos)` | Returns `WeatherLocal`: `exposed` (0 under a roof … 1 open sky), `indoor`, rain / snow reaching pos, sheltered wind, temperature, wetness, snow cover, fog. |
| `weather_exposure(world, pos)` | Five upward casts per 1 m cell, cached for 5 s. 10,000 cached queries take about 0.3 ms. |
| `weather_grip(world, pos)` | Ground grip multiplier, 0.4..1: wet −15 %, puddles −10 %, snow −45 %. For vehicles and sliding. |
| `weather_hearing_scale(world)` | Rain and wind mask sound: 1 … about 0.55 in a storm. For AI hearing. |
| `weather_add_listener(fn, user)` | Events `WEV_RAIN_START` / `STOP`, `WEV_SNOW_START` / `STOP`, `WEV_STORM_START` / `STOP` and `WEV_LIGHTNING`. |
| `weather_puddle_mask_at(x, z)` | The baked puddle mask. |

Events fire on every machine from the evaluated parameters, with hysteresis: start at 0.12, stop at 0.06. Console hooks: `weather_on_rain_start`, `weather_on_rain_stop`, `weather_on_snow_start`, `weather_on_snow_stop`, `weather_on_storm_start`, `weather_on_storm_stop`.

## Rendering

**Sky**
- Cloud cover and opacity come from `cloud_density`.
- Edge softness comes from fog and thin layers; darkness from precipitation and density (`clouds3`).
- Cloud shadows can be disabled with `r_cloud_shadows 0`.
- Sun and moon light loss grows with cover × density (at least 8 % of the light remains).
- Lightning raises the ambient (IBL) scale ×(1 + 8 × flash) and the exposure for a few frames.

**Fog**
- Weather fog goes into the froxel volumetrics.
- Thick fog is lit by the whole sky (`fog_ambient`), so it reads as fog rather than darkness.

**Rain and snow**
- Procedural instances in a 28 × 16 m box around the camera, with no simulation state. Each instance has a hashed spot that falls, drifts with the wind and wraps.
- Streaks are oriented along their velocity; flakes are camera-facing with turbulence.
- They are lit by the sky ambient, a little key light and lightning.
- Instances under a roof collapse, tested against the occlusion map.
- Splashes are spawned per time slot on the top surface from the occlusion map.
- Counts per `r_precip_quality`: 4,000 / 9,000 / 16,000 streaks × intensity, and 300 / 700 / 1,300 splashes × rain. `-1` = auto from `r_post_preset` (low → 1, medium → 2, high and ultra → 3).
- Precipitation is drawn after the particles in the main view and in the scope picture-in-picture.

**Rain occlusion map**
- A 512² D32 orthographic depth map of the static draws, ±48 m around the camera, from 60 m above.
- Re-rendered when the camera moves more than 4 m, or every 30 frames.
- Read by the precipitation and by every surface: dry under roofs and overhangs.

**Wet surfaces** (`weather_surface()`; the C mirror is `weather_wet_material()` for tests)
- Porous dielectrics darken by up to about 55 %. Every surface gets glossier, down to roughness 0.18, and walls get less of it.
- Metals barely change colour.
- The G-buffer writes the same roughness, so SSR reflects on wet ground.
- The viewmodel is never affected.

**Puddles**
- The level's mask is baked at load: a 256² top-down height bake over 192 m. 1–5 cm dips fill first, flat ground gets faint patches that fill only in heavy rain, and authored `"puddles"` boxes add to it.
- Cover is `smoothstep` of the mask against the puddle level.
- Puddle surfaces get roughness 0.03 (or 0.2 with `r_puddle_reflections 0`), with rain ripples, or wind ripples when it is dry. Ripples fade out between 12 and 35 m so they don't alias.

**Snow**
- Covers up-facing surfaces: white, roughness 0.62, sparkle glints towards the sun.
- Nothing settles under roofs.

**Lens droplets** (first person only)
- Rain hits the lens more when looking up or moving forward, and not under a roof.
- Drops grow, merge, run down with trails once heavy, slide when strafing or turning, and evaporate when dry or under cover.
- The game simulates up to 192 drops (`weather_lens_step`).
- `lens_drops.comp` builds a half-resolution height / normal map. `lens_apply.comp` refracts the HDR image through it, with a slight blur, darkened rims and a sky highlight.
- The pass runs after the sights and before bloom and exposure.
- The scope picture-in-picture is a separate render and never gets drops.
- Settings: `r_lens_drops`, `r_lens_drops_intensity`.

**Budget**
- Measured at 1600 × 900 on the dev machine in heavy rain (`weather_test frames`): lens passes about 0.2 ms, the transparent pass including precipitation about 0.03 ms. The occlusion map redraws the static draws once every 30 frames or 4 m. Well within the 1.5 ms budget.
- The low preset (`r_post_preset 0`) uses quality 1 and renders the same terms; checked by `weather.cfg`.

**Audio**
- `rain_loop`: louder in the open, muffled under a roof.
- `wind_loop`: by wind speed and snowfall.
- `thunder`: delayed by distance.
- Volume: `snd_weather`. The sounds are generated by `tools/gen_assets.py` (`gen_weather_sounds`).

## Settings

Settings → Graphics → Weather: rain / snow quality, puddle reflections, lens droplets (on / off and intensity), cloud shadows.

## Console

| Command | |
|---|---|
| `weather_set <preset[+preset]> [seconds]` | New target (host). Without seconds: the preset's transition time. |
| `weather_lock [0\|1]`, `weather_random on\|off` | Scheduler control (host). |
| `weather_params` | Print every parameter, the surfaces, the flash and the exposure here. |
| `weather_params <name> <value> … [time <s>]` | Custom target (host). |
| `weather_surface <wet> <puddles> <snow>` | Force the accumulated state (host; screenshots and tests). |
| `weather_demo [seconds\|stop]` | Every preset in turn (default 2 minutes), with surfaces sped up. |
| `weather_debug 1\|2` | Overlay: parameters from → to, surfaces, exposure, drops, grip, quality. `2` adds exposure probes around the player. |
| `weather_test indoor\|drops\|dump\|rates\|frames [quit]` | In-game checks; see Tests. |
| `post_status` | Also prints fog density, height and falloff. |

## Dev room weather yard

The yard is west of the dev room, around (−50, 0, 22). It has:

- a roofed shelter (dry, no rain under it);
- a wall with an overhang (a dry strip);
- a sunken square (fills with water first);
- a placeholder car (dry underneath);
- two authored puddle areas.

Stand at (−50, 0, 34) facing north and run `weather_demo`.

## Tests

**`test_weather`** (ctest `weather`) covers:
- presets and composition, plus loading the JSON files;
- deterministic, monotonic and smooth blending, per-parameter durations, the wind's short arc, continuous retargeting;
- pack / unpack and a late joiner evaluating identically, including lightning;
- surface rates (soak, fill, dry, drain, snow, melt) and clamping;
- wet material ranges;
- storm strike frequency and distances;
- the scheduler: deterministic, minimum stay, lock;
- the lens simulation: 300 frames headless (counts, finite values, running drops, evaporation under a roof, wipe, determinism).

**`tools/tests/weather.cfg`** runs:
- `weather_test indoor` (shelter, overhang, car, open yard), `drops`, `rates`, `dump`;
- screenshots of every preset in the yard, the shelter in rain, and the low post preset;
- GPU timings (`weather_test frames`).

**`tools/tests/weather_net.sh`**: the host changes the weather, a client joins 6 s later, and both dump the evaluated weather at fixed server ticks. The values must match.
