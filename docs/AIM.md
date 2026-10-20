# Aim: shots from the camera, the crosshair on the aim point

The bullets leave the **first-person camera** along the **view** (plus the recoil already applied
this tick, the sight offset and the spread cone), so they land where the crosshair says at any
distance. The drawn gun only shows the shot: the muzzle flash, the tracer's start and the casing are
on its barrel. This is the behaviour from right after the gun-driven fire animation (2026-10-17);
the later aim model (shots from the muzzle along the barrel, the hold aim turning the gun at the
crosshair's aim point, the crosshair moving to an obstruction) was reverted on 2026-10-19.

| Area | Code |
|---|---|
| The camera claim (pack, the host's check, out of walls) | `src/gameplay/aim.c/.h` (pure, `tests/test_aim.c`) |
| Where shots start, the visible muzzle, the shot | `src/gameplay/weapons.c` (`weapon_shot_eye`, `muzzle_world`, `fire`) |
| The claim in the command | `src/game/game.c` (`build_cmd`), `UserCmd.eye_cam` |
| The camera | `src/gameplay/characters.c` (`chars_eye_cam`: the nominal eye the first-person camera glides on, docs/CAMERA_HOLDPOINT.md "Rule 2") |
| Tracers | `src/gameplay/ballistics/bal_world.c` / `bal_fx.c` (`visual_offset`), `game_events.c` (`EV_TRACER`) |
| Crosshair | `src/ui/crosshair.c` |

## Where the shots start

- **The camera:** the nominal eye the first-person camera glides on (the head's "camera" socket
  without a table), relative to the simulated eye (`player_eye_pos`) and the view, in camera space
  (x right, y up, −z forward). The capsule eye can be 10 cm (standing) to 30 cm (crouched) away from it.
- **The claim:** the shooter puts it in every command (`UserCmd.eye_cam`, mm). The host checks it
  against its own copy of the shooter's pose (at most 4 ticks old): within 12 cm it is used as is,
  otherwise it is moved onto the 12 cm sphere around the host's and logged; never more than 0.6 m
  from the simulated eye. Without a claim or a pose: the capsule eye.
- **Never through a wall:** a cast from the capsule eye to the camera; on a hit the origin is pulled
  back 2 cm short of the surface (the render camera is kept out of geometry the same way).
- **Direction:** the command's view + the recoil of the earlier shots this tick + the sight offset
  (`weapon_aim_offset_deg`, 0 by default), then the spread cone (`recoil_cone_dir`), the ballistic zero
  and the muzzle device's POI (docs/BALLISTICS.md "Launch").
- Thrown items leave the eye with a little loft over the aim line; crossbow bolts 0.5 m ahead of it.

## What the drawn gun shows

- **The flash, the tracer's start, the casing:** the drawn gun's barrel tip for the local view
  (`hands_muzzle`, through the arms layer's projection), the held item's muzzle for others
  (`chars_item_muzzle`), else a barrel in front of the right side of the eye.
- **Tracers** start on the visible barrel and blend onto the bullet's real path over 60 m;
  the head streak and the tail blend the same way (bal_fx.c tracer_vis).
- The gun's sway, the fire animation's kick and the wall pull move the drawn gun only.

## Crosshair

- On the projection of the aim direction (where the next bullet goes): the screen centre, except
  while the fire animation's camera kick turns the view; then it stays on the true aim point and
  comes back with the camera. Third-person debug cameras: the aim point 50 m ahead.
- The gap is the exact screen projection of the spread cone.
- Aimed, there is none: it fades out over `ads_fade_time` and no dot replaces it (`ads_dot` false in
  `assets/ui/crosshair.json`, 2026-10-20); the sights are the aim.
- Hit markers fade out over `hitmarker.time` (smoothstep); a hit's is see-through (`alpha` 0.5, aimed
  `ads_alpha` 0.25 by the crosshair's ADS fade), a kill's opaque.
- `crosshair_pos` prints where it was drawn (tests).

## Tests

- `test_aim`: the claim's round trip, the host's tolerance / reach, the camera frame, the recovery.
- `tools/tests/bal_net.sh` (`net_fakelag 100`): a client's predicted hits match the host's.
