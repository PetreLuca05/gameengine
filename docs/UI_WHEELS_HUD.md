# Radial wheels, timed ring, kill counter, compass, pings, Commands tab

Everything here uses the game's design system: theme colours and `layout` tokens from
`assets/ui/theme.json`, `gw_*` widgets and `gd_*` drawing (docs/UI.md). The HUD pieces live in
`src/ui/hud/`, the shared widgets in `src/ui/widgets/`, the game glue in `src/game/game_wheels.c`,
`game_pings.c` and `game_peek.c`.

## Radial wheel

`src/ui/widgets/radial_wheel.{c,h}` (widget), `radial_math.{c,h}` (pure selection math),
`wheel_def.{c,h}` (pure JSON parsing).

- **Hold to open, release to select.** Each trigger key opens its wheel once held for
  `wheel_hold_time` (0.22 s). A shorter press is the key's tap action.
- **Selection.**
  - Mouse deltas and the right stick feed a virtual cursor, clamped to `wheel_cursor`.
  - Inside the `wheel_dead_zone` the selection stays as it is.
  - Leaving a slot needs the cursor past its edge by `wheel_hysteresis_deg`. That margin is capped
    at 45 % of half a slot, so 12-slot wheels still switch.
  - Number keys 1–9 pick a slot directly; the scroll wheel steps (and wraps).
  - Right click cancels.
- **Look.** While a wheel is open the mouse doesn't turn the view, and attack / reload don't reach
  the simulation.
- **Slots.**
  - 4–12 slots per wheel (2 minimum for static wheels).
  - The selected slot is white (theme `select`), with a white arc on the rim.
  - Empty slots are dimmed outlines. Disabled slots are greyed and struck through; their reason
    ("Full health", "Locked", "Door is open") shows in the centre.
- **Centre.** The selected slot's name, count and description, or the timed ring while an action
  runs.
- **Animation.** Opens and closes in 120 ms (scale + fade).
- **Slow motion.** `wheel_timescale` (0.35) slows the simulation while a wheel is open. It applies
  only in single player, or when hosting with nobody connected.
- **Sub-wheels.** A slot with `"sub": "<wheel id>"` opens that wheel instead of acting; the release
  then selects there.

### Wheel JSON (`assets/ui/wheels/*.json`)

```json
{ "id": "door", "title": "Door", "source": "static", "default": "open",
  "slots": [ { "id": "open", "label": "Open", "icon": "door_open", "desc": "...", "color": 0, "sub": "" } ] }
```

- **`source`**
  - `static`: the listed slots.
  - `inventory`: one slot per carried item type matching `filter`. `filter.kind` is
    `consumable` or `throwable`; `filter.tag` `heal` means heal / armor items that aren't vehicle
    items. Here `slots` is just the display order, as bare item ids.
- **`min_slots` / `max_slots`**: empty slots fill the wheel up to `min_slots`.
- **`hold_time`**: overrides the cvar.
- **`icon`**: names a vector icon from `src/ui/hud/hud_icons.c`. Without one, the item's baked icon
  is used, then its initials.
- **`color`**: an index into the theme's `ping_types` colours.

| Wheel | Key (tap / hold) | File |
|---|---|---|
| Healing | C, pad D-pad right: quick heal / wheel | `healing.json` |
| Throwables | G: drop the held item / wheel | `throwables.json` |
| Ping | middle mouse, Z: contextual ping / wheel | `ping.json` |
| Door | E on a door: use / wheel (Open, Slam, Peek) | `door.json` |

Healing items are taken into the hands and used at once (`wheel_heal_auto_use`). The wheels take
an item out of the grid into the hands (`II_HOLD`, docs/INVENTORY.md "Selection"). When it is used
up, the last weapon comes back; fire waits for a release. The HUD weapon list's selected row is
white too.

The G tap drops the held item (the drawn weapon or the item in the hands, never the built-in phone): `RPC_INV_DROP`,
applied by the host as a discard, so it lands in front of the player as a pickup (a weapon with its
attachments). `tools/tests/inv_drop.cfg` checks it.

## Timed ring

`src/ui/widgets/timed_ring.{c,h}`. `ui_timed_ring(duration, remaining, label)` draws a white ring
(theme `ring`, thickness `ring_thickness_wheel`, 1.6 × `bar_thickness`) with a countdown in the
middle.

- **Countdown text:** one decimal under 10 s, rounded up to the tenth so it never reads 0.0 before
  the end; whole seconds above that.
- **Modes:** drain (remaining / duration) and charge.
- **Fades:** a cancelled action fades out over 0.25 s and shows "Cancelled"; a finished one fades
  over 0.4 s.
- **Where it shows** (`hud_timed_ring`): consumable use, grenade cooking (drain over the fuse),
  throw charge, and hold interactions. It sits in the wheel's centre while a wheel is open,
  otherwise under the crosshair (`ring_hud_offset`).

## Door wheel and peek

- **E tap / hold.** A tap on a door uses it as before; the press waits for the release. Holding
  opens the door wheel.
- **Wheel actions** (`RPC_DOOR_ACTION`, host-validated):
  - **Open / Close**: the normal use.
  - **Slam Shut / Kick Open**: 3.5× the speed and a loud `door_slam`. `horde_noise` makes zombies
    within 40 m chase.
  - **Peek**: needs the door closed.
- **Peek** (`game_peek.c`, client only). Every door now has a door viewer (visor) at eye height on
  both faces (`Door.visor_local`, drawn in `game_render.c`).
  - The camera sits in the visor on the far face, looking away from you.
  - Lens: `peek_fov` 100°, `peek_distortion` barrel distortion, vignette and chromatic aberration
    through the post stack.
  - A round black mask with a soft edge and a brass rim.
  - Look is limited to `peek_yaw_limit` / `peek_pitch_limit` around the visor axis.
  - It ends on use (E), Esc, moving, jumping, attacking, taking damage, the door moving or opening,
    walking away (1.9 m) or dying.
  - `peek` toggles it on the focused door.

## Throwables

`src/gameplay/throwables.{c,h}`. Item kind `throwable` with a `throw` block in
`data/items.json`: `kind`, `fuse`, `radius`, `damage`, `speed_min` / `speed_max`,
`charge_time`, `bounce`, `light_time`, `light_color`.

- **Throwing.** Hold attack to pull the pin and charge; release to throw. Throw strength comes
  from the hold time.
- **Frag grenade.** Its fuse runs while you cook it; cooked too long, it goes off in your hand.
  It bounces, then explodes with radius damage: falloff, line of sight, kill-feed credit to the
  grenade. Zombies hear it (`horde_noise`).
- **Road flare.** Burns red for `light_time` (a point light).
- **Molotov.** Bursts on impact, then a 6 s fire that hurts what stands in it.
- **Presentation.** `EV_EXPLOSION` carries the effects (debris, flash) and the sound.
- **Healing interruption.** Consumables with `"interrupt_on_damage": true` (medkit, bandage,
  painkillers, armor) are cancelled by damage, on the host.

## Kill count and kill feed

`src/ui/hud/hud_kills.c`, model in `src/gameplay/kill_stats.h` (pure).

- **Source.** `EV_KILL` now carries flags (headshot, victim player / zombie, killer player, self)
  and `KillEventData` (killer name, victim name, weapon item). Every machine builds its own feed
  and counts its own kills; nothing extra is replicated. A late joiner starts at 0.
- **Count.** Not on the HUD: the inventory's header shows it at its right end (the total, the
  zombies / players split).
- **Feed (top right).** Five entries, 6 s each, fading over the last second, sliding in.
  - Each row: killer, weapon icon, collateral icon (the bullet had already killed someone: `KILLF_COLLATERAL`,
    counted per bullet on the host, `BalLive.kills`), headshot icon, victim. The icons draw white (the weapon icon is the silhouette of its baked item icon, `gd_image_mask`).
  - Your kills have a white edge; your deaths a red one.

## Compass

`src/ui/hud/hud_compass.c`, math in `compass_math.h` (pure), data in
`assets/ui/hud_compass.json`.

- **Strip (top centre).** Ticks every 5°, labels every 15°, cardinals bold, N in the accent
  colour. It fades at both ends (`edge_fade`), with the heading in degrees under the centre mark.
- **Directions.** North is -Z and east +X; the heading is -yaw.
- **Markers.**
  - Pings: type colour, icon and distance.
  - Teammates: the initial of their name.
  - A marker outside the strip sits at its end with a chevron.
- **Coordinates.** `X / Z / H` under the strip, refreshed at `coords_hz` (10 Hz).
  - Pause > Copy position, or `copypos`, copies them as a `setpos` line.
- **Day / night clock.** It now sits under the compass, leaving the top right to the counter.

## Pings

Core in `src/gameplay/ping.{c,h}` (pure); glue in `src/game/game_pings.c`; markers in
`src/ui/hud/hud_pings.c`.

- **Types.** Generic, Enemy, Loot, Danger, Go here, Need help, Vehicle, Defend. Each has a label,
  icon, colour, lifetime, chat text and sound, from `wheels/ping.json`.
- **Contextual tap.** What the crosshair is on decides the type:
  - Zombies and dummies → Enemy (placed over the head).
  - Pickups and containers → Loot.
  - Vehicles → Vehicle.
  - Anything else → Generic.
- **Replication.** The client sends `RPC_PING` (type plus the position in cm). The host validates
  it (alive, range ≤ 1000 m, rate limit: 0.25 s apart and at most 4 per 3 s). It then broadcasts
  `EV_PING` with a uid and the lifetime.
- **Shared list.** Every machine applies the event to its own `PingList`: 3 per player (the oldest
  goes), auto-removed at the end of the lifetime.
- **Removing.** Pinging within 2.5 m of one of your own pings removes it.
- **On every machine:**
  - A world marker (edge-clamped with an arrow off screen, a white ring on your own).
  - A compass marker.
  - A chat line ("Name spotted an enemy").
  - The sound (`ping` / `ping_alert`, the audio hook).
- **Cvars:** `ping_enable`, `ping_chat`, `ping_sound`, `hud_ping_markers`, `hud_ping_compass`.
- **Commands:** `ping [type]`, `ping_list`.

## HUD settings

Settings > HUD (all archived cvars):

- **Layout:** `hud_opacity`, `hud_scale`.
- **Compass:** `hud_compass`, `hud_coords`, `hud_ping_compass`.
- **Kills:** `hud_killfeed`.
- **Pings:** `hud_ping_markers`, `ping_enable`, `ping_chat`, `ping_sound`.
- **Wheels:** `wheel_enable`, `wheel_hold_time`, `wheel_timescale`, `wheel_heal_auto_use`,
  `hud_timed_ring`.

## Commands tab

Pause > Commands (`src/ui/pause/commands_tab.c`, `SCR_COMMANDS`). Shown with `developer 1` or
`con_enable 1`.

- **List.** Every command and cvar (the `+action` pairs stay on the bindings screen).
  - Search: every word must match the name or the description.
  - Categories: Favorites, Recent, All, then the categories.
  - A star to favorite (`con_favorites`; `*` means the defaults from the categories file).
- **Rows.**
  - Cvars: an inline editor (toggle, slider for ranged numbers, text field for the rest), "modified"
    when off their default, Reset.
  - Commands: Run, and Edit (puts the name in the console line for arguments).
  - Both: Copy. The description shows as a tooltip.
- **Recent.** Commands run from the tab are recorded in `con_recent`.
- **Embedded console.** The same history, completion and hints as `~` (`console.h`): coloured log,
  Tab completes, Up / Down walk the history, and a hint line shows the command's description or the
  cvar's value / default / range. **Pop out** opens the `~` console.
- **Destructive commands.** The `destructive` list (quit, map, kill, disconnect, zombie_clear …)
  asks first (`CONFIRM_COMMAND`), from the list or the console line.

### Categories and the lint

- **The file.** `assets/ui/command_categories.json` gives every registration a category: by exact
  name (`names`), else by the longest name prefix (`prefixes`). Pure lookup in
  `src/core/cmd_registry.c`.
- **The lint.** `tools/tests/lint_commands.py` (CTest `lint_commands`) scans every
  `cmd_register` / `cvar_reg*` call in `src/`. It fails on:
  - a non-literal name,
  - a missing or too-short description (< 8 characters),
  - a name without a category.
- **When adding a command or cvar:** give it a real description, and a prefix or name entry if
  nothing matches.

## Protocol

`DG_PROTOCOL_VERSION` 17:

- `EV_KILL` id flags + `KillEventData`.
- `EV_PING`, `RPC_PING`.
- `RPC_DOOR_ACTION`.
- `EV_EXPLOSION`.
- Throwable projectiles (`Projectile.item / fuse / burn`).

## Tests

- **`tests/test_hud.c`** (CTest `hud`, 510 checks). Covers:
  - wheel selection math at every slot count 2–12, dead zone and hysteresis, keys and scroll;
  - the wheel files and bad-file rejection;
  - the timed ring's fill and text;
  - pings: cap, eviction, expiry, the rate limit, wire packing, the types file;
  - kill stats and streaks, the feed;
  - compass math and the JSON;
  - registry categories, search and lists;
  - every `gui_tok` the HUD code uses existing in `theme.json`.
- **`tools/tests/hud_wheels.cfg`**: a scripted run with screenshots of every wheel, the healing ring,
  pings, kills, a grenade and a flare, the door wheel, peek, slam, and 21:9 / 4:3. It ends with
  `kill_stats` in the log.
- **`tools/tests/hud_commands.cfg`**: the Commands tab (all, search, favorites), Settings > HUD, and
  copy position.
- **Scripting commands:** `wheel_open <heal|throw|ping|door> [slot]`, `wheel_select`,
  `wheel_release`, `wheel_cancel`, `peek`, `zombie_kill_nearest [n] [head]`,
  `commands_filter <category> [search]`, `ui_screen commands|copypos`.
