# First-person view: the body, the gliding camera, the arms layer and the hold point

The first-person view is the player's own full body: the same character, pose and held item that
other players see. Three rules shape it:

1. **The model is never deformed for the view.** Beyond the authored animation the owner's body gets
   exactly what every copy of it gets: IK (hands on the held item, ledges, controls, feet) and the
   spine look. Nothing is scaled, stretched or re-posed for the camera; the head is hidden by mesh.
2. **The camera glides.** It lives on the head but ignores the animation's motion: the capsule plus
   the stance's mean head eye, smoothed by a spring. The animated head only validates it.
3. **The arms are a separate render.** The arms (shoulders down) and the held item are drawn a second
   time with their own camera, FOV and near plane. Placement tweaks, ADS and the equip raise move
   that layer rigidly; no bone moves.

| Area | Code | Data |
|---|---|---|
| Frame re-solve (spine look + IK on the tick's stage A), mesh masks, owner shadow | `src/gameplay/characters.c` (`fp_prepare`, `chars_render_local_fp`), `src/anim/procedural.c` (`proc_frame_solve`) | |
| Body solve (stage A → spine look → hold point → arm IK → fingers → anchors → foot IK) | `src/anim/procedural.c`, `src/anim/weapon_hold.c` (`proc_hold`) | `assets/human/controller.json` hold styles |
| Hold point: states, blends, ADS, procedural layer, collision | `src/player/holdpoint.c/.h` (pure, unit tested) | `assets/human/holdpoint.json`, item `"holdpoint"` blocks |
| Gliding camera: nominal eye, glide spring, anchor, death camera, collision | `src/player/camera_fp.c/.h`, `camera_fp_math.c` (pure), `src/anim/eye_nominal.c` (calibration), `src/game/game_render.c` | `assets/player/eye_nominal.json`, `cam_*` cvars |
| Arms layer: camera, rigid placement, ADS presentation, equip raise, seam | `src/render/arms_layer.c/.h` (pure), `src/game/game_arms.c`, `render.c` (`render_set_arms_layer`), `shaders/skin.vert` | `assets/player/arms_layer.json`, `cl_hands_fov` |
| Tuning | `src/ui/arms_tuner.c` (`arms_tuner`), `src/ui/holdpoint_editor.c` (F8) | |
| Tests | `tests/test_anim.c` (`test_frame_solve`, `test_holdpoint`, `test_camera_glide`), `tests/test_arms_layer.c`, `tools/tests/fp_*.cfg` | |

## Rule 1: the body is the pose everyone computes

**Stage A** is the shared animation, run the same way for every character: clips, leg yaw, the
hit-reaction additive, turn in place, seated / kneel / swim posture, pelvis drop, slope lean. After
it only two pass families may write bones:
- **spine look** (`proc_spine`, and the bladed stance that spreads the yaw over the spine and neck);
- **IK** (`proc_hold`: hold point → arm IK onto the item → fingers; `proc_hand_anchors`: ledges,
  controls, interactions; foot IK).

Each tick `proc_run` captures stage A for the viewed player. Each frame `fp_prepare` interpolates the
last two ticks' stage A and re-runs exactly those passes (`proc_frame_solve`) with this frame's view
angles and the interpolated hold point. That's what lets the spine, head and arms follow the mouse
at frame rate. With the tick's own inputs the frame re-solve reproduces the tick pose **bit for bit**:
- `test_frame_solve` checks this for every weapon through locomotion, crouch, recoil (gun kick, hands
  trailing it, trigger pull), hits and a full turn.
- In the game, `anim_fp_parity` (every 30 ticks) checks it in dev builds.
- Each frame also checks that the drawn pose is the re-solve's and that no bone is scaled.
- `fp_rules_report` prints PASS / FAIL.

What the owner's pose used to get, and no longer gets:
- **The head:** the neck and head bones were collapsed to 1e-4 scale. Now the mesh hides them:
  `fpbody` is LOD0 without the arm triangles and without the head / neck triangles (every vertex ≥
  50 % skinned to them). The shadow and phone cameras draw the whole body from the same pose.
- **Driving:** one arm was hidden by collapsing the other clavicle chain. Now each arm has its own
  mesh (`arm_side`).
- **The item's placement:** it was relative to the final render camera (collision push, spectator
  blend, kick) and rotated by the FOV ratio / sway / kick counter. Now it's relative to the **pose
  eye** (head bone × eye socket, the view's rotation), like every remote copy.
- **ADS:** the pose's hold point never aims down the sights, and there's no ADS spine lean: the model
  stays unchanged. A *presentation hold point* (`ProcState.hp_view`) evaluates the same inputs with
  ADS. The arms layer draws the presented item.
- **The fire animation:** one per character, for everyone: gun kick, hands trailing it, trigger pull.
  The owner's first-person instance only moves the camera and the arms layer.
- **The cosmetic Weapon sway:** it no longer scales anyone's pose.

**Sprint** hands the arms to the clip. The sprint leaf of the locomotion blend has `"hand_ik": false`,
so the hand IK weight is the blend weight of the other leaves: it fades out towards a full sprint, the
arms play `sprint_{weapon}` (falls back to `sprint`) and the item follows the right hand. There's no
procedural sprint pose, sprint bob or acceleration lean.

What stays different on remote clients is only data they don't have:
- the interaction reach target (door / pickup hover, not replicated);
- the sway phase where the player isn't simulated.

## Rule 2: the gliding camera

**Position = the nominal eye**: the capsule's feet + an eye offset from `assets/player/eye_nominal.json`.
- The table holds the head eye's **mean over each clip cycle**: the head with all animation-induced
  motion removed.
- It covers every posture (stand, crouch, swim, hang, downed, mantle) × hold style × speed sample
  (mantle: progress) × view pitch (−85 … 85°), in the view's yaw frame.
- `cam_calibrate` measures it from the shared animation (3–4 s). It also writes the animation library's
  hash.
- When the animations change, the game measures its own copy again at startup, and `test_anim` fails
  until `cam_calibrate` updates the source file.

The weights glide through a critically damped spring (`cam_glide_stiffness` 150, `cam_glide_damping`
24.5, about 0.4 s to settle). These are weights for posture (crouch amount, mode), speed and hold
style. The view pitch isn't smoothed: the head turns with the view, so looking down moves the eye
forward at once.

**Vertical:**
- A grounded snap (a step up or down: more than a slope can climb in one frame) is absorbed into an
  offset that glides out.
- Slopes, jumps, falls and lifts follow the capsule exactly.
- A landing adds no dip.

**None of these moves the camera:** walk-cycle bob, landing dips, hit reactions, mantle pose motion,
recoil push, turn in place, idle sway.

**Anchor:**
- Each frame the nominal eye is compared with the animated head eye (head bone × the `camera`
  socket).
- Past `cam_anchor_radius` (8 cm) it's logged; `cam_anchor_report` gives mean / max / per posture.
  The body is never moved.
- `cam_anim_influence` (dev, 0) blends towards the raw head eye for comparison.

**Rotation is the view angles.** Nothing comes from a bone: no head roll, no animation pitch. Two
optional, additive layers sit on top:
- **firing:** "Camera shake" (`cam_kick_scale`): the fire animation's camera kick, roll twitch and FOV
  punch;
- **movement:** "Movement shake" (`cl_move_shake`, 0 = off, the default): head bob, landing dip,
  ledge tilt, mantle nudge.

**Special states:**

| State | Camera |
|---|---|
| Mantle / vault / ledge | the capsule's path + the mantle / hang entries |
| Vehicles | the seat's eye point |
| Downed | the kneel entry |
| Death | the ragdoll head camera (`cam_death_follow`, `cam_death_spectate`): the one exception |

**Collision:** a sphere cast from the capsule keeps the eye `cam_collision_radius` from walls. It
pulls in at once and releases smoothly.

**Spectators** in first person get the same camera: the same formula from the spectated player's
interpolated state and forwarded angles. The glide restarts on a target switch.

| Cvar | Default | Meaning |
|---|---|---|
| `cam_glide_stiffness`, `cam_glide_damping` | 150, 24.5 | the glide spring (stance / speed / style changes, steps) |
| `cam_anchor_radius` | 0.08 | nominal eye ↔ head eye check (m), logged only |
| `cam_anim_influence` | 0 | dev: 1 = the raw head bone eye |
| `cl_move_shake` | 0 | Comfort "Movement shake" |
| `cam_kick_scale` | 1 | Comfort "Camera shake" (firing; 0.25 … 1) |
| `cam_collision_radius` | 0.11 | wall distance (m) |
| `cl_fov`, `sv_fov`, `sv_fov_max`, `sv_fov_max_aspect` | 80, 0, 0, 0 | Field of View: horizontal degrees at 16:9, Hor+ ("Field of view" below) |
| `cam_debug` | 0 | 1: draws the head eye (red), nominal eye (green), camera (white), hold point axes, pivot, sight, muzzle, grips, collision cast + a stats line; 2: also logs |

## Rule 3: the arms layer

The forearms and hands (the `arm` mesh: triangles ≥ 50 % skinned to a forearm chain) and the held
item with its attachments are drawn in the viewmodel pass:
- with their own **camera**, **FOV** and depth buffer, so they never clip walls;
- with the same skeleton instance as the body, so the bones are identical;
- the body draw (`fpbody`) leaves them out, so nothing is drawn twice; it also hides the owner's
  upper arms, like the head (the arms camera has its own position, "Framing").

**The arms camera** is the pose eye: the frame where the hold point lives. A rigid layer transform
in that camera's space places the drawn arms + item, in this order (`src/render/arms_layer.c`):

| Step | What |
|---|---|
| present | the move from the drawn item exactly onto the presented one (the presentation hold point), at the hip as aimed (the sight on the view axis, computed, never tuned). The presentation hold point is screen-locked (no soft pitch limit), so the item and the hands keep their place on screen at any view pitch: the body's reach pull at a steep pitch never shows, and no bone moves for it (the forearms' angle still follows the body's elbows). Faded out while a clip owns the hands (reload, equip, sprint) |
| tuned | `offset` (right, up, forward m), `rotation` (pitch, yaw, roll °) and `scale` (0.9 … 1.1) about the hold point, per hold point state and per item (`items`), blended over the state's `blend`. In ADS only the push along the view axis, the roll and the scale apply, about the sight |
| camera | the arms camera's own position against the eye, per state / item (`camera.position`, right / up / forward m): it moves, never turns; the hip framing ("Framing" below). Aimed it is back on the eye, weighted by the item's own ADS progress |
| kick | the player's cosmetic "Weapon kick" (`wpn_visual_scale`: the pose's kick shown smaller) + the first-person screen kick |
| equip | a newly selected item comes up from below the arms frustum to its hold over `equip.time` (0.3 s: the gun is ready then). It starts `equip.pitch` muzzle down and its item length + `equip.margin` under the bottom edge |
| aim | a rotation about the eye that puts the barrel where the world camera shows the shot: sway, the ADS counter of the camera kick, the arms FOV ≠ world FOV |
| screen | the picture's size on screen ("Screen size" below): after the projection, never in 3D |

**The arms FOV** is Weapon Field of View ("Field of view" below): a preset at the hip, the sight's own
FOV aimed. It changes nothing but the arms projection, and the fidelity levels don't bound it.

**Screen size** (`screen`, 2026-10-20): at the hip the arms + item picture is drawn at `screen.scale`
(0.85; it was 0.75, then 1.125; 0.25 .. 2) of its size about `screen.anchor` (screen fractions
from the top left: 0.75, 1 = on the bottom edge where the guns leave the screen), so a gun keeps its
place and is just bigger or smaller; what leaves the bottom edge still does. It goes back to 1 aimed with the camera weight (the sight's own ADS progress),
so the sight lands on the screen centre at its own size. It is a 2D scale after the arms projection
(`arms_layer_screen_clip`, in `vm_proj`); nothing in 3D moves, the depth is unchanged, and
`render_viewmodel_to_world` / `vm_apparent` apply the same map (`arms_layer_screen_view`), so the
muzzle flash, tracers and shadows stay on the drawn gun. The framing below (the fit, the reference
points) is measured before it: the layout the screen size then scales.

**The shoulder seam** (`seam.on`, off): the body is drawn by the world camera, the arms by the arms
camera. When on, arm vertices within `seam.r0` of an upper-arm joint keep the world camera's screen
position, past `seam.r1` the arms camera's, smoothly between (`skin.vert`, `arms_shoulder` in the
frame data; the depth stays the arms layer's). Off, the arms camera draws only the forearms, hands
and item: with its own position a blend would stretch the arm between the two cameras.

**Everything else follows the arms camera:**
- `render_viewmodel_to_world` maps an arms-layer point to where it appears (muzzle flash, tracers,
  sights).
- `render_arms_point` / `render_arms_dir` map the optic (lens, axes) through the arms camera for the
  sight shaders, which hit the lens with the arms projection. The body's item stays at the hip (ADS
  is never in the model), so the optic taken from it put the lens at the hip: the scope picture and
  the red dot vanished (fixed 2026-10-20).
- `vm_apparent` does the same in the shaders for the world shadows on the arms.
- **Shadows:** the arms and the item cast into the viewmodel tile and, with the whole body, into the
  world cascades from their real transforms.
- **Screen-space effects:** TAA, motion blur, SSR, AO, SSGI and contact shadows skip the arms layer
  (`GB_VIEWMODEL`), as before.

**The tuner** (`arms_tuner`, dev): edit offset / rotation / scale / FOV / blend per state, for every
item or the held one. Also: preview a state (`holdpoint_force_state`), the equip raise, the seam and
near plane, a wireframe of the arms layer (`r_arms_wireframe`), the body / arms toggles
(`cl_fp_layers`), save / reload. Console: `arms_layer_set`, `arms_layer_info`, `arms_layer_save`,
`arms_layer_reload`.

## Field of view

Three settings, after Call of Duty: Modern Warfare II (Settings → Graphics → Comfort). The math is
pure (`src/player/fov.c`, `tests/test_fov.c`); `camera.c` applies it to the world camera,
`game_sights.c` to ADS, sensitivity and the scope picture, `game_arms.c` / `arms_layer.c` to the arms
layer.

| Setting | Cvar | Values | What it changes |
|---|---|---|---|
| Field of View | `cl_fov` | 80 … 150 (default 80), horizontal degrees at 16:9 | the world camera at the hip, never the arms layer |
| ADS Field of View | `cl_ads_fov` | 0 Independent (default), 1 Affected | the world camera while aiming |
| Weapon Field of View | `cl_weapon_fov` | 0 Narrow, 1 Default (default), 2 Wide | the arms layer's projection at the hip, never the world |

**Hor+.** The setting is a horizontal FOV at 16:9; the vertical FOV is the same on every aspect, so
21:9 sees more to the sides and 4:3 less:

    V = 2·atan(tan(H16 / 2) · 9/16)          H(aspect) = 2·atan(tan(V / 2) · aspect)

| Field of View | vertical | 16:9 | 21:9 | 4:3 |
|---|---|---|---|---|
| 80 | 50.5° | 80° | 96.4° | 64.4° |
| 100 | 67.7° | 100° | 113.7° | 83.5° |
| 120 | 88.5° | 120° | 131.4° | 104.3° |
| 150 | 129.1° | 150° | 157.3° | 140.7° |

A host may force one Field of View for everyone (`sv_fov`) or cap it (`sv_fov_max`), in the same units
(0 = off). `sv_fov_max_aspect` (0 = Hor+ on every aspect) can stop wider screens from seeing more.

**ADS Field of View.** The sight's FOV at the default 80 is `H_s = 2·atan(tan(40°) / z)`, where `z` is
the sight's world zoom: a sight's `world_zoom`, a weapon's `ads_world_zoom`, else `cl_ads_world_zoom`
(1.15, the irons and red dots).
- **Independent:** `H_ads = H_s`. Aiming goes back to 80, then the sight's zoom applies: ADS looks the
  same whatever Field of View is.
- **Affected:** `H_ads = H_s · Field of View / 80`.
- Scopes magnify inside their lens against the ADS base: 80 (Independent) or Field of View (Affected)
  (`RenderSight.base_fov`).
- The blend from the hip runs over the weapon's ADS time on the existing curve: the magnification
  `k(t) = 1 + (tan(V_hip/2) / tan(V_ads/2) − 1) · smoothstep(t)`, `V(t) = 2·atan(tan(V_hip/2) / k)`;
  exactly `V_ads` the frame ADS completes, no snap.
- The ADS look sensitivity (`ads_sens_mode`, `ads_sens_coef`) is computed from the resulting FOVs:
  the hip FOV and the aimed world FOV (a scope: its lens FOV against the ADS base).

**Weapon Field of View.** The presets are `weapon_fov` in `assets/player/arms_layer.json` (narrow,
default, wide; hip, horizontal degrees at 16:9; Default comes from the reference framing below,
Narrow −10°, Wide +15°). They change only the arms layer's projection. The layer moves sideways at
the same depth by `tan(V_preset / 2) / tan(V_default / 2)`, so the sight keeps its pixel: Narrow draws
the gun and arms thicker, Wide thinner with more of the arms. Aimed, the arms use the sight's own FOV
(`RENDER_ADS_REF_FOV` / the sight's ADS zoom), the same for every preset, and the sight is on the
view axis, so the reticle stays centred. `cl_hands_fov` (dev, locked) overrides the preset.

**Multipliers.** Sprint widens the current FOV's tangent by `1 + cl_sprint_fov · speed / 6` (0.12 at
full sprint, locked). The fire camera effects look the same at every Field of View (they are authored
at a 75° vertical view, `FOV_PUNCH_REF`): the FOV punch is the zoom it gives there
(`tan(37.5°) / tan((75 − punch) / 2)`) at any FOV, the camera kick's pitch / yaw scale by
`tan(V / 2) / tan(37.5°)` with the world FOV in use (aiming included) so the view moves as far on
screen; roll and the push-back are alike at any FOV (`fov_punch`, `fov_kick_scale`). There is no
vehicle FOV and no bodycam.

**Spectators.** A player's three settings ride in every `UserCmd` (`fov_h16`, `fov_flags`, protocol 36).
A spectator in first person uses the target's (`camera_set_view_prefs`), so it sees the target's view.

**Tests:** `ctest -R fov` (Hor+ at 16:9 / 21:9 / 4:3, Independent = the sight at 80 for any Field of
View, Affected = sight × FOV / 80, the blend's ends / monotony / steepest frame, sensitivity from the
resulting FOVs, the multipliers, the presets and the lateral scale, the command round trip);
`ctest -R arms_layer` (the hip sight keeps its pixel across the presets, aimed it stays centred);
`tools/tests/fov_setting.cfg` + `fov_report` (80 / 120, both ADS modes, the presets, `sv_fov`).

### Framing (the reference)

The hip framing matches a Call of Duty: Modern Warfare II screenshot: world Field of View 80, Weapon
Field of View Default, an iron-sighted rifle at the hip (`docs/fp_framing/reference_mwii_80fov.jpg`,
the streamer's webcam inset blanked). Only the **arms camera** does it: per item, its own position
against the eye (it moves, never turns) and the arms FOV. The item and the hands stay where the body
holds them, the hold points are untouched, no bone moves.

**The reference, measured** (fractions of the width / height from the top left; `assets/player/fp_framing.json`):

| Point | Reference | Ours (rifle) |
|---|---|---|
| Front sight post | 0.563, 0.566 | 0.569, 0.553 |
| Rear sight | 0.688, 0.705 | 0.689, 0.696 |
| Left hand centre | 0.525, 0.79 | 0.586, 0.779 |
| Bore at the bottom edge | 0.80 | 0.786 |
| Sight line | 32.1° above horizontal | 33.8° |
| Front post | upright (± 5°) | upright |
| Visible length (front sight to the bottom edge) | 43 % of the height | 45 % |
| Right hand | off screen bottom right | off screen (0.70, 1.05) |

The reference's sight line passes through the screen centre: its rifle points straight ahead and the
angle is perspective. The camera was pitched up ~15° there; the arms layer is screen-locked, so the
framing is measured with the camera level and holds at any pitch.

**The fit.** `fp_framing_fit` solves the camera's position (three numbers, Levenberg-Marquardt) that
puts the held gun's front sight (the `front_sight` part's top), rear sight, left hand centre (wrist ..
middle knuckle), bore exit, sight line and an upright post on the targets, through the arms camera's
own projection. `fp_framing_sweep` repeats it per arms FOV: the residual is flat between 65 and 90
(1.4 .. 1.6 % of the screen); from 75 up, parts reach the near plane. **Default = 70** (43° vertical),
Narrow 60, Wide 85. The left hand stays ~6 % right of the reference's: our rifle's support grip (an IK
target, never moved) is 14 cm behind its front sight, the reference gun's hand sits under its front
sight, and a camera that only moves cannot change where the hand is on the gun (`tolerance.left_hand`
7 %).

**The other guns** keep the rifle's left hand on screen (the pistol its right hand at 0.63, 0.80) and
its sight height, their camera near the rifle's:

| Gun | Camera (right, up, forward m) | Hand off the target |
|---|---|---|
| rifle | 0.024, −0.042, −0.220 | (the reference) |
| pistol | 0.013, −0.025, −0.306 | right hand −0.014, +0.020 |
| smg | 0.038, −0.061, −0.288 | +0.006, +0.005 |
| lmg | 0.033, −0.013, −0.369 | −0.011, +0.023 |
| shotgun | 0.036, −0.051, −0.221 | −0.001, +0.000 |
| dmr | 0.033, −0.049, −0.163 | +0.010, −0.010 |
| sniper | 0.031, −0.057, −0.109 | +0.013, −0.018 |
| crossbow | 0.043, −0.048, −0.285 | −0.005, +0.009 |

The fit writes the camera to the item's hip / crouch / prone / sprint states (`arms_layer.json`
`items`, `camera.position`). Aimed the camera is back on the eye: its weight follows the item itself
(how far the presented sight has come from its aimed place to its hip place), so hip ↔ ADS moves on
one straight screen path, no overshoot. The ADS state takes the hip's camera (a second, state blend of
it would lag the item).

**What the arms camera draws:** the forearms, hands and the item only (`arm` mesh: triangles ≥ 50 %
skinned to a forearm chain). The body is the world camera's; the owner's upper arms are hidden in the
first-person body like the head (a camera 20 cm behind the eye would see the shoulders from inside, and
an upper arm drawn by the world camera would not meet the forearm). The shoulder seam is off
(`seam.on`).

**Tools:** `fp_framing [tag]` (this frame against the targets: PASS / FAIL, triangles cut by the near
plane on screen, the reach), `fp_framing_fit [fov]`, `fp_framing_sweep lo hi step`,
`fp_framing_overlay [0|1] [alpha]` (the reference over the view with the targets and this frame's
points), `fp_framing_track frames` (per-frame screen points), `fp_framing_capture name` (screenshot +
measurements into `docs/fp_framing/`; `py tools/fp_framing_compare.py name` composes
`name_compare.jpg`: reference / ours / 50 % blend; the raw captures are git-ignored). The arms tuner's "Reference framing" section: the
overlay, its opacity, draggable targets, camera level / the reference's pitch, fit, capture, save the
targets; "Camera" rows per state and item; the Weapon Field of View presets.

**Tests:** `tools/tests/fp_framing.cfg` + `fp_framing_check.py` (the rifle against the reference;
the same placement at Field of View 100 / 120 and 60° down / up; the sight's place across Narrow /
Wide; hip → ADS → hip without overshoot; every other gun's hand, near plane and muzzle);
`ctest -R arms_layer` (the camera moves, never turns, the input untouched, aimed the sight centred).
Comparison snapshots: `docs/fp_framing/rifle_fov80_compare.jpg`, `rifle_fov120_compare.jpg`,
`rifle_fov80_pitch15_compare.jpg`.

## Solve order (no feedback loop)

Each tick, for every character, local or remote, through the same code:

1. **Stage A:** base animation (locomotion, upper body), damage additive, turn, posture, pelvis drop,
   slope lean.
2. **Spine look:** view pitch and yaw spread over spine → neck → head (`proc_spine`).
3. **Bladed stance:** the chest yaws so the support shoulder comes forward; the neck counter-rotates,
   so the head still faces the view.
4. **Pose eye** = head bone × eye socket, with the view rotation.
5. **Hold point** (`holdpoint_eval`, never ADS) and the presentation hold point (players: the same +
   ADS). Item = pose eye × hold point × anchor⁻¹.
6. **Right-arm IK** onto `grip_r`; the item is finalized from the solved hand.
7. **Left-arm IK** onto `grip_l`. If out of reach, the hold point is pulled toward the shoulders first.
8. **Fire layer 3:** the hands trail the gun's kick; **fingers** (grip, trigger pull).
9. **Hand anchors** (ledges, vehicle controls, interactions), **foot IK**.

Each rendered frame, for the viewed player:

1. **View angles** come from the mouse (the camera kick is added afterwards, never to the body).
2. **`chars_fp_camera`:** stage A interpolated, spine look + IK re-run with this frame's view
   (`proc_frame_solve`).
3. **The camera** glides on the nominal eye (+ shake layers, + wall collision).
4. **`chars_render_local_fp`:** the body (masked), the shadow / phone copies, the arms layer's draws.
5. **`game_arms_frame`:** the arms layer's camera and FOV for the renderer.

## Hold point

### States

The state is picked by priority:

**empty > inspect > sprint > ADS > prone > crouch > hip**

The pose's hold point never takes ADS (rule 1): ADS is the presentation hold point's, shown by the
arms layer. Sprint has its state, but the sprint clip owns the arms (hand IK 0) at a full sprint.

- **empty:** no item, or hands busy (mantle, ledge hang, vault). Hanging with a sidearm uses the
  one-handed hip hold instead (docs/ANIMATION.md "Ledge hang").
- **inspect:** reserved, driven via `holdpoint_force_state`.
- **prone:** defined for when a prone movement mode exists. None exists yet.

**Positions** are camera space: `pos` = (right, up, forward) m, `rot` = (pitch, yaw, roll) degrees.
Pitch + is muzzle up, yaw + is muzzle left, roll + is cant left.

**Blends:**
- Switching state, or item (`item_key`), starts an **inertialization blend**. The current offset and
  velocity decay to zero over the state's `blend` time (quintic, Bollo 2018).
- Position and velocity stay continuous even when a blend is interrupted.

**ADS isn't authored, it's computed** (the presentation hold point): the item is placed so its
`sight` socket lands on the view axis at the style's `eye_relief`, barrel along the view
(`holdpoint_ads_item`). Any weapon with a
correct `sight` socket aligns automatically: less than 0.02 mm and 0.001° in the tests.

### Procedural layer (presentation)

It moves the drawn item only (it never moves a bullet: shots leave the camera, docs/AIM.md). It is scaled
down to `ads_scale` in ADS, and every rotation is around the **pivot**:

| Layer | Driven by |
|---|---|
| look lag | rate of change of the view angles (the item trails turns), spring `look_lag` — × `wpn_sway_scale` |
| idle sway + breathing | time (`sway`) — × `wpn_sway_scale` |
| movement bob | camera-space velocity (`bob`) — × `wpn_sway_scale` |
| kick | the recoil system's item spring (`recoil_visual_kick` → `vm_pos` / `vm_rot`) — × `wpn_kick_scale`; aim recoil still drives the view angles |
| soft pitch limit | past `pitch_soft` (65°; none in ADS) the item follows the view pitch only by `pitch_follow`, so arms stay in reach at near-vertical views. The body's pose only: the presentation hold point (`screen_locked`) has none, so the arms layer shows the item in the same place at any pitch |

`wpn_sway_scale` (0, replicated: it also scales the aim sway of `weapon_sway_deg`, so bullets and
the crosshair agree) defaults to zero: **the item is fixed to the pose eye** except for the state
poses, the gun kick and vault / mantle hand placement. The player's cosmetic "Weapon sway" and
"Weapon kick" settings only scale the arms layer, never a pose.

### Pivot

The pivot is an item-space point. The default is between the hands (`grip_r`/`grip_l` midpoint), or
just behind a one-handed grip, i.e. the wrist. Rifles therefore swing at the support hand and pistols
at the wrist. Override it with `"pivot"` in the item's sockets or `holdpoint` block.

### Weapon collision

A cast runs from the hold point along the item's length. When it hits, the pull (0..1, `curve`
exponent, fast in and slow out) moves the item back by `back`, up by `up`, and pitches it by `pitch`
around the pivot. It moves the drawn gun only: the bullets still leave the camera (docs/AIM.md).

### Reach

If a hand can't reach its socket:
1. The **shoulder reaches first**: the clavicle rotates towards the socket (up to 45°), so the item
   stays on its hold point while the walk / jump animation swings the shoulders.
2. Then the hold point slides along the item toward the body, up to `reach_pull` (the sight stays on
   the view axis in ADS).
3. At near-vertical views, it then moves straight toward the support shoulder by the remaining
   shortfall.
4. A warning is logged once per style (the per-frame first-person re-solve does the same, warned once
   per item).

Arms are never stretched and hands never leave their sockets.

### Upper body

- The clavicles get an additive rotation from the hold point's offset from its hip pose
  (`clavicle_gain`), so leaning the weapon turns the shoulders.
- The `body` block defines the bladed stance (`twist`, `clavicle_l`, `clavicle_r`).

## Placing sockets so ADS aligns

Item space: origin = right grip, −Z = barrel forward, +Y = up. In the weapon JSON `hold.sockets`:

```json
"sockets": {
  "grip_r": { "pos": [0, 0, 0], "fingers": [0, -0.5, -0.866], "back": [1, 0, 0] },
  "grip_l": { "pos": [0, 0.02, -0.3], "fingers": [0.75, -0.45, -0.3], "back": [-0.5, -0.85, 0] },
  "sight":  [0, 0.085, -0.04],      // the rear sight / optic centre the eye looks through
  "anchor": "grip_r",               // which socket sits on the hold point (grip_r default)
  "pivot":  [0, 0.01, -0.15],       // optional rotation pivot
  "muzzle": [0, 0.06, -0.62]
}
```

**Defaults:**
- `sight` defaults to the top of the model plus 12 mm, 4 cm behind the grip.
- `anchor` defaults to `grip_r`. It is used as a point; the hold point's rotation orients the item.

**Per-item overrides** go in a top-level `"holdpoint"` block of the weapon or item JSON:
`{ "states": { "hip": { "pos": [...], "rot": [...] } }, "eye_relief": 0.3, "pivot": [...] }`.
Only the fields you set apply.

## Clips taking control

Controller states with `"hand_ik": false` (reload, equip, melee, mantle, death) set the hand IK
weight to 0, blended by the controller transition (the sprint is such a state, entered by
`is_sprinting` only: a speed past the run never blends its arms in). A blend-tree leaf can say the
same (a weapon's own run clip): the weight is then the blend weight of the other leaves. This is
`holdpoint_mode: clip`. Otherwise, with a gun, the upper body plays the weapon's idle (the
controller's gun hold layer, docs/ANIMATION.md "Gun hold"): no locomotion clip moves the arms or the
gun; only the run clips (the sprint, a weapon's run clip) move the hands and the item.

- **During the clip:** the arms follow the animation and the item follows the right hand bone, since
  the item is always finalized from the solved right hand.
- **A weapon's own run clip carries the gun** (controller leaf `"carry": true`, on `run_f_{weapon}`):
  when the set binds the weapon's own clip (`run_f_rifle`: a Rifle Run), that clip owns the arms, the
  gun follows its right hand and the left hand stays IK'd on the gun's grip (`ctrl_carry_weight`,
  `ProcInput.carry_w`), so both hands stay on the gun while it moves with the animation. On screen the
  presentation lets go of the gun and the arms camera glides from the head's eye to the world camera by
  the carry share, so the run's motion shows (nothing holds the gun in place). Without the weapon's own
  clip (the plain `run_f` fallback, the jog) the hold keeps the gun as before. Sprint / reload / melee /
  equip clips keep their own left hand.
- **Blending back:** the IK weight rises and the hold point retakes control. The hold point state
  blend is inertialized, so nothing pops.

## Networking

Nothing new goes on the wire for the pose. Remote players already replicate aim yaw and pitch, body yaw, weapon,
and ADS / sprint / crouch flags. Each client runs the same stage A, spine look, hold point and IK for
them; the owner's frame re-solve runs the same functions (rule 1). The shots leave the first-person
camera (the nominal eye, `UserCmd.eye_cam`, checked by the host: docs/AIM.md).

Spectators in first person reproduce the gliding camera from the forwarded inputs and the arms layer
from the shared `assets/player/arms_layer.json`, with the target's FOV settings (forwarded in its
commands, "Field of view").

## Tuning and debug

- **`arms_tuner`:** the arms layer per state / item, equip raise, seam, wireframe, layer toggles, save.
- **`holdpoint_editor` (F8):** pick style and state; preview it (forced state, optional static pose);
  live sliders for state pose, blend, eye relief, procedural, collision, reach and body. Save writes
  `assets/human/holdpoint.json`. The gizmo is the `cam_debug` overlay.
- **`holdpoint_reload` / `holdpoint_save`:** reload or write the JSON.
- **`holdpoint_force_state <n>`:** -1 auto, 0 hip, 1 ADS, 2 sprint, 3 crouch, 4 prone, 5 empty,
  6 inspect.
- **`holdpoint_static 1`:** disables sway, lag, bob and kick.
- **`holdpoint_test`:** equips every weapon through every state, recording every frame (blends
  included). The report ends with PASS / FAIL:
  - hands ↔ socket error under 2 mm / 1°;
  - sight error (on the presented item) under 0.5 mm / 0.1°.
- **`cam_calibrate`:** measure the nominal eye table again (after animation changes).
- **`cam_anchor_report [reset]`:** the nominal eye against the head eye.
- **`fp_rules_report`:** rule 1 (parity checks, pose edits after spine look / IK, mesh masks).
- **`cl_fp_debug 2`:** the head shown (no mesh mask).
- **`cl_fp_layers`:** body (1) / arms layer (2).
- **`cam_anim_influence 1`:** the camera on the raw head eye, for comparison.

## Tests

`ctest -R anim` (`tests/test_anim.c`) covers:
- **Hand glue** for every weapon through locomotion, ADS, sprint, crouch, a recoil mag dump, hits,
  turn-in-place, ±89° pitch and ±yaw-limit sweeps: under 0.05 mm.
- **ADS:** for every gun at five pitches, the presented sight is on the view axis (about 0.0001 mm),
  and the body's item is where it is at the hip (the model ignores ADS).
- **Owner vs remote:** bit-identical poses (the owner also evaluates the presentation hold point).
- **Frame re-solve:** bit-identical to the tick pose for every weapon (locomotion, crouch, recoil with
  hand lag and trigger, hits, a turn); the passes are spine look + IK only.
- **Sprint vs run:** sprint hand IK weight 0, run 1.
- **Inertialized blends:** interrupted every 83 ms, no velocity kinks.
- **Camera:**
  - the shipped eye table is the animation's (within 5 mm, the same library hash);
  - idle / walk / run in place / crouch / hits never move the camera, while the head eye moves up to
    tens of cm;
  - the head eye stays within the anchor radius except under hits (reported);
  - stand → crouch glides without overshoot or jumps;
  - a 30 cm step-up glides out;
  - slopes and jumps follow the capsule exactly.

`ctest -R arms_layer` (`tests/test_arms_layer.c`):
- the identity layer is the eye's view;
- ADS keeps the sight at the screen centre for any arms FOV, push, roll or scale;
- tuning moves the layer on screen without touching its input;
- the equip raise starts below the view and ends on the hold;
- the aim correction lands the barrel on the shot's pixel;
- Weapon Field of View: the hip sight keeps its pixel across Narrow / Default / Wide, aimed it stays
  centred;
- screen size: the clip matrix and the view map agree on every pixel, the depth is unchanged, the
  anchor and the bottom edge stay put, the scale is the data's at the hip and 1 aimed.

`ctest -R fov`: the FOV model ("Field of view").

In the game:
- `tools/tests/hp_test.cfg` runs `holdpoint_test`.
- `tools/tests/cam_sweep.cfg` runs the camera penetration sweep.
- `tools/tests/fp_cam_test.cfg` (generated by `gen_fp_cam_test.py`) drives ~1770 frames: walking,
  strafing, sprinting, a 360° turn, crouching, steep looks, jumps, a mantle and every weapon hip + ADS
  (in batches of six: `inv_clear` and the next weapons, each drawn with `wield`).
  `fp_cam_test report` checks every frame:
  - the nominal eye within `cam_anchor_radius` of the head eye;
  - the camera never behind the head along the view yaw, nor behind the pelvis looking level or down;
  - both hands on their sockets < 2 mm / 1° while the hold owns them;
  - the item on its hold point (camera space) within 10 mm outside sprint / ADS / vault;
  - ADS: the presented sight on the view axis < 0.5 mm / 0.2° for every weapon.
- `tools/tests/fp_rules.cfg`:
  - `fp_rules_report` PASS;
  - the arms layer's offset changes the screen and not the pose hash;
  - the equip raise;
  - the layer toggles (screenshots).
