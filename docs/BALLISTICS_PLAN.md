# Ballistics, muzzle flash, tracers: plan (pre-implementation)

This plan comes before any code. It covers what exists today, the design, the data tables, the per-shot cost budget, the phases, and the open decisions.
When it is implemented, it becomes `docs/BALLISTICS.md`. Numbers marked ≈ come from a rough point-mass solver run while planning. The final reference values come from `tools/ballistics_ref.py`, which uses the full G1/G7 tables.

## 0. What exists today (survey)

| Area | Today | Consequence |
|---|---|---|
| Shot | `weapons_tick` → `fire()` (weapons.c:414). Each pellet does `trace_shot(eye, cone_dir, range, view_tick)`, an instant ray from the **eye**. Then `apply_hit` and `push_shot_events` (`EV_TRACER`, `EV_IMPACT`, `EV_SOUND`). | Becomes "spawn a projectile". `trace_shot` is split into one call per segment. |
| Seed | `shot_seed(cmd->seq, slot, ws->shots)`, a PCG seed for each shot. The magazine seed is `RecoilState.mag_seed`. Both live in `PlayerSim`, which the host sends back for reconcile. | Deterministic, replicated seeds already exist. Tracer index and ballistic RNG reuse them. |
| Projectiles | Crossbow only. One `ET_PROJECTILE` **entity** per bolt, Euler gravity, no drag/wind, no lag compensation, host-only (clients see the replicated entity). Its impact event packs the material wrongly (`id = hitbox?1:0`). | Entities are too heavy for 500 in flight (4096 total, replicated). Bullets get their own pool. The crossbow moves onto the pool later. |
| Hitboxes | Per-part capsules (`HitboxSet`, 17 `BodyPart`s), 32-tick rewind history, `hitbox_raycast` returns the first entry only. `chars_raycast` loops over all 2048 character slots for each ray. | Needs a per-capsule exit point (chord thickness) and a broadphase grid (see §6). |
| Materials | `SurfaceMaterial` enum (17 values, vfx_types.h:46), one per **physics body** (`RayHit.material`). No materials.json. | Add `assets/ballistics/materials.json` keyed by the existing names. |
| Physics | `phys_raycast` gives the closest hit with one ignore body. No multi-hit ray and no exit-face query. | Add `phys_raycast_body` (a ray against one body's shape) to measure thickness. Fall back to a reverse ray. |
| Muzzle VFX | 24 hand-tuned `muzzle_<type>_<device>.json` (core / side / smoke / embers / heat). The light is effect-level. "Exposure aware" is faked from sun intensity. | Replaced by component effects composed at runtime from physics inputs. |
| Tracers | Debug lines (`dbg_line`, 0.12 s, a fake 400 m/s head). | Rewritten from scratch. |
| Impacts | `impact_<material>` effects scaled by a cosmetic `VfxCaliber`. `VfxCaliberScale.decal` is never used (bug). | Scaled from impact energy instead. The decal bug is fixed along the way. |
| Audio | `audio_play(name,pos,vol,cat)`: 48 voices, inverse attenuation, **no delay, no filter**. Thunder keeps its own queue of `now + d/343` entries. | Add scheduled playback plus a per-voice low-pass filter. |
| Wind | `wind_at(WindField*, pos)` is pure and deterministic. The game glue is per-frame, on the render timeline, with local sources and client cvar overrides, so it is **not** host/client deterministic. | Ballistics builds its own `WindField` from the replicated `WeatherParams` and the tick, with no sources. |
| Weather | `WP_TEMPERATURE`, `WP_PRECIP`, `WP_FOG`, `WP_WIND*`. No humidity. | Humidity is derived from fog, precipitation and temperature (vapor trails only). Temperature feeds the speed of sound and air density. |
| Exposure | `render_exposure_ev()`, auto-exposure EV100, read back 2 frames late. `render_direct_exposure_ev()` is used when Fidelity is Off. | Drives the flash brightness curve. |
| Fire | No foliage fire state exists. | A `fire_try_ignite()` stub (log plus debug draw). |
| Weapons → ammo | `ammo_9mm` (pistol, smg), `ammo_rifle` = 5.56 (rifle), `ammo_762` (lmg, dmr, sniper), `ammo_shell` (shotgun, 8 pellets). | Each ammo item gets a `cartridge` id. |

## 1. Module layout

```
src/gameplay/ballistics/
  bal_data.c/h      cartridges / materials / devices JSON, hot reload (mtime poll), content hash for the join manifest
  bal_drag.c/h      G1 + G7 Cd(Mach) tables (79 points, linear interp) + exponential approximation      [pure]
  bal_sim.c/h       BulletState step: RK2 midpoint, drag, gravity, wind, transonic yaw, spin drift      [pure]
  bal_surface.c/h   ricochet / penetration / flesh decisions, exit velocity, deflection                 [pure]
  bal_acoustics.c/h crack emission point + arrival time, report delay, LPF cutoff, speed of sound       [pure]
  bal_muzzle.c/h    flash composer: (cartridge, barrel, device, heat, near_surface, EV) → component params [pure]
  bal_tracer.c/h    tracer pattern per magazine, ignition / burn / burnout state, streak length          [pure]
  bal_world.c/h     projectile pool, per-tick swept rays, lag compensation, hits → apply_hit, events, budget
  bal_client.c/h    prediction, replay of remote shots, reconciliation, crack/whizz/report scheduling
  bal_debug.c       ballistics_debug drawing, range commands, muzzle_preview
```

Everything marked `[pure]` builds into `test_ballistics` with no Jolt, Vulkan or world, following the `test_recoil` pattern. Those modules also build and run in this Linux container. The in-engine tests (script runs, validation layers, screenshots) need the Windows box.

## 2. Bullet simulation

### 2.1 State (≈ 80 B; a 1024-entry pool is 80 KB)

```c
typedef struct BulletState {
    vec3 pos, vel;          // m, m/s
    float bc_mul;           // 1, or ×0.35 while tumbling after a ricochet / yawed exit
    float yaw_rad;          // transonic / ricochet yaw that drives extra cone and drag
    float dist;             // path length, m (tracer ignition / burnout)
    u32 rng;                // PCG stream: shot_seed ^ pellet index
    u16 shot_id;            // (slot << 12) | (ws->shots & 0xFFF)
    u8 cart, flags;         // BF_SUPERSONIC_EVER, BF_SUBSONIC, BF_TRACER, BF_RICOCHET, BF_PENETRATED, BF_FRAGMENTED
    u8 events;              // number of ricochets + penetrations (max 4 total)
    u32 fire_tick;
    u32 view_tick256;       // shooter's view at fire time; host rewind = this + (tick - fire_tick)*256
} BulletState;
```

### 2.2 Step (fixed 60 Hz, sub-stepped)

- **Sub-step count.** `n = clamp(ceil(|v|·dt / 20 m), 1, 4)`. Integration uses RK2 midpoint. Drag is evaluated at the start and middle of each sub-step.
  - Drag deceleration: `a = ρ·Cd_G(M)·|v|²·π / (8·BC_SI)`, with `BC_SI = BC_lb/in² × 703.07 kg/m²`.
  - Mach number: `M = |v_air| / c`, where `v_air = v − wind`, so drag acts on air-relative velocity. That is where wind drift comes from.
- **Atmosphere.** Per tick, from the replicated weather:
  - `c = 331.3 + 0.606·T °C`
  - `ρ = 101325 / (287.05 · (T + 273.15))`
  - An `altitude` constant per level (0 by default).
- **Drag model.** `bal_drag_model` is 0 (exponential `a = k·v²`, with k fitted per cartridge at load) or 1 (G-table).
  - It is a **CVAR_SERVER**. The host's Fidelity picks the default at map load (Off/Low → exp, otherwise table), and the value is replicated.
  - Client fidelity must not change the authoritative trajectory.
- **Wind.** `wind_at` is sampled once per tick per bullet, at the sub-step-0 position. The field is ballistics-owned: built from `WP_WIND_DIR / WP_WIND / WP_GUST` at `tick·SIM_DT`, with no sources, and is identical on host and client.
- **Transonic instability.** While `0.8 < M < 1.2`:
  - `yaw += k_trans·dt` (k from cartridge stability, default 0.02 rad/s).
  - The extra lateral deflection is `rng_gauss(rng)·yaw·|v|·dt`.
  - Drag is multiplied by `1 + 2·yaw²`.
  - It is small and deterministic. It is visible only for 9 mm and for 7.62×39 past ~430 m.
- **Spin drift** (Litz): `drift = 1.25·(Sg + 1.2)·t^1.83` inches. It is applied as a lateral velocity term once past 300 m. The `twist` field is optional, and Sg comes from the Miller formula at load.
  - At 500 m this is about 2–5 cm, so it is a "tiny" term as the brief asks.
- **Termination:**
  - `|v| < 60 m/s`, or `t > 6 s`, or `dist > cart.max_range` (default 2 km), or below the world floor.
  - Open world: the bullet ends outside the streamed chunks.
- **Determinism:**
  - float32 with a fixed operation order. `sqrtf` is correctly rounded; no other libm calls run in the step. Air properties are computed once per tick.
  - Build flags: `-ffp-contract=off` (no FMA), and the MSVC default `/fp:precise` on the module (asserted in CMake).
  - Host and client run the same binary with the same inputs (eye, aim, seed, tick, weather), so the trajectory is bit-identical until the bullet hits something that differs between the two worlds.

### 2.3 Launch

- **Origin and zero.** The projectile starts at the **eye**, as hitscan does today, so gameplay, ADS and crosshair stay unchanged.
  - The launch direction is pitched up by the weapon's `zero_angle`. That angle is solved at load (bisection over the same sim, standard atmosphere, no wind) for `zero_distance`: pistol 25, smg 50, rifle/lmg 100, dmr 200, sniper 300 m.
  - Hold-over is then real: a 300 m zero for the sniper, and pistol rounds arc at range.
- **Muzzle obstruction.** A ray from the eye to the muzzle. If it is blocked, the bullet starts at the blocking point, so the barrel cannot shoot through a wall it is poked into.
- **Visual path.** Tracer and smoke start at the actual muzzle socket and blend onto the true path over the first 15 m.
- **Muzzle velocity.** `mv = interp(cart.mv_curve, weapon.barrel_length) × device.velocity_mul`. For example, a suppressor gives ×1.015.
  - A device POI shift (`poi_moa [x,y]`) is added to the launch direction.
- **Pellets.** Each pellet is its own bullet with `rng = seed ^ i`. The cone comes from `recoil_cone_dir`, unchanged.

### 2.4 Swept hits, per tick segment (host authoritative)

Order per segment `p0 → p1`:

1. **World:** `phys_raycast(PL_MASK_WORLD)`, which also covers dynamic props and vehicles.
2. **Tires:** `ray_tires(view)`.
3. **Characters:** `chars_raycast_grid` with the rewound tick `view_tick256 + (tick − fire_tick)·256`.
   - This keeps the rewind offset equal to the shooter's latency for the whole flight.
   - It is clamped to the 32-tick history (530 ms of ping plus interpolation).
4. **Legacy C_HITBOX entities:** same rule.

The closest hit wins. It returns `BulletHit {pos, normal, material, part, ent, body, incidence (rad from the surface), v, E = ½mv²}`.

### 2.5 Ricochet vs penetration (`bal_surface`)

- **Incidence.** `θ = asin(|dir·n|)`, measured from the surface.
- **Ricochet.**
  - Condition: `θ < mat.ricochet_deg`.
  - Probability: `p = 1 − (θ/θr)²`, using the bullet's RNG.
  - Retained speed: `lerp(0.60, 0.30, θ/θr)`.
  - Exit angle: `θ·U(0.3, 0.8)` (bullets leave flatter), plus a tumble cone of `U(2°, 8°)`.
  - After the ricochet: `bc_mul = 0.35` and `BF_RICOCHET`. The bullet keeps flying and can still hit.
- **Penetration capacity.**
  - `D = mat.depth_ref_mm × cart.pen[mat.class] × (E / cart.E_ref)`.
  - Linear in energy. The class is `hard` or `soft`. `E_ref` is the cartridge's muzzle energy at its reference barrel.
- **Thickness.** `phys_raycast_body(entry_body)` is cast backwards from `entry + dir·D` to find the exit face.
  - Fallback: a reverse world ray, accepted if it hits the same body.
  - If `t < D`, the bullet exits with `E_exit = E·(1 − t/D)`. The cone widens by `mat.exit_spread_deg × (t/D)`, and an exit event spawns on the far face.
  - Otherwise the bullet stops and `E` is deposited.
- **Flesh.**
  - Thickness is the chord through the hit capsule. `hitbox_raycast` is extended to return both roots.
  - Several capsules in a row (arm → chest) are processed in order, up to 4 layers.
  - **Fragmentation.** If `cart.fragment_v > 0` and `v > fragment_v` (M855 ≈ 760 m/s), the bullet fragments: tissue `D × 0.6`, exit spray ×2, cavity ×2.5.
  - **Yaw.** The cartridge's `yaw_depth_mm` is the point where the bullet turns sideways (≈ 120 mm for 5.56, ≈ 160 mm for 7.62×51). Past that point it gets cavity ×1.5 and BC ×0.3 on exit.
- **Damage** (decision Q1). By default the current balance is kept:
  - `damage_resolve` is unchanged, with the weapon's `damage` × part × falloff.
  - A penetrated hit is multiplied by `E_in/E_ref`.
  - The tissue fraction deposited scales the damage (a full stop gives 1.0; a clean through-and-through gives `1 − E_exit/E_in`, floored at 0.5).

### 2.6 Supersonic events and audio timing

- **Flags.** `BF_SUPERSONIC_EVER` is set while `M > 1` at any point. `BF_SUBSONIC` is set from the launch if `M < 1` (the .45, or a suppressed subsonic load).
- **Crack** (per listener, computed by each client for its own camera; nothing goes on the network).
  - For each supersonic segment, find the wavefront arrival `t_a = min over τ of (τ + |L − p(τ)|/c)` along the segment. The minimum is where the Mach cone (`sin μ = c/v`) passes the listener.
  - Fire `crack` at `t_a` if the miss distance is less than `bal_crack_radius` (default 120 m) and the listener is not the shooter.
  - The crack's loudness follows the N-wave law: overpressure ∝ `r^(−3/4)`, duration ∝ `r^(1/4)`. This picks one of three sample lengths.
- **Whizz.** For subsonic bullets: closest approach under 8 m. A Doppler-pitched `whizz` is played at the closest-approach time plus `r/c`.
- **Report.** `t = fire_time + |muzzle − L|/c`.
  - The low-pass cutoff is `fc = 20 kHz · 10^(−d/900 m)`, floored at 1.5 kHz (an atmospheric-absorption approximation).
  - Past 150 m the sound crossfades to a `_distant` layer.
  - Volume is device dB × the inverse-distance law.
- **Impact sound.** `t = impact_time + |impact − L|/c`.
- **Resulting order for a distant listener off the line of fire:** crack → impact → report. For a shot 300 m away with the bullet passing 5 m off: crack at ≈ 0.42 s, report at 0.875 s.
  - The test checks the interval against the analytic value.
- **Audio additions:**
  - `audio_play_ex(name, pos, vol, cat, double at_time, float lpf_hz, float pitch)`.
  - Implementation: a scheduled queue (like thunder) plus one `ma_lpf_node` per voice, so 48 nodes. The cutoff is set at start.
  - Sounds are generated by `tools/sfxgen/ballistic_sfx.py`:
    - N-wave cracks: an analytic N-wave with ground reflection.
    - Whizz: band-passed noise with a Doppler sweep.
    - Distant report tails: the existing shots, low-passed and convolved with an exponential tail.
    - Suppressed reports: the existing shots −28 dB with the high band removed, plus a mechanical action layer.

### 2.7 Network and prediction (protocol 18 → 19)

- **`MSG_SHOTS`** (new, unreliable, batched once per host tick).
  - One 28-byte `ShotRecord` per shot (not per pellet): `shooter u8, shot_id u16, fire_tick u32, origin f32×3, dir oct16×2, seed u32, cart u8, flags u8` (device, tracer type, pellets).
  - Every client replays the shot deterministically against its own world. That gives tracers, flight, cracks and reports for **remote** shots, with no per-segment traffic.
  - This replaces `EV_TRACER` and the shot `EV_SOUND`, which cost 3 reliable ~250-byte `GameEvent`s per shot today.
- **`MSG_IMPACTS`** (new, unreliable, batched).
  - A 24-byte `ImpactRecord`: `shot_id, tick_delta u8, pos f32×3, normal oct16×2, material u8, energy u16 (log-encoded J), flags u8` (ricochet / penetrate / exit / fragment / headshot / part).
  - These are authoritative. Clients schedule them on the render timeline at `impact_tick`. They usually arrive before the render clock reaches that tick, because one-way latency is normally below the interpolation delay; when one arrives late it plays immediately.
- **Character hits** keep the reliable path, because damage numbers, hit confirms and kills must arrive: `EV_IMPACT` with the flesh pack, as today.
- **Shooter prediction.** On the first prediction of a shot, the client spawns a predicted bullet with the same seed, eye, aim and tick, so it has the same trajectory as the host. It simulates against the replica world and the current replica characters; no rewind is needed, because the host rewinds to exactly what the client saw.
  - Predicted visuals: tracer, impact, flesh predict, `chars_predict_hit`.
  - **Reconcile.** When `MSG_IMPACTS` arrives for one of our own shot_ids:
    - Within 0.25 m and the same flags: drop it.
    - Otherwise: play the authoritative impact and fade the predicted decal (decal handle kept for 2 s).
    - If the host reports no impact, nothing is undone: the particles were brief. The decal fades.
- **Tracer magazine pattern.** `is_tracer(i) = ((shot_in_mag + mag_seed) % every == 0) || shot_in_mag ≥ mag_size − last_n`.
  - `mag_seed` and `shots` are already in `PlayerSim`, so the value is replicated and checked by reconcile.
  - All-tracer magazines need the ammo type per magazine. `InvSlot` gets `u8 load` (the cartridge variant loaded at reload).

## 3. Muzzle flash, composed from physics (`bal_muzzle`)

### Inputs

- Weapon `barrel_length` (m).
- Cartridge: `powder_charge` (g), `gas_temperature` (K), `burn_length` (m: the barrel length at which the powder is fully burnt), `muzzle_pressure_ref` (MPa at the reference barrel), `bore` (m).
- Muzzle device block.
- `heat` (per-player cosmetic counter: +charge per shot, τ = 8 s).
- `near_surface`: two 0.5 m rays from the muzzle, forward and down. Client-side only.
- Scene EV.

### Derived values

| Quantity | Formula | Basis |
|---|---|---|
| Unburnt fraction u | `clamp(0.15 + 0.85·(1 − L/burn_length), 0.05, 0.9)` | Short barrels vent unburnt propellant (fuel for the secondary flash). |
| Muzzle pressure p_m | `p_ref·(L_ref/L)^1.2` | Isentropic expansion. Short barrels are louder and brighter. |
| Shock bottle / Mach disk distance | `0.67·bore·√(p_m/p_atm)`, **clamped to 2–6 cm** (the brief) | Ashkenas–Sherman. The formula gives about 9 cm for a full rifle. The clamp keeps the visible gap at the brief's scale. |
| Intermediate size | `0.6·x_M` disc. Colour = blackbody 2,500 K, with a +8 % blue mix when `mv > 800` | Hot gas behind the Mach disk. |
| Secondary radius | `R = 0.06·charge^(1/3)·(1 + 1.8u)` m, clamp 0.04–0.35 | Cube-root energy scaling (Hopkinson–Cranz). |
| Secondary distance | `0.10 + 0.12·charge^(1/3)·(1 + u)`, clamp 0.10–0.50 m | Re-ignition after the shock bottle mixes with air. |
| Secondary life | `1 + round(2u)` ticks (1–3) | |
| Smoke | Alpha `0.25 + 0.3u`, life 0.2–0.5 s, wind-advected. Haze emitter while `heat > 6·charge`. | |
| Sparks | Count `round(24·u·short)`, where `short = clamp(0.3/L, 0.3, 2)`. They cool in 10 cm (life = 0.1/speed). | Unburnt powder grains. |
| Blast | Distortion sprite for 2–3 ticks, radius ∝ `p_m^(1/3)`. If near a surface: `muzzle_dust`, scale ∝ `charge^(1/3)·device.blast`. | |
| Light | Intensity `I = 6·charge^(2/3)·(0.3 + u)·device.light`, life 1–2 ticks. Shadows only when `vfx_quality = 3` (Ultra/Modern). | |
| Exposure | `s(EV) = 2^(EV − EV_night)·g(EV)`, with `g = lerp(1, 0.15, smoothstep(5, 12, EV))` and `EV_night = 3` | Perceived = radiance × exposure. Day ≈ 0.15× night (test ≤ 0.2×), and it feeds bloom because it is HDR emissive. |

### Effects

Each component is its own small effect, spawned with its own `VfxSpawnParams` (scale, intensity, tint = blackbody RGB, offset along the barrel):

`muzzle_primary`, `muzzle_mach`, `muzzle_secondary` (16-frame petal atlas, `variant_no_repeat`), `muzzle_vent`, `muzzle_prongs`, `muzzle_smoke`, `muzzle_haze`, `muzzle_sparks`, `muzzle_blast`, `muzzle_dust`, `muzzle_vapor`, `muzzle_pop`.

This avoids touching the VFX core except for two things:

- `vfx_sys_set_intensity(h, f)`, used by tracers.
- A per-spawn `emissive_mul`.

At most 8 spawns per shot. The 24 old `muzzle_<type>_<device>.json` files are deleted. `tools/vfxgen/make_effects.py` emits the new set plus the petal atlas (16 procedural variants).

### Device mapping (`muzzle_device` block in each attachment JSON; multipliers applied to components)

| Component | bare | flash hider | comp / brake | suppressor |
|---|---|---|---|---|
| primary | 1 | 1 | 1 | 0 (inside the can) |
| intermediate (Mach) | 1 | 0.6 | 1 | 0 |
| secondary | 1.0 | **0.2** (0.1–0.3) | **0.7** | **0** |
| forward cone / prong spikes | 0 | 0.35 (`prongs: 3–4`) | 0 | 0 |
| side/up vents | 0 | 0 | **0.8** at `ports: [[±1,0.3,0],[0,1,0]]` | 0 |
| vapor wisp | 0 | 0 | 0 | 1 |
| smoke | 1 | 1 | 1.1 | 0.6 (from the can face) |
| sparks | 1 | 0.5 | 1 | 0 |
| blast / dust | 1 | 1 | **1.6** | 0.3 |
| light | 1 | 0.25 | 0.8 | 0.05 (pop: 0.6) |
| report dB | 0 | 0 | **+3** (+6 to the shooter's side) | **−28** (−20…−35) |
| crack | unchanged | unchanged | unchanged | unchanged (no crack only if the load is subsonic) |
| velocity_mul / POI | 1 / 0 | 1 / 0 | 1 / 0 | **1.015 / [0, −0.5] MOA** |
| first-round pop | – | – | – | flash 0.6 on the first shot after `cold_s` = 8 s idle |
| can heat haze | – | – | – | after 4 rounds; `can_heat` decays with τ = 20 s |

New attachments are `flash_hider.json` and `muzzle_brake.json`. The rifle and LMG base data say `device: hider`; that becomes `integral_muzzle: "flash_hider"`, a non-removable reference to the same block.

## 4. Tracers (`bal_tracer`, rendering in `bal_client`)

### Data

Per cartridge `tracer`:

- `{every: 4|5, last_n: 3, color: red|green|white|orange, ignition_m, burn_s, burn_m}`.
- Optional `variant` mass/BC overrides (M856 is heavier and has a lower BC).

All-tracer magazines come from the `ammo_*_tracer` item variants.

### States

| State | Range | Look |
|---|---|---|
| Igniter | 0 → `ignition_m` | Dim, 0.15× |
| Burning | until `burn_s` or `burn_m` | Full brightness |
| Burnout | 0.15 s fade | |
| Dark | after burnout | The projectile continues invisible. |

`bal_tracer_ignition 0` is the arcade mode.

### Rendering

- One CPU-tier VFX instance per visible tracer:
  - The head is a velocity-stretched additive billboard. Stretch = `|v|·shutter`, with `shutter = 1/fps`, or the `shutter_preview` override. That gives ≈ 15 m for 5.56 at 60 FPS.
  - The tail is a ribbon fed from the bullet's last 8 tick positions, so it curves with drop and drift.
  - A faint self-lit smoke trail (GPU, `distance` spawn mode).
  - The instance is moved each frame with `vfx_sys_set_transform` and dimmed with `vfx_sys_set_intensity`.
- **Light.** The 6 tracers nearest the camera get a `render_add_light` point light at night (sun < 0.2), with no shadow.
- **Ricochet.** A ricocheting tracer keeps burning. Its head flickers (seeded noise) and its tail sputters, using spawn-rate jitter.
- **Fire hook.** A tracer that hits or ends on `GRASS` with `wetness < 0.2` and no precipitation calls `fire_try_ignite(pos, energy)`. That is a stub (log plus debug marker) until foliage fire exists.
- **Non-tracer rounds:**
  - Optional `bullet_trace`: a distortion ribbon for 0.25 s, only when the local player is ADS with ≥ 3× magnification.
  - `vapor_trail`: only when supersonic and `humidity ≈ clamp(0.4 + 0.5·fog + 0.4·precip) > 0.8` or `T < 0 °C`.

## 5. Impacts (energy and angle driven)

`ImpactInfo {pos, normal, dir, material, E, v, flags, cart}` replaces the cosmetic `VfxCaliber`.

### Scaling

- Base effect `impact_<material>` (kept), with `scale = (E/1500 J)^(1/3)`, because crater volume ∝ E.
- `count_mul` per material:
  - sparks ∝ `hardness` (steel 1, concrete 0.4, wood/dirt 0)
  - dust ∝ `porosity`
- Decal size = `decal_ref × (E/1500)^(1/3) × mat.decal_scale`. This fixes the `VfxCaliberScale.decal` bug.

### New component effects

| Effect | When |
|---|---|
| `impact_ricochet` | Spark streak along the deflection plus a "zing" sound. |
| `impact_exit_spall` | Cone ×1.8 the size of the entry, on the far face. |
| `impact_plate_ring` | Steel plate with `E > 500 J`: ringing audio loop (0.6 s), lead splash disc decal, grey spatter. |
| `impact_pyro` | Tiny flash plus 1 tick of light when `hardness > 0.8 && v > 600`. Visible at night. |

### Flesh

- Entry mist (existing `flesh_entry_mist`).
- Exit spray along `dir`. Scale = `cavity = E_deposit^(1/3) × (fragment ? 2.5 : 1) × (yawed ? 1.5 : 1)`, ×1.4 for headshots.
- This passes through `flesh_pack_event` as an added energy byte, so `flesh_fx` keeps ownership of wounds, decals and gore levels.

## 6. Data tables

### 6.1 `assets/ballistics/cartridges.json`

`mv_curve` holds `[barrel_mm, m/s]` points. BC is lb/in² in the named model.

| id | Mass g | Ref MV (barrel) | BC | Charge g | Gas K | burn_length m | p_ref MPa | Tracer (every/col/ign/burn) | Fragment v / yaw mm | pen hard/soft |
|---|---|---|---|---|---|---|---|---|---|---|
| `9x19_fmj` | 8.0 | 360 (102 mm) | G1 0.15 | 0.35 | 2,600 | 0.13 | 40 | 5 / red / 0 / 1.0 s | – / 0 | 0.25 / 0.45 |
| `45acp_fmj` | 14.9 | 260 (127 mm) | G1 0.19 | 0.32 | 2,500 | 0.13 | 25 | 5 / red / 0 / 1.0 s | – / 0 | 0.22 / 0.40 |
| `556_m855` | 4.0 | 900 (368 mm) | G7 0.151 | 1.65 | 2,950 | 0.51 | 70 | 5 / red / 70 m / 800 m (M856) | 760 / 120 | **1.0 / 1.0** (reference) |
| `762x39_m43` | 7.9 | 720 (415 mm) | G7 0.138 | 1.6 | 2,850 | 0.42 | 60 | 4 / green / 50 m / 800 m | – / 180 | 1.2 / 1.1 |
| `762x51_m80` | 9.5 | 850 (559 mm) | G7 0.200 | 2.9 | 2,950 | 0.61 | 75 | 5 / red / 100 m / 900 m (M62) | 700* / 160 | **2.0** / 1.6 |
| `50bmg_m33` | 42.8 | 900 (1143 mm) | G7 0.330 | 15.0 | 3,000 | 1.14 | 80 | 5 / orange / 100 m / 1450 m (M17) | – / 250 | 6.0 / 3.0 |
| `12ga_00buck` | 9 × 3.5 | 400 (470 mm) | G1 0.046 (sphere) | 1.9 | 2,700 | 0.66 | 35 | – | – / 0 | 0.12 / 0.35 per pellet |

\* M80 fragmentation is velocity-dependent and weaker than M855's. `max_range` is 2,000 m everywhere.

Velocity per barrel length: the curve gives ≈ 0.3 m/s per mm for 5.56 near 370 mm.

Weapon mapping (new `barrel_length` field and the cartridge on the ammo item):

| Weapon | Barrel | Cartridge |
|---|---|---|
| pistol | 0.102 m | 9×19 (360 m/s) |
| smg | 0.225 m | 9×19 (≈ 400 m/s) |
| rifle | 0.368 m | 5.56 |
| lmg | 0.56 m | 7.62×51 |
| dmr | 0.508 m | 7.62×51 (≈ 830) |
| sniper | 0.61 m | 7.62×51 (≈ 865) |
| shotgun | 0.47 m | 12 ga 00 buck (keeps 8 pellets via the weapon override) |

`45acp`, `762x39` and `50bmg` are defined and tested but have no weapon yet.

### 6.2 Expected trajectories

Standard atmosphere, bore-line drop (no zero), 5 m/s full crosswind, rough solver:

| | 100 m: v / drop / drift | 300 m | 500 m | Subsonic from |
|---|---|---|---|---|
| 5.56 M855 @ 900 | 794 / 6.9 cm / 4.4 cm | 605 / 72 cm / 38 cm | 443 / 2.49 m / 1.20 m | > 600 m |
| 7.62×51 M80 @ 850 | 772 / 7.5 cm / 3.7 cm | 630 / 76 cm / 29 cm | 502 / 2.46 m / 0.90 m | ≈ 800 m |
| 7.62×39 @ 720 | 620 / 11 cm | 442 / 1.2 m | 319 / 4.4 m | ≈ 430 m |
| 9 mm @ 360 | 309 / 44 cm | 255 / 4.6 m | – | ≈ 20 m |
| .50 @ 900 | 851 / 6.5 cm | 756 / 62 cm | 669 / 1.87 m | > 1.5 km |

Pistol rounds visibly arc: 44 cm at 100 m, 4.6 m at 300 m.

Tests compare against `tests/data/ballistics_ref.json`. That file is generated by an independent Python RK4 solver with the full G1/G7 tables and is spot-checked against published M855/M80 tables. The tolerance is 5 %. Wind drift is checked against Didion's `D = W·(t − x/v0)`.

### 6.3 `assets/ballistics/materials.json`

Keyed by the existing `SurfaceMaterial` names. `depth_ref_mm` is the penetration of 5.56 M855 at muzzle energy (1,620 J).

| Material | class | ricochet ° | depth_ref mm | hardness | porosity | exit_spread ° | decal_scale | Basis |
|---|---|---|---|---|---|---|---|---|
| metal_thin | hard | 20 | 6 (mild steel) | 1.0 | 0.05 | 6 | 0.6 | Brief: 6 mm mild steel |
| metal_thick | hard | 20 | 6 | 1.0 | 0.05 | 6 | 0.6 | Same steel; bodies are thicker. |
| concrete | hard | 12 | 40 | 0.7 | 0.5 | 10 | 1.0 | Spall-dominated; repeated hits are needed. |
| brick | hard | 12 | 60 | 0.6 | 0.6 | 10 | 1.0 | |
| tile | hard | 15 | 30 | 0.8 | 0.2 | 8 | 0.9 | |
| asphalt | hard | 10 | 60 | 0.5 | 0.4 | 8 | 1.0 | |
| glass | hard | 25 | 80 | 0.8 | 0 | 4 | 0.8 | |
| wood (pine) | soft | 8 | 120 | 0.3 | 0.3 | 5 | 1.0 | Brief: ~12 cm pine |
| plaster | soft | 6 | 400 | 0.2 | 0.9 | 4 | 1.2 | Drywall is about 13 mm per sheet. |
| sand (sandbag) | soft | 5 | 200 | 0.1 | 1.0 | 6 | 1.2 | Brief: ~20 cm sandbag |
| dirt / grass | soft | 6 | 300 | 0.1 | 1.0 | 6 | 1.2 | Grazing skips off the ground. |
| gravel | soft | 10 | 250 | 0.3 | 0.8 | 8 | 1.1 | |
| water | soft | 7 | 900 | 0 | 0 | 15 | – | Water skip; rifle rounds break up within about 1 m. |
| flesh | soft | – | 450 | 0 | 0 | 8 | – | Chord from the capsule; a torso is about 25–30 cm. |
| cloth | soft | – | 1000 | 0 | 0.2 | 2 | – | |

Resulting behaviour:

- 7.62×51 is about 2× 5.56 on hard targets (12 mm steel).
- A 9 mm stops in about 200 mm of tissue, so it often stays in a torso but goes through limbs.
- 5.56 under 760 m/s exits a torso with reduced energy; above that it fragments.
- Thickness also comes from level brushes, e.g. a 6 mm `metal_thick` plate on the range.

### 6.4 Weapon and attachment additions

- Weapon JSON: `barrel_length`, `zero_distance`, `integral_muzzle`.
- `range` becomes the termination/HUD distance only.
- `falloff` stays for damage (Q1).

## 7. Per-shot cost budget (host)

The target is ≤ 2 µs per projectile step, and 500 in flight per tick ≤ 1 ms.

| Part | Budget | How |
|---|---|---|
| Integrate (RK2 + drag lerp + gravity, 1–2 sub-steps) | 0.08 µs | ~60 flops, table lerp, no libm |
| Wind sample (once per tick) | 0.15 µs | `wind_at` without sources; cache per 8 m cell per tick if over budget |
| World segment ray (Jolt CastRay, ≤ 15 m) | **1.2 µs** | Measured in the bench. The current ray also builds and destroys an ignore filter per call; pool it. |
| Character broadphase | 0.25 µs | **New uniform grid** (4 m cells, rebuilt once per tick with the rewind slack). Today's `chars_raycast` scans 2048 slots per ray (≈ 5–10 µs with a horde). This also speeds up existing hitscan and melee. |
| Tires / legacy hitboxes / bookkeeping | 0.2 µs | |
| **Step total** | **≈ 1.9 µs** | |
| Hit resolution (rare, ~1 per bullet) | 3–6 µs | Thickness ray plus apply_hit. Not per step. |
| Per shot spawn | ~1 µs host. Client: muzzle composer ≈ 3 µs + ≤ 8 VFX spawns | |

- **Bandwidth:** 28 B per shot (`MSG_SHOTS`) plus 24 B per impact, versus about 750 B of reliable events per shot today.
- **Memory:** 1,024-entry pool, 80 KB. Clients replay remote shots with the same pool, at 0 network cost per step.
- **Guardrails:**
  - `ballistics_bench <n>` spawns n bullets into the current map and logs p50/p99 per step and per tick.
  - If p99 goes over budget, bullets more than 400 m from every player and outside the character grid ray-test every second tick (a 30 m segment). Hits stay exact, because the segment is still swept.
  - The world-ray phase can be parallel-for across the job system; Jolt queries are read-only.

## 8. Debug and dev room

- `ballistics_debug 1`. Per bullet:
  - The path polyline coloured by Mach (red supersonic, orange transonic, blue subsonic).
  - Drop and drift ticks every 100 m.
  - Ricochet points (yellow) and penetration entry/exit (cyan / magenta).
  - The supersonic crack radius tube.
  - Text with crack, impact and report times for the listener.
- **Ballistics range.** A new lane parallel to the sighting lane at z = −40, firing point (32, 0, −40), facing +X:
  - Steel `metal_thick` gongs at 25 / 100 / 300 / 500 m, sized 0.3 / 0.5 / 1 / 1.5 m.
  - A 6 mm plate row at 30 m angled 5°, 10°, 15°, 20° and 30° (ricochet).
  - Penetration stacks at 15 m: steel 3/6/12 mm; pine 10/12/15 cm; sandbag 20/30 cm; brick; 2 plaster sheets. Each has a witness plaster panel behind it.
  - Labels on the floor.
- **Commands / cvars:**
  - `bal_range` teleports to the range.
  - `bal_wind <m/s>` sets a crosswind; 0 turns it off. It goes through the weather override.
  - Day/night uses the existing time commands.
  - `fire_tracers_only 1`, `shutter_preview <s>`, `muzzle_preview <weapon> <device>`. The last one spawns at 1.5 m in front of the camera, cycles variants and sets `host_timescale 0.05`.
  - All get the category `weapons`. The new prefixes `bal_` / `ballistics_` are added to `command_categories.json`.

## 9. Tests

**`test_ballistics`** (pure; ctest):

- Drop and velocity at 100 / 300 / 500 m within 5 % of the reference for 5.56 and 7.62×51, for both drag models (exp only within 10 %).
- Wind drift vs Didion.
- Ricochet decision table, swept over material × angle × seed.
- Penetration depth table, over material × cartridge × velocity.
- Crack/report interval: geometric cases with analytic answers, including crack → impact → report ordering.
- Tracer index per magazine: deterministic across seeds, `last_n`, all-tracer magazines.
- Device multipliers: each device composes the expected component scales.
- Daylight flash perceived ≤ 0.2× night.
- First-round pop: fires exactly once per cold can, again after `cold_s`.
- Determinism: the same seed gives a bit-identical trajectory over 1,000 steps.

**In-engine / script tests:**

- `tools/tests/bal_range.cfg`: headless range render with `--validation` and zero validation errors, plus screenshots (day and night, tracers, ricochet, penetration witness).
- `ballistics_bench 500` in the devroom and in zombieland: the 1 ms/tick budget.
- `tools/tests/bal_net.sh`: host plus client. Tracer index and impact positions are compared through the log, and the predicted vs authoritative impact delta is under 0.25 m.

## 10. Phases (each one builds, is tested and is committed separately)

| Phase | Content | Verify |
|---|---|---|
| P0 | Data files, `bal_drag/sim/surface/acoustics/muzzle/tracer` (pure), `tools/ballistics_ref.py`, `test_ballistics`, `docs/BALLISTICS.md` skeleton | ctest (also in this Linux container) |
| P1 | `bal_world`: pool, launch/zero, swept rays, character grid + chord exits, `phys_raycast_body`, ricochet/penetration wired into `apply_hit`. Hitscan weapons switch over; `kind: hitscan` stays as an escape hatch (`bal_enable 0`). | Range scripts, bench |
| P2 | Protocol 19: `MSG_SHOTS` / `MSG_IMPACTS`, `InvSlot.load`, prediction + replay + reconcile. `EV_TRACER` and the shot `EV_SOUND` removed. | `bal_net.sh` |
| P3 | `audio_play_ex` (schedule + LPF), crack/whizz/report/impact timing, generated sfx | Unit timing test + listen pass |
| P4 | Muzzle composer + component effects + petal atlas, device blocks, `flash_hider` / `muzzle_brake` attachments, exposure curve, old muzzle files removed | Screenshots per device, day/night, `muzzle_preview` |
| P5 | Tracers: pattern, states, head/ribbon/smoke, lights, ricochet flicker, bullet trace, vapor, fire stub | Night range screenshots |
| P6 | Impact energy scaling, spall/ring/pyro, flesh cavity, decal bug fix | Range screenshots |
| P7 | Range level, debug draw, commands, lint, full validation run, docs final | Everything in §9 |

## 11. Decisions needed

1. **Damage model.** Recommended: keep the current per-weapon `damage` and `falloff` balance. Ballistics decides *whether* and *what* is hit (drop, ricochet, penetration), and scales damage only by the energy left after penetration and by the tissue deposit fraction. The alternative is fully energy-derived damage, which needs a balance pass on all 8 guns.
2. **Projectile origin.** Recommended: the eye, with a solved zero angle and a visual blend from the muzzle. The alternative is the true muzzle socket, which is more physical but makes the third-person barrel and first-person sight disagree near walls.
3. **Sniper zero.** 300 m fixed for now, with adjustable zeroing (hold-over UI) later. Or add `zero_up/down` now.
4. **Crossbow.** Move it onto the bullet pool (one system, gets drag, wind and lag compensation) in P1, or leave it on entities.
5. **Ammo types.** Add `ammo_rifle_tracer` / `ammo_762_tracer` all-tracer items and loot entries now, or only the per-magazine every-Nth pattern.
6. **Verification.** The pure modules build and test in this container (gcc/cmake). The engine run, validation layers and screenshots need your Windows box (`.toolchain/` is absent here). P1–P7 can be committed with the pure tests green and script tests ready to run, but GPU and engine verification would be yours, unless the environment can build the game.
