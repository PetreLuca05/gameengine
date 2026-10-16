# Hand poses and the hand pose editor

Every hand that grips something takes its pose from data: where the palm sits on the item (the
**socket**), a **wrist offset**, the **elbow pole** hint, the **IK weight** and every **finger
joint** (thumb and four fingers, 3 joints each: curl, spread, twist). The data is per **entry**
(a weapon, an item, an attachment grip, a vehicle's controls, an interactable, a hold style),
per **hand** and per **state**. It is edited in a dedicated studio scene, `scene handpose`.

New holdables and interactables register themselves: there is no list to maintain.

| Area | Code | Data |
|---|---|---|
| Data, inheritance, limits, presets, mirror, fingers, contact solver, JSON | `src/anim/handpose.c/.h` (pure) | `assets/handposes/*.json`, `assets/handposes/styles/*.json` |
| Runtime: body hold, inertialized blend, vehicle anchors | `src/anim/weapon_hold.c` (`proc_hold`, `proc_hand_anchors`) | |
| First-person hold + interactions | `src/gameplay/characters.c` (`chars_render_local_fp`), `src/player/hands.c` | |
| Discovery / auto-registration (pure) | `src/editor/handpose/hp_discover.c` | |
| Game side: scans, commands, hot reload, interactable targets | `src/editor/handpose/hp_system.c` | |
| Editor scene, viewport, panels | `src/editor/handpose/hp_editor.c`, `hp_gizmo.c` (pure), `hp_undo.c` (pure) | `data/levels/handpose.json` (studio) |
| Report tool | `tools/handpose_report` | |
| Tests | `tests/test_handpose.c` (ctest `handpose`) | |

## Workflow

1. **Open the editor.** Pause menu → *Hand pose editor*, or the console: `scene handpose`.
   The studio level loads, and the editor opens on the first unreviewed entry.
   `scene devroom` (any map) leaves; `handpose_editor` toggles the panels in the current map.
2. **Pick an entry** in the left list (grouped by category, search box). *New (N)* filters the
   unreviewed ones; *Next unreviewed* walks through them.
   - **Weapons and items:** the character holds the item.
   - **Attachment grips:** the item is mounted on its first host weapon.
   - **Interactables:** the character stands at its station and the hand is held on its target.
   - **Vehicles:** the character sits in the driver's seat.
3. **Pick the hand and the state** on the bottom bar. Only the states the entry uses are shown.
   *default* applies to every state; a state only stores what differs from it.
4. **Edit** on the right panel or in the viewport:
   - **Socket and wrist:** gizmo (W move, E rotate, local / world, snap), numeric fields
     (mm, pitch / yaw / roll degrees), and a nudge keypad (±1 mm, ±1° about the frame's own axes).
     *override here* shows whether this layer sets the field or inherits it.
   - **Fingers:**
     - A clickable hand diagram: orange = selected, blue = overridden here, grey = inherited.
     - Per-joint curl / spread / twist, and a master curl jog.
     - Presets: open, relaxed, fist, trigger finger, pistol grip, rifle grip, foregrip, handlebar,
       flat palm, pinch, point.
     - Joint dragging in the viewport: click a joint marker, drag up / down to curl; drag left /
       right on a base joint to spread.
   - **Contact:** *Solve contact* closes a finger from open until it touches the item (or the
     station's box) and leaves it within 1 mm of the surface. *Pick point* + a click on the item
     sets the finger's contact target, which the solver aims for when nothing is touched. If the
     open finger is already inside the geometry, the hand socket is inside the item: move it out
     first.
   - **Elbow pole, IK weight.**
5. **Check it:**
   - Cameras: *Both hands*, *Grip close-up*, *First person* (the real game camera), *Full body*.
   - Orbit with the right mouse button, pan with the middle one, zoom with the wheel; F reframes.
   - Toggles: hands only, x-ray (the item as a wireframe, the finger bones through it),
     wireframe, socket axes, joint axes, contacts, symmetry (the other hand's socket mirrored),
     sway / bob (the hold point's procedural layer).
   - Pose the body with a clip (scrub or play) or a walk speed.
   - The top readout shows the hand-glue error (left palm ↔ socket, right hand ↔ placement).
6. **Save** (Ctrl+S): writes the entry, marks it `reviewed: true` and applies at once. *Save all*
   saves every entry marked `*`.
   - Other actions: *Revert* (back to the file), *Reset layer* (drop this hand × state's
     overrides), *Copy* / *Paste* (also across entries), *Mirror to other*, *To all states*,
     *Fingers to style* (the hold style's default, so every item of that style without its own
     finger overrides gets them).
   - Undo / redo are unlimited for the session (Ctrl+Z, Ctrl+Y). Unsaved entries are autosaved
     every 20 s to the user directory (`handpose_drafts/`). The next session offers to restore or
     discard them.

Scripts drive the editor with `hpe` (`hpe select rifle`, `hpe state ads`, `hpe hand l`,
`hpe cam grip`, `hpe orbit <yaw> <pitch> <dist>`, `hpe toggle xray 1`, `hpe preset fist`,
`hpe solve all`, `hpe save`, `hpe next`) for screenshots and checks.

## Data format

`assets/handposes/<id>.json`. Only the fields a layer sets are written. Floats use the shortest
text that reads back to the same float, so a save / load round trip is lossless.

```json
{
  "schema": 1,
  "id": "rifle",
  "label": "Assault Rifle",
  "category": "weapon",
  "source": "data/weapons/02_rifle.json",
  "inherits": "style/rifle",
  "hands": ["left", "right"],
  "states": ["default", "hip", "ads", "sprint", "crouch", "prone", "empty", "inspect"],
  "relative": false,
  "reviewed": true,
  "auto_placed": false,
  "left": {
    "default": {
      "socket": { "pos": [0, 0.02, -0.31], "q": [0.1, 0.2, 0.3, 0.9] },
      "wrist": { "pos": [0, 0, 0.004], "q": [0, 0, 0, 1] },
      "pole": [-0.6, -1, 0],
      "ik_weight": 1,
      "fingers": { "index": [[40, 5, 0], [55, 0, 0], [40, 0, 0]] },
      "contacts": [{ "finger": "index", "point": [0.01, -0.01, -0.3] }]
    },
    "ads": { "fingers": { "thumb": [[20, 10, 0], [10, 0, 0], [5, 0, 0]] } }
  },
  "right": {}
}
```

- **Ids:** weapons and items use their id. Attachment grips are `att_<id>`, vehicles `veh_<name>`,
  interactables `use_<name>`, hold styles `style/<name>` (in `styles/<name>.json`). The grip styles
  `style/grip_<grip>` (relaxed, fist, point, grab, pistol, rifle, melee, palm) are the
  first-person interaction grips.
- **States:**
  - `default`;
  - the hold point states `hip ads sprint crouch prone empty inspect vehicle_onehand
    phone_portrait phone_landscape`;
  - `use` and `hold` (interactables);
  - `drive` and `drive_onehand` (vehicle controls).
- **Fields:**
  - `socket`: the palm frame (`-Z` fingers, `+Y` back of the hand) in the entry's space. That is
    the item's space for weapons and items, and the attachment's own space for a grip. For
    `relative` entries (vehicles, interactables), it is an offset from the target the gameplay
    code computes (wheel rim, door knob). `rot: [pitch, yaw, roll]` is accepted instead of `q`.
  - `wrist`: the palm offset relative to the socket, in the palm frame.
  - `pole`: the elbow hint, an aim-space direction like the styles' `elbow_r` / `elbow_l`.
  - `ik_weight`: multiplies the hand IK weight.
  - `fingers`: a finger's three joints `[curl, spread, twist]` in degrees.
    - **Curl** is flexion: positive closes toward the palm.
    - **Spread** is abduction: positive moves toward the thumb. On the thumb, it lifts it off the
      palm.
    - **Twist** is a rotation about the bone.
    - A finger is written whole: setting one joint overrides the finger's three joints.
  - `contacts`: per-finger target points for the contact solver, in the entry's space.
  - `curl: [thumb, index, middle, ring, pinky]` is accepted as shorthand. It's split over the
    joints the way the old curl tables were.
- **Joint limits** are enforced on load, on every edit and when resolving (`handpose_joint_limits`).
  - Fingers: base −20..95, middle −5..110, tip −10..90 curl; the base spreads ±25.
  - Thumb: −30..70 / −15..70 / −20..90 curl; the base spreads −45..60 and twists ±45.
- **Schema:** `schema` is versioned. `handpose_migrate` upgrades older files on load and the next
  save writes the current schema; a newer schema than the build knows is refused. Schema 0
  (unversioned) used top-level `curl_r` / `curl_l`, the controller.json hold style shorthand.

## Inheritance

A pose for (entry, hand, state) is resolved from an open hand with no socket override by applying,
root first, each ancestor's `default` layer and then its layer for the state. The entry's own
layers come last.

- An item stores only what differs from its style.
- A field set on the item's `default` wins over the style's per-state value.
- At runtime the **base is always the hold style in use**, with the entry's chain on top
  (`handpose_resolve_over`). An attachment grip therefore inherits whatever weapon carries it,
  and an interactable inherits its grip style (`grip_grab`, or `grip_point` for buttons).

The style defaults in `assets/handposes/styles` were generated from the previous curl tables
(controller.json `curl_r` / `curl_l` and the first-person grip table): `handpose_gen_styles` writes
any that are missing. Without a file, the same defaults are built in memory. Items without
overrides behave exactly as before; the tests check this bone for bone against the old code.
The controller.json curls are only a fallback now: edit the style files.

## Runtime

- **Body hold** (`proc_hold`, every character, local and remote):
  - Each hand's target is resolved for the current hold point state: the socket in item space
    with the wrist folded in, the pole, the IK weight and the fingers.
  - The right hand is solved onto it and the item is finalized from it. The left hand is solved
    onto its socket, and the fingers come last. Sockets stay the anchors, so the hand-glue
    guarantee is unchanged.
- **Blending:** a change of entry, style or state is inertialized. The offset between the current
  output and the new target, for the socket, pole, IK weight and every finger joint, decays
  quintically over the style blend time. Data edits apply at once.
- **First person:** the owner's per-frame re-solve uses the same resolved sockets and poles.
  Interactions (door knobs, levers, pickups) and vehicle controls resolve their entry (`use_*`,
  `veh_*`) over the grip style. The relative socket offsets the code's target, then the wrist,
  pole and fingers apply. Third-person characters on vehicle controls use the same anchors.
- **Interactables without a hand target in code** (bench, fuel pump, vehicle doors): discovery
  gives them auto-placed sockets on the top of the entity's bounds, facing the user. They only act
  in game once a person has reviewed the entry; the editor previews them regardless.
- **Hot reload:** `handpose_reload` re-reads the files and rescans. Files edited outside the game
  are picked up within a second (`handpose_watch`). While the editor holds unsaved edits, a banner
  asks instead of reloading.

## Auto-registration

Discovery runs at startup (dev builds log what it created), on every map load (placed
interactables), on `handpose_reload`, when the editor opens and on *Scan for new entries*.

- **Sources:**
  - every weapon (`data/weapons/*.json`) and item (`data/items.json`) with a `hold` block;
  - every attachment with a `grip_l` (`assets/attachments`);
  - every `assets/vehicles/*.json` with a driver seat, new files included;
  - every registered interactable (`InteractDef`), placed in a level or not.
- **For each source without an entry**, a stub is created:
  - `inherits`: its style or grip default;
  - its hands and states: inferred from the hold style, the attachment's host weapons, the
    vehicle controls, or the interactable's hand mode and hold time;
  - no overrides, and `reviewed: false`.

  Interactables without a code hand target also get `auto_placed: true` and default sockets.
  Dev builds write the stub to `assets/handposes/<id>.json` in the source tree and the runtime
  copy; release builds keep it in memory. **A stub never overwrites an existing file.**
- **When a source disappears**, its entry is flagged **orphan**. Orphans are listed in the editor
  (*Orphans* filter, *Remove orphans* with a confirmation) and by the report. Nothing is ever
  deleted silently.

**When a new entry shows up:** open `scene handpose`, pick *New*, check the hold in each of its
states (move the socket, set the fingers, solve the contacts) and save. Saving marks it reviewed.
An auto-placed interactable needs its sockets checked before it acts in game.

`tools/handpose_report` prints the unreviewed, auto-placed and orphaned entries.
- `--create` writes the missing stubs.
- `--strict` exits 1 when anything is unreviewed or orphaned.

It discovers weapons, items, attachments and vehicles itself. Interactables are C definitions, so
it reads their entries from disk; the in-game `handpose_report` covers everything.

## Networking

Nothing is replicated: poses derive locally from the replicated item id, hold style and state.
The data must therefore match on every machine. Its hash is the join manifest's `hand_poses`
category (protocol 15), and a mismatch is refused like any other asset category. Only entries
that change a pose count toward the hash, so stubs and the `reviewed` flag don't.

## Tests

`tests/test_handpose.c`:

- Generated style defaults reproduce the old curl tables bone for bone.
- Inheritance and override resolution; cycles are cut.
- The net hash ignores stubs.
- Lossless round trip of 50 random entries; schema migration; a newer schema is refused.
- Mirror (point mirroring, right-handed frames, mirroring twice is the identity, L/R fingertips
  on the rig); joint limits on edit and load; every preset.
- Contact solver: within 1 mm of the surface for every finger of both hands, the same result from
  a curled start, and blocked when the finger starts inside the geometry.
- Hand glue for every weapon and held item, at hip and ADS, with random finger, wrist and socket
  tweaks.
- Discovery against a plain text scan of the assets. A new item and a new interactable produce
  exactly one stub each, with the right hands and states. Rescans create nothing, existing files
  are never overwritten, and removed sources are orphaned, reported and removed only on request.
- Editor-forced states are bit-identical to the gameplay pose: 10 weapons × hip / ADS / sprint /
  crouch, with edited data.
- Gizmo projection and drags; undo / redo; the manifest hash.
