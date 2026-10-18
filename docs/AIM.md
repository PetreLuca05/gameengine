# Aim model: shots along the barrel, crosshair on the real impact

Bullets, tracers, muzzle flashes, casings and the report come from the weapon's **muzzle socket**,
not the camera, and fly along the **barrel's forward axis**. Nothing corrects them towards the
camera's aim point. ADS puts the sight on the camera axis, so the barrel lies along it there; hip
fire keeps the gun's natural offset. The crosshair stays at the screen centre. Only when the barrel
line is **obstructed** (it hits something clearly closer than what the camera sees) does the
crosshair move to where that line ends.

| Area | Code | Data |
|---|---|---|
| Pure math: muzzle / barrel claims and their checks, the obstruction rule, recovery, crosshair ring / smoothing | `src/gameplay/aim.c/.h` | |
| Solve (traces), fire, host checks, `aim_debug`, `aim_report`, `aim_stats` | `src/gameplay/weapons.c` (`weapon_aim_solve`) | weapon `"aim"` block |
| Muzzle and barrel axis in camera space per tick pose | `src/gameplay/characters.c` (`chars_muzzle_cam`, `chars_barrel_cam`) | `hold.sockets.muzzle`, item −Z |
| Claim in the command | `src/game/game.c` (`build_cmd`, `tick`), `src/game/usercmd.h` | |
| Crosshair | `src/ui/crosshair.c` | theme `crosshair_obstructed`, `crosshair_friendly` |
| Thrown items | `src/gameplay/throwables.c` (`spawn_thrown`) | |
| Tests | `tests/test_aim.c`, `tools/tests/aim_sockets.py`, `tools/tests/aim_obstruct.cfg` + `aim_check.py` | |

## The model

```
    eye ──────── camera ray ──────────────────────► P   (first hit, or eye + dir × max_range)
     │                                                  far = |P − eye|
     │ muzzle claim (camera space)
     ▼
     M ══ barrel forward B ═══ Q ✕ ══════════════►      bullet: along B (+ spread / recoil), never towards P
                                                         Q: first hit of M + B·t, t ≤ max_range
```

1. **Camera ray / P.** From the **first-person camera** (the head's `camera` socket of the tick pose,
   the point the player looks from) along the view plus recoil and the sight offset
   (`weapon_aim_offset_deg`). The capsule eye (`player_eye_pos`) is only the frame the claims are
   expressed in: it sits ~11 cm behind / left of the camera standing and ~30 cm below it crouched.
   First hit of the shot trace (world, tires, hitboxes; lag compensated on the host), or
   `eye + dir × max_range`. P only says **how far the camera sees** (`far`); no bullet turns towards it.
2. **Muzzle M.** The `muzzle` socket of the held weapon in the latest evaluated tick pose: hold point,
   fire animation, recoil kick, wall pull-back. `chars_muzzle_cam` stores it in camera space (x right,
   y up, −z forward) relative to that tick's simulated eye and view. First and third person are the
   same body and hold point (docs/CAMERA_HOLDPOINT.md), so the socket agrees in both.
3. **Direction B.** The held item's −Z axis (the barrel, `ItemSockets`) in the same pose, camera space
   (`chars_barrel_cam`), re-expressed in the shot's view. Shots later in the same tick turn it by the
   recoil already applied this tick (`aim_view_delta`), exactly as that recoil turns the camera. The
   spread / recoil cone (`recoil_cone_dir`) is applied around B. Ballistics (zero, drop, drift) are
   unchanged: the zero angle is added on top of B.
   - ADS: the sight sits on the camera axis and the barrel is parallel to it, so B is within a fraction
     of a degree of the camera ray (`aim_obstruct.cfg` `open_ads`).
   - Hip: B is wherever the hold point points the gun. The impact keeps the muzzle's offset from the
     crosshair at every distance (a few cm), with no convergence angle.
4. **Q.** One trace along the barrel line, up to `max_range`. Q is its first hit (or its end).

## Obstruction

The rule (`aim_classify`), with `hit` = Q's distance from the muzzle and `far` = P's distance from the
camera (`max_range` when the camera ray hits nothing):

```
depth    = hit + the muzzle's depth ahead of the camera   (both distances from the camera)
margin   = max(obstruct_margin, obstruct_margin_rel × far, parallax)
parallax = separation × cot(grazing angle at P), at most max(3 m, 25 % of far)
separation = the muzzle's distance from the camera ray + far × tan(angle between B and the ray)
```

| Class | Rule (in this order) | Crosshair |
|---|---|---|
| `clear` | no barrel hit | the screen centre, nothing adjusted |
| `friendly` | the hit is a teammate's hitbox (any distance) | moves to Q, `crosshair_friendly` tint, "friendly in line of fire" glyph |
| `barrel` | `hit ≤ obstruct_near` (whatever the camera sees) | moves to Q, amber, "barrel blocked" glyph |
| `obstructed` | `depth < far − margin` | moves to Q, amber, ring = the spread at \|Q − M\| |
| `clear` | otherwise | the screen centre |

- The camera sees a target at 50 m while the barrel line hits a wall at 1 m: obstructed.
- The camera ray hits a wall at 40 m and the barrel ray lands on it beside P (hip offset), or 1.7 m
  short on an oblique face: **not** obstructed. That is parallax, not an obstruction (`margin` = 2 m
  at 40 m).
- Aiming at the ground ahead: a barrel 12 cm under the camera ray reaches the ground ~1 m before P at
  a 6.5° grazing angle. The `parallax` term covers it.
- ADS at far terrain: P on a slope 145 m out at a 0.6° grazing angle. A barrel 0.07° off the ray
  lands ~21 m short on the same slope. That is parallax again; a wall 60 m short is still an
  obstruction.
- Hip fire in the open: the hip barrel can point ~1–2° off the camera ray. At range its line may end
  on something well short of what the camera sees (a post 140 m out, P on a hill at 220 m). By the
  rule that is obstructed and the crosshair shows it: the bullet really lands there.
- When the obstruction clears, the crosshair eases back to the centre (`crosshair_obstruct_lerp`).

When the crosshair is moved it shows where the bullet goes. Bullets always fly along B: the crosshair
tells the truth about where that line ends, it changes nothing.

## Networking and validation

- `UserCmd.muzzle_cam` (3 × i16 mm): the muzzle the shooter sees, written by `build_cmd` from
  `chars_muzzle_cam`. All zero means "no claim".
- `UserCmd.barrel_cam` (3 × i16, unit × 32767, protocol 27): the barrel's forward axis of the same pose
  (`chars_barrel_cam`). All zero means "no claim".
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
  - The barrel claim gets the same treatment against the host's pose's barrel axis: within
    `wpn_barrel_tolerance` (3°) it is used as is (host and client fire along the same bits), beyond it
    is turned back onto the tolerance cone and logged (no aiming a virtual barrel off the real gun).
  - Without a barrel claim the host's pose is used, without either the camera direction.
- The host recomputes P from the client's view and the lag-compensated world. It compares P with
  `aim_dist` and logs mismatches over max(0.5 m, 5 %) at debug level.
- `aim_stats [reset]` counts checks, clamps, the worst errors (muzzle, camera, barrel), muzzles inside
  geometry and blocked trigger pulls.
- The shot record is unchanged: the seed, `EV_SHOT` (`pos` = the muzzle origin, `dir` = the barrel
  direction B) and the hitscan `EV_TRACER` (from the origin). Replays and observers' tracers start
  exactly where the host's bullets did. Their flash and casing stay on the shooter's visible barrel.
- Spectators: commands are forwarded whole, `muzzle_cam` and `barrel_cam` included. The spectated player's simulation
  runs the same solve on the same claim, view and pose, so the spectator's crosshair moves the same way
  (`weapon_aim_preview` with `game_view_cmd()`).

## Edge cases

| Case | Handling |
|---|---|
| Muzzle inside geometry (after a mantle, vehicle entry, barrel poked into a wall) | A cast from the eye (inside the capsule, always free) to M. If it hits, the origin is pulled back to 2 cm short of the surface (`aim_recover_origin`). Class `barrel`, Q = origin: the bullet hits the wall at point blank and never starts inside or behind it. |
| Muzzle through a thin wall while the camera sees past it | The same eye → muzzle cast stops at the near face. The bullet hits the wall from the shooter's side and the crosshair shows it. |
| Wall edge / corner, window frame, mullion, vehicle bodywork, own cover | `obstructed` (or `barrel` when close). The crosshair moves to Q. The dev room stand has each case. |
| A barrel line landing near P (parallax) | `clear`: the margin and the parallax term (see "Obstruction"). The crosshair stays at the centre. |
| Teammate in the line of fire | The barrel trace hits the teammate's hitbox: `friendly` (colour + glyph), at any distance. |
| Moving vehicles | The eye, view and claim are from the current tick. The claim is relative to the eye, so it moves with the seat. `weapon_shooter_ignore_body` ignores the own vehicle on the recovery cast and the traces. |
| Shotgun pellets | One solve per shot. Every pellet is a cone around the barrel axis B from the muzzle and is traced separately. The obstruction is classified on the central ray. |
| Melee | Unchanged (eye sweep). |
| Thrown items | `spawn_thrown`: the right palm of the pose (`chars_palm_world`) is the origin, with the same recovery. A palm has no barrel: the direction converges on P (40 m, `aim_converge`, the only convergence left), then the 8° loft. |
| Prone / swimming / crouched behind low cover, muzzle near the ground | The ground is world geometry, never the player: only the shooter's own capsule / vehicle body is ignored. A barrel in the dirt or the sill is `barrel` / `obstructed`. |
| ADS through a magnified optic (zoom ≥ 3) | The reticle cannot move. The glyph shows at the reticle, an ✕ impact marker sits where Q projects (inside the scope view), and a faint red frame edges the screen. |
| ADS with a red dot / irons | The crosshair is drawn at Q over the sight while obstructed, like hip. |
| `block_fire_when_barrel_blocked 1` (host rule, default 0) | A `barrel` class holds the trigger: no shot, no ammo, a dry click. 0: the bullet hits the wall at point blank. |
| No pose (first tick, item just switched) | The claims alone (reach capped), else a default barrel beside the eye pointing along the camera. |

## Data and cvars

- Weapon JSON: `"aim": { "obstruct_near": 0.25, "obstruct_margin": 0.75, "obstruct_margin_rel": 0.05,
  "max_range": 300 }`. The defaults are those, and `max_range` defaults to the weapon `range` (it is
  both the camera ray's and the barrel line's length). `tools/tests/aim_sockets.py` checks that every
  gun's `hold.sockets.muzzle` matches the first-person `muzzle`, sits in front of the grip, and has a
  sane `aim` block.
- `crosshair_obstruct 1`: move the crosshair to Q when obstructed.
- `crosshair_obstruct_lerp 0.1`: time constant (s) of the move to and from Q. It is frame-rate
  independent.
- `block_fire_when_barrel_blocked 0`.
- `wpn_muzzle_tolerance 0.3`, `wpn_barrel_tolerance 3` (degrees).
- `aim_debug 1` draws:
  - the camera ray (white) and P (green hit / blue far);
  - the barrel line to where it lands, Q (yellow);
  - M (orange), the recovered origin (magenta), and Q with its class.
- `aim_debug 2` also logs every shot.
- `aim_report` prints this frame's solve and Q's screen position, the rule's terms (`far`, the barrel
  hit, the margin, the angle between B and the camera ray), and the ray's eye against the render
  camera (`G->view.eye`, within ~1 mm). `crosshair_pos` prints where the HUD drew the crosshair and
  the frame's target (Q's projection). Settled on an obstruction, the crosshair follows Q exactly (the
  ease is only for the moves to and from it).
- Theme: `crosshair_obstructed` (amber), `crosshair_friendly` (blue) in `assets/ui/theme.json`.

## Tests

- `test_aim` (ctest `aim`, pure):
  - claim round trip within 1 mm, barrel claim round trip within 0.005°;
  - camera frames match `dir_from_angles`;
  - no correction: for hip and ADS muzzles at 10 / 50 / 200 m the impact is on the barrel ray and
    keeps the muzzle's offset from the camera ray, and every spread pellet is within the cone radius
    of the barrel line; the in-tick recoil turns B like the view;
  - the obstruction rule: no hit, wall at 1 m with a target at 50 m (obstructed), barrel within
    `obstruct_near`, the 40 m parallax cases (clear), the grazing ground and far-terrain ADS cases (clear), a crate short of
    P (obstructed), friendly;
  - muzzle-inside-wall recovery never inside the wall (64 poses);
  - validation: muzzle and barrel claims in tolerance verbatim, cheats clamped, the reach cap, host
    origin == client origin;
  - spread ring and the crosshair's smoothing (within 1 px of Q after 1 s at 60 and 144 Hz).
- `aim_sockets` (ctest, Python): the muzzle socket and `aim` block of every gun.
- `tools/tests/aim_obstruct.cfg` + `aim_check.py` (game): the dev room aim stand.
  - Open field hip (clear: the crosshair at the centre, the shot along the barrel line, Q on it) and
    ADS (clear, B within 0.3° of the camera axis).
  - Wall edge (obstructed), barrel on the wall (barrel), the barrel rule (trigger held), back in the
    open (the crosshair returns to the centre), the ground 15 m ahead (parallax: clear).
  - Window crouched, low wall, thin wall (recovered), car bonnet, a teammate (friendly).
  - Each obstructed case checks the crosshair within 1 px of Q's projection; each shot checks its
    direction against the barrel's and that Q lies on its line.
- `tools/tests/bal_net.sh` (LAN, `FAKELAG=100` for `net_fakelag 100`): the client's predicted impacts
  (ADS and hip) match the host's within 0.25 m. Both fire from the claimed muzzle along the claimed
  barrel.
