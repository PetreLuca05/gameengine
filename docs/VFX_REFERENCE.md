# VFX data reference

Field by field reference for effect JSON (`assets/vfx/effects/*.json`), texture metadata
(`assets/vfx/tex/*.meta.json`) and generation recipes (`assets/vfx/gen/*.json`). Overview and
workflow: [VFX.md](VFX.md). The parser is `src/vfx/vfx_library.c`; unknown keys are ignored.

## Common value types

| Type | Accepted forms |
|---|---|
| **range** | `0.5` (fixed) or `[min, max]` (seeded uniform per particle) |
| **color** | `[r, g, b]` or `[r, g, b, a]`, linear, HDR allowed (> 1) |
| **curve** (over normalized particle life 0..1) | `1.0` (constant) · `[[t, v], [t, v], ...]` linear · `[[t, r, g, b, a], ...]` for color · `{"type": "bezier", "keys": [[t, v], ...]}` Hermite with automatic tangents · bezier keys with explicit tangents `[t, v.., in.., out..]` |

Curves have up to 12 keys and are baked into 32-sample LUTs for the GPU. Multi-channel curves
broadcast a scalar key to all channels.

## Effect

```json
{
  "name": "impact_concrete",          // default: file stem
  "duration": 0.1,                    // instance lifetime (s); 0 = until its particles die
  "loop": false,
  "priority": 1,                      // 0 low .. 3 critical (budget eviction; 3 ignores cull distance)
  "max_instances": 48,                // per effect; the oldest is replaced
  "cull_distance": 160,               // no spawn beyond (m)
  "lod": [[25, 0.7], [60, 0.4], [110, 0.2]],   // count multipliers by distance
  "light": { "color": [1, .7, .4], "intensity": 6, "radius": 5, "duration": 0.025, "exposure_aware": true },
  "decal": { "texture": "decal_concrete", "normal": "decal_concrete_n", "size": [0.07, 0.11],
             "lifetime": 90, "depth": 0.05, "variants": 4, "tint": [1, 1, 1, 1] },
  "sound": "impact_hard",             // optional: played at spawn (impacts default to impact_<class>)
  "emitters": [ ... up to 12 ... ]
}
```

Lights are optional. Each is budgeted per frame, and the brightest and nearest are kept. With
`exposure_aware`, a light is scaled by the frame's exposure visibility.
Decals spawn only when the hook says the surface accepts them (static level geometry).

## Emitter

```json
{
  "name": "core",            // "core" in muzzle effects = the flash counted by the one-flash-per-shot test
  "tier": "gpu",             // gpu | cpu
  "deterministic": false,    // true forces the CPU tier, seeded from the replicated seed + tick
  "enabled": true,
  "lod_cull": 0,             // skip this emitter beyond (m), 0 = never
  "spawn": { ... }, "update": { ... }, "render": { ... }
}
```

### spawn

| Key | Type | Default | Meaning |
|---|---|---|---|
| `mode` | burst \| rate \| distance | burst | burst at start; continuous rate; per meter travelled |
| `count` | range | 1 | particles per burst (stochastic rounding of fractional counts × quality × LOD × caller multiplier) |
| `bursts`, `burst_interval` | int, s | 1, 0.05 | repeated bursts |
| `rate` | /s | 0 | `mode: rate` |
| `per_meter` | /m | 0 | `mode: distance` (trails) |
| `delay`, `duration` | s | 0 | emitter start / active time (rate) |
| `shape` | point \| sphere \| box \| cone \| disc \| surface | point | spawn volume |
| `radius`, `box` | m, [x,y,z] | 0 | shape size (box = half extents) |
| `angle` | deg | 0 | cone half angle around the base direction |
| `dir` | socket \| normal \| reflect \| incoming \| up \| random \| side \| surface | socket | base direction: effect forward (−Z of its rotation, muzzle axis) · hit normal · incoming reflected about the normal · along the shot · world up · uniform sphere · alternating ± effect right (compensator vents) · tangent to the surface along the shot (grazing hits) |
| `dir_bias` | 0..1 | 0.5 | reflect / surface: blend towards the normal |
| `speed` | range m/s | 0 | × caller energy (caliber) |
| `inherit` | 0..1+ | 0 | fraction of the owner's velocity |
| `life` | range s | 1 | |
| `size` | range m | 0.1 | × caller scale (caliber) |
| `rotation`, `spin` | range deg, deg/s | 0..360, 0 | |
| `color`, `color2` | color | white | random blend per particle |
| `offset` | m | 0 | spawn offset along the base direction |
| `local` | bool | false | particles live in the effect's space (follow a socket: first-person flashes) |
| `frame_random` | bool | false | random start frame / variant |
| `variant_no_repeat` | bool | false | never the same variant twice in a row for this emitter |

### update

| Key | Default | Meaning |
|---|---|---|
| `gravity` | 0 | multiplier of 9.81 m/s² (negative rises) |
| `drag` | 0 | 1/s exponential |
| `wind` | 0 | response to the global wind vector |
| `turbulence` | 0 | number = strength, or `{strength, freq, speed}`: curl noise (GPU: 3D curl texture; CPU: hashed swirl) |
| `size_curve`, `color_curve`, `alpha_curve`, `velocity_curve`, `spin_curve` | 1 | over life; `alpha_curve` is a scalar curve multiplying only alpha (ignored when `color_curve` is present) |
| `frame_over_life` | false | flipbook frame = life × frames |
| `fps` | 0 | or a fixed flipbook rate (frames / s) |
| `motion_vectors` | false | blend flipbook frames with the texture's motion vectors (needs `_mv.png`) |
| `collide` | none | none \| depth (GPU: previous frame's depth buffer) \| physics (CPU: Jolt raycasts, material sounds, sub-emitters) |
| `bounce`, `friction` | 0.3, 0.4 | collision response |
| `kill_on_hit` | false | die on first contact |
| `attractor` | – | `{pos: [x,y,z] effect space, strength}` (CPU tier) |
| `vortex` | 0 | rad/s around the effect up axis (CPU tier) |
| `sub_emitter` | – | `{on: death \| collision, effect: "name", count}`; GPU sub-emitters spawn the GPU emitters of that effect (up to 3), CPU ones spawn the whole effect (with decals) |

### render

| Key | Default | Meaning |
|---|---|---|
| `mode` | alpha | add (emissive, order independent) · alpha (premultiplied, sorted back to front) · distort (refracts the scene copy) · mesh (CPU: casings, shards) · ribbon (CPU trails) · none |
| `orient` | billboard | billboard · stretch (velocity stretched) · cross (billboard + quad along the axis: flash cores) · axis (quad along the velocity / socket axis: side flames) · normal (facing the spawn normal: ripples) · segment (ribbon piece) |
| `lighting` | none | none · simple (sun + shadow + point lights + ambient, per particle) · sixway (needs `_lm0/_lm1` maps, else simple) |
| `texture` | – | texture name (`assets/vfx/tex/<name>.meta.json`); empty = soft round sprite |
| `normal` | – | normal map texture (distortion; decals use the effect's `decal.normal`) |
| `mesh` | – | mesh particles: `casing_pistol`, `casing_rifle`, `shell_shotgun`, `shard_*`, `splinter_*`; also the sound key `<kind>_<class>` |
| `emissive` | add: 1 | HDR multiplier |
| `exposure_aware` | false | alpha × exposure visibility (fades flashes in daylight) |
| `soft` | 0.15 | soft particle fade distance against the scene depth (m) |
| `near_fade` | 0.25 | fade when closer to the camera than this (m); off for first-person spawns |
| `stretch`, `stretch_max` | 0.02, 8 | seconds of velocity per size unit, max length multiplier |
| `aspect` | 1 | quad width / height |
| `distortion` | 0.02 | refraction strength (uv units) |
| `light` | – | per particle light `{radius, intensity, color}` (CPU tier sparks / embers; budgeted) |

## Texture metadata (`<name>.meta.json`)

Written by vfxgen, next to `<name>.png` (premultiplied RGBA unless `premultiplied: false`).

| Key | Meaning |
|---|---|
| `file` | image file (default `<name>.png`) |
| `width`, `height` | atlas pixel size |
| `grid` | `[cols, rows]` flipbook layout, frame 0 top-left, row major |
| `frames` | frame count (≤ cols × rows) |
| `variants` | > 1: the cells are independent variants (picked per particle) instead of an animation |
| `fps`, `loop` | flipbook playback hints |
| `motion_vectors` | `<name>_mv.png` exists: RG = per-frame flow to the next frame, 0.5 = none; decoded as `(tex*2-1) * cell * 0.15` |
| `lighting` | `sixway`: `<name>_lm0.png` (R right, G left, B top) and `<name>_lm1.png` (R bottom, G back, B front) |
| `premultiplied` | colour is premultiplied by alpha |
| `placeholder` | procedural texture (regenerate with the API when wanted) |
| `kind`, `source`, `recipe`, `recipe_hash`, `generated`, `tool_version` | provenance |

All textures are packed at load into a 2048² RGBA8 array atlas with mips (shelf packer, 2 px padding,
half-texel inset). Textures larger than a page are halved until they fit.

## Recipe (`assets/vfx/gen/<name>.json`)

| Key | Meaning |
|---|---|
| `name`, `kind` | texture name; emissive / volume / sprite / decal / distortion |
| `grid`, `frames`, `variants`, `cell`, `fps`, `loop` | output layout (cell = pixels per frame) |
| `model` | Higgsfield model id; absent = procedural only |
| `prompt`, `negative_prompt`, `duration`, `aspect_ratio`, `resolution`, `cfg_scale` | API parameters (images: `resolution` 720p / 1080p; video: `duration` 5 / 10) |
| `api_variants`, `images_per_request` | how many image requests to pay for (variants); images per request |
| `background` | black (luma key) · green / white / magenta (chroma key with despill) |
| `alpha` | `{black, white, gamma, albedo}` luma key or `{tol, soft}` chroma key |
| `crop` | `[x0, y0, x1, y1]` fractions kept (drops props the model added) |
| `fill_holes` | fill keyed-out regions not connected to the border (bullet hole centres) |
| `trim` | `[t0, t1]` fraction of a video kept |
| `pad`, `edge_fade`, `edge_margin` | fit padding, border fade |
| `motion_vectors`, `lighting` (`sixway`/`simple`), `normal_map`, `normal_strength`, `raw` | outputs |
| `placeholder` | `{type, seed, ...}` procedural generator (`tools/vfxgen/placeholder.py` `GENERATORS`) used with `--placeholder`, for recipes without `model`, and as the fallback when generation fails |

Only `model`, the prompts and the API parameters form the cache key, so editing post-processing
fields re-processes the cached download without paying again.

## Gameplay hooks (C)

```c
fx_muzzle_ex(pos, dir, d->vfx_muzzle_type, d->vfx_muzzle_device, viewmodel);
fx_impact_ex(pos, normal, incoming, material, caliber, FX_IMPACT_HEAD | FX_IMPACT_FLESH | FX_IMPACT_DECAL);
fx_shell_ex(pos, velocity, caliber);
vfx_game_spawn("effect_name", &params);   // anything else (VfxSpawnParams from vfx_spawn_params(pos))
```

`EV_IMPACT.id = fx_impact_pack(material, caliber, flags)`, `EV_IMPACT.data` = incoming direction,
`EV_IMPACT.ent2` = shooter slot + 1. `EV_TRACER.id` bit 0 marks a shot's first pellet:
type << 4 | device << 8 | caliber << 12.
