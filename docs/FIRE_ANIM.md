# Fire animation (gun-driven, layered, mass-based)

Code: `src/anim/fire_anim.c` (pure layers + mass model, unit tested by `tests/test_fire_anim.c`),
`src/game/game_fire.c` (drives it from the shots, camera / post / casing / tools),
`src/player/sway.c` (the global sway), `src/player/weapon_sway.c` (the viewmodel sway),
`src/ui/fire_tuner.c` (the tuner).
Data: `assets/weapons/fire_poses/<class>.json`, the weapons' `"fire_anim"` block
(`data/weapons/*.json`), the attachments' `"fire"` block (`assets/attachments/*.json`),
`assets/weapons/sway_global.json`, `assets/weapons/viewmodel_sway.json`.

The fire animation is cosmetic. Aim recoil (`RecoilState`, docs/RECOIL.md) is unchanged and
still moves the shots; the reticle-anchored ADS rule still holds.

## Why the gun used to not move

`wpn_kick_scale` was already 1 by default: config migration 2 reset an archived 0. The weak feel
came from how the kick was built:

* `recoil_visual_kick` added **velocity** to an item spring. The kick eased in over ~5 ticks and
  returned in about the same time, with ~5 % overshoot. Equal kick and return times read as weightless.
* The camera kick turned the camera, and the item is a child of the camera. The view moved, the
  gun stayed put on screen, and the arms re-bent with the kicked view pitch: "the hands move, not the gun".
* The hold point read the kick of the previous frame, and the item was lerped across the tick.
* The flash spawned at the un-kicked muzzle, and motion blur kept the viewmodel sharp.

## Principles: weight and impact

* **Asymmetric timing.** The kick is a displacement on the shot's own tick, with no ease-in. An
  under-damped spring (damping ratio 0.33–0.5) returns it, overshoots past rest and settles
  10–60× slower than the kick. On the shot's tick the item skips render interpolation, so the
  kick, the flash and the sound land on the same frame.
* **Mass.** Every channel integrates `F / m`:
  * translations use the mass;
  * rotations use the inertia `m · r_gyr² + Σ m_att · d²`, where `r_gyr` grows with `moment`
    (front-heaviness) and the length.
  * Stiffness is set at the class's reference mass. A heavier gun peaks at
    `peak · sqrt(m_ref / m)` and settles in `∝ sqrt(m / k)`. Damping keeps the damping ratio, so
    every class keeps its overshoot.
* **Chain with lag.** The gun moves first. The hands trail its one-tick jump and close it in 1–2
  ticks (≤ 1 cm), then stay glued under 2 mm. The shoulders take the push. The camera follows
  the body 2–3 ticks later with its own overshoot, so it never leads the gun.
* **Perceptual punch.** On the shot tick:
  * an FOV punch that releases over ~7 ticks;
  * a 2-tick roll twitch;
  * a screen kick of the whole rig toward the camera;
  * a 1–2 tick motion-blur spike on the viewmodel;
  * a chromatic / vignette tick on heavy guns.

  These are off at Engine Fidelity Low and Off, and at 0.6 at Medium. The player's own scale is
  `cl_fire_punch`.
* **Sustained fire builds.** Each shot of a string moves the springs' rest point (`ride`,
  saturating at `ride_max` × the kick), so the gun rides up and back while the hands fight it.
  Releasing the trigger plays the release-settle curve and returns the ride, and the spring
  overshoots it.

## Layer order (per tick)

After hold point placement, global sway and ADS alignment, and before the hand IK:

| # | Layer | Output | Where it is applied |
|---|---|---|---|
| 1 | Fire pose curves (first / sustained / release, cropped at the next shot) | gun pos / rot | `holdpoint_eval` kick input |
| 2 | Mass springs: pitch, yaw, roll, kickback, vertical | gun pos / rot | same; hip about the pivot, ADS about the sight (forward-only translation) |
| 3 | Hands + body: hand error, shoulder | `hand_pos/rot`, `shoulder` (`trigger` is computed but drives nothing: a gripping hand's fingers never move, docs/HANDPOSE.md "No finger animation") | `proc_hold` (weapon_hold.c `fire_hand_lag`: the hands trail the kick) |
| 4 | Mechanical: slide / bolt / pump, wobble, eject cue | `slide`, `wobble_rot` | `hands.c draw_weapon` (`"mech"` parts, attachment slots); casing in `game_fire_tick` |
| 5 | Camera: delayed spring (pitch / yaw / roll: the view kicks, the crosshair stays on the true aim point, docs/AIM.md), FOV punch, push-back, screen kick, blur, chroma | `cam_*`, `fov_punch`, ... | `game_render.c` camera, `game_post.c` (`mb_viewmodel`, chromatic / vignette) |
| 6 | Sync | flash on the kicked muzzle (`game_fire_flush_flash`), sound on the shot tick | weapons.c `weapons_set_local_flash` |

Two instances (docs/CAMERA_HOLDPOINT.md "Rule 1"):
- **Every body's** (`CharacterInst.tp_fire`, the shooter's own included): layers 1 to 4 at
  `wpn_fire_anim_scale`, with no camera or punches, the seed from the shot count. The viewed /
  predicting player's shots trigger it in `chars_notify_attack`, everyone else's through
  `AEV_FIRE`. Its gun kick and hand lag go into the hold point and `proc_hold` alike on
  every machine, so the shooter's body is the pose everyone sees.
- **The viewed player's first-person one** (`game_fire.c`, with the shot's exact sub-tick time
  `ShotEvent.sub_tick` and fire `interval` for cropping auto fire): only the camera (layer 5), the
  drawn weapon's mechanics (layer 4: slide, wobble), the screen kick and the arms layer's cosmetic
  Weapon kick; never a bone.

**Field of view.** Layer 5 looks the same at every Field of View (docs/CAMERA_HOLDPOINT.md "Field of
view"): the camera kick's pitch / yaw (authored at a 75° vertical view) scale by `tan(V / 2) /
tan(37.5°)` with the world FOV in use, so the view moves as far on screen (a burst: 14.4 px at Field
of View 80, 15.5 px at 150); the FOV punch is the same zoom at any FOV; roll, the push-back, the
screen kick, blur and chroma don't depend on it. The weapon's ADS counter uses the same scaled kick.

## Parameters

Class file `assets/weapons/fire_poses/<class>.json`. Every key also works in a weapon's
`"fire_anim"` block, which overrides only the keys it sets.

| Key | Meaning |
|---|---|
| `mass`, `moment`, `length` | kg, front-heaviness 0..1, grip to muzzle (m) |
| `class_mass`, `class_moment` | the reference the peaks were authored at (class files only) |
| `springs.<pitch\|yaw\|roll\|back\|up>` | `peak` (deg / m at the reference mass), `random` [min, max], `omega` (rad/s), `zeta`, `ads` (ADS scale), `follow` (momentum), `sign_random`, `ride`, `ride_max` |
| `first_shot` | first shot of a string: × the kickback (shoulder pushback) |
| `hip_scale`, `ads_scale` | overall |
| `curves.first / sustained / release` | keys `[t, pitch, yaw, roll, back, up]` (s, deg, m), stretched by `sqrt(m / m_ref)` × the attachments' `settle_mult` |
| `string_gap`, `ride_release` | s without a shot before a new string, s for the ride to return |
| `body` | `hand_omega`, `hand_zeta`, `hand_max_err`, `shoulder_push / omega / zeta`, `trigger_pull` |
| `mechanical` | `slide_travel`, `slide_time`, `lock_empty`, `bolt_delay` (manual cycle), `wobble_*`, `heat_per_shot`, `heat_decay`, `droop` |
| `camera` | `gain.<channel>`, `delay_ticks`, `omega`, `zeta`, `hip` (the camera's share at the hip, 1 aimed: its kick, roll twitch and FOV punch; the pistol 0: no camera effect at the hip, 2026-10-20), `fov_punch`, `fov_release`, `roll_twitch`, `screen_kick`, `screen_release`, `blur_ticks`, `chroma` |

Attachment `"fire"` block:

| Key | Meaning |
|---|---|
| `mass` | kg at the mount, which is computed from the slot socket (`dist` / `height` override it) |
| `impulse.<channel\|all>` | multipliers on the kicks |
| `omega_mult`, `zeta_add` | sharper (brake) |
| `settle_mult` | longer and heavier ring-out (suppressor) |
| `yaw_lag` | extra yaw inertia (heavy optics) |
| `droop` | deg per unit of sustained-fire heat (front-heavy devices) |
| `fov_punch`, `screen_kick` | multipliers on the perceptual punches |

Model parts take `"mech": "slide" | "bolt" | "pump" | "mag"`. Every gun model has its mechanical
parts tagged (pistol slide and front sight, rifle / SMG / LMG / DMR bolt and magazine, shotgun
pump, sniper bolt handle).

Cvars and commands:

| Name | Effect |
|---|---|
| `wpn_fire_anim_scale` | master scale (1) |
| `wpn_kick_scale` | the gun kick on the hold point (1) |
| `cam_kick_scale` | the camera layer |
| `cl_fire_punch` | the perceptual punches |
| `wpn_fire_debug 1` / `2` | log every shot / every frame |

## Class defaults

Hip, one shot, built-in values. From `sway_check`'s fire-motion listing and `tests/test_fire_anim.c`:

| Class | Mass (kg) | Moment | Peak pitch | Kickback | Settle (ticks) | Overshoot | FOV punch | Character |
|---|---|---|---|---|---|---|---|---|
| Pistol | 0.9 | 0.3 | 6.7° | 3.2 cm | 18 | 19 % | 2° | sharp up, slide flip |
| SMG | 2.8 | 0.35 | 2.0° | 1.9 cm | 16 | 13 % | 2° | fast, small, jittery yaw |
| Rifle | 3.6 | 0.45 | 3.2° | 3.4 cm | 23 | 28 % | 2.5° | up + back, slight roll |
| DMR | 4.5 | 0.5 | 5.7° | 3.8 cm | 27 | 29 % | 3° | one big punch |
| Shotgun | 3.4 | 0.55 | 9.3° | 5.9 cm | 49 | 31 % | 4° | heavy back + up, a pause at the top, pump |
| LMG | 9.0 | 0.6 | 2.3° | 2.8 cm | 39 | 30 % | 2.5° | heavy, slow, builds up |
| Sniper | 6.5 | 0.55 | 11.8° | 7.3 cm | 64 | 31 % | 4° | big, long, shoulder push, bolt |
| Crossbow | 3.0 | 0.4 | 4.1° | 2.3 cm | 25 | 19 % | 1.5° | soft |

ADS:
* rotation impulses × 0.35–0.5, kickback × 0.65–0.7, no vertical translation;
* the pivot is the sight socket;
* the reticle stays within 1 px during a full-strength mag dump (`sight_test recoil`: 0.49 px irons,
  0.83 px red dot, 0.57 px 4x).

## Global sway

Idle sway, breathing, look lag, movement bob, the landing dip and hold breath use **one profile
for every item**: `assets/weapons/sway_global.json` (`src/player/sway.c`, the `sway_global()`
profile).

* Weapons, their `mass` / `moment` and their attachments never reach `sway_eval`.
* The whole hip sway is x `hip_scale` (0.5) and the whole ADS sway x `ads_scale` (1/3): the
  sway as it was before the fire animation, dimmed at the user's request (2026-10-19). Its springs
  are sub-stepped at 120 Hz again, as they were then.
* The only modifiers are global: the hip / ADS multipliers, the optic's own `sway_scale`
  (a property of the optic, the same on every gun), stance, low health, after-sprint wobble and
  hold breath.
* Removed:
  * the per-style `look_lag` / `sway` / `bob` blocks of `holdpoint.json`;
  * the per-weapon `recoil.sway.hip/ads`, `idle_sway` and `scope_sway` (ignored with a warning);
  * the heavy stock's and vertical grip's `sway_scale`.
* The ADS raise / lower blend shape is the hold point's (global). Its duration is the weapon's
  ADS time, so the gun and the zoom finish together.

In first person the viewmodel sway (below) replaces this sway's look lag and bob on the drawn weapon;
the body keeps them, and breathing and the landing dip stay as they are.

## Viewmodel sway

An MWII-style procedural motion of the first-person weapon (`src/player/weapon_sway.c`, pure): layers
composed into one camera-space offset that the arms layer applies to the drawn arms + item about the
pivot (the sight aimed). It is client-side and cosmetic: it is never networked, never changes the body
pose, the aim or the camera, and third-person weapons never see it. It has no recoil part: the kick is
the fire animation's (no layer reads `fire_pulse`).

| Layer | Input | Motion | ADS (rot / pos) |
|---|---|---|---|
| look | the view's rotation rate (deg/s), 20 Hz filter | yaw / pitch trail the turn, roll into it, drag against it; clamped (5 deg, 25 mm); spring k 160, damping ratio 0.68 (slight overshoot, then it settles) | 0.2 / 0.1 |
| move | camera-relative velocity / walk speed | roll into the strafe (2.5 deg), shift against it (12 mm); moving forward: back 7 mm, muzzle down 0.8 deg; spring k 110, ratio 0.72; a sudden start / stop (> 6 m/s^2) kicks the spring with the velocity change | 0.2 / 0.2 |
| bob | ground distance | figure-eight: side sway + two dips per cycle (the steps), roll / pitch / yaw; 0.3 cycles per metre; amplitude by speed / walk speed (crouch 0.5, sprint 1.8 with a longer stride and more roll / yaw); in the air or standing the phase freezes and the amplitude decays (0.25 s); the sprint pose blends in over 0.18 s and out over 0.08 s, eased; aimed a small residual at the same frequency | 0.12 / 0.12 |
| air | jump / airborne vertical speed / landing impact / crouch edges | jump: 0.35 m/s down into the spring (~2 cm dip) and 50 deg/s nose up; airborne: floats up 2.5 mm and noses up 0.4 deg per m/s of falling (25 mm / 3 deg max); landing: (impact - 2.5 m/s) x 0.16 m/s into the spring, max 1.2 (a 3 m drop: ~4-5 cm dip, ~6-7 deg muzzle down; a normal jump ~1.5 cm), 140 deg/s of pitch per m/s, crouched x0.5; the heavy spring (k 150, ratio 0.5) bounces once; crouching dips 0.3 m/s, standing up lifts 0.15 m/s (`weapon_sway_stance_change`, the stance hook) | 0.35 / 0.3 |
| idle | standing still | breathing: two sines at 0.35 / 0.9 Hz (pitch; yaw at half f1: a slow figure-eight) + slow value noise (0.15 deg at 0.25 Hz) so it never repeats visibly, 0.4 deg / 0.8 mm; fades out when moving or in the air (0.2 s) and back in when still (0.5 s); crouched x0.7; holding the breath x0.1 (eased over 0.2 s) | 0.25 / 0.25 |

* **ADS transition**: the multipliers follow the ADS weight eased with a smoothstep (`ads_transition.ease`);
  entering / leaving ADS kicks the look spring (20 deg/s of pitch and 4 cm/s back per unit of ADS
  change) so the gun settles into the sight instead of sliding. The ADS / FOV model stays authoritative:
  `ads_transition.fov_delta` (0) adds to the arms FOV x the ADS weight.
* **Composition**: `weight` (0 light .. 0.5 as written .. 1 heavy) scales every spring's stiffness (x1.25 ..
  x0.75) and every amplitude (x0.8 .. x1.2) from one value (pistol 0.25, SMG 0.35, rifle 0.5, shotgun
  0.55, DMR 0.6, sniper 0.75, LMG 0.85). `pivot_offset` (camera space m) moves the rotation pivot at the
  hip (towards the hands / shoulder; aimed the sight stays the pivot). The total is clamped per axis
  (`max_rot` 40 / 40 / 30 deg, `max_pos` 8 cm): the gun never leaves the view. Rotations are summed in
  degrees every frame (pitch, yaw, roll) and turned into one rotation: nothing accumulates.
* **The camera's landing dip** (`weapon_sway_camera_offset`, 25 deg/s per m/s of the landing impulse,
  k 120, ratio 0.6: ~1.3 deg on a 3 m drop) moves the picture, never the aim. It is applied only with
  `cl_viewmodel_land_camera 1`, x the movement shake `cl_move_shake` (0 by default): by default the
  camera is untouched.
* **Footsteps**: `weapon_sway_bob_phase` (the dips at pi / 2 and 3 pi / 2) and `weapon_sway_footstep`
  (the step the last update crossed: 1 left, 2 right) let the footstep audio follow the bob (not wired).
* While a weapon's own run / sprint clip carries the gun (controller `"carry"`), the bob and the
  sprint pose step aside: the clip already moves and lowers it.
* The arms layer takes the hold point's look lag + bob + breathing + landing share out of the drawn
  weapon while it is on (`ArmsLayerIn.lb_*`, src/player/sway.c), so nothing is doubled; the low-health /
  after-sprint wobble and the reticle aim sway stay the global sway's.
* Frame-rate independent: `exp(-k dt)` filters, springs sub-stepped at <= 1/240 s; the look input is a
  rate, the acceleration kick is the velocity change, the jump / landing / stance kicks are velocity
  impulses.

**Config** `assets/weapons/viewmodel_sway.json`, resolved per held item: built-in defaults <
`"default"` < `"classes"` (the weapon's `fire_anim` class: pistols stiffer and snappier, LMGs and
snipers softer and heavier, with a deeper sprint pose) < `"weapons"` (item id). Every block lists only
the keys it changes. Sections: `look`, `move`, `bob`, `sprint_pose`, `air`, `camera`, `idle`,
`ads_transition`, `ads` ([rotation, position] per layer), `layers` (on / off), and `weight`,
`pivot_offset`, `max_rot`, `max_pos`, `scale`. One field table (`weapon_sway_fields`) drives the
parser, the save and the tuner.

**Tuner** `viewmodel_sway_tuner`: the held weapon's resolved config, every value edited live, each
layer on / off with this frame's output, the player's Weapon motion; **save default / class / weapon**
writes the live values into that block (only what differs from the level above, the block replaced;
the source tree + the runtime copy); **reload file** drops unsaved edits. Switching weapons re-resolves
(unsaved edits are lost).

| Command / cvar | |
|---|---|
| `cl_viewmodel_sway` | 1 (default): on; 0: the hold point's own look lag + bob + breathing + landing, as before. |
| `cl_viewmodel_sway_scale` | Settings > Controls > Mouse, "Weapon motion" (0..1, archived): scales the whole output (accessibility; 0 = still). |
| `cl_viewmodel_land_camera` | 0 (default): the camera never dips; 1: the landing camera dip x `cl_move_shake`. |
| `viewmodel_sway_tuner` | The tuner panel. |
| `viewmodel_sway_info` | This frame's total, each layer, the idle / ADS weights, the camera dip (raw / applied), the footstep, the crouch. |
| `viewmodel_sway_debug 1` | Logs it every 30 frames. |
| `viewmodel_sway_reload` | Re-reads the config. |

Tests: `tests/test_weapon_sway.c` (ctest `weapon_sway`): the same runs at 30 / 60 / 144 Hz (look / move
/ bob within 0.18 deg / 0.7 mm; jump, fall, landing, crouch and breathing within 0.05 deg / 0.3 mm), the
look trail, overshoot and settle, the clamps, the ADS multipliers per layer, the strafe roll and the
stop's settle swing, the bob phase following the distance and freezing, its decay without a snap,
crouch, the sprint pose blend times and ease; landing soft vs hard (3 mm vs 5 cm, 7 deg), crouched
x0.5, one bounce, the camera dip (and none without it); jump dip / nose up, the falling float and its
clamp; the stance dips; breathing amplitude hip / ADS / held breath / crouched, not periodic at f1,
its fade out / in times; the ADS settle and easing, `fov_delta`; weight 0.5 = the config as written,
heavy lags more and settles slower; the final clamp; footsteps alternating, two per cycle; the JSON
diff round trip; config overrides; no NaN. In game, `tools/tests/viewmodel_sway.cfg` (generated by
`gen_viewmodel_sway_cfg.py`) captures rest / turn / overshoot / strafe / walk / sprint / aimed and
the sequences idle / a 3 m drop / a jump / crouch with the system off and on, and
`viewmodel_sway_check.py <log>` checks them, including that the applied camera dip stays 0 by default.

## Global sway checks

`sway_check` runs the real hold point evaluation with every weapon's effective sockets and kick
parameters. It covers every weapon × attachment combination for 600 ticks each of idle, walk and
ADS, and asserts that the sway outputs are bit-identical (compared per optic). It then lists the
fire motion per weapon, which must differ. Last run: 8 weapons, 59 combinations, PASS.

## Tools

| Command | What it does |
|---|---|
| `fire_tuner` | Pick a gun; edit mass, springs, curve amplitudes, body, camera and mechanical values live. **Single / Burst / Mag dump** loops with a slow-motion slider (`fire_loop`). Graphs of gun pitch vs hand pitch, kickback, camera pitch and FOV punch, with the shots (audio transient + flash) marked. **A/B** (store A, swap). **Save class** writes `assets/weapons/fire_poses/<class>.json` to the source tree and the runtime copy. |
| `fire_loop <single\|burst\|magdump\|off> [timescale]` | The tuner's loops from the console or a script. The trigger is fed straight into the user command. |
| `fire_compare` | Mag-dumps every gun in turn and logs peak, the lowest pitch during the dump (no reset) and the release settle. |
| `fire_bench` | Every gun × muzzle device: mass, pitch inertia, peak, kick, settle, overshoot. Gives every gun for the dev-room bench (sighting lane). |
| `sway_check` | See above. |
| `fire_reload` | Re-reads the class files, the weapons and the sway profile. |
| `dev_fixed_fps 60` | Every frame advances exactly one tick, for deterministic captures on slow machines. |

## Tests

`tests/test_fire_anim.c` (ctest `fire_anim`):

* every class: peak within 1 tick, settle ≥ 10 ticks, overshoot;
* heavier → smaller peak and longer settle (monotonic);
* hands lag the gun 1–2 ticks, glue ≤ 1 cm and back under 2 mm within 6 ticks;
* the camera peaks after the gun, and the audio tick is the peak tick;
* 600 / 900 / 1200 RPM: the peak builds over the first 5 shots with no reset;
* FOV punch (released in 5–8 ticks) and a 1–2 tick blur at High, none at Low;
* a suppressor lengthens the settle and a brake shortens the peak, for every class;
* mag dumps and the third-person instance are bit-identical; 600 ticks with no NaN;
* ADS: no vertical translation, smaller rotation; slide locks back on empty and ejects on the return;
* class JSON round trip;
* the global sway is bit-identical for different fire definitions.

In game, `tools/tests/fire_anim.cfg` takes hip / ADS captures around a shot and runs `sway_check`
and `fire_bench`. The reticle lock is checked with `sight_test recoil`.

## Tuning a new weapon

1. Set `"fire_anim": { "class": "<closest class>", "mass": <kg>, "moment": <0..1> }` in its JSON.
   Mass alone already makes it heavier or lighter in the right way.
2. Tag its model's mechanical parts (`"mech"`).
3. Open `fire_tuner`, equip it, and loop **Single** at 0.25 slow motion:
   * shape the first-tick peak with `springs.*.peak`;
   * set the settle and overshoot with `omega` / `zeta`, keeping the damping ratio below 0.5 so it overshoots;
   * check that the hands trail and the camera lags in the graphs.
4. Loop **Mag dump**: shape the climb with `ride` / `ride_max` and `follow`, and the release with
   the release curve.
5. Check ADS (the reticle must stay put: `sight_test recoil`), then **A/B** against the stored set.
6. **Save class** writes a new class file, or put the overrides in the weapon's `fire_anim` block.
   Never touch sway: it is global.
