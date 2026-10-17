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
tables (`curl_r` / `curl_l` in `controller.json`, the grip curl table) are gone.

What you see in the editor is the game: the editor and every runtime consumer evaluate a grip or a
case with the same code (`src/anim/handpose.c`).

| Area | Code | Data |
|---|---|---|
| Grips, cases, resolution, limits, presets, mirror, contact solver, JSON, migration (pure) | `src/anim/handpose.{c,h}` | `assets/handposes/{items,cases,styles}/*.json` |
| Discovery / stub creation (pure) | `src/editor/handpose/hp_discover.c` | |
| Startup scan, hot reload, commands, saving | `src/editor/handpose/hp_system.c` | |
| Control rig: controls, hierarchy, transforms -> data (pure) | `src/editor/handpose/hp_rig.c` | |
| Blender-style modal transforms (pure) | `src/editor/handpose/hp_xform.c` | |
| Field logic + undo / redo (pure) | `src/editor/handpose/hp_edit.c` | |
| Editor scene: viewport, gizmo, outliner, N-panel, timeline | `src/editor/handpose/hp_editor.c` | |
| Simulated cases: props, seating, timeline driver, frozen world | `src/editor/handpose/hp_case.c` | |
| Weapon / item hold (root, IK, fingers) | `src/anim/weapon_hold.c` | |
| Interactions (first person), vehicle controls, ledges | `src/player/hands.c`, `src/gameplay/characters.c`, `src/game/game_vehicles.c` | |
| Migration tool | `tools/handpose_migrate` | |
| Tests | `tests/test_handpose.c`, `tests/test_hp_edit.c`, `tests/test_handpose_rig.c`, `tools/tests/hp_*.cfg` | |

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
| `root` | the item relative to its **hold frame** (the frame the hold point places: hip, sprint, ADS, ...). Position (m), rotation, uniform scale (0.5 .. 2, calibration). The hold point computes ADS from the root-transformed `sight` socket, so ADS stays automatic. Unset = identity. Only on the item itself (never inherited). |
| `ik` | the palm target of the hand **in item space** (a child of the root: moving the root carries both hands). Unset = the item's `grip_r` / `grip_l` socket. Attachments: in the attachment's frame. |
| `pole` | elbow direction (camera space). Unset = the hold style's `elbow_r` / `elbow_l`. |
| `ik_weight` | multiplies the hold's hand IK weight. |
| `fingers` | per finger (thumb: CMC, MCP, IP; fingers: MCP, PIP, DIP) curl / spread / twist degrees, clamped to the joint limits on load, on every edit and when resolving. Set per finger as a whole. |
| `contacts` | fingertip targets on the item (item space) for the contact solver. |

Sparse: a field the grip does not set is inherited from its `inherits` chain (item -> style). The
runtime puts the item's chain on top of the hold style in use (`handpose_resolve_over`).

**Attachment sub-grips** (`att_<id>`, e.g. the vertical grip) define the left hand only; mounted,
they override the weapon's left hand (IK target in the attachment's frame, fingers, pole).

### Case (schema 2)

```json
{
  "schema": 2, "id": "door_knob", "label": "Door: knob", "kind": "interact",
  "bind": "interactable:door", "inherits": "style/grip_grab", "hands": ["right"],
  "reviewed": false,
  "setup": { "prop": "door", "distance": 0.75 },
  "anchor": { "pos": [0, 0, 0], "q": [0, 0, 0, 1] },
  "duration": 0.76,
  "markers": { "contact": 0.16, "use": 0.3, "release": 0.54 },
  "keys": [
    { "t": 0.16, "name": "contact", "right": { "ik": {...}, "fingers": {...} } },
    { "t": 0.54, "name": "release", "right": { ... } }
  ]
}
```

| Field | Meaning |
|---|---|
| `kind` | `interact` (interactables), `vehicle` (seats), `item` (an item in a forced hold state: `phone_portrait`), `reload` (clip owns the hands: view only, hand-off markers editable), `mantle` (ledge grab), `stub` (no runtime yet: `ladder_rung`). |
| `bind` | what uses it at runtime: `interactable:<name>`, `interactable:<name>/open` (the variant used when the entity is open: `door_push`), `vehicle:<def>:driver`, `vehicle:passenger`, `item:<id>`, `reload:<weapon>`, `mantle`. |
| `setup` | how the editor builds the scene: `prop` to spawn, `distance` (character to the prop), `vehicle`, `seat`, `item`, `hold_state`. |
| `anchor` | the case **root**: an offset of the anchor from the gameplay target (door knob / lever grip from the interactable's hand target, steering wheel / handlebar from the vehicle's steering pivot frame, ledge grip from the ledge anchor). Moving it moves every key's hands with it; for vehicles it also moves the drawn wheel. |
| `duration`, `markers` | the timeline (s): `contact` = end of the reach, `use`, `release` = start of the return. The runtime maps its interaction phases onto it: reach -> [0, contact], hold -> [contact, release], return -> [release, duration]. |
| `keys` | keyframed poses (`t` in s), per hand the same sparse fields as a grip (`ik` relative to the anchor). Between keys the pose blends (smoothstep, fingers per joint, IK target lerp / slerp); before the first / after the last key it holds. No keys = the style (the anchor alone). |

Cases auto-register like items: every interactable type and vehicle driver seat without a case
bound to it gets a stub (`use_<interactable>`, `vehicle_<def>_driver`) with `"reviewed": false`.

### Versioning and migration

Schema 1 (per state layers `"left": { "default": {...}, "hip": {...}, ... }`, flat directory) is
migrated on load: the grip is `default` with `hip` on top (the hip version is kept); other states
that differ from it are reported. Interactable (`use_*`) and vehicle (`veh_*`) entries become cases
(their `default` + `use` / `drive` layer is the single key). `tools/handpose_migrate` rewrites the
files into `items/` / `cases/` / `styles/`, writes the style defaults that used to be generated from
the curl tables, and prints the differences.

Networking is unchanged: poses are derived locally from the item / case id; every grip / case that
changes a pose is in the hand pose hash of the asset manifest.

## Control rig (the editor)

The editor shows a **control skeleton** over the character for both arms. Hierarchy (outliner):

```
Root (item root / case anchor)          translate / rotate / scale (uniform)
├─ IK_hand_L                            translate / rotate   (moves the whole left arm via IK)
│  └─ L thumb 1-2-3, index 1-2-3, middle 1-2-3, ring 1-2-3, pinky 1-2-3   rotate (limits)
├─ IK_hand_R                            translate / rotate
│  └─ R fingers (as left)
├─ Pole_L                               translate (elbow direction)
└─ Pole_R                               translate
```

- The arms move only through the IK controls (shoulder -> elbow -> wrist solved by the existing
  IK); the pole sets the elbow direction. No shoulder / elbow / wrist FK.
- Finger bones rotate only, per joint, within the joint limits: X = curl, Y = spread, Z = twist.
  A per-finger **curl** slider drives the three joints together.
- The **root** is the base of the whole alignment: the IK controls are its children, the fingers
  children of the hands. The item mesh is not selectable: it moves only through the root.

## Editor

(See the sections below, filled in with the implementation: shortcuts, transform tool, N-panel,
timeline, frozen world, adding a case.)
