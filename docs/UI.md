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
                                 ├─ Inventory ─────▶ INVENTORY (full screen, docs/INVENTORY.md)
                                 ├─ Settings ──────▶ SETTINGS  (centred modal, tabs, scroll)
                                 │                     └─ display change ─▶ CONFIRM (keep? 15 s, reverts)
                                 ├─ Multiplayer ───▶ MULTIPLAYER (host LAN / Steam, connect, LAN list)
                                 ├─ Invite friends   (Steam builds only)
                                 ├─ Leave game / Stop hosting ─▶ CONFIRM
                                 ├─ Quit to desktop ───────────▶ CONFIRM
                                 └─ Phone card (top right) ─▶ PHONE (the Connect portal modal, docs/PHONE.md)
in game ── Tab / I / pad Back ──▶ INVENTORY (grids left, the player centre, hotbar right)
                                     └─ discard a weapon / container / wearable ─▶ CONFIRM
world: crate (hold E), body, trunk ──▶ INVENTORY with the loot grid (right column)
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
| Settings → Graphics | A **Default** button on top (asks first; `settings_graphics_defaults`, console `r_defaults`): every archived `r_*` / `post_*` / `vfx_*` cvar plus `cl_fov`, `cl_ads_world_zoom`, `cam_weapon_fov`, `gore_level`, `sight_brightness` back to its registered default, then the Engine Fidelity level's table defaults with the custom tweaks cleared; the display (resolution, window mode, vsync) and the benchmark results stay. Two sub-tabs, **Normal** and **Debug** (see "Settings → Graphics: Normal and Debug" below). Bounded rows (`row_fid_*`): options above the level greyed with "requires &lt;level&gt;", features the level lacks locked with "Requires Engine Fidelity: &lt;level&gt;". |
| `src/ui/inventory/` | The inventory screen (docs/INVENTORY.md): layout + drag and drop (pure, tested), grids, inspect, cell images, editor. |
| `src/gameplay/inventory/` | The grid inventory model: tree, rules, intents, wire. Pure (tested). |
| `src/ui/item_view.c`, `src/ui/icons.c` | Item models for the UI, the icon atlas and bakes. |
| `src/render/render_ui.c` | Backdrop blur, offscreen item views, PNG readbacks. |
| `src/ui/hud.c` | The HUD. |

The UI frame starts in `menus_tick` (`gui_frame_begin`: input edges, DPI scale, draw list reset),
screens and the HUD draw during `game_render`, and `ui_render` flushes the old HUD batch
(crosshair, dev overlays), then the game UI (`gui_frame_end`: navigation, tooltip, flush), then
Nuklear.

### Settings → Graphics (since the settings audit)

docs/SETTINGS_AUDIT.md section 6 is current: Graphics = "Why can't I change X?" + Default, the
Engine Fidelity card, **Display** and **Comfort**; the crosshair moved into the HUD tab; Debug exists
only in dev builds (developer mode, no release locks) and holds tier features only. The text below
describes the layout before the audit.

### Settings → Graphics: Normal and Debug

The Graphics tab has two sub-tabs (`gw_tabs` under the main tab bar, `k_gfx_pages` in
`scr_settings.c`). **Debug is shown only in developer mode** (the `developer` cvar, the same switch
as the dev HUD and the pause Commands tab); with `developer 0` there is no sub-tab bar and the
page is Normal. `developer` defaults to 0 for players (saved; `developer 1` in the console turns it on)
and `--test` runs force it on. The rule: options a PC player expects in a commercial game are Normal; engine
internals, diagnostics and per-feature tuning are Debug.

**Normal** (a player's page, one control per concept):
* Default button (as above).
* Display: resolution, window mode, VSync (Apply / Revert, 15 s confirm), frame rate limit,
  stretched resolution.
* View: field of view (or the server's), hands FOV follows camera, hands / weapon FOV, red dot
  brightness.
* Quality card: **Overall graphics quality** = the Engine Fidelity level used as a preset (Off,
  Low, Medium, High, Modern, plus "Custom", greyed, shown when any feature differs from the level's
  defaults: `fid_custom_count`). Picking a level clears the tweaks and applies the level's table;
  Run benchmark, Apply recommended. Below it: render scale, upscaling (Off / TAA-U / FSR 1 and its
  resolution), anti-aliasing, sharpening, then the simple qualities **Post-processing, Shadows,
  Textures, Effects, Foliage, View distance** and motion blur.
* Picture: brightness, gamma, HDR10 + paper white / peak when
  the display supports it.
* Accessibility & content: colour-blind filter, screen effects, gore.

The simple qualities are **fidelity groups** (`fid_group_*`, `src/render/fidelity`): each a set of
features; tier t = every feature at level t's own table default for this era (Lowest / Low /
Medium / High / Ultra = the Off .. Modern columns). Applied as user changes through the bound hook,
so the level bounds hold and the overall level reads Custom afterwards. A tier the level doesn't
allow is greyed "requires &lt;level&gt;"; a row reads Custom when the cvars match no tier.

| quality | features |
|---|---|
| Post-processing | ao, contact_shadows, ssr, ssgi, puddle_reflections, volumetrics, vol_local_lights, dof, dof_quality, ads_dof, bloom, bloom_quality |
| Shadows | shadows, shadow_resolution, shadow_cascades, shadow_distance, soft_shadow_quality, shadow_min_radius, shadow_caster_limit, light_shadows, light_shadow_quality, cloud_shadows, moon_shadows |
| Textures | texture_quality, aniso, terrain_quality |
| Effects | vfx_quality, vfx_lighting, decal_max, precip_quality, water, lens_drops |
| Foliage | foliage_density, foliage_wind |
| View distance | lod_bias, impostor_px, vgeo_shadow_error_px, vgeo_lod_px |

Foliage density and the discrete LOD thresholds are era-owned: the era preset (`r_era`, `vk_vgeo.c
era_detect`) also accepts a lower level's own value of the era, so these tiers keep the era.

**Debug** (developer mode): Diagnostics (performance overlay), the full
upscaler list (FSR 2 / DLSS slots), Post-processing & Effects (the legacy post preset, every
screen-space / fog / DoF / bloom / lens / grain / exposure control, motion blur strength),
Shadows & lighting (cascades, splits and the split bar, near plane, shadowmask, min caster, lamp /
cloud / moon shadows), Geometry / textures / foliage (no Engine Fidelity card or era look any more: the Normal tab's Overall graphics quality picks the level; textures,
aniso, density, impostors, terrain, character LOD / quality), Effects (particles, lit particles,
decals), Sights (scope picture resolution, aiming DoF), Weather (rain / snow, puddles, water).

**Other tabs, developer mode only:** Character → Developer: third person view, spectator delay
readout. Phone → Developer: HTTPS, pairing code lifetime, stream width / height.

`ui_screen settings:normal` / `settings:debug` open the sub-tabs (scripts);
`tools/tests/ui_settings_graphics.cfg` screenshots both, developer 0 / 1, at 1920x1080, 1280x720,
2560x1080 and 1024x768, plus Overall Medium and a one-option Custom.

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

The grid inventory (Tarkov-style: items take rectangles of cells, rotate, live in containers that
are items, equipment slots, world containers / corpses / trunks with the same screen) has its own
document: **docs/INVENTORY.md** (data model, rules, intents, networking, screen, commands, tests).

## HUD

All HUD elements sit in the safe area:
* **Vitals** (bottom left): small-caps label, a slim health bar (pulses red under 30 %), the
  value, and an armour bar. The lowest bar sits on the safe area's bottom edge, the same edge as the
  hotbar slots and the ammo panel's magazine ticks.
* **Ammo** (bottom right): weapon name, magazine (large; amber below 25 %, red when empty), the
  reserve, magazine ticks, and the fire mode when the weapon has several.
* **Hotbar** (bottom centre): six slots (keys 1-6) bound to item instances (docs/INVENTORY.md), the
  slot widget with the cell image, the selected slot lifted; the item name fades in on switch.
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
| `ui_screen <name>` | open a screen: `pause`, `settings[:tab]` (`settings:normal` / `settings:debug`: the Graphics sub-tabs), `multiplayer`, `inventory`, `bench`, `confirm_quit`, `confirm_leave`, `close`. No args: list plus the current stack |
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
| `ui_inv_drag`, `ui_inv_click`, `inv_dump`, `inv_intent`, `inv_give`, `inv_fill`, `inv_open`, `inv_editor`, `bake_cells` | the inventory's scripted gestures, dumps, intents and tools (docs/INVENTORY.md "Commands") |

## Verification

**Unit tests:** `build/bin/test_ui` (229 checks).
* Layout:
  * basics (anchors, flex, aspect, stretch, gap, DPI);
  * the pause layout at 1080p, 21:9, 4:3, 720p, 4K and 3440×1440: stack bottom left in the safe
    area, logo bottom right, modal at most 60 % wide and never over the stack, dialog centred,
    4K exactly twice 1080p, hidden buttons take no space.
* The inventory has its own tests (`test_inventory`, `test_inventory_ui`, docs/INVENTORY.md "Tests").
* An icon and a cell image for every item.

**Scripted runs:**
* `tools/tests/ui_pause.cfg`: pause / settings / inventory screenshots.
* `ui_resolutions.cfg`: HUD, settings and inventory at 1280×720, 1920×1080, 2560×1080,
  3840×2160 and 1024×768.
* `ui_nav.cfg`: pad and keyboard through pause → settings (tabs, rows) → back → inventory → back,
  multiplayer, the quit dialog cancelled.
* `ui_inventory.cfg`, `ui_crate.cfg`, `inv_net.sh`: the inventory screen, crates / the gear
  locker, two clients looting one crate (docs/INVENTORY.md "Tests").
