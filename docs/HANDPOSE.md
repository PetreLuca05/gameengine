# Hand poses and the hand pose editor

A hand pose says where a hand goes on something it holds and how its fingers close. The data is
kept per **entry** × **hand** (left / right) × **state**. The holdable things are weapons, items,
attachment grips (the vertical grip), vehicle controls, interactables and the hold styles they
inherit from. One file per entry, `assets/handposes/<id>.json`, holds **only what differs** from
the entry's parent. Every runtime consumer resolves a pose with the same code, and so does the
editor (`scene handpose`). What you see in the editor is the game.

| Area | Code | Data |
|---|---|---|
| Data, resolution, limits, presets, mirror, contact solver, JSON (pure) | `src/anim/handpose.{c,h}` | `assets/handposes/*.json`, `styles/*.json` |
| Discovery / stub creation (pure) | `src/editor/handpose/hp_discover.c` | |
| Startup scan, hot reload, commands, saving | `src/editor/handpose/hp_system.c` | |
| Editor field logic + undo / redo (pure) | `src/editor/handpose/hp_edit.c` | |
| Editor scene (camera, panels, `hpe` command) | `src/editor/handpose/hp_editor.c` | |
| Dev scenes (`scene <name>`) | `src/game/game_scene.c`, `src/game/scene.h` | |
| Weapon / item hold (sockets, IK, fingers) | `src/anim/weapon_hold.c` | |
| Vehicle controls, interactables (third person / first person) | `src/gameplay/characters.c`, `src/player/hands.c` | |
| Attachment grips | `src/gameplay/attachments.c` | `assets/attachments/*.json` |
| Tests | `tests/test_handpose.c`, `tests/test_hp_edit.c`, `tools/tests/hp_scene.cfg` | |
| CI report | `tools/handpose_report` | |

## Data

### Entries and ids

| Category | Id | Discovered from |
|---|---|---|
| weapon | `<weapon id>` (`rifle`) | `data/weapons/*.json` with a `"hold"` block |
| item | `<item id>` (`medkit`) | `data/items.json` items with a `"hold"` block |
| attachment | `att_<id>` (`att_vertical_grip`) | attachments that add a left-hand grip |
| vehicle | `veh_<def>` (`veh_car`) | `assets/vehicles/*.json` with steering controls |
| interactable | `use_<name>` (`use_door`) | registered interactable types (C) |
| style | `style/<hold style>`, `style/grip_<grip>` | hold styles (`controller.json`), first-person grips |

Styles that have no file are generated in memory from the old finger curl tables. So an entry that
overrides nothing behaves exactly as before hand poses existed. `handpose_gen_styles` writes them
out as files.

### States

`default` applies in every state. The others are: the hold point states (`hip`, `ads`, `sprint`,
`crouch`, `prone`, `empty`, `inspect`, `vehicle_onehand`, `phone_portrait`, `phone_landscape`),
the interactable states (`use`, `hold`) and the vehicle control states (`drive`, `drive_onehand`).

### Fields (one layer = one hand × one state)

| Field | JSON | Meaning |
|---|---|---|
| socket | `"socket": { "pos": [x, y, z], "q": [x, y, z, w] }` (or `"rot"`: pitch / yaw / roll degrees) | palm target in entry space. Unset: the item's own `grip_r` / `grip_l`. Relative entries (vehicles, interactables): an offset from the gameplay target |
| wrist offset | `"wrist"` (same form) | palm offset relative to the socket (palm frame) |
| pole hint | `"pole": [x, y, z]` | elbow direction, camera space. Unset: the hold style's `elbow_r` / `elbow_l` |
| IK weight | `"ik_weight": 0..1` | multiplies the hold's hand IK weight |
| fingers | `"fingers": { "index": [[curl, spread, twist] × 3], ... }` | degrees per joint (thumb: CMC, MCP, IP; fingers: MCP, PIP, DIP), clamped to the joint limits on load, on every edit and when resolving |
| contacts | `"contacts": [{ "finger": "index", "point": [x, y, z] }]` | fingertip targets on the item (entry space), used by the contact solver |

A finger is overridden as a whole (all three joints). An override of a field that the layer did not
set before starts from the value it inherited, so turning one on changes nothing until you edit it.

### Resolution

```
base (open hand, item grip socket, style elbow, weight 1)
  <- for each ancestor, root first: layer[default], then layer[state]
```

So an item stores only what differs from its style, and a field set on the item's `default` layer
wins over the style's per-state value. At runtime the hold style in use is the base chain, and the
item's entry goes on top (`handpose_resolve_over`). A style that is already in the entry's chain is
applied once.

### Files, discovery, reload

- At startup, on map load and on `handpose_scan`, every source without an entry gets a **stub**.
  A stub inherits its style, lists the hands and states its source uses, overrides nothing and has
  `"reviewed": false`. Dev builds also write the stub to `assets/handposes` in the source tree and
  to the runtime copy, but never over an existing file (`handpose_autoreg`). Entries whose source
  is gone are flagged **orphan** and listed, never deleted silently. **Auto-placed** stubs (an
  interactable whose code has no hand target) got a socket from their type and need review.
- Files edited outside the game reload by themselves within about a second (`handpose_watch`).
  `handpose_reload` reloads them by hand.
- The DB hash (entries that change a pose) is part of the animation manifest. Peers must have the
  same files, so `handpose_reload` in a live session warns.

| Command | |
|---|---|
| `handpose_reload` | re-read the files and rescan |
| `handpose_scan` | scan for new sources now |
| `handpose_report` | log the unreviewed, auto-placed and orphaned entries |
| `handpose_gen_styles` | write the generated style defaults to `assets/handposes/styles` |

## Editor (`scene handpose`)

`scene handpose` opens the editor in the running game (host, local player). `scene off` (or
`scene`, or the **Leave** button) closes it. `scene` with nothing open lists the scenes. While the
editor is open:

- **The local player holds the selected entry's item.** It is equipped into hotbar slot 1 and the
  previous slot comes back on leave. Weapons and items show themselves. An attachment is mounted on
  the first weapon that takes it. A hold style shows on its first weapon or item. Vehicle controls,
  interactables and grip styles have no item: sit in the vehicle or use the interactable with the
  editor open to preview them.
- **The edited state is forced** with `holdpoint_force_state`, and the sway is off
  (`holdpoint_static`). `default` and the states that can't be forced preview as `hip`.
- **View**: an orbit camera around the edited hand (or the item) with the body drawn whole (third
  person), or the normal first-person view. Drag in the view (left or right mouse) to orbit, and use
  the wheel to zoom. The View section has yaw / pitch / distance / FOV. Markers: the palm frame of
  the edited hand (red / green / blue = palm x / y / z) and the contact points (yellow, with a line
  to their fingertip).
- **Edits apply at once.** They go into the live DB, and the runtime picks them up on the next tick
  through the same resolution code. Nothing is written until you save.

### Panels

**HAND POSES** (left): every entry with flags (`*` unreviewed, `A` auto-placed, `O` orphan, `+` has
overrides, `!` edited and not saved). You can filter by category or text, and show only the
unreviewed, auto-placed, orphaned or edited entries.

**HAND POSE** (right):
- Header: id, category, source, parent; what the player is holding for the preview.
- Hand (left / right; "unused" when the source doesn't use that hand) and state. In the state list,
  `*` marks the states the source uses and `+` the ones this layer overrides.
- **Save** writes the canonical file (only the overrides, stable key order) to `assets/handposes` in
  the source tree and to the runtime copy, and marks the entry `"reviewed": true`. **Revert** reloads
  the entry from its file (and can be undone). **Undo** / **Redo** keep the last 64 edits (Ctrl+Z /
  Ctrl+Y); dragging one control counts as one step. Undo works on whole entries, so it crosses entry
  changes.
- **Hand**: socket (position m, rotation pitch / yaw / roll degrees), wrist offset, pole hint, IK
  weight.
- **Fingers**: preset, then **apply preset** (all five fingers of this layer); **mirror** copies every
  state of this hand to the other one, mirrored (`handpose_mirror_entry_hand`). For each finger,
  three joints × curl / spread / twist, limited to the joint limits.
- **Contacts**: contact points (finger + point in entry space). **add at fingertip** adds the
  chosen finger's current tip. **solve contacts** closes each finger that has a point until it
  touches the item or reaches the curl closest to its point. **close all fingers** also closes the
  others until they touch. The item is approximated by its part boxes (weapon parts + mounted
  attachments, or the held item's box). The solve starts from this tick's body pose with the
  fingers back at the animation pose.
- **View**: orbit / first person, focus on the hand or the item, yaw / pitch / distance / FOV.

Every field row shows where its value comes from: `override` (with a **reset** button that goes
back to the inherited value), `inherited: style/rifle (hip)`, or `inherited: base`. `override
(hidden by ...)` means a field set on `default` that this entry's own state layer hides.

The editor warns on leave about edited entries that weren't saved. They stay in memory (and in
use) until `handpose_reload`.

### Scripted use (`hpe`)

Every editor action is also a command, for tests and bindings:

```
hpe select <id>        hpe hand l|r            hpe state <name>
hpe joint <finger> <0-2> <curl> [spread] [twist]
hpe preset <name>      hpe mirror              hpe reset socket|wrist|pole|ikw|contacts|<finger>
hpe contact <finger>   hpe solve [all]         hpe undo | redo | save | revert | dump
hpe view orbit|fp|hand|item | hpe view <yaw> <pitch> <dist>
```

`handpose_editor_dir <dir>` makes **Save** write only into that directory, so tests don't touch
`assets/`.

### Tests

- `tests/test_hp_edit.c` (ctest `hp_edit`): field provenance, seeding an override keeps the
  resolved pose for every field and state, reset to inherited, joint limits on edits, presets,
  mirror, contacts, the written file holds only the overrides, undo / redo (coalescing, the 64-step
  bound, redo cleared by a new edit, entries found again after a reload reorders them).
- `tests/test_handpose.c` (ctest `handpose`): the data layer (resolution, round trip, migration,
  limits, contact solver, hand glue).
- `tools/tests/hp_scene.cfg`: opens the scene, closes the rifle's right index, undo / redo, save
  (into `t_hp_scene/`), the vertical grip with every finger solved, a medkit with a contact point,
  and the first-person view. It takes a screenshot of each. Run it from `build/bin`:
  `./deadground.exe --test --nosound --log t_hp_scene.log --script ../../tools/tests/hp_scene.cfg`
  (add `--validation` with `VK_ADD_LAYER_PATH` set, see CLAUDE.md).

## Adding a scene

A scene is a static `GameScene` (`src/game/scene.h`): name, enter / leave, an optional camera
callback (return true to own the camera; the body is then drawn in third person), a Nuklear draw
callback, and whether it needs the cursor and hides the HUD. Register it once at init with
`scene_register`. `scene <name>` opens it and leaves the current one first.
