# Inventory (grid / container model)

Tarkov-style inventory: items take rectangles of cells, rotate 90°, and live inside containers
(pockets, rig, backpack, cases) that are items with their own grids. Equipment slots hold the
worn items. World containers (crates, cabinets, car trunks, corpses, the fabricator queue) use the
same trees and the same screen. Host-authoritative: every change is an intent.

| Part | Code |
|---|---|
| Tree, footprint, fit, space search, weight (pure) | `src/gameplay/inventory/inv_tree.{h,c}` |
| Rules: equip slots, filters, nesting, quick-move order (pure) | `src/gameplay/inventory/inv_rules.{h,c}` |
| Intents: apply / validate across two trees (pure) | `src/gameplay/inventory/inv_intent.{h,c}` |
| Wire: tree + intent encoding (pure) | `src/gameplay/inventory/inv_wire.{h,c}` |
| Gameplay facade (`Inventory`, hotbar accessor, pickups) | `src/gameplay/inventory.{h,c}` |
| Item / container definitions | `data/items.json`, `data/containers.json`, `src/gameplay/items.c` |
| World containers, corpses, vehicle trunks (trees per entity), host intent handling, loot sync | `src/gameplay/entities.c` (`container_tree`), `src/game/game_events.c`, `src/net/net.c` |
| Screen: layout + drag and drop (pure), grid panels, inspect, context menu, gamepad | `src/ui/inventory/{inv_layout,inv_dd,inv_screen}.c` |
| Cell images (grid-aspect icons, baker) | `src/ui/inventory/inv_cells.c`, `tools/bake_icons/bake_cells.sh` |
| Editor panel | `src/ui/inventory/inv_editor.c` (`inv_editor`) |
| Showcase camera (the player faces the screen) | `src/game/game_render.c` (`inv_showcase`) |
| Tests | `tests/test_inventory.c`, `tests/test_inventory_ui.c`, `tools/tests/lint_inventory.py`, `ui_inventory.cfg`, `ui_crate.cfg`, `inv_net.sh` + `inv_net_check.py` |

## 1. Data model

Items and their instances (as the code has them; JSON keys in the second column).

### Item definition (`ItemDef`, items.json)

| Field | JSON | Meaning |
|---|---|---|
| `grid_w`, `grid_h` | `"grid": [w, h]` | cells, unrotated. Missing: the class default (below) |
| `rotatable` | `"rotatable"` (default true when w != h) | R / X rotates it |
| `inv_weight` | `"weight"` kg | **informational only**: shown per item, per container and in total. Nothing in movement, stamina or physics reads it (`lint_inventory.py` checks every reader) |
| `max_stack` | `"stack"` | stack size |
| `container` | `"container": { ... }` | its own grid(s): see containers |
| `equip_slot` | `"equip"` | `primary`, `holster`, `melee`, `headwear`, `earpiece`, `face_cover`, `eyewear`, `body_armor`, `tactical_rig`, `backpack`, `armband`, `secure_container`. Long guns default to `primary` (fits primary and secondary), pistols to `holster`, melee weapons to `melee` |
| `use_type`, `charges` | `"use": "consume" / "charges"`, `"charges"` | consumables: one per use, or charges per item (a medkit with 3 uses) |
| `durability` | `"durability"` | max points (0 = none); instances start full |
| `fold_dw` | `"fold": 1` | columns the weapon loses when its stock is folded (rifle, SMG) |
| `cell_image` | `"cell_image"` | override PNG for the grid image (else `assets/ui/cells/<id>.png`, else a live bake) |

**Class defaults** (`inv_class_defaults`, applied after the weapons load): pistol 2×1, SMG 3×2,
shotgun / rifle / crossbow 4×2 (DMR 5×2), LMG 5×2, sniper 5×2, melee 1×3 (bat) / 1×2 (knife),
medkit 2×2, bandage / painkillers / syringe / DNA / grenade / phone / keys / batteries 1×1,
fuel can 2×3, tire kit 2×2, spare tire 3×3, ammo 1×1, attachments 1×1 (scopes 2×1).

**Attachments** change a weapon's footprint: `"grid": [dw, dh]` in the attachment JSON (default:
muzzle devices longer than 10 cm add a column, so a suppressor makes a 4×2 rifle 5×2).

### Containers (`InvContainerDef`)

```json
"container": {
  "compartments": [ { "w": 2, "h": 1, "at": [0, 0] }, { "w": 2, "h": 2, "at": [2, 0] } ],
  "allow": ["ammo"],             // item kinds (empty = any)
  "deny": ["weapon"],            // item kinds
  "deny_items": ["dna_sample"],
  "no_containers": true,         // holds no container items (secure container, small cases)
  "max_item": [2, 2]             // largest footprint it takes (0 = any)
}
```

`at` places the compartment in the panel (cells); compartments may be separate grids (rigs) or one
big grid (backpacks). Root containers (what a tree *is*) live in `data/containers.json`: `body`
(the player: 4 pocket grids of 1×1 plus the equipment slots; it must stay first: a zeroed tree's
root is container 0), `crate` 6×4, `crate_small` 4×3, `cabinet` three 4×2 drawers, `trunk` 8×5,
`fabricator` 4×2, `stash` 10×12. A level container picks its root with `"grid": "<id>"` (default
`crate`; building-kit furniture `cabinet`); a corpse's tree is a `body` holding what the player
carried; a car's trunk is a `trunk`.

**Nesting:** an item can't go into itself or its own contents; a container item can't go into a
container whose capacity (cells) is not larger than its own (a backpack never holds a backpack of
the same or a larger size); `no_containers` and the filters are checked for every item of a moved
subtree's top.

### Instances (`InvItem`; 21 bytes on the wire)

```c
typedef struct InvItem {
    InvSlot s;       // item, count, ammo (rounds loaded / DNA id), att[5]: what gameplay reads
    u16 uid;         // unique within its tree (0 = free)
    u16 parent;      // uid of the container item it is in; 0 = the tree root
    u8 loc;          // INV_LOC_EQUIP + slot (root only) or compartment index
    u8 x, y;         // top-left cell
    u8 flags;        // INVF_ROT (90°), INVF_FOLDED
    u16 dur;         // durability left
} InvItem;
```

A tree (`InvTree`) is a pool of 128 instances plus its root container def. The player's
`Inventory` is a tree rooted at the body (equipment slots + pockets) plus the hotbar binds:

```c
typedef struct Inventory {
    InvTree t;
    u16 quick[INV_HOTBAR];   // hotbar slots 1-5: instance uids (0 = empty)
    InvSlot none;            // what an empty hotbar slot reads as
    u32 revision;
} Inventory;
```

* **Footprint** = def grid + attachment deltas - fold, swapped when rotated (`inv_item_size`).
* **Weight** of an instance = def weight × count + its contents (`inv_item_weight`); the screen
  shows per-container and total weight. No gameplay effect.
* **Stacking:** same def, both stackable: merge up to `max_stack`, the remainder stays.
* **Hotbar binds** reference instances, never cells: moving the item anywhere in the tree keeps
  the bind; leaving the tree (dropped, moved to a crate, used up) clears it.
* **Secure container:** on death everything goes to the corpse except the `secure_container`
  slot's subtree; binds into what left are cleared (`inv_strip_to_corpse`).
* **Charges:** `"use": "charges"` items count the charges used in `ammo` (a medkit: 3 uses).
* **Dev tools** (hold point / hand pose editors, sight tests) bind an instance held outside the
  grids (`loc` `INV_LOC_NONE`, `inv_dev_hot`); it never takes cells.

## 2. Rules (`inv_rules.c`)

| Rule | Check |
|---|---|
| Fit | the footprint lies inside the compartment and overlaps nothing but the moving item |
| Rotation | only when `rotatable`; R while dragging / X on a pad |
| Equip | the slot accepts the item's `equip_slot` (primary / secondary both take `primary`) |
| Filters | allow / deny kinds, deny items, `max_item`, `no_containers` |
| Nesting | no cycles; capacity strictly larger; containers never in `no_containers` |
| Stack merge | same def, `max_stack` > 1 |
| Swap | dropping a single item on a different single item: both must fit in each other's place |
| Attachments | mounting re-checks the weapon's new footprint where it lies (rejected with "No room") |
| Quick move (Ctrl+click) | loot open: across (player → loot root, loot → player). Else to the next player container in the order `inv_quick_order` (`rig,backpack,pockets`) |
| Pickup (`inv_add`) | merge into stacks, equip a weapon into a free matching slot, else first space in the quick order; weapons / consumables / throwables bind to a free hotbar slot |

## 3. Intents (`inv_intent.h`)

Every change is an `InvIntent` applied by `inv_intent_apply(ctx, intent)` on the host and, as a
prediction, on the client. `ctx` holds the two sides: `INV_SIDE_SELF` (the player's tree) and
`INV_SIDE_LOOT` (the open world container's tree, or none).

| Intent | Fields | Result |
|---|---|---|
| `II_MOVE` | src side / uid, dst side / parent / loc / x / y / rot, count | place, merge onto a stack, swap with a single item; count < stack = split (Shift-drag) |
| `II_INSERT` | src, dst item uid | into the first free space of a container item |
| `II_ROTATE` | src | rotate in place (when it still fits) |
| `II_EQUIP` | src (`aux` = slot, 255 = its own) | into the equipment slot (an occupant swaps out to the source cell when it fits) |
| `II_QUICK_MOVE` | src | the quick-move rule above |
| `II_SPLIT` | src, count, dst | count off the stack into dst (or the first space) |
| `II_DISCARD` | src, count | removed; the host spawns the pickup (whole subtree for containers) |
| `II_USE` | src | consumables (heal / armour / vehicle use): host effect |
| `II_MOUNT` / `II_UNMOUNT` | weapon uid, attachment uid / slot | as before, footprint re-checked |
| `II_FOLD` | weapon uid | fold / unfold the stock (footprint changes) |
| `II_UNLOAD` | weapon uid | rounds back into an ammo stack |
| `II_BIND` | src uid (0 = clear), `aux` = hotbar slot | hotbar bind |
| `II_TAKE_ALL` | — | everything in the loot root that fits |
| `II_SORT` | dst side / container, `aux` key | re-pack one container by type / weight / size (all or nothing) |

Results: `IIR_OK`, `IIR_NOOP`, `IIR_INVALID`, `IIR_NO_ROOM`, `IIR_FILTER`, `IIR_NESTING`,
`IIR_GONE` (the item is no longer there: someone else took it), `IIR_TOO_FAR`, `IIR_FULL`
(pool), `IIR_EQUIP`, `IIR_NOT_USABLE`, `IIR_INCOMPATIBLE`, `IIR_NO_SLOT`, `IIR_NOT_WEAPON`.
The text for each is shown in the footer and on the shaken target.

## 4. Networking

Every intent is applied to scratch copies of the trees and committed only when it succeeds, so a
rejected one changes nothing. Pure (`inv_intent_apply`); the host adds the world effects (pickups
for discarded items, healing).

* **Client → host:** `MSG_INV_INTENT` (reliable): the packed intent (`inv_wire.c`), sequence.
* **Host → owner:** `MSG_INVENTORY`: slot, revision, last applied sequence, hotbar binds, the
  live instances. The client takes it, drops acknowledged intents and re-applies the rest.
* **Loot:** opening a container (`EV_CONTAINER`) sets `Player.open_container`; the host validates
  the distance (`sv_inv_reach`, 3.5 m, vehicles + 1 m) on every loot intent and sends `MSG_LOOT`
  (entity, revision, tree) to every player that has that container open whenever its revision
  changes (relevance: only open containers' trees travel; a closed one is sent once as closed).
  Walking away closes it; the screen closing sends `RPC_CONTAINER_CLOSE`.
* **Client prediction:** the client keeps the host's last trees (bases) and shows bases + the
  unacknowledged intents re-applied (`cl_repredict`, on every `MSG_INVENTORY` / `MSG_LOOT`).
* **Conflicts:** the host is the only writer; intents apply in arrival order. A loot intent whose
  source uid is gone, or whose cells are taken, is rejected (`IIR_GONE` / `IIR_NO_ROOM`); both
  clients converge on the next `MSG_LOOT`. Moving between trees is one host step (remove +
  insert), so items are never duplicated or lost (the two-client test runs this under 150 ms lag).
* Corpses (`ET_CORPSE`: tap use when no DNA extraction is on offer) and car trunks (`ET_VEHICLE`:
  use from behind the car) use the same path through `container_tree(w, ent)`. The fabricator's
  sample queue stays its own machine (docs/REVIVE.md); its `fabricator` root is defined for a
  later move onto a grid.
* Protocol 27 (`DG_PROTOCOL_VERSION`). Story saves keep the trees (`tree_json`); saves from before
  the grid inventory load with empty inventories / containers.

## 5. Screen (`src/ui/inventory/`)

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ INVENTORY                         total 23.4 kg                         syncing…     │
├───────────────────────────┬───────────────────────────────┬──────────────────────────┤
│ POCKETS      0.3 kg  4/4  │   [HEAD] [EAR]   [FACE] [EYE]  │ HOTBAR                   │
│ ▢ ▢ ▢ ▢                   │                               │ [1] ▭▭▭▭ Assault Rifle   │
│ TACTICAL RIG 2.1 kg 6/12  │   [ARMOR]   (player 3D,   [RIG]│ [2] ▭▭  Pistol           │
│ ▢▢ ▢▢ ▢▢▢▢               │            live, front)       │ [3] ▢   Medkit           │
│ BACKPACK     8.0 kg 14/30 │   [BACKPACK]           [ARMBAND]│ [4] ▢   Frag             │
│ ▢▢▢▢▢                     │                               │ [5] ▢   —                │
│ ▢▢▢▢▢   (scrolls)         │   [PRIMARY] [SECONDARY]        │──────────────────────────│
│ SECURE       0.4 kg  1/4  │   [HOLSTER] [MELEE]  [SECURE] │ CRATE / BODY / TRUNK      │
│ ▢▢                        │                               │ (loot grid when open)     │
├───────────────────────────┴───────────────────────────────┴──────────────────────────┤
│ hints  ·  last rejection                                                             │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

* **Left:** the player's grids stacked in the quick-move order (pockets, rig, backpack, secure
  container), each a panel with its name, weight and used / total cells; the column scrolls.
* **Centre:** the player, live (the game camera turns to face the body while the screen is open,
  `inv_showcase`), with the equipment slots around it (left: headwear, earpiece, face cover,
  eyewear, armband; right: body armour, rig, backpack, secure container; below: primary,
  secondary, holster, melee) and the Discard zone. No backdrop behind the player.
* **Right:** the hotbar: the five bound items as large tiles (cell image at its grid aspect, name,
  magazine / reserve, key number). With a world container open, the hotbar compacts to a row of
  squares and the loot grid (crate, corpse, trunk) fills the rest of the column (it scrolls; a
  corpse shows its equipment tiles, its rig / backpack grids, then its pockets).
* **No tabs.** Inspect, nested containers and the split dialog are floating windows.
* **Footer:** key hints and the last rejection reason.

**Mouse:** drag between any grids / slots / hotbar tiles; the footprint shows green (fits) or red
(doesn't); **R** rotates while dragging; dropping on a stack merges, on a container item inserts,
on a different single item swaps; Ctrl+click quick-moves; Alt+click equips; Shift+drag splits
(dialog); double-click opens a container in a window (nested windows, movable) or inspects an
item; right-click: Use, Equip, Unequip, Open, Inspect, Fold / Unfold, Unload, Split, Bind to
hotbar, Discard; dragging out of the panel or onto Discard drops it. Rejected drops shake the
target and print the reason.

**Inspect window:** the 3D preview (`item_view`), stats, the attachment slots (drag attachments
in / out); the footprint line updates live (`4×2 → 5×2`).

**Tooltips:** name, kind, size, weight, stack, durability, the slot it is worn in (no value: the
game has no economy).

**Filters / sort** per panel: the filter chip cycles All / Guns / Ammo / Meds / Gear (the rest
dims); Sort re-packs the container (`II_SORT`) by type, then weight, then size on the next click.

**Gamepad:** a cell cursor; d-pad / stick move it across cells (a whole item is one stop), LB / RB
cycle panels (grids → equipment → hotbar → loot), A picks up / places, X rotates while carrying
(menu when idle), Y quick-moves, B cancels / closes.

## 6. Commands

| Command | |
|---|---|
| `inv_give <item> [count]` | add through the pickup rule |
| `inv_fill <container uid or slot name> <item> [count]` | fill a container's free cells |
| `inv_dump` | log the tree (uid, def, parent, loc, cell, rot, count) |
| `inv_intent <type> ...` | run an intent (tests) |
| `inv_open <entity>` | open a world container / corpse / vehicle storage (dev, ignores distance) |
| `inv_editor` | the editor panel: item set, fill, run intents, fit visualisation |
| `bake_cells [dir] [quit]` | grid-aspect cell images into `assets/ui/cells/<id>.png` (+ `_r` rotated); `tools/bake_icons/bake_cells.sh` |
| `ui_inv_drag <from> <to> [shift] [rot]`, `ui_inv_click <target> [ctrl\|alt\|shift\|right\|double]` | scripted gestures; targets `c:<self\|loot>:<parent uid\|pockets\|rig\|backpack\|secure>:<loc>:<x>:<y>`, `e:<slot>`, `h:<1-5>`, `a:<attachment slot>`, `discard`, `out` |
| `inv_quick_order` (cvar) | quick-move order |
| `inv_showcase` (cvar) | turn the camera to the player while the screen is open |
| `sv_inv_reach` (cvar, locked) | reach to an open world container (tests raise it) |

## 7. Adding an item / a container

1. `data/items.json`: `"grid"`, `"weight"`, `"equip"` / `"container"` when needed (or rely on the
   class defaults for weapons).
2. Run `bake_cells` (`tools/bake_icons/bake_cells.sh`) and `bake_icons`, commit the PNGs. Without
   a GPU, `python3 tools/bake_icons/placeholder_cells.py` fills the gaps (the item's icon fitted to
   its grid aspect, or a drawn stand-in); the PNGs in the repository for the gear added with the
   grid inventory are such placeholders until the next bake.
3. A new root container (a world container kind): `data/containers.json`, then `"grid": "<id>"`
   on the level entity.
4. `ctest -R "inventory|lint_inventory|ui"` (the rule matrix picks up every item and container).

## 8. Cell images

Each item has an image at its grid aspect (a 4×2 rifle is a 4:2 picture), packed on shelves in one
atlas at 64 px per cell (`inv_cells.c`). Source order: the item's `"cell_image"`, then
`assets/ui/cells/<id>.png` (any size; resampled to the rect), then a live bake through the UI item
view renderer (one per frame). Rotated items draw the same image turned a quarter
(`gd_image_rot90`); `bake_cells` also writes `<id>_r.png` for tools. The HUD hotbar and the
attachment chips use the cell images too.

## 9. Tests

* `test_inventory` (4500+ checks): definitions and class defaults; every item × every container ×
  rotation against the filters / sizes; rotation blocked / allowed; stack merge with a remainder
  and a split drag (conservation); nesting (cycles, same-size packs, `no_containers`, kind
  filters); quick-move order (and `inv_quick_order`), across to a crate; swaps (grid and
  equipment); attachment footprints (suppressor 4×2 → 5×2 rejected where it doesn't fit), fold,
  unload; every weapon × attachment mount / unmount; optics swap; extended magazine rounds;
  sort; secure container on death and the corpse tree; the wire format; weight totals; a fuzz of
  20000 random intents (garbage included: the invariants and conservation always hold); two
  clients looting one crate through a 150 ms round trip with conflicting intents (nothing
  duplicated or lost, every copy converges, conflicts rejected).
* `test_inventory_ui`: the layout at 16:9 / 21:9 / 4:3 / 720p / 4K / 3440×1440 with and without
  loot (inside the safe area, no overlaps, usable cells, an 8-wide trunk fits); pad navigation
  reaches every target at 16:9 and 21:9; the drag-and-drop state machine for every source →
  target (cell, equipment, hotbar, attachment chip, discard, outside, panel), rotation while
  dragging, split, clicks (Ctrl / Alt / right / double), Esc, the pad carry.
* `test_ui`: an icon and a cell image for every item.
* `lint_inventory.py`: theme tokens only in the screen; nothing outside the inventory reads the
  weight (so speed is the same with an empty and a full inventory); items.json completeness.
* Scripted: `ui_inventory.cfg` (every gesture, `inv_dump` after each, screenshots),
  `ui_crate.cfg` (crate + gear locker), `ui_resolutions.cfg` (the screen per resolution),
  `inv_net.sh` (host + two clients, `net_fakelag 150`, checked by `inv_net_check.py`).
