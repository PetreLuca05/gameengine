# Hitboxes, body-part damage, hit reactions and ragdolls

Code: `src/gameplay/hitboxes.c` (hitbox sets, rewind, ray / sweep tests, multipliers),
`src/gameplay/weapons.c` (`trace_shot`, `apply_hit`), `src/gameplay/health.c` (`apply_damage_part`),
`src/gameplay/characters.c` (`chars_on_damage`, reactions, death, ragdolls),
`src/anim/damage_additive.c`, `src/anim/ragdoll_def.c`, `src/physics/ragdoll.c`.
Data: `assets/human/hitboxes.json`, `assets/human/ragdoll.json`, `assets/characters/*.json`
(`part_mult`), `data/weapons/*.json` (`part_mult`, `falloff`), `controller.json → damage_reactions`.

## Body parts and hitboxes

There are 17 body parts (`src/anim/body_parts.h`): `head`, `neck`, `chest`, `stomach`, `pelvis`, plus
`upper_arm`, `lower_arm`, `hand`, `upper_leg`, `lower_leg` and `foot`, each `_l` / `_r`.

Hitboxes **are the ragdoll bodies**: one capsule per bone group, fitted to the canonical mesh, with
lateral capsules on the torso. `hitboxes.json` tags each body with a part and an optional
`radius_scale`. Because one definition serves both, what you hit is what falls over.

* **Host.** Builds the hitbox set every tick from the **sim-only** animated pose. That's the
  controller output without cosmetic IK, so it's identical on every host. The set is pushed into a
  per-character lag-compensation ring of `HB_HISTORY` = 32 ticks (0.53 s).
* **Clients.** Build hitboxes from their local rendered pose, for prediction and debug draw only.
* **Shots.** `trace_shot` tests characters with `chars_raycast`. On the host it is **rewound to the
  shooter's view tick**: an exact copy for whole ticks, an interpolation between the two
  neighbouring ticks otherwise. Melee uses `chars_sweep` (capsule sweep). Static geometry still
  blocks: the closest hit wins.
* `hitbox_debug 1` draws the capsules over the mesh, colored by part, with `part xmult` labels.
  `hitbox_reload` re-reads the JSON.

## Damage

```
final = base × character.part_mult[part] × weapon.part_mult[part] × falloff(distance)
```

* **Character multipliers.** `assets/characters/<id>.json → part_mult`, per part or by group
  (`arms`, `hands`, `legs`, `feet`, `limbs`, `torso`). Unlisted parts are ×1. Player: head 2.5,
  neck 1.5, stomach / pelvis 0.9, arms 0.6, hands 0.5, legs 0.7, feet 0.5. Zombies: head 3.0 to
  4.5, neck 1.5 to 2.0, limbs 0.4 to 0.6.
* **Weapon multipliers.** `data/weapons/*.json → part_mult`, the same keys, on top of the character
  value. Shotgun head ×0.75 (pellets), sniper head ×1.6 / neck ×1.3, DMR head ×1.2.
* **Falloff.** `"falloff": { "start": 8, "end": 30, "min": 0.2 }` gives full damage up to `start`,
  then a linear fall to `min` at `end`. With no block there's no falloff.
* `headshot_mult` only applies to the legacy capsule targets (non-character entities).

`apply_damage_part` applies the result and raises a damage-number event carrying the part and the
multiplier. Hit feedback:
* the number is labelled `<damage> <part> x<mult>`;
* head hits use the head hitmarker colour, kills the kill hitmarker (larger);
* hit markers are visual only: no hit, headshot or kill confirm sound.

On the test stand dummies, each hit is also logged:

```
dummy hit: head x2.50 (char 2.50, weapon 1.00, falloff 1.00) = 45.0 damage
```

### Limb status effects

These are host-authoritative and replicated in `AnimNetState.limb_status`:

| Bit | Trigger | Effect |
|---|---|---|
| `LIMB_LEGS` | a leg / foot hit on a zombie | moves at 45 % speed for 4 s |
| `LIMB_ARMS` | an arm / hand hit on a player | +1.2° weapon spread for 3 s (`chars_limb_spread`) |

## Armor

Armor points sit in the `Health` component (`armor`, `max_armor`; `ARMOR_ABSORB` 0.6 of every hit
until it runs out, falls and drowning excepted).
- **Plates:** an Armor Plate (`"armor": 50`) is one plate (`ARMOR_PER_PLATE`).
  - Without a plate carrier a player holds `ARMOR_BASE_PLATES` (1): `max_armor` 50.
  - With one in the body armor slot, its `"plates"` (4): `max_armor` 200.
  - The HUD draws one bar segment per plate.
  - Using a plate with the armor full is refused ("Armor full: a plate carrier holds 4 plates").
  - **V** (`ACT_PLATE`, "Insert Armor Plate") puts one in: the first plate carried goes into the
    hands and is used at once (the healing wheel's auto-use, whatever `wheel_heal_auto_use` says).
    - With none carried, or the armor full, it only says so (a hint).
    - In a seat V stays the vehicle camera.
    - `tools/tests/plate_key.cfg` + `plate_key_check.py` test it.
- **The carrier holds its plates:** `health_sync_armor`, on the host every tick, keeps the armor
  above the base plate in the worn carrier's `InvSlot.ammo`.
  - Taken off, dropped or looted from a corpse, the carrier keeps them; armor drops to at most 1 plate.
  - Put on, they come back on top of the base plate.
- **Helmet:** a head hit loses `HELMET_ABSORB` (half) of its damage to a worn helmet while its
  durability lasts. The helmet loses the absorbed points.
- `max_armor` is replicated with `NF_HP` (protocol 34).

## Hit reactions

Every hit applies a **procedural additive reaction** at the start of the post-pass, before the hold
solve, so the hands stay glued. A damped spring per bone rotates the struck chain away from the shot
direction, falling off along the chain, with strength from the damage.

Presets in `controller.json → damage_reactions`:

| Preset | Used for | Notes |
|---|---|---|
| `head` | head, neck | |
| `torso` | chest, stomach, pelvis | small translation |
| `arm` | arms, hands | |
| `leg` | legs, feet | pelvis drop |

Each preset has a strength, a falloff, a chain depth, stiffness and damping. The springs come to
**exact rest**: they snap to zero below the rest thresholds, and a test checks they return to zero.
`hit_mode` selects `procedural` (default), `clips` (the additive `hit` layer with `hit_front` /
`back` / `left` / `right` slots) or `both`.

Physical hit reactions (a powered ragdoll blended over the animation) aren't implemented. Living
characters react only procedurally or with clips, and Jolt is used only after death.

On clients, the shooter predicts the reaction immediately (`cl_predict_hit_reactions`). The host's
hit event then confirms it and isn't doubled, as long as it names the same part within 0.3 s.

## Death and ragdolls

On the host, a lethal hit does the following:
1. fires the `die` trigger (the death state, with a variant from the seed);
2. sends `AEV_DIE` (tick, seed, part, direction, strength);
3. drops the held item as a physics prop that despawns after 30 s (test dummies keep theirs and respawn).

Every peer then builds its own **cosmetic Jolt ragdoll** from its local pose. Ragdolls are never
replicated.

* **Bodies.** 17 capsules. Mass is normalized to 80 kg with a human distribution, and
  `ragdoll_scale` scales them per character.
* **Joints.** Swing-twist joints (cone + twist), and hinges for knees and elbows. Limits are
  relative to the bind pose (`ragdoll.json`).
* **Bodies collide with** static geometry and each other, except across directly jointed pairs
  (group filter). They collide with characters only with `anim_corpse_collision 1`. Ragdoll bodies
  get extra solver iterations, and while any ragdoll is simulating the world runs 2 collision
  substeps per tick (`phys_set_collision_steps`).
* **Spawn.** Velocities come from the last two animated poses, plus the killing-hit impulse on the
  struck body. A push-out raycast keeps limbs from spawning through walls.
* **Muscle tone.** For `motor_time` (0.3 s) the joints are driven toward the death pose with fading
  strength.
* **Sleep and freeze.** After `sleep_time` (1.5 s) at rest, the bodies are removed and the pose is
  frozen. Zombies despawn after `anim_ragdoll_time`. `anim_max_ragdolls` (24) caps simulated
  ragdolls: the oldest freezes when the cap is hit.
* **LOD.** At LOD3 or when culled, there's no simulation: a settled lying pose is chosen from the seed.
* **Late joiners and long-dead characters.** They get the death tick and seed in the replicated
  state and use the settled pose.

Tools: `ragdoll [push N]` ragdolls or pushes the character under the crosshair. `char_kill [damage]`
kills it (host), and `anim_stand_drop` / `anim_stand_push` work on the test stand.
`anim_ragdoll_debug 1` shows the bodies, masses, cones and hinge axes.

### Ragdoll test

`test_ragdoll` (ctest `ragdoll`) runs real Jolt physics. It drops ragdolls in five cases:
* a bind T-pose from 1 m;
* an idle pose falling backward;
* a walking pose pushed sideways;
* a head-first drop from 3 m;
* a rolled 2 m drop with spin.

Each case simulates 10 s at 60 Hz. Every tick, every joint is decomposed into swing and twist in the
child's bind frame (Jolt's convention) and compared to its limits.

| Check | Bound | Measured |
|---|---|---|
| transient overshoot on impact | ≤ 20° | 6–15° |
| settled pose | ≤ 3° | ≤ 0.07° |
| no NaN, max body speed | < 25 m/s | |
| tunnelling into the floor | < 7 cm | |
| resting penetration | < 5 cm | Jolt's 2 cm slop + residue; the mesh is wider than the capsules |
| freezes within 10 s | | |

It also checks the definition: 80 kg total, joint count, normalized axes, limit ranges, and one body
per body part.
