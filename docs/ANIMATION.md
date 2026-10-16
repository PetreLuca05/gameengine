# Skeletal animation, controller, procedural IK and character LODs

Every human-shaped character (players, zombies, dummies) runs on **one canonical skeleton**, taken
from `assets/human/Dummy.fbx`. Clips, controllers, hold styles, ragdolls and hitboxes are authored
once against it and shared.

Related docs: [DAMAGE.md](DAMAGE.md) (hitboxes, body-part damage, ragdolls) and
[ANIMATION_NET.md](ANIMATION_NET.md) (replication, events, lag compensation).

| Area | Code | Data |
|---|---|---|
| FBX import (ufbx), bake cache | `src/assets/fbx_loader.c`, `src/assets/anim_cache.c` | `assets/human/*.fbx` → `assets/cache/*.dgc` |
| Skeleton, clips, pose math, IK | `src/anim/anim.c` | |
| Skinned mesh + LOD generation | `src/anim/anim_mesh.c` (meshoptimizer) | `_LOD<n>` meshes in the FBX, else generated |
| Controller (layers, state machines, blend trees) | `src/anim/controller.c` | `assets/human/controller.json` |
| Anim sets (slot → clip) | `src/anim/controller.c` (`ctrl_bind`) | `assets/human/anim_sets/*.json`, `clips.json` |
| Procedural post-pass | `src/anim/procedural.c`, `turn.c`, `spine.c`, `weapon_hold.c`, `damage_additive.c`, `foot_ik.c` | `controller.json` → `procedural`, `hold_styles`, `damage_reactions` |
| Character definitions + rig validation | `src/assets/character.c` | `assets/characters/*.json` |
| Library (everything above, hashed) | `src/anim/anim_library.c` | |
| Per-character runtime | `src/anim/character_rt.c` | |
| World integration, LOD, ragdolls, rendering | `src/gameplay/characters.c` | |
| GPU skinning | `src/render/render.c`, `shaders/skin*.vert`, `skin.frag` | |
| Debug panel + test stand | `src/ui/anim_debug.c` | |

## Import pipeline

FBX files are read directly with ufbx. There's no conversion step. Each import produces three kinds
of cache entry in `assets/cache/`, named `<stem>.<kind>.<key>.dgc`: the skeleton, the skinned mesh
(all LODs), and the baked clips. The key hashes the file contents, the bake version
(`ANIM_BAKE_VERSION`) and the sample rate, so editing an FBX re-bakes only that file.

* `--rebuild-cache` on the game's command line ignores every cache entry once.
* `anim_rebake` re-bakes changed FBX files in a running game.
* `tools/bake_anim [--rate 30|60] [--rebuild] [files|dirs]` bakes offline and prints one line per clip
  (frames, duration, loop, root travel and speed, turn, foot-plant markers, hash).
* `tools/validate_rig [--canon rig.fbx] [--tolerance 0.25] <fbx|dir>...` checks rigs (see below).

**Space conversion.** Every transform is converted to game space: Y up, −Z forward, metres, using
`C = Ry(180°) · unit scale`. Bones stay at unit scale. `tests/test_anim.c` checks that the character
is 1.80 m tall and faces −Z.

**Clip baking.** Clips are sampled at `anim_sample_rate` (30 or 60 Hz, archived) by evaluating each
frame's node transforms with ufbx and storing local TRS per bone. Root motion is taken from the Hips:
* Loops lose their linear travel and yaw, so they play in place, and the travel becomes `avg_speed`.
* One-shots lose all root travel.
* The per-frame root track is kept for `root_motion` clips. Only the host applies it, to NPCs.

Foot-plant markers are detected from foot heights unless `clips.json` provides them.

**Clip names** are the FBX file stem, or `file:take` for multi-take files. `assets/human/clips.json`
adds per-clip metadata: `loop`, `speed`, `root_motion`, `markers`, `events`, `blend_in`,
`blend_out`, `additive` and `reference`.

### Clips in use

Locomotion and idles come from the takes in `assets/human/Animations.fbx`. `clips.json` gives them
short names (`idle`, `idle_rifle`, `jog_forward`, `jog_backward`, `jog_strafe_left`, `sprint`, ...)
and a `root_speed` for these in-place clips, so playback rate follows the move speed. Strafe right
is `jog_strafe_left@mirror`. `jump` and `Hanging Idle` stay separate files.

**Diagonals.** There are no diagonal clips. On a forward or backward diagonal, the legs (Hips and
below) turn toward the move direction, up to 60°, while the spine counter-rotates so the upper body
keeps facing the aim. The blend tree gets the remaining direction (pure forward or backward). The
leg yaw is smoothed per tick, and so are the blend-tree direction and speed (about 0.15 s), so
changing direction or speed cross-fades the clips instead of popping (`CharRuntime.leg_yaw`,
`blend_dir`, `character_rt.c`). `test_anim` checks the hip angle, chest facing and blend direction
for every diagonal.

**Smooth locomotion.** Speed and move direction go through critically damped springs (no
velocity kinks), and the direction is split continuously into hip yaw plus blend direction:
exactly 45° on a diagonal, untwisting through the strafe band, with no cut-off angles. Every
looping clip's feet are synced to one shared reference (phase 0 = left-foot plant; mirror and
reverse aware), so leaves can join or leave a blend without a phase jump. Blend spaces only use
bound children, so unbound slots never take weight. Loops are made seamless at bake (bake
version 5), and foot locking eases in and out. `test_anim` drives a full accelerate / diagonal /
strafe / back / stop sequence through the runtime and fails on any acceleration spike, and checks
every loop seam.

**First-person camera.** The camera is the capsule eye plus the view's spine bend. It takes only a
filtered, clamped share of the head socket's vertical bob (`cl_camera_socket_follow`, 0.35;
0 = rock steady), so turning and direction changes never move it sideways.

**Sample rate.** Clips are baked at the simulation rate (60 Hz, `anim_sample_rate`), so the
animation advances one baked frame per game tick. `cl_thirdperson 1` shows your own body from
behind, for checking locomotion.

**Facing.** At bake, each clip's start facing is aligned with the canonical bind, and `test_anim`
checks every clip's chest faces forward. Bake version 4.

## Canonical skeleton and rig validation

`skel_finalize` derives bind model transforms, bone lengths, the left/right mirror table, the height,
semantic **roles** (`BR_HIPS`, `BR_SPINE2`, `BR_L_HAND`, …, found by name suffix so `mixamorig:` or
other prefixes don't matter) and `lod_keep`, the reduced bone set used at LOD2+.

Character meshes must use the canonical rig or a compatible one (`rig_validate`):

| Model rig | Result |
|---|---|
| identical | OK |
| subset (missing bones) | OK: missing bones keep the canonical bind |
| extra **leaf** bones | OK: not animated (they keep their bind pose) |
| renamed / re-parented / missing role bones | **FAIL** (the report names the bones) |
| proportions differ by more than the tolerance | warning (animation is retargeted by rotation only) |

Bone names match ignoring the namespace prefix (`mixamorig:`, `mixamorig1:`, `mixamorig8:` … depending
on the Mixamo download). Skinned vertices are **retargeted** at import: each skin cluster's bind pose
in the file is mapped onto the canonical bone's bind pose, so a character with other proportions keeps
its shape on the shared skeleton (identity for `Dummy.fbx`).

## Characters

`assets/characters/<id>.json`:

```json
{ "id": "zombie_brute", "character_id": 3, "kind": "zombie",
  "mesh": "assets/human/Dummy.fbx",
  "materials": { "Alpha_Body_MAT": [0.48, 0.40, 0.34] },
  "anim_set": "zombie",
  "part_mult": { "head": 3.0, "neck": 1.5, "limbs": 0.4 },
  "ragdoll_scale": 1.12, "scale": 1.12, "hp": 260, "move_speed": 0.9 }
```

`character_id` (1–255) is the network id. It's replicated in the animation state and must be unique.
`kind` is `player`, `zombie` or `dummy`. Material colors override the FBX material colors by name.
`lods` and `lod_bias` are optional per-character LOD overrides. Shipped definitions: `player` (1),
`zombie_placeholder` (2), `zombie_brute` (3), `zombie_runner` (4), `target_dummy` (5).

## Skins (`assets/human/skins.json`)

A skin is a cosmetic mesh on the shared skeleton: same animation, hitboxes, ragdoll and network id,
another look. `skins.json` lists them (`id`, `name`, `mesh`, optional texture `atlas` + `tiles`), the
default for players (`"player"`) and one per non-player character type (`"npc"`: armed NPCs `player` →
Officer, `target_dummy` → Athlete); zombies keep their own meshes. The local player picks with
`cl_skin` or **Pause → Skin** (applies immediately, saved). Other players are shown with the default
skin (skins aren't replicated yet).

Textured skins: the FBX textures are extracted with `fbx_textures`
(`build/bin/fbx_textures --out assets/human/textures assets/human/Ch42_nonPBR.fbx`), then
`python tools/gen_skin_atlas.py` puts each skin's material diffuse maps side by side (1024² tiles) into
`assets/human/textures/skin_<id>.png` and registers it in `data/assets.json`. At load every vertex's UV
is remapped into its material's tile, so a skin is one mesh + one texture (`SkinnedDraw.texture`,
batched by mesh and texture). Normal / specular / glossiness maps are extracted but not used yet.

## Anim sets

The controller refers to **slots** (`walk_f`, `jump_start`, `reload_{weapon}`, …). An anim set maps
slots to clips, with optional modifiers:

```json
"walk_b": "walking@reverse",         // play backwards
"walk_l": "right strafe walking@mirror",
"jump_start": "jump@0.0:0.34",       // normalized sub-range
```

Unbound slots are allowed:
* In blend trees they're dropped and the remaining weights renormalize.
* On the base layer, a state whose slots are all unbound falls back to `idle`.
* On upper layers they evaluate as empty.

Binding logs one summary line per set. `anim_template` writes `anim_sets/player_template.json` and
`zombie_template.json`, listing every slot, including the expanded `{weapon}` patterns.

## Controller (`controller.json`)

A deterministic, data-driven state-machine controller. Time is integer ticks. Phases and weights
are pure functions of state, parameters and ticks. There's no RNG: variant picks use the `seed`
parameter, set from events.

* **Parameters.** Standard parameters map from gameplay: `speed`, `move_x`/`move_y`, `grounded`,
  `crouch`, `prone`, `sprint`, `ads`, `mantle`/`mantle_t`, `hang`, `vault`, `vertical_vel`,
  `turn_angle`, `aim_pitch`/`aim_yaw`, `weapon_type`, `weapon_id`, `health`, `limb_status`, `is_dead`,
  `is_attacking`. Triggers: `jump`, `land`, `hit`, `fire`, `reload`, `melee`, `die`, `equip`.
  Payloads: `hit_part`, `hit_dir`, `hit_strength`, `seed`. Custom parameters can be declared too.
* **Layers.** Each layer has a mask (`masks`: bone + weight + recursive), a blend mode (override or
  `additive`), a weight and an optional `weight_param`. Layers can be **muted or soloed** in the debug
  panel. A layer whose current state is `"empty": true` contributes nothing.
* **States.** A state has a `clip` slot or a `tree`, plus:
  * `variants` (alternative clips picked by seed);
  * `loop`;
  * `speed` / `speed_param` / `time_param` (normalized time from a parameter, e.g. mantle progress);
  * `sync`: keeps the normalized phase across transitions between sync states;
  * `speed_match`: playback rate follows `speed` divided by the clip's root speed, so feet don't slide;
  * `hand_ik`: enables the procedural weapon hold in this state.
* **Blend trees.** `blend1d` (thresholds) and `blend2d` (`"mode": "cartesian"` uses inverse-distance
  weights; `"directional"` uses gradient-band weights, good for 8-way locomotion). Trees nest.
  Synchronized children are phase-locked with foot-plant markers.
* **Transitions.** Fields:
  * `from` (state, or `"*"` for any state) and `to`;
  * up to 4 `when` conditions (`[param, op, value]`, or `[trigger, "trigger"]`);
  * `duration`, `exit_time`;
  * `interruptible` (may be interrupted while blending);
  * `allow_self` (an any-state transition may re-enter the current state);
  * `inertialize`.

  Inertialized transitions switch pose immediately and decay the pose difference with a quintic
  (Bollo), instead of cross-fading two poses.

Weapon-dependent clips use `{weapon}` in the slot name. It's resolved per `weapon_type`
(`weapon_types` lists the values).

## Procedural post-pass (render side, every LOD-allowed tick)

The pose from the controller goes through a fixed post-pass (`proc_run`):

1. **Damage additive.** Per-part spring reactions (see DAMAGE.md). They return exactly to zero.
2. **Pelvis.** Foot-IK pelvis drop and slope/acceleration **lean**.
3. **Spine.** Aim pitch and yaw distributed over Spine..Head with `procedural.spine.weights`, within
   `yaw_limit`/`pitch_limit`. Beyond the yaw limit, **turn-in-place** turns the body: procedurally,
   or with turn clips when `turn.mode` is `"clips"`. The body yaw itself is simulated in movement
   (`PlayerMove.body_yaw`, `anim_body_yaw_step`), so it's deterministic and replicated.
4. **Right arm.** Two-bone IK puts the right palm on the item's `grip_r` socket.
5. **The item** is computed **from the solved right hand** (`item = palm_r · grip_r⁻¹`). This is the
   transform that gets rendered.
6. **Left arm.** Two-bone IK with full rotation puts the left palm on `grip_l`. If the grip is out of
   reach, the item slides back along its own barrel axis (or toward the shoulder for melee) by
   exactly the shortfall, up to `max_shift` (0.38 m). The left arm is never stretched. The rare
   failures are logged once per style.
7. **Fingers.** Curl per style. Fingers never move the wrist.
8. **Hand anchors.** Ledge grabs during mantle and hang override the hands.
9. **Leg IK.** Feet are raycast onto the ground, with foot locking.

**Hand glue guarantee.** Both palms are solved after every system that moves the body (reactions,
lean, spine, recoil kick), and nothing moves the hands or the item afterwards. `tests/test_anim.c`
holds every weapon through 600 ticks covering idle, walk, run, strafe-ADS, sprint, crouch, a recoil
mag dump, a hit on every body part and a full turn in place. It then sweeps ±89° pitch across
stance, ADS and sprint, and ±yaw-limit aim offsets. The left palm stays within **2 mm / 1°** of
`grip_l` (measured ≈ 0.03 mm), and the item moves < 0.1 mm after the right-hand solve.
`anim_hold_assert 1` logs any in-game frame over that bound.

**Palm frames** (`HandFrames`, computed from the skeleton): −Z points along the fingers and +Y is the
back of the hand. Item sockets use the same convention.

### Hold styles and item sockets

Weapons (`data/weapons/*.json`) and items (`data/items.json`) have a `hold` block:

```json
"hold": { "style": "rifle", "attach": "RightHand",
  "sockets": { "grip_r": { "pos": [0, 0, 0], "fingers": [0, -0.6, -0.8], "back": [1, 0, 0] },
               "grip_l": { "pos": [0, -0.02, -0.32], "fingers": [1, 0, 0], "back": [0, -1, 0] },
               "muzzle": { "pos": [0, 0.05, -0.62] } } }
```

Item space: the origin is at the right grip, −Z is the barrel's forward direction and +Y is up.
A socket gives a position plus the finger and back-of-hand directions (or `rot` in degrees).

Styles: `none`, `pistol`, `rifle`, `shotgun`, `smg`, `lmg`, `melee_1h`, `melee_2h`, `item_small`,
`item_large`. They live in `controller.json → hold_styles`:
* hip / ADS / sprint / crouch offsets in aim space;
* elbow pole hints, clavicle rotations, finger curls, `max_shift`;
* `aim_pitch_scale`: how much the item follows aim pitch;
* `ads_weight`: 0 for melee, which has no ADS pose.

Style changes blend over `blend_time`.

## Runtime and determinism

`CharRuntime` holds a controller instance, the procedural state, and base and final poses.

* **Host.** Runs the controller and the base pose for every character every tick (no IK, no
  skinning). That produces the per-part hitboxes and the lag-compensation history.
* **Clients.** Run the same controller from replicated parameters. See ANIMATION_NET.md.

The controller is pure integer-tick logic. `tests/test_anim.c` runs two instances for 10 000 ticks
with randomized inputs and requires bit-identical states and phases.

Root motion is host-only and moves NPCs. Players move by their character controller, and their
animation follows the movement (`speed_match`).

## Rendering

* **GPU skinning.** One SSBO holds bone palettes and another holds per-instance data
  (`PIPE_SKIN`, `PIPE_SKIN_SHADOW`, `PIPE_SKIN_WIRE`). Instances of the same mesh and LOD are batched
  into one instanced draw. Vertices carry 4 weights each (44-byte `SkinVertex`).
* **CPU fallback.** `r_cpu_skinning 1` skins on the CPU into dynamic vertex buffers, for debugging
  and drivers without the SSBO path. The output looks the same as GPU skinning.
* **Local player.** The full body is drawn **shadow-only** (`MAT_SHADOW_ONLY`), so first-person views
  keep a body shadow without clipping the camera.
* **Held items.** Rendered at the procedural `item_world` with `hands_draw_held`.

## First-person body (`cl_fp_body`, true first person)

The local player sees their own, undistorted body, Unreal "true first person" style:

* **Camera socket.** Characters have named sockets (bone + offset / rotation, like Unreal skeletal
  mesh sockets) in `assets/characters/*.json` → `"sockets"`. The camera position follows the
  `camera` socket on the `Head` bone; its rotation is the view (pawn control rotation). Each frame
  the spine is bent to that frame's view first, so the head and camera never lag the mouse.
  Stair smoothing and the landing dip are kept. `cl_camera_socket 0` uses the old eye camera, and
  `cl_camera_socket_roll` blends in the head's roll.
* **Same pose everyone sees.** The weapon is the body's procedural hold, with both hands glued, as
  other players see it. The owner gets a raised hip pose (`fp_hip` per hold style, eye space) so
  the weapon sits in view. Only the head is hidden.
* **Static arms.** While an item is held, the arms (clavicles down) don't play the locomotion clip.
  They take a fixed relaxed pose under the hold IK, in third person and on other players too.
* **Interactions.** Door knobs, levers, switches, pickups and ledge grabs are IK'd on top of the
  body pose, blended by weight. While holding an item, the free (left) hand does them and the item
  stays in the right. The target is captured at the use press, so pickups that vanish in the same
  tick still get a reach.
* **Passes.** The world pass draws the body without its arm triangles. The arms are drawn once,
  in the viewmodel pass, with the weapon and its own FOV.
* **FOV.** Camera FOV (`cl_fov`) and hands / weapon FOV (`r_viewmodel_fov`, 0 = follow the camera)
  are separate sliders in Settings → Graphics.
* **Debug.** `cl_fp_debug 1` draws targets; `2` shows the raw body pose. `cl_fp_body 0` brings back
  the cube hands.

## LOD

`lod_select` picks a LOD from the projected height in pixels, with hysteresis 0.15, moving one
boundary per tick. Thresholds are 260 / 120 / 50 px. `r_lod_bias` and per-character `lod_bias`
scale them, and `r_lod_force` overrides the choice.

| LOD | Mesh | Update | Procedural | Ragdoll | Shadow |
|---|---|---|---|---|---|
| 0 | full | every tick | all (foot IK, spine, turn, lean, hold, damage) | yes | LOD1 mesh |
| 1 | LOD1 | every 2nd tick | spine, hold, damage | yes | LOD2 |
| 2 | LOD2 (reduced bone set) | every 4th | baked style arms, item pinned to the right hand | yes | LOD3 |
| 3 | LOD3 | every 8th | baked style arms | no (settled pose) | LOD3 |

* Below 3 px a character is **culled**. Its state machine keeps ticking, but no pose is evaluated.
* LOD switches **cross-fade** with a 0.15 s screen-space dither.
* Shadows use one LOD coarser.
* `r_char_quality` scales the whole table.
* **Budget.** `anim_eval_budget` (64) caps full post-passes per tick. When more characters are due,
  the local player goes first, then the largest on screen, weighted by how overdue they are. The
  rest keep their last pose for a tick. `char_list` prints the last tick's post-passes and deferrals,
  and `tools/tests/anim_budget.cfg` runs the DevRoom with a budget of 4.
* `anim_lod_lineup` (host) spawns a player dummy and a zombie at 5 / 15 / 30 / 60 / 120 m along the
  view. With `r_lod_debug 1` each character shows its LOD, pixel height, update interval and active
  systems. `tools/tests/anim_lod.cfg` runs this on the flat map.

## Hot reload

| Command | Reloads |
|---|---|
| `anim_reload` | anim sets, `clips.json`, `controller.json` (incl. hold styles, damage presets) |
| `anim_rebake` | changed FBX files (by hash) |
| `character_reload` | `assets/characters/*.json` |
| `hitbox_reload` | `assets/human/hitboxes.json` |
| `weapon_reload` | `data/weapons/*.json` (hold sockets, part multipliers) |

Reloading in a live session changes the asset hashes, so the host warns that clients will desync
until they load the same files.

## Debugging

| Cvar / command | What |
|---|---|
| `anim_debug 1/2` | skeleton lines + panel for the character under the crosshair (2 = all characters). The panel shows layers (state, phase, time, cross-fade source, mute/solo), non-default parameters, the last 8 transitions, hand-IK weight, glue error, reach shift, damage-reaction activity, limb status and HP. On clients it also shows host vs local state and phase, resync count and events received. |
| `anim_target` | lock / unlock the panel on the character under the crosshair |
| `anim_solo <layer\|off>`, `anim_mute <layer\|off>`, `anim_force <layer> <state>` | controller debugging on the panel's character |
| `anim_hold_debug 1`, `anim_hold_assert 1` | sockets / palms / muzzle crosses; log glue errors > 2 mm / 1° |
| `hitbox_debug 1` | per-part hitbox capsules + `part xmult` labels (drawn over the mesh) |
| `r_lod_debug 1`, `r_lod_force <n>`, `r_lod_bias` | LOD info / overrides |
| `anim_ragdoll_debug 1` | ragdoll bodies, masses, joint cones / hinge axes |
| `anim_foot_ik`, `anim_spine`, `anim_turn`, `anim_hold`, `anim_lean`, `anim_damage`, `anim_ragdoll` | per-system enables |
| `char_list`, `anim_clips [filter]` | characters (state, LOD, resyncs, events) / baked clips |

### Test stand

`anim_stand` takes over the character under the crosshair (the DevRoom stand at (8.4, 0, 0.5) holds
an LMG). The stand commands are:

| Command | Effect |
|---|---|
| `anim_stand_clip <name\|index\|none>` | play one clip instead of the controller |
| `anim_stand_scrub <0..1>` | pause at a normalized time |
| `anim_stand_play` | toggle playback |
| `anim_stand_rate <x>` | playback rate |
| `anim_stand_mirror` | mirrored playback |
| `anim_stand_item [next\|prev\|id]` | cycle items, and with them hold styles |
| `anim_stand_char <id>` | swap the character definition |
| `anim_stand_hit <part\|all> [damage]` | a real (host) hit on a body part |
| `anim_stand_drop` | toggle a tool ragdoll; the character doesn't die, and a second call stands it up |
| `anim_stand_push [N]` | push the ragdoll along the view |

The DevRoom also has 20 mixed zombies (`zombie_attack`, `zombie_spawn [n] [id] [radius]`,
`zombie_clear`) and four player dummies holding a rifle, pistol, shotgun and bat. `char_spawn <id> x y
z [yaw] [item]` places any character, and `char_kill [damage]` kills the one under the crosshair.

## Tests

* `test_anim` (ctest `anim`) covers:
  * FBX load and space conversion; rig validation (identical / subset / extra / renamed);
  * blend and additive identities; two-bone IK;
  * hold placement within 1 cm, and the glue test above;
  * body-part rays; damage reactions returning to zero;
  * 10 000-tick determinism; quantization round trips; exact lag-comp rewind; LOD hysteresis.
* `test_ragdoll` (ctest `ragdoll`) is covered in DAMAGE.md.
* Scripted checks: `tools/tests/anim_stand.cfg`, `anim_kill.cfg`, `anim_lod.cfg`, `anim_dbg.cfg`,
  and the two-instance `anim_net_host.cfg` + `anim_net_client.cfg`.
