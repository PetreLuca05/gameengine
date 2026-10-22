# Settings audit (fair play, consistency, the simplified Graphics tab)

Every registered cvar has one class. What changes what a player can see, timing or hit
registration is one value for everyone. What only trades performance for fidelity is set by the
Engine Fidelity level. Only personal display / comfort settings are the player's. Section 8 is
generated (`python3 tools/tests/lint_settings.py --write`) from `assets/ui/settings_audit.json`; the
lint fails when a registration has no class flag, disagrees with that file, or the table here is
stale. In the game, `settings_audit [filter] [locked|tier|editable|rule|ui]` prints the same table.

## 1. Classes and enforcement

| Class | Flag | Who may write | In the UI | In config.cfg |
|---|---|---|---|---|
| Player-editable | `CVAR_EDITABLE` | anyone (UI, console, config), bounded by the fidelity level | yes, or console only | yes |
| Tier-driven | `CVAR_TIER` | Engine Fidelity / the era preset only (`CVAR_SRC_FIDELITY`) | not in Settings (console only) | no |
| Hard-coded | `CVAR_LOCKED` | the consistency set at startup (`CVAR_SRC_HOST`); dev builds also the console | no | no |
| Hard-coded, host rule | `CVAR_LOCKED \| CVAR_RULE` | as LOCKED, plus the host's server rules (lobby rules, `sv_rule`), replicated with `MSG_CVARS`; never a client | Lobby only | the host's own value |

- **Release locks** (`src/core/cvar.c`): CMake option `DG_DEV_BUILD` (ON by default; the shipped
  build turns it off) or `--release-locks` at run time. While on, a user-source write of a TIER or
  LOCKED cvar is rejected and logged (`<cvar> is fixed for fair play ... rejected`), counted in
  `settings_audit`. Privileged writers bracket themselves with `cvar_source_push`: Engine Fidelity
  and the era preset (`CVAR_SRC_FIDELITY`), the consistency set, `MSG_CVARS` from the host, config
  migrations and dev tools (`CVAR_SRC_HOST`). A LOCKED | RULE cvar takes user writes only where
  this machine is not a client (`cvar_set_rule_gate`, `net.c`).
- **Config**: `cvar_saved` writes archived EDITABLE cvars and the host's archived rules only. An
  older `config.cfg` with TIER / LOCKED entries loads through `config_load_file`: those entries are
  dropped with one log line and the file is rewritten without them. `r_fidelity_custom` keeps only
  the player's own fidelity features under the release locks.
- **Consistency set** (`assets/render/consistency.json`, `src/core/consistency.c`): 63 LOCKED
  values, each with a reason. Applied at startup (each value also becomes the cvar's default, so a
  reset returns to it), checked (every entry must be a registered CVAR_LOCKED cvar; a bad entry is
  logged as a broken build), and hashed (FNV-1a over the sorted `name=value;` pairs). The hash is
  XORed into the lobby build hash (other versions are listed as incompatible) and sent in
  `MSG_HELLO` after the animation manifest: the host rejects a different set ("consistency set
  mismatch"). `DG_PROTOCOL_VERSION` 25. `consistency_check` lists values that differ now (a host
  rule, a dev-build change).
- **Server rules**: the lobby's "Foliage density (all players)" (50 .. 100 %, host only,
  `LobbySettings.foliage_pct`) is applied by the host at game start (`lobby_apply_rules`) and
  replicated to every client with the other host cvars. `sv_rule [cvar value]` lists / sets any
  rule from the console on the host.

## 2. Where each Graphics row went

| Row before | Cvar(s) | Now |
|---|---|---|
| Default (reset) | the player's graphics cvars | header, next to "Why can't I change X?"; resets EDITABLE cvars only |
| Resolution / Window mode / VSync / Apply | r_width, r_height, r_mode, r_vsync | Display |
| Frame rate limit | r_maxfps | Display |
| Stretched resolution | r_stretch | Display (back at the user's request, 2026-10-19) |
| Field of view | cl_fov | Comfort "Field of View", 80 .. 150 horizontal deg at 16:9, Hor+ (the MWII model, 2026-10-19), with "ADS Field of View" (`cl_ads_fov`) and "Weapon Field of View" (`cl_weapon_fov`): docs/CAMERA_HOLDPOINT.md "Field of view" |
| Hands FOV follows camera / Hands FOV | cam_weapon_fov, cl_hands_fov | removed: Weapon Field of View presets (Narrow / Default / Wide) instead; `cl_hands_fov` is a locked dev override |
| Sprint FOV effect | cl_sprint_fov | removed (2026-10-19, the FOV model has three settings): a locked multiplier |
| Red dot brightness | sight_brightness | HUD > Crosshair, 0.25 .. 2 |
| Overall graphics quality (+ benchmark) | r_fidelity | replaced (2026-10-19) by Quality: Overall + seven groups (`r_quality_*`), no benchmark (docs/ENGINE_FIDELITY.md "Player settings") |
| Render scale / Upscaling / resolution | r_render_scale, r_upscaler, r_upscale_ratio | Display (bounded by the level) |
| Anti-aliasing | r_aa | removed, tier |
| Sharpening | r_sharpen | Comfort |
| Post-processing / Shadows / Textures / Effects / Foliage / View distance | fid groups | removed (tier parts follow the level; foliage density and ranges are locked) |
| Motion blur (+ strength) | r_motion_blur, r_motion_blur_strength | Comfort "Motion Blur": Off / World / Hands and world (2026-10-20, the only control of it; out of the Post-processing quality); the strength stays locked |
| Brightness | r_brightness | Display, 0.85 .. 1.15 (was 0.3 .. 3) |
| Gamma | r_gamma | removed, locked 1 |
| HDR10 / paper white / peak | r_hdr_output, r_hdr_paper_white, r_hdr_max_nits | Display (paper white 100 .. 300); peak console only |
| Colour-blind filter | r_colorblind | Display |
| Screen effects | r_post_feedback | Comfort, 0.5 .. 1 |
| Gore | gore_level | Comfort (was Character > Content) |
| Show FPS (Character tab) | cl_showfps | Display |
| Head bob (Controls tab) | cl_headbob, cl_headbob_amount | removed, locked (designed value) |
| Weapon kick / sway (Crosshair tab), Camera shake | wpn_visual_scale, cl_weapon_sway, cam_kick_scale | removed, locked (designed values) |
| UI scale / Safe area (Character tab) | ui_scale, ui_safe_area | HUD > Interface |
| Follow camera kick (Crosshair tab) | crosshair_follow_kick | removed (the cvar is gone: the crosshair is always on the true aim point, docs/AIM.md) |
| Health bars (Character tab) | cl_healthbars | removed, lobby rule (off) |
| Free camera collision (Character tab) | spec_free_collide | removed, host rule |

The Era and Look pickers were already removed from Settings on main (commit 2cd1d95: every session
starts on Realism); `r_era` and `r_look` are locked (modern / realism) here. There is no Graphics >
Debug page any more: the tier features (the parts a level sets) are the engine's, console only.

## 3. The ambiguous calls

- **AO, contact shadows, SSR, SSGI**: tier. SSR reflects only pixels already on screen.
- **Shadows**: Hard vs Soft, resolution, cascades and filter are tiers, but no level disables the
  sun shadows (Off had Disable: enemy shadows and dark interiors gone). The distance is locked.
- **Lamp / cloud / moon shadows**: locked on. Without lamp shadows a lamp lights a player behind a
  wall. Only the lamp tile size stays a tier.
- **Lens flare / dirt / distortion, bloom strength, tonemapper**: the Look's, locked. Glare and
  distortion change what you see near lights. (Bloom on / off itself follows the level.)
- **Lens droplets**: locked on, at every level including Off (they cleared the view in rain).
- **Rain / snow**: the streak count around the camera is the same at every tier and never 0; the
  tier only buys splash detail.
- **Gore**: editable (content / age rating), moved to Character. Gore Off keeps neutral impact
  puffs. Corpse bleeding (`gore_bleed`) is locked.
- **Head bob, weapon kick, weapon sway, camera shake** are locked at the designed values (removed
  from Settings at the user's request, 2026-10-19). The fire punches are the same at every level
  (they were off at Low). The fire-animation tuning cvars are locked (FIRE_ANIM.md: one global sway).
- **Motion blur**: the player's Comfort choice, Off / World (the view + moving things, the hands
  and gun sharp) / Hands and world (the hands and gun blur along their own motion too); the fire
  animation's viewmodel blur spike is the same for everyone.
- **Screen effects**: editable for photosensitivity, floored at 0.5 (at 0 a flashbang did nothing).
- **Brightness / gamma**: one narrow brightness band 0.85 .. 1.15; gamma locked 1.
- **HDR paper white** 100 .. 300 nits; peak nits is the monitor's (console only).
- **Crosshair**: style, colour, size, opacity, outline, hit markers and target tint are the
  player's (turning feedback off only hides your own information); where it sits (the true aim
  point) is behaviour.
- **Health bars, damage numbers**: game state, locked off; the host may turn them on (rules).
- **Stretched resolution**: the player's again (Graphics > Display), at the user's request; it
  widens targets on screen for whoever uses it.
- **Occlusion culling**: locked two-pass (mode 1 drew a player leaving cover one frame late).
- **Spectator free camera collision / speed**: host rules.
- **Character animation features** (lean, foot IK, ragdolls ...): locked; every client sees remote
  bodies the same way.
- **Phone camera exposure / filters**: editable; the in-game phone works the same for everyone.

## 4. Locked values

`assets/render/consistency.json` lists each value with its reason. The main ones: foliage density
1.0 (lobby rule 0.5 .. 1), impostor switch 32 px (trees and small cover), classic LOD pick 2 px,
character LOD bias 1, sun shadow distance 150 m, lamp / cloud /
moon shadows on (10 shadowed lamps), fog scale 1, aerial perspective on, auto exposure on with
0 EV compensation, gamma 1, the Look's tonemapper / bloom / no flare, dirt or distortion, droplets
on, no stretch, 256 impact decals, the full particle pool, tracers and bullet traces on, draw
distance 4 chunks / 70 m (rules), hands FOV follows the camera, the crosshair on the true aim point,
100 ms interpolation, the death sequence, health bars / damage numbers off (rules),
remote lean on, 45 s / 24 ragdolls, door peek 100 deg, terrain / surface shading on, no wireframe /
hitboxes / collision / third person.

## 5. Tier violations found and fixed

| # | Where | Violation | Fix |
|---|---|---|---|
| V1 | fidelity `foliage_density` caps 0.1 / 0.25 / 0.5 / 0.8 / 1.0; eras 1.0 / 0.7 / 0.4 / 0.2 | Low drew a tenth of the grass | feature removed from the tables and the era preset; locked 1.0 (rule 0.5 .. 1) |
| V2 | `impostor_px` 96 .. 16 by level; classic 64 vs vgeo 16 | trees turned into cards at different ranges | removed; 32 px (the classic renderer, the only one since the nanite removal) |
| V3 | `vgeo_error_px` x4 .. x1, `vgeo_lod_px` x8 .. x1, eras 2 .. 12 px | silhouettes at range differed | removed; `r_vgeo_lod_px` locked 2 px (the tiers keep the shadow-view error; the vgeo renderer and its error went with the nanite removal) |
| V4 | `lod_bias` 0.25 .. 1; `r_char_quality` scaled the character LOD switch points | distant characters dropped detail earlier on Low | both removed from the tables; locked 1 / 2 (x1) |
| V5 | `shadow_distance` 50 .. 300 (+ 2010-era Off 70); Off `shadows` 0; `cloud_shadows`, `light_shadows`, `moon_shadows` off / missing below Medium / High | shadows reached different distances, shadow types missing | distance locked 150 m (`SHADOW_LOCKED_DISTANCE` = consistency.json, tested); every level at least Hard; Off / Low use 1024 px maps and the two-cascade split is 25 % so cascade 0 keeps its texel density (`test_shadow_csm`); lamp / cloud / moon shadows locked on |
| V6 | froxel fog `max(fog * 1.5, ...)` + the weather's fog floor vs the analytic distance fog `env.fog_density` (Off, Low, no volumetrics, 2010 era) | fog was thinner without volumetrics | `render_fog_extinction()`: the analytic lit-shader fog, the sky fog and the scope use the froxels' density |
| V7 | `water` tier 0 has no refraction | checked: tier 0 still shows the scene under the surface through the same turbidity, only without the refraction offset | no change needed |
| V8 | `precip_quality` 0 at Off / Low, streak counts 4000 .. 16000 by tier | rain as cover disappeared / thinned on low levels | 9000 streaks x intensity at every tier, quality 0 removed; the tier only changes splashes |
| V9 | `vfx_quality` emission multiplier 0.35 .. 1.35; `decal_max` 16 .. 4096 | thinner smoke / dust / blood, fewer impact decals | emission x1 at every tier (the tier buys sorting, collisions, lighting); decals locked 256 |
| V10 | Off: fixed exposure, `auto_exposure` off below | night darkness differed | Off already meters the light with the shared day / night EV clamps (`post_core.c direct_exposure`); auto exposure and compensation locked |
| V11 | `lens_drops` missing below High, no post at Off | droplets disappeared | locked on; the droplet passes run in Off's direct path too (scene passes, not post) |
| V12 | `lod.json` tiers 0.3 .. 1.0 and eras 0.7 .. 1.0 multiplied every LOD, foliage-fade, impostor, terrain, shadow and VFX range | everything popped / faded nearer on low levels | tiers and eras must be 1 (`lod_table_parse` rejects others, `test_lod`) |
| V13 | grass assets: per-level fade start / end, shadow distance and density (Off: 8 .. 13 m at 35 %) | grass ended 40 m nearer on Off | every level's entry = the Modern one (`lint_settings.py` checks) |
| V14 | LOD / impostor / character LOD pixel sizes measured at the render or display height | render scale (0.6 at Off) or a 4K screen moved every switch | `RENDER_LOD_REF_H` 1080: the classic projections and the character LOD use the 1080p-equivalent size |

`tools/tests/run_settings_tests.py` runs the game at every level in the dev room foliage yard and
compares `visibility_probe` (the classic main view's visible / fading / impostor instance parts, fog extinction, shadow
distance, LOD / impostor / grass ranges, droplets, decals, draw distance) within tolerance. Cost of
the fixes: Off and Low now draw full-density grass to 55 m, lamp shadows and 150 m shadows; Low
renders the same world cheaper, never smaller.

## 6. The tabs

```
Settings:  Graphics | Controls | HUD | Audio | Phone      (Crosshair and Character merged into HUD)

Graphics                                         [ (i) Why can't I change X? ]   [ Default ]
  QUALITY   Overall (Low / Medium / High / Ultra, Custom) · Anti-aliasing · Post-processing · Shadows ·
            Textures · Effects · Foliage · View distance (each Low / Medium / High / Ultra)
  DISPLAY   Resolution · Window mode · VSync · [Revert] [Apply] · Frame rate limit · Render scale ·
            Stretched resolution · Upscaling: Off / TAA-U / FSR 1 / FSR 2 / DLSS (+ resolution) ·
            HDR10 output (+ paper white) · Brightness · Colour-blind filter · Show FPS
  COMFORT   Field of View (or "set by the server") · ADS Field of View · Weapon Field of View · Screen effects · Gore

HUD        INTERFACE (UI scale, safe area) · CROSSHAIR (style, colour, length, thickness, extra gap, opacity,
           outline, target tint, hit markers, red dot brightness) · LAYOUT · COMPASS (+ compass arc
           90 .. 180 deg) · KILLS · PINGS · WHEELS
(no Graphics > Debug page; the skin is picked in the main menu; spectating times are console only)
```

Tooltip of "Why can't I change X?": settings that change what you can see compared with other
players (foliage and grass, fog and weather, view, shadow and detail distances, lens effects,
exposure and brightness beyond a small range, the hands FOV) are the same for everyone, for fair
play; Engine Fidelity changes how good the world looks and how fast it runs, never what you can
see; the host's lobby rules may set some of them for a whole game.

## 7. Decisions

- FOV band 60 .. 120 deg vertical (the user kept the top of the old range).
- Locked foliage density 1.0, shadow distance 150 m, impostor switch 32 px.
- Off keeps its direct output (no post stack) but runs the droplets and keeps its metered exposure
  with the shared EV clamps.
- `DG_DEV_BUILD` defaults ON: dev builds and every test keep writable cvars; `test_settings` and
  `--release-locks` exercise the release behaviour.

## 8. Full classification

501 cvars: 127 EDITABLE, 53 TIER, 236 LOCKED, 85 LOCKED + host rule. Generated by `tools/tests/lint_settings.py --write` from `assets/ui/settings_audit.json` (the class must match each registration's flag; `settings_audit` prints the same table in the game). **UI after the rebuild** "—" means the cvar is in no settings tab.

| Cvar | Category | Class | UI after the rebuild | Locked in release | Reason |
|---|---|---|---|---|---|
| `snd_master` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_music` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_sfx` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_ui` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_voice` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_weather` | audio | EDITABLE | Audio | no | Volumes. |
| `voice_chat` | audio | EDITABLE | Audio | no | A player's choice: voice chat on / off. |
| `voice_mic_gain` | audio | EDITABLE | Audio | no | The player's microphone level. |
| `cl_fov` | camera | EDITABLE | Graphics > Comfort | no | Field of View (the MWII model, docs/CAMERA_HOLDPOINT.md "Field of view"): 80 .. 150, horizontal deg at 16:9, Hor+ (the vertical FOV is the same on every aspect). Personal comfort; sv_fov / sv_fov_max still override. |
| `hud_compass` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_compass_arc` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_coords` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_killfeed` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_opacity` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_phone_corner` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_phone_size` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_ping_compass` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_ping_markers` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_scale` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `hud_timed_ring` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `mm_globe_detail` | hud | EDITABLE | — | no | Maps tab globe look (docs/MENU_GLOBE.md): idle turning, texture opacity, line weight, flight time, water / admin detail. Menu only, no gameplay effect. |
| `mm_globe_lines` | hud | EDITABLE | — | no | Maps tab globe look (docs/MENU_GLOBE.md): idle turning, texture opacity, line weight, flight time, water / admin detail. Menu only, no gameplay effect. |
| `mm_globe_rotate` | hud | EDITABLE | — | no | Maps tab globe look (docs/MENU_GLOBE.md): idle turning, texture opacity, line weight, flight time, water / admin detail. Menu only, no gameplay effect. |
| `mm_globe_texture` | hud | EDITABLE | — | no | Maps tab globe look (docs/MENU_GLOBE.md): idle turning, texture opacity, line weight, flight time, water / admin detail. Menu only, no gameplay effect. |
| `mm_globe_travel` | hud | EDITABLE | — | no | Maps tab globe look (docs/MENU_GLOBE.md): idle turning, texture opacity, line weight, flight time, water / admin detail. Menu only, no gameplay effect. |
| `ping_chat` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ping_enable` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ping_sound` | hud | EDITABLE | HUD | no | Pings: personal filters of teammates' pings. |
| `ui_safe_area` | hud | EDITABLE | HUD > Interface | no | UI scale / safe area: display. |
| `ui_scale` | hud | EDITABLE | HUD > Interface | no | UI scale / safe area: display. |
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
| `cl_name` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `lobby_privacy` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `net_autohost` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `net_port` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `net_showstats` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `sv_hostname` | network | EDITABLE | — | no | Identity / network preferences / console. |
| `phone_auto_pair` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_battery_drain` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_bind` | phone | EDITABLE | Phone (dev) | no | Phone companion transport (PHONE.md): personal device setup. |
| `phone_device_days` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_exposure` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_filter` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_fps` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_pitch` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_roll` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
| `phone_invert_yaw` | phone | EDITABLE | Phone | no | Phone companion: personal device / stream preferences; the in-game item behaves the same for everyone. |
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
| `cl_ads_fov` | player | EDITABLE | Graphics > Comfort | no | ADS Field of View: Independent (aiming goes back to the default 80, then the sight's zoom) or Affected (scales with Field of View). Personal comfort within the Field of View band. |
| `cl_fp_body` | player | EDITABLE | — | no | Own body in first person: only your view of yourself. |
| `cl_showfps` | player | EDITABLE | Graphics > Display | no | FPS overlay toggle. |
| `cl_skin` | player | EDITABLE | Main menu (profile / cosmetics) | no | Skin: cosmetic, seen by everyone (same set for all). |
| `cl_weapon_fov` | player | EDITABLE | Graphics > Comfort | no | Weapon Field of View Narrow / Default / Wide: only the arms layer's projection (your arms + the held item), never the world; aimed, the sight picture is the same for everyone (cosmetic). |
| `developer` | player | EDITABLE | — | no | Identity / network preferences / console. |
| `g_map` | player | EDITABLE | — | no | Identity / network preferences / console. |
| `spec_blend_time` | player | EDITABLE | — | no | Death cam time / blend: personal pacing of your own spectator camera. |
| `spec_death_cam_time` | player | EDITABLE | — | no | Death cam time / blend: personal pacing of your own spectator camera. |
| `spec_show_delay` | player | EDITABLE | — | no | Spectator delay readout. |
| `r_brightness` | rendering | EDITABLE | Graphics > Display | no | Brightness: display calibration, narrowed to 0.85 .. 1.15 (was 0.3 .. 3.0, which lifted the night darkness floor). |
| `r_colorblind` | rendering | EDITABLE | Graphics > Display | no | Colour-blind correction: accessibility; remaps hues, not luminance, at every level (Off included). |
| `r_hdr_max_nits` | rendering | EDITABLE | — | no | HDR display peak: a property of the monitor (tone-map shoulder), not of the scene; console only. |
| `r_hdr_output` | rendering | EDITABLE | Graphics > Display | no | HDR10 output: a display capability, purely tonal. |
| `r_hdr_paper_white` | rendering | EDITABLE | Graphics > Display | no | HDR paper white: the HDR equivalent of brightness, bounded 100 .. 300 nits so it cannot lift night scenes past the SDR brightness band. |
| `r_height` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_maxfps` | rendering | EDITABLE | Graphics > Display | no | Frame cap: personal; no effect on what is drawn. |
| `r_mode` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_motion_blur` | rendering | EDITABLE | Graphics > Comfort | no | Motion Blur Off / World / Hands and world: the player's own comfort choice (motion sickness), the only thing setting it (no quality group, 2026-10-20); it shows nothing others can't see. |
| `r_perf_overlay` | rendering | EDITABLE | — | no | Performance overlay (frame times + budget): diagnostics only, shows nothing about the world. |
| `r_post_feedback` | rendering | EDITABLE | Graphics > Comfort | no | Screen effects (damage / flash / low health): photosensitivity accessibility, 0.5 .. 1 (at 0 a flashbang did nothing). |
| `r_quality_aa` | rendering | EDITABLE | Graphics > Quality | no | Anti-aliasing quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_effects` | rendering | EDITABLE | Graphics > Quality | no | Effects quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_foliage` | rendering | EDITABLE | Graphics > Quality | no | Foliage quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_post` | rendering | EDITABLE | Graphics > Quality | no | Post-processing quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_shadows` | rendering | EDITABLE | Graphics > Quality | no | Shadows quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_textures` | rendering | EDITABLE | Graphics > Quality | no | Textures quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_quality_view_distance` | rendering | EDITABLE | Graphics > Quality | no | View distance quality Low / Medium / High / Ultra: the only thing that sets its features (Overall sets all seven). |
| `r_render_scale` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level. Fewer pixels look softer but never remove objects (LOD / culling / impostor switches are locked). |
| `r_stretch` | rendering | EDITABLE | Graphics > Display | no | Stretched resolution simulator 0 .. 1 (the 4:3 stretched look), back in Settings at the user's request (2026-10-19). It widens targets on screen for whoever uses it. |
| `r_upscale_ratio` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level. Fewer pixels look softer but never remove objects (LOD / culling / impostor switches are locked). |
| `r_upscaler` | rendering | EDITABLE | Graphics > Display | no | Resolution of the 3D view, bounded by the level. Fewer pixels look softer but never remove objects (LOD / culling / impostor switches are locked). |
| `r_vsync` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `r_width` | rendering | EDITABLE | Graphics > Display | no | Display mode: personal hardware choice, applied with Apply / confirm. |
| `story_autosave` | story | EDITABLE | Console | no | Story mode: save to slot 'auto' at every checkpoint (Continue); a player preference. |
| `story_save_dir` | story | EDITABLE | Console | no | Story saves folder (empty = the user's pref dir); tests point it at a scratch folder. |
| `story_vista_life` | story | EDITABLE | Console | no | Story vista life (helicopters, smoke, traffic, birds) on or off; cosmetic, this machine only (the story does not depend on it). |
| `cfg_migration` | system | EDITABLE | — | no | Config migration counter: bookkeeping the game writes (saved, so migrations run once). |
| `con_enable` | system | EDITABLE | — | no | Identity / network preferences / console. |
| `con_favorites` | system | EDITABLE | — | no | Identity / network preferences / console. |
| `con_recent` | system | EDITABLE | — | no | Identity / network preferences / console. |
| `gore_level` | vfx | EDITABLE | Graphics > Comfort | no | Gore (moved from Graphics): a content / age-rating choice. Gore Off keeps neutral impact puffs, so hits stay readable. |
| `ads_sens_coef` | weapons | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `ads_sens_mode` | weapons | EDITABLE | Controls | no | Input: sensitivities, inversion, ADS scaling, deadzone, toggles. |
| `cl_viewmodel_sway_scale` | weapons | EDITABLE | Controls | no | Weapon motion: accessibility scale of the cosmetic viewmodel sway (0 = still). |
| `crosshair_color` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_gap` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_hitmarker` | weapons | EDITABLE | HUD > Crosshair | no | Hit markers / target tint: turning them off only removes information from yourself; their timing / accuracy is fixed in code. |
| `crosshair_length` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_opacity` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_outline` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_style` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `crosshair_target_color` | weapons | EDITABLE | HUD > Crosshair | no | Hit markers / target tint: turning them off only removes information from yourself; their timing / accuracy is fixed in code. |
| `crosshair_thickness` | weapons | EDITABLE | HUD > Crosshair | no | Crosshair style / colour / size / opacity / outline: style only (the gap stays the exact spread plus this). |
| `sight_brightness` | weapons | EDITABLE | HUD > Crosshair | no | Red dot brightness (moved from Graphics): 0.25 .. 2 (was 0 .. 4). |
| `cl_clock` | world | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass (+ its arc), coordinates, kill feed, pings, phone panel, timed ring, clock. |
| `anim_eval_budget` | animation | TIER | — | yes (fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
| `r_aa` | rendering | TIER | — | yes (fidelity writes) | Anti-aliasing mode: a quality / cost trade at the same visibility; picked by the level. |
| `r_aniso` | rendering | TIER | — | yes (fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_ao` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI / puddle reflections: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_bloom` | rendering | TIER | — | yes (fidelity writes) | Bloom on / off follows the level; its strength is the Look's (r_bloom_intensity, locked). |
| `r_bloom_quality` | rendering | TIER | — | yes (fidelity writes) | DoF / bloom gather quality. |
| `r_char_maps` | rendering | TIER | — | yes (fidelity writes) | Character skin PBR maps: shading quality. |
| `r_chromatic` | rendering | TIER | — | yes (fidelity writes) | Set by the player's quality settings only (Anti-aliasing: sharpening; Post-processing: the rest), docs/ENGINE_FIDELITY.md "Player settings". |
| `r_contact_shadows` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI / puddle reflections: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_dof` | rendering | TIER | — | yes (fidelity writes) | Set by the player's quality settings only (Anti-aliasing: sharpening; Post-processing: the rest), docs/ENGINE_FIDELITY.md "Player settings". |
| `r_dof_quality` | rendering | TIER | — | yes (fidelity writes) | DoF / bloom gather quality. |
| `r_film_grain` | rendering | TIER | — | yes (fidelity writes) | Set by the player's quality settings only (Anti-aliasing: sharpening; Post-processing: the rest), docs/ENGINE_FIDELITY.md "Player settings". |
| `r_foliage_wind` | rendering | TIER | — | yes (fidelity writes) | Foliage shading tier: wind layers. Density and fade distances are locked. |
| `r_grass_sss` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_grass_tex` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_light_shadow_quality` | rendering | TIER | — | yes (fidelity writes) | Lamp / flashlight shadow tile size (256 / 512 px). Whether lamps are shadowed is locked. |
| `r_precip_quality` | rendering | TIER | — | yes (fidelity writes) | Rain / snow shading tier (splash detail). The particle count is the same at every tier and never 0 (V8). |
| `r_puddle_reflections` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI / puddle reflections: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_scope_pip_scale` | rendering | TIER | — | yes (fidelity writes) | Scope picture resolution: like render scale for the scope (sharpness, not content). |
| `r_shadow_cascade_border` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_cascades` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_caster_limit` | rendering | TIER | — | yes (fidelity writes) | Grass / ground cover and small plants cast sun shadows only into the near cascade(s); trees, characters, buildings and the plants themselves are unchanged. The reach is about the same at every level (ground cover 10 .. 15 m, small plants 30 m; tests/test_shadow_casters.c), and a grass shadow farther away never hides or reveals anyone: cost only (PF-0013). |
| `r_shadow_far_interval` | rendering | TIER | — | yes (fidelity writes) | The two far sun cascades are re-rendered in turn (each every N frames) and keep the tile they were drawn with in between; forced after a cut or once the fit moved. Far shadows of moving casters lag up to N - 1 frames; nothing is hidden or revealed that the near cascades show: cost only (PF-0028). |
| `r_shadow_min_radius` | rendering | TIER | — | yes (fidelity writes) | Small props (< radius) skip the sun shadow: cost only. |
| `r_shadow_near_offset` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_projection` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_resolution` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_split2` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_split4_1` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_split4_2` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadow_split4_3` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadowmask_distance` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadowmask_mode` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_shadows` | rendering | TIER | — | yes (fidelity writes) | Shadow mode: Hard vs Soft is a quality tier, but every level has at least Hard. Disable removed every sun shadow (enemy shadows, dark interiors), an advantage (V5). |
| `r_sharpen` | rendering | TIER | — | yes (fidelity writes) | Set by the player's quality settings only (Anti-aliasing: sharpening; Post-processing: the rest), docs/ENGINE_FIDELITY.md "Player settings". |
| `r_soft_shadow_quality` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and the shadow types are locked. |
| `r_ssgi` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI / puddle reflections: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_ssr` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI / puddle reflections: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_terrain_quality` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_terrain_tex_size` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_texture_quality` | rendering | TIER | — | yes (fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_vgeo_shadow_error_px` | rendering | TIER | — | yes (fidelity writes) | Classic renderer shadow cost (shadow-view geometry error, scene content in the shadow maps): shading cost only, the locked LOD / impostor outcome is the same. |
| `r_vgeo_shadows` | rendering | TIER | — | yes (fidelity writes) | Classic renderer shadow cost (shadow-view geometry error, scene content in the shadow maps): shading cost only, the locked LOD / impostor outcome is the same. |
| `r_vignette` | rendering | TIER | — | yes (fidelity writes) | Set by the player's quality settings only (Anti-aliasing: sharpening; Post-processing: the rest), docs/ENGINE_FIDELITY.md "Player settings". |
| `r_vm_shadow` | rendering | TIER | — | yes (fidelity writes) | Viewmodel shadow tile: only your own arms. |
| `r_vol_local_lights` | rendering | TIER | — | yes (fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog density is locked (r_fog_scale) and the distance-fog fallback uses the same density (V6). |
| `r_volumetrics` | rendering | TIER | — | yes (fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog density is locked (r_fog_scale) and the distance-fog fallback uses the same density (V6). |
| `r_water` | rendering | TIER | — | yes (fidelity writes) | Water shading tier (reflections, ripples, caustics, waves). Refraction / turbidity are the same at every tier (V7). |
| `vfx_half_res` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget, collisions. Emission counts are the same at every tier (V9). |
| `vfx_lighting` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget, collisions. Emission counts are the same at every tier (V9). |
| `vfx_quality` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget, collisions. Emission counts are the same at every tier (V9). |
| `ads_dof` | weapons | TIER | — | yes (fidelity writes) | ADS weapon blur: availability by level; r_dof 0 turns it off. |
| `world_chunk_uploads` | world | TIER | — | yes (fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
| `anim_corpse_collision` | animation | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `anim_damage` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `anim_foot_ik` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_forearm_twist` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_fp_parity` | animation | LOCKED | — | yes | Dev check of first-person rule 1 (the drawn body is the pose everyone computes): logging only. |
| `anim_hold` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_hold_assert` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `anim_hold_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `anim_ik_shoulders` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_lean` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_local_resim` | animation | LOCKED | — | yes | Spectator extrapolation / pop warning / anim resync: netcode tuning. |
| `anim_max_ragdolls` | animation | LOCKED | — | yes | Corpse lifetime / ragdoll cap: corpses are cover and a DNA source (REVIVE.md). |
| `anim_net_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `anim_ragdoll` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_ragdoll_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `anim_ragdoll_time` | animation | LOCKED | — | yes | Corpse lifetime / ragdoll cap: corpses are cover and a DNA source (REVIVE.md). |
| `anim_resync_ticks` | animation | LOCKED | — | yes | Spectator extrapolation / pop warning / anim resync: netcode tuning. |
| `anim_sample_rate` | animation | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `anim_spine` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `anim_turn` | animation | LOCKED | — | yes | Character animation features (foot IK, spine, turn, hold, lean, damage reactions, ragdolls): remote players look the same to everyone; lean off would hide a peeking head. |
| `char_inspect` | animation | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hitbox_debug` | animation | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `audio_debug` | audio | LOCKED | — | yes | Audio debug overlay: dev view. |
| `snd_degrade` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_echo_max` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_echoes` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_env` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_helmet` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_occlusion_voices` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_offline` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_procedural` | audio | LOCKED | — | yes | Procedural audio switches (environment chain, echoes, occlusion budget, degradation, helmet stub, offline mixer, recipes): dev / A-B toggles, one value for everyone. |
| `snd_trace` | audio | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `voice_loopback` | audio | LOCKED | — | yes | Dev test tool. |
| `voice_loopback_dist` | audio | LOCKED | — | yes | Dev test tool. |
| `voice_radio_dist` | audio | LOCKED | — | yes | Game design: where voices switch to the radio is the same for everyone. |
| `voice_radio_fade` | audio | LOCKED | — | yes | Game design: the proximity / radio crossfade. |
| `voice_test_tone` | audio | LOCKED | — | yes | Dev test tool. |
| `arms_layer` | camera | LOCKED | — | yes | The first-person arms layer (own camera / FOV, ADS presented there): one presentation for all; 0 is a dev comparison. |
| `arms_layer_debug` | camera | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cam_anchor_radius` | camera | LOCKED | — | yes | First-person camera check: how far the nominal eye may stray from the animated head eye before it is logged (never corrects anything). |
| `cam_anim_influence` | camera | LOCKED | — | yes | Dev comparison: the camera on the raw head bone moves with every animation (motion sickness, peeking differences). 0 for everyone. |
| `cam_collision_radius` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): aim stability and what peeks over cover. One rig for all. |
| `cam_death_follow` | camera | LOCKED | — | yes | Death / downed sequence and its camera. |
| `cam_death_spectate` | camera | LOCKED | — | yes | Death / downed sequence and its camera. |
| `cam_debug` | camera | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cam_glide_damping` | camera | LOCKED | — | yes | First-person camera rig (the gliding nominal eye, docs/CAMERA_HOLDPOINT.md): where the eye sits decides what peeks over cover. One rig for all. |
| `cam_glide_stiffness` | camera | LOCKED | — | yes | First-person camera rig (the gliding nominal eye, docs/CAMERA_HOLDPOINT.md): where the eye sits decides what peeks over cover. One rig for all. |
| `cam_kick_scale` | camera | LOCKED | — | yes | Camera shake (the visual camera kick on shots and the fire punches): Removed from Settings (the user's call, 2026-10-19): fixed at the designed value. Aim recoil is separate (wpn_recoil_scale, host). |
| `cam_recon_smooth_time` | camera | LOCKED | — | yes | First-person camera rig tuning (head follow / spring / clamp / socket bob): aim stability and what peeks over cover. One rig for all. |
| `cam_third` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `cl_ads_world_zoom` | camera | LOCKED | — | yes | ADS FOV per sight: world zoom and eye relief come from the sight data. |
| `cl_camera_socket` | camera | LOCKED | — | yes | First-person camera rig (the gliding nominal eye, docs/CAMERA_HOLDPOINT.md): where the eye sits decides what peeks over cover. One rig for all. |
| `cl_hands_fov` | camera | LOCKED | — | yes | Dev override of the arms layer's hip FOV (tuning): players pick Weapon Field of View (the FOV model has exactly three settings). |
| `cl_move_shake` | camera | LOCKED | — | yes | Movement shake (head bob, landing dip, ledge tilt): removed from Settings with head bob / camera shake (the user's call, 2026-10-19): off, the camera glides. |
| `cl_sprint_fov` | camera | LOCKED | — | yes | Sprint widening: a multiplier on the current FOV, fixed at the designed value (the FOV model has exactly three settings). |
| `cl_thirdperson` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `cl_thirdperson_dist` | camera | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `cl_viewmodel_land_camera` | camera | LOCKED | — | yes | The viewmodel sway's camera dip on hard landings: off by default, scaled by the movement shake (itself off); dev switch only. |
| `handpose_autoreg` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `handpose_editor_dir` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `handpose_editor_window` | camera | LOCKED | — | yes | Hand pose editor in its own window: dev tools. |
| `handpose_watch` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hands_debug` | camera | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `hands_forearm` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `hands_upper_arm` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `holdpoint_force_state` | camera | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `holdpoint_static` | camera | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `lean_distance` | hud | LOCKED | — | yes | Window lean: how far the head goes out past the window; what you can see from it is game state. |
| `lean_drop` | hud | LOCKED | — | yes | Window lean: how far the head drops while leaning; what you can see from it is game state. |
| `lean_pitch_down` | hud | LOCKED | — | yes | Window lean: how far down the view may look; what you can see from it is game state. |
| `lean_yaw_limit` | hud | LOCKED | — | yes | Window lean: the look left / right limit; what you can see from it is game state. |
| `peek_distortion` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_fov` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_pitch_limit` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `peek_yaw_limit` | hud | LOCKED | — | yes | Door peek FOV / fisheye: how much you see through the visor is game state. |
| `ui_debug` | hud | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cl_interp` | network | LOCKED | — | yes | Networking: entity interpolation delay (archived and client-set before). |
| `cl_predict_hit_reactions` | network | LOCKED | — | yes | Hit feedback prediction (own impacts / reactions shown before the host confirms): game-state timing; on for all. |
| `cl_predict_impacts` | network | LOCKED | — | yes | Hit feedback prediction (own impacts / reactions shown before the host confirms): game-state timing; on for all. |
| `host_timescale` | network | LOCKED | — | yes | Time scale: cheat. |
| `net_fakejitter` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `net_fakelag` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `net_fakeloss` | network | LOCKED | — | yes | Network condition simulator: dev only. |
| `sv_inv_reach` | network | LOCKED | — | yes | Inventory reach to an open world container: a server rule value for tests (dev builds), not a player choice. |
| `cl_fp_debug` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cl_fp_layers` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cl_interact_range` | player | LOCKED | — | yes | Interaction ray length: gameplay. |
| `cl_parts` | player | LOCKED | — | yes | Hand pose editor / hands rig tuning: dev tools. |
| `dev_fixed_fps` | player | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `pl_debug_mantle` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `pl_noclip_speed` | player | LOCKED | — | yes | Noclip speed: dev. |
| `spec_dump` | player | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `spec_extrap_ticks` | player | LOCKED | — | yes | Spectator extrapolation / pop warning / anim resync: netcode tuning. |
| `spec_pop_warn` | player | LOCKED | — | yes | Spectator extrapolation / pop warning / anim resync: netcode tuning. |
| `perf_fix_disable` | rendering | LOCKED | — | yes | Perf audit A/B switch for the fidelity-neutral fixes (perf_verify): dev tool. |
| `r_ao_blur` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_blur_sharpness` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_falloff` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_foliage_radius` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_full_res` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_intensity` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_radius` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_slices` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_steps` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_temporal` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_ao_thickness` | rendering | LOCKED | — | yes | GTAO tuning (radius, falloff, foliage clamp, sampling, blur, temporal): engine values, not a player setting; the quality is r_ao (tier). |
| `r_arms_wireframe` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_async_compute` | rendering | LOCKED | — | yes | Async compute queue (PF-0027): where the sky / screen-space / froxel passes run, never what they compute (the same image); a performance and driver-debug switch. |
| `r_atmosphere` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `r_auto_exposure` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Exposure compensation (+-4 EV) and auto exposure on / off move the night darkness floor; the Off level had fixed exposure (V10). |
| `r_bloom_intensity` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. |
| `r_char_normal_scale` | rendering | LOCKED | — | yes | Character normal map strength: authored value. |
| `r_char_quality` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Character quality only scaled the character LOD thresholds (0.5 .. 1.25x by level, V4): fixed at 2 (x1). |
| `r_classic_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees / small cover turn into cards: classic trees 64 px vs small cover 16 px, tiers 96 .. 16 px (V2); one value for every level. |
| `r_cloud_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Cloud shadows darken patches of the map; Off / Low dropped them (V5). |
| `r_cpu_skinning` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_culling` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Frustum culling on / off (dev toggle). |
| `r_drawcollision` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_drawhitboxes` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `r_era` | rendering | LOCKED | — | yes | Era preset: no longer a setting (modern for everyone); an era only changed geometry style and cost, and its foliage / LOD columns are locked anyway. |
| `r_exposure` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Exposure compensation (+-4 EV) and auto exposure on / off move the night darkness floor; the Off level had fixed exposure (V10). |
| `r_fb_underwater` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable could drop the fog pass). |
| `r_fidelity` | rendering | LOCKED | — | yes | The quality tables' level, dev tools only (bench_fidelity, fidelity_test, shadow_preset): the player's quality settings set the features. |
| `r_foliage_wind_solo` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_gamma` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Gamma 0.5 .. 2.0 lifted shadows like a cheat; folded into the narrow brightness band, gamma fixed at 1.0. |
| `r_grass_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_grid` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Unit grid drawn on dev geometry / terrain. |
| `r_ground_macro` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on / off: dev switches (vertex colours change ground contrast). |
| `r_lens_dirt` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. |
| `r_lens_distortion` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. |
| `r_lens_drops` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lens droplets: hard-coded on (switching them off cleared the view in rain); missing below High before (V11). |
| `r_lens_drops_intensity` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lens droplets: hard-coded on (switching them off cleared the view in rain); missing below High before (V11). |
| `r_lens_flare` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. |
| `r_light_shadow_count` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lamp / flashlight shadows: without them lamps light through walls and the players behind cover; Off / Low dropped them (V5). Tile size stays a tier. |
| `r_light_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Lamp / flashlight shadows: without them lamps light through walls and the players behind cover; Off / Low dropped them (V5). Tile size stays a tier. |
| `r_lod_bias` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Character LOD bias: tiers 0.25 .. 1.0 (V4). |
| `r_lod_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_lod_fade_band` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD / impostor cross-fade band and time: part of the LOD ranges. |
| `r_lod_fade_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_lod_fade_time` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD / impostor cross-fade band and time: part of the LOD ranges. |
| `r_lod_force` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_lod_legacy` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Scope LOD rule and the legacy LOD path: dev switches. |
| `r_lod_scope` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Scope LOD rule and the legacy LOD path: dev switches. |
| `r_look` | rendering | LOCKED | — | yes | The Look: every session starts on Realism (Settings no longer offers it); the others are dev presets. |
| `r_look_modulators` | rendering | LOCKED | — | yes | Look modulators: dev switch. |
| `r_lut` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. User LUT file: could flatten the darkness; dev only. |
| `r_moon_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Twilight moon shadows: darkness at dusk; missing below High (V5). |
| `r_motion_blur_strength` | rendering | LOCKED | — | yes | Motion blur strength: fixed (the Post-processing quality turns motion blur on / off). |
| `r_post_compare` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable could drop the fog pass). |
| `r_post_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_post_disable` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable could drop the fog pass). |
| `r_post_profile` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable could drop the fog pass). |
| `r_post_volumes_draw` | rendering | LOCKED | — | yes | Post compare / profile / volumes / underwater hook / disabled passes: dev tools (r_post_disable could drop the fog pass). |
| `r_shader_hotreload` | rendering | LOCKED | — | yes | Shader hot reload, fixed frame time, anim bake rate: dev tools. |
| `r_shadow_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_shadow_max_distance` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Sun shadow distance: tiers 50 .. 300 m (max 1000) (V5). One distance for everyone; tiers keep resolution / cascades. |
| `r_surfaces` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on / off: dev switches (vertex colours change ground contrast). |
| `r_terrain` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Terrain / building texturing on / off: dev switches (vertex colours change ground contrast). |
| `r_terrain_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_terrain_watch` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_texture_compress` | rendering | LOCKED | — | yes | Block-compressed textures (BC7, PF-0035): one texture format for everyone; 0 (RGBA8) is the dev A / B reference. |
| `r_tonemapper` | rendering | LOCKED | — | yes | Look-owned: tonemapper and bloom / flare / dirt / distortion strength belong to the Look; a glare or distortion slider changes what you see near lights and the sun. |
| `r_vgeo_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees / small cover turn into cards: classic trees 64 px vs small cover 16 px, tiers 96 .. 16 px (V2); one value for every level. |
| `r_vgeo_lod_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. The classic discrete LOD pick decides silhouettes at range: tiers scaled it x8 .. x1, eras 2 .. 12 px (V3). |
| `r_water_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `r_wireframe` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `death_impact_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `death_sequence` | revive | LOCKED | — | yes | Death / downed sequence and its camera. |
| `revive_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `kit_hot_reload` | story | LOCKED | — | yes | Dev: reload building kit pieces whose .glb changed on disk (artists' round trip); not a player choice. |
| `kit_validate_on_load` | story | LOCKED | — | yes | Dev: run the kit validator as pieces load (warnings in the log). |
| `vehicle_debug` | vehicles | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `fx_tracers` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `gore_bleed` | vfx | LOCKED | — | yes | Corpse bleeding: blood trails are information. |
| `vfx_debug` | vfx | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `vfx_decal_max` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impact decal budget: tiers kept 16 .. 4096 decals (V9); one budget. |
| `vfx_enable` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Particle effects on / off (muzzle flashes, impacts): dev only. |
| `vfx_max_particles` | vfx | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Particle pool size: a small pool silently drops smoke / impact particles. |
| `vfx_timescale` | vfx | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom / dev night: dev views. |
| `ads_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `ads_dof_focus` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_dof_fstop` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_dof_near_blur` | weapons | LOCKED | — | yes | ADS DoF tuning (focus / f-stop / near blur): tuning values, not choices. |
| `ads_eye_relief_scale` | weapons | LOCKED | — | yes | ADS FOV per sight: world zoom and eye relief come from the sight data. |
| `bal_bullet_trace` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_crack_radius` | weapons | LOCKED | — | yes | Supersonic crack radius: an audio cue of incoming fire. |
| `bal_tracer_ignition` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_vapor_trails` | weapons | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Tracers / bullet trace / vapour trails / tracer ignition: muzzle and tracer visibility. |
| `bal_watch` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `ballistics_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `cl_fire_punch` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): item kick, master scale, punch, item bob, look lag. No player-facing control (Weapon kick / sway / Camera shake were removed from Settings). |
| `cl_viewmodel_sway` | weapons | LOCKED | — | yes | Viewmodel sway (cosmetic, first person only: look lag, movement sway, walk / sprint bob, sprint pose of the drawn weapon): a designed system, dev switch only. |
| `cl_weapon_sway` | weapons | LOCKED | — | yes | Weapon sway (cosmetic: aiming stays exact): Removed from Settings (the user's call, 2026-10-19): fixed at the designed value. |
| `shutter_preview` | weapons | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom / dev night: dev views. |
| `sight_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `viewmodel_sway_debug` | weapons | LOCKED | — | yes | Dev logging of the viewmodel sway layers. |
| `wpn_bob_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): item kick, master scale, punch, item bob, look lag. No player-facing control (Weapon kick / sway / Camera shake were removed from Settings). |
| `wpn_debug_melee` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `wpn_fire_anim_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): item kick, master scale, punch, item bob, look lag. No player-facing control (Weapon kick / sway / Camera shake were removed from Settings). |
| `wpn_fire_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `wpn_kick_scale` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): item kick, master scale, punch, item bob, look lag. No player-facing control (Weapon kick / sway / Camera shake were removed from Settings). |
| `wpn_look_lag` | weapons | LOCKED | — | yes | Fire animation tuning (FIRE_ANIM.md, ONE global sway): item kick, master scale, punch, item bob, look lag. No player-facing control (Weapon kick / sway / Camera shake were removed from Settings). |
| `wpn_recoil_debug` | weapons | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `wpn_visual_scale` | weapons | LOCKED | — | yes | Weapon kick (visual camera + viewmodel kick): Removed from Settings (the user's call, 2026-10-19): fixed at the designed value. |
| `env_night` | world | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom / dev night: dev views. |
| `sky_aerial_max` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `sky_aerial_scale` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Atmosphere / aerial perspective: distant haze. sky_aerial_scale was archived and player-settable 0 .. 20. |
| `sky_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `sky_disc_zoom` | world | LOCKED | — | yes | Shutter preview / VFX time scale / sky disc zoom / dev night: dev views. |
| `tod_day_ambience` | world | LOCKED | — | yes | Ambience sound overrides: audio cues are the same for everyone. |
| `tod_night_ambience` | world | LOCKED | — | yes | Ambience sound overrides: audio cues are the same for everyone. |
| `tod_on_midnight` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_noon` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_sunrise` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `tod_on_sunset` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `water_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `water_wave_wind_scale` | world | LOCKED | — | yes | Water wave wind scale: cheat tuning. |
| `weather_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `weather_on_rain_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_rain_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_snow_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_snow_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_storm_start` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `weather_on_storm_stop` | world | LOCKED | — | yes | Day / night and weather script hooks (run console text on events): tests only. |
| `wind_debug` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `wind_dir` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind come from the weather. |
| `wind_gust` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind come from the weather. |
| `wind_solo` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `wind_speed` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind come from the weather. |
| `wind_timescale` | world | LOCKED | — | yes | Wind overrides (dev): foliage motion and the ballistic wind come from the weather. |
| `wind_view` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `world_debug_chunks` | world | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `world_foliage_test` | world | LOCKED | — | yes | Test foliage / corpse collision / hold point state / sound trace: dev switches. |
| `world_terrain_grid` | world | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Unit grid drawn on dev geometry / terrain. |
| `horde_debug` | zombies | LOCKED | — | yes | Debug view / log: dev builds only (debug views can show geometry and entities through walls). |
| `sv_fov` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max_aspect` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_infinite_ammo` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_join_dead` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_maxplayers` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_relevance` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_respawn_mode` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_respawn_time` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_snaprate` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_spec_allowed` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_spec_fp_enemies` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `cl_healthbars` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Health bars over characters: shows enemy health (game state). Off; a lobby rule may turn them on for everyone. |
| `dev_damage_numbers` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Damage numbers: game state; a lobby rule. |
| `pl_accel` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_accel` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_control` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_climb_facing_cone` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_climb_hold_to_jump` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_coyote_time` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_crouch_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_friction` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_gravity` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_reach` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_shimmy_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_stamina` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_two_handed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_yaw_limit` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_buffer` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_min` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_reach` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_max_slope` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_regrab_cooldown` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_slow_walk_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_sprint_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_step_height` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_accel` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_breath` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_depth` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_dive_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_exit_depth` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_eye` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_rise` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_spring` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_sprint_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_swim_stamina` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_vault_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_vault_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_crouch_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_slow` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_wade_sprint_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_walk_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `spec_free_collide` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `spec_free_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `r_fog_scale` | rendering | LOCKED+RULE | Lobby rules | yes (+ server rule) | Visibility: changes what you can see relative to other players. Fog density multiplier (0 .. 10, archived): fixed 1.0; weather_fog / the lobby weather set the fog instead. |
| `r_foliage_density` | rendering | LOCKED+RULE | Lobby rules | yes (+ server rule) | Visibility: changes what you can see relative to other players. Foliage density: one value for everyone (consistency.json); a lobby rule may lower it for the whole session. Tiers capped it at 0.1 / 0.25 / 0.5 / 0.8 / 1.0 and eras at 1.0 / 0.7 / 0.4 / 0.2 (V1). |
| `sv_cheats` | system | LOCKED+RULE | Lobby rules | yes (+ server rule) | Cheats switch: host-owned; 0 in release builds. |
| `vehicle_fuel_scale` | vehicles | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_drag_model` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_enable` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_spin_drift` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind_dir` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `fire_tracers_only` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_bob` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_sway` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_recoil_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_spread_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_ads_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `ow_zombies` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `water_drown_damage` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_clouds` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_fog` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `world_chunk_radius` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable before). Host rule only. |
| `world_lod_distance` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable before). Host rule only. |
| `world_seed` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | World generation seed: the host's. |
| `horde_enable` | zombies | LOCKED+RULE | Lobby rules | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `horde_jobs` | zombies | LOCKED+RULE | Lobby rules | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `zombie_speed` | zombies | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
