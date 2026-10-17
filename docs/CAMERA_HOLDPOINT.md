# True first-person camera and hold point

The first-person view is the player's own full body: the same `Dummy.fbx` character, pose and held
item that other players see. There is no separate viewmodel mesh.

- The **camera** sits on the Head bone's eye socket, stabilized.
- Every held item lives on a **hold point**: one transform in camera space. Its position, ADS,
  sway, kick, look lag, bob and wall collision are all defined relative to it.
- **Both arms** are solved with IK onto the item's sockets.

| Area | Code | Data |
|---|---|---|
| Hold point: states, blends, ADS, procedural layer, collision | `src/player/holdpoint.c/.h` (pure, unit tested) | `assets/human/holdpoint.json`, item `"holdpoint"` blocks |
| Camera on the head: stabilizer, collision, death camera | `src/player/camera_fp.c/.h`, `camera_fp_math.c` (pure) | `cam_*` cvars |
| Body solve (spine look → hold point → arm IK → fingers) | `src/anim/weapon_hold.c` (`proc_hold`) | `assets/human/controller.json` hold styles (elbows, curls, left hand) |
| Owner per-frame re-solve, debug draw | `src/gameplay/characters.c` (`chars_render_local_fp`) | |
| Camera integration | `src/game/game_render.c` | |
| Commands, test driver, save | `src/game/game_holdpoint.c` | |
| Editor (F8) | `src/ui/holdpoint_editor.c` | |
| Tests | `tests/test_anim.c` (`test_hold_and_glue`, `test_holdpoint`), `tools/tests/hp_test.cfg`, `cam_sweep.cfg` | |

## Solve order (no feedback loop)

Each simulation tick, for every character, local or remote, through the same code:

1. **Base animation:** locomotion and the upper-body layer.
2. **Additive layers:** damage reactions.
3. **Spine look:** view pitch and yaw spread over spine → neck → head (`proc_spine`).
4. **Bladed stance:** the chest yaws so the support shoulder comes forward. The neck counter-rotates,
   so the head (and the camera on it) still faces the view. The clavicles protract (style `body`).
5. **Camera** = Head bone × eye socket, with the view rotation.
6. **Hold point** (`holdpoint_eval`): item = camera × hold point × anchor⁻¹.
7. **Right-arm IK** onto `grip_r`. The item is finalized from the solved hand, so it's glued by
   construction.
8. **Left-arm IK** onto `grip_l`. If out of reach, the hold point is pulled toward the shoulders first.
9. **Finger curl.**
10. **Foot IK.**

Each rendered frame, for the owner only:

1. **View angles** come from the mouse.
2. **`chars_fp_camera`** bends the interpolated spine the rest of the way to this frame's view and
   reads the head eye.
3. **The stabilizer** turns it into the camera position.
4. **The item** is placed relative to that exact camera (camera × the tick's camera-space item).
5. **Both arms are re-solved** onto its sockets. The hands stay glued (under 0.05 mm) and the item
   never lags the view.

Input never depends on output: angles → spine → head → camera → hold point → arms.

## Camera

| Cvar | Default | Meaning |
|---|---|---|
| `cam_bob_scale` | 0 | how much of the old capsule stabilizer offset (and the landing dip) is added to the head eye; 0 = the camera is exactly on the head |
| `cam_kick_scale` | 0 | visual camera kick on shots (aim recoil is separate and unchanged) |
| `cam_head_follow`, `cam_head_follow_lateral`, `cam_head_stiffness`, `cam_head_damping`, `cam_head_clamp` | 0.8, 0.35, 260, 30, 0.07 | the stabilizer that `cam_bob_scale` blends in |
| `cam_collision_radius` | 0.11 | sphere cast from the capsule to the eye; pulls in instantly, releases smoothly |
| `cam_recon_smooth_time` | 0.1 | prediction-correction smoothing |
| `cam_death_follow`, `cam_death_spectate` | 6, 0 | damped follow of the ragdoll's eye socket; seconds before lifting into a spectator view (0 = stay) |
| `cam_weapon_fov` | 0 | arms + item projection FOV (0 = camera FOV); also the Settings "Hands FOV" |
| `cam_third` (or `cl_thirdperson`) | 0 | third-person debug camera, same body / hold point math |
| `cam_debug` | 0 | 1: draws (head eye red, ideal green, camera white, hold point axes, pivot magenta, sight cyan, muzzle orange, grips yellow, collision cast) and a stats line; 2: also logs it |

**Position = the head eye**: the `Head` bone × the `"camera"` socket of the character
(`assets/characters/player.json`: 0.10 m up, 0.10 m forward in bind space), read after this frame's
spine / head look. `fp_eye_check` verifies it against the mesh: 11 cm below the top of the head and
1.2 cm behind the face surface, i.e. at the eyes. Rotation is always the view angles.

Before October 2026 the camera was "ideal eye (capsule) + clamp(spring(head − ideal) × follow)": the
capsule eye with a 7 cm clamp and only 28 % of the horizontal head motion. The rifle stance and the
walk / sprint lean put the head 10–32 cm in front of the capsule (crouch 27 cm above it), so the camera
sat 8–45 cm behind the head, inside the neck and back, and you saw your own shoulders from behind.
That stabilizer is now an optional offset (`cam_bob_scale`, default 0).

Pitch is clamped to ±88°. Looking up puts most of the pitch into the neck and head
(`spine.weights_up` in `assets/human/controller.json`), so the torso doesn't lean back. The neck, head
(hair, eyes) of the owner's body are collapsed to zero scale in the owner's view only; other players
and the shadow see the full body.

`cam_stats` (with `cam_debug 1`) reports frames where the eye was behind geometry, as seen from the
capsule, or geometry came within 6 cm. `tools/tests/cam_sweep.cfg` mantles, vaults, crawls the crouch
tunnel and faces a wall: 0 / 0.

## Hold point

### States

The state is picked by priority:

**empty > inspect > sprint > ADS > prone > crouch > hip**

- **empty:** no item, or hands busy (mantle, ledge hang, vault).
- **inspect:** reserved, driven via `holdpoint_force_state`.
- **prone:** defined for when a prone movement mode exists. None exists yet.

**Positions** are camera space: `pos` = (right, up, forward) m, `rot` = (pitch, yaw, roll) degrees.
Pitch + is muzzle up, yaw + is muzzle left, roll + is cant left.

**Blends:**
- Switching state, or item (`item_key`), starts an **inertialization blend**. The current offset and
  velocity decay to zero over the state's `blend` time (quintic, Bollo 2018).
- Position and velocity stay continuous even when a blend is interrupted.

**ADS isn't authored, it's computed:** the item is placed so its `sight` socket lands on the view
axis at the style's `eye_relief`, barrel along the view (`holdpoint_ads_item`). Any weapon with a
correct `sight` socket aligns automatically: less than 0.02 mm and 0.001° in the tests.

### Procedural layer

It is scaled down to `ads_scale` in ADS, and every rotation is around the **pivot**:

| Layer | Driven by |
|---|---|
| look lag | rate of change of the view angles (the item trails turns), spring `look_lag` — × `wpn_sway_scale` |
| idle sway + breathing | time (`sway`) — × `wpn_sway_scale` |
| movement bob | camera-space velocity (`bob`) — × `wpn_sway_scale` |
| kick | the recoil system's item spring (`recoil_visual_kick` → `vm_pos` / `vm_rot`) — × `wpn_kick_scale`; aim recoil still drives the view angles |
| soft pitch limit | past `pitch_soft` (65°; none in ADS) the item follows the view pitch only by `pitch_follow`, so arms stay in reach at near-vertical views |

`wpn_sway_scale` (0, replicated: it also scales the aim sway of `weapon_sway_deg`, so bullets and
the crosshair agree) and `wpn_kick_scale` (0) default to zero: **the item is fixed to the camera**
except for the state poses (sprint, ADS, crouch …) and vault / mantle hand placement.

### Pivot

The pivot is an item-space point. The default is between the hands (`grip_r`/`grip_l` midpoint), or
just behind a one-handed grip, i.e. the wrist. Rifles therefore swing at the support hand and pistols
at the wrist. Override it with `"pivot"` in the item's sockets or `holdpoint` block.

### Weapon collision

A cast runs from the hold point along the item's length. When it hits, the pull (0..1, `curve`
exponent, fast in and slow out) moves the item back by `back`, up by `up`, and pitches it by `pitch`
around the pivot. It's cosmetic: shots always use the camera ray.

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
weight to 0, blended by the controller transition. This is `holdpoint_mode: clip`.

- **During the clip:** the arms follow the animation and the item follows the right hand bone, since
  the item is always finalized from the solved right hand.
- **Blending back:** the IK weight rises and the hold point retakes control. The hold point state
  blend is inertialized, so nothing pops.

## Networking

Nothing new goes on the wire. Remote players already replicate aim yaw and pitch, body yaw, weapon,
and ADS / sprint / crouch flags. Each client runs the same hold point and IK code for them, with the
camera at their head bone plus eye socket. `test_holdpoint` checks that an owner pose and a remote
pose solved from the same inputs are **bit-identical**; `first_person` changes nothing. The host's
shots leave the muzzle socket of that pose and converge on the camera ray's aim point (docs/AIM.md):
the shooter claims its muzzle in camera space in the `UserCmd` (`muzzle_cam`), the host checks it
against its own solve of the same pose.

## Tuning and debug

- **`holdpoint_editor` (F8):** pick style and state; preview it (forced state, optional static pose);
  live sliders for state pose, blend, eye relief, procedural, collision, reach and body. Save writes
  `assets/human/holdpoint.json`. The gizmo is the `cam_debug` overlay.
- **`holdpoint_reload` / `holdpoint_save`:** reload or write the JSON.
- **`holdpoint_force_state <n>`:** -1 auto, 0 hip, 1 ADS, 2 sprint, 3 crouch, 4 prone, 5 empty,
  6 inspect.
- **`holdpoint_static 1`:** disables sway, lag, bob and kick.
- **`holdpoint_test`:** equips every weapon through every state. It records the hand ↔ socket error
  every frame, blends included, and the ADS sight error. The report ends with PASS / FAIL: hands under
  2 mm / 1°, sight under 0.5 mm / 0.1°. Currently PASS: hands ≤ 0.03 mm, sight ≤ 0.009 mm.

## Tests

`ctest -R anim` (`tests/test_anim.c`) covers:
- **Hand glue** for every weapon through locomotion, ADS, sprint, crouch, a recoil mag dump, hits,
  turn-in-place, ±89° pitch and ±yaw-limit sweeps: under 0.05 mm.
- **ADS sight alignment** for every gun at five pitches: about 0.01 mm / 0.000°.
- **Owner vs remote** bit-identical poses.
- **Inertialized blends:** interrupted every 83 ms, no velocity kinks.
- **Head stabilizer** through a landing, hit jerk, mantle and stagger: within the clamp, no jumps.

`tools/tests/hp_test.cfg` runs `holdpoint_test` in the game. `tools/tests/cam_sweep.cfg` runs the
camera penetration sweep.

`tools/tests/fp_cam_test.cfg` (generated by `gen_fp_cam_test.py`) drives ~1770 frames of walking,
strafing, sprinting, a 360° turn, crouching, steep looks, jumps, a mantle and every weapon hip + ADS,
and `fp_cam_test report` checks every frame:
- |camera − head bone| < 0.15 m (it is the 0.141 m eye offset);
- the camera is never behind the head along the view yaw, nor behind the pelvis looking level or down;
- both hands on their sockets < 2 mm / 1° while the hold owns them;
- the item on its hold point (camera space) within 10 mm outside sprint / ADS / vault (0.5 mm in
  > 98 % of frames; jump landings reach ~7 mm for a few frames);
- ADS: the sight on the view axis < 0.5 mm / 0.2° for every weapon.
