# Main menu and lobby

The game starts in the **main menu** and every multiplayer game goes through a **lobby**. Both use
the game's own UI (docs/UI.md): the theme, the widgets, the layout engine, the screen stack and the
pause menu's look (thin lines, semi-transparent blurred panels, small-caps labels, one accent colour).

| Part | Code | Data |
|---|---|---|
| Main menu shell, tabs, lobby screen, browser, loading screen, chat | `src/ui/mainmenu/` | `assets/ui/screens/mainmenu*.json`, `assets/ui/theme.json` (`layout` tokens, rarity / chat colours) |
| Shared widgets added for it | `src/ui/gui/gui_widgets.c`: `gw_tabbar`, `gw_power`, `gw_avatar`, `gw_textfield_activate`; `gui_key_pressed` | |
| Lobby model + state machine (pure) | `src/net/lobby.c` | |
| Lobby glue: session, replication, Steam lobby / LAN discovery, migration, loading | `src/net/lobby_net.c` | |
| Maps | `src/world/maplist.c` | `assets/maps/*.json`, `assets/maps/thumbs/` (`tools/gen_map_thumbs.py`) |
| Seasons | `src/world/season.c`, `weather_apply_season` (`src/game/game_weather.c`), `daynight_set_calendar` | `assets/world/seasons.json` |
| Cosmetics | `src/gameplay/cosmetics.c`; `cl_parts` in `src/gameplay/characters.c` | `assets/cosmetics/*.json`, `assets/characters/player.json` `parts` |
| Profile | `src/game/profile.c` | `<pref dir>/saves/profile.json` |
| Steam | `third_party/steam_shim/` (lobby type / data / owner, lobby list, friends, invites, avatars, relay ping), `src/net/transport_steam.c` | |
| Credits | `tools/gen_credits.py` | `assets/ui/credits.txt`, `tools/credits_team.txt` |
| Tests | `tests/test_lobby.c` (ctest `lobby`), `tools/tests/mm_*.cfg`, `tools/tests/lobby_lan.sh` | |

## Screen map

```
launch ── splash (1.6 s, any key / click skips) ──▶ MAIN MENU
  background: the dev room, your character (3rd person, idle, your cosmetics, the held item),
              slow camera drift, lit by the selected map's season / time / weather (frozen)
  top bar:  Play · Player · Cosmetics · Maps · Settings · Credits                  [⏻]
            Q / E · LB / RB · 1-6 · mouse                         ⏻ ─▶ CONFIRM "Quit to desktop?"
  ├─ Play ──┬─ Host game ─────────────────────────────────────▶ LOBBY (host)
  │         ├─ Join ─▶ SERVER BROWSER [Steam lobbies | Friends | LAN | Direct IP] ─▶ LOBBY
  │         ├─ Continue  (stub: greyed until a saved session exists)
  │         ├─ Quick play (first public lobby with space and the same version, else hosts one)
  │         └─ Host on: Steam / LAN
  ├─ Player     3D preview (the scene camera frames the real character; drag / wheel / stick),
  │             name (LAN name field offline), stats (stubs), loadout, "Preview in map lighting"
  ├─ Cosmetics  categories ▸ item grid (inventory slot widget, rarity strip, LOCKED) ▸ equip
  ├─ Maps       list (thumbnail, biome, size, players; test maps toggle) ▸ details ▸ Session:
  │             Season · Start time · Start weather · Dynamic weather ▸ Host on this map
  ├─ Settings   the game's settings screen (Engine Fidelity, Graphics, Controls, Crosshair,
  │             Audio, Character, Phone), applying live
  └─ Credits    rolling credits + third-party licences
  footer: avatar · name · Online / Offline (LAN only) / LAN build · relay ping · version   patch notes

LOBBY (the Play tab while in a session; Maps and Credits are locked, the tab reads "Lobby")
  players (avatar, name colour, HOST, ready, mic stub, ping; host: Kick, Make host)
  | map (host: selector, the map's season / time / weather come with it; others: Vote stub)
  | game settings (difficulty, max players, privacy, join a running game, PvP stub,
  |                season, start time, start weather, dynamic weather)   host edits, others read
  | chat (timestamps, system lines, name colours, /ready /unready /map /kick /promote /start /cancel)
  [Ready] [Start | Start in 10 s | Cancel start] [Invite friends] status [Leave]
       ── start ──▶ LOADING (thumbnail, season / time / weather, progress, tip) ──▶ IN GAME
  closed (kicked, rejected, host left): "Session ended" + reason + OK

IN GAME
  Esc ▶ PAUSE: Resume · Inventory · Settings · Lobby ▸ (players, chat, invite, kick)
               · Invite friends · Leave to main menu ▸ CONFIRM · Quit to desktop ▸ CONFIRM
  Enter ▶ chat line (bottom left); recent lines fade out above the vitals
```

Keyboard, mouse and gamepad all work everywhere: the spatial focus navigation of docs/UI.md, A /
Enter accept, B / Esc back (browser → Play, a tab → Play, Play → the quit prompt), LB / RB and Q / E
cycle the tabs (inside Settings they cycle the settings' own tabs), 1-6 jump to a tab. Key hints
show next to the tab names while the keyboard / pad drives.

`--test` runs boot into the dev room as before (scripts, screenshots); `--menu` forces the menu. `--nomenu` skips the splash and the menu in a normal run (your config loads as usual): straight into `+map <id>` / `+scene <id>`, or the boot map.

## Lobby state machine

Every machine runs the same machine for its own session (`lobby_fsm_next`, a table in
`src/net/lobby.c`; `tests/test_lobby.c` checks every cell against an independent copy).

```
IDLE ──host──▶ CREATING ──transport up──▶ LOBBY ◀──────────────────────────────┐
IDLE ──join──▶ JOINING ──first snapshot──▶ LOBBY                                │
JOINING ──rejected / failed / kicked / host lost──▶ CLOSED                      │
JOINING ──start (late join)──▶ LOADING                                          │
LOBBY ──start (everyone ready)──▶ LOADING ──map loaded + streamed──▶ IN_GAME    │
LOBBY ──force start──▶ COUNTDOWN (10 s) ──done / start──▶ LOADING               │
COUNTDOWN ──cancel──▶ LOBBY                                                     │
LOBBY / COUNTDOWN ──host lost──▶ MIGRATING ──new host up / snapshot──▶ ─────────┘
MIGRATING ──fail / rejected / kicked / host lost──▶ CLOSED
LOADING / IN_GAME ──host lost / kicked / failed──▶ CLOSED (back to the menu)
any ──leave──▶ IDLE            CLOSED ──OK──▶ IDLE
```

**Host-authoritative.** The host owns the model (`Lobby`: members, ready flags, settings, chat,
bans). Clients send intents (`LobbyIntent`: ready, chat, map, settings, kick, promote, start,
cancel); `lobby_host_intent` validates them (host-only actions, map in the catalogue, settings in
range, start weather in the season, chat rate limit) and the host broadcasts a full snapshot (a few
hundred bytes) after every change plus each chat line. The host's own actions go through the same
function.

**Wire** (`MSG_LOBBY` on the reliable channel, protocol 16): `LOP_SNAPSHOT`, `LOP_CHAT`,
`LOP_INTENT`, `LOP_START`, `LOP_KICKED`, `LOP_MIGRATE`, `LOP_ERROR`. `HELLO` carries the build hash,
version string, Steam id and LAN listen port.

**Join.** HELLO → protocol / animation manifest checks (as before) → `lobby_on_hello`: the build hash
(`lobby_version_hash(DG_VERSION, DG_PROTOCOL_VERSION, maps ^ cosmetics)`: "Version mismatch: host
0.1.0, you 0.1.1" or "Game data mismatch"), the ban list, the size, late join. In the lobby the
member waits (no WELCOME); late joiners get the snapshot, `LOP_START`, then WELCOME at once.

**Start.** `LOP_START` to everyone; every screen shows the loading screen; the host loads the map two
frames later (`game_host_ex(level, GH_KEEP_NET)`), applies the season / time / weather
(`lobby_apply_environment`) and WELCOMEs every member; each client loads on WELCOME. A machine is
IN_GAME when its local player exists and the chunks around it are streamed.

**Kick.** Host only: the member is removed, its identity (`steam:<id>`, or `lan:<ip>/<name>`) is
banned for the session, `LOP_KICKED` goes out, the peer is dropped 0.3 s later. A banned identity is
refused at HELLO ("You were kicked from this lobby").

**Host migration (lobby only).** When the host leaves the lobby it sends `LOP_MIGRATE` (successor +
its address; Steam: also `SetLobbyOwner`). When the host vanishes, every member picks the same
successor itself (`lobby_successor`: the earliest-joined remaining member). The successor hosts
(same settings, bans and chat) after 0.2 s, the others reconnect after 2 s (LAN: the successor's IP
as the host saw it + its listen port; Steam: its SteamID). "Make host" in the player list is the
same, voluntary. In a running game the host leaving ends it ("The host left the game", back to the
menu); in-game migration is out of scope.

**Chat.** At most 256 characters (longer input is cut; a longer wire message is refused), printable
ASCII, a token bucket per member (4 back to back, then one per second), the profanity filter stub
(whole words, `k_profanity` in lobby.c), system lines for join / leave / kick / ready / map / start,
slash commands parsed on the sender (`lobby_parse_chat`) and validated on the host.

**Steam mirror.** The host's lobby state is written into the Steam lobby's metadata once a second
when it changed (`game`, `name`, `map`, `players`, `max`, `version`, `mode`, `state`, `privacy`,
`difficulty`, `late_join`, `season`), the type follows the privacy (public / friends only / private)
and the lobby stops being joinable when full or started without late join. Rich presence `status`:
"In lobby - 3/4 - Forest", "In game - Dev Room", "In the main menu"; `connect` = `+connect_lobby <id>`.

## Steam setup

* Build with `STEAM_SDK_DIR` (the SDK's `sdk` folder); the build writes `steam_appid.txt` (480,
  Spacewar) next to the exe. Without it the game is a LAN build ("LAN build (no Steam)").
* With Steam not running the menu is in offline / LAN mode: Steam lobbies, Friends, Steam hosting and
  invites are greyed with the reason; LAN discovery and direct IP work.
* **Join through Steam:** the friends list "Join game", an accepted invite (overlay) and the
  `+connect_lobby <id>` launch parameter all run `connect_lobby` → `lobby_join(TRANSPORT_STEAM,
  "lobby:<id>")`: the game enters the Steam lobby, connects P2P to its owner and shows the lobby.
* **Invite:** "Invite friends" opens the in-game friends list (online first, Invite buttons:
  `InviteUserToLobby`); Shift+click opens the Steam overlay dialog instead.
* Avatars: `GetSmallFriendAvatar` (retried while Steam downloads it); relay ping: the nearest POP.
* The C++ shim needs the SDK to compile; its C side is syntax-checked with `DG_HAVE_STEAM`.

## Time, season and weather

The Maps tab and the lobby pick the session's look: **season** (spring, summer, autumn, winter),
**start time** (hour) and **start weather** (one of the season's presets, or "Season weather
(random)"), plus **dynamic weather**. Each map has defaults (`"season"`, `"time_of_day"`,
`"weather"`); choosing a map in the lobby brings them along.

A season (`assets/world/seasons.json`) sets:

| | Spring | Summer | Autumn | Winter |
|---|---|---|---|---|
| Calendar day (sun path, day length) | 110 | 196 | 288 | 15 |
| Temperature offset | -2 C | +8 C | -4 C | -14 C |
| Weather it brings | showers, clouds, some fog | mostly fair, rare storms | **mostly rain**, overcast, fog | **mostly snow**, overcast, blizzards |
| Ground at the start | damp | dry | wet | snow cover 0.7 |

How the weather follows the season (host, `weather_pick_next`): every scheduled change either takes
a Markov step (clearing / worsening, `pick_markov`) re-weighted by the season or jumps straight to
the season's own distribution; presets the season weights 0 never come. The temperature offset is
added to every target, so in winter every precipitation preset falls as snow, and autumn / spring
rain stays rain. Over a 4000-step walk (test_lobby): autumn 55 % rain, winter 61 % snow and no rain,
summer 75 % fair. With dynamic weather off the start weather stays.

Console: `season [spring|summer|autumn|winter|off] [start preset|random] [static]` (host, any map),
`lobby_season <season> [hour] [weather|random] [static]`, `mm_map <id> [season] [hour] [weather]`.

## Adding a map

1. `assets/maps/<id>.json` (the file stem is the id the lobby and the network use):
   ```json
   { "name": "Forest", "level": "forest", "thumbnail": "assets/maps/thumbs/forest.png",
     "size_m": 1500, "biome": "Forest", "players": [1, 4], "season": "autumn", "time_of_day": 7.5,
     "weather": "", "order": 30, "description": "..." }
   ```
   `level` is what `game_host` loads (`data/levels/<level>.json`, or `streaming` / `flat`);
   `weather` "" = the season's pick; `"dev": true` marks a test map ("Show test maps", on by default,
   hides them when turned off); `"scene": "handpose"` opens that dev scene once the level is up (the
   Hand Pose Editor entry); `size_m` 0 = unbounded. The console's `map` (no argument) lists every level,
   menu entry and scene; `map <menu id>` resolves an entry to its level / scene.
2. A 384 × 216 thumbnail: `python tools/gen_map_thumbs.py` (brush levels are drawn top-down;
   open-world levels: `mapgen_preview data/levels/<level>.json x.png 8` then
   `--preview <id>=x.png`), or any PNG.
3. `test_lobby` validates every map file (required fields, ranges, season, thumbnail and level exist).

The map catalogue is part of the build hash: both sides must have the same maps.

Note: there is no 4 × 4 km map in the repository. The open world (`zombieland`) is 2 × 2 km and the
"Streaming Test Terrain" (`streaming`) is unbounded noise terrain.

## Adding a cosmetic

1. The part: an option in `assets/characters/player.json` `"parts"` (`head`, `torso`, `legs`, `hands`,
   `hair`, `gear_0`..`gear_3`; a mesh, or a placeholder cut from the dummy with `region` / `inflate` /
   `scale` / `materials`, docs/CHARACTERS.md). Outfits for the body slot are skins
   (`assets/human/skins.json` or `assets/skins/`).
2. The item: `assets/cosmetics/<any>.json` `items`:
   `{ "id", "name", "slot", "option", "rarity": common|uncommon|rare|epic|legendary, "unlock":
   default|locked|steam:<itemdef>, "icon": "png" (optional), "tint": [r, g, b], "description" }`.
3. `test_lobby` checks every item resolves to an option of the player character and round-trips
   through the part selection.

Equipping writes the profile and sets `cl_skin` (body) / `cl_parts` (`head:helmet_olive
torso:vest_tan`); the character shows it at once and the host replicates it (`RPC_SET_PART`). Players
wear only what they equip (non-body slots start empty). Weapon skins are a catalogue stub.

## Data and persistence

`<pref dir>/saves/profile.json`: equipped cosmetics, unlocks, the last map, the last lobby settings
(map, difficulty, max players, privacy, season, hour, weather, dynamic, PvP, late join), the LAN name
and stat stubs (kills, deaths, matches, play time). Written on changes and at exit; never in `--test`
runs. Settings stay in the config file.

Quit (power icon / pause / Esc on Play) asks first; on exit `main.c` leaves the lobby (others
migrate), saves the profile and the settings, then shuts Steam down.

## Theme tokens

Screen JSON sizes in the main menu screens are `"@token"` strings resolved from `theme.json`
`"layout"` (`gui_layout_set_tokens`; an unknown token is a parse error). Code uses the same tokens
(`gui_tok`) and the theme's colours. `test_lobby` lints every `mainmenu*.json`: no numeric x / y / w /
h / min / max / gap / pad, no colour keys; ratios (`w_pct`, `flex`, `aspect`) may be numbers.

## Verification

**Unit tests** (`test_lobby`, about 2000 checks): every (state, event) cell; join / full / duplicate
names / kick + rejoin blocked / late join; ready and start gating, forced countdown, cancel, all ready
during the countdown; chat (trim, rate limit per member, 256 cap on input and on the wire, profanity,
scrollback ring, slash commands); version and content mismatch; snapshot / chat / intent round trips
and truncated input; Steam metadata and rich presence strings; migration successor; every map file;
cosmetics → part options → selection → cosmetics for every slot; profile JSON round trip; seasons
(file vs built-ins, presets exist, long scheduler walks per season, winter rain is snow); every main
menu screen linted and solved at 1920×1080, 2560×1080, 1024×768, 1280×720, 3840×2160 and 3440×1440.

**Scripted runs** (`tools/tests/`):
* `mm_nav.cfg` (`--test --menu`): every tab by keys 1-6, Q / E, pad LB / RB, mouse; the browser;
  Back; the quit prompt cancelled; screenshots.
* `mm_resolutions.cfg`: the menu at 1280×720, 1920×1080, 2560×1080, 1024×768.
* `lobby_lan.sh`: two instances on one machine (LAN): host, discovery, join, chat both ways, ready,
  map / season change, kick + rejoin refused, host leaves → promotion, start → both load. The
  scripts sync on `lobby_wait players <n> | ready <n> | state <name> | host [timeout frames]` (the
  two processes boot at different speeds; fixed waits raced). `DG_PORT=27115` uses another LAN port
  (`+set net_port` applies before the boot map hosts; `connect` / `lobby_join` without a port use
  `net_port`).

**Not run here** (needs Steam / two accounts): the Steam lobby list, invites through the friends list
and the overlay, `+connect_lobby` from a cold start, avatars, relay ping, Steam host migration.
