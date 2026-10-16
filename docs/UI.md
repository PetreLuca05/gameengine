# Game UI

The game's menus, inventory and HUD: a small retained-layout plus immediate-widget UI written
for this engine (`src/ui/gui`), screens in `src/ui/screens`, data in `assets/ui`. The console and
the developer editors (VFX, hold point, day/night, flesh range) still use Nuklear.

## Screen map

The main menu, the lobby, the loading screen and the in-game chat are in docs/MENU_LOBBY.md (they use
this document's theme, widgets and layouts; the pause menu gains Lobby and Leave to main menu).

```
in game ── Esc / pad Start ──▶ PAUSE
                                 button stack (bottom left)       logo, version, ping (bottom right)
                                 ├─ Resume
                                 ├─ Inventory ─────▶ INVENTORY (centred modal)
                                 ├─ Settings ──────▶ SETTINGS  (centred modal, tabs, scroll)
                                 │                     └─ display change ─▶ CONFIRM (keep? 15 s, reverts)
                                 ├─ Multiplayer ───▶ MULTIPLAYER (host LAN / Steam, connect, LAN list)
                                 ├─ Invite friends   (Steam builds only)
                                 ├─ Leave game / Stop hosting ─▶ CONFIRM
                                 └─ Quit to desktop ───────────▶ CONFIRM
in game ── Tab / I / pad Back ──▶ INVENTORY (full screen dim, same modal)
                                     └─ drop a non-stackable outside ─▶ CONFIRM
world: crate (hold E) ──▶ INVENTORY with the crate panel
world: weapon bench   ──▶ BENCH (free attachment changes, stats)
```

* **Esc / B** pops the top screen; popping the last one resumes. Only the top screen receives
  input (`gui_push_input`), the screens below stay drawn. Opening a sub-menu keeps the pause stack
  visible with the entry highlighted; back returns the focus to that entry.
* **Pause:** with no remote players (single player, or a host alone) the pause menu stops the
  simulation (`Game.paused`, `menus_can_pause`). In multiplayer it opens without pausing.
* The **HUD hides** while any screen is open.

## Architecture

| Module | What it does |
|---|---|
| `src/ui/gui/gui_layout.c` | Retained layout: a flat node tree (parent before child) solved into rects. Anchors (9-point + offset), row / column stacks (gap, padding, cross alignment incl. stretch, flex shares), fixed / percent / fit sizes, min / max (incl. percent of the parent), aspect ratio, safe area, `avoid`. Pure (tested). |
| `src/ui/gui/gui_draw.c` | Draw list in three layers (base, overlay for popups, top for tooltips / drag ghost): rects, gradients, SDF rounded rects and circles (9-slice of an SDF circle, crisp at any radius), outlines, soft-shadow 9-slices, rings, images, the blurred backdrop, SDF text. One draw per texture / clip change. |
| `src/ui/gui/gui.c` | Frame input (mouse, keyboard, gamepad incl. stick auto-repeat), ids, hover resolved topmost-first (1-frame latency, `gui_block_rect` for opaque popups), press / click / double click, spatial keyboard + gamepad focus with scopes, tweens, UI sounds, tooltips, `ui_debug`. |
| `src/ui/gui/gui_widgets.c` | Menu item (slide + fade in, accent bar), buttons, tabs (animated underline), the top tab bar (`gw_tabbar`: key hints, locked tabs with a reason), the power icon, avatars, slider, toggle, dropdown (overlay list; `gw_dropdown_ex`: greyed items + right-aligned notes), key-bind button, text field, scroll region (wheel, draggable bar, keeps the focus visible), settings row, inventory slot. `gw_disabled(true)`: following rows / sliders / toggles / dropdowns faint and without input. Sliders only snap / change on a drag or key press. |
| `src/ui/gui/gui_theme.c` | `assets/ui/theme.json`. |
| `src/ui/menus.c` | Screen stack, pause screen (from `assets/ui/screens/pause.json`), confirm dialog, commands. |
| `src/ui/screens/` | Settings, multiplayer, inventory, bench. |
| Settings → Graphics | Engine Fidelity card on top (docs/ENGINE_FIDELITY.md), then Display, View, Resolution & upscaling, Post-processing & Effects (the former Post-processing tab), Shadows & lighting, Geometry / textures / foliage, Effects, Sights, Weather. Bounded rows (`row_fid_*`): options above the level greyed with "requires &lt;level&gt;", features the level lacks locked with "Requires Engine Fidelity: &lt;level&gt;". |
| `src/ui/inv_dragdrop.c` | Inventory drag and drop state machine. Pure (tested). |
| `src/gameplay/inv_ops.c` | Inventory intents: the one implementation of every change. Pure (tested). |
| `src/ui/item_view.c`, `src/ui/icons.c` | Item models for the UI, the icon atlas and bakes. |
| `src/render/render_ui.c` | Backdrop blur, offscreen item views, PNG readbacks. |
| `src/ui/hud.c` | The HUD. |

The UI frame starts in `menus_tick` (`gui_frame_begin`: input edges, DPI scale, draw list reset),
screens and the HUD draw during `game_render`, and `ui_render` flushes the old HUD batch
(crosshair, dev overlays), then the game UI (`gui_frame_end`: navigation, tooltip, flush), then
Nuklear.

## Style

Dark and minimal. Panels are semi-transparent over a blurred copy of the game; labels are small
caps with tracking; one accent colour (`accent`, orange). Item types have their own tints (weapon,
ammo, consumable, attachment, key, misc).

**Font:** [Barlow](https://github.com/jpt/barlow) by Jeremy Tribby (Regular, Medium, SemiBold), SIL
Open Font License 1.1 (`assets/ui/fonts/OFL.txt`). It was chosen because it is a slightly rounded
grotesk with narrow proportions: compact labels, clear figures for ammo and stats, and readable in
small caps at 12 px as well as at 4K.

**SDF text:**
* The glyphs are baked once at startup into a 2048² atlas (44 px, 6 px spread; about 600 glyphs
  covering Latin-1 plus some symbols, in about 200 ms).
* Kerning comes from the font's GPOS / kern tables (ASCII pairs, precomputed).
* Text shadows reuse the same glyphs with a wide soft edge.
* The distance field is computed from a 4× rasterized coverage bitmap with an exact Euclidean
  distance transform, not from stb_truetype's analytic SDF. Barlow's static instances have
  overlapping contours, and the analytic SDF carves seams along them.
* `shaders/ui.frag` takes its derivatives from the uv in uniform control flow, and minified
  (small) text uses 4 rotated-grid samples. Thin strokes stay solid at 12 px.

**Scale:**
* Design pixels are 1080p. The auto scale is `height / 1080`, keeping at least 1280 design pixels
  across on 4:3. `ui_scale` overrides it.
* The safe area is a margin (`safe_frac`, or `ui_safe_area`) plus an aspect limit (`max_aspect`
  2.39). On 21:9 and wider, the menus and the HUD stay near the centre of view.

## Data

* **`assets/ui/theme.json`:**
  * colours (RGBA 0–255), type sizes, metrics;
  * animation times: menu slide-in 0.15 s with a 22 ms stagger, hover 0.08 s, modal 0.14 s,
    rejection shake 0.35 s, tooltip delay 0.35 s;
  * backdrop blur;
  * UI sound names (`assets/sounds/ui_*.wav`) and their volume (plus the `snd_ui` volume setting);
  * font files.
* **`assets/ui/screens/pause.json`:** the pause layout.
  * Nodes with an `action` are the button stack, in order; `label` is their text.
  * `if` hides a node (`steam`, `online`, `world`, `!` negates).
  * `modal` is where sub-menus open: 60 % of the width, 640–1160 design px, `avoid: stack` so it
    moves beside the stack on 4:3.
  * `dialog` is the confirm box.
* **`"layout"` tokens** (theme.json): named design-px sizes; a screen JSON size may be `"@name"` (the main menu
  screens use nothing else, linted by test_lobby), code reads them with `gui_tok`.
* **Hot reload:** `ui_reload` reloads the theme, fonts, screen layouts and icons.
* **Item JSON:**
  * `"icon": "path.png"` overrides an item's icon.
  * Attachment items are created automatically, one per `assets/attachments/*.json`
    (`att_<id>`), unless `data/items.json` defines one with `"kind": "attachment"` and
    `"attachment": "<id>"`.

## Inventory

**Layout.** The left side holds the backpack (6 × 4) and the hotbar (1–9) at the same total width.
Slots show:
* the icon;
* the stack count;
* the type tint (bottom strip);
* for guns, a magazine bar and the rounds left (the loaded weapon shows the live magazine);
* the hotkey number.

The right side holds the preview:
* a live 3D view: auto-rotates when idle, drag to rotate, wheel to zoom;
* attachment slots (one chip per slot the weapon has);
* name, type, description;
* stats with attachment deltas against the bare weapon. Green is better and red is worse;
  "lower is better" stats are inverted.

With a crate open, the crate replaces the preview.

**Mouse** (`ui/inv_dragdrop.c`):

| Gesture | Result |
|---|---|
| click | select (preview) |
| drag (6 px threshold) | the item follows the cursor as a ghost; the source slot dims |
| release on a slot | move: merge (the remainder stays), swap, or into an empty slot |
| Ctrl + drag | carry half the stack (split) |
| release on the hotbar | assign (a move) |
| attachment onto a weapon slot / onto the preview / onto the matching attachment chip | mount (the item is consumed; a replaced attachment comes back in its place) |
| incompatible target | rejected: the target shakes red, the reason shows in the footer ("Doesn't fit this weapon", "This weapon has no slot for it", "Takes no attachments") |
| attachment chip dragged out | unmount (onto the slot it was dropped on, or the first free one) |
| release outside the panel | drop into the world; non-stackable items (weapons, keys, attachments) ask first |
| right click | context menu: Use, Equip, Split, Move to hotbar / backpack, Remove attachments, Drop one / Drop, Drop all |
| Shift + click | quick move (hotbar ↔ backpack, merging into existing stacks first) |
| double click | equip (to the first free hotbar slot, or swapped with the selected one, and selected) |
| crate | drag into / out of the crate panel, Shift+click takes, "Take all" |

While dragging, drop targets light up (accent) or show red when they would reject. Dragging an
attachment highlights the matching socket on the 3D model (a glowing marker, plus a leader line
from the chip).

**Gamepad / keyboard.**
* The focus is the cursor: d-pad, stick or arrows move between slots and chips.
* A / Enter picks an item up and places it (the same rules as a mouse release).
* X opens the context menu; Y quick-moves.
* B / Esc cancels a carry, then closes.
* Tab, I or pad Back closes.

### Inventory intents (network)

Every change is an `InvOp` (`gameplay/inv_ops.h`): move, split, drop, use, equip, quick move,
mount, unmount. It goes to the host as `RPC_INV_OP` with a sequence number.

* **Host:** applies it with `inv_op_apply`, then does the world effects (the dropped pickup,
  healing). It always answers with the inventory and the last applied sequence (`MSG_INVENTORY`
  carries the acknowledgement; protocol 9), even for a rejected intent.
* **Client:** applies the same function to its copy at once (prediction) and keeps the intent.
  On every inventory from the host it takes the host's slots, drops the acknowledged intents and
  re-applies the rest (`game_cl_inventory`). A rejected prediction is corrected by the next
  message.
* **Host's own player:** applies directly.
* The UI shows "syncing…" while intents are pending.

## HUD

All HUD elements sit in the safe area:
* **Vitals** (bottom left): small-caps label, a slim health bar (pulses red under 30 %), the
  value, and an armour bar.
* **Ammo** (bottom right): weapon name, magazine (large; amber below 25 %, red when empty), the
  reserve, magazine ticks, and the fire mode when the weapon has several.
* **Hotbar** (bottom centre): the inventory's slot widget, the selected slot lifted; the item name
  fades in on switch.
* **Interaction prompt** (under the crosshair): a key cap plus text, with a hold-progress ring
  around the key.
* **Reload / use progress:** a ring around the crosshair.
* **Hints** (lower centre), **kill feed** (top right, with an accent bar), damage direction
  arrows and flash.
* **Hit markers and crosshair:** `crosshair.c`, unchanged.

## Icons and preview

**Item models.** `item_view_parts` builds an item's parts the way the hands draw it:
* weapons: the effective weapon's boxes plus the mounted attachments' parts;
* attachment items: the attachment's parts;
* other items: their box or cylinder.

`render_ui3d` renders such a part list into an offscreen RGBA8 target (`src/render/render_ui.c`,
pass `ui_views`):
* its own camera;
* studio lighting (key, cool fill, warm rim, hemispheric ambient), independent of the world's time
  of day;
* sRGB-encoded output, a transparent background, and 2× supersampling (the UI samples it
  downscaled).

**Preview:** a perspective orbit camera.

**Icons:**
* An orthographic 3/4 view, bare weapons, transparent background.
* `tools/bake_icons/bake_icons.sh` runs the `bake_icons [dir] [quit]` command. It writes
  `assets/ui/icons/{128,256}/<id>.png` into the source tree; commit them.
* At startup `icons.c` packs one atlas (128 px tiles, 256 px when the UI scale is over 1.4). For
  each item it uses, in order: the item's `icon` override, the baked PNG of the tile size, the
  other size resampled.
* An item with neither is rendered live into its tile, one per frame, so new items get an icon
  before anyone runs the baker.

**Backdrop:** while a modal panel draws, the UI asks for `render_ui_backdrop()`. The final image
is box-downsampled to a quarter resolution, display-encoded, and blurred twice with a separable
17-tap Gaussian (`shaders/post/ui_blur*.comp`, pass `ui_blur`). Panels sample it in screen space.
It costs nothing when no panel is open.

## Commands and settings

| | |
|---|---|
| `ui_screen <name>` | open a screen: `pause`, `settings[:tab]`, `multiplayer`, `inventory`, `bench`, `confirm_quit`, `confirm_leave`, `close`. No args: list plus the current stack |
| `menu <page>` | the same through the pause stack (also `controls`, `crosshair`, `audio`, `character`) |
| `inventory` | toggle the inventory |
| `ui_reload` | theme, fonts, screen layouts, icons |
| `ui_debug 1` | widget / focus / layout-node rects, the safe area, a status line (scale, hover, focus, focusables, draw commands, vertices) |
| `ui_scale <s>` | 0 = automatic |
| `ui_safe_area <f>` | margin fraction, -1 = theme |
| `snd_ui` | interface sound volume |
| `bake_icons [dir] [quit]` | bake every item icon |
| `ui_dump_atlas [path]` | the SDF atlas as a PNG |
| `ui_nav <dir\|accept\|back\|tab_next\|tab_prev\|alt\|alt2\|menu\|view> [pad]` | synthetic keyboard / gamepad input (scripted tests) |
| `ui_mouse <x> <y> [down\|up\|click\|rclick\|dclick\|wheel n]` | synthetic mouse (scripted tests) |
| `ui_inv_drag`, `ui_inv_drag_att`, `ui_inv_drag_to_att`, `ui_inv_drag_out`, `ui_inv_drag_from_crate`, `ui_inv_rclick`, `ui_inv_shiftclick`, `ui_inv_select`, `ui_inv_dump` | scripted inventory gestures and a dump of the slots |

## Verification

**Unit tests:** `build/bin/test_ui` (402 checks).
* Layout:
  * basics (anchors, flex, aspect, stretch, gap, DPI);
  * the pause layout at 1080p, 21:9, 4:3, 720p, 4K and 3440×1440: stack bottom left in the safe
    area, logo bottom right, modal at most 60 % wide and never over the stack, dialog centred,
    4K exactly twice 1080p, hidden buttons take no space.
* Stack merge math, including conservation over a count sweep.
* Every intent.
* Attachment compatibility plus a mount / unmount round trip for every weapon × attachment.
* Swapping optics; extended-magazine removal returning rounds.
* The drag-drop state machine: click, threshold, move, split, mount, reject with reason, unmount,
  drop / confirm, cancel, context, double click, gamepad carry, crate.
* Prediction plus reconcile at every acknowledgement point.
* An icon for every item.

**Scripted runs:**
* `tools/tests/ui_pause.cfg`: pause / settings / inventory screenshots.
* `ui_resolutions.cfg`: HUD, settings and inventory at 1280×720, 1920×1080, 2560×1080,
  3840×2160 and 1024×768.
* `ui_nav.cfg`: pad and keyboard through pause → settings (tabs, rows) → back → inventory → back,
  multiplayer, the quit dialog cancelled.
* `ui_inventory.cfg`: every mouse gesture with an `inv:` dump after each.
* `ui_crate.cfg`: hold-open a crate, drag an item out, Tab closes.
* `inv_net_host.cfg` + `inv_net_client.cfg`: 2-client LAN with `net_fakelag 200`. Intents show at
  once (pending 1 and 3, including three back to back inside one round trip) and converge to the
  host's identical inventory (pending 0).
