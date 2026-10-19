# Aim model: the gun points at the crosshair, the bullet follows the barrel

The crosshair is **exactly the screen centre**. The gun's gameplay hold points its barrel at what the
centre is on: the camera ray's aim point P (the **hold aim**, a small turn of the gun about its pivot,
like ADS puts the sight on the view axis). The bullet leaves the weapon's **muzzle socket along that
barrel**, plus the weapon's spread / recoil cone and the ballistic zero; nothing ever turns it towards
P. The crosshair moves only when the barrel line does **not** end at P: something blocks it (a wall
edge, a window sill, the car's bodywork) or the gun was moved (pushed back against a wall, P closer
than the hold can turn to). Then it shows where the line really ends, Q.

| Area | Code | Data |
|---|---|---|
| Pure math: the hold aim, muzzle / barrel claims and their checks, the obstruction rule, recovery, crosshair ring / smoothing | `src/gameplay/aim.c/.h` (`aim_hold_solve`, `aim_classify`, `DG_DETMATH`) | |
| The hold aim in the hold point (gameplay pose vs drawn pose) | `src/player/holdpoint.c` (`holdpoint_eval`), `src/anim/weapon_hold.c` | `assets/human/holdpoint.json` |
| P per tick pose, the raw pose and the claims in camera space | `src/gameplay/characters.c` (`hold_aim_input`, `chars_aim_raw`, `chars_muzzle_cam`, `chars_barrel_cam`) | `hold.sockets.muzzle`, item −Z |
| Solve (traces), fire, host checks, `aim_debug`, `aim_report`, `aim_stats` | `src/gameplay/weapons.c` (`weapon_aim_solve`, `weapons_fill_aim_claim`) | weapon `"aim"` block |
| Claim in the command | `src/game/game.c` (`build_cmd`), `src/game/usercmd.h` | |
| Crosshair | `src/ui/crosshair.c` | theme `crosshair_obstructed`, `crosshair_friendly` |
| Camera layer of the fire animation | `src/game/game_render.c`, `src/player/camera_fp.c` (`cam_kick_rot`) | |
| Thrown items | `src/gameplay/throwables.c` (`spawn_thrown`) | |
| Tests | `tests/test_aim.c`, `tests/test_anim.c` (`test_hold_aim`), `tools/tests/aim_sockets.py`, `tools/tests/aim_obstruct.cfg` + `aim_accuracy.cfg` + `aim_check.py` | |

## The model

```
    eye ──────── camera ray ─────────────────────────► P   (first hit, or eye + dir × max_range)
     │                                         ╱             far = |P − eye|
     │ hold aim: the gameplay pose turned     ╱
     │ about its pivot (≤ 12°, ADS ≤ 3°)     ╱
     ▼                                      ╱
     M ══ barrel B ════════════════════════╱             bullet: from M along B (+ spread / recoil, zero)
                                                         Q: first hit of M + B·t; clear when Q is P
```

1. **Camera ray / P.** From the **first-person camera** (the head's `camera` socket of the tick pose,
   the point the player looks from) along the view plus recoil and the sight offset
   (`weapon_aim_offset_deg`, 0 by default). The capsule eye (`player_eye_pos`) is only the frame the
   claims are expressed in: it sits ~11 cm behind / left of the camera standing and ~30 cm below it
   crouched. First hit of the shot trace (world, tires, hitboxes; lag compensated on the host), or
   `eye + dir × max_range`.
2. **Hold aim** (below): the gameplay hold pose turned so the barrel line passes through P.
3. **Muzzle M.** The `muzzle` socket of that aimed gameplay pose. `chars_muzzle_cam` stores it in
   camera space (x right, y up, −z forward) relative to the tick's simulated eye and view. First and
   third person are the same body and hold point (docs/CAMERA_HOLDPOINT.md).
4. **Direction B.** The held item's −Z axis (the barrel, `ItemSockets`) in the same pose
   (`chars_barrel_cam`). Shots later in the same tick turn it by the recoil already applied this tick
   (`aim_view_delta`), exactly as that recoil turns the camera. The spread / recoil cone
   (`recoil_cone_dir`) is applied around B, ballistics add the zero angle and the device's POI on top
   (docs/BALLISTICS.md "Launch").
5. **Q.** One trace along the barrel line, up to `max_range`. With the hold aim reaching P, Q is P.

## Hold aim

A line offset from the camera ray meets it at one distance only. With no correction of the bullet
and the crosshair fixed at the centre, hits land on the crosshair at every distance only if the gun
itself points at P. So the **gun pose** aims, never the bullet, the same way ADS already computes the
sight onto the view axis.

**Two poses out of the hold point** (`holdpoint_eval`, docs/CAMERA_HOLDPOINT.md "Solve order"):

| Pose | What | Used by |
|---|---|---|
| gameplay | the state target + the inertialized blend → item = base × anchor⁻¹ → soft pitch limit → wall collision (cast from this pose, smoothed pull, pull transform) = `gp_item` → **hold aim** = `aim_item` | the bullet (`chars_muzzle_cam` / `chars_barrel_cam`), the claims, the host's check, the crosshair |
| drawn | `aim_item` + presentation: breathing, look lag, bob, landing (the global sway), the fire kick (`kick_pos` / `kick_rot`), the ADS sight anchoring and push, the phone's driven rotation = `item` | what is rendered; the hands are solved by IK onto it |

Presentation never reaches the gameplay pose, so sway, look lag, bob, landing, the fire kick and
`cl_weapon_sway` never move a bullet. The arm solve's reach shift (the hold point pulled towards the
shoulder when a hand is out of reach, `Δ = item_final × desired⁻¹`, normally identity) is applied to
both poses (`weapon_hold.c`).

**The turn** (`aim_hold_solve`, pure, `DG_DETMATH`):
- The bore line relative to the pivot C: its perpendicular offset `b = (M − C) − ((M − C)·B) B`.
  Turned by R it is `{C + R b + s R B}`; it passes through P when `R B = u = normalize(P − C − R b)`.
- R depends on u: 3 fixed iterations of `u = normalize(P − C − R b)`, `R = from_to(B, u)` (a
  contraction with ratio ~|b| / |P − C|, sub-µm at 2 m). A fixed count: every machine stops at the same
  bits.
- Past the limit, B is turned towards u by the limit only (`clamped`); then the weight scales the turn.
  P less than 5 cm ahead of the muzzle along B: no turn, faded in over the next 20 cm (continuous:
  there the turn is past the limit anyway).
- The result: `rot`, `muzzle = C + R (M − C)`, `barrel = R B`, `miss` = P's distance from that ray.
  The hold point applies `rot` about the pivot to the whole item, the claims and the host re-run the
  same function on the same raw: owner, remote copies, host and re-applications agree to the bit.

**Pivot and limits:**
- Hip: the item's pivot socket (between the hands). ADS: the **sight**, so the sight stays exactly on
  the view axis while the bore (5.2 cm under the rifle's irons / red dot) is aimed through P. Blended
  with the hold point's ADS progress.
- `wpn_hold_aim_max` (12°, a host rule); ADS `min(that, 3°)` (`AIM_HOLD_ADS_MAX_DEG`), blended the same way.
- **Weight:** 1 in hip, ADS, crouch, prone and the one-handed vehicle / hang hold; 0 in sprint, empty
  hands, inspect and the phone, blended with the state blend (`lerp(w(prev), w(state), 1 − k)`), so a
  sprint → hip blend eases the turn in. Times `1 − pull^curve`: **pushed against a wall the hold aim
  fades out** and the gun points where the pull pitched it (up by `collision.pitch`, 28°).

**The rifle's turn** (first-person camera, the hip toe-in 1.5° in the hold data; `aim_accuracy.cfg`,
`aim_obstruct.cfg`):

| P | 1.0 m | 1.26 m | 2 m | 2.5 m | 5 m | 25 m | 100 m | 300 m |
|---|---|---|---|---|---|---|---|---|
| hip | 11.9° | 8.4° | 4.76° | 3.5° | 1.60° | 1.29° | 1.44° | 1.48° |
| ADS (irons / red dot, about the sight) | — | — | 1.57° | — | 0.61° | 0.12° | 0.03° | 0.01° |

Closer than ~1 m the 12° limit is reached (the line misses P: obstructed); at ~0.8 m the wall
collision starts pulling the gun back. The toe-in no longer decides where the gun points; the data
stays.

**Where P comes from:**

| Consumer | P |
|---|---|
| Tick pose (`characters.c` `hold_aim_input`, armed players with the hold solve: LOD ≤ 1) | `weapons_aim_point`: the shot trace in the present world from the camera (the simulated eye + the previous pose's `eye_cam`; remote players on a client: the previous pose's camera socket) along the view + the aim offset; no hit: `max_range`. Fed in camera space (`hp_aim_p` = D × the direction). Remote players on clients: cosmetic only. |
| Command (`build_cmd` → `weapons_fill_aim_claim`) | `raw = chars_aim_raw`, the distance D traced from the command's eye claim (as packed, mm) along this command's view; `aim_hold_solve(raw, eye_cam + dir·D)` → `muzzle_cam`, `barrel_cam`; `aim_dist` = D in cm (saturating at 655 m: < 0.003°). The claim converges on the P of the tick it is fired in, also right after a near → far flick. |
| Crosshair, `aim_report`, `aim_debug` | `weapon_aim_preview` with `from_pose`: the raw re-aimed at this frame's P, so the crosshair stays centred while sweeping across depth edges. |
| Host check | the raw re-aimed at the host's P (below). |

`chars_aim_raw` is the gameplay pose before the hold aim in the claim frame (muzzle, barrel, pivot,
weight, limit), valid only when the hold solve ran (not the LOD2+ baked arms).

## Sway, recoil and the camera

| What | Moves | Never moves |
|---|---|---|
| Breathing, look lag, walk / sprint bob, landing dip (one global sway, docs/FIRE_ANIM.md) | the drawn gun | the bullet, the crosshair |
| Fire animation gun kick (layers 1–4) | the drawn gun, the hands | the bullet, the crosshair |
| Aim recoil (`RecoilState`) | the view: P moves with it, the hold re-aims, the crosshair stays centred | — |
| Spread and bloom | a cone around B (the crosshair gap) | the centre |
| Fire camera layer (layer 5) | roll twitch, FOV punch, push-back; pitch / yaw × `cam_kick_rot` (0) | the aim line: the render camera never leaves it, so the centre stays on it |
| `wpn_ads_sway` / `wpn_ads_bob` (host rules, 0) | the reticle (deliberately) and the bullets with it | — |

Optics draw the dot and the picture-in-picture on the eye → sight line, so ADS turning about the sight
keeps the reticle centred. Iron front posts dip at most ~1.8° at 2 m.

## Obstruction

The rule (`aim_classify`), with `hit` = Q's distance from the muzzle, `far` = P's distance from the
camera (`max_range` without a camera hit) and `miss` = P's distance from the barrel line
(`aim_line_miss`):

| Class | Rule (in this order) | Crosshair |
|---|---|---|
| `friendly` | the first barrel hit is a teammate's hitbox (any distance) | moves to Q, `crosshair_friendly` tint, "friendly in line of fire" glyph |
| `barrel` | the muzzle was inside geometry (recovered), or `hit ≤ obstruct_near` | moves to Q, amber, "barrel blocked" glyph |
| `obstructed` | `hit + depth < far − margin`, `margin = max(obstruct_margin, obstruct_margin_rel × far, parallax)`, `parallax = aim_parallax(miss, sin graze, far)` | moves to Q, amber, ring = the spread at \|Q − M\| |
| `obstructed` | `miss > aim_reach_tol(far)` = max(5 mm, far × tan 0.1°): the line does not end at P | moves to Q |
| `clear` | otherwise (no hit and miss ≈ 0: the sky) | exactly the screen centre |

The miss rule is "the gun and the hands were moved": the wall pull, past the 12° limit, the soft
pitch limit at steep views, the ADS 3° cap, reach shifts. The pull is not a separate input (reported
for diagnostics only). With the line through P, parallax is 0 and Q is P on any surface.

Examples:
- **Wall at 1 m, target at 50 m:** the line aimed at the 50 m P hits the wall: obstructed, the
  crosshair moves onto the wall where the bullet lands.
- **Wall edge up close** (`aim_obstruct.cfg` `wall_edge`): the camera 4 cm left of a wall's edge
  1.35 m away sees the panel 16 m down range; the hip barrel line, still ~9 cm right of the camera ray
  there, hits the wall: obstructed.
- **Pushed against a wall to the max** (pull 1): the hold aim is faded out, the barrel is pitched up
  28° by the pull, the bullet follows it (never a point above the barrel); the muzzle is inside the
  wall: `barrel`, Q = the recovered origin.
- **0.9 m from a wall** (nothing pulls yet): P needs more than 12°: the line misses P and lands on the
  wall beside it: obstructed by the miss, the crosshair on Q.
- **The ground ahead, far terrain:** the line goes through P on the surface: clear by construction (no
  parallax term needed).
- When the obstruction clears the crosshair eases back to the centre (`crosshair_obstruct_lerp`) and
  snaps to it exactly once within 0.05 px.

When the crosshair is moved it shows where the bullet goes. Bullets always fly along B: the crosshair
tells the truth about where that line ends, it changes nothing.

## Crosshair

- First person, class `clear`: exactly `(ui_w / 2, ui_h / 2)`, not P's projection (no eye vs render
  camera millimetre error). With `wpn_ads_sway`, `wpn_ads_bob` or `cam_kick_rot` above 0 it is drawn
  at the projection of the aim **direction** (a point at infinity). Third-person debug and vehicle
  orbit cameras keep P's projection.
- Not clear: the target is Q's projection, eased (0.1 s), then followed exactly once it has been the
  same obstruction for 0.3 s (same class, Q's distance within 25 %): a Q at point blank moves many px a
  frame with the body's idle motion. When Q goes to P (the pull fading out) the move is continuous.
- On clearing it eases back and snaps to the exact centre under 0.05 px.
- Through a magnified optic (zoom ≥ 3) the reticle cannot move: an ✕ impact marker sits where Q
  projects and a faint red frame edges the screen. Red dots / irons: the crosshair is drawn at Q
  over the sight while obstructed.
- `crosshair_pos` prints where it was drawn (2 decimals), the class, the weight and the target.

## Networking and validation

`UserCmd` layout as protocol 27; the meaning changed in protocol 30:
- `muzzle_cam` (3 × i16 mm) / `barrel_cam` (3 × i16, unit × 32767): the shooter's **gameplay pose
  aimed at this command's P** (`weapons_fill_aim_claim`). All zero means "no claim".
- `aim_dist` (cm): that P's distance along the command's own camera ray, written every tick a gun is
  held.
- `eye_cam` (3 × i16 mm): the first-person camera in the same frame (`chars_eye_cam`). The host checks
  it against its own pose's camera socket (12 cm tolerance, 0.6 m reach cap) and casts from the
  capsule eye to it so it is never behind a wall.

Host (`weapon_aim_solve`):
1. The eye (the camera claim checked as above), then P (lag compensated).
2. The reference: its own copy of the shooter's raw (same hold point and IK on the replicated view,
   stance and weapon state; at most 4 ticks old) re-aimed at its P, taken along the command's own view
   without the in-tick recoil: `ref = aim_hold_solve(raw, P_host)`.
3. A muzzle claim within `wpn_muzzle_tolerance` (0.3 m) and a barrel claim within
   `wpn_barrel_tolerance` (3°) of `ref` are used **as is**: host and client fire bit-identical origins
   and directions.
4. Otherwise, within tolerance of `alt = aim_hold_solve(raw, eye_cam + dir × aim_dist)` (the pose
   aimed at the shooter's own P: a depth edge or lag can give the two machines different P): used as
   is, logged at debug level, counted in `aim_stats` ("accepted at the shooter's own aim point"). The
   shooter can only pick a point on its own camera ray, which an honest gun does anyway.
5. Otherwise clamped onto the tolerance sphere / cone around `ref` and logged (anti-cheat: no poking
   a virtual barrel around a corner, no barrel aimed off the real gun).
6. No raw (LOD, culled): the claims against the latest pose, reach capped. No claims: the reference.
   Neither: a default gun beside the eye aimed at P within the limit.
- Every muzzle is capped at 1.5 m from the eye.
- The host compares P with `aim_dist` and logs mismatches over max(0.5 m, 5 %) at debug level.
- `aim_stats [reset]` counts checks, clamps, alt acceptances, the worst errors (muzzle, camera,
  barrel), muzzles inside geometry and blocked trigger pulls.
- The shot record is unchanged: the seed, `EV_SHOT` (`pos` = the muzzle origin, `dir` = B) and the
  hitscan `EV_TRACER`. Replays and observers' tracers start exactly where the host's bullets did; the
  flash and casing stay on the shooter's visible (drawn) barrel (`hands_muzzle`).
- Spectators: commands are forwarded whole; the spectated player's simulation runs the same solve on
  the same claims; the crosshair preview re-aims the target's pose at the frame's P.

## Edge cases

| Case | Handling |
|---|---|
| Muzzle inside geometry (after a mantle, vehicle entry, barrel poked into a wall) | A cast from the eye (inside the capsule, always free) to M. If it hits, the origin is pulled back to 2 cm short of the surface (`aim_recover_origin`). Class `barrel`, Q = origin: the bullet hits the wall at point blank and never starts inside or behind it. |
| Muzzle through a thin wall while the camera sees past it | The same eye → muzzle cast stops at the near face. |
| Wall edge / corner, window frame, mullion, vehicle bodywork, own cover | `obstructed` (or `barrel` when close). The crosshair moves to Q. The dev room stand has each case. |
| At the hold aim's limit (P closer than ~1 m) | The arm solve's reach shift (a hand out of reach moves the hold point a few cm, applied to both poses) is not re-aimed in the drawn pose: the drawn barrel can be a few degrees off the bullet's there. The line misses P either way (obstructed: the crosshair shows the bullet's Q). |
| Soft pitch limit (steep views) | The gameplay pose stops following the view pitch past `pitch_soft`; the hold aim turns it back within its limit, beyond it the line misses P: obstructed. |
| ADS irons / red dot | The gun turns about the sight (≤ 3°): the reticle stays centred, the bore line goes through P (1.7° at 2 m, 0.12° at 25 m). |
| `wpn_hold_aim_max 0` | A fixed hold: the hip barrel keeps the hold's offset (toed in), the line misses P by a few cm and the crosshair truthfully shows Q (a few px off the centre). |
| Sprint, empty hands, inspect, phone | No hold aim (weight 0), eased back in with the state blend. |
| Teammate in the line of fire | The barrel trace hits the teammate's hitbox: `friendly`, at any distance. |
| Moving vehicles | The eye, view and claim are from the current tick; the claim is relative to the eye. `weapon_shooter_ignore_body` ignores the own vehicle. |
| Shotgun pellets | One solve per shot; every pellet is a cone around B from the muzzle; the class is the central ray's. |
| Melee | Unchanged (eye sweep). |
| Thrown items | `spawn_thrown`: the right palm is the origin; a palm has no barrel and no hold aim, so the direction converges on P (40 m, `aim_converge`, the only convergence left), then the 8° loft. |
| `block_fire_when_barrel_blocked 1` (host rule, default 0) | A `barrel` class holds the trigger: no shot, no ammo, a dry click. |
| No pose (first tick, item just switched) | The claims alone (reach capped), else a default gun beside the eye aimed at P. |

## Data and cvars

- Weapon JSON: `"aim": { "obstruct_near": 0.25, "obstruct_margin": 0.75, "obstruct_margin_rel": 0.05,
  "max_range": 300 }`; `max_range` defaults to the weapon `range` (the camera ray's and the barrel
  line's length). `tools/tests/aim_sockets.py` checks every gun's muzzle socket and `aim` block.
- `wpn_hold_aim_max 12` (deg, host rule): the hold aim's limit (ADS at most 3); 0 = a fixed hold.
- `cam_kick_rot 0` (locked): the fire camera layer's pitch / yaw (1 for an A/B comparison).
- `crosshair_obstruct 1`, `crosshair_obstruct_lerp 0.1` (s, frame-rate independent).
- `block_fire_when_barrel_blocked 0`, `wpn_muzzle_tolerance 0.3`, `wpn_barrel_tolerance 3` (deg).
- `aim_debug 1` draws the camera ray (white) and P (green hit / blue far), the barrel line to Q
  (yellow), M (orange), the recovered origin (magenta) and Q with its class; `aim_debug 2` also logs
  every shot (`aim: shot class … origin … dir … pose … hold … miss …`, the prefix
  `bal_net_compare.py` matches, then `aim: shot impact … P at …`: the central ray's first hit).
- `aim_report`: this frame's solve and Q's screen position; the rule's terms (`far`, the barrel hit,
  the margin, the angle between B and the camera ray); the ray's eye against the render camera; the
  hold aim (`hold` turn, `miss`, `tol`, wall `pull`, `weight`, `max`, the drawn barrel vs the bullet's).
- Theme: `crosshair_obstructed` (amber), `crosshair_friendly` (blue) in `assets/ui/theme.json`.

## Tests

- `test_aim` (ctest `aim`, pure):
  - the hold aim for a rifle (toed in), a pistol and an LMG with a suppressor, P on the axis at
    2 / 5 / 25 / 100 / 300 m and off axis: the line within 0.1 mm of P, the turn within 0.01° of the
    exact (double precision) fixed point, bit-identical repeats, a planar case against the closed
    form; the shot (cone 0) on P, pellets within the cone around P, the packed claim within the reach
    tolerance (1 mm at 2 m);
  - ADS about the sight (the sight bit-exact on the axis, ≤ 3°), the clamp (the limit, the exact miss),
    P behind the muzzle, weight 0 / 0.5;
  - the classification table (line through P on any surface, sky, no hit with a miss, wall at 1 m,
    near → barrel, recovered → barrel, friendly first, the 5 mm / 0.52 m tolerance, a pulled gun);
  - host checks: a 2 m client P against a 20 m host P accepted through the shooter's own aim point, a
    6° cheat out of tolerance of every point of the ray and clamped to the 3° cone, the muzzle against
    the re-aimed reference; the claim round trips, frames, `validate_dir`, recovery, validation and
    the crosshair's smoothing.
- `test_anim` (`test_hold_aim`): the evaluation's hold aim is `aim_hold_solve` on its raw
  (bit-identical); full sway and kick move the drawn item but never the gameplay pose (static on vs
  off bit-identical); ADS with the aim on keeps the sight on the axis (< 0.02 mm) for every gun at five
  pitches and 2 / 25 / 300 m; owner vs remote bit-identical; pull 1 → no turn; sprint ↔ hip without a pop.
- `aim_sockets` (ctest, Python): every gun's muzzle socket and `aim` block; every gun's hip / crouch
  hold reaches P at 2 m within 12° with its longest muzzle device fitted.
- `tools/tests/aim_obstruct.cfg` + `aim_check.py` (game, the dev room aim stand, `wpn_spread_scale 0`):
  open hip (the shot on P within 1 cm), open ADS, the near wall edge, barrel, the barrel rule, the wall
  push at its maximum (pull ≥ 0.99, the shot along the pulled-up barrel ≥ 10° off the line to P, the
  drawn barrel = the bullet's with `holdpoint_static 1`), clear again, partial clear (obstructed, then
  back to the exact centre monotonically), out of reach at 0.9 m, the ground ahead, window, low wall,
  thin wall, car, friendly; idle / walk / recoil with sway and kick on (always exactly centred, every
  shot along the gameplay barrel while the drawn one sways off it). Every clear report: the crosshair
  ≤ 0.01 px from the centre and the miss within tolerance; every obstructed one: ≤ 1 px from Q; every
  shot within 0.02° of the pose's barrel.
- `tools/tests/aim_accuracy.cfg` + `aim_check.py --accuracy` (the ballistics range gongs): rifle hip,
  ADS irons and ADS red dot at 2 / 5 / 25 / 100 / 300 m: clear, exactly centred, the impact on P within
  max(1 cm, 0.01 % of D) without ballistics; with ballistics the impact's height above P matches the
  zero / drop path (`tests/data/ballistics_ref.json`) within 2 cm at 25 / 100 m.
- `tools/tests/bal_net.sh` (LAN, `net_fakelag 100`, `aim_debug 2`): every client shot leaves from the
  same origin (2 mm) along the same barrel direction (0.02°) on the host; the predicted impacts match
  within 0.25 m.

## History

1. **Convergence** (d7caac4): shots from the muzzle socket turned towards the camera's P.
2. **Barrel-forward, toed in** (27fee81): no convergence, shots along the barrel, the crosshair moved to
   Q only when obstructed; the hip barrel (toed in 1.5°) crossed the camera ray at ~4 m, so hip hits
   landed off the centred crosshair at most distances, and sway / the fire kick steered bullets.
3. **Hold aim** (protocol 30): the gameplay pose points the barrel at P, presentation is drawn only,
   the camera layer stays on the aim line: the bullet follows the barrel and lands on the centred
   crosshair, the crosshair moves only when the barrel is obstructed.
