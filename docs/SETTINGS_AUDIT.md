# Settings audit (fair play, consistency, the simplified Graphics tab)

Every registered cvar has exactly one class. What a player may change, what Engine Fidelity sets,
and what is the same for everyone. The table at the end is generated from the registrations
(`python3 tools/tests/lint_settings.py --write`). The lint fails when a registration has no class,
when `assets/ui/settings_audit.json` (the reason and UI place of each cvar) disagrees with it, or when
the table is stale. In game, `settings_audit [filter|class]` prints the same table with the live
lock state.

## 1. Classes and enforcement

| Class | Flag | Who may write | In the UI | In config.cfg |
|---|---|---|---|---|
| Player-editable | `CVAR_EDITABLE` | anyone (UI, console, config), bounded by the fidelity level | yes, or console only | yes |
| Tier-driven | `CVAR_TIER` | Engine Fidelity, the era preset and the post presets (`CVAR_SRC_FIDELITY`) | Graphics > Debug (dev builds) | no |
| Hard-coded | `CVAR_LOCKED` | the consistency set and the host's replication (`CVAR_SRC_HOST`) | no | no |
| Hard-coded, host rule | `CVAR_LOCKED \| CVAR_RULE` | as LOCKED, plus a player who is not someone else's client (the host sets the rules) | Lobby rules / `sv_rule` | yes (the host's own rules) |

- **Release locks** (`src/core/cvar.c` `cvar_writable`): on in a release build (CMake
  `-DDG_DEV_BUILD=OFF`), off in dev builds. `--release-locks` turns them on in a dev build. A
  rejected write keeps the old value and logs `<cvar> is fixed for fair play (docs/SETTINGS_AUDIT.md):
  write "<v>" rejected` (or "set by Engine Fidelity" / "a server rule of the host").
- **Write sources**: `cvar_source_push(CVAR_SRC_FIDELITY)` wraps the fidelity code (`fidelity.c
  set_value`, the group tiers), the era preset (`vk_vgeo.c era_apply`), the post presets
  (`game_post.c apply_preset`) and the fidelity snapshot restore. `CVAR_SRC_HOST` wraps the
  consistency set, `MSG_CVARS`, `sv_rule`, the lobby rules, the config migrations and the viewmodel
  FOV mirror.
- **config.cfg**: `cvar_saved` writes archived EDITABLE cvars and the host's archived rules.
  `config_load_file` drops TIER / LOCKED entries an older config still has, logs the count, and
  rewrites the file on the next save. (The settings file is `config.cfg`; there is no
  `settings.json`.)
- **`developer`** only shows developer UI, and only in a dev build (`dev_ui()` in
  `scr_settings.c`). It never unlocks a write.
- **sv_cheats** is a host rule: on by default in dev builds, off in release builds.

## 2. The consistency set (`assets/render/consistency.json`)

`consistency_init` (after every registration, before the config) applies each entry to its cvar
(also as its default) and logs the set's hash. `lint_settings.py` and `consistency_apply` reject an
entry that isn't a CVAR_LOCKED cvar. Each entry carries its `why`.

- **Build identity**: `consistency_hash()` (FNV-1a over the sorted `name=value` pairs of the file) is
  XORed into the content hash of `lobby_version_hash`, so the server browser shows a different set
  as a version mismatch.
- **Join handshake** (`DG_PROTOCOL_VERSION` 25): the client appends `consistency_hash_effective()`
  (its *current* values of the non-rule entries) to `MSG_HELLO`. The host compares it with its own
  value (`net_consistency_ok`) and rejects a mismatch with `consistency set mismatch (host ..., you
  ...)`. A dev-build client that edited a locked value in the console can't join a host that didn't.
- **Self-check**: `consistency_check` lists entries whose cvar no longer holds the locked value.
  `settings_audit` prints them.

## 3. Server rules (host-controlled values)

`CVAR_RULE` cvars (`sv_rule` lists them) replicate with `MSG_CVARS` like `CVAR_SERVER`, and a change
re-sends them. A client takes the host's values while connected and gets its own back on disconnect
(`rules_save` / `rules_restore`, net.c). Only the host (or an offline player) may set them:
`cvar_set_rule_gate` rejects a client's writes.

- **Lobby rule "Foliage density"** (`LobbySettings.foliage_pct`, 50 .. 100 %): applied as
  `r_foliage_density` by `lobby_apply_environment` on the host, then replicated.
- **Other rules**: `r_fog_scale`, `world_chunk_radius` / `world_lod_distance` (draw distance),
  `dev_damage_numbers`, the spectator free camera, every `sv_*`, `pl_*` and `wpn_*` gameplay cvar.

## 4. The ambiguous calls (reasoning)

- **AO, contact shadows, SSR, SSGI**: tier. SSR only reflects pixels already on screen; AO darkens
  corners slightly more at high levels (the task puts AO in the tier list).
- **Shadows**: Hard vs Soft, resolution, cascades and filter are tiers, but every level has at least
  Hard shadows (`min: 1`). "Disable" removed enemy shadows *and* the dark interiors that hide
  players.
- **Lamp / flashlight / cloud / moon shadows**: locked on. Without lamp shadows a lamp lights a
  player behind a wall. The lamp tile size stays a tier.
- **Bloom**: on / off and its mip quality are tiers (Off has none). Its strength, lens flare, lens
  dirt, lens distortion and the tonemapper are locked to the Look's values (flare, dirt and
  distortion are 0).
- **Lens droplets**: hard-coded on at every level (the fairness default). Turning them off cleared
  the view in rain.
- **Rain / snow**: the streak count depends on the weather only (9000 x intensity). The quality tier
  changes the ground splashes, and it is never 0.
- **Particles**: emission counts, collision, the sort budget and the effect lights are the same at
  every `vfx_quality`. Lighting and the half-res pass stay tiers.
- **Gore**: stays a player choice (content / age rating), moved to Character > Content. Gore Off
  shows dust puffs instead of blood, so hits stay readable. Corpse bleeding is locked.
- **Weapon kick** 0.5 .. 1: at 0 the sight picture stayed still while the aim recoiled.
  **Weapon sway** 0 .. 1 (cosmetic: aiming stays exact). **Camera shake** (`cam_kick_scale`)
  0.25 .. 1 for motion sickness. It also scales the fire punch, which used to be off at Low and
  Medium: a steadier picture on those levels. The fire-animation tuning (FIRE_ANIM.md, ONE global
  sway) is locked.
- **Motion blur**: camera / object blur is editable. Camera blur never gathers the viewmodel
  (`mb_blur.comp`), so it can't mask recoil. The 1–2 tick fire blur spike follows Camera shake.
- **Depth of field** Off / When aiming / Always: editable. Off also turns off the ADS weapon blur.
- **Screen effects** (`r_post_feedback`) 0.5 .. 1: photosensitivity, never off (at 0 a flashbang
  did nothing).
- **Brightness** 0.85 .. 1.15 (was 0.3 .. 3). **Gamma** locked at 1 (was 0.5 .. 2), and
  **exposure compensation** at 0: they lifted the night darkness floor. **HDR paper white**
  100 .. 300 nits. Peak nits is the monitor's (console only).
- **FOV**: 60 .. 120 deg vertical (`cl_fov`; was 50 .. 120). `sv_fov` / `sv_fov_max` still let a
  server fix or cap it. The sprint FOV kick is 0 .. 7 deg. The viewmodel FOV and the per-sight ADS
  FOV are locked.
- **Crosshair**: style, colour, size, opacity and outline are editable, and so are hit markers and
  target tint (turning them off only removes your own information). The obstruction behaviour and
  follow-kick (true aim point vs screen centre) are locked.
- **Health bars, damage numbers**: game state, off. Damage numbers are a host rule.
- **Stretched resolution**: locked 0 (the 4:3-stretch trick makes targets wider).
- **Occlusion culling**: locked at two-pass. Last frame's Hi-Z drew a player stepping out of cover
  one frame late.
- **Era**: editable, but an era only changes how geometry is built and drawn (vgeo vs classic, the
  2010 three-LOD chain, the shadow-view error). Its foliage density (1 / 0.7 / 0.4 / 0.2) and LOD
  thresholds (2 / 3 / 4 / 12 px) are gone.
- **Character animation** (lean, foot IK, ragdolls, corpse lifetime): locked. Every client sees
  remote bodies the same way.
- **Networking**: `cl_interp` is locked (100 ms). Snapshot rate and relevance are host rules.
- **Phone**: its settings stay editable. The phone is an in-game item with the same controls for
  everyone.

## 5. Tier violations found and fixed

| # | Was | Fix |
|---|---|---|
| V1 | foliage density capped 0.1 / 0.25 / 0.5 / 0.8 / 1.0 by level, 1 / 0.7 / 0.4 / 0.2 by era | locked 1.0 (lobby rule 50 .. 100 %); out of every tier table and the era preset |
| V2 | impostor switch 96 .. 16 px by level; classic 64 px vs vgeo 16 px | locked 32 px for both renderers |
| V3 | vgeo error x4 .. x1, classic LOD pick x8 .. x1 by level, eras 2 .. 12 px | locked 1 px / 2 px; levels keep the shadow-view error, software-raster threshold and streaming budgets |
| V4 | character LOD bias 0.25 .. 1; `r_char_quality` only scaled the LOD thresholds | both locked (bias 1, quality 2 = x1) |
| V5 | sun shadow distance 50 .. 300 m (era override 70); Off had no sun shadows; cloud / lamp / moon shadows off on low levels | locked 150 m (Off 2 cascades, Low 4 cascades keep near detail); Hard minimum; cloud / lamp / moon shadows locked on |
| V6 | froxel fog `max(level * 1.5, 0.004) * r_fog_scale` + weather; without volumetrics the lit shaders used the bare level / time-of-day density (thinner, far thinner in fog weather) | `game_post_fog_density()` feeds the analytic fog (game_render.c): one density for every level |
| V7 | water tier 0 had no refraction (swimmers hidden on Off / Low) | water tier >= 1 at every level |
| V8 | rain / snow off (0) allowed on Off / Low; streaks 4000 / 9000 / 16000 by quality | never off; streak count fixed (9000 x intensity); quality = splashes |
| V9 | particle emission x0.35 .. x1.35 by `vfx_quality`, no collision at 0, sort budget 8k .. 128k, 8 vs 24 effect lights; decals 16 .. 4096 by level | counts, collision, sort budget (64k) and lights fixed; decal budget locked 256 |
| V10 | Off: no post (fixed exposure, no night floor) | Off runs the minimal post pass; auto exposure locked on |
| V11 | lens droplets missing below High | locked on at every level |
| V12 | `lod.json` tiers 0.3 / 0.4 / 0.6 / 0.85 / 1 and eras 0.85 / 0.7 scaled every classic / character / terrain / VFX LOD range | tiers and eras 1 (data and built-ins) |
| V13 | the fire punch (FOV punch, roll, blur spike) off at Low, 0.6 at Medium | the same at every level, x Camera shake |
| V14 | vgeo / classic LOD, impostor, foliage fade and culling, the character LODs and the scope's LOD measured in render-target pixels: render scale, the upscaler, the monitor's resolution and the scope picture resolution moved every switch | measured at a 1080-pixel reference height (`RENDER_LOD_REF_HEIGHT`, render.h) |

Tier tables may no longer list a locked feature: `fid_tables_complete` rejects one.
`test_fidelity` checks that every level x era resolves every locked feature to the consistency value,
whatever level came before.

## 6. The Settings tabs

```
Graphics   (i) Why can't I change X?                                       [Default]
  Engine Fidelity   level · description · benchmark · Run benchmark · Apply recommended · Reset tweaks
  Era · Look (preview strip, hold to compare)
  DISPLAY  Resolution · Window mode · VSync · Revert / Apply · Frame rate limit · Render scale ·
           Upscaling (+ resolution) · HDR10 + paper white · Brightness · Colour-blind filter · Show FPS
  COMFORT  Field of view · Sprint FOV effect · Head bob · Camera shake · Weapon sway · Weapon kick ·
           Motion blur (+ strength) · Depth of field · Film grain · Chromatic aberration · Vignette ·
           Sharpening · Screen effects
HUD        Crosshair (style, colour, length, thickness, extra gap, opacity, outline, target tint, hit
           markers, red dot brightness) · Layout (HUD scale, opacity, phone panel) · Compass (+ arc
           90 .. 180 deg) · Kills · Pings · Radial wheels
Controls · Audio · Character (skin, interface, Content: gore, spectating) · Phone
```

The "(i)" row's tooltip explains the locks. The Graphics > Debug sub-tab (every per-feature control)
exists only in a dev build in developer mode. `menu crosshair` / `settings_open(2)` open the HUD tab.

## 7. Tests

- `lint_settings` (ctest): one class flag on every registration; no archived TIER / LOCKED cvar;
  settings_audit.json and consistency.json agree with the flags; no TIER / LOCKED cvar on a player
  page of scr_settings.c; the Graphics tab is exactly Fidelity / Era / Look / Display / Comfort; the
  HUD tab starts with the crosshair; this table is current.
- `test_settings` (ctest): release locks reject player writes of TIER / LOCKED cvars and accept
  fidelity / host writes; rule cvars follow the rule gate; `config_save` leaves TIER / LOCKED out and
  `config_load_file` drops them; the consistency set loads, applies, hashes stably, and an edited
  value changes the effective hash; `consistency_join_ok` (the MSG_HELLO check) rejects a mismatch or a missing hash; every
  fidelity level x era resolves the same value for every locked cvar (the resolved set diffed per
  tier).
- `test_fidelity` / `test_lod` / `test_shadow_*`: the tier tables after the fixes (above).
- `tools/tests/settings_tiers.cfg` + `run_settings_tests.py`: renders the dev room's foliage yard in
  fog at every level, vgeo and classic, and compares `settings_visibility` (foliage instances,
  impostors, fog density, shadow distance and types, LOD scales, floors) within tolerance.
- `tools/tests/settings_locks.cfg` (`--release-locks`): `settings_lock_test` on an EDITABLE, a TIER,
  a LOCKED and a host-rule cvar, after level and era changes (none of which may trip a lock). Both
  .cfg files run from `run_settings_tests.py`, which also runs headless on Linux (xvfb + Mesa
  lavapipe): every level draws the same foliage instances and impostors (vgeo 1485 .. 1486 / 1121 ..
  1122, classic 3636 .. 3638 / 1797 .. 1799 in the foliage yard) with the same fog and locked values.
- `tools/tests/consistency_net.sh` (two instances + one): a client with the same consistency set
  joins, one whose console changed `r_gamma` is rejected with the reason, the host's
  `sv_rule r_foliage_density 0.6` reaches the joined client and goes away when it leaves.

## 8. Full classification (every registered cvar)

<!-- settings_audit:begin (generated by tools/tests/lint_settings.py --write) -->
459 cvars: 128 EDITABLE, 52 TIER, 198 LOCKED, 81 LOCKED + host rule. "UI" "—": in no settings tab.

| Cvar | Category | Class | UI | Locked in release | Reason |
|---|---|---|---|---|---|
| `snd_master` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `snd_music` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `snd_sfx` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `snd_ui` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `snd_voice` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `snd_weather` | audio | EDITABLE | Audio | no | Volumes: personal; every sound plays the same for everyone. |
| `cam_kick_scale` | camera | EDITABLE | Graphics > Comfort | no | Camera shake: visual camera kick on shots, 0.25 .. 1 (accessibility). Aim recoil is separate (wpn_recoil_scale, host). Also scales cl_fire_punch. |
| `cl_fov` | camera | EDITABLE | Graphics > Comfort | no | World FOV: personal comfort within the band 60 .. 120 deg vertical (was 50 .. 120). sv_fov / sv_fov_max still override. |
| `cl_headbob` | camera | EDITABLE | Graphics > Comfort | no | Head bob amount 0 .. 1 (was 0 .. 3; above 1 only makes it worse). cl_headbob stays as its on/off alias. |
| `cl_headbob_amount` | camera | EDITABLE | Graphics > Comfort | no | Head bob amount 0 .. 1 (was 0 .. 3; above 1 only makes it worse). cl_headbob stays as its on/off alias. |
| `cl_sprint_fov` | camera | EDITABLE | Graphics > Comfort | no | Sprint FOV kick: comfort (motion sickness); bounded 0 .. 7 deg, it can only narrow the change. |
| `hud_compass` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_compass_arc` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_coords` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_killcount` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_killcount_split` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_killfeed` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_opacity` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_phone_corner` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_phone_size` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_ping_compass` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_ping_markers` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_scale` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `hud_timed_ring` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `ping_chat` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ping_enable` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ping_sound` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ui_safe_area` | hud | EDITABLE | Character > Interface | no | UI scale / safe area: display. |
| `ui_scale` | hud | EDITABLE | Character > Interface | no | UI scale / safe area: display. |
| `wheel_enable` | hud | EDITABLE | HUD | no | Radial wheels: input comfort (slow motion only applies alone). |
| `wheel_heal_auto_use` | hud | EDITABLE | HUD | no | Radial wheels: input comfort (slow motion only applies alone). |
| `wheel_hold_time` | hud | EDITABLE | HUD | no | Radial wheels: input comfort (slow motion only applies alone). |
| `wheel_timescale` | hud | EDITABLE | HUD | no | Radial wheels: input comfort (slow motion only applies alone). |
| `cl_ads_sensitivity` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_crouch_toggle` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_invert_y` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_sensitivity` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_sensitivity_y` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_sprint_toggle` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `pad_deadzone` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `pad_sensitivity` | input | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_name` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `lobby_privacy` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `net_autohost` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `net_port` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `net_showstats` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `sv_hostname` | network | EDITABLE | — | no | Identity / networking preferences / console. |
| `phone_auto_pair` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_battery_drain` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_bind` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_exposure` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_filter` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_fps` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_pitch` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_roll` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_yaw` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_pair_timeout` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_pairing` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_photo_hires` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_port` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_quality` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_record_max` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_res_h` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_res_w` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_sensitivity` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_server` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_smoothing` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_tls` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_token_ttl` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_video_kbps` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `cl_fp_body` | player | EDITABLE | — | no | Own body in first person: only your view of yourself. |
| `cl_showfps` | player | EDITABLE | Graphics > Display | no | FPS overlay toggle (kept as asked). |
| `cl_skin` | player | EDITABLE | Character | no | Skin: cosmetic, seen by everyone (same set for all). |
| `developer` | player | EDITABLE | — | no | Identity / networking preferences / console. |
| `g_map` | player | EDITABLE | — | no | Identity / networking preferences / console. |
| `spec_blend_time` | player | EDITABLE | Character > Spectating | no | Death cam time / blend: personal pacing of your own spectator camera. |
| `spec_death_cam_time` | player | EDITABLE | Character > Spectating | no | Death cam time / blend: personal pacing of your own spectator camera. |
| `spec_show_delay` | player | EDITABLE | Character (dev) | no | Spectator delay readout. |
| `r_brightness` | rendering | EDITABLE | Graphics > Display | no | Brightness: kept for display calibration but narrowed to 0.85 .. 1.15 (was 0.3 .. 3.0, which lifted the night darkness floor). Applied after tonemapping like before. |
| `r_chromatic` | rendering | EDITABLE | Graphics > Comfort | no | Film grain, chromatic aberration, vignette, sharpening: cosmetic, each bounded by the level and the Look. |
| `r_colorblind` | rendering | EDITABLE | Graphics > Display | no | Colour-blind correction: accessibility; remaps hues, not luminance, at every level (Off included). |
| `r_dof` | rendering | EDITABLE | Graphics > Comfort | no | Depth of field Off / When aiming / Always: cosmetic, only ever blurs your own view. Off also disables the ADS weapon blur (ads_dof). |
| `r_era` | rendering | EDITABLE | Graphics (top) | no | Era: kept editable, but eras may only change geometry *style* and cost (vgeo vs classic, shading). Their foliage density and LOD columns move to the locked set (see violations V1 / V3). |
| `r_fidelity` | rendering | EDITABLE | Graphics (top) | no | Engine Fidelity level: the one quality choice a player makes. |
| `r_fidelity_autodetect` | rendering | EDITABLE | — | no | Fidelity bookkeeping written by the game (benchmark results, recommendation, auto-detect, target fps). |
| `r_fidelity_bench` | rendering | EDITABLE | — | no | Fidelity bookkeeping written by the game (benchmark results, recommendation, auto-detect, target fps). |
| `r_fidelity_custom` | rendering | EDITABLE | — | no | Player tweaks of EDITABLE fidelity features only (render scale, upscaler, motion blur, DoF, grain, CA, vignette, sharpen). TIER / LOCKED features are no longer stored here; old entries are dropped on load. |
| `r_fidelity_recommended` | rendering | EDITABLE | — | no | Fidelity bookkeeping written by the game (benchmark results, recommendation, auto-detect, target fps). |
| `r_fidelity_target_fps` | rendering | EDITABLE | — | no | Fidelity bookkeeping written by the game (benchmark results, recommendation, auto-detect, target fps). |
| `r_film_grain` | rendering | EDITABLE | Graphics > Comfort | no | Film grain, chromatic aberration, vignette, sharpening: cosmetic, each bounded by the level and the Look. |
| `r_hdr_max_nits` | rendering | EDITABLE | — | no | HDR display peak: a property of the monitor (tone-map shoulder), not of the scene; console only, defaults from the display's reported HDR headroom. |
| `r_hdr_output` | rendering | EDITABLE | Graphics > Display | no | HDR10 output: a display capability, purely tonal. |
| `r_hdr_paper_white` | rendering | EDITABLE | Graphics > Display | no | HDR paper white: the HDR equivalent of brightness; bounded to 100 .. 300 nits so it cannot lift night scenes past the SDR brightness band. |
| `r_height` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_look` | rendering | EDITABLE | Graphics (top) | no | Look: five fixed grading presets (LOOKS.md). They grade colour; exposure floor / fog / bloom strength are locked underneath, and lint_looks checks no look lifts the night floor. |
| `r_maxfps` | rendering | EDITABLE | Graphics > Display | no | Frame cap: personal; no effect on what is drawn. |
| `r_mode` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_motion_blur` | rendering | EDITABLE | Graphics > Comfort | no | Motion blur (camera / camera + objects): comfort; the viewmodel is masked out of the velocity buffer so blur can never hide the weapon's recoil (hard-coded). |
| `r_motion_blur_strength` | rendering | EDITABLE | Graphics > Comfort | no | Motion blur (camera / camera + objects): comfort; the viewmodel is masked out of the velocity buffer so blur can never hide the weapon's recoil (hard-coded). |
| `r_perf_overlay` | rendering | EDITABLE | — | no | Performance overlay (frame times + budget): diagnostics only, shows nothing about the world. Developer page. |
| `r_post_feedback` | rendering | EDITABLE | Graphics > Comfort | no | Screen effects (damage / flash / low health): photosensitivity accessibility, bounded 0.5 .. 1 (was 0 .. 1: zero hid every flashbang). |
| `r_render_scale` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level (fidelity bounds). Fewer pixels look softer but never remove objects (LOD / culling no longer depend on it, see locked rows). |
| `r_sharpen` | rendering | EDITABLE | Graphics > Comfort | no | Film grain, chromatic aberration, vignette, sharpening: cosmetic, each bounded by the level and the Look. |
| `r_upscale_ratio` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level (fidelity bounds). Fewer pixels look softer but never remove objects (LOD / culling no longer depend on it, see locked rows). |
| `r_upscaler` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level (fidelity bounds). Fewer pixels look softer but never remove objects (LOD / culling no longer depend on it, see locked rows). |
| `r_vignette` | rendering | EDITABLE | Graphics > Comfort | no | Film grain, chromatic aberration, vignette, sharpening: cosmetic, each bounded by the level and the Look. |
| `r_vsync` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_width` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `cfg_migration` | system | EDITABLE | — | no | Config migration counter: bookkeeping the game writes (saved, so migrations run once). |
| `con_enable` | system | EDITABLE | — | no | Identity / networking preferences / console. |
| `con_favorites` | system | EDITABLE | — | no | Identity / networking preferences / console. |
| `con_recent` | system | EDITABLE | — | no | Identity / networking preferences / console. |
| `gore_level` | vfx | EDITABLE | Character > Content | no | Gore (moved from Graphics): content / age rating, must stay a choice. Gore Off keeps neutral impact puffs, so hits stay readable. |
| `ads_sens_coef` | weapons | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `ads_sens_mode` | weapons | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_weapon_sway` | weapons | EDITABLE | Graphics > Comfort | no | Weapon sway (cosmetic: aiming stays exact, crosshair tab note): 0 .. 1. |
| `crosshair_color` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_gap` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_hitmarker` | weapons | EDITABLE | HUD > Crosshair | no | Hit markers / target tint: turning them off only removes information from yourself; their timing / accuracy is fixed in code. |
| `crosshair_length` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_opacity` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_outline` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_style` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_target_color` | weapons | EDITABLE | HUD > Crosshair | no | Hit markers / target tint: turning them off only removes information from yourself; their timing / accuracy is fixed in code. |
| `crosshair_thickness` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `sight_brightness` | weapons | EDITABLE | HUD > Crosshair | no | Red dot brightness (moved from Graphics): bounded 0.25 .. 2 (was 0 .. 4). |
| `wpn_visual_scale` | weapons | EDITABLE | Graphics > Comfort | no | Weapon kick (visual camera + viewmodel kick): bounded 0.5 .. 1 (was 0 .. 3). Zero kick would give a still sight picture while the aim recoils, an advantage when spraying. |
| `cl_clock` | world | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
| `anim_eval_budget` | animation | TIER | — | yes (Engine Fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
| `r_aa` | rendering | TIER | — | yes (Engine Fidelity writes) | Anti-aliasing mode: a quality / cost trade at the same visibility; picked by the level. |
| `r_aniso` | rendering | TIER | — | yes (Engine Fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_ao` | rendering | TIER | — | yes (Engine Fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_bloom` | rendering | TIER | — | yes (Engine Fidelity writes) | Bloom on / off follows the level; its strength is locked to the Look (r_bloom_intensity). |
| `r_bloom_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | DoF / bloom gather quality. |
| `r_char_maps` | rendering | TIER | — | yes (Engine Fidelity writes) | Character skin PBR maps: shading quality. |
| `r_contact_shadows` | rendering | TIER | — | yes (Engine Fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_dof_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | DoF / bloom gather quality. |
| `r_foliage_wind` | rendering | TIER | — | yes (Engine Fidelity writes) | Foliage *shading* tier: wind layers. Density / fade distances are locked. |
| `r_grass_sss` | rendering | TIER | — | yes (Engine Fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_grass_tex` | rendering | TIER | — | yes (Engine Fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_light_shadow_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | Lamp / flashlight shadow tile size (256 / 512 px). Whether lamps are shadowed is locked. |
| `r_post_preset` | rendering | TIER | — | yes (Engine Fidelity writes) | Post preset: follows the level. |
| `r_precip_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | Rain / snow shading tier. Particle density is locked (violation V8): tiers change shading, never count. |
| `r_puddle_reflections` | rendering | TIER | — | yes (Engine Fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_scope_pip_scale` | rendering | TIER | — | yes (Engine Fidelity writes) | Scope picture resolution: like render scale for the scope (sharpness, not content; the scope's LOD is locked). |
| `r_shadow_cascade_border` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_cascades` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_min_radius` | rendering | TIER | — | yes (Engine Fidelity writes) | Small props (< radius) skip the sun shadow: cost only; characters, vehicles and doors are exempt (to verify in the shadow caster filter). |
| `r_shadow_near_offset` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_projection` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_resolution` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split2` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_1` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_2` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_3` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadowmask_distance` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadowmask_mode` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadows` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow mode: Hard vs Soft is a quality tier, but every level has at least Hard: Disable removed every sun shadow (enemy shadows, dark interiors), an advantage (V5). |
| `r_soft_shadow_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_ssgi` | rendering | TIER | — | yes (Engine Fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_ssr` | rendering | TIER | — | yes (Engine Fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_terrain_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_terrain_tex_size` | rendering | TIER | — | yes (Engine Fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_texture_quality` | rendering | TIER | — | yes (Engine Fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_vgeo` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_characters` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_pool_mb` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_shadow_error_px` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_shadows` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_stream_pages_per_frame` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_sw_px` | rendering | TIER | — | yes (Engine Fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vm_shadow` | rendering | TIER | — | yes (Engine Fidelity writes) | Viewmodel shadow tile: only your own arms. |
| `r_vol_local_lights` | rendering | TIER | — | yes (Engine Fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog *density* is locked (r_fog_scale) and the distance-fog fallback must match it (violation V6). |
| `r_volumetrics` | rendering | TIER | — | yes (Engine Fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog *density* is locked (r_fog_scale) and the distance-fog fallback must match it (violation V6). |
| `r_water` | rendering | TIER | — | yes (Engine Fidelity writes) | Water shading tier (ripples, caustics, waves). Transparency / turbidity are locked (violation V7: tier 0 drops refraction). |
| `vfx_half_res` | vfx | TIER | — | yes (Engine Fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `vfx_lighting` | vfx | TIER | — | yes (Engine Fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `vfx_quality` | vfx | TIER | — | yes (Engine Fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `ads_dof` | weapons | TIER | — | yes (Engine Fidelity writes) | ADS weapon blur: availability by level, and r_dof 0 turns it off. |
| `world_chunk_uploads` | world | TIER | — | yes (Engine Fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
| `anim_corpse_collision` | animation | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `anim_damage` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `anim_foot_ik` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_forearm_twist` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_hold` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_hold_assert` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `anim_hold_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `anim_ik_shoulders` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_lean` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_local_resim` | animation | LOCKED | — | yes | Spectator extrapolation / pop warning: netcode tuning. |
| `anim_max_ragdolls` | animation | LOCKED | — | yes | Corpse lifetime / ragdoll cap: corpses are cover and a DNA source (REVIVE.md). |
| `anim_net_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `anim_ragdoll` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_ragdoll_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `anim_ragdoll_time` | animation | LOCKED | — | yes | Corpse lifetime / ragdoll cap: corpses are cover and a DNA source (REVIVE.md). |
| `anim_resync_ticks` | animation | LOCKED | — | yes | Spectator extrapolation / pop warning: netcode tuning. |
| `anim_sample_rate` | animation | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `anim_spine` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `anim_turn` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players must look the same to everyone; lean off would hide a peeking head. |
| `char_inspect` | animation | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hitbox_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `snd_trace` | audio | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `cam_bob_scale` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_collision_radius` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_death_follow` | camera | LOCKED | — | yes | Death / downed sequence and its camera (task: death / downed sequences). |
| `cam_death_spectate` | camera | LOCKED | — | yes | Death / downed sequence and its camera (task: death / downed sequences). |
| `cam_debug` | camera | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `cam_head_clamp` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_head_damping` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_head_follow` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_head_follow_lateral` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_head_stiffness` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_recon_smooth_time` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cam_third` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `cam_weapon_fov` | camera | LOCKED | — | yes | Viewmodel FOV (task: viewmodel FOV locked). Hands follow the camera FOV; the sight's ADS zoom still applies. |
| `cl_ads_world_zoom` | camera | LOCKED | — | yes | ADS FOV per sight: world zoom and eye relief come from the sight data. |
| `cl_camera_socket` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cl_camera_socket_follow` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cl_camera_socket_roll` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): changes aim stability and what peeks over cover. One rig for all. |
| `cl_thirdperson` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `cl_thirdperson_dist` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `handpose_autoreg` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `handpose_editor_dir` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `handpose_watch` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hands_debug` | camera | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `hands_forearm` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hands_upper_arm` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `holdpoint_force_state` | camera | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `holdpoint_static` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `peek_distortion` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_fov` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_pitch_limit` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_yaw_limit` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `ui_debug` | hud | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `cl_interp` | network | LOCKED | — | yes | Networking (task: interp exposed as a knob): entity interpolation delay, archived and client-set today. |
| `cl_predict_hit_reactions` | network | LOCKED | — | yes | Hit feedback prediction (own impacts / reactions shown before the host confirms): timing of game state; on for all. |
| `cl_predict_impacts` | network | LOCKED | — | yes | Hit feedback prediction (own impacts / reactions shown before the host confirms): timing of game state; on for all. |
| `host_timescale` | network | LOCKED | — | yes | Time scale: cheat. |
| `net_fakejitter` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `net_fakelag` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `net_fakeloss` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `cl_fp_debug` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `cl_healthbars` | player | LOCKED | — | yes | Health bars over characters: shows enemy health (game state). Off; a lobby rule may turn them on for everyone. |
| `cl_interact_range` | player | LOCKED | — | yes | Interaction ray length: gameplay. |
| `cl_parts` | player | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `dev_fixed_fps` | player | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `pl_debug_mantle` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `spec_dump` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `spec_extrap_ticks` | player | LOCKED | — | yes | Spectator extrapolation / pop warning: netcode tuning. |
| `spec_pop_warn` | player | LOCKED | — | yes | Spectator extrapolation / pop warning: netcode tuning. |
| `r_atmosphere` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `r_auto_exposure` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Exposure: compensation (+-4 EV) and auto-exposure on/off move the night darkness floor; the Off level has fixed exposure today (V10). |
| `r_bloom_intensity` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. Flare / dirt / distortion are 0 at every level today. |
| `r_char_normal_scale` | rendering | LOCKED | — | yes | Character normal map strength: authored value. |
| `r_char_quality` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Character quality only scaled the character LOD thresholds (0.5 .. 1.25x by level, V4): fixed at 2 (x1). |
| `r_classic_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees turn into cards; vgeo 16 px vs classic 64 px today, and tiers 96 .. 16 px (V2); locked 32 px for both renderers. |
| `r_cloud_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Cloud shadows darken patches of the map: Off / Low drop them (V5). |
| `r_cpu_skinning` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_culling` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Frustum culling on/off (dev toggle). |
| `r_drawcollision` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_drawhitboxes` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_exposure` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Exposure: compensation (+-4 EV) and auto-exposure on/off move the night darkness floor; the Off level has fixed exposure today (V10). |
| `r_fb_underwater` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable can drop the fog pass). |
| `r_foliage_wind_solo` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_gamma` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Gamma 0.5 .. 2.0 lifts shadows like a cheat; folded into the narrow brightness band, gamma fixed at 1.0. |
| `r_grass_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_grid` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Unit grid drawn on dev geometry / terrain. |
| `r_ground_macro` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on/off: dev switches (vertex colours change ground contrast). |
| `r_lens_dirt` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. Flare / dirt / distortion are 0 at every level today. |
| `r_lens_distortion` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. Flare / dirt / distortion are 0 at every level today. |
| `r_lens_drops` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lens droplets: fairness default, hard-coded on (switching them off clears the view in rain); missing below High today (V11). |
| `r_lens_drops_intensity` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lens droplets: fairness default, hard-coded on (switching them off clears the view in rain); missing below High today (V11). |
| `r_lens_flare` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. Flare / dirt / distortion are 0 at every level today. |
| `r_light_shadow_count` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lamp / flashlight shadows: without them lamps light through walls and players behind cover; Off / Low drop them (V5). Tile size stays a tier. |
| `r_light_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lamp / flashlight shadows: without them lamps light through walls and players behind cover; Off / Low drop them (V5). Tile size stays a tier. |
| `r_lod_bias` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Character LOD bias: tiers 0.25 .. 1.0 (V4). |
| `r_lod_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_lod_fade_band` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD / impostor cross-fade band and time: part of the LOD ranges. |
| `r_lod_fade_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_lod_fade_time` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD / impostor cross-fade band and time: part of the LOD ranges. |
| `r_lod_force` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_lod_legacy` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Scope LOD rule and the legacy LOD path: dev switches. |
| `r_lod_scope` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Scope LOD rule and the legacy LOD path: dev switches. |
| `r_look_modulators` | rendering | LOCKED | — | yes | Look modulators: dev switch. |
| `r_lut` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. User LUT file: could flatten the darkness; dev only. |
| `r_moon_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Twilight moon shadows: darkness at dusk; missing below High (V5). |
| `r_post_compare` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable can drop the fog pass). |
| `r_post_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_post_disable` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable can drop the fog pass). |
| `r_post_profile` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable can drop the fog pass). |
| `r_post_volumes_draw` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable can drop the fog pass). |
| `r_shader_hotreload` | rendering | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `r_shadow_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_shadow_max_distance` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Sun shadow distance: tiers 50 .. 300 m (max 1000) today (V5). One distance for everyone; tiers keep resolution / cascades. |
| `r_stretch` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Stretched resolution makes targets wider on screen (the 4:3 stretch trick): fixed 0. |
| `r_surfaces` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on/off: dev switches (vertex colours change ground contrast). |
| `r_terrain` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on/off: dev switches (vertex colours change ground contrast). |
| `r_terrain_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_terrain_watch` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_tonemapper` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. Flare / dirt / distortion are 0 at every level today. |
| `r_vgeo_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_vgeo_debug_spheres` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_vgeo_error_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD error / pick thresholds decide silhouettes at range (task: vgeo error threshold, LOD bias). Tiers scale them x4 .. x1 (vgeo) and x8 .. x1 (classic), eras 2 .. 12 px (V3). |
| `r_vgeo_freeze` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_vgeo_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees turn into cards; vgeo 16 px vs classic 64 px today, and tiers 96 .. 16 px (V2); locked 32 px for both renderers. |
| `r_vgeo_lod_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD error / pick thresholds decide silhouettes at range (task: vgeo error threshold, LOD bias). Tiers scale them x4 .. x1 (vgeo) and x8 .. x1 (classic), eras 2 .. 12 px (V3). |
| `r_vgeo_occlusion` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Occlusion culling mode: mode 1 (last frame's Hi-Z) shows a player stepping out from cover one frame late; two-pass (2) for everyone. |
| `r_viewmodel_fov` | rendering | LOCKED | — | yes | Viewmodel FOV (task: viewmodel FOV locked). Hands follow the camera FOV; the sight's ADS zoom still applies. |
| `r_water_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_wireframe` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `death_impact_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `death_sequence` | revive | LOCKED | — | yes | Death / downed sequence and its camera (task: death / downed sequences). |
| `revive_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `vehicle_debug` | vehicles | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `fx_tracers` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `gore_bleed` | vfx | LOCKED | — | yes | Gore follows gore_level: wounds bleeding (blood trails are information). |
| `vfx_debug` | vfx | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `vfx_decal_max` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impact decal budget: tiers keep 16 .. 4096 decals (V9); one budget. |
| `vfx_enable` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Particle effects on/off (muzzle flashes, impacts): dev only. |
| `vfx_max_particles` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Particle pool size: a small pool silently drops smoke / impact particles. |
| `vfx_timescale` | vfx | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom: dev views. |
| `ads_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `ads_dof_focus` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_dof_fstop` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_dof_near_blur` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_eye_relief_scale` | weapons | LOCKED | — | yes | ADS FOV per sight: world zoom and eye relief come from the sight data. |
| `aim_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `bal_bullet_trace` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_crack_radius` | weapons | LOCKED | — | yes | Supersonic crack radius: an audio cue of incoming fire. |
| `bal_tracer_ignition` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_vapor_trails` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_watch` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `ballistics_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `cl_fire_punch` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): the item kick, master scale, punch, item bob, look lag. Player-facing control is Weapon kick / sway / Camera shake. |
| `crosshair_follow_kick` | weapons | LOCKED | — | yes | Crosshair follow-kick changes the crosshair from the true aim point to the screen centre: spread / aim behaviour, not style. |
| `crosshair_obstruct` | weapons | LOCKED | — | yes | Crosshair obstruction (task: obstruction crosshair behaviour): where the bullet will land. |
| `crosshair_obstruct_lerp` | weapons | LOCKED | — | yes | Crosshair obstruction (task: obstruction crosshair behaviour): where the bullet will land. |
| `shutter_preview` | weapons | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom: dev views. |
| `sight_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `wpn_bob_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): the item kick, master scale, punch, item bob, look lag. Player-facing control is Weapon kick / sway / Camera shake. |
| `wpn_debug_melee` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `wpn_fire_anim_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): the item kick, master scale, punch, item bob, look lag. Player-facing control is Weapon kick / sway / Camera shake. |
| `wpn_fire_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `wpn_kick_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): the item kick, master scale, punch, item bob, look lag. Player-facing control is Weapon kick / sway / Camera shake. |
| `wpn_look_lag` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): the item kick, master scale, punch, item bob, look lag. Player-facing control is Weapon kick / sway / Camera shake. |
| `wpn_recoil_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `env_night` | world | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom: dev views. |
| `sky_aerial_max` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `sky_aerial_scale` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `sky_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `sky_disc_zoom` | world | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom: dev views. |
| `tod_day_ambience` | world | LOCKED | — | yes | Ambience sound overrides: audio cues are the same for everyone. |
| `tod_night_ambience` | world | LOCKED | — | yes | Ambience sound overrides: audio cues are the same for everyone. |
| `tod_on_midnight` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_noon` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_sunrise` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_sunset` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `water_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `water_wave_wind_scale` | world | LOCKED | — | yes | Water wave wind scale: cheat tuning. |
| `weather_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `weather_on_rain_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_rain_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_snow_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_snow_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_storm_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_storm_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `wind_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `wind_dir` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind must come from the weather. |
| `wind_gust` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind must come from the weather. |
| `wind_solo` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `wind_speed` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind must come from the weather. |
| `wind_timescale` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind must come from the weather. |
| `wind_view` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `world_debug_chunks` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `world_foliage_test` | world | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `world_terrain_grid` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Unit grid drawn on dev geometry / terrain. |
| `horde_debug` | zombies | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `sv_fov` | camera | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max` | camera | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max_aspect` | camera | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_infinite_ammo` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_join_dead` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_maxplayers` | network | LOCKED+RULE | — | yes (+ host rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_relevance` | network | LOCKED+RULE | — | yes (+ host rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_respawn_mode` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_respawn_time` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_snaprate` | network | LOCKED+RULE | — | yes (+ host rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_spec_allowed` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_spec_fp_enemies` | network | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `dev_damage_numbers` | player | LOCKED+RULE | — | yes (+ host rule) | Damage numbers (archived, on by default): game state; a lobby rule. |
| `pl_accel` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_accel` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_control` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_coyote_time` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_crouch_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_friction` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_gravity` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_reach` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_yaw_limit` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_buffer` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_min` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_reach` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_max_slope` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_noclip_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_slow_walk_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_sprint_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_step_height` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_accel` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_breath` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_depth` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_dive_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_exit_depth` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_eye` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_rise` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_spring` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_sprint_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_stamina` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_vault_max` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_vault_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_crouch_max` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_slow` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_sprint_max` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_walk_speed` | player | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `spec_free_collide` | player | LOCKED+RULE | — | yes (+ host rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `spec_free_speed` | player | LOCKED+RULE | — | yes (+ host rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `r_fog_scale` | rendering | LOCKED+RULE | — | yes (+ host rule) | Visibility: changes what you can see relative to other players. Fog density multiplier (0 .. 10, archived!): fixed 1.0; weather_fog / lobby rule set the weather instead. |
| `r_foliage_density` | rendering | LOCKED+RULE | Lobby rules | yes (+ host rule) | Visibility: changes what you can see relative to other players. Foliage density is one value for everyone (consistency.json); a lobby rule may change it for the whole session. Today tiers cap it at 0.1 / 0.25 / 0.5 / 0.8 / 1.0 and eras at 1.0 / 0.7 / 0.4 / 0.2 (V1). |
| `sv_cheats` | system | LOCKED+RULE | — | yes (+ host rule) | Cheats switch: host-owned; defaults to 0 in release builds (registered with 1 today, 'dev build default on'). |
| `vehicle_fuel_scale` | vehicles | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_drag_model` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_enable` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_spin_drift` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind_dir` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `block_fire_when_barrel_blocked` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `fire_tracers_only` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_bob` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_sway` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_muzzle_tolerance` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_recoil_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_spread_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_ads_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `ow_zombies` | world | LOCKED+RULE | — | yes (+ host rule) | Open-world zombies / horde: host / dev. |
| `water_drown_damage` | world | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_clouds` | world | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_fog` | world | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `world_chunk_radius` | world | LOCKED+RULE | — | yes (+ host rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable today). Host rule only. |
| `world_lod_distance` | world | LOCKED+RULE | — | yes (+ host rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable today). Host rule only. |
| `world_seed` | world | LOCKED+RULE | — | yes (+ host rule) | World generation seed: the host's. |
| `horde_enable` | zombies | LOCKED+RULE | — | yes (+ host rule) | Open-world zombies / horde: host / dev. |
| `horde_jobs` | zombies | LOCKED+RULE | — | yes (+ host rule) | Open-world zombies / horde: host / dev. |
| `zombie_speed` | zombies | LOCKED+RULE | Lobby rules | yes (+ host rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
<!-- settings_audit:end -->
