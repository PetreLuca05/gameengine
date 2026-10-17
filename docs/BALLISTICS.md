# Ballistics, muzzle flash, tracers

Every gun with a cartridge fires simulated bullets. Bullets drop and slow down with drag, drift
in the wind, wobble through the transonic band, ricochet or penetrate depending on the surface,
and crack past listeners. The muzzle flash is assembled from physical components (cartridge,
barrel, muzzle device). Tracers are ammunition, not a visual on every shot.

Realism comes from the data (cartridges, materials, devices), not from per-weapon tuning.
`bal_enable 0` switches back to instant hitscan.

The original planning notes, with the survey of the old code, are in `BALLISTICS_PLAN.md`.

## Code

| File | What |
|---|---|
| `src/gameplay/ballistics/bal_data.c` | Parses `assets/ballistics/cartridges.json`, `materials.json` and the attachments' `muzzle_device` blocks. Pure. |
| `bal_drag.c` | G1 / G7 / GS (sphere) Cd-vs-Mach tables (79 / 84 / 81 points) and the point-mass retardation. Pure. |
| `bal_sim.c` | `BalBullet` step: RK2 sub-steps, drag on air-relative velocity, transonic yaw, spin drift, zero solver. Pure. |
| `bal_surface.c` | Ricochet / penetration / flesh decisions. Pure, deterministic (bullet RNG). |
| `bal_acoustics.c` | Crack (Mach cone) arrival, whizz, report delay, air-absorption low-pass, N-wave level. Pure. |
| `bal_tracer.c` | Tracer rounds per magazine, igniter / burn / burnout states, streak length. Pure. |
| `bal_muzzle.c` | The flash composer, exposure gain, blackbody colours, heat / suppressor pop. Pure. |
| `bal_world.c` | Bullet pool, launch, per-tick swept hits with lag compensation, events, replays, crack / report scheduling, console. |
| `bal_fx.c` | Spawns the composed flash, draws tracers (head + curved tail + smoke + night light), bullet trace / vapour trails, `muzzle_preview`, the fire hook. |
| `tests/test_ballistics.c` | Unit tests (ctest `ballistics`). |
| `tools/ballistics_ref.py` | Reference trajectories (py-ballisticcalc) → `tests/data/ballistics_ref.json`. |
| `tools/sfxgen_ballistics.py` | Crack / whizz / ricochet / plate ring / distant-report sounds. |
| `tools/vfxgen/make_ballistic_effects.py` | Component muzzle effects, tracer, ballistic impact effects. |
| `tools/tests/bal_range.cfg` | Range test with screenshots. |

## Physics model

**Integration**
- Fixed 60 Hz tick; sub-steps of at most 20 m (`n = ceil(v·dt / 20)`, max 4), RK2 midpoint.
- Termination: `v < 60 m/s`, 6 s of flight, the cartridge's `max_range`, or leaving the streamed world.

**Drag**
- Retardation: `a = ρ · Cd_G(M) · v² · π / (8 · BC)`, with `BC` in kg/m² (lb/in² × 703.07).
  - Derivation: drag force `½ρv²·Cd·A` on the standard projectile with sectional density BC.
- `v` and `M` use the air-relative velocity (bullet minus wind). The wind drift follows from this; the code adds no separate wind term.
- `bal_drag_model`: −1 auto (tables), 0 exponential approximation, 1 tables.
  - The exponential model is `a = k·v²`, with k fitted per cartridge so it matches the table velocity at 300 m (100 m for subsonic rounds). It has no transonic rise.
  - Host-owned and replicated: client Fidelity never changes the authoritative path.

**Atmosphere**
- Speed of sound: `c = 331.3 + 0.606·T`.
- Density: `ρ = p / (287.05·(T + 273.15))`, with `p = 101325·e^(−h/8434)`. `T` is the weather's temperature.

**Wind**
- `wind_at` on a ballistics-owned `WindField`, built from the replicated weather (`WP_WIND_DIR / WP_WIND / WP_GUST`) at `tick·SIM_DT`.
- No local sources, so it is identical on host and clients. Sampled once per bullet per tick.
- `bal_wind <m/s>` / `bal_wind_dir` set a steady override for the range.

**Transonic instability**
- While `0.8 < M < 1.2`: yaw grows at the cartridge's `transonic_k` (rad/s).
- Drag is multiplied by `1 + 2·yaw²`.
- The bullet random-walks sideways by `yaw · v · 0.1 · dt` per axis, using the seeded RNG.
- Dynamic stability damps the yaw once subsonic.
- Visible on 9 mm (subsonic from about 20 m) and on 7.62×39 past about 430 m.

**Spin drift**
- Litz: `drift = 1.25·(Sg + 1.2)·t^1.83` inches, applied as its time derivative to the right of the path after 0.3 s.
- `Sg` is the Miller stability from mass, calibre, length, twist and velocity.
- A few cm at 500 m. Off with `bal_spin_drift 0`.

**Launch**
- From the eye, along the aim plus the spread cone (`recoil_cone_dir`, unchanged).
- Pitched up by the zero elevation, solved by bisection on the same sim (standard atmosphere, no wind) for `zero_distance`:
  - pistol 25 m, SMG 50 m, rifle / LMG 100 m, DMR 200 m, sniper 300 m.
- Plus the device's point-of-impact shift (`poi_moa`).
- Speed: `mv_curve(barrel_length) × device.velocity_mul`.
- The visible tracer and smoke start at the muzzle socket and blend onto the true path over the first 15 m.

**Determinism**
- float32 in a fixed operation order. `-ffp-contract=off` / `/fp:precise` on `src/gameplay/ballistics` (CMake).
- Seeds: the shot seed (`shot_seed`: command sequence, slot, shot counter) and the pellet index.
- Host and predicting client run the same binary on the same inputs, so their paths are bit-identical until the worlds differ.

### Reference check

From `test_ballistics`: bore line, ICAO atmosphere, 5 m/s full-value crosswind, the game's 60 Hz tick. The reference is py-ballisticcalc.

| | 100 m: v / drop / drift | 300 m | 500 m |
|---|---|---|---|
| 5.56 M855 @ 900 | 796 / 6.6 cm / 3.5 cm (ref 796 / 6.6 / 3.5) | 610 / 71 cm / 36 cm (ref 610 / 71 / 36) | 448 / 2.45 m / 1.16 m (ref 448 / 2.45 / 1.16) |
| 7.62×51 M80 @ 850 | 773 / 7.3 cm / 2.9 cm (ref 773 / 7.2 / 2.8) | 632 / 75 cm / 28 cm (ref 632 / 75 / 28) | 506 / 2.42 m / 0.87 m (ref 506 / 2.42 / 0.87) |

- Agreement is within 0.3 % (the test allows 5 %).
- Drift equals Didion's `D = W·(t − x/v₀)`.
- The exponential model is within 10 % to 300 m.

## Surfaces (`bal_surface.c`)

**Incidence**
- `θ = asin(|dir·n|)`, measured from the surface.

**Ricochet**
- Possible only when `θ < ricochet_deg`.
- Probability: `1 − (θ/θr)²`.
- Speed kept: `lerp(60 %, 30 %, θ/θr)`.
- Leaves flatter (`θ·U(0.3, 0.8)`), plus a 2–8° tumble cone.
- BC × 0.35 and `BF_RICOCHET`. The bullet keeps flying and can hit again.

**Penetration**
- Capacity: `D = depth_ref · pen[class] · E / E_ref`.
  - `E_ref` is the cartridge's muzzle energy at its reference barrel.
  - `depth_ref` is what 5.56 M855 defeats at its muzzle.
  - `pen` is what this cartridge defeats at its own muzzle, relative to that.
- Thickness: measured by casting the ray back from `entry + dir·D` against the same body. A ray starting inside a solid means "thicker than D". Fallback: the material's `thickness_mm`.
- Exit: `E_out = E·(1 − t/D)`, cone `exit_spread_deg·(t/D)`, BC × `lerp(0.6, 1, E_out/E)`, plus an exit-side event.
- At most 4 ricochets + penetrations per bullet.

**Flesh**
- Thickness is the chord through the hit capsule (`chars_raycast_one`).
- Several capsules in a row are handled in order (arm, then chest).
- Fragmentation (`v > fragment_v`): the core exits with half the energy, the cavity is ×2.5.
- Yaw (tissue past `yaw_depth`): cavity ×1.5, BC × 0.3 on exit.
- Temporary cavity = `(E_deposit / 1 kJ)^(1/3) × multipliers`. It scales the flesh effects; headshots ×1.4.

**Damage keeps the weapon balance**
- `damage × part_mult × falloff`, unchanged.
- Scaled by the energy left after earlier penetrations, and by `lerp(0.75, 1, deposit fraction)`.
- A ricochet that still hits does a quarter.

## Data

### `assets/ballistics/cartridges.json`

| Field | Unit | Basis |
|---|---|---|
| `mass_g`, `diameter_mm`, `length_mm`, `twist_mm` | g, mm | Published specs (M855 62 gr, M80 147 gr, M43 123 gr, M33 660 gr, 124 gr 9 mm, 230 gr .45). Twist and length feed the Miller Sg. |
| `drag` / `bc` | G1, G7 or GS; lb/in² | Published BCs: M855 G7 0.151, M80 G7 0.200, M43 G7 0.138, M33 G7 0.33, 9 mm G1 0.15, .45 G1 0.19; 00 buck = sphere SD 0.071 on GS. |
| `mv_curve` | [mm, m/s] | Velocity by barrel length (manufacturer and chronograph tables; about 0.3 m/s per mm for 5.56 near 370 mm). `ref_barrel_mm` is the reference. |
| `powder_g` | g | Typical charges: 9 mm 0.35, 5.56 1.65, 7.62×51 2.9, .50 15. |
| `gas_k` | K | Propellant flame temperature (about 2,500–3,000 K). |
| `burn_length_m` | m | Barrel length where the charge has fully burnt. Shorter barrels vent unburnt powder, which fuels the secondary flash. |
| `muzzle_pressure_mpa` | MPa | Muzzle pressure at the reference barrel (rifles 60–80, pistols 25–40). |
| `pen.hard` / `pen.soft` | × M855 | Hard targets (plate, masonry): 7.62×51 ≈ 2×, pistols ≈ 0.25×. Soft media (wood, earth, tissue) by sectional density: 9 mm ≈ 0.45×. |
| `fragment_v` | m/s | M855 yaw-and-break threshold about 760 m/s (2,500 fps); M80 weaker (820). 0 = never. |
| `yaw_depth_mm` | mm | Gelatin: 5.56 turns at about 12 cm, 7.62×51 at about 16 cm. |
| `transonic_k` | rad/s | Yaw growth through Mach 1.2 → 0.8. |
| `tracer` | | `every` (5 = 1 in 5 on a belt / magazine), `last_n` (the last rounds of a magazine), `color` (red = strontium, green = barium, white = magnesium, orange), `ignition_m` (dim igniter), `burn_m` / `burn_s` (M856: 800 m, M62: 900 m, M17: 1,450 m), tracer `mass_g` / `bc`. |

### `assets/ballistics/materials.json` (keys = SurfaceMaterial names)

| Material | Class | Ricochet ° | depth_ref | Hardness | Porosity | Basis |
|---|---|---|---|---|---|---|
| metal_thin / metal_thick | hard | 20 | 6 mm | 1.0 | 0.05 | 5.56 M855 ≈ 6 mm mild steel at the muzzle; steel skips below about 20°. |
| concrete / brick / tile / asphalt | hard | 10–15 | 40 / 60 / 30 / 60 mm | 0.5–0.8 | 0.2–0.6 | Masonry spalls; repeated hits are needed. |
| glass | hard | 25 | 80 mm | 0.8 | 0 | |
| wood (pine) | soft | 8 | 120 mm | 0.3 | 0.3 | About 12 cm of pine. |
| plaster (drywall) | soft | 6 | 400 mm | 0.2 | 0.9 | A 13 mm sheet barely slows a rifle round. |
| sand (sandbag) / dirt / grass / gravel | soft | 5–10 | 200–300 mm | 0.1–0.3 | 0.8–1 | About 20 cm of sandbag; bullets skip off flat ground at grazing angles. |
| water | soft | 7 | 900 mm | 0 | 0 | Water skips; rifle rounds break up within about 1 m. |
| flesh | soft | 0 | 450 mm | 0 | 0 | Torso chord 25–30 cm: rifles exit, a 9 mm often stays. |
| cloth | soft | 0 | 1 m | 0 | 0.2 | |

`hardness` drives impact sparks and pyrophoric flashes. `porosity` drives dust. `decal_scale`
scales the hole. `thickness_mm` is the fallback thickness.

### Weapons (`data/weapons/*.json`)

- `cartridge`, `barrel_length` (m), `zero_distance` (m), and optionally `integral_muzzle` (an attachment id: the rifle and LMG carry `flash_hider` through their `vfx.muzzle.device: hider`).

### Muzzle devices (attachment JSON, `muzzle_device` block)

```json
"muzzle_device": { "kind": "hider|comp|suppressor|bare",
  "flash": { "primary": 1, "mach": 1, "secondary": 1, "prongs": 0, "vents": 0, "vapor": 0, "smoke": 1, "sparks": 1, "blast": 1, "light": 1 },
  "prongs": 4, "ports": [[1, 0.3, 0], [-1, 0.3, 0], [0, 1, 0]],
  "report_db": 0, "report_db_side": 0, "velocity_mul": 1, "poi_moa": [0, 0],
  "pop_cold_s": 0, "pop_flash": 0.6, "can_heat_rounds": 4, "can_heat_tau": 20 }
```

- Ports are given in muzzle space (x right, y up, −z forward).
- A plain string `muzzle_device` (old style) only picks the VFX device.

| Component | bare | flash_hider | compensator | muzzle_brake | suppressor |
|---|---|---|---|---|---|
| primary | 1 | 1 | 1 | 1 | 0 |
| intermediate (Mach disk) | 1 | 0.6 | 1 | 1 | 0 |
| secondary | 1 | **0.2** | **0.7** | **0.7** | **0** |
| prong spikes | – | 0.35 (4 prongs) | – | – | – |
| side / up vents | – | – | **0.8** (2 sides + top) | **0.8** (4 side ports) | – |
| vapour wisp | – | – | – | – | 1 |
| smoke | 1 | 1 | 1.1 | 1.2 | 0.6 |
| powder sparks | 1 | 0.5 | 1 | 1 | 0 |
| blast / dust | 1 | 1 | **1.6** | **2.0** | 0.3 |
| light | 1 | 0.25 | 0.8 | 0.85 | 0.05 |
| report | 0 dB | 0 dB | +3 dB (+6 beside) | +4 dB (+8 beside) | **−28 dB**, tone low-passed |
| velocity / POI | 1 | 1 | 1 | 1 | ×1.015, −0.5 MOA |
| first-round pop | – | – | – | – | 0.6 flash after 8 s idle |
| can heat haze | – | – | – | – | after 4 rounds, τ 20 s |

The supersonic crack is never changed by a device; only subsonic ammunition removes it.

## Muzzle flash (`bal_muzzle.c`)

Inputs: the cartridge, `barrel_length` (L), the device, the heat of recent fire, near-surface
(two 0.5 m rays from the muzzle), and the scene exposure.

| Quantity | Formula | Basis |
|---|---|---|
| Unburnt fraction u | `clamp(0.15 + 0.85·(1 − L/burn_length), 0.05, 0.9)` | Propellant still burning at the crown. |
| Muzzle pressure | `p_ref·(L_ref/L)^1.2` | Isentropic expansion over the extra bore volume. |
| Intermediate flash distance | `0.67·d·√(p/p_atm)`, clamped 2–6 cm | Ashkenas–Sherman Mach-disk distance. A full rifle gives about 9 cm; the clamp keeps the visible gap at game scale. |
| Intermediate colour | blackbody 2,500 K, +8 % blue above 800 m/s | |
| Secondary radius | `0.06·charge^(1/3)·(1 + 1.8u)`, 4–35 cm | Cube-root energy scaling (Hopkinson–Cranz). |
| Secondary distance | `0.10 + 0.12·charge^(1/3)·(1 + u)`, 10–50 cm | Re-ignition where the jet mixes with air. |
| Secondary life | 1 + round(2u) ticks | Effects `muzzle_secondary_{1,2,3}`, 16 petal variants, never the same twice in a row. |
| Primary flash | 1.2 + 1.2·charge^(1/3) cm, 1,800 K, < 1 tick | |
| Smoke | alpha 0.25 + 0.3u, 0.2–0.5 s, wind-driven | |
| Powder sparks | `24·u·clamp(0.3/L, 0.3, 2)` grains, cool in about 10 cm | |
| Blast | heat-haze ring `0.12·(p/10)^(1/3)` m, 2–3 ticks; dust when near a surface | |
| Light | `6·charge^(2/3)·(0.3 + u)`, 1–2 ticks, radius `3 + 2·charge^(1/3)`; shadows at `vfx_quality 3` | |

**Exposure**
- Hot emitters (flash, tracer, sparks) are exposure aware: the VFX layer multiplies them by `bal_flash_exposure_gain(EV, EV_night, daylight)`.
  - `gain = 2^(EV − EV_night) · lerp(1, 0.15, daylight)`.
  - The first factor cancels the camera's adaptation relative to night. The engine's auto-exposure range is compressed: noon is about EV 1 against a −1.5 night floor, so the EV alone is not enough.
  - The second factor is the eye's adaptation. `daynight_daylight()` is the sun's share of full noon light, 0 at night and 1 in full sun.
  - `EV_night` is the night floor in `assets/world/daynight.json`.
- After the tonemapper's `2^−EV`, perceived brightness is the authored night look at night and about 0.15× of it in full daylight. The flash also feeds bloom.
- The flash light is physical and gets no gain.

**Heat**
- Per shooter: `heat += powder_g` per shot, τ = 8 s. Lingering haze appears above 6 charges.
- Suppressor: a pop when the can has been idle for `pop_cold_s`; a can shimmer after `can_heat_rounds`.

`muzzle_preview <weapon> [device] [timescale] | off` spawns a flash every 0.8 s in front of the
camera, seen from the side, in slow motion.

## Tracers

**Which rounds**
- `bal_tracer_round`: every `every`-th round of the magazine, starting at a phase taken from the magazine seed (`RecoilState.mag_seed`), plus the last `last_n` rounds.
- The magazine seed and the shot counter are in the replicated `PlayerSim`, so every machine agrees. The host's `EV_SHOT` carries the flag.
- `fire_tracers_only 1` makes every round a tracer (range / debug).

**Look over the flight**
- Dim igniter up to `ignition_m`.
- Full burn until `burn_s` or `burn_m`, whichever comes first.
- 0.15 s burnout, then dark while the bullet flies on.
- `bal_tracer_ignition 0` is the arcade mode: bright from the muzzle.

**Drawing**
- Segment sprites with the `tracer_head` effect's emitters (`vfx_game_push_sprite`).
- Head: velocity-stretched, length = speed × shutter (`shutter_preview`, default the frame time, about 15 m for 5.56 at 60 FPS), soft head.
- Tail: the last 8 tick positions, so it curves with drop and wind, fading.
- Width at least about 1.5 px.
- A faint smoke trail while burning (`tracer_smoke`).
- At night the 6 nearest tracers are small point lights.
- A ricocheting tracer keeps burning, flickers and sputters.
- Tracers that stop in dry grass call `fire_try_ignite`, a stub until foliage burns.

**Non-tracer rounds**
- Invisible, except the bullet trace (`bal_bullet_trace`: a heat / shock ripple behind supersonic bullets) through ≥ 3× optics.
- Vapour trails (`bal_vapor_trails`) in humid (fog / rain) or freezing air.

## Sound

**Crack**
- Each machine evaluates its own listener against every bullet it simulates. Nothing extra goes over the network.
- The shock reaches the listener at `min over τ of τ + |L − p(τ)|/c`, where the line to the listener meets the track at the Mach angle.
- Miss distance up to `bal_crack_radius` (120 m). Level ∝ r^−3/4 (N-wave).
- The `bullet_crack_{0,1,2}` sample length grows ∝ r^1/4.
- Subsonic bullets `bullet_whizz` within 8 m, at the closest approach plus `r/c`.

**Report**
- Delayed by `d / c` (plus the shot's offset on the render clock on clients).
- Low-pass `20 kHz · 10^(−d/900 m)`, floored at 1.5 kHz.
- Above 150 m: the `shot_<gun>_distant` layer.
- Device dB; suppressor tone low-passed at 3.5 kHz.
- The shooter hears its own report immediately.

**Impacts**
- Every impact sound is delayed by its travel time and low-passed.
- Steel plates also ring (`impact_plate_ring`); ricochets "zing" (`bullet_ricochet`).

**Resulting order**
- For a distant listener off the line of fire: crack → impact → report. For example, 300 m away and 5 m off the line: 0.42 s, 0.50 s, 0.87 s.

**`audio_play_fx`**
- Scheduled one-shots (delay), a per-voice `ma_lpf_node`, pitch, and per-sound distance law.
- `snd_trace 1` logs every scheduled sound; this works with `--nosound`, for tests.

## Network (protocol 19)

**Host**
- The host's bullets are authoritative.
- Per tick, after `chars_host_tick`, every bullet steps and sweeps its segment against:
  - the world (Jolt),
  - tires,
  - legacy hitboxes,
  - characters through a uniform 8 m grid, rebuilt per tick only while bullets fly.
- Characters are rewound to `view_tick + (tick − fire_tick)`. The rewind stays as old as the shooter's latency for the whole flight, clamped to the 32-tick history.

**Events**
- `EV_SHOT`, once per shot rather than per pellet, carries a `BalShotRecord` (seed, tick, weapon + attachments, cartridge, device, spread, speed, zero, POI, tracer flag).
  - It replaces the per-pellet `EV_TRACER` and the shot `EV_SOUND`.
  - Observers flash at the shooter's visible barrel, eject the casing, play the report, and replay the bullets on their render clock for tracers and cracks.
- `EV_IMPACT` (reliable) carries the hits. Id bits 16–31 hold the energy (log scale) and the outcome (ricochet / penetrate / exit / fragment / yaw / tracer).

**Shooter**
- The shooter predicts its own bullets with the same seed, spawning impact effects when they land. The host's copy of a predicted impact is skipped, as before (`cl_predict_impacts`).

**Damage**
- Damage, hit confirmations and kills are unchanged.

## Budget

- Pure step (integration, drag, transonic, spin drift): about 0.15 µs (`test_ballistics` prints it).
- Per tick and bullet in game: one Jolt ray over the step (≤ 15 m at 900 m/s), a hashed-grid character query, tires, legacy hitboxes (skipped when none exist).
- Hit resolution adds a thickness ray.
- `ballistics_bench <n> [cartridge]` launches n bullets from the player; `bal_info` prints the last tick's steps, rays and ms (worst too).
- Target: ≤ 2 µs per step, 500 bullets ≤ 1 ms per tick on the reference machine.

## Debug and dev room

**`ballistics_debug`**
- 1 draws the paths: red supersonic, orange transonic, blue subsonic.
- Ricochet points yellow, penetration entry cyan / exit magenta, the fire hook orange.
- 2 also logs every hit (outcome, energy in / out, incidence, capacity, thickness) and every crack / whizz / report with its timing.

**Ballistics range** (dev room, east, `bal_range`)
- Firing point (32, 0, −40), facing +X.
- Steel gongs at 25 / 100 / 300 / 500 m (0.3 / 0.5 / 1 / 1.5 m).
- 6 mm ricochet plates at 30 m, turned to 5 / 10 / 15 / 20 / 30° incidence from the firing point.
- `bal_range pen` → (32, 0, −56): penetration stacks at 15 m, each with a plaster witness panel 0.6 m behind:
  - steel 3 / 6 / 12 mm,
  - pine 10 / 12 / 15 cm,
  - sandbag 20 / 30 cm,
  - brick 11 cm,
  - 2 plaster sheets.
- Controls:
  - Wind: `bal_wind <m/s>`, `bal_wind_dir`.
  - Day / night: `time_set`.
  - Slow motion: `host_timescale` (simulation), `vfx_timescale` (effects).
  - `fire_tracers_only`, `shutter_preview`, `muzzle_preview`.

## How to add a cartridge

1. Add an entry to `assets/ballistics/cartridges.json`: mass, diameter, length, twist, drag table + BC, a velocity / barrel curve, charge, gas temperature, burn length, muzzle pressure, penetration factors, fragmentation / yaw, tracer loading.
2. Point a weapon at it: `"cartridge": "<id>"`, `"barrel_length"`, `"zero_distance"`.
3. Add a reference case to `tools/ballistics_ref.py` (`CASES`), run it, and rebuild. `test_ballistics` then checks the new trajectory.
4. Files hot-reload (`bal_watch`, or `bal_reload`); on a live server every client must have the same data (the hash is logged).

## How to add a muzzle device

1. Write an attachment JSON with `"slot": "muzzle"`, a `muzzle_length`, the `modifiers` (recoil etc.) and a `muzzle_device` block (table above).
2. Allow it in the weapons' `attachment_slots.muzzle.allow` and bake its icon (`tools/bake_icons/bake_icons.sh`).
3. Check it with `muzzle_preview <weapon> <device>`, day and night.

## Tests

**`test_ballistics` (ctest `ballistics`)**
- Drop, velocity, time of flight and drift at 100 / 300 / 500 m within 5 % of py-ballisticcalc, for 5.56 and 7.62×51 (and 9 mm / 7.62×39).
- Drift against Didion; the exponential model.
- Zero solve; determinism (bit-identical, seeded transonic walk).
- Ricochet probability and speed per material (Monte Carlo); penetration table (6 mm steel / 12 cm pine / 20 cm sandbag, 7.62×51 = 2× on steel, pistols); flesh: 9 mm stops in a torso and passes a limb, 5.56 fragments, 7.62 exits.
- Crack geometry (whole path and tick segments) and crack → impact → report ordering with the interval.
- Tracer spacing / phase / last rounds / all-tracer.
- Device component multipliers, blackbody, day ≤ 0.2× night, first-round pop once per cold can.
- The pure step budget.

**`tools/tests/bal_range.cfg`**
- Range overview.
- Flashes per device, day and night.
- Tracers in slow motion.
- Ricochet plates.
- Penetration witness panels.
- Run with `--validation`; screenshots go to `screenshots/ballistics/`.
