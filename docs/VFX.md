# VFX: GPU particles, muzzle flashes, bullet impacts

Milestone 1 of the particle system covers muzzle flashes and bullet impacts, with AI-generated
textures. All effects are **data**:
- effect JSON in `assets/vfx/effects/`,
- textures plus metadata in `assets/vfx/tex/`,
- texture recipes in `assets/vfx/gen/`.

The field-by-field schema is in [VFX_REFERENCE.md](VFX_REFERENCE.md).

| Area | Code | Data |
|---|---|---|
| Types, curves, hashing | `src/vfx/vfx_types.h`, `vfx_curve.c` | |
| Library: JSON parsing, selection tables | `src/vfx/vfx_library.c` | `assets/vfx/effects/*.json`, `assets/vfx/tex/*.meta.json` |
| Simulation: instances, scheduling, CPU tier, decals, lights | `src/vfx/vfx_sim.c` (pure, unit tested) | |
| Atlas packing | `src/vfx/vfx_atlas.c` | |
| Game layer: world raycasts, sounds, renderer feed, hooks, dev commands | `src/vfx/vfx.c` | |
| GPU module: compute sim, sort, draws, decals, distortion, bloom | `src/render/vk_vfx.c` | `shaders/vfx_*.comp/.vert/.frag`, `shaders/bloom_*.frag` |
| Editor | `src/ui/vfx_editor.c` | writes `assets/vfx/effects/*.json` |
| Texture generator | `tools/vfxgen/` (`vfxgen.py`, `hf_client.py`, `process.py`, `placeholder.py`, `make_effects.py`) | `assets/vfx/gen/*.json` |
| Tests | `tests/test_vfx.c` (ctest `vfx`), `tools/tests/run_vfx_gpu_test.sh` | |

## Frame flow

```
gameplay hook (shot / impact / casing)                        fixed 60 Hz ticks (vfx_sys_tick)
   fx_muzzle_ex / fx_impact_ex / fx_shell_ex  ──►  VfxSystem: instances ──► burst / rate / distance
   (src/gameplay/effects.c)                          │                     schedule (deterministic per tick)
                                                     ├─ GPU emitters ──► VfxGpuSpawn records
                                                     ├─ CPU emitters ──► CPU particles (Jolt raycasts)
                                                     ├─ decals (ring buffer, budget) and effect lights
render frame (vfx_game_render)
   spawn records + prefix sums, CPU sprites (sorted), decals, instance transforms ──► render_vfx_frame
render_end_frame:
   compute:  reset? → begin → emit (binary search over the prefix) → sub-emit → args →
             update (indirect dispatch) → draw args → bitonic sort of the alpha list
   scene:    static opaque │ decals (depth read-only) │ characters + blended │ scene copy │
             particles: distortion → sorted alpha (GPU, then CPU sprites) → additive │ viewmodel (own depth)
   post:     bloom (prefilter, 13-tap downsample chain, tent upsample) → tonemap
```

The GPU pool is persistent: 1M particles by default (`vfx_max_particles`), stored as SoA
buffers. The CPU never reads them back in the hot path. Counters are copied to a host buffer and read
two frames later, only for stats and to size the sort.

## Tiers

- **GPU tier** (the default): smoke, sparks, debris sprites, dust, flash cores. It handles any count:
  1M particles at about 1 ms of compute on an RTX 3060 Ti.
  - **Depth collision** works against the previous frame's scene depth. The depth target is kept
    (STORE) for exactly this.
  - **Sub-emitters** on death or collision write to a sub-spawn buffer, which the next frame consumes.
  - **Local space (`"local": true`):** particles are stored relative to their instance, and the
    renderer applies a per-instance transform buffer each frame. Flashes stay glued to a moving
    barrel this way.
- **CPU tier** (`"tier": "cpu"` or `"deterministic": true`): mesh particles (casings, shards),
  ribbons, Jolt-raycast collision with bounce sounds, and anything that must be identical on every
  peer (a replicated seed plus the tick clock). The test `determinism: 600 ticks` hashes this state.

## Muzzle flashes

Muzzle flashes are composed from physics: `gameplay/ballistics/bal_muzzle.c` derives each component's
size, distance along the bore, life, brightness and colour from the cartridge (powder charge, gas
temperature, burn length, muzzle pressure), the barrel length and the muzzle device.
`gameplay/ballistics/bal_fx.c` spawns one small effect per component:

- `muzzle_primary`: a dim orange glow at the crown, under a tick.
- `muzzle_mach`: the intermediate flash behind the Mach disk, 2-6 cm out.
- `muzzle_secondary_{1,2,3}`: the fireball 10-50 cm out, as 16 petal variants that never repeat twice in a row.
- `muzzle_prongs`: what a flash hider leaves.
- `muzzle_vent`: compensator and brake port jets, one per port.
- `muzzle_vapor`: the suppressor's wisp.
- `muzzle_smoke`: smoke.
- `muzzle_haze` / `muzzle_can_shimmer`: sustained-fire haze and hot-can haze.
- `muzzle_sparks`: unburnt powder.
- `muzzle_blast`: the blast ring.
- `muzzle_dust`: dust when the muzzle is near a surface.
- `muzzle_light(_shadow)`: the light.

The effects come from `tools/vfxgen/make_ballistic_effects.py`. Device multipliers live in each
attachment's `muzzle_device` block. Details are in [BALLISTICS.md](BALLISTICS.md) "Muzzle flash".
`vfx_game_muzzle(type, device)` still works through a representative cartridge per type, which
is how the `vfx_pedestal` command uses it. `muzzle_preview <weapon> <device>` shows a weapon's
real flash in slow motion.

Rules:
- **Exactly one flash per shot.** The shooter's flash comes from the predicted shot. Observers get it
  from the first pellet's `EV_TRACER` (`id` bit 0 plus type, device and caliber), placed at the
  shooter's visible barrel (`chars_item_muzzle`). The test fires a 1200 RPM burst for 600 ticks.
- **Day/night:** emitters with `exposure_aware` and the flash light are scaled by the frame's
  `exposure_visibility`. That is 1 at night and down to 0.2 in bright daylight, computed from the sun
  intensity. Use `vfx_daynight` to compare.
- **First-person flashes follow the barrel:** the game layer re-parents the latest viewmodel flash to
  the hands' muzzle socket every frame.

## Impacts

Every physics body carries a **surface material**: `BodyDesc.material` → `RayHit.material`.

**Where materials come from:**
- **Level brushes:** a `"material"` per brush (default per type: box/stairs/tunnel concrete, doorway
  plaster, ramp wood), plus `floor_material` / `wall_material` for the room.
- **Entities:** derived from type and shape in `world.c` `entity_material`. Crates are wood, barrels
  metal_thin, gates metal_thick, characters flesh.
- **Hitboxes:** always flesh. A headshot selects `impact_flesh_head`.

**Selection:**
- `impact_<material>` handles all 16 tags. `default` falls back to `impact_concrete`.
- **Caliber scaling** (`vfx_caliber_scale`) changes count, size, energy and decal size.
- **Grazing angles** lower the intensity. Emitters with `"dir": "surface"` stream along the surface.
- **Decals** are only placed on static level geometry (`FX_IMPACT_DECAL`), never on props or
  characters. They are box projected, angle faded, lit (sun plus shadow plus point lights) and capped
  by `vfx_decal_max` (oldest first).
- **Flesh:** blood drops are CPU particles that hit the world and spawn `blood_splat` decals.
- **Water:** splash flipbook, droplets and a ripple ring.

**Sounds:**
- `impact_<class>`, where class is hard / metal / wood / soft / glass / water / flesh.
- CPU debris plays `debris_<class>` on bounce.
- **Casings** (`shell_pistol` / `shell_rifle` / `shell_shotgun`, `fx_shell_ex`) are visual only: they
  fly out of the ejection port with gravity, drag and spin, never collide (no bounce, no rest, no
  raycasts) and play no sound. They shrink away after 0.55 to 0.7 s, about when a hip-height casing
  reaches the ground, so they do not visibly sink into it.
- They are generated by `tools/gen_assets.py --vfx-sounds`.

## Networking

**Effects are never replicated.**
- **Shooter:** predicts its tracer, impacts and flash immediately (`cl_predict_impacts 1`).
- **Host:** sends `EV_IMPACT` to everyone, packing material, caliber and flags into `id`. The shot
  direction goes in `data`, and `ent2` is the shooter slot plus 1.
- **Shooter, on receipt:** skips the host's copy of its own impacts when it predicted them. With
  `cl_predict_impacts 0` it waits for the host's copy instead.
- **Melee impacts:** same path, with caliber `melee`.

## LOD and budgets

| Control | Effect |
|---|---|
| `lod` per effect `[[dist, mul], ...]` | spawn count multiplier by camera distance (viewmodel effects are exempt) |
| `cull_distance`, `priority` 0..3 | no spawn beyond; global instance budget evicts the oldest instance with priority ≤ the new one |
| `max_instances` per effect | the oldest instance of the same effect is replaced |
| `vfx_quality` 0..3 (Graphics menu "Effects quality") | count multiplier 0.35 / 0.65 / 1 / 1.35, sorted particles 8k..131k, depth collision off on Low, light budget 8 / 24 |
| `vfx_decal_max` | decal ring size |
| `vfx_max_particles` | GPU pool size, applied at restart (1M default, 84 MB) |

GPU timings (compute, decals, particles, bloom) come from timestamp queries and appear in the
`vfx_debug 1` overlay line.

## Textures: vfxgen

```
python tools/vfxgen/vfxgen.py --placeholder         # every texture, procedural, free
python tools/vfxgen/vfxgen.py --dry-run             # API cost estimate per recipe (free)
python tools/vfxgen/vfxgen.py flash_core smoke_puff # generate via Higgsfield (cached; --regen to pay again)
python tools/vfxgen/make_effects.py                 # regenerate the effect JSON set (keeps hand-edited files)
```

**API key:**
- Read from `OPEN_HIGGSFIELD_READ_WRITE_TOKEN` in the project `.env`. The file is git-ignored; see
  `.env.example`.
- Only the tool reads it. The game never does.
- The token is never printed.

**Calls and spending:**
- Every call is appended to `tools/vfxgen/logs/api_log.jsonl`, with credentials redacted.
- Each request is estimated first and checked against `--max-usd` (default $20).
- Submissions use a deterministic `Idempotency-Key`.
- Failed or NSFW results are not billed and not used.

**Caching:** downloads are cached under `tools/vfxgen/cache/<name>_<generation hash>`. The hash
covers only what the API sees, so post-processing tweaks never trigger a paid regeneration.

**Post-processing (`process.py`)** turns a raw image or video into game textures:
1. Background removal: luma key on black (emissive and smoke), or chroma key on green or white with
   despill (decals). For decals, enclosed background is filled dark so a hole stays a hole.
2. Crop and fit to cells, flipbook resampling, edge fade and bleed, premultiplied alpha.
3. Motion vectors: coarse-to-fine Lucas–Kanade optical flow, encoded the way the particle shader
   decodes it.
4. 6-way lightmaps. These are approximated from the 2D density by transmittance marches, so
   relightable smoke works without volumetric sources.
5. Normal maps from height (decals and distortion).
6. Optional KTX2 (RGBA8 plus mips).
7. `<name>.meta.json` describing grid, frames, fps, motion vectors, lighting and provenance.

**Model IDs verified against the API** (`GET /models`): `higgsfield-ai/soul/standard`
(images, `resolution` is 720p or 1080p, one image per request in practice) and
`kling-video/v2.5-turbo/pro/text-to-video` (video, 5 s).

**Current set:**
- Generated (Higgsfield):
  - flashes: core, side, mach, prong, petal, suppressed;
  - video flipbooks: smoke, dust, blood mist, flesh mist, water splash;
  - all impact decals (concrete, brick, plaster, tile, metal, wood, dirt, glass, scorch, blood) and
    the tyre skid (`decal_skid`, new recipe);
  - debris sheets (brick, concrete, plaster, tile, wood, dirt, glass), cloth fibres, grass bits,
    blood drops, ember;
  - every flesh texture (droplet, strand, drip, splatter, pool, stains, wounds, bone, helmet spark).
- Procedural: `glow`, `spark`, `tracer_head`, `heat_distort`, and `water_ripple` (the generated
  ripple was a perspective ellipse; the procedural double ring reads better).
- `assets/vfx/tex_placeholder/` keeps the full procedural set. To compare, copy it over
  `build/bin/assets/vfx/tex`, then rebuild to restore.
- `meta.json` says `"placeholder": true` for procedural textures.

**Recipe techniques that worked** (fields: [VFX_REFERENCE.md](VFX_REFERENCE.md#recipe-assetsvfxgennamejson)):
- Surface decals: a full-frame photo of the damaged surface keyed against its own colour
  (`background: corners`, `vignette`). An "isolated on green" prompt returns a whole brick, tile or
  plank instead.
- Debris, petals, drops: one image of many pieces split into the variant cells (`sheet`). It is
  cheaper than one request per variant, and every variant is real.
- Thin or dark flesh: blood on a white wall or white cloth, wounds on pale skin (`corners` key).
  Wounds need "no knife, no hands"; the model likes to add the blade.
- Directional sprites limit the augmented copies (`augment_ops`): drips and splats along u, flames
  with their base at the bottom (`anchor: bottom`).

Committed textures stay small: `tex` is about 8 MB and `tex_placeholder` about 7 MB. The tool warns
above 50 MB; above that, ask before committing.

## Editor and dev room

**Editor** (`F7` or `vfx_editor`):
- Pick or filter an effect, spawn it once or on repeat where you look, and slow time
  (`vfx_timescale`).
- Edit effect, emitter, spawn, update and render parameters live: the emitter table and curve LUTs
  re-upload instantly.
- Save writes the effect JSON back to `assets/vfx/effects/` (source tree and runtime copy).

**Dev room:**

| Thing | Where / command |
|---|---|
| Impact range: one panel per surface material, plus a water pool | x −28 … −16, z −12 (floor label "impact range") |
| Muzzle pedestal | `vfx_pedestal` cycles all 24 flashes; `vfx_pedestal <type> [device]`; `vfx_pedestal off` |
| Day / night | `vfx_daynight [0|1]` (`env_night`) |
| GPU stress test | `vfx_stress [count]` (default 1,000,000) |
| Everything at once | `vfx_spawn_all` |
| Misc | `vfx_spawn <effect> [dist]`, `vfx_list`, `vfx_clear`, `vfx_reload` |

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `vfx_enable` | 1 | master switch (falls back to the legacy line sparks when 0) |
| `vfx_quality` | 2 | 0 low, 1 medium, 2 high, 3 ultra (Graphics menu) |
| `vfx_debug` | 0 | 1: stats line (instances, CPU particles, decals, lights, GPU alive / emitted / dropped / lists / sort size, GPU ms); 2: plus instance markers |
| `vfx_half_res` | 0 | reserved: particles at half resolution once the post stack's half-res infrastructure lands (currently ignored) |
| `vfx_lighting` | 1 | lit particles (sun, shadow, point lights, 6-way); 0 = unlit |
| `vfx_decal_max` | 256 | decal budget |
| `vfx_timescale` | 1 | VFX time scale (cheat) |
| `vfx_max_particles` | 1048576 | GPU pool (restart) |
| `cl_predict_impacts` | 1 | own impacts are shown immediately |
| `r_bloom`, `r_bloom_intensity` | 1, 0.06 | bloom chain |
| `env_night` | 0 | dev night lighting |

## Tests

- **`ctest -R vfx`** (`tests/test_vfx.c`, 336 checks): curve evaluation (const, linear, bezier,
  baked LUT); 600-tick CPU-tier determinism; texture-meta parsing; atlas packing; decal budget; and
  asset checks. The asset checks are: every material resolves its own impact effect, every type ×
  device resolves its muzzle effect, every referenced texture exists, the editor save round-trips,
  and a 1200 RPM burst produces one flash per shot with no repeated variant and no piling.
- **`tools/tests/run_vfx_gpu_test.sh`:** about 360 frames with Vulkan validation enabled. It spawns
  every effect, runs the 1M stress test, `vfx_reload`, switches quality and toggles the system. It
  fails on any validation message (currently 0).
- **Visual scripts:** `tools/tests/vfx_range.cfg`, `vfx_pedestal.cfg`, `vfx_flash.cfg`,
  `vfx_basic.cfg`, `vfx_editor.cfg`.

## Known limits (milestone 1)

- **Mesh particles** (casings, shards) are primitive cylinders and boxes, rendered as regular
  unshadowed draws.
- **Ribbons** are CPU only, made of 8-point trails.
- **6-way lightmaps** are approximated from 2D density, not baked from a volume.
- **`vfx_half_res`** is a placeholder until the post-processing stack adds half-resolution targets.
- **Soul images:** the API returns one image per request, so a generated texture has as many variants
  as requests were paid for (`api_variants`).
