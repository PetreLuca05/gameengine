# Rendering: frame graph and post-processing stack

Deadground renders every frame through a small **frame graph** (`src/render/graph/`). The
post-processing / screen-space stack (`src/render/post/`, `shaders/post/`) is a set of graph
passes around the lit scene. Everything after the lit pass is linear HDR until the tonemapper.

```
vfx compute -> shadow map                                    (explicit barriers, render.c)
prepass (depth + G-buffer: normal, material, velocity)
hi-z -> contact shadows -> GTAO (+temporal, upsample) -> SSGI (+temporal, upsample)
     -> SSR (+temporal) -> volumetric froxels (inject, integrate)
opaque (lit, consumes AO / contact / SSR / SSGI) -> decals -> opaque_dynamic (characters)
fog_apply -> scene_copy ("hdr_opaque") -> transparent (blended, lines, text, particles)
colour pyramid + depth history (next frame's SSR / SSGI / temporal)
vm_prepass (viewmodel G-buffer) -> viewmodel
TAA / TAA-U / FXAA / SMAA -> FSR 1 EASU -> motion blur -> depth of field
exposure (histogram) -> bloom -> lens flare -> tonemap + grade (or debug view)
final (CAS, grain, dither, sRGB / PQ) + UI -> present
```

## Frame graph (`src/render/graph/fg.{h,c}`)

* `fg_begin(render w/h, output w/h)`, then per pass `fg_pass(name, exec, user)` with
  `fg_read` / `fg_write(pass, resource, access)`; `fg_execute(cmd)` allocates, inserts the
  barriers (synchronization2), timestamps and runs every pass in registration order.
* **Resources**: `fg_texture` (transient: pooled by format / size / mips, **aliased** when the
  lifetimes don't overlap), `fg_history` (persistent by name, e.g. `taa_hist_a/b`;
  `fg_history_valid()` is false on the first frame / after a resize), `fg_import` (renderer-owned
  images: HDR, depth, viewmodel depth, swapchain, the VFX scene copy; layouts tracked through a
  pointer so they stay correct across frames).
* **Access → state**: `FG_SAMPLED` (shader read-only / depth read-only), `FG_STORAGE` (GENERAL),
  `FG_COLOR`, `FG_DEPTH`, `FG_DEPTH_READ`, `FG_TRANSFER_SRC/DST`. A first write to a transient
  discards (UNDEFINED). Mip chains (Hi-Z, bloom) stay in GENERAL and sync their mips with
  `fg_mip_barrier`. The storage buffers (exposure, histogram) use `post_buffer_barrier`.
* **Diagnostics**: GPU ms per pass (`fg_pass_ms`, two frames late), pool memory, debug-utils names
  on the pooled images and labels per pass (validation builds), an aliasing sanity check
  (two resources of one pass never share an image).
* `r_post_disable ssr,gtao,...` skips passes by name (a skipped pass's consumers fall back to
  neutral inputs). `final` and the scene passes should stay enabled.

## G-buffer, velocity and jitter

| target | format | content |
|---|---|---|
| `gb_normal` | RG16_SNORM | octahedral world normal |
| `gb_material` | RGBA8 | r roughness, g metallic, b flags/255 (1 viewmodel, 2 dynamic, 4 unlit), a emissive/8 |
| `gb_velocity` | RG16F | (current − previous) unjittered position in UV units |
| depth | D32 | written by the prepass; the lit passes shade at LESS_OR_EQUAL (`invariant gl_Position`) |

* **Previous transforms**: `DrawItem.object_id` / `SkinnedDraw.object_id` (stable ids, 0 =
  static). The renderer keeps an id → last model / last palette hash; moving draws get their
  previous model through the `DrawPrev` SSBO (set 0 binding 4, push `flags.y`), skinned draws the
  previous palette appended to the bone SSBO (`info.z`). Viewmodel parts without an id are
  identified by draw order. Entities (`game_render.c`) and characters (`characters.c`) set ids.
* **Jitter**: Halton(2,3), 8 phases, as an NDC translation after the projection (`R->proj_j`),
  only while TAA / TAA-U is active. `R->view_proj` stays unjittered for gameplay queries.
  Post shaders get the **unjittered** `proj` / `inv_proj` plus `U.jitter` and add / remove it
  explicitly (`view_pos`, SSR / SSGI / contact screen projections, TAA sample positions).
* **Camera cuts** (teleport > 4 m in one frame, resize, `render_camera_cut()`): temporal history
  and velocities reset.

## Passes

| pass | file | notes |
|---|---|---|
| Hi-Z | `hiz.comp` | min-depth pyramid (SSR) |
| contact shadows | `contact.comp` | sun ray march in clip space; N·L ≤ 0 and > 50 m skipped; low = 8 steps at half res, high = 14 steps full res |
| GTAO | `gtao.comp` + `temporal` + `upsample` | XeGTAO style, half res, 2–3 slices, blue-noise rotated; AO only darkens indirect light (+ specular occlusion) |
| SSGI | `ssgi.comp` | half res, previous frame's colour pyramid, temporal + bilateral upsample (Ultra) |
| SSR | `ssr.comp` + `temporal` | Hi-Z march, roughness-dependent mip of the colour pyramid, fallback to the sky / ground env; half res (low) is sampled bilinearly by the lit pass |
| volumetrics | `vol_inject`, `vol_integrate`, `fog_apply` | froxels (80×45×48 … 240×135×96, 80 m log depth), height fog, sun with shadow-map visibility (light shafts), point lights, HG phase, temporal reprojection; blended surfaces and particles apply the integrated volume themselves (lit set 2 binding 4, `vfx_particle.frag` set 2 binding 0). `fog_apply` also fogs the sky: sky pixels take the full froxel range (as the geometry beyond 80 m, no horizon seam); without froxels it puts the lit shaders' analytic fog over the sky through the height-fog layer up to the far plane (full at the horizon, only the layer towards the zenith). The scope PiP gets the same fog: each PiP pixel's world point is looked up in these froxels / the aerial perspective through the main projection (`scope_pip_fog`, docs/ATTACHMENTS.md "The PiP pass") |
| TAA / TAA-U | `taa.comp` | Blackman-Harris reconstruction of the jittered samples, YCoCg variance clip, velocity dilation, disocclusion via depth history, viewmodel / transparency aware (`hdr_opaque`); TAA-U reconstructs at output resolution |
| FXAA / SMAA | `fxaa.comp`, `smaa_*` | SMAA 1x (third_party/smaa, edges → weights → blend) |
| FSR 1 | `fsr_easu.comp` | AMD FidelityFX EASU (third_party/fsr, MIT); FSR 2/3 and DLSS are enum slots that fall back to FSR 1 (SDKs not bundled) |
| motion blur | `mb_tiles`, `mb_neighbor`, `mb_blur` | McGuire reconstruction; mode 1 camera only (depth reprojection), 2 camera + objects; shutter fraction, 40 px clamp, viewmodel sharp |
| depth of field | `dof_focus`, `dof_coc`, `dof_down`, `dof_gather`, `dof_composite` | thin-lens CoC (focal length, f-stop, 24 mm sensor), GPU autofocus on the crosshair (eased in log space), half-res near / far gather, circle or hexagonal bokeh; mode 1 = only while aiming |
| auto exposure | `histogram`, `exposure` | 256-bin log histogram, centre weighted, percentile window, EV100 clamp, separate up / down adaptation; result stays on the GPU (`exposure_buf`), read back for the UI / tests |
| bloom | `bloom_down`, `bloom_up` | Jimenez 13-tap down (Karis on mip 0) / tent up, energy-conserving mix, lens dirt |
| lens flare | `flare.comp` | ghosts + halo + anamorphic streak from the quarter-res bloom mip |
| tonemap + grade | `tonemap.comp` | distortion, chromatic aberration, bloom / flare (Look threshold / tint, halation), exposure, vignette, white balance, contrast / saturation, gameplay feedback, the **Look**'s baked LUT (docs/LOOKS.md; its tonemapper and whole grade in one fetch) or **ACES** (Hill fit) / **AgX** / **Khronos PBR Neutral**, lift / gamma / gain, 3D LUT (.cube), colour-blind correction, brightness / gamma, letterbox, A/B split |
| final | `final.frag` | CAS sharpening, film grain, blue-noise triangular dither, sRGB OETF or **HDR10 PQ** |

Shaders use one push-descriptor layout (`post_internal.h`): bindings 0–7 samplers, 8–11
storage images, 12 `PostUBO`, 13/14 storage buffers, 128 B push constants. Pipelines are
created lazily from `shaders/post/<name>` and rebuilt on shader hot reload.

## HDR10

`r_hdr_output 1` picks an `HDR10_ST2084` swapchain (A2B10G10R10) when the display offers one
(`VK_EXT_swapchain_colorspace`). The tonemapper shoulders at `r_hdr_max_nits` with
`r_hdr_paper_white` as the SDR reference white, converts Rec.709 → Rec.2020 and the final pass
PQ-encodes. Limitation: the UI is drawn unchanged into the HDR swapchain (no paper-white scaling
yet); the development display has no HDR so the path is untested on hardware.

## Settings, presets and budgets

`src/game/game_post.c` builds the `PostSettings` (`src/render/post/post_settings.h`) every frame
from the cvars, the level's post volumes, gameplay feedback and photo mode, and passes them to
`render_post_settings`. The **Graphics** menu (Settings → Graphics → Post-processing & Effects)
edits the cvars directly (saved with the config).

**Engine Fidelity** (docs/ENGINE_FIDELITY.md) is the base layer: every cvar below is bounded by the
level (`r_fidelity`, Off .. Modern), presets included (a preset the level doesn't allow is greyed).
At **Off** `PostSettings.direct_output` is set and the stack schedules no pass at all: the final pass
shows the lit HDR through `final_direct.frag` (fixed metered exposure, plain tonemap, sRGB). New
tiers used by the levels: `r_dof_quality` (gather taps 16 / 32 / 48), `r_bloom_quality 0` (tiny,
4 mips), `r_vol_local_lights` (lamp scattering in the froxels; off = fog only). Passes and their
resources exist only with a reader (depth history, colour pyramid, G-buffer velocity); every graph
is validated (`fidelity_check`).

| preset | AA | AO | contact | SSR | SSGI | volumetrics | motion blur | DoF | bloom | flare |
|---|---|---|---|---|---|---|---|---|---|---|
| Low | FXAA | off | off | off | off | off | off | off | low | off |
| Medium | TAA | low | low | half | off | low | camera | ADS | low | off |
| High (default) | TAA | high | high | full | off | high | camera | ADS | high | off |
| Ultra | TAA | high | high | full | on | ultra | camera + objects | ADS | high | on |

Changing any of them shows *Custom* (`r_post_preset 4`). Main cvars: `r_aa` (0 off, 1 FXAA, 2
SMAA, 3 TAA), `r_upscaler` (0 off, 1 TAA-U, 2 FSR 1, 3 FSR 2 slot, 4 DLSS slot),
`r_upscale_ratio`, `r_sharpen`, `r_ao`, `r_contact_shadows`, `r_ssr`, `r_ssgi`,
`r_volumetrics`, `r_fog_scale`, `r_motion_blur`, `r_motion_blur_strength`, `r_dof`, `r_bloom`,
`r_bloom_quality`, `r_bloom_intensity`, `r_lens_flare`, `r_lens_dirt`, `r_chromatic`,
`r_vignette`, `r_film_grain`, `r_lens_distortion`, `r_tonemapper` (0 ACES, 1 AgX, 2 Neutral; with a
Look the tonemapper is the Look's own and `r_tonemapper` drives only the A/B split and photo mode;
vignette / grain / chromatic / sharpen / bloom intensity scale the Look's finish, docs/LOOKS.md),
`r_auto_exposure`, `r_exposure`, `r_lut` (.cube path, e.g. `assets/post/luts/teal_orange.cube`;
samples from `tools/gen_luts.py`), `r_colorblind`, `r_brightness`, `r_gamma`,
`r_post_feedback`, `r_hdr_output`, `r_hdr_paper_white`, `r_hdr_max_nits`, `r_render_scale`.

**Stretched resolution simulator** (`r_stretch` 0 .. 1, Settings → Graphics → View): the
competitive "4:3 stretched" look. The 3D view (world and viewmodel) is projected as if the screen
were k = 1 + 1.5 × r_stretch times narrower at the same vertical FOV and fills the full width, so
everything looks k times wider (0.25 ≈ 4:3 on a 16:9 display, 1 = 2.5x). Every projection-based
pass and query (culling, `render_world_to_screen`, post, sights) uses the same matrices; the HUD
and menus are not stretched. It is a look, not a cost setting: the render resolution is unchanged.

**Budgets** (post passes = every pass except prepass / opaque / decals / opaque_dynamic /
scene_copy / transparent / vm_prepass / viewmodel), RTX 3060 Ti, 1920×1080, dev-room test corner,
`post_bench 240` (tools/tests/run_post_tests.py):

| preset | post | all passes | frame graph memory | budget |
|---|---|---|---|---|
| Low | 0.72 ms | 1.26 ms | 81 MB | – |
| Medium | 1.84 ms | 2.42 ms | 175 MB | ≤ 2 ms ✔ |
| High | 2.69 ms | 3.19 ms | 214 MB | ≤ 4 ms ✔ |
| Ultra | 3.31 ms | 3.82 ms | 279 MB | – |

## Post volumes and gameplay feedback

Levels (`data/levels/<map>.json`) may define a global grade (`"post": {...}`) and boxes:

```json
"post_volumes": [
  { "name": "cold_corridor", "min": [-16, 0, -29.2], "max": [-9, 2.6, -26.3], "blend": 1.5, "priority": 2,
    "settings": { "temperature": -0.35, "saturation": 0.7, "vignette": 0.5, "grain": 0.12 } }
]
```

Fields: `exposure` (EV, added), `saturation` / `contrast` / `bloom` (multipliers), `temperature`,
`tint`, `vignette`, `fog_density`, `fog_height`, `fog_albedo`, `sun_shafts`, `chromatic`,
`grain`, `lut_weight`, `ao`, `lift` / `gain` / `gamma` (rgb). A volume's weight is 1 inside and
fades to 0 over `blend` metres outside (evaluated at the camera); higher priorities blend over
lower ones. `post_volumes` lists them with their current weights, `post_reload` reloads,
`r_post_volumes_draw 1` draws the boxes.

Gameplay feedback (scaled by `r_post_feedback`): damage (red edge tint, from the HUD damage
flash), low health (< 35 %: pulsing vignette, desaturation), flash (`game_post_flash`,
`post_flash`), hit markers (chromatic kick), sprint (vignette), underwater (`r_fb_underwater`
hook: wobble + absorption tint), night vision (`nightvision`), death (desaturation).

## Debug and capture

* `r_post_debug N`: 1 ao, 2 contact, 3 ssr, 4 ssgi, 5 velocity, 6 normal, 7 material, 8 depth,
  9 hi-z, 10 CoC, 11 luminance (EV false colour), 12 TAA history weight, 13 froxel slice,
  14 bloom, 15 flare, 16 motion blur tiles, 17 fog opacity.
* `r_post_compare 1`: A/B split (left = raw lit HDR, exposed + tonemapped only).
* `r_post_profile 1|2`: per-pass GPU ms overlay with the preset budget (+ memory);
  `post_bench [frames] [label]` logs averages; `post_status` prints the active settings.
* `screenshot_hdr [name]`: PNG + linear EXR (half float, scene-referred, exposure applied) into
  `screenshots/`. `photomode`: HUD hidden, panel with DoF (auto / manual focus, f-stop, focal
  length), exposure, temperature, saturation, vignette, grain, tonemapper and capture; fly with
  `noclip`.
* Dev room **test corner** (south-west, "POST-PROCESS TEST CORNER"): glossy floor, mirror, gold /
  chrome / copper / plastic props (SSR, specular occlusion), neon signs (bloom, flare), god-ray
  room with a slit roof and a fog volume (volumetrics), dark corridor with a cold grade
  (auto exposure, volumes). Brushes accept `roughness`, `metallic`, `emissive`, `unlit`.
  `tools/tests/post_corner.cfg` captures it.

## Tests

`python tools/tests/run_post_tests.py [build_dir]` (scripts generated by
`tools/tests/gen_post_tests.py`):

1. **toggle**: every pass disabled on its own, every AA / upscaler / tonemapper / debug view /
   colour-blind mode / preset / feedback effect / LUT / photo mode / HDR capture / render scale,
   with the Vulkan validation layers: 0 validation messages, every frame plausible, EXR readable.
2. **perf**: GPU ms per preset at 1080p against the budgets above.
3. **taa**: a scripted camera pan rendered with TAA and without AA at identical poses: < 1 % of
   pixels may differ by more than 0.3 (edges only, no trails), and 12 frames after the pan stops
   the image must match the converged frame (no residual ghosting).

## Known limitations

* FSR 2/3 and DLSS are selectable slots without their SDKs (they use FSR 1).
* Post volumes are evaluated at the camera position (the whole frame takes the blend), not per
  pixel.
* GPU particles don't receive the volumetric fog yet (blended lit surfaces do).
* The frame graph pool favours speed over memory (≈ 175–280 MB at 1080p); transients alias only
  within identical format / size / mip keys.
