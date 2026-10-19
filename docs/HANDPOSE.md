# Hand poses and the hand pose editor

A hand pose says where a hand goes on something it holds and how its fingers close. There are two
kinds of data:

- **Grips** (`assets/handposes/items/<id>.json`): one **universal grip** per weapon, item and
  grip attachment. A grip is the item's **root** alignment plus, per hand, the **hand IK target**,
  the **elbow pole**, the IK weight and the **finger joints**. It is used everywhere the item is
  held: hip, ADS, sprint, crouch, prone, vehicle one-hand, phone, inventory preview, third person,
  remote players and spectators. There are no per-state grips: a state only moves the hold frame
  (hold point placement, ADS from the `sight` socket), never the hands on the item.
- **Cases** (`assets/handposes/cases/<id>.json`): interactions whose hands are not simply holding
  an item (door knob, lever, steering wheel, ledge, reload hand-off, ...). A case has an **anchor**
  (the root of the case: door handle, steering wheel, lever grip) and **keyframed poses** on a
  timeline (approach -> reach -> contact -> use -> release); the runtime blends between the keys
  exactly as the editor shows them.

Hold styles and the first-person grip styles are the defaults every grip / case inherits
(`assets/handposes/styles/<name>.json`). The runtime reads only these files: the old finger curl
tables (`curl_r` / `curl_l` in `controller.json`, the grip curl table of the interaction pass) are
gone; the style files hold the same values (bit for bit, `tests/test_handpose.c`).

What you see in the editor is the game: the editor and every runtime consumer evaluate a grip or a
case with the same code (`src/anim/handpose.c`).

| Area | Code | Data |
|---|---|---|
| Grips, cases, resolution, limits, presets, mirror, contact solver, JSON, migration (pure) | `src/anim/handpose.{c,h}` | `assets/handposes/{items,cases,styles}/*.json` |
| Discovery / stub creation (pure) | `src/editor/handpose/hp_discover.c` | |
| Startup scan, hot reload, commands, saving, runtime case lookup | `src/editor/handpose/hp_system.c` | |
| Control rig: controls, hierarchy, transforms -> data (pure) | `src/editor/handpose/hp_rig.c` | |
| Blender-style modal transforms (pure) | `src/editor/handpose/hp_xform.c` | |
| Field logic + undo / redo (pure; a grip step carries its gun's muzzle through `HpSocketIO`) | `src/editor/handpose/hp_edit.c` | |
| Muzzle socket: the weapon file patch, the number format, the bore / tip helpers (pure) | `src/editor/handpose/hp_muzzle.c` | `data/weapons/*.json` `hold.sockets.muzzle` |
| Editor scene: viewport, gizmo, outliner, N-panel, timeline, the muzzle marker, `hpe` | `src/editor/handpose/hp_editor.c` | |
| Simulated cases: props, seating, ledge, timeline driver | `src/editor/handpose/hp_case.c` | |
| Frozen world | `src/game/game_freeze.c` (+ `weather_freeze`, `g_chars_frozen`) | |
| Weapon / item hold (root, IK, fingers) | `src/anim/weapon_hold.c` | |
| Interactions (first + third person), vehicle controls, ledges | `src/player/hands.c`, `src/gameplay/characters.c`, `src/game/game_vehicles.c` | |
| Migration tool | `tools/handpose_migrate` | |
| Tests | `tests/test_handpose.c`, `tests/test_hp_edit.c`, `tests/test_handpose_rig.c`, `tools/tests/hp_scene.cfg`, `tools/tests/hp_gameplay.cfg`, `tools/tests/hp_muzzle.cfg` + `hp_muzzle_check.py` | |

## Data model

### Grip (schema 2)

```json
{
  "schema": 2, "id": "rifle", "label": "Assault Rifle", "category": "weapon",
  "source": "data/weapons/02_rifle.json", "inherits": "style/rifle",
  "hands": ["left", "right"], "reviewed": false, "auto_placed": false,
  "root":  { "pos": [0, 0, 0], "q": [0, 0, 0, 1], "scale": 1 },
  "left":  { "ik": { "pos": [x, y, z], "q": [x, y, z, w] }, "pole": [x, y, z], "ik_weight": 1,
             "fingers": { "index": [[curl, spread, twist], [..], [..]], ... },
             "contacts": [{ "finger": "index", "point": [x, y, z] }] },
  "right": { ... }
}
```

| Field | Meaning |
|---|---|
| `root` | the item relative to its **hold frame** (the frame the hold point places: hip, sprint, ADS, ...). Position (m), rotation, uniform scale (0.5 .. 2, calibration; the drawn item scales with it). The hold point computes ADS from the root-transformed `sight` socket, so ADS stays automatic. Unset = identity. Only on the item itself (never inherited); an attachment uses its weapon's root. |
| `ik` | the palm target of the hand **in item space** (a child of the root: moving the root carries both hands). Unset = the item's `grip_r` / `grip_l` socket. Attachments: in the attachment's grip frame. A legacy `wrist` offset is folded into it on load. |
| `pole` | elbow direction (camera space, relative to the aim yaw). Unset = the hold style's `elbow_r` / `elbow_l`. |
| `ik_weight` | multiplies the hold's hand IK weight. |
| `fingers` | per finger (thumb: CMC, MCP, IP; fingers: MCP, PIP, DIP) curl / spread / twist degrees, clamped to the joint limits on load, on every edit and when resolving. Set per finger as a whole. `"curl": [5 values]` is a shorthand. |
| `contacts` | fingertip targets on the item (item space) for the contact solver. |

Sparse: a field the grip does not set is inherited from its `inherits` chain (item -> style). The
runtime puts the item's chain on top of the hold style in use (`handpose_resolve_over`), resolved
by `proc_hand_grip` (`src/anim/weapon_hold.c`) - the same call for every state, view and machine.

**Attachment sub-grips** (`att_<id>`, the vertical grip) define the left hand only; mounted, they
override the weapon's left hand (IK target in the attachment's frame, fingers, pole).

### Case (schema 2)

```json
{
  "schema": 2, "id": "door_knob", "label": "Door: knob", "kind": "interact",
  "bind": "interactable:door", "inherits": "style/grip_grab", "hands": ["left", "right"],
  "reviewed": false, "auto_placed": false,
  "setup": { "prop": "door", "distance": 0.42 },
  "anchor": { "pos": [0, 0, 0], "q": [0, 0, 0, 1] },
  "duration": 0.76,
  "markers": { "contact": 0.16, "use": 0.312, "release": 0.54 },
  "keys": [
    { "t": 0.16, "name": "contact", "right": { "ik": {...}, "fingers": {...} } },
    { "t": 0.54, "name": "release", "right": { ... } }
  ]
}
```

| Field | Meaning |
|---|---|
| `kind` | `interact` (interactables), `vehicle` (seats), `item` (an item in a forced hold state: `phone_portrait`, edits the item's grip), `reload` (the clip owns the hands: view only, the hand-off markers are editable), `mantle` (the ledge grab), `stub` (authored, no runtime yet: `ladder_rung`). |
| `bind` | what uses it at runtime: `interactable:<name>`, `interactable:<name>/open` (the variant used while the entity is open: `door_push`), `vehicle:<def>:driver`, `vehicle:passenger`, `item:phone:portrait`, `reload:<weapon>`, `mantle`. A case with keys / a reviewed one wins over a stub bound to the same thing. |
| `setup` | how the editor builds the scene: `prop` to spawn (`door`, `lever`, `container`, `pickup`, `switch`, `bench`, `ledge`, `ladder`, else a stand-in box), `distance` (the prop in front of the character, m), `vehicle` + `seat`, `item`, `hold_state`. |
| `anchor` | the case **root**: an offset from the gameplay target (the knob / lever grip from the interactable's hand target, the passenger hands from the lap, the ledge grip from the ledge anchor). Moving it moves every key's hands with it. **Driver seats**: the anchor moves the steering frame itself (`steer_frame` in `game_vehicles.c`) - the drawn wheel / handlebar and both grips on it - so the hands follow the wheel. |
| `duration`, `markers` | the timeline (s). `contact` = end of the reach (the runtime's reach time), `use`, `release` = start of the return (`duration - release` = the return time). The interaction runtime maps its phases onto it: reach -> [0, contact], hold -> [contact, release] (a held interaction's hold time scaled into it), return -> [release, duration] (`handpose_case_time` / `handpose_case_phase`). Seats evaluate at `use`, the ledge at the traversal progress x duration (hanging: `use`). Reloads: the IK lets go over [0, contact] and takes the hands back over [release, duration] (scaled to the weapon's reload time), only once the case is reviewed. |
| `keys` | keyframed poses (`t` in s), per hand the same sparse fields as a grip (`ik` relative to the anchor). Between keys the pose blends (smoothstep; fingers per joint, IK target lerp / nlerp, pole, IK weight); before the first / after the last key it holds. No keys = the style alone at the anchor. |

Cases auto-register like items: every interactable type, every vehicle driver seat, the passenger
seat, every gun's reload, the phone in portrait, the ledge and the ladder get a stub
(`use_<interactable>`, `vehicle_<def>_driver`, `reload_<weapon>`, ...) unless a case is already
bound to them; stubs show with the unreviewed badge.

### Versioning and migration

Schema 1 (per state layers `"left": { "default": {...}, "hip": {...}, ... }` directly in
`assets/handposes/`) is migrated on load and by `tools/handpose_migrate` (run from the repo root):

- grips: the grip is `default` with `hip` on top (the hip version is kept). Every other state layer
  that differs from it is reported (`ads right: index differs (dropped)`) and dropped;
- interactables (`use_*`) / vehicles (`veh_*`) become cases: one key at the contact marker from
  `default` + `use` / `hold` (`drive`). When an authored case is already bound to the same thing
  (`door_knob` binds `interactable:door`) the old entry is superseded (its key moved over when the
  authored case has none);
- the style defaults that used to be generated from the curl tables were written to `styles/`.

The shipped data had no per-state layers: the migration reported 0 differing layers (58 grips /
styles, 4 cases, 6 entries superseded by authored cases). Once `items/` exists, stale flat files
(a runtime copy) are ignored.

Networking is unchanged: poses are derived locally from the item / case id; every grip / case that
changes a pose (and every reviewed case) is in the hand pose hash of the asset manifest.

## Control rig

The editor shows a **control skeleton** over the character for both arms. Hierarchy (outliner):

```
Root (item root / case anchor)          translate / rotate / scale (scale: grips only, uniform)
|- IK_hand_L                            translate / rotate   (moves the whole left arm via IK)
|  |- L_thumb_1-2-3, L_index_1-2-3, L_middle_1-2-3, L_ring_1-2-3, L_pinky_1-2-3   rotate (limits)
|- IK_hand_R                            translate / rotate
|  |- R_... fingers (as left)
|- Pole_L                               translate (elbow direction)
|- Pole_R                               translate
```

- The arms move only through the IK controls (shoulder -> elbow -> wrist solved by the existing
  two-bone IK, the clavicle reaching first); the pole sets the elbow direction. No shoulder /
  elbow / wrist FK.
- Finger bones rotate only, per joint, within the joint limits: rotation X = curl, Y = spread, Z =
  twist (the gizmo's rotation is decomposed onto the joint's axes, `hp_rig_finger_axes`). A
  per-finger **curl** slider (N-panel) drives the three joints together.
- The **root** is the base of the whole alignment: the IK controls are its children, the fingers
  children of the hands. The item mesh is not selectable: it moves only through the root.
- Colours: root orange, left hand blue, right red, poles magenta; selected white (the active one
  brightest), the children of the selection grey, hover yellow. A red line from a palm to its IK
  target means the hand can't reach it (the glue error).

## Editor (`scene handpose`)

`scene handpose` opens the editor in the running game (host, local player); `scene off` / the
**Leave** button closes it. While it is open the world is frozen (below), the local player holds
the selected grip's item (hotbar slot 1, restored on leave), the hold point has no sway, and the
view is an orbit camera (or first person).

### Panels

- **HAND POSES** (left top): Grips / Cases tabs, a category filter (grips), a text filter,
  unreviewed / edited filters. Marks: `*` unreviewed, `+` overrides (grips) / keys (cases), `!`
  edited and not saved. Selecting a case opens its simulated scene.
- **OUTLINER** (left bottom): the rig hierarchy; click to select (Shift extends), whole-finger rows,
  whole-hand buttons. Controls that can't be edited for the target are marked `(-)`. A gun adds a
  **Muzzle (item socket)** row (below: "Muzzle").
- **Toolbar** (top): the tool (Select / Move / Rotate / Scale: the gizmo), the transform
  orientation (Global / Local / Parent / Normal), the pivot (Individual Origins / Median Point /
  Active Element), **mirror** (edit one hand, the other follows mirrored, live), **neutral light**
  (12:30, clear sky; off = the world's light as it was). The status line shows the running
  transform (`Rotate X (Local) 20.00 deg typed: 20`).
- **ITEM / TRANSFORM** (right, the N-panel): the target, the hand glue per used hand (red =
  unreachable: saving is refused), Save / Undo / Redo / Leave, the preview state (grips: the grip is
  the same in every state - the state only places the item), and for the active control:
  Location X / Y / Z, Rotation X / Y / Z (Euler, plus the quaternion; fingers: curl / spread /
  twist with their limits), Scale (the root of a grip). Drag to scrub, double-click to type,
  per-channel locks (loc / rot / scale), reset location / rotation / scale, copy / paste transform,
  the finger curl slider. **Hand**: finger presets, mirror L <-> R, IK weight. **Muzzle** (guns):
  the muzzle socket in mm and its helpers (below). **View**: orbit / first person, focus on the
  control / the root, yaw / pitch / distance / FOV, onion skin.
- **TIMELINE** (bottom, cases): |< < play/pause > >| (step = 1/60 s), loop, speed, store key (I),
  delete the key under the cursor, the scrub slider, a strip with the markers (contact green, use
  yellow, release red), the keys (purple, white = under the cursor) and the cursor, the markers and
  duration as fields. A reload's clip owns the time (view only: play runs the real reload).

### Shortcuts (viewport, when no text field is active)

| Key | |
|---|---|
| `G` / `R` / `S` | move / rotate / scale the selection (modal; the mouse drives it) |
| `X` / `Y` / `Z` | constrain to the axis (in the chosen orientation); again: the other of Global / the chosen one; a third time: free |
| `Shift` + axis | the plane without that axis (move) |
| `Ctrl` (held) | snap: 5 mm, 5 deg, 0.05 |
| `Shift` (held during a transform) | precision: the mouse motion made while it is held counts a tenth (no jump on release); with `Ctrl`: snap 1 mm, 1 deg, 0.01. The status line shows `(precision)` |
| `0-9 . -` `Backspace` | type the value (`-` negates): metres / degrees / factor along the constraint |
| `Enter` / left click | confirm; `Esc` / right click: cancel (the target goes back exactly) |
| gizmo handle drag | a transform constrained to that axis (releasing confirms) |
| `Alt+G` / `Alt+R` / `Alt+S` | clear location / rotation / scale of the selection (IK / pole: back to inherited; the muzzle: `Alt+G` back to the default) |
| click / `Shift`+click | select / extend; click on nothing: deselect; `A` all / none |
| `[` / `]` | select the parent / the children (`Shift`: extend); `;` the siblings; `M` the mirror control |
| `1` `2`/`W` `3`/`E` `4` | tools Select / Move / Rotate / Scale |
| `I` | store a keyframe at the timeline time (cases) |
| `Space`, `Left` / `Right` | play / pause, step the timeline |
| `Ctrl+Z` / `Ctrl+Shift+Z`, `Ctrl+Y` | undo / redo (64 steps; one transform or one drag = one step) |
| `Ctrl+C` / `Ctrl+V` | copy / paste the active control's transform (same kind of control); the muzzle selected: its position |
| `Ctrl+S` | save |
| middle / right drag, `Alt`+left drag, wheel | orbit, zoom |

Transforms respect what a control allows: fingers rotate only, IK controls move + rotate, poles
move, the root moves / rotates / scales (scale on grips only). Selected children of a selected
parent are carried by it (not transformed twice). Every update is computed from the state the
transform started from (no drift), so cancel restores it exactly.

### Editing a case

Selecting a case builds its scene in front of the player with the real systems: an interactable
prop at the use distance (the interaction is driven at the timeline time through the real hand
system: `hands_case_drive`), a vehicle spawned and the player seated in it, a wall to hang from,
the case's item in hand in its forced hold state. The timeline opens on the contact frame. Pause
anywhere and edit: a hand edit at a time without a key adds one there (seeded from the blended
pose, so nothing jumps); the root (anchor) needs no key. Onion skin draws the arms re-solved onto
the neighbouring keys (blue before, orange after).

### Muzzle

A gun's grip (or a case that edits it) also places the gun's **muzzle socket**,
`hold.sockets.muzzle` in its weapon file. It is in item space: the origin is the right grip, x
points right, y up, and -z along the barrel. Shots leave it along the item's -Z (docs/AIM.md), and
the first-person flash is drawn on it. A mounted muzzle device (suppressor, compensator, ...)
replaces it with its own front face; the socket stays where it is under the device.

- **Marker**: a light green ring across the bore and a cross on the socket. A dim line runs back
  through the barrel (is it on the bore?) and 1.5 m ahead along the shot line. Hover is yellow;
  selected is white, with the label. With a device mounted, its front face is a cyan cross with the
  device's name, joined to the socket by a cyan line. The shots leave the cyan cross, so moving the
  socket changes nothing until the device comes off.
- **Select** by clicking the marker (it wins over a rig control under it), the outliner row, or the
  Muzzle tab. The muzzle is always selected alone, never with rig controls.
- **Move** with `G` or the move gizmo, in the item's axes (Local / Parent / Normal = the item,
  Global = the world). `X` / `Y` / `Z` constrain, typed values are metres (`G` `-Z` `0.03` = 3 cm
  forward along the barrel), `Ctrl` snaps (5 mm), `Shift` gives precision. `R` / `S` refuse: the
  muzzle is a point. `Alt+G` puts it back on the default, `Ctrl+C` / `Ctrl+V` copy and paste it.
- **Muzzle tab** (N-panel): the weapon file and whether the value is edited, select / show the
  marker, X / Y / Z in mm (drag to scrub, double-click to type), and these buttons:
  - **to bore**: onto the barrel's centre line, z kept;
  - **to tip**: the centre of the barrel's front face;
  - **default**: what the game uses without a socket, 1 cm in front of the barrel face;
  - **revert to file**, **save muzzle**.

  The tab also shows the barrel tip, the muzzle - tip offset and any mounted device. The barrel is
  the forward-most model part, leaving out the iron sight parts.
- Values are kept at the files' 0.1 mm. A muzzle edit is an undo step of its gun's grip: undo /
  redo restore the socket with the grip. An edited muzzle marks the grip `!` in the list, and the
  tab shows "edited, not saved". A muzzle the editor did not write (re-read by `weapon_reload`)
  drops the mark: the edit is gone.
- The game uses the edit at once. The live weapon definition is edited, and the characters rebuild
  the held gun's sockets from it every tick (`update_item_sockets`), so the next shot, the aim
  claim and the flash use the new value. `hpe muzzle` prints the game's side next to the marker:
  the socket the character holds, the point the shots leave from, and the aim claim (the muzzle in
  the player's view, as the UserCmd carries it) turned back into the world.

### Saving

**Save** writes the canonical file (only the overrides, stable key order) to `assets/handposes`
in the source tree and to the runtime copy, and marks it reviewed. It refuses a grip or an
interaction / seat key whose hand ends more than 5 mm from its IK target (unreachable socket: move
the root or the IK control closer). `handpose_editor_dir <dir>` makes Save write only there (tests).

Save also writes the gun's muzzle when it was edited (**save muzzle** in the Muzzle tab writes the
muzzle alone):

- `hold.sockets.muzzle` in the weapon file is patched in the text, and only that value changes. A
  missing key is added to `hold.sockets` (and a missing `sockets` object to `hold`), indented like
  its neighbours. The file's formatting and line endings are kept. A file without a `hold` block
  is refused.
- It goes to the source tree and the runtime copy, or with `handpose_editor_dir` only to
  `<dir>/data/weapons/<file>`.
- `weapon_reload` reads the saved value back. Muzzles edited and not saved stay in memory until
  `weapon_reload` (`handpose_reload` keeps them); leaving the scene warns about them.

### Frozen world

While the editor is open (`game_world_freeze`): the time of day is paused (and optionally set to
12:30), the weather holds its current values or a clear sky (its transition resumes on leave), the
weather scheduler and surface integration stop, wind gusts (`wind_timescale` 0), VFX time
(`vfx_timescale` 0), zombie / NPC simulation (`g_chars_frozen`, the horde, entity ticks), vehicle
physics and props stop, and the ambient loops are silent. The local player and its animation keep
running; nothing else advances unless the case timeline plays. Leaving restores everything.

### Scripted use (`hpe`)

Every editor action is a command, for tests and bindings:

```
hpe select <grip>          hpe case <id>            hpe ctrl <name> [add]
hpe key <keys...>          hpe mouse <x> <y>        (0..1 of the window)
hpe xform move|rotate|scale [x|y|z|-x|-y|-z] <value>    (begin, constrain, type, confirm)
hpe local loc|rot|scale x [y z]       hpe clear g|r|s
hpe time <s> | play | stop | keyframe
hpe tool <name> | orient <name> | pivot individual|median|active | mirror on|off | onion on|off
hpe light neutral|world | state <hold state> | view orbit|fp|root|ctrl|<yaw> <pitch> <dist>
hpe save | undo | redo | dump | freeze begin|check
hpe muzzle [<x> <y> <z> | select | deselect | bore | tip | default | revert | save | show on|off]
```

`hpe muzzle` with no argument prints the muzzle (item space, edited / selected, the file's value),
the marker in the world, and the game's side (see "Muzzle"). `<x> <y> <z>` set it (item space, m).
With the muzzle selected, `hpe xform move` moves it like `G`.

## Adding a case

1. Write `assets/handposes/cases/<id>.json` with `kind`, `bind`, `inherits` (a grip style), `hands`
   and `setup` (or let discovery create the stub: every interactable type and vehicle seat gets one).
2. A new interaction kind needs its runtime hook: the interaction phases are mapped by
   `handpose_case_time`; seats / ledges evaluate the case through `ProcInput.anchor_case[_t]`
   (`proc_hand_anchors`); a new bind is looked up in `hp_system.c` (`handpose_*_case`).
3. A new editor scene (`setup.prop`) is spawned in `hp_case.c` (`case_spawn_prop`), its base frames
   (the anchor's parent, each hand's target) come from `hp_case_rig_frames`.
4. Open it in `scene handpose`, key the poses, save. Add it to the `must` list of
   `test_discovery` when it ships.

## Tests

- `tests/test_handpose_rig.c` (ctest `handpose_rig`): the rig hierarchy, moving the root moves both
  hands' IK targets by the same transform (grip and case), the root scale, IK moves never move the
  root / the other hand, fingers only rotate and stay within their limits (gizmo and typed values),
  locks, the item is not a control, mirror, undo / redo round trips 28 operations, the modal
  transforms (constraints, second press, plane, typed values, snapping, pivots, cancel, view plane
  grab, screen rotation sign, scale, Shift precision).
- `tests/test_handpose.c` (ctest `handpose`): the style files reproduce the removed curl tables bone
  for bone, inheritance, lossless grip / case round trips, the schema 0 / 1 migration (the hip grip
  kept, differences reported, interactables / vehicles -> cases), the case timeline (phase mapping,
  blending, deterministic scrubbing), the universal grip bit-identical in 8 hold states x 2
  runtimes, hand glue with random grip + root tweaks for every weapon, every held item and every
  case keyframe, discovery (grips + case stubs, never over a file, orphans).
- `tests/test_hp_edit.c` (ctest `hp_edit`): field provenance, seeding, reset, limits, presets,
  mirror, contacts, the root, case keys as targets, undo / redo (coalescing, bounds, reorder, cases,
  a grip step carrying its gun's muzzle). The muzzle: the 0.1 mm rounding and the number format; the
  text patch (in place, inserted, one-line and empty objects, CRLF kept, refusals); the bore / tip
  helpers; every shipped weapon file patched (exactly one line changes) and read back.
- `tools/tests/hp_scene.cfg`: the editor end to end (rig, transforms, finger, root, undo / redo,
  save, the frozen world for 600 ticks: `hpe freeze: PASS`, the door knob keyed at contact /
  release with onion skin, the car with the wheel moved, the ledge, the phone), screenshots
  `t_hp_scene_*.png`. `tools/tests/hp_gameplay.cfg`: hip / ADS / sprint first and third person,
  the car and the motorcycle, `vehicle_test glue`.
- `tools/tests/hp_muzzle.cfg` + `hp_muzzle_check.py`: the rifle's muzzle selected, moved by a typed
  grab (`G -Z 3 cm`), typed exactly, snapped to the tip and the bore, undo / redo, a suppressor
  mounted, saved into `handpose_editor_dir`. The check asserts each value, that one tick after an
  edit the game's socket, its shot origin and the aim claim are on the marker (the device's front
  with the suppressor), that the saved file differs from the shipped one in exactly the muzzle
  line, and the screenshots `t_hp_muzzle_*.png`. Run from `build/bin`:
  `./deadground.exe --test --nosound --log t_hp_muzzle.log --script ../../tools/tests/hp_muzzle.cfg`,
  then `python ../../tools/tests/hp_muzzle_check.py t_hp_muzzle.log t_hp_muzzle`.

## Adding a scene

A scene is a static `GameScene` (`src/game/scene.h`): name, enter / leave, an optional camera
callback (return true to own the camera; the body is then drawn in third person), a Nuklear draw
callback, and whether it needs the cursor and hides the HUD. Register it once at init with
`scene_register`. `scene <name>` opens it and leaves the current one first.
