# Settings audit (fair play, consistency, the simplified Graphics tab)

Status: **DRAFT, proposal before implementation.** Nothing in the engine has changed yet. After
review, the `settings_audit` command and `tools/tests/lint_settings.py` will generate the table in
section 8 from the registration flags, and this draft becomes their checked-in output.

## 1. Classes

| Class | Flag | Who may write | In the UI | In config.cfg |
|---|---|---|---|---|
| Player-editable | `CVAR_EDITABLE` | anyone (UI, console, config), bounded by the fidelity level as now | yes, or console only | yes |
| Tier-driven | `CVAR_TIER` | the Engine Fidelity / Era code only (an internal setter); console writes rejected in release | no (Graphics > Debug in dev builds) | no |
| Hard-coded | `CVAR_LOCKED` | dev builds only; in release the value comes from `assets/render/consistency.json` and writes are rejected and logged | no | no |
| Hard-coded, host rule | `CVAR_LOCKED \| CVAR_RULE` | as LOCKED, plus the host's lobby **server rules** (replicated with `MSG_CVARS`, enforced by the host, never per client) | Lobby rules only | no |

"Release build" means a new compile definition `DG_DEV_BUILD=0` (CMake option `DG_DEV_BUILD`, on by
default so the user's builds and every test keep today's behaviour; the shipped build turns it off).
The `developer` cvar stays a UI switch and **never** unlocks a write.

Note: the task names `settings.json`; the actual settings file is `config.cfg` (archived cvars,
`src/app/video.c`). Locked and tier cvars stop being written there and are ignored when read back
(old entries are dropped with one log line).

## 2. What the current Graphics tab holds, and where each row goes

**Graphics > Normal** (today)

| Row | Cvar(s) | Fate |
|---|---|---|
| Default (reset button) | all graphics | kept, top right |
| Resolution / Window mode / VSync / Apply | r_width, r_height, r_mode, r_vsync | Display |
| Frame rate limit | r_maxfps | Display |
| Stretched resolution | r_stretch | **removed, locked 0** (targets look wider) |
| Field of view | cl_fov | Comfort, band 60 .. 90 deg vertical |
| Hands FOV follows camera / Hands FOV | cam_weapon_fov | **removed, locked 0** (viewmodel FOV) |
| Red dot brightness | sight_brightness | moves to HUD > Crosshair, 0.25 .. 2 |
| Overall graphics quality (+ Custom, benchmark) | r_fidelity | becomes the top **Engine Fidelity** card (benchmark, Apply recommended); no more "Custom" |
| Render scale / Upscaling / Upscaling resolution | r_render_scale, r_upscaler, r_upscale_ratio | Display (bounded by the level) |
| Anti-aliasing | r_aa | **removed, tier** |
| Sharpening | r_sharpen | Comfort |
| Post-processing / Shadows / Textures / Effects / Foliage / View distance (group tiers) | fid groups | **removed, tier** (Foliage and View distance changed visibility: locked parts listed in 5) |
| Motion blur | r_motion_blur | Comfort |
| Look (+ preview strip, hold to compare) | r_look | top, unchanged |
| Brightness | r_brightness | Display, 0.85 .. 1.15 |
| Gamma | r_gamma | **removed, locked 1.0** |
| HDR10 output / paper white / peak brightness | r_hdr_output, r_hdr_paper_white, r_hdr_max_nits | Display (paper white 100 .. 300); peak brightness console only |
| Colour-blind filter | r_colorblind | Display |
| Screen effects | r_post_feedback | Comfort, 0.5 .. 1 (accessibility) |
| Gore | gore_level | moves to Character > Content |

**Graphics > Debug** (developer mode today): Engine Fidelity switch and tweaks, performance overlay,
every upscaler slot, post preset, AO, contact shadows, SSR, SSGI, volumetrics, lamp scattering, motion
blur strength (moves to Comfort), DoF mode (moves to Comfort) and quality, bloom, bloom quality and
intensity, lens flare, lens dirt, chromatic aberration (Comfort), lens distortion, lens droplets and
intensity, vignette (Comfort), film grain (Comfort), auto exposure, exposure compensation, the whole
Unity-style shadow block, era, vgeo switch, textures, aniso, foliage density, impostors, terrain
detail, character LOD, character quality, particles, lit particles, impact decals, scope picture
resolution, ADS DoF, rain / snow quality, puddle reflections, water.
In a release build the Debug sub-tab no longer exists. In a dev build it stays as the tuning page, its
rows marked TIER / LOCKED.

## 3. The ambiguous calls (reasoning)

- **AO, contact shadows, SSR, SSGI**: tier. AO darkens corners a little more on higher levels; the
  task puts AO in the tier list, and SSR only reflects pixels already on screen, so neither shows
  anything new.
- **Shadows on/off**: locked at a minimum of *Hard*. "Disable" removes enemy shadows *and* the dark
  interiors that hide players (the classic "no shadows" advantage). Hard vs Soft, resolution,
  cascades and filter stay tier.
- **Lamp / flashlight shadows**: locked on. Without them a lamp lights a player standing behind a
  wall. Only the tile size (256 / 512 px) stays a tier.
- **Lens flare / dirt / distortion / bloom strength**: locked to the Look. Glare near the sun, lamps
  and muzzle flashes is a visibility outcome. Flare, dirt and distortion are already 0 at every level,
  so the lock costs nothing today.
- **Lens droplets**: hard-coded on (the task's default). Turning them off clears the view in rain.
- **Rain / snow quality**: the tier changes shading, never particle count. Dense rain is cover.
- **Gore**: stays editable because it is a content / age-rating choice, but moves out of Graphics.
  Gore Off already keeps neutral dust puffs, so hits stay readable. Corpse bleeding (`gore_bleed`)
  is locked.
- **Weapon kick**: editable within 0.5 .. 1. At 0 the sight picture stays still while the aim
  recoils, an advantage in sprays. **Weapon sway**: 0 .. 1, because it is cosmetic (aiming stays
  exact). **Camera shake** (`cam_kick_scale`): 0.25 .. 1, an accessibility setting for motion
  sickness. The fire-animation tuning (`wpn_kick_scale`, `wpn_fire_anim_scale`, `cl_fire_punch`,
  bob, look lag) is locked: FIRE_ANIM.md has ONE global sway.
- **Motion blur**: camera / object blur stays editable. The viewmodel is excluded from the velocity
  buffer (hard-coded; to verify in the velocity pass), so blur can never mask recoil.
- **Screen effects** (`r_post_feedback`): editable for photosensitivity, but floored at 0.5. At 0 a
  flashbang does nothing.
- **Brightness / gamma**: one narrow brightness band, 0.85 .. 1.15. Gamma is locked at 1.0: the old
  ranges (brightness 0.3 .. 3, gamma 0.5 .. 2) were a night-vision cheat.
- **HDR paper white**: editable within 100 .. 300 nits (the HDR form of brightness). Peak nits is a
  property of the monitor: console only.
- **Crosshair**: style, colour, size, opacity and outline are editable. Hit markers and target tint
  are editable too, since turning them off only removes your own information. Follow-kick (true aim
  point vs screen centre) and obstruction are behaviour, so they are locked.
- **Health bars / damage numbers**: they show game state, so they are locked off. The host may turn
  them on for everyone (rule).
- **Stretched resolution**: locked 0. It is the 4:3-stretch trick that makes targets wider on screen.
- **Occlusion culling**: locked at two-pass. Mode 1 (last frame's Hi-Z) draws a player stepping out
  of cover one frame late.
- **Era**: editable (the task), but an era may only change geometry *style* and cost. Its foliage
  density (1.0 / 0.7 / 0.4 / 0.2) and LOD pick (2 / 3 / 4 / 12 px) columns move into the locked set.
- **Spectator free camera collision**: a host rule. A ghost flying through walls can call out
  positions.
- **Character animation features** (lean, foot IK, ragdolls ...): locked. Every client must see
  remote bodies the same way (lean off would hide a peeking head).
- **Phone exposure / filters**: editable. The phone is an in-game item with the same controls for
  everyone; it is not a renderer setting.

## 4. Proposed locked values (`assets/render/consistency.json`)

Each entry gets a comment in the file. The startup self-check hashes the sorted
`name=value` pairs (FNV-1a, like `maplist_hash`) and XORs the result into the content hash of
`lobby_version_hash`. It is also sent in `MSG_HELLO`, and the host rejects a mismatch with
"consistency set mismatch". That bumps `DG_PROTOCOL_VERSION` to 25.

| Cvar | Locked value | Today (range across levels) |
|---|---|---|
| r_foliage_density | 1.0 (rule-adjustable 0.5 .. 1.0) | 0.1 .. 1.0 |
| r_vgeo_impostor_px / r_classic_impostor_px | 16 / 16 | 96 .. 16 / 64 |
| r_vgeo_error_px | 1.0 | x4 .. x1 |
| r_vgeo_lod_px | 2.0 | x8 .. x1, eras 2 .. 12 |
| r_lod_bias | 1.0 | 0.25 .. 1.0 |
| r_lod_fade_band / r_lod_fade_time | lod.json values | as is |
| r_vgeo_occlusion | 2 | 0 .. 2 |
| r_shadow_max_distance | 150 m | 50 .. 300 (1000) |
| r_shadows | >= 1 (Hard); 1 vs 2 is tier | 0 .. 2 |
| r_cloud_shadows / r_moon_shadows / r_light_shadows | 1 / 1 / 1 | off on low levels |
| r_light_shadow_count | MAX_SHADOW_POINTS | 0 .. max |
| r_fog_scale | 1.0 | 0 .. 10 (archived) |
| r_atmosphere / sky_aerial_scale / sky_aerial_max | 1 / 2.0 / 3000 | settable |
| r_auto_exposure / r_exposure | 1 / 0 | off at the Off level |
| r_gamma | 1.0 | 0.5 .. 2.0 |
| r_bloom_intensity, r_lens_flare, r_lens_dirt, r_lens_distortion, r_tonemapper | the Look's | settable |
| r_lens_drops / r_lens_drops_intensity | 1 / 0.8 | missing below High |
| r_stretch | 0 | 0 .. 1 |
| vfx_decal_max / vfx_max_particles | 256 / 1 << 20 | 16 .. 4096 |
| fx_tracers, bal_bullet_trace, bal_vapor_trails, bal_tracer_ignition | 1 | settable |
| world_chunk_radius / world_lod_distance | 4 / 70 m (rule) | settable |
| r_viewmodel_fov / cam_weapon_fov | 0 / 0 (follow the camera) | settable |
| crosshair_obstruct / crosshair_follow_kick | 1 / 0 | settable |
| cl_interp | 100 ms | settable |
| death_sequence | 1 | settable |
| cl_healthbars / dev_damage_numbers | 0 / 0 (rule) | settable |
| sv_cheats | 0 in release | 1 |

## 5. Fidelity tier violations found (each fixed in the implementation)

| # | Where | Violation | Fix |
|---|---|---|---|
| V1 | fidelity `foliage_density` max 0.1 / 0.25 / 0.5 / 0.8 / 1.0; eras 1.0 / 0.7 / 0.4 / 0.2 (`k_eras`, vk_vgeo.c) | Low draws a tenth of the grass: prone players are visible only to Low | entries removed; the density is locked; Low gets cheaper cover shading (no SSS, half atlas, wind 1) instead |
| V2 | `impostor_px` 96 .. 16; classic 64 vs vgeo 16 | trees turn into cards at different ranges | one locked switch size for both renderers |
| V3 | `vgeo_error_px` x4 .. x1, `vgeo_lod_px` x8 .. x1, eras 2 .. 12 px | silhouettes at range differ | locked; the tiers keep sw-raster threshold, shadow error and streaming budgets |
| V4 | `lod_bias` 0.25 .. 1.0; `r_char_quality` "scales LOD thresholds" | distant characters drop detail earlier on Low | LOD bias locked 1.0; char quality only selects skin shading |
| V5 | `shadow_distance` 50 .. 300 (+ era 2010 Off 70); `shadows` default 0 at Off; `cloud_shadows`, `light_shadows`, `moon_shadows` off / missing on low levels | shadows reach different distances, whole shadow types missing | locked distance 150 m; Hard minimum everywhere; cloud / lamp / moon shadows on at every level (lowest tile size, 1 cascade at Off) |
| V6 | game_post.c:517 froxel density `max(level_fog * 1.5, 0.004) * r_fog_scale` vs render.c:1414 distance fog `e->fog_density` | without volumetrics (Off, Low, 2010 era) the fog is thinner than with them | one pure `fog_extinction()` used by both paths; the distance-fog fallback matches the froxel result at the same depth |
| V7 | `water` tier 0 = "flat + sky reflection" (no refraction) | swimmers hidden on Off / Low, visible on Medium+ | tier 0 keeps the cheap refraction + the locked turbidity; tiers change ripples / caustics / waves |
| V8 | `precip_quality` allowed [0, 1] on Off / Low (0 = no rain) and -1 "from the post preset" | rain as cover disappears on low levels | the particle count is locked; tiers change shading only; 0 and -1 removed |
| V9 | `vfx_quality` -> `k_quality_mul` scales emission counts; `decal_max` 16 .. 4096 | thinner smoke, fewer impact decals on low levels | the emission multiplier is fixed at 1; tiers change sort budget, lighting, collision and half-res; locked decal budget |
| V10 | Off level: `post: false` = fixed exposure, no auto exposure | night darkness differs on Off | the Off level runs the minimal visibility pass (1x1 luminance auto exposure with the shared EV clamps) |
| V11 | `lens_drops` missing below High (Off has no post) | droplets disappear on low levels | droplets in the minimal pass at every level |

The visible test: `tools/tests/run_settings_tests.py` renders the same frame per level (and per era)
headless and compares foliage instance counts, fog density at fixed depths, shadow distance, LOD /
impostor picks and particle counts within tolerance. Cost of the fixes: Off and Low draw full-density
grass and lamp shadows. If they then miss their budget (3 / 5 ms), lower the **locked** density for
everyone (decision D2), never per level.

## 6. Proposed tab layout

```
Settings:  Graphics | HUD | Controls | Audio | Character | Phone      (Crosshair merges into HUD)

Graphics                                         [ (i) Why can't I change X? ]   [ Default ]
  Engine Fidelity   [ High v ]   description · last benchmark · [Run benchmark] [Apply recommended]
  Era               [ Modern v ]
  Look              [ Realism v ]   preview strip · hold to compare with Realism

  DISPLAY
    Resolution · Window mode · VSync · [Revert] [Apply]
    Frame rate limit
    Render scale            (bounded by the level)
    Upscaling · Upscaling resolution
    HDR10 output · HDR paper white      (only if the display supports HDR)
    Brightness              0.85 .. 1.15
    Colour-blind filter
    Show FPS

  COMFORT
    Field of view           60 .. 90 deg (or "set by the server")
    Sprint FOV effect       0 .. 7 deg
    Head bob                0 .. 1
    Camera shake            0.25 .. 1
    Weapon sway             0 .. 1
    Weapon kick             0.5 .. 1
    Motion blur             Off / Camera / Camera + objects   (+ strength when on)
    Depth of field          Off / When aiming / Always
    Film grain · Chromatic aberration · Vignette · Sharpening
    Screen effects          0.5 .. 1  (damage, flash, low health)

HUD
  CROSSHAIR  style · colour · length · thickness · extra gap · opacity · outline · hit markers ·
             target tint · red dot brightness
  LAYOUT     HUD scale · HUD opacity · phone panel size / corner
  COMPASS    compass · compass arc (new: hud_compass_arc 90 .. 180 deg) · coordinates · pings on compass
  KILLS      kill counter · always split · kill feed
  PINGS      markers · other players' pings · chat lines · sounds
  WHEELS     hold to open · hold time · slow motion · use healing at once · timed ring

Character: skin · UI scale · safe area · CONTENT: gore · SPECTATING: death cam time, blend
           (health bars move out, locked; free camera collision becomes a host rule)
```

Tooltip on the Graphics header: *"Why can't I change X? Settings that change what you can see
compared with other players (foliage, fog, view and shadow distance, detail ranges, weather, lens
effects, brightness beyond a small range) are the same for everyone for fair play. Engine Fidelity
changes how good they look, not what you can see."*

## 7. Open decisions (defaults proposed)

- **D1. FOV band**: 60 .. 90 deg vertical (about 91 .. 121 deg horizontal at 16:9). Today 50 .. 120.
- **D2. Locked foliage density**: 1.0 (the authored density). If Off / Low miss their budget, the
  fallback is one lower value for everyone (for example 0.6), with a lobby rule for 0.5 .. 1.0.
- **D3. Locked shadow distance**: 150 m (Medium's max). Off then draws 1 cascade over 150 m (soft
  but present).
- **D4. Off level**: it gains a minimal "visibility pass" (auto exposure, fog, droplets). The
  alternative, dropping Off from the shipped levels, is simpler but loses the weakest-GPU option.
- **D5. Crosshair tab merges into HUD** (the task lists crosshair style / colour under HUD).
- **D6. `DG_DEV_BUILD` defaults ON** in CMake, so the user's builds and every test keep today's
  writable cvars. Lock enforcement is tested with `DG_DEV_BUILD=0` test binaries and a
  `--release-locks` runtime switch for script tests.

## 8. Full classification (every registered cvar)

458 cvars: 126 EDITABLE, 52 TIER, 200 LOCKED, 80 LOCKED + host rule. Generated from the registrations. **UI after the rebuild** "—" means the cvar is in no settings tab.

| Cvar | Category | Class | UI after the rebuild | Locked in release | Reason |
|---|---|---|---|---|---|
| `snd_master` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_music` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_sfx` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_ui` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_voice` | audio | EDITABLE | Audio | no | Volumes. |
| `snd_weather` | audio | EDITABLE | Audio | no | Volumes. |
| `cam_kick_scale` | camera | EDITABLE | Graphics > Comfort | no | Camera shake: visual camera kick on shots, 0.25 .. 1 (accessibility). Aim recoil is separate (wpn_recoil_scale, host). Also scales cl_fire_punch. |
| `cl_fov` | camera | EDITABLE | Graphics > Comfort | no | World FOV: personal comfort, narrowed to a band of 60 .. 90 deg vertical (~91 .. 121 deg horizontal at 16:9; was 50 .. 120). sv_fov / sv_fov_max still override. |
| `cl_headbob` | camera | EDITABLE | Graphics > Comfort | no | Head bob amount 0 .. 1 (was 0 .. 3; above 1 only makes it worse). cl_headbob stays as its on/off alias. |
| `cl_headbob_amount` | camera | EDITABLE | Graphics > Comfort | no | Head bob amount 0 .. 1 (was 0 .. 3; above 1 only makes it worse). cl_headbob stays as its on/off alias. |
| `cl_sprint_fov` | camera | EDITABLE | Graphics > Comfort | no | Sprint FOV kick: comfort (motion sickness); bounded 0 .. 7 deg, it can only narrow the change. |
| `hud_compass` | hud | EDITABLE | HUD | no | HUD layout / visibility: scale, opacity, compass, coordinates, kill counter / feed, pings, phone panel, timed ring, clock. |
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
| `anim_eval_budget` | animation | TIER | — | yes (fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
| `r_aa` | rendering | TIER | — | yes (fidelity writes) | Anti-aliasing mode: a quality / cost trade at the same visibility; picked by the level. |
| `r_aniso` | rendering | TIER | — | yes (fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_ao` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_bloom` | rendering | TIER | — | yes (fidelity writes) | Bloom on / off follows the level; its strength is locked to the Look (r_bloom_intensity). |
| `r_bloom_quality` | rendering | TIER | — | yes (fidelity writes) | DoF / bloom gather quality. |
| `r_char_maps` | rendering | TIER | — | yes (fidelity writes) | Character shading quality (skin maps). Violation V4: it also scales the LOD thresholds today; that part moves out. |
| `r_char_quality` | rendering | TIER | — | yes (fidelity writes) | Character shading quality (skin maps). Violation V4: it also scales the LOD thresholds today; that part moves out. |
| `r_contact_shadows` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_dof_quality` | rendering | TIER | — | yes (fidelity writes) | DoF / bloom gather quality. |
| `r_foliage_wind` | rendering | TIER | — | yes (fidelity writes) | Foliage *shading* tier: wind layers. Density / fade distances are locked. |
| `r_grass_sss` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_grass_tex` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_light_shadow_quality` | rendering | TIER | — | yes (fidelity writes) | Lamp / flashlight shadow tile size (256 / 512 px). Whether lamps are shadowed is locked. |
| `r_post_preset` | rendering | TIER | — | yes (fidelity writes) | Post preset: follows the level. |
| `r_precip_quality` | rendering | TIER | — | yes (fidelity writes) | Rain / snow shading tier. Particle density is locked (violation V8): tiers change shading, never count. |
| `r_puddle_reflections` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_scope_pip_scale` | rendering | TIER | — | yes (fidelity writes) | Scope picture resolution: like render scale for the scope (sharpness, not content; the scope's LOD is locked). |
| `r_shadow_cascade_border` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_cascades` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_min_radius` | rendering | TIER | — | yes (fidelity writes) | Small props (< radius) skip the sun shadow: cost only; characters, vehicles and doors are exempt (to verify in the shadow caster filter). |
| `r_shadow_near_offset` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_projection` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_resolution` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split2` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_1` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_2` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadow_split4_3` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadowmask_distance` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_shadowmask_mode` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_soft_shadow_quality` | rendering | TIER | — | yes (fidelity writes) | Shadow filter / resolution / cascades / projection / splits / border / near offset / shadowmask: quality of the same shadows. Distance and on/off are locked. |
| `r_ssgi` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_ssr` | rendering | TIER | — | yes (fidelity writes) | AO / contact shadows / SSR / SSGI: lighting quality tiers. SSR only reflects pixels already on screen (no new information). |
| `r_terrain_quality` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_terrain_tex_size` | rendering | TIER | — | yes (fidelity writes) | Terrain / ground shading tiers (TERRAIN_SHADING.md): texture variation, parallax, transitions; no effect on what stands on it. |
| `r_texture_quality` | rendering | TIER | — | yes (fidelity writes) | Texture mips / anisotropic filtering: quality only. |
| `r_vgeo` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_characters` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_pool_mb` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_shadow_error_px` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_shadows` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_stream_pages_per_frame` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vgeo_sw_px` | rendering | TIER | — | yes (fidelity writes) | Renderer choice (vgeo vs classic) and its internal budgets: picked by Era / level; both paths must reach the same locked LOD / impostor outcome. |
| `r_vm_shadow` | rendering | TIER | — | yes (fidelity writes) | Viewmodel shadow tile: only your own arms. |
| `r_vol_local_lights` | rendering | TIER | — | yes (fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog *density* is locked (r_fog_scale) and the distance-fog fallback must match it (violation V6). |
| `r_volumetrics` | rendering | TIER | — | yes (fidelity writes) | Volumetric fog quality (froxel tiers, lamp scattering). Fog *density* is locked (r_fog_scale) and the distance-fog fallback must match it (violation V6). |
| `r_water` | rendering | TIER | — | yes (fidelity writes) | Water shading tier (ripples, caustics, waves). Transparency / turbidity are locked (violation V7: tier 0 drops refraction). |
| `vfx_half_res` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `vfx_lighting` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `vfx_quality` | vfx | TIER | — | yes (fidelity writes) | Particle shading: half-res pass, lighting, sort budget. Emission counts are locked (violation V9). |
| `ads_dof` | weapons | TIER | — | yes (fidelity writes) | ADS weapon blur: availability by level, and r_dof 0 turns it off. |
| `world_chunk_uploads` | world | TIER | — | yes (fidelity writes) | Streaming / animation budgets: cost spread over frames; final state identical. |
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
| `r_classic_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees turn into cards; vgeo 16 px vs classic 64 px today, and tiers 96 .. 16 px (V2). |
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
| `r_shadows` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Shadows Disable removes every sun shadow (enemy shadows, dark interiors): minimum Hard for every level; Hard vs Soft stays a tier (V5). |
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
| `r_vgeo_impostor_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Impostor switch size decides when trees turn into cards; vgeo 16 px vs classic 64 px today, and tiers 96 .. 16 px (V2). |
| `r_vgeo_lod_px` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. LOD error / pick thresholds decide silhouettes at range (task: vgeo error threshold, LOD bias). Tiers scale them x4 .. x1 (vgeo) and x8 .. x1 (classic), eras 2 .. 12 px (V3). |
| `r_vgeo_occlusion` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Occlusion culling mode: mode 1 (last frame's Hi-Z) shows a player stepping out from cover one frame late; two-pass (2) for everyone. |
| `r_viewmodel_fov` | rendering | LOCKED | — | yes | Viewmodel FOV (task: viewmodel FOV locked). Hands follow the camera FOV; the sight's ADS zoom still applies. |
| `r_water_debug` | rendering | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `r_wireframe` | rendering | LOCKED | — | yes | Visibility: changes what you can see relative to other players. Wireframe / hitboxes / collision / CPU skinning / third person: wallhack-class views, dev only. |
| `death_impact_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `death_sequence` | revive | LOCKED | — | yes | Death / downed sequence and its camera (task: death / downed sequences). |
| `revive_debug` | revive | LOCKED | — | yes | Debug view / log: dev builds only (debug views can expose geometry and entities through walls). |
| `cfg_migration` | system | LOCKED | — | yes | Config migration counter: written by the game only. |
| `sv_cheats` | system | LOCKED | — | yes | Cheats switch: host-owned; defaults to 0 in release builds (registered with 1 today, 'dev build default on'). |
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
| `sv_fov` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_fov_max_aspect` | camera | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_infinite_ammo` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_join_dead` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_maxplayers` | network | LOCKED+RULE | — | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_relevance` | network | LOCKED+RULE | — | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_respawn_mode` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_respawn_time` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_snaprate` | network | LOCKED+RULE | — | yes (+ server rule) | Host networking: snapshot rate / relevance distance (what is sent at all) / max players. |
| `sv_spec_allowed` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `sv_spec_fp_enemies` | network | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `dev_damage_numbers` | player | LOCKED+RULE | — | yes (+ server rule) | Damage numbers (archived, on by default): game state; a lobby rule. |
| `pl_accel` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_accel` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_control` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_air_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_coyote_time` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_crouch_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_friction` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_gravity` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_reach` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_hang_yaw_limit` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_buffer` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_jump_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_max` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_min` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_reach` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_mantle_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_max_slope` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `pl_noclip_speed` | player | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
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
| `spec_free_collide` | player | LOCKED+RULE | — | yes (+ server rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `spec_free_speed` | player | LOCKED+RULE | — | yes (+ server rule) | Spectator free camera collision / speed: a ghost flying through walls can call out positions. Host rule. |
| `r_fog_scale` | rendering | LOCKED+RULE | — | yes (+ server rule) | Visibility: changes what you can see relative to other players. Fog density multiplier (0 .. 10, archived!): fixed 1.0; weather_fog / lobby rule set the weather instead. |
| `r_foliage_density` | rendering | LOCKED+RULE | — | yes (+ server rule) | Visibility: changes what you can see relative to other players. Foliage density is one value for everyone (consistency.json); a lobby rule may change it for the whole session. Today tiers cap it at 0.1 / 0.25 / 0.5 / 0.8 / 1.0 and eras at 1.0 / 0.7 / 0.4 / 0.2 (V1). |
| `vehicle_fuel_scale` | vehicles | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_drag_model` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_enable` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_spin_drift` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `bal_wind_dir` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `block_fire_when_barrel_blocked` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `fire_tracers_only` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_bob` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_ads_sway` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_muzzle_tolerance` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_recoil_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_spread_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_ads_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `wpn_sway_scale` | weapons | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `ow_zombies` | world | LOCKED+RULE | — | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `water_drown_damage` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_clouds` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `weather_fog` | world | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
| `world_chunk_radius` | world | LOCKED+RULE | — | yes (+ server rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable today). Host rule only. |
| `world_lod_distance` | world | LOCKED+RULE | — | yes (+ server rule) | Visibility: changes what you can see relative to other players. Draw distance: chunks loaded around the player and the first terrain LOD switch (archived, player-settable today). Host rule only. |
| `world_seed` | world | LOCKED+RULE | — | yes (+ server rule) | World generation seed: the host's. |
| `horde_enable` | zombies | LOCKED+RULE | — | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `horde_jobs` | zombies | LOCKED+RULE | — | yes (+ server rule) | Open-world zombies / horde: host / dev. |
| `zombie_speed` | zombies | LOCKED+RULE | Lobby rules | yes (+ server rule) | Host gameplay / world rule (CVAR_SERVER, replicated with MSG_CVARS): never a client setting. |
