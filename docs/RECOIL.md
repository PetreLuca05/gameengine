# Weapon recoil, spread and crosshair

Code: `src/gameplay/recoil.c` (pure math, unit tested by `tests/test_recoil.c`),
`src/gameplay/weapons.c` (fire loop), `src/ui/crosshair.c`, `src/ui/recoil_debug.c`.
Data: `data/weapons/*.json` (one gun per file), `assets/ui/crosshair.json`.

## Three layers plus a cone

| Layer | Where | Affects aim? | Networked |
|---|---|---|---|
| **Aim recoil** | `RecoilState` in `WeaponState` (part of `PlayerSim`) | yes: rotates the real view | predicted on the client and replayed identically on the host |
| **Camera kick** | `RecoilVisual.cam_*` springs (client only) | no: camera only | no |
| **Viewmodel kick** | `RecoilVisual.vm_*` springs to `HandsInput.vm_kick_*` | no: weapon/hands only | no |
| **Spread cone** | `recoil_spread()` | bullets are drawn inside it | derived from the replicated state |

### Aim recoil

Per shot `i` of the current pattern run, the step is a pure function of `(params, mag_seed, i)`:

```
base_v   = first_shot                                   (i == 0)
         = lerp(first_shot, per_shot, i / ramp_shots)   (i < ramp_shots)
         = per_shot                                     (after)
pattern  = pattern[min(i, count - 1)]                   (x = horizontal mult, y = vertical mult)
vertical = lerp(base_v * pattern.y, base_v * rand(v_min, v_max), pattern_randomness)
horizontal = lerp(pattern.x * h_per_shot, rand(h_min, h_max), pattern_randomness) + bias
kick = step * modifiers   (ADS blend, crouch, prone, moving, air, fire mode, after-sprint, wpn_recoil_scale)
```

Angles are degrees. Horizontal is positive to the right. The game's yaw is positive to the left, and
`weapons_tick` converts.

**Seed:** with `randomize_seed_per_mag`, each reload (and each equip) picks
`seed = hash(command seq, player slot)`. Every magazine is then a different but repeatable pattern,
and host and client agree without sending it. With `false`, the seed is fixed per weapon id, so you
can learn one pattern.

**Countering vs recovery.** `acc` is the offset the system added since the run started. Each tick,
`recoil_observe_input` compares the command's view change with what the system itself emitted last
tick. The rest is player input, and pulling against `acc` counts as `countered` (never more than
`acc`). This runs inside the simulation from command angles only, so the host derives the same value.
After `recovery.delay` without a shot, recovery returns `fraction × (acc − countered)` with an
exponential ease (initial rate `speed` deg/s), then the pattern restarts at shot 0. A new shot during
recovery keeps what was already returned and countered and continues the pattern. Pull down
perfectly and recovery has nothing to undo. Don't pull and you end up `(1 − fraction) × acc` above
your start.

### Camera and viewmodel kick

Each locally predicted shot raises a `ShotEvent`, and the game's hook `on_shot` adds an impulse to
the client springs (`recoil_visual_kick`). The springs are stepped per tick (semi-implicit Euler, 4
substeps) and sampled with the render alpha. The camera spring adds pitch, yaw and roll plus a small
backward offset. The aim is untouched, so the crosshair drifts against the kick. With
`crosshair_follow_kick 1` the crosshair is pinned to the screen center instead. The viewmodel spring
pushes the grip back and up, rotates the muzzle, and the IK support hand follows. `wpn_visual_scale`
scales both layers. ADS scales the camera kick by 0.6 and the viewmodel kick by
`viewmodel_kick.ads_scale`.

### Spread cone and crosshair

`cone = lerp(hip, ads, ads_t) × crouch × prone × lerp(1, move, speed) × air + bloom`, times
`wpn_spread_scale`. Bloom grows by `per_shot_bloom` per shot up to `bloom_max`, decays at
`bloom_recovery` deg/s, and grows when you take damage (flinch, host side).

The value is the **full** cone angle. Bullets use `recoil_cone_dir`: a center-weighted radius
truncated at the edge, so no bullet ever leaves the cone. The crosshair gap is the exact projection:

```
gap_px = tan(cone / 2) * (screen_h / 2) / tan(fov_v / 2)
```

Shotguns (pellets > 1) draw the cone as a circle. `wpn_recoil_debug 1` draws the same cone in the
world at the aim distance, and with `wpn_magdump` every bullet is checked
(`bullets checked / outside / max offset`).

## Hip vs ADS

Every multiplier blends with the aim progress (`ads_t`), so aiming mid-burst never pops.

| | Hip-fire | ADS |
|---|---|---|
| Aim recoil (view climb) | × `hip.aim_mult` (rifle 0.35: the camera climbs much less) | × `modifiers.ads` × `ads.aim_mult` (the original values) |
| Spread | `spread.hip` × `hip.spread_mult` | `spread.ads` |
| Bloom per shot / max / recovery | × `hip.bloom_mult` / `hip.bloom_max_mult` / `hip.bloom_recovery_mult` (more, higher, slower) | as configured |
| Item (gun) kick | × `hip.kick_mult` (1: the full animation) about the hip anchor | × `ads.kick_mult` about the **reticle centre** |
| Camera visual kick | unchanged | unchanged; the gun counters it so the reticle keeps marking the aim |

Hip accuracy is governed by the cone: the crosshair gap opens with the bloom on a mag-dump.

**Reticle-anchored ADS kick** (`src/player/holdpoint.c`):

- **Pivot:** in ADS the item kick, look lag and sway turn the gun about its `sight` socket, which
  is the optic's sight point or the iron-sight socket. That socket is already on the view axis at
  the eye relief.
- **Translation** is limited to the view axis: a push back (`ads.forward_push`) plus roll about
  the axis (`ads.roll`). The sight point therefore never leaves the screen centre; the rear of the
  weapon moves instead.
- **Eye clearance:** the push never brings the sight closer than 5 cm to the eye.
- **Iron sights only:** `ads.max_misalign_deg` limits the rotation so the rear sight doesn't
  cover the front post. Optics are parallax-free, so the limit doesn't apply.
- **Hip-to-ADS blend:** the hip and ADS contributions blend with the hold point's own ADS
  progress (pivot, rotation and translation), in both directions.
- **Interpolation:** the first-person item is interpolated between ticks about its sight point,
  and it stays exactly on the hold point while the hands hold it. The arms follow by IK; a reach
  pull in ADS runs along the view axis.
- **Optics:** a red dot draws its dot on the eye→sight line, and a picture-in-picture scope looks
  along it, so neither drifts when the gun turns.
- **Camera kick:** the visual camera kick turns the view but not the aim. While aimed, the first-
  person weapon counters it (`chars_set_fp_aim_offset`), so the reticle shows exactly where the
  shots go.

Measured: `tools/tests/recoil_ads.cfg` (mag-dumps at `wpn_kick_scale 3`) with `sight_test recoil`
/ `recoil_report`. The worst reticle-to-aim error was ≤ 0.81 px for irons, red dot, 4x, 8x and
the pistol. Camera climb over the same burst: hip 3.9°, ADS 7.7°.

## Weapon sway

The held weapon sways on its hold point (`src/player/holdpoint.c`), applied before the recoil kick.
The arms follow by IK, so the hands stay glued. Sway is cosmetic: it never moves the shots.

**Components:**
* **Breathing:** layered slow sines: a figure-eight, two slower drifts and a breath bob.
* **Look lag:** the gun trails mouse turns on a spring, in proportion to the turn speed.
* **Movement bob:** a walk / run cycle that grows with speed; sprinting bounces harder.
* **Landing dip:** sized by the fall speed, settling on a spring.

**Multipliers:**
* Each component has a hip and an ADS multiplier. They are blended with the hold point's ADS
  progress, so aiming mid-stride or mid-turn never pops.
* Stance scales everything: crouch × 0.75, prone × 0.55.
* An extra wobble is added at low health (below 35 %) and while aiming within 0.7 s of a sprint.

| | Hip | ADS (default) |
|---|---|---|
| Anchor | the pivot socket (`sway.anchor.hip`) | the **reticle centre** (`sight` socket) |
| Motion | rotation + translation | rotation about the sight only (translations become the equivalent rotation of a 0.35 m lever) |
| Breathing / look lag / bob / landing | `sway.hip` (1 / 1 / 1 / 1) | `sway.ads` (0.35 / 0.25 / 0.15 / 0.5) |
| Optic | – | × the optic's `sway_scale` (red dot 1.0, 4x 0.7, 8x 0.5) |
| Hold breath (Shift while aimed) | – | × `hold_breath_mult` (0.1) for `hold_breath` seconds, then a short shaky overshoot |

**In ADS:**
* The sight point never leaves the view axis, so the reticle stays centred and the bullets land
  there. The gun body breathes and lags around it.
* The scope eye-box shadow uses the optic's own axis, so it reacts to the residual sway. The
  picture follows the eye → sight line.

**Same sway on every item:** no weapon sets its own `sway`, `idle_sway` or `scope_sway`; every held
item uses the defaults above (idle sway 0.05). Only attachments still scale it (`sway_scale`). The
keys still parse, so a weapon can override them again.

**Aim sway:** the old aim sway that moved the reticle itself (`wpn_ads_sway`, `wpn_ads_bob`) is
still available, but defaults to 0.

```json
"sway": { "hip": { "breathing": 1.0, "look_lag": 1.0, "bob": 1.0, "landing": 1.0 },
          "ads": { "breathing": 0.35, "look_lag": 0.25, "bob": 0.15, "landing": 0.5 }, "hold_breath_mult": 0.1 },
"hold_breath": 3.5
```

**Settings:**

| Setting | Effect |
|---|---|
| `wpn_sway_scale` | master, host |
| `wpn_sway_ads_scale` | × the ADS multipliers |
| `cl_weapon_sway` | Settings › Crosshair › "Weapon sway" 0–1, cosmetic |
| `wpn_look_lag` | the hip look lag, 0.5 |
| `wpn_bob_scale` | the walk bob, 1 |

**Measured** with `tools/tests/weapon_sway.cfg` (`sight_test sway` / `sway_report`, `sight_test
recoil` / `recoil_report`); the reticle-to-aim error is over 600 ticks of ADS walking with mouse
sweeps:

| Sight | Hip range | ADS walking range | Reticle-to-aim error |
|---|---|---|---|
| Rifle irons | 1.1° | 0.45° | 0.58 px |
| Red dot | 1.0° | 0.45° | 0.66 px |
| 4x | 1.1° | 0.31° | 0.66 px |
| 8x | 0.9° | 0.22° | 0.54 px |
| Pistol | 1.1° | 0.46° | 0.47 px |

Hold breath, standing aimed (rifle irons): 0.23° → 0.014°; the release overshoot is 0.68°.
`sight_test align`: 26 combos PASS.

## Fire timing and modes

* **Sub-tick RPM:** the cooldown carries its remainder across ticks (`recoil_fire_budget`), and
  several shots can fire in one tick. 750 rpm fires exactly 12.5 rounds/s at 60 Hz. Releasing the
  trigger doesn't bank idle time.
* **Modes:** `fire_modes: ["auto", "burst", "single"]`, cycled with **B** (the `firemode` action).
  Burst fires `burst_count` at `burst_rpm` and then pauses one `rpm` interval.
* **Sprint to fire:** `sprint_to_fire` seconds after sprinting before you can shoot. The first shot
  after a sprint is scaled by `modifiers.after_sprint`.
* **ADS:** `ads_t` moves toward 1 over `ads_time` inside the simulation, so recoil, spread and sway all
  blend smoothly and deterministically.
* **Sway / hold breath:** in ADS the aim sways on a slow figure-eight (`scope_sway`, or `idle_sway`
  if that is 0) driven by the command sequence, so host and client agree. Bullets and camera both
  follow it. Hold **Shift** while aiming to steady it to 12% for `hold_breath` seconds. Running out
  makes the sway worse until your breath recovers.

## Shot hooks

```c
void weapons_add_shot_hook(ShotHook fn, void *user);   // gameplay/weapons.h
```

Called once per shot on its first prediction, with muzzle, aim direction, cone, pattern index, mag
left, fire mode, ADS amount and the per-shot seed. The game uses it for:
* the visual kick;
* shell casings (`fx_shell`);
* the bolt / pump cycle (sound plus a delayed casing) for `bolt_action` guns.

Hook muzzle-flash variants, brass sounds or animation events here.

The muzzle point is the barrel tip of the weapon model: the front face of its forward-most part,
derived at load time. For the local player, flashes, tracers and casings start at the viewmodel's
real barrel. Hitscan traces still start at the eye, so bullets go where the crosshair says.

## Data reference (`data/weapons/<nn>_<id>.json`)

```jsonc
{
  "id": "rifle", "item": "rifle", "kind": "hitscan",
  "rpm": 750, "burst_rpm": 900, "burst_count": 3, "fire_modes": ["auto", "burst", "single"],
  "ads_time": 0.2, "sprint_to_fire": 0.2, "idle_sway": 0.05, "scope_sway": 0, "hold_breath": 0,
  "bolt_action": false,
  "recoil": {
    "vertical":   { "first_shot": 0.75, "per_shot": 0.42, "min": 0.85, "max": 1.15 },
    "horizontal": { "per_shot": 0.22, "min": -0.28, "max": 0.32, "bias": 0.02 },
    "pattern": [[0, 1], [0.3, 1], [0.6, 1]],            // optional, last entry repeats
    "pattern_randomness": 0.35, "ramp_shots": 3, "randomize_seed_per_mag": true,
    "recovery": { "delay": 0.09, "speed": 10, "fraction": 0.85, "stop_on_input": true },
    "camera_kick":    { "pitch": 0.7, "yaw": 0.2, "roll": 0.45, "back": 0.012, "stiffness": 200, "damping": 20 },
    "viewmodel_kick": { "back": 0.035, "up": 0.01, "pitch": 2.6, "yaw": 0.9, "roll": 1.4, "random": 0.3,
                        "stiffness": 260, "damping": 22, "ads_scale": 0.45 },
    "spread":    { "hip": 2.2, "ads": 0.12, "per_shot_bloom": 0.18, "bloom_max": 2.4, "bloom_recovery": 7,
                   "move": 1.7, "crouch": 0.75, "prone": 0.55, "air": 3.0 },
    "modifiers": { "ads": 0.7, "crouch": 0.82, "prone": 0.6, "moving": 1.2, "air": 1.6,
                   "single": 0.9, "burst": 1.05, "auto": 1.0, "after_sprint": 1.25 },
    "hip":    { "aim_mult": 0.35, "kick_mult": 1.0, "spread_mult": 1.6, "bloom_mult": 2.0,
                "bloom_max_mult": 2.0, "bloom_recovery_mult": 0.5 },
    "ads":    { "aim_mult": 1.0, "kick_mult": 0.45, "forward_push": 1.0, "roll": 1.0, "max_misalign_deg": 1.5 },
    "anchor": { "hip": "pivot_socket", "ads": "sight" },          // pivot_socket | hold_point | sight
    "sway":   { "anchor": { "hip": "pivot_socket", "ads": "sight" } }
  }
}
```

Missing `hip` values come from the weapon type (`recoil_type_defaults`: rifle, pistol / crossbow,
SMG, LMG, shotgun, sniper / DMR), so a new weapon file gets sensible hip behaviour for free.
`ads.kick_mult` is the same value as `viewmodel_kick.ads_scale`, whichever comes last wins.

Files load sorted by name, and the weapon index is replicated, so keep the `nn_` prefixes stable.
`weapon_reload` re-reads the values in place. Adding or removing weapons needs a restart.
Prone is data-only until a prone stance exists.

## Presets

| File | Feel |
|---|---|
| `01_pistol` | Semi, big first-shot snap, fast full recovery |
| `02_rifle` (AR) | Auto/burst/single, steady climb with a learnable right-then-left drift |
| `03_shotgun` | 8-pellet fixed cone (circle crosshair), heavy kick, pump hook |
| `04_crossbow` | Projectile, simple kick, zero ADS cone |
| `07_smg` | 900 rpm, low climb, jittery horizontal, best hip fire |
| `08_lmg` | Heavy first shots that settle (`ramp_shots` 8); crouch steadies it a lot |
| `09_dmr` | Semi, strong kick that recovers almost fully, hold breath |
| `10_sniper` | Bolt action, one huge fully recovered kick, 5 s breath hold |

## Verifying

* `ctest --test-dir build` (or `build/bin/test_recoil`): determinism, ramp and pattern, modifiers,
  recovery of the uncountered amount, countering, cancel-on-shot, the cone never exceeded
  (20k samples × 12 cases), exact RPM for 48–4800 rpm, and springs settling.
* `build/bin/test_recoil_ads`:
  * the hip multipliers give the configured ratios and blend smoothly;
  * hip bloom exceeds ADS bloom and stays capped;
  * 600 ticks of 3× ADS kick keep the sight exactly on the view axis and at least 5 cm from the
    eye for all 26 weapon × optic combinations.
* `tools/tests/recoil_ads.cfg`: hip and ADS mag-dumps; `sight_test recoil_report` checks the
  reticle stays within 1 px of the aim.
* `tools/tests/recoil.cfg`: mag dumps of every preset at the 10 / 25 / 50 m wall with
  `wpn_recoil_debug 2`. Each dump logs `PASS`.
* `tools/tests/recoil_modes.cfg`: fire-mode cycling and measured RPM.
* `tools/tests/net_recoil_host.cfg` + `net_recoil_client.cfg`: a client firing full auto under fake
  lag and loss. `status` prints the recoil prediction error, which should be 0.
