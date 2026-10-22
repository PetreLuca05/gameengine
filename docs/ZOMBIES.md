# Zombie hordes

Hundreds of zombies that follow the nearest player, keep out of each other's way, slide along walls
and attack when close. Not full AI yet: there is no navmesh, but the seek term goes through a
`Pathfinder` hook so one can replace it (see [Pathfinding](#pathfinding)).

| File | What |
|---|---|
| `src/gameplay/zombies/horde.c/.h` | Host simulation, commands, stats, debug draw, `horde_test` |
| `src/gameplay/zombies/horde_steer.c/.h` | Config (`assets/ai/zombie.json`), tiers, steering kernel (pure) |
| `src/gameplay/zombies/horde_grid.c/.h` | Uniform spatial hash for neighbour queries (pure) |
| `src/gameplay/characters.c` | Horde-aware animation / hitbox evaluation, ray broadphase |
| `src/net/replication.c` | Zombie update rates, byte budget, compact deltas, client smoothing |
| `src/core/jobs.c` | `jobs_parallel_for` |
| `tests/test_horde.c` | Grid vs brute force, determinism across thread counts, hysteresis |

## Commands

| Command | |
|---|---|
| `spawn_horde <count> [radius] [ring\|random\|cluster] [character_id\|mixed]` | Around the local player (default 30 m ring, mixed walker / runner / brute / helmet), on valid ground, 32 per tick |
| `despawn_horde` | Remove all horde zombies |
| `horde_stats` | Count, tiers, physics bodies, per-phase tick cost, attacks |
| `horde_target <players\|player [slot]\|point [x y z]\|none>` | Override the targeting (testing) |
| `horde_debug 1\|2\|3` | Tiers (red near, yellow mid, green far, grey dormant) + velocities + capsules; 2 + wall casts; 3 + grid cells |
| `horde_test [count] [quit]` | Spawn, let them close in for 10 s, check placement and the host tick budget |
| `horde_reload` | Re-read `assets/ai/zombie.json` |
| `net_snapstats` | Host: per-client kB/s and held zombie updates. Client: zombie motion smoothness |

All are host-only except the stats; clients get zombies through normal replication. The dev room
has a **horde yard** south of the room: stand on the red pad at (0, 0, 50). It has crates and
walls to steer around, plus a funnel corridor to the north-east.

## Architecture

Zombies stay world entities (`ET_ZOMBIE`) with a `CharacterInst`. Rendering, per-bone hitboxes,
damage, ragdolls and replication are unchanged. The horde owns their movement and decisions in
structure-of-arrays (SoA) form. That covers a 2048-slot pool (`HMAX`) with position, velocity,
yaw, target, tier, timers and cached ground height. There are no per-tick allocations.

The host tick (`horde_host_tick`, before `chars_host_tick`) runs these phases in order:

1. **Spawn queue:** at most `spawn.per_tick` spawns per tick. Each place is checked at enqueue:
   a downward cast, the slope, a capsule-overlap test, and up to 8 pushes around obstacles.
2. **Sync:** the dead and the removed are dropped. Each zombie gets its distance to the nearest
   player and its **tier**. The target is the nearest player, re-picked every `retarget_ticks`
   with staggering.
3. **Grid:** a counting-sort hash with 2 m cells over the living zombies, used for separation,
   alignment and attack range.
4. **Steering:** `horde_steer_range` over chunks with `jobs_parallel_for`.
   - The terms are seek (through the pathfinder hook, with arrival), separation, alignment and
     wall avoidance.
   - It reads last tick's arrays and writes per-zombie outputs, so results are bit-identical for
     any thread count (`test_horde`).
5. **Wall casts:** each near or mid zombie casts one forward ray every `cast_interval` ticks,
   round-robin. A hit gives a push-off plus a slide toward the side the target is on.
6. **Mover:** runs every tick because it is cheap, and moves dormant zombies once a second.
   - Ground height is cached and re-cast every `ground_interval` ticks: twice as often when near,
     half as often when far.
   - It handles wall slide and refuses steps that are too high, slopes that are too steep, and
     edges.
   - Zombies never walk inside a player (0.62 m).
7. **Physics tier:** near zombies keep their kinematic Jolt capsule so they block and get pushed;
   others have it disabled. The near tier also keeps lag-compensation history.
8. **Attacks:** a near zombie in range of its target snaps to face it and swings (`chars_npc_melee`); it
   stays snapped onto the player while the attack runs (no turn rate; `attack.facing_deg` is unused). The hit
   lands after `hit_time` if the target is still close, through `apply_damage`.
9. **Facing / look:** chasing a player within `movement.face_target_dist` (4 m), or attacking, the
   body faces the player instead of the motion (crowd separation and wall slides used to turn them
   sideways, which also blocked the attack's facing check). Head and spine track the nearest living
   player within 16 m and 120 deg of the body (`zombie_look` in characters.c, through the aim
   inputs). Host and clients run the same rule from replicated positions, so nothing extra is sent.
10. **Attack clips:** four Mixamo clips (`assets/human/Zombie Attack*.fbx`, `Zombie Punching*.fbx`,
   git-ignored like the other FBX): swipe, two-handed smash and lunge play full body on the base
   layer (trimmed to the strike in `anim_sets/zombie.json`, sped up in `clips.json`); the punch plays on
   the upper body (`zombie_punch` state) so a zombie that is still walking keeps its legs.
   `chars_npc_melee` picks one at random, never the same twice in a row (a moving zombie always
   punches), sends it as the melee event's seed, and returns that clip's contact time for the hit.

`chars_host_tick` then evaluates animation and hitboxes:

- Horde zombies are evaluated every `anim_interval[tier]` ticks, phase-spread, in parallel jobs.
- Between evaluations their hitboxes are shifted with the body, so hits stay on the visible
  zombie.
- Non-horde characters (players, dummies, `zombie_area` wanderers) evaluate as before.

`chars_raycast` tests a bounding sphere before any hitbox rewind, so a shot into a crowd stays
cheap.

### Tiers

The tier is chosen by distance to the nearest player. Every value is in `assets/ai/zombie.json`.

| Tier | Distance | Steering + mover | Animation + hitboxes | Jolt capsule | Separation | Net updates |
|---|---|---|---|---|---|---|
| near | < 20 m | every tick | every tick | yes (< 20 m, +4 m hysteresis) | yes | every snapshot |
| mid | < 50 m | every 2nd tick (moves every tick) | every 2nd | no | yes | every 2nd |
| far | < 140 m | every 4th | every 4th | no | no | every 4th |
| dormant | beyond | once a second | once a second | no | no | not sent |

Bands have 3 m of hysteresis (`tiers.hysteresis`), so a zombie on an edge doesn't flap between
tiers.

Zombies a level places outside the horde (the dev room hall, story chapters before they wake) use the
same bands (PF-0051, `chars_host_tick`): their wall / ground rays every `sim_interval` ticks (they walk
straight in between), animation + hitboxes every `anim_interval` ticks (hitboxes shifted with the body
in between) and no Jolt capsule beyond `physics_dist`. Their drawn pose follows the character LOD as
before. `perf_phys_active` lists the physics bodies awake (Jolt sleeps idle props on its own).

## Budgets (measured)

Measured on this machine (RTX 3060 Ti, 12 logical cores) with `horde_test`, while the horde closes
in on the player:

| Zombies | Host tick, horde + their animation | Budget |
|---|---|---|
| 500 | 1.8 ms avg, 3.2 ms max | 2 ms |
| 1000 | 2.3 ms avg, 3.6 ms max | 4 ms |

Per-phase cost at 1000, about 1.5 ms of it animation evaluation (≈ 600 evaluations per tick across
the workers):

| Phase | ms |
|---|---|
| grid | 0.03 |
| steering | 0.3 |
| casts | 0.07 |
| mover | 0.13 |
| physics + attacks | 0.06 |

The steering kernel alone (500 zombies) costs 0.03 ms on 7 workers (`test_horde`). Rendering
1000 zombies in view runs at the 60 FPS cap. Skinned draws are already instanced by mesh, and the
bone palette buffer was raised to 196,608 entries (~1000 characters plus shadow draws).

## Networking

Hordes are host-authoritative. Clients run no steering; they animate from replicated state.
Per-client rules, in `repl_rate_limit`:

- **Relevance** is unchanged (120 m). Zombies are never dropped from a snapshot by the rate
  rules; they are only *held*.
- **Rates:** a zombie that isn't due this snapshot (near every one, mid every 2nd, far every 4th)
  keeps the baseline's transform and animation, so its delta is empty and nothing is sent.
- **Byte budget** (`net.budget_bytes_per_s`, 22 kB/s): due zombies go out by priority
  (snapshots waited × distance weight 4/2/1). The rest wait and gain priority.
- **Compact deltas:**
  - zombie positions are 3-byte deltas (1/64 m) against the client's acknowledged state, falling
    back to 6-byte (1/256 m) or full positions;
  - quantized values are stored back into the sent frame, so deltas never drift;
  - zombies send only discrete animation state (states, flags, limbs, weapon, character, death);
    clients derive speed and facing from the motion.
- **Client:**
  - interpolation samples are added only when a zombie moved;
  - past the newest sample it extrapolates along the last velocity (up to 0.3 s);
  - the drawn position follows the network position over about 0.1 s, so corrections blend
    instead of snapping.

Rotation baselines now hold the quantized value. Before, every rotation of every entity looked
changed and was resent each snapshot, which was the biggest cost even without zombies.

Measured with 500 zombies converging on the players, host plus 3 LAN clients (4 players):

- **Bandwidth:** **38 kB/s per client**, including about 5 kB/s of dev-room target dummies.
- **Smoothness** on the client: the per-frame velocity change is 5% of the walking speed.

A new horde appearing costs a one-off burst of full states (about 50 kB for 500).

## Pathfinding

`horde_steer.h` declares `typedef bool (*HordePathFn)(void *ctx, vec3 from, vec3 to, vec3 *next)`.
The kernel calls it per zombie and seeks `next` instead of the target when it returns true.
`horde.c` passes `path_stub`, which is a straight line. To add a navmesh:

1. Build or load the mesh in a `src/gameplay/zombies/navmesh.c`.
2. Keep per-zombie corridors (path ids) in the pool, refreshed on retarget or when a zombie
   strays.
3. Return the next corridor corner from the hook.

Separation, alignment, wall casts and the mover stay as they are. Path queries belong in the
retarget phase, spread over ticks, never in the kernel.

## Tuning

`assets/ai/zombie.json`, reloaded with `horde_reload`:

- **`movement`:**
  - speed multiplier, acceleration, turn rate, arrival distance, ±speed jitter;
  - separation, alignment, avoidance and seek weights and radii;
  - cast and ground intervals, step height, maximum slope.
- **`attack`:** range, facing angle, cooldown, hit delay, damage.
- **`tiers`:** distance bands, hysteresis, sim and animation intervals, physics distance.
- **`spawn`:** spawns per tick, default radius, pool capacity (≤ 2048).
- **`net`:** update intervals per tier, distance bands, per-client byte budget.

Characters keep their own `move_speed`; the horde multiplies it. Leg hits slow zombies to 45%
through `limb_status`.

## Not done yet

- A shared clip-sampling cache for identical (clip, frame) pairs at LOD 2+ isn't built. The
  budget is met without it, and crowd variety comes from random start phases and the speed
  jitter.
- Far-tier deaths use the existing path, which ragdolls when the global ragdoll cap allows and
  otherwise settles into a death pose.
- Spawn and despawn ride the snapshots rather than separate batched reliable events. New zombies
  arrive with the normal full-state delta.
