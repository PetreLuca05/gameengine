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
| `cl_fov`, `sv_fov`, `sv_fov_max`, `sv_fov_max_aspect` | 75, 0, 0, 16:9 | the world camera's vertical FOV (Hor+ up to the max aspect); ADS narrows it by the sight's world zoom |
| `cam_debug` | 0 | 1: draws the head eye (red), nominal eye (green), camera (white), hold point axes, pivot, sight, muzzle, grips, collision cast + a stats line; 2: also logs |

## Rule 3: the arms layer

The arms from the upper arms down (the `arm` mesh: triangles ≥ 50 % skinned to the arm chains) and
the held item with its attachments are drawn in the viewmodel pass:
- with their own **camera**, **FOV** and depth buffer, so they never clip walls;
- with the same skeleton instance as the body, so the bones are identical;
- the body draw (`fpbody`) leaves them out, so nothing is drawn twice.

**The arms camera** is the pose eye: the frame where the hold point lives. A rigid layer transform
in that camera's space places the drawn arms + item, in this order (`src/render/arms_layer.c`):

| Step | What |
|---|---|
| present | the move from the drawn item onto the presented one (the presentation hold point). At the hip only the two hold points' difference (identity: the arms stay where the body has them). Aimed, exactly onto it: the sight on the view axis, computed, never tuned. Faded out while a clip owns the hands (reload, equip, sprint) |
| tuned | `offset` (right, up, forward m), `rotation` (pitch, yaw, roll °) and `scale` (0.9 … 1.1) about the hold point, per hold point state and per item (`items`), blended over the state's `blend`. In ADS only the push along the view axis, the roll and the scale apply, about the sight |
| kick | the player's cosmetic "Weapon kick" (`wpn_visual_scale`: the pose's kick shown smaller) + the first-person screen kick |
| equip | a newly selected item comes up from below the arms frustum to its hold over `equip.time` (0.3 s: the gun is ready then). It starts `equip.pitch` muzzle down and its item length + `equip.margin` under the bottom edge |
| aim | a rotation about the eye that puts the barrel where the world camera shows the shot: sway, the ADS counter of the camera kick, Hands FOV ≠ world FOV |

**Hands FOV** (`cl_hands_fov`, Settings → Graphics → Comfort, 50 … 90°, default 65):
- It is vertical, with the same Hor+ rule as the world FOV.
- Aimed, the arms use the sight's own FOV (`RENDER_ADS_REF_FOV` / the sight's ADS zoom), so the sight
  picture is the same for everyone.
- It changes nothing but the arms projection. The fidelity levels don't bound it (like the world FOV).

**The shoulder seam:** the body is drawn by the world camera, the arms by the arms camera. Arm
vertices within `seam.r0` of an upper-arm joint keep the world camera's screen position, past
`seam.r1` the arms camera's, smoothly between (`skin.vert`, `arms_shoulder` in the frame data; the
depth stays the arms layer's).

**Everything else follows the arms camera:**
- `render_viewmodel_to_world` maps an arms-layer point to where it appears (muzzle flash, tracers,
  sights).
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
| soft pitch limit | past `pitch_soft` (65°; none in ADS) the item follows the view pitch only by `pitch_follow`, so arms stay in reach at near-vertical views |

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
weight to 0, blended by the controller transition. A blend-tree leaf can say the same (the sprint
leaf): the weight is then the blend weight of the other leaves. This is `holdpoint_mode: clip`.

- **During the clip:** the arms follow the animation and the item follows the right hand bone, since
  the item is always finalized from the solved right hand.
- **Blending back:** the IK weight rises and the hold point retakes control. The hold point state
  blend is inertialized, so nothing pops.

## Networking

Nothing new goes on the wire. Remote players already replicate aim yaw and pitch, body yaw, weapon,
and ADS / sprint / crouch flags. Each client runs the same stage A, spine look, hold point and IK for
them; the owner's frame re-solve runs the same functions (rule 1). The shots leave the first-person
camera (the nominal eye, `UserCmd.eye_cam`, checked by the host: docs/AIM.md).

Spectators in first person reproduce the gliding camera from the forwarded inputs and the arms layer
from the shared `assets/player/arms_layer.json`.

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
- ADS keeps the sight at the screen centre for any Hands FOV, push, roll or scale;
- tuning moves the layer on screen without touching its input;
- the equip raise starts below the view and ends on the hold;
- the aim correction lands the barrel on the shot's pixel.

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
