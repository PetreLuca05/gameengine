# Inventory (grid model)

Tarkov-style grid mechanics on a deliberately simple model:
- Items take rectangles of cells and rotate 90°.
- A player has six equipment slots (primary, secondary, holster, backpack, helmet, body armor) and
  one grid: the worn backpack's.
- Nothing nests: no rigs, cases, pouches or containers inside containers.
- There is no weight and no hotbar.
- World containers (crates, cabinets, car trunks, corpses, the fabricator) are one grid each, on
  the same screen.
- Host-authoritative: every change is an intent.

| Part | Code |
|---|---|
| Tree, footprint, the worn pack and the grid (pure) | `src/gameplay/inventory/inv_tree.{h,c}` |
| Rules: equipment slots, filters, where items may go, free-space search (pure) | `src/gameplay/inventory/inv_rules.{h,c}` |
| Intents: apply / validate across two trees, the backpack rules, the hands, the pickup rule (pure) | `src/gameplay/inventory/inv_intent.{h,c}` |
| Wire: tree + intent encoding (pure) | `src/gameplay/inventory/inv_wire.{h,c}` |
| Format-1 save migration (pure) | `src/gameplay/inventory/inv_migrate.{h,c}` |
| Gameplay facade (`Inventory`, selections, pickups) | `src/gameplay/inventory.{h,c}` |
| Item / container definitions | `data/items.json`, `data/containers.json`, `src/gameplay/items.c` |
| Armor plates, helmet | `src/gameplay/health.c` (`health_sync_armor`, `apply_damage_ex`), docs/DAMAGE.md "Armor" |
| World containers, corpses, vehicle trunks, host intent handling, loot sync | `src/gameplay/entities.c` (`container_tree`), `src/game/game_events.c`, `src/net/net.c` |
| Screen: layout + drag and drop (pure), panels, inspect, context menu, gamepad | `src/ui/inventory/{inv_layout,inv_dd,inv_screen}.c` |
| The character render on the screen | `src/gameplay/characters.c` (`chars_preview`), docs/CHARACTERS.md "Character preview" |
| HUD weapon list, health / armor | `src/ui/hud.c` (`draw_weapon_list`, `draw_vitals`) |
| Cell images (grid-aspect icons, baker) | `src/ui/inventory/inv_cells.c`, `tools/bake_icons/bake_cells.sh` |
| Editor panel | `src/ui/inventory/inv_editor.c` (`inv_editor`) |
| Drops (G tap, discards, downed) | `src/game/game_events.c` (`drop_held`, `spawn_dropped`), `src/gameplay/entities.c` (`spawn_pickup_tree`, `pickup_use`) |
| Tests | `tests/test_inventory.c`, `tests/test_inventory_ui.c`, `tools/tests/lint_inventory.py`, `ui_inventory.cfg`, `ui_crate.cfg`, `inv_drop.cfg` + `inv_drop_check.py`, `inv_net.sh` + `inv_net_check.py`, `inv_camera.cfg` + `inv_camera_check.py`, `hud_vitals.cfg` + `hud_vitals_check.py` |

## 1. Data model

### Equipment slots (`InvEquipSlot`)

| Slot | JSON `"equip"` | Takes |
|---|---|---|
| `EQ_PRIMARY`, `EQ_SECONDARY` | `primary` | long guns, the bat (a long gun fits either; the pickup rule fills the primary first) |
| `EQ_HOLSTER` | `holster` | pistols, the knife |
| `EQ_BACKPACK` | `backpack` | the three backpacks: the body's grid |
| `EQ_HEADWEAR` | `headwear` | the combat helmet |
| `EQ_BODY_ARMOR` | `body_armor` | the plate carrier |

- The first three are the **selections** 1-3 (keys 1-3, `UserCmd.slot`); selection 4 is the hands.
- A **body** root (`data/containers.json` `body`, container 0) has these slots and no grid of its
  own.
- A world container's root is one grid.

### Item definition (`ItemDef`, items.json)

| Field | JSON | Meaning |
|---|---|---|
| `grid_w`, `grid_h` | `"grid": [w, h]` | Cells, unrotated. Missing: the class default (below). |
| `rotatable` | `"rotatable"` (default true when w != h) | R / X rotates it. |
| `max_stack` | `"stack"` | Stack size. |
| `equip_slot` | `"equip"` | The slot above. Long guns default to `primary`, pistols and the knife to `holster`, the bat to `primary`. |
| `container` | `"container": { "compartments": [ { "w": 10, "h": 6 } ] }` | Backpacks only: their grid (one compartment). |
| `default_pack` | `"default": true` | The bag every body wears when it wears no other (the Sling Bag). |
| `plates` | `"plates"` | Body armor: the armor plates it holds (docs/DAMAGE.md "Armor"). |
| `category` | `"category"` | The category bar: `weapons`, `attachments`, `medical`, `throwables`, `tools`, `misc`. Missing: from the kind (below). |
| `hands` | `"hands": true` | A hand tool taken into the hands like a consumable (the flashlight). |
| `use_type`, `charges` | `"use": "consume"` / `"charges"`, `"charges"` | Consumables: one per use, or charges per item (a medkit with 3 uses). |
| `durability` | `"durability"` | Max points (0 = none); instances start full. The helmet's protection. |
| `fold_dw` | `"fold": 1` | Columns the weapon loses when its stock is folded (rifle, SMG). |
| `cell_image` | `"cell_image"` | Override PNG for the grid image (else `assets/ui/cells/<id>.png`, else a live bake). |

**Class defaults** (`grid_defaults`, applied after the weapons load):

| Class | Size |
|---|---|
| Pistol | 2×1 |
| SMG | 3×2 |
| Shotgun, rifle, crossbow | 4×2 |
| DMR, LMG, sniper | 5×2 |
| Bat / knife | 1×3 / 1×2 |
| Medkit, tire kit | 2×2 |
| Fuel can | 2×3 |
| Spare tire | 3×3 |
| Ammo, small consumables, keys, attachments | 1×1 |
| Scopes | 2×1 |

**Categories by kind:**

| Category | Items |
|---|---|
| Weapons | weapons and ammo |
| Attachments | attachments |
| Medical | consumables that aren't vehicle items (heal / armor / food / water) |
| Throwables | grenades, flares, molotovs |
| Tools | keys, vehicle items, `"hands"` tools, cameras; the syringe and fishing kit say so explicitly |
| Misc | everything else |

**Attachments** change a weapon's footprint: `"grid": [dw, dh]` in the attachment JSON (default:
muzzle devices longer than 10 cm add a column, so a suppressor makes a 4×2 rifle 5×2).

### Backpacks

| Item | Grid | Lying in a grid | |
|---|---|---|---|
| `sling_bag` Sling Bag | 10×4 | 2×2 | `"default": true`: everyone wears it when they wear no other |
| `backpack` School Backpack | 10×6 | 3×3 | Chapter 1's coat closet |
| `backpack_hiking` Hiking Backpack | 10×8 | 4×5 | |

Every pack is ten columns wide, so a bigger one only adds rows. The rules (`inv_intent.c`):

* **Always one worn.**
  - Spawning, respawning, loading a save and `inv_clear` give a body the default bag
    (`inv_ensure_pack`).
  - Every intent ends with one worn.
* **The default bag never moves.** It can't be discarded, dragged, quick-moved or taken from a
  corpse (`IIR_DEFAULT_PACK`). It is never loot (`test_inventory` checks the levels). A second one
  isn't taken.
* **Putting a pack on** (drag onto the slot or the character, Alt+click, Equip, or picking up a
  pack while wearing the default bag):
  - The worn pack's contents move into the new one, each in its own cell when that is inside the
    new grid and free, the rest packed largest first.
  - The default bag is then simply gone.
  - A replaced School / Hiking pack goes where the new one was, or into the new grid when that was
    inside the old one.
  - Bigger packs always take the contents over (same columns, more rows); a smaller one refuses
    with `IIR_NO_ROOM` when they don't fit.
* **Taking a pack off** (dragged to the grid or a crate, discarded, quick-moved into a crate): its
  contents move into a fresh default bag the same way, or it is refused (`IIR_NO_ROOM`). A
  discarded pack drops empty: the contents stay with the player.
* **Nothing nests.**
  - Items go only into a tree's grid: a world container's root, or a body's worn pack
    (`inv_check_place`; anything else is `IIR_NESTING`).
  - A pack anywhere but on a back is an empty item.
  - A corpse's full pack is `IIR_NOT_EMPTY` until its contents are taken.
  - `Take all` takes the contents, then the weapons and gear, then the emptied pack.
* **Corpses are looted, not dressed:** nothing goes into a loot tree's equipment slots (`IIR_EQUIP`).

### Instances (`InvItem`; 21 bytes on the wire)

```c
typedef struct InvItem {
    InvSlot s;       // item, count, ammo (rounds loaded / DNA id / charges used / a carrier's plates), att[5]
    u16 uid;         // unique within its tree (0 = free)
    u16 parent;      // 0 = the tree root (equipment slots, a world container's grid), else the worn pack's uid
    u8 loc;          // INV_LOC_EQUIP + slot (root only) or the compartment (always 0)
    u8 x, y;         // top-left cell
    u8 flags;        // INVF_ROT (90°), INVF_FOLDED
    u16 dur;         // durability left
} InvItem;
```

A tree (`InvTree`) is a pool of 128 instances plus its root container def. The player's `Inventory`:

```c
typedef struct Inventory {
    InvTree t;
    u16 hands;       // the grid item in the hands (uid, 0 = none)
    InvSlot none;
    u32 revision;
} Inventory;
```

* **Footprint** = def grid + attachment deltas - fold, swapped when rotated (`inv_item_size`).
* **The grid** of a tree is `inv_grid`: a body's worn pack (its uid), a world container's root (0).
  `inv_grid_size` gives its cells.
* **Stacking:** same def, both stackable: merge up to `max_stack`, the remainder stays.
* **The hands** reference an instance, never a cell.
  - The healing / throwable wheels and Hold put a grid item there (`II_HOLD`).
  - Only `inv_holdable` items: consumables, throwables and `"hands"` tools. Weapons are drawn from
    their slots, and the built-in phone has its own key.
  - The item stays in its cell. Moving it keeps it in the hands; leaving the tree (used up, dropped,
    moved to a crate) clears the hands.
* **Death:** everything moves into the corpse in the same places (`inv_strip_to_corpse`): the corpse
  wears the weapons, helmet, plate carrier and the pack with its contents. The body gets a fresh
  default bag.
* **Charges:** `"use": "charges"` items count the charges used in `ammo` (a medkit: 3 uses).
* **Dev tools** (hold point / hand pose editors, sight tests): `inv_dev_sel` writes a selection's
  payload directly (a weapon slot, or a detached instance in the hands, `loc` `INV_LOC_NONE`).

### Selection (`game_select`, `src/game/game.c`)

* Keys 1-3 draw the primary / secondary / holster weapon (an empty slot: empty hands).
* The mouse wheel cycles the slots that hold a weapon.
* The wheels put a grid item in the hands (selection 4); when it is used up or leaves the grid, the
  last weapon comes back.
* A weapon newly in a slot is drawn when nothing is in hand (the first gun picked up, a looted rifle).
* `slot <1-4>` selects; `wield <item>` takes a carried item in hand (a weapon into its slot and
  drawn, anything else into the hands). Test scripts use `wield`.

## 2. Rules (`inv_rules.c`, `inv_intent.c`)

| Rule | Check |
|---|---|
| Fit | The footprint lies inside the grid and overlaps nothing but the moving item. |
| Where | Only a tree's grid (`inv_grid`) or an equipment slot; never inside a pack lying in a grid. |
| Rotation | Only when `rotatable`; R while dragging / X on a pad. |
| Equip | The slot accepts the item's `equip_slot` (primary / secondary both take `primary`). |
| Filters | World containers: allow / deny kinds, deny items, `max_item`. |
| Stack merge | Same def, `max_stack` > 1. |
| Swap | Dropping a single item on a different single item: both must fit in each other's place. |
| Attachments | Mounting re-checks the weapon's new footprint where it lies ("No room"); a worn weapon has no footprint to check. |
| Quick move (Ctrl+click) | Loot open: across (player → loot root, loot → the pickup rule). Loot closed: a worn weapon → the first free cell, a grid weapon → a free matching slot. |
| Pickup (`inv_add`) | Merge into the grid's stacks; a weapon / helmet / carrier into a free matching slot; a pack over the default bag; else the first free cell (row-major, unrotated first). |
| Compact (`II_COMPACT`) | Re-pack a grid: largest first, reading order kept among equal sizes; all or nothing. |

## 3. Intents (`inv_intent.h`)

Every change is an `InvIntent` applied by `inv_intent_apply(ctx, intent)`. The host applies it and,
as a prediction, so does the client. `ctx` holds the two sides:
- `INV_SIDE_SELF`: the player's tree and hands;
- `INV_SIDE_LOOT`: the open world container's tree, or none.

| Intent | Fields | Result |
|---|---|---|
| `II_MOVE` | src side / uid, dst side / parent / loc / x / y / rot, count | Place, merge onto a stack, or swap with a single item; count < stack = split (Shift-drag). Onto the backpack slot: put the pack on. |
| `II_ROTATE` | src | Rotate in place (when it still fits). |
| `II_EQUIP` | src (`aux` = slot, 255 = its own) | Into the equipment slot (an occupant swaps out to the source cell when it fits; a backpack: the pack rules). |
| `II_QUICK_MOVE` | src | The quick-move rule above. |
| `II_SPLIT` | src, count, dst | Count off the stack into dst (or the first space). |
| `II_DISCARD` | src, count | Removed into `InvEffect.dropped`; the host spawns the pickup. A worn pack comes off first. |
| `II_USE` | src | Consumables (heal / armour): host effect. |
| `II_MOUNT` / `II_UNMOUNT` | weapon uid, attachment uid / slot | Footprint re-checked. |
| `II_FOLD` | weapon uid | Fold / unfold the stock (footprint changes). |
| `II_UNLOAD` | weapon uid | Rounds back into an ammo stack. |
| `II_HOLD` | src uid (0 = put away) | Into the hands (`IIR_NOT_USABLE` unless `inv_holdable`). |
| `II_TAKE_ALL` | — | Everything of the loot tree that fits, through the pickup rule: its grid, its weapons and gear, its emptied pack. What stays is reported on the screen ("No room for N items"). |
| `II_COMPACT` | dst side | Re-pack that side's grid. |

**Results:** `IIR_OK`, `IIR_NOOP`, `IIR_INVALID`, `IIR_NO_ROOM`, `IIR_FILTER`, `IIR_NESTING`,
`IIR_GONE`, `IIR_TOO_FAR`, `IIR_FULL`, `IIR_EQUIP`, `IIR_NOT_USABLE`, `IIR_INCOMPATIBLE`,
`IIR_NO_SLOT`, `IIR_NOT_WEAPON`, `IIR_NOT_ROTATABLE`, `IIR_NOT_EMPTY`, `IIR_DEFAULT_PACK`.
- The text for each is shown in the footer and on the shaken target.

## 4. Networking

Every intent is applied to scratch copies of the trees and committed only when it succeeds, so a
rejected one changes nothing.

* **Client → host:** `MSG_INV_INTENT` (reliable): the packed intent (`inv_wire.c`), sequence.
* **Host → owner:** `MSG_INVENTORY`: slot, revision, last applied sequence, the hands uid, the live
  instances.
  - The client takes it, drops acknowledged intents and re-applies the rest.
  - A changed `MSG_LOOT` goes out before it.
* **Drops:** the G tap sends `RPC_INV_DROP` (a = the selection).
  - The host discards the held item (`drop_held`): the drawn weapon, or the item in the hands.
  - Downed players drop the held item the same way (`revive.c drop_slot`), unless it is a sidearm
    the rules keep.
* **Loot:**
  - Opening a container (`EV_CONTAINER`) sets `Player.open_container`.
  - The host validates the distance (`sv_inv_reach`, 3.5 m; vehicles + 1 m) on every loot intent.
  - It sends `MSG_LOOT` to every player with that container open whenever its revision changes.
  - Walking away closes it; the screen closing sends `RPC_CONTAINER_CLOSE`.
* **Conflicts:**
  - The host is the only writer; intents apply in arrival order.
  - A loot intent whose source uid is gone, or whose cells are taken, is rejected (`IIR_GONE` /
    `IIR_NO_ROOM`), and both clients converge on the next `MSG_LOOT`.
  - Moving between trees is one host step, so items are never duplicated or lost. The two-client
    test runs this under 150 ms lag.
* **Armor:** `NF_HP` carries `max_armor` (the plate cap) for the HUD.
* **Protocol 34** (`DG_PROTOCOL_VERSION`): the simplification (the hands uid in `MSG_INVENTORY`,
  `UserCmd.slot` = the selections, the intent types renumbered, `max_armor`).

### Saves

Story saves keep the trees in **format 2**:

```
{"v": 2, "root": "<container id>", "next": n,
 "items": [["<item id>", count, ammo, uid, parent, loc, x, y, flags, dur, att0..att4] ...]}
```

- Items are stored by id, so items added or removed later don't shift anything.
- Each player's entry stores its `"hands"` uid.

**Format 1** (an array of item indices in the old items.json order) is rebuilt by `inv_migrate_v1`:
- the weapons, helmet, plate carrier and worn backpack go back in their slots;
- a bat or knife in the old melee slot goes to a free weapon slot;
- the worn pack's contents keep their cells;
- everything else that still exists goes into the grid: pockets, the rig's / secure container's /
  cases' contents, spare packs;
- deleted items and overflow are dropped with a `story: save migration` warning.

World container saves migrate the same way (a three-drawer cabinet into its one grid).

## 5. Screen (`src/ui/inventory/`)

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│ INVENTORY                                                                    syncing…    │
├─────────────────────────┬─────────────────────────────────┬──────────────────────────────┤
│ PRIMARY             AUTO│                                 │ ALL WEAPONS ATTACHMENTS …    │
│ ▬▬▬▬ Assault Rifle 30·90│        the player character     │ TOOLS MISC        [COMPACT]  │
│ [OPT][MUZ][UND][MAG][STK]│      (a second render: idle,    │ CRATE / BODY / TRUNK 6 × 4   │
│ SECONDARY               │       worn gear, its own light, │ (open only; a body: its gear)│
│ [OPT][MUZ][UND][MAG][STK]│       head to mid-thigh)        │ ┌──────────┐   [Take all]    │
│ HOLSTER                 │                                 │ └──────────┘                 │
│ [HELMET][ARMOR][PACK]   │  drop a weapon / gear on it:    │ HIKING BACKPACK  7 / 80 cells│
│ INSPECT                 │  it is worn                     │ ┌────────────────────┐       │
│ (3D item, chips, stats) │          [ DISCARD ]            │ └────────────────────┘ scrolls│
├─────────────────────────┴─────────────────────────────────┴──────────────────────────────┤
│ hints  ·  last rejection                                                                 │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

* **The camera never moves.**
  - Opening the inventory changes nothing in the view: the game keeps rendering in first person
    (body, arms, viewmodel) behind it.
  - A full-screen UI blur (`gd_backdrop`) under the theme's `scrim` covers it.
  - The simulation pauses or runs as before (the pause menu rule).
  - `tools/tests/inv_camera.cfg` checks the camera's position, rotation and FOV over 60 frames.
* **Left column:**
  - The weapon slots stacked.
  - Primary and secondary each have a row of attachment chips under them: one per attachment slot
    the weapon has, the mounted one or an outline with the slot name. Drag an attachment onto a
    chip to mount it, drag a chip off to unmount it; a wrong one shakes.
  - The holster.
  - The gear row: helmet (durability), body armor (plates held / cap), backpack (its grid size).
  - The **inspect panel** docked under them: the selected item turning in 3D (drag to turn, wheel
    to zoom), its attachment chips, its description and stats.
* **Centre:**
  - A second render of the player character (`chars_preview`): a separate instance of the same
    model with the same cosmetics and the worn helmet / carrier / pack. It plays its own idle pose
    with empty hands, is lit by a fixed light rig, and sits on a transparent background over the
    blur. It is framed head to mid-thigh, facing the camera.
  - Drag on it to turn it; it eases back.
  - Dropping a weapon or gear on it equips it into its own slot.
  - The Discard zone sits along the bottom.
* **Right column:**
  - The **category bar**: All / Weapons / Attachments / Medical / Throwables / Tools / Misc. It only
    dims what is outside the category and never moves anything.
  - **Compact** packs the grid.
  - Under the bar, scrolling, the open world container's grid sits above the player's grid. A body
    shows its equipment row first and Take all.
  - The cell size is the same everywhere (ten cells across the column) and never shrinks for a
    taller grid: the column scrolls.
* **Text** never goes below 10 px (`INV_MIN_TEXT_PX`).
* **No floating windows** except the context menu and the split dialog.

**Mouse:**
- Drag between any grids, slots, chips and the character. The footprint shows green (fits) or red
  (doesn't); **R** rotates while dragging.
- Dropping on a stack merges; on a different single item swaps.
- Ctrl+click quick-moves, Alt+click equips, Shift+drag splits (dialog), double-click inspects.
- Right-click menu: Use, Hold / Put away, Equip, Unequip, Inspect, Fold / Unfold, Unload, Remove
  attachments, Rotate, Split, Take / Put in container, Discard (one / all).
- Dragging out of the screen or onto Discard drops it.
- Rejected drops shake the target and print the reason.

**Gamepad:**
- A cell cursor: d-pad / stick move it (a whole item is one stop).
- LB / RB cycle the regions (slots, chips, loot, grid, inspect chips).
- A picks up / places, X rotates while carrying (the menu when idle), Y quick-moves, B cancels /
  closes.

**Opening:** Tab opens and closes it (a toggle, also I and the pad's Back); Esc closes it. The pause
menu's Inventory entry opens the same screen.

## 6. HUD (`src/ui/hud.c`)

* **Weapon list, bottom left** (theme `layout` tokens `hud_wlist_*`):
  - Three rows, primary / secondary / holster: the key, the cell image, the name, magazine /
    reserve, and the drawn gun's fire mode.
  - The selected row is white (theme `select`); an empty row is a dim outline with the slot's name.
  - Keys 1-3 and the wheel cycle them.
  - While the hands hold a grid item, an "In hands" row shows above.
* **Health and armor, bottom centre** (`hud_vitals_*`):
  - The armor bar above the health bar, one segment per plate the player can hold (1, or 4 with a
    plate carrier). Each has its number.
  - `tools/tests/hud_vitals.cfg` checks the block is centred at 16:9, 21:9 and 4:3.
* The ammo readout stays bottom right.

## 7. Commands

| Command | |
|---|---|
| `inv_give <item> [count]` | Add through the pickup rule. |
| `give <item> [count] [slot]` | The same from the dev console (refuses the built-in phone). |
| `give_all [slot]` | One of every item, ending with a usable loadout: the roomiest backpack, then the weapons each with its ammo, then consumables / throwables, then the rest. |
| `inv_clear [slot]` | Empty down to the default bag. |
| `inv_fill <backpack\|loot> <item> [count]` | Fill a grid's free cells. |
| `inv_dump` | Log the tree (uid, def, parent, loc, cell, rot, count), the grid size, the hands. |
| `inv` | The selections, the backpack, health / armor. |
| `inv_intent <type> ...` | Run an intent (tests). |
| `inv_open <entity>` | Open a world container / corpse / vehicle storage (dev, ignores distance). |
| `inv_wait <loot\|synced\|closed <slot>\|offline> [frames]` | Scripts wait for a state instead of a frame count. |
| `inv_editor` | The editor panel: item set, fill, run intents, fit visualisation. |
| `slot <1-4>`, `wield <item>` | Select a weapon slot / the hands; take a carried item in hand. |
| `bake_cells [dir] [quit]` | Grid-aspect cell images into `assets/ui/cells/<id>.png`. |
| `ui_inv_drag <from> <to> [shift] [rot]`, `ui_inv_click <target> [ctrl\|alt\|shift\|right\|double]` | Scripted gestures. Targets: `c:<self\|loot>:<grid\|uid>:<loc>:<x>:<y>`, `e:<slot>`, `a1:` / `a2:<attachment slot>` (the primary's / secondary's chips), `a:<slot>` (the inspect panel's), `char`, `discard`, `out`. |
| `ui_inv_cat <category\|all>` | The category bar (tests; logs what stays lit). |
| `sv_inv_reach` (cvar, locked) | Reach to an open world container (tests raise it). |

## 8. Adding an item / a container

1. `data/items.json`: `"grid"`, `"equip"` when worn, `"category"` when the kind's default is wrong,
   and its detailed model (`"mesh"`, docs/ITEM_PIPELINE.md).
   - A new backpack: `"equip": "backpack"` and a ten-wide one-compartment `"container"`.
   - A new body armor: `"plates"`.
2. Run `bake_cells` (`tools/bake_icons/bake_cells.sh`) and `bake_icons`, and commit the PNGs.
3. A new root container (a world container kind): `data/containers.json` (one compartment), then
   `"grid": "<id>"` on the level entity.
4. `ctest -R "inventory|lint_inventory"` (the matrix picks up every item and container).

## 9. Cell images

- Each item has an image at its grid aspect (a 4×2 rifle is a 4:2 picture), packed on shelves in
  one atlas at 64 px per cell (`inv_cells.c`).
- Source order: the item's `"cell_image"`, then `assets/ui/cells/<id>.png`, then a live bake through
  the UI item view renderer.
- Rotated items draw the same image turned a quarter (`gd_image_rot90`).
- The HUD weapon list and the attachment chips use the cell images too.

## 10. Tests

* `test_inventory` (4,400+ checks):
  - The schema: exactly the six slots (the deleted names resolve to nothing), the three backpacks
    and the default bag, only backpacks with a grid, one-grid roots, the deleted items gone.
  - Class defaults and categories.
  - Every item × every world container and the default bag × rotation; nothing nests.
  - Rotation, stacks, splits.
  - The backpack rules: over the default bag, contents keep their cells, a smaller pack refused, a
    discarded pack drops empty, a corpse's full pack, a corpse not dressed.
  - Quick move and swaps; the hands.
  - Attachment footprints and every weapon × attachment; folding, unloading.
  - Compact; death into a corpse and Take all from it.
  - The wire format; format-1 save migration (a body and a three-drawer cabinet).
  - A fuzz of 20,000 random intents: the invariants hold, nothing is created or lost, and the grid
    changes size only when the worn pack changes.
  - Two clients looting one crate through a 150 ms round trip (nothing duplicated or lost, every
    copy converges).
  - Every stocked world container (levels, building-kit prefabs) holding its loot.
* `test_inventory_ui`:
  - The layout at 16:9 / 21:9 / 4:3 / 720p / 4K / 3440×1440, for every pack, with and without a
    trunk or a corpse: inside the safe area, no overlaps, the chips under their weapons, the loot
    above the grid, one cell size, a tall stash scrolls.
  - Pad navigation reaches every target.
  - The drag-and-drop state machine for every source → target (cell, slot, chip, the character,
    discard, outside, panel); rotation while dragging, split, clicks, Esc, the pad carry.
  - The category bar only dims.
* `lint_inventory.py`:
  - Theme tokens in the screen and in the HUD weapon list / vitals.
  - No deleted slot / field / API anywhere in code, tests, scripts or data.
  - No `"weight"` key.
  - The six slot names; only ten-wide backpacks have grids, one default; one-grid roots.
* **Scripted:**
  - `ui_inventory.cfg`: every gesture with `inv_dump` after each, screenshots.
  - `ui_crate.cfg`: the attachment crate, then the gear locker: Alt+click the pack on, the carrier
    dropped on the character, the helmet onto its slot.
  - `ui_resolutions.cfg`: the screen per resolution.
  - `inv_drop.cfg` + `inv_drop_check.py`: the G tap drops the drawn rifle and it comes back; a
    discarded pack drops empty and comes back with its contents.
  - `inv_net.sh`: host + two clients, `net_fakelag 150`, checked by `inv_net_check.py`.
  - `inv_camera.cfg` + `inv_camera_check.py`: Tab opens and closes the screen, and the camera
    doesn't move for 60 frames.
  - `hud_vitals.cfg` + `hud_vitals_check.py`: the vitals centred at three aspects.
