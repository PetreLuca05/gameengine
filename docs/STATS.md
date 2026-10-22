# Player statistics and the Stats tab

Per-player statistics: lifetime totals per profile, per-weapon totals, the last 100 match records,
a per-day roll-up of the last year, and the match in progress. The host is authoritative for
combat numbers. The Stats tab shows them in the pause menu and the main menu.

| Piece | Code |
|---|---|
| Data model, events, derived values, history rings, host deltas, public stats (pure) | `src/gameplay/stats/stats.{h,c}` |
| Persistence: JSON from field tables, migration, CRC-32 (pure) | `src/gameplay/stats/stats_save.c` |
| `stats_fake` generator (pure) | `src/gameplay/stats/stats_fake.c` |
| Game glue: hooks, match lifecycle, net, autosave, Steam, commands | `src/game/game_stats.{h,c}` |
| Chart library | `src/ui/ui_chart.{h,c}`, pure math in `src/ui/ui_chart_math.c` |
| The tab | `src/ui/ui_stats_tab.{h,c}`, `assets/ui/screens/mainmenu_stats.json` |
| Tests | `tests/test_stats.c` (ctest `stats`), `tools/tests/stats_tab.cfg`, `tools/tests/stats_net.sh` |

## Data model

- **`PlayerStats`** holds the lifetime numbers:
  - combat: kills, deaths, headshots (headshot kills), damage dealt / taken, best killstreak, and the
    longest kill (distance, weapon, map, time);
  - team: revives given, times downed, time played;
  - movement: distance (grounded, horizontal), steps, jumps;
  - matches: played, won, highest wave;
  - `WeaponStats weapons[32]`: shots, hits, kills, headshots, damage, time equipped, longest kill,
    headshot hits and a kill-distance histogram.
- **Derived values** are functions, not stored: `stats_kd`, `stats_headshot_pct`,
  `stats_overall_accuracy`, `stats_favorite_weapon` (most kills), `stats_win_rate`,
  `stats_weapon_accuracy`.
- **Weapon registry.** Weapons are stored by the stats' own registry (`weapon_ids`). Slot *i* is
  the weapon named `weapon_ids[i]` in that file. A weapon removed from the game keeps its numbers.
  The glue maps item database weapon indices to these slots.
- **`MatchRecord`** is a ring buffer of the last 100 matches. Each record has:
  - time, map, result (win / loss / abandon), duration, waves reached;
  - kills, deaths, headshots, shots, hits, damage dealt / taken, revives, downs, distance, steps,
    jumps, best streak, longest kill;
  - per weapon: kills, shots and hits;
  - per wave: kills, downs and revives.
  The ring is the source of every per-match chart.
- **Timeline buckets.** No wave system reports waves today. Until one does
  (`game_stats_wave_complete`), the per-wave arrays are 2-minute segments (`segment_sec` = 120 in
  the record). The first reported wave switches the match to waves.
- **`DailyRecord`** is a ring buffer of 365 days on the local calendar: matches, kills, deaths,
  time, shots and hits.
- **`MatchStats`** is the match in progress. Lifetime totals update live as events arrive. At the
  match end the record goes into the ring and the day, and the match counters update.
- **Discarded matches.** A match shorter than 15 s with no kills, deaths, shots or downs is not
  recorded (map changes, the main menu's background world).

## Tracking

Each event costs a few adds and allocates nothing: `stats_on_shot_fired`, `stats_on_hit`,
`stats_on_kill`, `stats_on_death`, `stats_on_down`, `stats_on_revive_given`,
`stats_on_damage_taken`, `stats_on_jump`, `stats_on_step`, `stats_on_wave_complete`,
`stats_on_match_start` / `stats_on_match_end`, and `stats_tick` (time, equipped time, distance).

| Hook | Called from |
|---|---|
| shots (projectiles; a shell counts its pellets) | `src/gameplay/weapons.c` `fire()` (host) |
| melee swing (counts as a shot) | `weapons.c` `weapons_tick()`, where `ws->swing_t = 0` (host) |
| hits, kills (zombies and players), damage taken, deaths | `src/gameplay/health.c` `apply_damage_ex()` -> `stats_sv_damage` |
| downs | `src/gameplay/revive/revive.c` `revive_sv_down()` -> `stats_sv_down` |
| revives given | `revive.c` `revive_sv_revive()` -> `stats_sv_revive` |
| jumps | `src/player/movement.c` `player_move_tick()`, where `jumped = true` (local player, first simulation only) |
| steps | `src/game/game_audio.c` `footsteps()`: the local player's foot plants (`chars_take_steps`) |
| time, equipped time, distance, flushes, autosave | `src/game/game.c` `game_frame()` -> `game_stats_frame` |
| match start | `game.c` `game_host_ex()`, `game_cl_welcome()` -> `game_stats_level_loaded` |
| match end (abandon) | `game_host_ex()`, `game_cl_welcome()`, `game_disconnect_to_local()`, `mainmenu_enter()` -> `game_stats_level_leaving`; `game_shutdown()` |
| match end (win) | `src/game/game_story.c` `story_frame()`, at the chapter's end |
| wave survived | `game_stats_wave_complete(wave)` (no wave system yet: `stats_wave` command) |

- **Killstreak.** Kills since the last death or down. `best_killstreak` updates live.
- **Steps.** Footstep events when they arrive. A match without any uses distance / 0.75 m.
- **Authority.**
  - The host simulates every player. Its own player's combat events go straight into its stats.
  - A remote player's combat events go into a per-slot `StatsDelta`. The host sends it as
    `MSG_STATS STN_DELTA` every 30 s, when its 64-entry event log fills, on a wave, and before the
    world goes away.
  - The delta carries per-weapon counters (only weapons with activity) plus an ordered event log:
    kills (with distance), deaths, downs, revives, 4 bytes each. The client replays it in order, so
    the killstreak is right.
  - Weapons on the wire are item database weapon indices.
  - Movement and time are tracked by each machine for its own player.
- **Public stats.**
  - A client sends its own `StatsPublic` (totals, favorite weapon, about 100 bytes) to the host
    after the welcome and every 30 s.
  - The player selector asks for a slot with `STN_REQUEST`. The host answers with its cached copy,
    or `STN_NONE` when it has none.
  - `DG_PROTOCOL_VERSION` 37 added `MSG_STATS`.

## Persistence

- **File.** `<pref>/saves/stats_<profile>.json`, next to `profile.json`. The profile is the Steam
  id, or `local` without Steam. It uses the project's save format (JSON), written from field tables
  (`stats_save.c`): appending a field is one table row.
- **Compatibility.** Keys a file lacks read as zero. Unknown keys, such as a newer build's, are
  skipped. Arrays are written up to their last non-zero element.
- **Version.** `STATS_VERSION` is 1. Version 0 is the old `profile.json` stat stubs (kills,
  deaths, matches, play time). They are imported once, when no stats file exists yet.
  `stats_migrate` brings older files up to date.
- **Writes.**
  - Atomic: `.tmp`, then a rename.
  - Autosaved at every match end and every 60 s in a match. The stats are copied on the main
    thread and serialised and written on a job, so the frame never stalls.
  - The CRC-32 of the text skips unchanged writes.
  - The match in progress is saved too. After a crash it is recorded as abandoned on the next load.
  - The quit path saves synchronously.
- **`--test` runs** keep the stats in memory. They read and write nothing in the user's folders.
- **Export.** "Export JSON" (or `stats_export`) writes a readable copy to
  `saves/stats_export_<profile>_<date>.json` (`--test`: `exports/` in the build tree).
- **Steam.** Configure with `-DDG_STEAM_STATS=ON`, which defines `DEADGROUND_STEAM`, on a Steam
  build. It mirrors kills, deaths, headshots, matches_won and highest_wave as INT stats
  (`dgs_stat_set_int` / `dgs_stats_store`) on load, at match end and on reset. The stats must exist
  with these API names in the app's Steamworks settings.

## Charts (`ui_chart.h`)

Every chart builds its triangles (the atlas' white texel) and labels into a `ChartCache`, in the
chart's local space. A scrolled chart is not rebuilt. The cache rebuilds when the caller's data
revision or the rect's size changes. Each frame draws the cached buffers (`gd_tris`, `gd_text`).

- Hover is answered from the cached geometry: every chart returns the data index under the mouse,
  and `chart_tooltip` draws the caller's text on the top layer.
- Empty data draws a placeholder.
- Axes use "nice" ticks (1 / 2 / 5 x 10^n, `chart_nice_ticks`).
- Colours come from the UI theme (`chart_style_default`).

The charts:

- `chart_line`: N series, an optional right axis, a moving-average overlay, a filled area,
  win / loss point colours, and X labels from a callback.
- `chart_sparkline`: no axes; a dot on the last value.
- `chart_bar`: vertical or horizontal, grouped, per-bar colours, a reference line, value labels.
- `chart_stacked_area`
- `chart_donut`: labels, %, centre text.
- `chart_radar`: up to 8 axes, 1-2 polygons, values 0..1.
- `chart_histogram`: with a marker.
- `chart_scatter`: per-point colour and size.
- `chart_calendar_heatmap`: 53 x 7.
- `chart_gauge`: a 270 degree arc.
- `chart_timeline`: bars with down / revive markers.

## Stats tab

Pause menu > Stats (`SCR_STATS`, a centred modal) and the main menu's Stats tab (`MT_STATS`, key 6).
The sub-pages switch with LB / RB or Q / E, or a click; every control is focusable with a keyboard
or gamepad.

1. **Overview.** Cards for K/D, kills, deaths, accuracy, headshot %, revives, time played, highest
   wave, best killstreak, longest kill (weapon and map), favorite weapon, distance (with steps and
   jumps) and win rate. Each card shows the lifetime value, a sparkline of the last 10 matches, and
   the change vs the 10 before (arrow plus %). Accuracy, headshot % and win rate are gauges.
2. **Performance.**
   - Pick a metric (kills, deaths, K/D, accuracy, damage dealt / taken, revives, waves, distance),
     a range (last 10 / 25 / 50 / 100 matches, the last 30 days, all time) and a view (per match /
     per day).
   - The line chart has a moving average, an optional second metric on the right axis, and win /
     loss coloured points.
   - Below: a kills-per-match histogram (the last match marked) and an accuracy-vs-kills scatter.
   - Per-day values come from the day roll-up. Metrics it lacks come from the day's match records
     still in the history.
3. **Weapons.**
   - A sortable table: click a header to sort, click it again to reverse.
   - Charts: kills by weapon; accuracy by weapon with the overall accuracy as a reference line;
     donuts for the share of kills and of time equipped; kills per weapon over the last matches
     (stacked area).
   - Click a weapon for its detail:
     - every number it has;
     - a radar of accuracy, headshot %, kills per minute, damage per shot and longest kill, each
       axis normalised by your best weapon on it, with "Compare with..." for a second weapon;
     - its kills and accuracy per match;
     - the kill-distance histogram, with the longest kill marked.
4. **Movement.** Total distance, steps, jumps, the average per match, and the distance per match.
5. **History.** The last 100 matches. Click one for its kills timeline (per wave or per 2-minute
   segment, downs and revives as markers), a kills-per-weapon donut, and that match vs your lifetime
   averages. Esc / B goes back to the list.
6. **Activity.** A calendar heatmap of the last year, showing matches played or kills. Below it:
   active days, the year's total, the best day, and the current streak.

Multiplayer:

- The player selector lists every connected player. Another player shows their public summary.
- "Compare with you" shows side-by-side cards and an overlaid radar of kills, accuracy, headshot %,
  revives, distance and highest wave, normalised across the two players.

Toolbar:

- Units: metric or imperial (`ui_units`, also in Settings > HUD).
- "Export JSON".
- "Reset stats", which asks for confirmation (`CONFIRM_STATS_RESET`).

Numbers use separators: 1,234 / 12.3 % / 1.2 km / 1h 23m. With no matches yet, the tab says so.
The charts show the stats revision at most once a second, so a running match does not rebuild them
every shot.

## Commands

- `stats_fake [N] [keep] [seed]`: N plausible matches over the last year (default 100), replacing
  the stats unless `keep`. They are random but correlated: a skill that rises over the year drives
  accuracy, kills, waves and wins, and fewer downs.
- `stats_dump`: lifetime numbers, per weapon, and the match in progress.
- `stats_reset`, `stats_export`, `stats_save`.
- `stats_match_end [win|loss|abandon]`: record the match in progress with a result; a new match
  starts on the same world.
- `stats_wave [n]`: report a wave survived.
- `stats_flush`: the host sends the deltas now.
- `stats_public [slot]`: the public stats this machine has for a player; a client asks the host.
- `stats_tab <page> [arg]`: switch the tab's page for scripted screenshots.
  `menu stats` / `ui_screen stats` open the screen.

## Tests

- `test_stats` covers:
  - derived values (accuracy, K/D, headshot %, favorite weapon, win rate, accuracy clamped);
  - the killstreak reset on death and down;
  - grounded distance, teleports ignored, steps, waves vs segments;
  - the match and day rings;
  - the calendar;
  - the delta wire format and its in-order replay;
  - public stats;
  - a save / load round trip, byte-identical on a second write;
  - migration from `profile.json` and from a newer file;
  - stats_fake's correlation;
  - nice ticks, the moving average, hover lookups, calendar cells and number formatting.
- `tools/tests/stats_tab.cfg` screenshots every page after `stats_fake`.
- `tools/tests/stats_net.sh` runs a host and a client over LAN (`stats_net_host.cfg`,
  `stats_net_client.cfg`). The client fires; the host downs, revives and kills it, then flushes.
  It checks that the shots, the down and the death reach the client, and that public stats arrive
  both ways.

## Not wired yet

- **Waves.** No wave mode exists yet. A wave system calls `game_stats_wave_complete(wave)` on the
  host for each wave survived, and `game_stats_match_end(MATCH_WIN / MATCH_LOSS)` when the match is
  decided. Until then, matches end as abandoned, or as won at a story chapter's end.
- **Losses.** Nothing ends a match as lost today (for example, the whole team dead). The wave mode
  decides that.
- **Steam Stats.** Create the five INT stats in Steamworks, then build with `-DDG_STEAM_STATS=ON`.
- **A client leaving.** A client that leaves loses the combat numbers of its last flush window
  (up to 30 s): the host has nobody to send them to.
