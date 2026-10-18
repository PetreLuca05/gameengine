# Engine Fidelity

> **Settings audit (docs/SETTINGS_AUDIT.md) supersedes parts of this page.** Levels trade cost for
> quality only: foliage density, LOD / impostor / grass ranges, shadow distance and shadow types,
> fog, exposure, lens effects and decal budgets are no longer features; they are locked values in
> `assets/render/consistency.json`, the same at every level and era. No level disables sun shadows
> or rain; Off / Low use 1024 px shadow maps for the locked 150 m. The per-feature tables below
> predate that and still list the removed entries for history.

Five levels, **Off, Low, Medium, High, Modern**, form the base layer of the graphics settings. A
level decides which systems the frame graph contains and their internal cost tiers. Every other
graphics control (post-processing & effects, resolution / upscaler, shadows, geometry / foliage,
effects, the era look) is layered on top and **bounded by it**: those controls fine-tune inside what
the level allows and can never exceed it.

| | |
|---|---|
| Tables | `assets/render/fidelity/{off,low,medium,high,modern}.json`, `eras.json` (era capability matrix) |
| Bounds / apply (pure C) | `src/render/fidelity/fidelity.{h,c}` |
| Game side | `src/game/game_fidelity.c` (level cvar, detection, `bench_fidelity`, checks, overlay) |
| Frame graph check | `src/render/graph/fg_check.{h,c}` (run on every graph by `fg.c`) |
| Off output | `shaders/post/final_direct.frag`, `post_direct.glsl` |
| UI | Settings → Graphics → Normal: "Overall graphics quality" (the level as a preset) + the simple qualities (`fid_group_*`); Debug (developer mode): the "Engine Fidelity" card and every per-feature row (`src/ui/screens/scr_settings.c`, docs/UI.md) |
| Tests | `tests/test_fidelity.c` (ctest `fidelity`), `tools/tests/run_fidelity_tests.py` |
| Tables as Markdown | `python tools/fidelity_table.py` (the tables below are its output) |

Cvars: `r_fidelity` (0 Off .. 4 Modern, saved), `r_fidelity_custom` (the user's own choices,
internal), `r_fidelity_recommended`, `r_fidelity_bench` (last measured ms per level, internal),
`r_fidelity_autodetect` (first-run detection pending), `r_fidelity_target_fps` (60),
`r_perf_overlay`. Commands: `bench_fidelity <level|all> [frames] [quit]`, `fidelity_detect [popup]`,
`fidelity_check`, `fidelity_test [quit]`, `fidelity_status`.

## The levels

| level | what it is |
|---|---|
| **Modern** | Everything the renderer has, at full: GTAO, contact shadows, full-res SSR, SSGI, froxel volumetrics up to ultra with lamp scattering, 3 cascades of 2048 (up to 4096) per cascade with sky-driven PCSS, TAA / TAA-U / FSR, bokeh DoF (48 taps), camera + object motion blur, lens flare / dirt / chromatic aberration / distortion / droplets, HDR10 output, lit 6-way particles, full foliage, discrete LODs at the era's error. **Defaults = the engine's defaults before Engine Fidelity existed** (so Modern changes nothing for existing configs). Every control unlocked. |
| **High** | The same feature set at cheaper tiers: SSR half res (max), SSGI unavailable, volumetrics "high" froxel grid (160x90x64) max, shadows 2048 per cascade by default with 3 cascades, PCSS, reach -25 %, bloom 5 mips (max), hybrid DoF (32 taps), foliage cap 0.8, LOD bias +0.5 (characters x0.71, discrete LOD error x1.41), decals 256 (max 512). |
| **Medium** | Drops the expensive screen-space work: SSR unavailable (sky / ground environment reflections), GTAO low tier, fog-only volumetrics (80x45x48, no lamp scattering), 3 cascades at 1024, camera-only motion blur, aiming-only gaussian-style DoF (16 taps), lens flare / dirt / CA / distortion / droplets unavailable, FSR 1 at 0.75 by default (TAA by default, FXAA / SMAA selectable), unlit particles, foliage cap 0.5, impostors from 24 px, LOD bias +1, decals 128, textures one mip down (max), particles "medium". |
| **Low** | The minimum that still looks like the game: AO / SSR / volumetrics unavailable (the lit shaders' analytic distance fog), 2 cascades of 1024 with half the reach (12-tap PCF), tiny bloom (4 mips), DoF / motion blur unavailable, FXAA only, render scale 0.66 with a bilinear upscale (FSR 1 selectable), ACES / Neutral tonemapper + LUT, unlit "low" particles, foliage cap 0.25, impostors from 48 px, LOD bias +2, discrete LOD error x4, textures two mips down, props under 0.5 m cast no sun shadow. |
| **Off** | **No post-processing at all.** The frame graph schedules the scene passes and the final pass, which tonemaps the lit HDR straight to the swapchain (`final_direct.frag`): no AA, TAA jitter, velocity, bloom, AO, SSR, SSGI, volumetrics, DoF, motion blur, exposure adaptation (a fixed metered exposure), lens effects, sharpening, grain or vignette. The Look's colour grade stays as its baked LUT in place of the plain tonemap (one fetch, docs/LOOKS.md); its finish effects are dropped. One short hard cascade at 1024, render scale 0.6 (0.5 .. 0.66), foliage cap 0.1 with impostors from 96 px, LOD bias +3, discrete LOD error x8, decals 16, low particles, textures two mips down, props under 1 m cast no sun shadow. Every effect control is locked. Plain but correct lighting (sun, shadow, sky / IBL ambient, lamps, analytic fog). |

## Per-feature table (modern era)

Every feature is one cvar. A level gives it **unavailable** (`-`: forced off, the control locked),
**available, default off** (`off`) or **available, default on** with its default value, plus the
bounds the user may choose within: `max` (highest tier / value), `min` (for features where higher
is cheaper: LOD error thresholds, impostor size, the smallest shadow caster) or an explicit set of
allowed values `{...}`. Era-owned features (`vgeo*`, `foliage_density`) take their value from the
era preset (`r_era`): `era x3` = three times the era's own error threshold, also the floor;
`era, max 0.25` = the era's foliage density, capped.

| feature | Off | Low | Medium | High | Modern |
|---|---|---|---|---|---|
| `aa` | - | 1, max 1 | 3, max 3 | 3, max 3 | 3, max 3 |
| `upscaler` | - | off, {0,2} | 2, {0,1,2} | off, {0,1,2,3,4} | off, {0,1,2,3,4} |
| `upscale_ratio` | 0.67 | 0.67 | 0.75 | 0.67 | 0.67 |
| `render_scale` | 0.6, max 0.66, min 0.5 | 0.66, max 1, min 0.5 | 1, max 1, min 0.5 | 1, max 1.5, min 0.5 | 1, max 2 |
| `sharpen` | - | off, max 1 | 0.35 | 0.35 | 0.35 |
| `ao` | - | - | 1, max 1 | 2, max 2 | 2, max 2 |
| `contact_shadows` | - | - | 1, max 1 | 2, max 2 | 2, max 2 |
| `ssr` | - | - | - | 1, max 1 | 2, max 2 |
| `ssgi` | - | - | - | - | off |
| `puddle_reflections` | - | - | - | on | on |
| `volumetrics` | - | - | 1, max 1 | 2, max 2 | 2, max 3 |
| `vol_local_lights` | - | - | - | on | on |
| `motion_blur` | - | - | 1, max 1 | 1, max 2 | 1, max 2 |
| `dof` | - | - | 1, max 1 | 1, max 2 | 1, max 2 |
| `dof_quality` | 0, max 0 | 0, max 0 | 0, max 0 | 1, max 1 | 2, max 2 |
| `ads_dof` | - | - | on | on | on |
| `bloom` | - | on | on | on | on |
| `bloom_quality` | 0, max 0 | 0, max 0 | 1, max 1 | 1, max 1 | 2, max 2 |
| `lens_flare` | - | - | - | off, max 2 | off, max 2 |
| `lens_dirt` | - | - | - | off, max 2 | off, max 2 |
| `chromatic` | - | - | - | 0.15, max 2 | 0.15, max 2 |
| `lens_distortion` | - | - | - | off | off |
| `lens_drops` | - | - | - | on | on |
| `vignette` | - | 0.25 | 0.25 | 0.25 | 0.25 |
| `film_grain` | - | off, max 1 | 0.04 | 0.04 | 0.04 |
| `auto_exposure` | - | on | on | on | on |
| `tonemapper` | - | 0, {0,2} | 1 | 1 | 1 |
| `hdr_output` | - | - | - | off | off |
| `shadows` (0 Disable, 1 Hard Only, 2 Hard and Soft) | 0, max 1 | 1, max 1 | 2, max 2 | 2, max 2 | 2, max 2 |
| `shadow_resolution` (0 Low .. 3 Very High) | 0, max 0 | 0, max 1 | 1, max 2 | 2, max 3 | 3, max 3 |
| `shadow_projection` (0 Close, 1 Stable Fit) | 1, {0,1} | 1, {0,1} | 1, {0,1} | 1, {0,1} | 1, {0,1} |
| `shadow_cascades` | 1, {1} | 2, {1,2} | 2, {1,2,4} | 4, {1,2,4} | 4, {1,2,4} |
| `shadow_distance` (m) | 50, max 50 | 50, max 80 | 100, max 150 | 200, max 300 | 300, max 1000 |
| `soft_shadow_quality` (0 Low .. 3 Ultra) | 0, max 0 | 0, max 0 | 0, max 1 | 1, max 3 | 3, max 3 |
| `shadow_min_radius` | 1, min 1 | 0.5, min 0.5 | 0.25, min 0 | 0, min 0 | 0, min 0 |
| `shadow_caster_limit` (0 off, 1 near cascades, 2 cascade 0 + caps; docs/SHADOWS.md "Caster cascade limits") | 2, min 2 | 2, min 2 | 2, min 1 | 1, min 1 | 1, min 0 |
| `light_shadows` | - | off | on | on | on |
| `light_shadow_quality` | 1, max 1 | 1, max 1 | 1, max 1 | 2, max 2 | 2, max 2 |
| `cloud_shadows` | - | off | on | on | on |
| `moon_shadows` | - | - | off | on | on |
| `texture_quality` | 0, max 0 | 0, max 0 | 1, max 1 | 2, max 2 | 2, max 2 |
| `aniso` | 1, {1,2} | 2, {1,2,4} | 4, {1,2,4,8} | 8, {1,2,4,8,16} | 8, {1,2,4,8,16} |
| `vgeo_shadow_error_px` | era x4 | era x3 | era x2 | era x1.41 | era x1 |
| `vgeo_lod_px` | era x8 | era x4 | era x2 | era x1.41 | era x1 |
| `foliage_density` | era, max 0.1 | era, max 0.25 | era, max 0.5 | era, max 0.8 | era, max 1 |
| `foliage_wind` | 1, max 1 | 1, max 1 | 2, max 2 | 3, max 3 | 3, max 3 |
| `impostor_px` | 96, min 96 | 48, min 48 | 24, min 24 | 16, min 16 | 16, min 0 |
| `terrain_quality` | 0, max 0 | 1, max 1 | 2, max 2 | 3, max 3 | 4, max 4 |
| `lod_bias` | 0.25, max 0.25 | 0.25, max 0.25 | 0.5, max 0.5 | 0.71, max 0.71 | 1, max 4 |
| `char_quality` | 0, max 0 | 0, max 1 | 1, max 2 | 2, max 3 | 2, max 3 |
| `scope_pip_scale` | 0.5, max 0.5 | 0.6, max 0.75 | 0.75, max 1 | 1, max 1.5 | 1, max 2 |
| `vfx_quality` | 0, max 0 | 0, max 0 | 1, max 1 | 2, max 2 | 2, max 3 |
| `vfx_lighting` | - | - | - | on | on |
| `decal_max` | 16, max 32 | 64, max 128 | 128, max 256 | 256, max 512 | 256, max 4096 |
| `precip_quality` | 1, {0,1} | 1, {0,1} | -1, {-1,0,1,2} | -1, {-1,0,1,2,3} | -1, {-1,0,1,2,3} |

Budgets (1080p, mid-range GPU): Off 3 ms, Low 5 ms, Medium 8 ms, High 12 ms, Modern 16 ms.

**Per-era table differences** (on top of the base table above):

| era | level | feature | entry |
|---|---|---|---|
| 2010 | Off | `shadow_distance` | 0.6, max 0.6 (2010-era geometry (discrete LODs) is cheap: the one cascade reaches further) |
| 2015 | Low | `shadow_filter` | 1, max 1 (2015 / 2010 eras: cheaper geometry pays for soft shadows) |
| 2010 | Low | `aa` | 2, max 2 (2010-era Low: SMAA (MLAA era) fits the budget) |
| 2010 | Low | `shadow_filter` | 1, max 1 |
| 2015 | Medium | `ao` | 2, max 2 (no SSR / SSGI in the era: full GTAO instead) |
| 2010 | Medium | `ao` | 2, max 2 |
| 2010 | Medium | `aa` | 2, max 3 (no TAA in the 2010 era: SMAA by default) |
| 2010 | High | `aa` | 2, max 3 |
| 2010 | Modern | `aa` | 2, max 3 |

**Era capability matrix** (eras.json: what an era lacks is unavailable at every level):

| era | lacks | limits |
|---|---|---|
| modern | - | - |
| 2020 | - | - |
| 2015 | `vgeo_shadow_error_px`, `ssgi` | - |
| 2010 | `vgeo_shadow_error_px`, `ssgi`, `ssr`, `puddle_reflections`, `volumetrics`, `vol_local_lights`, `hdr_output` | `aa` {0,1,2}; `upscaler` {0}; `motion_blur` max 1; `dof_quality` max 1 |


### Feature → cvar

| feature | cvar | | feature | cvar |
|---|---|---|---|---|
| aa | `r_aa` (0 off, 1 FXAA, 2 SMAA, 3 TAA) | | shadows / shadow_resolution | `r_shadows` / `r_shadow_resolution` (512 .. 4096 per cascade; docs/SHADOWS_UNITY_MODEL.md) |
| upscaler | `r_upscaler` (0 off, 1 TAA-U, 2 FSR 1, 3 / 4 slots) | | shadow_cascades / shadow_projection | `r_shadow_cascades` (1 / 2 / 4) / `r_shadow_projection` |
| upscale_ratio | `r_upscale_ratio` | | shadow_distance | `r_shadow_max_distance` (m) |
| render_scale | `r_render_scale` | | soft_shadow_quality | `r_soft_shadow_quality` (tent 3 / 5 / 7, 3 Ultra = PCSS) |
| sharpen | `r_sharpen` | | shadow_min_radius / shadow_caster_limit | `r_shadow_min_radius` (m) / `r_shadow_caster_limit` (grass / small plant shadow cascades) |
| ao / contact_shadows | `r_ao` / `r_contact_shadows` | | light_shadows / _quality | `r_light_shadows` / `r_light_shadow_quality` |
| ssr / ssgi | `r_ssr` / `r_ssgi` | | cloud_shadows / moon_shadows | `r_cloud_shadows` / `r_moon_shadows` **new** |
| puddle_reflections | `r_puddle_reflections` | | texture_quality / aniso | `r_texture_quality` / `r_aniso` |
| volumetrics | `r_volumetrics` | | vgeo_shadow_error_px, vgeo_lod_px | `r_vgeo_shadow_error_px`, `r_vgeo_lod_px` (the classic renderer's shadow / discrete LOD errors) |
| vol_local_lights | `r_vol_local_lights` **new** | | foliage_density / foliage_wind / impostor_px | `r_foliage_density` / `r_foliage_wind` (0 none, 1 main bending, 2 + branches, 3 + leaf flutter, bent normals; docs/FOLIAGE_WIND.md) / `r_vgeo_impostor_px` |
| | | | terrain_quality | `r_terrain_quality` **new** (0 minimal .. 4 ultra: tiling breakers, colour variation, transitions, parallax, scatter; docs/TERRAIN_SHADING.md) |
| motion_blur | `r_motion_blur` | | lod_bias / char_quality | `r_lod_bias` / `r_char_quality` |
| dof / dof_quality | `r_dof` / `r_dof_quality` (16 / 32 / 48 taps) **new** | | scope_pip_scale | `r_scope_pip_scale` |
| ads_dof | `ads_dof` | | vfx_quality / vfx_lighting | `vfx_quality` / `vfx_lighting` |
| bloom / bloom_quality | `r_bloom` / `r_bloom_quality` (0 tiny 4 mips **new**, 1 low, 2 high) | | decal_max | `vfx_decal_max` |
| lens_flare, lens_dirt, chromatic, lens_distortion, lens_drops | `r_lens_flare`, `r_lens_dirt`, `r_chromatic`, `r_lens_distortion`, `r_lens_drops` | | precip_quality | `r_precip_quality` (-1 auto .. 3) |
| vignette, film_grain | `r_vignette`, `r_film_grain` | | auto_exposure, tonemapper, hdr_output | `r_auto_exposure`, `r_tonemapper`, `r_hdr_output` |

Not bounded (accessibility / display calibration / taste, at every level including Off):
brightness, gamma, colour-blind filter, field of view, exposure compensation, bloom intensity,
motion blur strength, fog density scale, gore, frame rate limit.

## How the bounds work

* **One hook for every path.** `fid_attach_cvars()` puts `cvar.bound` (new in `core/cvar`) on every
  feature cvar. Every set (menu, console, config file, post presets, the era preset, scripts) is
  clamped to the current level's bounds before `on_change` sees it: a level can't be exceeded by any
  route. Clamping picks the nearest allowed value that costs less (higher-is-cheaper features: the
  floor); choices without a cost order (upscaler, tonemapper, rain quality) fall back to the first
  allowed value.
* **Changing the level** (`r_fidelity`, applied live, saved): every feature takes the new level's
  default, **except the user's own choices that are below the new default**, which are kept
  (clamped to the new bounds). Lowering clamps everything above the new bounds; raising restores
  the level's defaults. The user's choices are remembered per feature (`r_fidelity_custom`,
  "ssr=1;motion_blur=0"): set half-res SSR on Modern, go to Medium (no SSR), come back: half-res
  SSR again. A choice equal to the level default is no choice. Settings → Graphics shows "Reset N
  tweaks" to drop them.
* **Unavailable** features are forced to their off value; features without an off state (render
  scale, texture quality ...) keep their value inside the bounds and their control is locked.
* **The frame graph follows** on the next frame: passes and resources exist only for what the
  settings enable (see "No dead passes"); three frames later `fg_trim` frees the pool images the
  new graph doesn't use, so a switch never accumulates GPU memory (`fidelity_test`).
* **Post presets** (Low / Medium / High / Ultra in "Post-processing & Effects") apply through the
  bounds; a preset whose values the level doesn't allow is greyed with the level it needs, and one
  only reads back when every value fits (else Custom).
* **UI.** Settings → Graphics starts with the Engine Fidelity card (dropdown with the "Recommended"
  tag, one-line description, last benchmark fps / ms and the budget, Run benchmark, Apply
  recommended, Reset tweaks). The sections below it (Display, View, Resolution & upscaling,
  Post-processing & Effects, Shadows & lighting, Geometry / textures / foliage, Effects, Sights,
  Weather) draw every bounded control through `row_fid_*`: options above the level are greyed with
  "requires High" in the dropdown, a feature the level lacks is a faint row reading "Requires Engine
  Fidelity: Medium" (or "Not in the 2010 era"), sliders span only the level's range (the tooltip
  says where more is allowed). There is no "Custom" fidelity level: custom tuning is the sections
  below, inside the level. The former Post-processing tab is part of the Graphics tab.

## Internal consistency

Checked by `test_fidelity` for every level x era:

* TAA unavailable → TAA-U unavailable, no projection jitter (`post_wants_jitter`) and no velocity
  target (below).
* HDR10 output only at High / Modern (and not in the 2010 era).
* Puddle reflections need SSR; aiming DoF needs DoF; lens dirt needs bloom.
* Upscaler defaults per level: Medium FSR 1 at 0.75, Low a bilinear 0.66 (FSR 1 selectable),
  Off a bilinear 0.6, High / Modern native.
* Higher levels never take a feature away or lower a ceiling (`fid_tables_complete`).

## Off: scene + direct output

`PostSettings.direct_output` (Off). `post_screen_space` schedules nothing (no Hi-Z, contact, GTAO,
SSGI, SSR, froxels), `post_capture_scene` nothing (no colour pyramid, no depth history),
`post_resolve` no AA / upscale / motion blur / DoF / lens droplets / exposure / bloom / flare /
tonemap / debug view and returns the lit HDR; the final pass runs `final_direct.frag` on it:
bilinear upscale from the render resolution, a **fixed exposure**, a hue-preserving Reinhard on
luminance, the colour-blind filter and the user's brightness / gamma, sRGB with a blue-noise dither.
The UI backdrop blur takes the same transform (`ui_blur_down.comp` with `pc.u0.x`).

What stays: the scene passes (prepass without velocity, opaque, decals,
characters, the sky and its LUTs, `fog_apply` for the analytic fog over the sky, scene copy,
transparency, viewmodel), the weapon sights (red dot / scope: aiming is gameplay), the scope /
phone picture-in-picture, the UI. The gameplay screen effects (damage tint, low health, flash) live
in the tonemap pass and are not drawn at Off.

**Off exposure.** No histogram, no adaptation: a light meter on the CPU
(`post_core.c direct_exposure`): the sun's illuminance on the ground plus the sky ambient, mid grey
lit by it, +1.4 EV scene key, clamped to the day / night EV range (`daynight.json` night floor / day
max), plus exposure compensation. Measured against the histogram exposure in the dev room:

| time | histogram EV | Off metered EV |
|---|---|---|
| 12:00 | 1.20 | 1.30 |
| 07:00 | 0.40 | 0.12 |
| 19:30 | -1.49 | -1.50 |
| 23:30 | -1.50 | -1.50 (night floor) |

## No dead passes

Every frame `fg_execute` runs `fgc_analyze` on the graph it is about to execute
(`fg_last_report`, `fidelity_check`):

* **dead pass**: it writes something and nothing it writes is consumed: an imported image, a
  transient used by a later pass, or a history read in this frame's graph under its own or its
  ping-pong partner's name (`taa_hist_a` ↔ `_b`). Passes without tracked writes are side-effect
  passes (buffer / readback work: exposure, autofocus, EXR capture).
* **unused resource**: a transient / history declared but used by no scheduled pass.
* **undefined read**: a transient read before any pass wrote it. `fg_reference()` marks a binding
  that keeps an image alive without reading it (bound by one pass and written later).
* **stale images**: pool images the graph doesn't use (freed by `fg_trim` after a level switch and
  by the 240-frame eviction otherwise).

What this changed in the existing graph: the depth history (and its `depth_prev` history) and the
colour pyramid are only scheduled / declared when a reader exists (TAA, temporal AO / SSR / SSGI);
the G-buffer **velocity** target exists only when TAA / TAA-U, motion blur, the temporal passes or
the velocity debug view read it. Without it the prepass renders with a null third attachment
(`VK_EXT_dynamic_rendering_unused_attachments`, enabled when the device has it; without the
extension velocity is always written and the log says so).

## Shadows

Every preset's shadow entries follow one formula (docs/SHADOWS.md "Per-preset tables",
`tests/test_shadow_csm.c` asserts it). The knobs are Unity's Quality > Shadows
(docs/SHADOWS_UNITY_MODEL.md; `shs_quality_for_tier` mirrors the JSON and the tests check it):
- `r_shadows`: Disable / Hard Only / Hard and Soft.
- `r_shadow_resolution`: the tile size per cascade, 512 .. 4096 (the atlas is 2 x 2 tiles).
- `r_shadow_cascades`: 1 / 2 / 4 at Unity's split percentages up to the shadow distance. Unused
  tiles aren't drawn and the classic renderer doesn't cull them.
- `r_shadow_max_distance`: metres; the last cascade ends there and fades over the cascade border.
- `r_soft_shadow_quality`: tent 3 / 5 / 7, or Ultra = PCSS + wide mode.
- `r_moon_shadows`: the moon's own map while the sun still owns the cascades.
- `r_shadow_min_radius`: skips sun shadow casters smaller than the radius.
- `r_shadow_caster_limit`: grass / ground cover casts sun shadows only into cascade 0, small plants
  into cascades 0 - 1 (mode 2 adds 15 / 30 m caps for the one- and two-cascade levels), trees into all
  (docs/SHADOWS.md "Caster cascade limits", PF-0013).
- `r_vm_shadow`: the first-person rig's own tile (1024 .. 2048, following the sun's resolution). The 2010 era
keeps a fixed-hardness kernel; its shadows still lighten and vanish with the sky.

## Era axis

`r_era` (modern, 2020, 2015, 2010) is the look / content axis; Engine Fidelity bounds its cost.

* **Per-era tables.** Each level file has a base table and per-era differences (`"eras"`). The
  2015 / 2010 eras render discrete LODs (cheaper geometry), so their Low affords soft shadows (and
  SMAA in 2010), their Medium full GTAO; 2010-era Off has a longer shadow reach.
* **Era-owned features** take the era's own value: Low's discrete LOD error is 4x the era's (modern 2 px →
  8 px, 2020 3 px → 12 px), its foliage the era's density capped at 0.25 (2010: 0.2 stays 0.2). The
  era preset's detection compares against the bounded values, so `r_era` still reads back as the era.
* **Capability matrix** (`eras.json`, table above). What an era lacks is unavailable at every level
  and its fidelity entry is ignored: the 2015 era has no SSGI; the 2010 era also no SSR (nor
  puddle mirrors), froxel volumetrics, HDR10, TAA (FXAA / SMAA only) or upscalers, at most camera
  motion blur and hybrid DoF. Custom (`r_era 4`) has the modern capabilities.

## Auto-detect, benchmark, overlay

* **First run** (`r_fidelity_autodetect`: a new install, or the first start of a config that
  predates Engine Fidelity): once the world has rendered 30 frames, every level runs for a moment
  (4 warm-up frames, then 0.2 .. 0.4 s and at least 3 .. 6 frames: about 2 s in all) behind an opaque "Detecting graphics settings" cover, measuring the GPU
  frame time (timestamps around the whole command buffer, `render_gpu_frame_ms`). The highest
  level within the frame time of `r_fidelity_target_fps` (60 fps = 16.7 ms) is recommended once in
  a dialog with **Apply recommended** / Keep current; the measurements go to `r_fidelity_bench`.
  Every cvar is restored exactly afterwards. Re-run any time: Settings → Graphics → Run benchmark
  (or `fidelity_detect`). Never in `--test` runs.
* **`bench_fidelity <level|all> [frames] [quit]`**: the fixed dev room flythrough (hall → post
  corner → firing line → horde yard → back, Catmull-Rom, camera override) at the level's defaults;
  logs the GPU frame time against the budget, the CPU frame time, every pass's GPU ms (post passes
  marked), post total, post pass count and GPU memory; restores everything. Runs headless (Mesa
  llvmpipe under Xvfb).
* **`r_perf_overlay`** (Settings → Graphics → View): level, fps, CPU / GPU frame ms, the budget
  line over a 180-frame frame-time graph.

## Budgets and measured results

Budgets, 1080p on a mid-range GPU (the RTX 3060 Ti of `docs/RENDERING_POST.md`): **Modern ≤ 16 ms,
High ≤ 12, Medium ≤ 8, Low ≤ 5, Off ≤ 3** (whole GPU frame). Check them on that machine with
`python tools/tests/run_fidelity_tests.py build --budgets` (1920x1080, `bench_fidelity all`).

**Not yet measured on the reference GPU.** This change was developed in a Linux container without a
GPU; everything below ran on Mesa **llvmpipe** (software Vulkan) at 960x540. Absolute times are
meaningless there; the ratios and the structure are not.

| level | passes (post) | frame graph pool | all GPU memory | GPU frame (llvmpipe, flythrough) | vs Modern | budget vs Modern |
|---|---|---|---|---|---|---|
| Off | 21 (0) | 2.8 MB | 703.1 MB | 105 ms | 0.19 | 0.19 |
| Low | 25 (4) | 11.4 MB | 713.0 MB | 178 ms | 0.32 | 0.31 |
| Medium | 36 (15) | 25.9 MB | 729.5 MB | 350 ms | 0.62 | 0.50 |
| High | 39 (18) | 63.3 MB | 833.8 MB | 525 ms | 0.94 | 0.75 |
| Modern | 39 (18) | 68.2 MB | 838.7 MB | 561 ms | 1.00 | 1.00 |

(Static dev room view for the pass counts / memory, `fidelity_test`; the flythrough for the frame
time, 40 measured frames per level. The Medium / High ratios are higher than their budgets on a CPU
rasteriser, where the compute-heavy screen-space passes cost relatively more than on a GPU.)
50 level switches, back to Modern: 838.7 MB → 838.7 MB. Vulkan validation: 0 messages over every
level and the switches.

## Spec mapping: where the implementation differs

* **Cascades.** The shadow atlas is 2x2 with three cascade tiles and Modern itself has 3, so High
  has 3 cascades (not 4) at 2048 per cascade. Softness follows the sky on every tier (docs/SHADOWS.md):
  High / Modern measure the blocker (PCSS), the lower tiers assume its distance (PCF).
* **AO tiers.** GTAO always runs at half resolution; Medium's "cheap SSAO quarter res" is GTAO's
  low tier (6 steps), not a quarter-res pass.
* **Particles at half / quarter resolution.** `vfx_half_res` exists as a cvar but the renderer has no
  low-resolution particle pass, so the levels don't use it; particle cost is tiered by `vfx_quality`
  (counts / sorting / collision / lights) and `vfx_lighting` (lit 6-way vs unlit soft).
* **Foliage fade.** Fade distances are per material in the content (`.vgeo` / model manifest) and
  have no global scale; "shorter fade / short grass / grass off" is approximated by the density cap
  (Off 0.1) and earlier impostors.
* **LOD bias.** Characters: `r_lod_bias` = 2^-bias (+0.5 → 0.71, +1 → 0.5, +2 / +3 → 0.25, the
  cvar's minimum); shadow LOD error x1.41 / x2 / x3 / x4, discrete LODs x1.41 / x2 / x4 / x8. The classic
  renderer and the shared systems take their ranges from `assets/render/lod.json` x its tier
  multipliers (Off 0.3, Low 0.4, Medium 0.6, High 0.85, Modern 1) instead, `r_lod_bias` against the
  level's default as the user's bias (docs/LOD.md).
* **Medium AA.** There is no separate "TAA-low": Medium defaults to TAA, FXAA / SMAA selectable.
* **DoF.** "Gaussian" / "hybrid" / "bokeh" are the gather's 16 / 32 / 48 taps (hexagonal bokeh only
  at 48).
* **"Low may reuse the 2015-era pipeline".** Not forced: the era decides the discrete LOD errors
  (the 2015 / 2010 eras get their own, cheaper Low tables).
* **Benchmark "offscreen".** The measured frames go to the swapchain behind an opaque cover (the
  renderer has no headless path inside the game loop).
* **Off gameplay feedback.** Damage / flash / low-health screen effects are tonemap-pass effects and
  are not shown at Off.

## Adding a feature

1. Give it a cvar (registered before `game_fidelity_init`, i.e. during engine / game init) and make
   the renderer honour it.
2. Add it to `FidFeature` and `k_features` in `src/render/fidelity/fidelity.c`: JSON id, cvar, kind,
   whether it has an off value (and which), its full range, whether higher is cheaper, whether the
   era owns it.
3. Give it an entry in **every** level file (`fid_tables_complete` refuses a missing one): `"avail"`
   `no` / `off` / `on`, `"default"`, and `"max"` / `"min"` / `"allowed"`; era-owned: `"scale"` or a
   cap. Per-era differences go in the level's `"eras"` block, era capabilities in `eras.json`.
   Higher levels may not take it away or lower its ceiling.
4. Put its control in Settings → Graphics with `row_fid_combo` / `row_fid_toggle` /
   `row_fid_slider` (greying and locking come for free).
5. If it adds passes: schedule them only when it's on, classify them in `fid_pass_is_post` if they
   are post-processing, declare resources only when a pass uses them.
6. Run `test_fidelity`, `tools/tests/run_fidelity_tests.py`, regenerate the tables above with
   `python tools/fidelity_table.py`.

## Tests

* `test_fidelity` (ctest `fidelity`, pure): the real tables load and are complete / monotonic for
  every era; applying every level from every level, in every era, then reading every cvar matches
  the table; every option of every control is allowed / greyed exactly per level and its "requires"
  level is the lowest allowing it, a set outside the bounds stores an allowed, cheaper value;
  lowering clamps, raising restores the defaults, explicitly lower choices survive (and persist);
  Off has every post feature off; the internal consistency rules; era matrix / era-owned scaling;
  pass classification; the frame graph check on hand-built graphs (dead depth history, ping-pong
  histories, unused / undefined resources); malformed tables rejected.
* `tools/tests/run_fidelity_tests.py [build] [--res WxH] [--frames N] [--budgets]`: the above,
  then in the game with the validation layers `fidelity_test` (every level: valid graph, no stale
  pool image after the trim, Off zero post passes, every cvar = the table; 50 switches without GPU
  memory growth; 0 validation messages), Off / Modern screenshots, `bench_fidelity all` for all five
  levels. Headless under Xvfb on Linux.
