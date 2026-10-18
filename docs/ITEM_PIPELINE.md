# Detailed items: pipeline and standard

Every weapon, attachment and item has a detailed model built by script in Blender and exported as a
`.dgim` file. Detail comes from geometry: parts with pivots, bevels, machined cuts, rails at the real
pitch, screws, pins, lettering. Materials come from per-vertex material IDs plus baked masks that the
engine layers at load. There are no textures (by design: colours, roughness and metallic only).

| What | Where |
|---|---|
| Item scripts (the source) | `assets/src/items/<id>.py` (shared: `_parts.py`, `_ar.py`, `_optics.py`, `_scope.py`, `_muzzle.py`, `_extmag.py`, `_light.py`, `_keys.py`, `_boxes.py`) |
| Modelling kit | `tools/blender/item_lib.py` |
| Build (script -> .blend -> .dgim -> validate) | `tools/blender/build_items.py` |
| Export conventions / writer | `tools/blender/item_export.py` |
| Baking (AO, curvature, dirt, grunge, cavity) | `tools/bake_item/bake_item.py` |
| Validator | `tools/validate_item/validate_item.py` (ctest `validate_items`) |
| Catalog (the item list, budgets, required parts / sockets, modelling path) | `assets/items/catalog.json` |
| Material library + conditions | `assets/items/materials.json` |
| Exported meshes | `assets/items/meshes/<id>.dgim` |
| Engine: loader, layering, LODs, drawing | `src/assets/item_mesh.c` |
| Engine: hands / ground / third person | `src/player/hands.c` (`draw_weapon`, `draw_held_item`, `hands_emitter`) |
| Engine: inventory / icons | `src/ui/item_view.c`, `src/render/render_ui.c` (`RenderUi3dPart.mesh`) |
| Inspector, showcase, `item_reload` | `src/game/game_items.c` |
| Screenshots | `tools/tests/item_shots.py`, `docs/items/` |

## Modelling path used

Every model in this pass uses **path (b): procedural modelling in Blender scripts**. There is no
image/text-to-3D service available to the project: the Higgsfield key drives the image model only,
and this pass uses no textures anyway. So there is no `tools/gen_item`. The `path` field of each
catalog entry records the path an item used, so a future generated base mesh can be tracked.

## Conventions (item space)

- Metres. The origin is the **`grip_r` socket**: where the right hand holds the item.
- Engine axes: `+Y` up, `-Z` forward (the barrel or lens direction), `+X` right.
  - Melee weapons run along `+Y` from the grip.
  - In Blender this is `Z` up, forward `+Y`. `item_lib` converts both ways (`E2B` / `B2E`).
- Scene layout:
  - One root empty `ITEM_<id>` at the world origin, unrotated and unscaled.
  - Parts are meshes named `P_<name>`, placed under the root or under another part. The hierarchy
    is kept, so a follower can sit under its magazine.
  - Sockets are empties named `S_<name>`.
- Part custom properties:
  - `mech`: `slide`, `bolt`, `pump`, `mag` (the fire animation moves these, docs/FIRE_ANIM.md), or a
    static role name (`trigger`, `safety`, `mag_release`, `charging_handle`, `dust_cover`, `cap`,
    `lid`, `spoon`, `pin`...). Roles keep a part separate, with its pivot, for later animation.
  - `hide`: the attachment slot that hides the part while that slot is filled. A hidden part's
    children hide with it.
    - `magazine`: the stock magazine.
    - `stock`: the stock and its tube.
    - `muzzle`: the integral device.
    - `optic`: an iron sight that stands where the optic mounts.
  - `fold` with `fold_axis` / `fold_angle`: the part rotates about its pivot while the slot is
    filled (flip-up front sights fold for an optic).
  - `detail`: 1 drops a small part from the coarsest LOD.
- Material slots are named after the keys of `assets/items/materials.json`.
- Gameplay sockets come from the JSON. Item scripts read the weapon or attachment JSON and build
  the geometry around those values: bore, rail top, sight line, front sight, muzzle and slot
  positions (`_ar.py expect()`). The validator checks every socket against the JSON to within 2 mm.
- Recoil, ADS, hold poses and VFX still read the JSON, and the box `"model"` stays as the gameplay
  shape (bounds, iron-sight generation, muzzle). The mesh's bounds rest pickups on the ground
  (`item_model_bounds`).
- New sockets the meshes carry:
  - Weapons: `ejection_port`, `shell_eject`, `charging_handle`, `bolt_handle`, `feed_tray`,
    `stock_pos_N` (collapsible stock positions), `string_latch`, `tip`.
  - Attachments: `mount` (the slot frame), `lens`, `objective`, `reticle` (second focal plane),
    `emitter` (lights, lasers).
  - Items: `needle`, `cap`, `spout` / `pour`, `pin`, `spoon`, `ignition`, `wick`, `screen`,
    `camera_lens`, `front_lens`.
- **Palm-held items** are modelled at real size, with their bounds centred where the old held box
  sat: `(0, min(hy, 0.07) + 0.03, -0.05)` (`Item.hold_small()`). The phone and **large items**
  (fuel can, spare tire, backpack, armor) are placed by their own grip: the carry handle or edge
  sits at the origin.

### Optics

The sight pass draws the window and the scope image at the JSON lens position (docs/ATTACHMENTS.md),
so a mesh must keep the view path clear:

- No opaque geometry inside the window radius in front of the eye-side lens plane.
- A red dot is an open tube. The glass and the dot are the sight pass's.
- In a scope, the ocular lens sits 4 mm forward of the lens plane. The full lens stack (objective,
  erector pair, ocular) is real geometry, and the `reticle` socket marks the focal plane.

### Attachments per weapon

An attachment can carry `"mesh_for": { "<weapon id>": "<mesh>" }` to fit a particular weapon.
The extended magazine has one variant per magazine well (rifle 5.56, `smg` 9 mm, `dmr` 7.62), and
the weapon light has a compact variant for the `pistol` frame rail (`attach_model_for`).

### Lights and lasers

An attachment with `"emitter": "light"` or `"emitter": "laser"`, mounted in the underbarrel slot,
takes over the flashlight toggle (F) in first person. The same goes for a held item whose mesh has
an `emitter` socket (the flashlight item).

- **Light:** the spot leaves the `emitter` socket along the barrel (`hands_emitter`,
  `game_render.c flashlights_render`).
- **Laser:** a faint beam runs to the first world hit, with a red dot and a little glow there
  (`laser_render`).

Other players still see the eye-mounted flashlight (no protocol change).

## Budgets (LOD 0 triangles, `assets/items/catalog.json`)

| Tier | Budget | This pass |
|---|---|---|
| Firearms | 40k .. 150k (crossbow 20k) | 42k .. 65k |
| Melee | 5k .. 60k | bat 18k, knife 6k |
| Attachments | 2.5k .. 80k | 2.7k (mags) .. 31k (8x scope) |
| Items | 3k .. 40k | 3.9k .. 20k |
| Cheap tier (ammo, crafting, props) | 0.5k .. 20k | 1.9k .. 16k |

**LODs:**

- LOD 1 is about 30 % and LOD 2 about 9 % of LOD 0, by collapse decimation per part. A small part
  keeps at least 20 faces.
- LODs switch at `radius x 40` and `radius x 120` metres, corrected for FOV.
- The viewmodel always uses LOD 0.
- The game mesh is the source. There is no separate high-poly cage, because there is no texture to
  bake to.

## Materials and wear (no textures)

Every vertex stores a material slot plus five masks, baked by `tools/bake_item`:

- `wear`: convex curvature, favouring exposed areas.
- `dirt`: crevices, occlusion and upward faces.
- `ao`: Cycles ambient occlusion over the whole item, so parts occlude each other.
- `grunge`: low-frequency 3D noise.
- `cavity`: concave curvature.

At load, `compose()` in `src/assets/item_mesh.c` layers each library material over these masks:

1. Edge wear exposes the `edge` substrate (bare steel under bluing or paint, bare aluminium under
   anodizing). It is gated by the grunge mask, so edges chip in patches instead of shining evenly.
2. Grunge varies albedo and roughness.
3. Dirt darkens and roughens.
4. In the rusty condition, ferrous materials rust outward from cavities.

The result goes into the vertex colour (`sqrt(albedo)`, alpha = AO) and the uv (`(roughness,
metallic)`). Draws use `MAT_VERTEX_PBR`, so a part carries every one of its materials in one draw.
The static parts are merged into a single body draw, plus one draw per moving or hidden part, so a
rifle is about 8 draws.

**Conditions** (`materials.json "conditions"`: `new`, `worn`, `rusty`):

- The item JSON's `"condition"` picks one (the default is `worn`).
- The `item_condition <new|worn|rusty|auto>` command overrides it everywhere.
- Each condition's GPU meshes are composed on first use.

The skin hook works the same way: a skin is another material table over the same masks.

**Adding a material:** add a key to `materials.json` with `color`, `rough`, `metal`, optionally
`edge`, `wear`, `rust`, `grunge` and `dirt`, then run `item_reload`. Both the Blender viewport
colour and the validator read the same file.

## File format (`.dgim` v2)

The header (116 bytes, uncompressed) is followed by the zlib-compressed payload:

- materials (32-byte names)
- parts (204 bytes each): name, parent, pivot, roles, bounds, per-LOD ranges
- sockets (64 bytes each)
- hulls
- vertices (16 bytes each): position quantised in the part's bounds, octahedral normal, material,
  wear, dirt, AO, grunge, cavity
- u16 indices
- hull points

The layout is `static_assert`ed in `item_mesh.c`, written by `item_export.py` and read by
`validate_item.py`. Each part carries a convex hull (64 points at most) for dropped items and
physics props. Pickups have no physics bodies yet, so nothing consumes the hulls today; the
validator checks they exist.

## Workflow: adding a new item at this standard

1. Add the gameplay entry first: `data/items.json`, `data/weapons/<nn>_<id>.json`, or
   `assets/attachments/<id>.json`, including its sockets and slots.
2. Write `assets/src/items/<id>.py` with `build(ctx) -> item_lib.Item`.
   - `ctx.weapon` / `ctx.item` / `ctx.attachment` hold the JSON objects.
   - Read the gameplay dimensions from them, as `_ar.py` does.
   - Model real parts with pivots and roles, and add every socket the systems need.
   - Palm-held items end with `it.hold_small()`.
3. Add a catalog entry to `assets/items/catalog.json`: `kind`, `budget`, `path`, plus the `parts`
   and `sockets` you require.
4. Build: `python tools/blender/build_items.py <id>` (`pip install bpy`), or
   `blender -b -P tools/blender/build_items.py -- <id>`. This saves `assets/src/items/<id>.blend`,
   which is git-ignored, and writes the `.dgim`, then validates it.
   - To export a hand-edited scene that follows the conventions:
     `build_items.py --blend <file>`.
5. Point the JSON's `"mesh"` at the file (`assets/items/meshes/<id>.dgim`), plus `"condition"` if
   it shouldn't be `worn`.
6. Check it in game:
   - `item_inspect <id> [shaded|wire|matid|wear|ao|dirt|sockets]`, with `item_inspect_orbit`,
     `item_inspect_spin` and `item_inspect_lod`.
   - `item_reload` after rebuilding.
   - `item_showcase` (dev room, every item on a turntable).
   - `give <id>` for first person.
   - `sight_test align all` for weapons and optics.
   - `item_meshes 0|1` for before / after.
7. Run `tools/bake_icons/bake_icons.sh` to re-bake the icons; they render through the mesh. Commit
   the PNGs.
8. If its proportions moved a socket, update the JSON and re-run `test_recoil_ads`, `test_handpose`,
   `tools/tests/sight_align.cfg` and `tools/tests/item_nearplane.cfg`.

## Tests

- `validate_items` (ctest, Python): every catalog item, as described in the validator's header,
  plus the **hand glue**. Every hand socket (`grip_r`, `grip_l`) must lie within 30 mm of the LOD 0
  surface (point-to-triangle distance), so a hand never floats off the new geometry.
- `item_mesh` (ctest, `tests/test_item_mesh.c`, C):
  - The engine loader runs on every shipped `.dgim`, and every condition composes with values in
    range.
  - Posing works: bolt travel, the front sight fold, hidden magazine groups. Static parts merge
    into at most 12 draws.
  - Every weapon's mesh sockets match what `items.c` derives: the muzzle from the box model, the
    sight from `iron_sights` (ADS alignment), the slot sockets and the support hand.
  - Every attachment's mesh sockets match `attachments.c`: mount, sight point, lens, grip, emitter.
  - Every allowed weapon x attachment combination has a mesh.
- `tools/tests/item_nearplane.cfg` + `item_nearplane.py`: every weapon in first person at the
  default FOV, hip and ADS. It counts viewmodel pixels clipped by the near plane (the nearest depth
  of the viewmodel pass).
- `tools/tests/sight_align.cfg` (`sight_test align all`): every weapon x optic keeps the sight on the
  view axis.
- `test_recoil_ads` and `test_handpose`: unchanged gameplay sockets. The glue tests run against the
  JSON sockets the meshes were built on.
