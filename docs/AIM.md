# Aim model: shots from the barrel, crosshair on the real impact

Bullets, tracers, muzzle flashes, casings and the report come from the weapon's **muzzle socket**,
not the camera. The crosshair still means what it always meant (the camera ray). When something sits
between the barrel and that point, the crosshair moves to where the bullet will actually land.

| Area | Code | Data |
|---|---|---|
| Pure math: claims, convergence, classes, recovery, crosshair ring / smoothing | `src/gameplay/aim.c/.h` | |
| Solve (traces), fire, host checks, `aim_debug`, `aim_report`, `aim_stats` | `src/gameplay/weapons.c` (`weapon_aim_solve`) | weapon `"aim"` block |
| Muzzle in camera space per tick pose | `src/gameplay/characters.c` (`chars_muzzle_cam`) | `hold.sockets.muzzle` |
| Claim in the command | `src/game/game.c` (`build_cmd`, `tick`), `src/game/usercmd.h` | |
| Crosshair | `src/ui/crosshair.c` | theme `crosshair_obstructed`, `crosshair_friendly` |
| Thrown items | `src/gameplay/throwables.c` (`spawn_thrown`) | |
| Tests | `tests/test_aim.c`, `tools/tests/aim_sockets.py`, `tools/tests/aim_obstruct.cfg` + `aim_check.py` | |

## The model

```
    eye ──────── camera ray ────────────────► P   (first hit, or eye + dir × max_range)
     │                                      ╱
     │ muzzle claim (camera space)       ╱   bullet: d = normalize(P − M), spread around d
     ▼                                ╱
     M ────────────────── Q ✕ ─ ─ ╱        Q: first hit of M → P before P (obstruction)
```

1. **Aim point P.** The camera ray: from the **first-person camera** (the head's `camera` socket of the
   tick pose, the point the player looks from) along the view plus recoil and the sight offset
   (`weapon_aim_offset_deg`). The capsule eye (`player_eye_pos`) is only the frame the claims are
   expressed in: it sits ~11 cm behind / left of the camera standing and ~30 cm below it crouched, so a
   ray from it missed the screen centre (and put crouched P under window sills the player saw over). First hit of the shot trace (world, tires, hitboxes; lag
   compensated on the host), or `eye + dir × max_range`.
2. **Muzzle M.** The `muzzle` socket of the held weapon in the latest evaluated tick pose: hold point,
   fire animation, recoil kick, wall pull-back. `chars_muzzle_cam` stores it in camera space (x right,
   y up, −z forward) relative to that tick's simulated eye and view. First and third person are the
   same body and hold point (docs/CAMERA_HOLDPOINT.md), so the socket agrees in both.
3. **Direction.** `d = normalize(P − M)` (`aim_converge`). The spread / recoil cone (`recoil_cone_dir`) is
   applied around `d`. Ballistics (zero, drop, drift) are unchanged. ADS: the sight is on the camera axis,
   so `d` is within a fraction of a degree of the camera ray. Hip: a small convergence angle.
4. **Obstruction Q.** One more trace along `M → P`, stopping 3 cm short of P. A hit there is where the
   bullet lands (`aim_classify`):

| Class | Rule | Crosshair |
|---|---|---|
| `clear` | nothing before P | on P (the projection of the camera ray's hit) |
| `partial` | a hit farther than `obstruct_near` | moves to Q, amber tint, ring = the spread at \|Q − M\| |
| `barrel` | a hit within `obstruct_near` (default 0.25 m) | moves to Q, amber, "barrel blocked" glyph |
| `friendly` | a player hit (any distance) | moves to Q, `crosshair_friendly` tint, "friendly in line of fire" glyph |

## Networking and validation

- `UserCmd.muzzle_cam` (3 × i16 mm): the muzzle the shooter sees, written by `build_cmd` from
  `chars_muzzle_cam`. All zero means "no claim".
- `UserCmd.eye_cam` (3 × i16 mm, protocol 23): the first-person camera in the same frame
  (`chars_eye_cam`). The host checks it against its own pose's camera socket (12 cm tolerance, 0.6 m
  reach cap) and casts from the capsule eye to it so it is never behind a wall. Without a claim or a
  pose the capsule eye is used. `UserCmd.aim_dist` (cm): the client's predicted
  distance to P of its first shot of the tick. Clients send their commands **after** predicting the
  tick, so it is filled in. Protocol 22.
- Host (`weapon_aim_solve`, `aim_host_check`):
  - It rebuilds the muzzle from its own copy of the shooter's pose. That copy uses the same hold point
    and IK code on the replicated view, stance and weapon state, and it must be at most 4 ticks old
    (LOD / culled poses are not used to check).
  - A claim within `wpn_muzzle_tolerance` (0.3 m) is used **as is**. Host and client then fire from
    bit-identical origins (`test_aim`).
  - A claim outside the tolerance is clamped onto the tolerance sphere and logged (anti-cheat: no
    poking a virtual barrel around a corner).
  - Every muzzle is capped at 1.5 m from the eye.
- The host recomputes P from the client's view and the lag-compensated world. It compares P with
  `aim_dist` and logs mismatches over max(0.5 m, 5 %) at debug level.
- `aim_stats [reset]` counts checks, clamps, the worst errors, muzzles inside geometry and blocked
  trigger pulls.
- The shot itself is unchanged: the seed, `EV_SHOT` (`pos` = the muzzle origin, `dir` = the converged
  aim) and the hitscan `EV_TRACER` (from the origin). Replays and observers' tracers start exactly where
  the host's bullets did. Their flash and casing stay on the shooter's visible barrel.
- Spectators: commands are forwarded whole, `muzzle_cam` included. The spectated player's simulation
  runs the same solve on the same claim, view and pose, so the spectator's crosshair moves the same way
  (`weapon_aim_preview` with `game_view_cmd()`).

## Edge cases

| Case | Handling |
|---|---|
| Muzzle inside geometry (after a mantle, vehicle entry, barrel poked into a wall) | A cast from the eye (inside the capsule, always free) to M. If it hits, the origin is pulled back to 2 cm short of the surface (`aim_recover_origin`). Class `barrel`, Q = origin: the bullet hits the wall at point blank and never starts inside or behind it. |
| Muzzle through a thin wall while the camera sees past it | The same eye → muzzle cast stops at the near face. The bullet hits the wall from the shooter's side and the crosshair shows it. |
| Wall edge / corner, window frame, mullion, vehicle bodywork, own cover | `partial` (or `barrel` when close). The crosshair moves to Q. The dev room stand has each case. |
| Teammate in the line of fire | The `M → P` trace hits the teammate's hitbox: `friendly` (colour + glyph). Teammates in front of the camera are already P. |
| Moving vehicles | The eye, view and claim are from the current tick. The claim is relative to the eye, so it moves with the seat. `weapon_shooter_ignore_body` ignores the own vehicle on the recovery cast and the traces. |
| Shotgun pellets | One solve per shot. Every pellet is a cone around the central `d` from the muzzle and is traced separately. The obstruction is classified on the central ray. |
| Melee | Unchanged (eye sweep). |
| Thrown items | `spawn_thrown`: the right palm of the pose (`chars_palm_world`) is the origin, with the same recovery. Direction = converge to P (40 m), then the 8° loft. |
| Prone / swimming / crouched behind low cover, muzzle near the ground | The ground is world geometry, never the player: only the shooter's own capsule / vehicle body is ignored. A barrel in the dirt or the sill is `barrel` / `partial`. |
| ADS through a magnified optic (zoom ≥ 3) | The reticle cannot move. The glyph shows at the reticle, an ✕ impact marker sits where Q projects (inside the scope view), and a faint red frame edges the screen. |
| ADS with a red dot / irons | The crosshair is drawn at Q over the sight while obstructed, like hip. |
| `block_fire_when_barrel_blocked 1` (host rule, default 0) | A `barrel` class holds the trigger: no shot, no ammo, a dry click. 0: the bullet hits the wall at point blank. |
| No pose (first tick, item just switched) | The claim alone (reach capped), else a default barrel beside the eye. |

## Data and cvars

- Weapon JSON: `"aim": { "obstruct_near": 0.25, "max_range": 300 }`. The default `max_range` is the
  weapon `range`. `tools/tests/aim_sockets.py` checks that every gun's `hold.sockets.muzzle` matches the
  first-person `muzzle`, sits in front of the grip, and has a sane `aim` block.
- `crosshair_obstruct 1`: move the crosshair to Q.
- `crosshair_obstruct_lerp 0.1`: time constant (s) of the move to and from Q. It is frame-rate
  independent.
- `block_fire_when_barrel_blocked 0`.
- `wpn_muzzle_tolerance 0.3`.
- `aim_debug 1` draws:
  - the camera ray (white) and P (green hit / blue far);
  - the bullet's line to where it lands (yellow), and the unreached part (red);
  - M (orange), the recovered origin (magenta), and Q with its class.
- `aim_debug 2` also logs every shot.
- `aim_report` prints this frame's solve and Q's screen position, and the ray's eye against the render
  camera (`G->view.eye`, within ~1 mm). `crosshair_pos` prints where the HUD drew the crosshair and
  the frame's target (Q's projection). Settled on an obstruction, the crosshair follows Q exactly (the
  ease is only for the moves to and from it).
- Theme: `crosshair_obstructed` (amber), `crosshair_friendly` (blue) in `assets/ui/theme.json`.

## Tests

- `test_aim` (ctest `aim`, pure):
  - claim round trip within 1 mm;
  - camera frames match `dir_from_angles`;
  - convergence: the impact at P at 10 / 50 / 200 m for hip and ADS muzzles, and every spread pellet
    within the cone radius of P;
  - ADS convergence angle < 0.3°;
  - classes;
  - muzzle-inside-wall recovery never inside the wall (64 poses);
  - validation: in tolerance verbatim, a cheat clamped, the reach cap, host origin == client origin;
  - spread ring and the crosshair's smoothing (within 1 px of Q after 1 s at 60 and 144 Hz).
- `aim_sockets` (ctest, Python): the muzzle socket of every gun.
- `tools/tests/aim_obstruct.cfg` + `aim_check.py` (game): the dev room aim stand. Open field hip / ADS
  (clear), wall edge (partial), barrel on the wall (barrel), the barrel rule (trigger held), window
  crouched, low wall, thin wall (recovered), car bonnet, a teammate (friendly). Each case checks its
  class and that the crosshair is within 1 px of Q's projection.
- `tools/tests/bal_net.sh` (LAN): the client's predicted impacts match the host's within 0.25 m. Both
  now fire from the claimed muzzle.
