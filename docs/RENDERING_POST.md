# Rendering: frame graph and post-processing stack

Deadground renders every frame through a small **frame graph** (`src/render/graph/`). The
post-processing / screen-space stack (`src/render/post/`, `shaders/post/`) is a set of graph
passes around the lit scene. Everything after the lit pass is linear HDR until the tonemapper.

```
vfx compute -> foliage wind -> rain occlusion                (explicit barriers, render.c)
shadow maps: sun cascades, moon, viewmodel tile, local lights (the graph's "shadows" marker)
sky LUTs / sky view / clouds / cloud shadow / environment    (docs/DAYNIGHT.md)
prepass (depth + G-buffer: normal, material, velocity)
hi-z -> contact shadows -> GTAO (+bilateral blur, temporal, upsample) -> SSGI (+temporal, upsample)
     -> SSR (+temporal) -> volumetric froxels (inject, integrate)
opaque (lit, consumes AO / contact / SSR / SSGI) -> decals -> opaque_dynamic (characters)
fog_apply -> scene_copy ("hdr_opaque") -> transparent (blended, lines, text, particles)
colour pyramid + depth history (next frame's SSR / SSGI / temporal)
vm_prepass (viewmodel G-buffer) -> viewmodel
TAA / TAA-U / FXAA / SMAA -> FSR 1 EASU -> motion blur -> depth of field
exposure (histogram) -> bloom -> lens flare -> tonemap + grade (or debug view)
final (CAS, grain, dither, sRGB / PQ) + UI -> present
```

That is the serial order. With async compute (below) the frame is declared prepass -> sky ->
screen-space chain -> shadow maps -> froxels -> opaque ..., the sky and the screen-space chain run
on a second queue next to the shadow maps, the froxels next to the opaque pass.

## Frame graph (`src/render/graph/fg.{h,c}`)

* `fg_begin(render w/h, output w/h)`, then per pass `fg_pass(name, exec, user)` with
  `fg_read` / `fg_write(pass, resource, access)`; `fg_exec_begin(cmd)` allocates and schedules,
  `fg_exec_until(marker)` / `fg_exec_rest()` insert the barriers (synchronization2), timestamps and
  run every pass in registration order (the caller records a marker pass's work in between:
  render.c's shadow maps at `fg_marker("shadows")`), `fg_submit` ends and submits the command
  buffers. `fg_async(pass)` / `fg_after(pass, dep)`: async compute, below.
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

## Async compute (PF-0027)

Compute passes marked `fg_async` run on a second queue: the sky passes (`sky_luts` .. `sky_env`),
the screen-space chain (`hiz`, `contact`, `gtao`, `ao_blur_x/y`, `gtao_temporal`,
`gtao_upsample`, SSGI, SSR) and the froxels (`vol_inject`, `vol_integrate`). Their GPU time hides
under the raster-bound shadow maps and opaque pass, where the compute units idle.

| | |
|---|---|
| Queues (`vk_device.c`) | a compute-only family (its own hardware queue on NVIDIA / AMD) and a second queue of the graphics family, when the device has them; `--no-async-compute` creates neither (all resources EXCLUSIVE). The log line `vulkan: async compute:` says what was found |
| `r_async_compute` (LOCKED, default 1) | 0 off, 1 the compute family, 2 the second graphics queue; off when the device has not that queue (macOS / MoltenVK: one family, so off by default). Read every frame; `perf_fix_disable PF-0027` turns it off. Mode 2 measured slower on the RTX 3060 Ti (GPU frame +9 .. +14 %, the async passes' scopes do not grow: the driver time-slices the graphics family's queues instead of overlapping them) |
| Sharing | with a compute family, what both queues touch is `VK_SHARING_MODE_CONCURRENT` between the two families (`vkd_share_image` / `vkd_share_buffer`): the frame graph's pooled images, depth, the sun shadow map, the sky images, the post textures (noise / white / LUT), every `vkr_buffer_create` buffer. No ownership transfers |
| Scheduling (`fg.c schedule`) | every pass gets a queue; a pass depends on the previous user of each of its resources and on its `fg_after` passes, an async pass also on the graphics work declared before it (only graphics passes declared after it overlap it: fewer batches). When one of those is on the other queue (and no earlier batch of its queue already waited for it or a later one), the pass starts a batch that waits for the other queue's timeline value of the batch holding it, and that batch ends with a signal. Batch 0 holds the work recorded before the graph, whose signal also follows the previous frame's graphics work (the sky images the async passes rewrite were read there). The last graphics batch waits for the last async batch (the frame's fence covers both queues) |
| Layouts | what an async pass uses next is moved to its layout at the end of the graphics batch it waits for (`batch_handoff`): the compute queue never changes a depth layout. A resource last used on the other queue (or in an earlier frame there) needs no source access in its barrier: the timeline wait ordered and published the writes |
| Non-graph dependencies | `fg_after`: the lit passes (`opaque`, `opaque_dynamic`, `transparent`, `sky`, `fog_apply`) after `sky_last_pass()` (the environment, SH, cloud shadow and aerial perspective images); `vol_inject` after the `shadows` marker (the shafts sample the sun shadow map). Stage masks for barriers inside an async exec go through `fg_stages` (the compute family has no fragment / vertex stages) |
| Frame order | the shadow maps are recorded at the graph's `shadows` marker. With async compute this frame (`fg_async_frame`) the graph is declared prepass, sky, screen-space chain, `shadows`, froxels, opaque ... (`build_frame_graph`): the sky and the screen-space chain form one async batch after the prepass, next to the shadow maps; the froxels one after the shadow maps, next to the opaque pass. Without it the marker comes first and the sky before the prepass (the old order). The rain occlusion map, which the prepass reads for wet surfaces, is drawn before the graph in both |
| Submission | one `vkQueueSubmit2` per queue (timeline semaphores allow a wait submitted before its signal); the last graphics batch waits for the swapchain image and signals the present and the slot's fence |
| Profiling | GPU scopes per queue (`vkp_stream`; no pipeline statistics on the compute family). An async pass's scope is its elapsed time while it shares the GPU (it grows, e.g. `vol_inject` 0.08 -> 0.5 ms): compare `gpu_frame` |
| `r_async_info` | logs the last frame's batches, e.g. `G[pre..prepass]+ C[sky_luts..ssr_temporal]+ G[shadows]+ C[vol_inject..vol_integrate]+ G[opaque..sky] G[fog_apply..final]` (G graphics, C async, + signals); the first frames and every change of shape are logged (a few times) |

Measured (RTX 3060 Ti, driver 581.80, Modern, 1600 x 900, `perf_verify PF-0027`, A / B alternating
in one process, the GPU shared with other work; docs/perf/FINDINGS.md): GPU frame, pooled, -3.6 %
(p < 1e-4), images and state identical. Per scene over two runs: the dev room -4.8 .. -10.4 %
(4.4 -> 4.0 ms), the meadow -3.6 .. -3.8 % (10.2 -> 9.8 ms), the pine forest -0.5 .. -5.3 % (its
14 ms shadow pass is far longer than the ~0.8 ms of async work, the gain is bounded by that work).
CPU work +3 .. +5 % (+0.15 ms, not significant on this machine: one more submit, six command
buffers instead of one). The first layout (sky in its own batch next to the prepass, eight batches)
saved about as much GPU time with more CPU time; the froxels on the graphics queue (four batches)
saved less in the forest. `r_async_compute 2` (the second graphics queue) made the GPU frame 9 ..
14 % slower.

Validation: the core layers report nothing with async compute on (compute family and second
graphics queue) or off; synchronization validation reports the same 164 messages with it on and
off in the dev room (a pre-existing write-after-write report on a depth layout transition after a
sampled read; with async compute half of them are reported at submit time, because the write and
the transition now sit in different command buffers), none on the compute queue.

## G-buffer, velocity and jitter

| target | format | content |
|---|---|---|
| `gb_normal` | RG16_SNORM | octahedral world normal |
| `gb_material` | RGBA8 | r roughness, g metallic, b flags/255 (1 viewmodel, 2 dynamic, 4 unlit, 8 thin occluder; bits 4-7 (1 − ao_strength) × 15), a emissive/8 |
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
| GTAO | `gtao.comp` + `ao_blur` (×2) + `temporal` + `upsample` | XeGTAO style, half (or full) res, blue-noise rotated, thin-occluder aware, per-material weight; AO only darkens indirect light (+ specular occlusion). See "Foliage-aware AO" |
| SSGI | `ssgi.comp` | half res, previous frame's colour pyramid, temporal + bilateral upsample (Ultra) |
| SSR | `ssr.comp` + `temporal` | Hi-Z march, roughness-dependent mip of the colour pyramid, fallback to the sky / ground env; half res (low) is sampled bilinearly by the lit pass |
| volumetrics | `vol_inject`, `vol_integrate`, `fog_apply` | froxels (80×45×48 … 240×135×96, 80 m log depth), height fog, sun with shadow-map visibility (light shafts), point lights, HG phase, temporal reprojection; blended surfaces and particles apply the integrated volume themselves (lit set 2 binding 4, `vfx_particle.frag` set 2 binding 0). `fog_apply` also fogs the sky: sky pixels take the full froxel range (as the geometry beyond 80 m, no horizon seam); without froxels it puts the lit shaders' analytic fog over the sky through the height-fog layer up to the far plane (full at the horizon, only the layer towards the zenith). The scope PiP gets the same fog: each PiP pixel's world point is looked up in these froxels / the aerial perspective through the main projection (`scope_pip_fog`, docs/ATTACHMENTS.md "The PiP pass") |
| TAA / TAA-U | `taa.comp` | Blackman-Harris reconstruction of the jittered samples, YCoCg variance clip, velocity dilation, disocclusion via depth history, viewmodel / transparency aware (`hdr_opaque`); TAA-U reconstructs at output resolution |
| FXAA / SMAA | `fxaa.comp`, `smaa_*` | SMAA 1x (third_party/smaa, edges → weights → blend) |
| FSR 1 | `fsr_easu.comp` | AMD FidelityFX EASU (third_party/fsr, MIT) |
| FSR 2 | `fsr2_prep.comp` + `fsr2/*.comp` | AMD FidelityFX FSR 2.2 temporal upscaling (third_party/fsr2, MIT), see "FSR 2"; DLSS is an enum slot that runs FSR 2 (SDK not bundled) |
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

## FSR 2

`r_upscaler 3` (Settings → Graphics → Upscaling: FSR 2; Medium, High and Modern allow it) resolves the
jittered HDR frame with AMD FidelityFX Super Resolution 2.2 (`src/render/post/post_fsr2.c`). The SDK's
own compute shaders are vendored in `third_party/fsr2` (MIT; `third_party/VERSIONS.txt`) and built by
CMakeLists.txt as one permutation per pass (HDR input, render-resolution unjittered motion vectors,
standard depth, FP32, Lanczos reprojection as the SDK picks on wave32 GPUs, no RCAS: `r_sharpen` in the
final pass sharpens as for every upscaler). `post_fsr2.c` is the host side of the SDK's `ffx_fsr2.cpp` in
C over the frame graph: the constants, the jitter phases (8 × (display / render)², `post_jitter_pixels`),
the resources and the dispatch order. Each pass's descriptor set layout is built from its SPIR-V
(`src/render/spv_reflect.c`, tests/test_spv_reflect.c): set 0 the two samplers, set 1 the resources
(push descriptors). With `r_upscale_ratio` 1 it runs as native-resolution AA.

| pass | what |
|---|---|
| `fsr2_reset` | a reset (first frame, resize, camera cut): the persistent images cleared |
| `fsr2_prep` | ours: the depth with the viewmodel moved into the world projection (FSR 2 dilates motion vectors to the nearest depth), the reactive mask of the transparent pass (FidelityFX's generation: tonemapped colour vs `hdr_opaque`, 0.2 → 0.9), the exposure (`exposure_buf[0]`) as a 1×1 texture |
| `fsr2_luminance` | SPD: the luminance mips (shading change) and FSR 2's own exposure for its lock logic |
| `fsr2_reconstruct` | dilated motion vectors and depth, the reconstructed previous depth, luma history |
| `fsr2_depth_clip` | disocclusion, dilated reactive masks, the prepared colour |
| `fsr2_lock` | new locks on thin features; clears the reconstructed depth for the next frame |
| `fsr2_accumulate` | the upscaled history and the output (display resolution) |

Two storage formats in `ffx_fsr2_callbacks_glsl.h` are changed from the SDK's GLSL to what its own C++
allocates (each line marked "Deadground"): the dilated depth is `r32f` (fp16 cannot tell far depths apart
in a standard-depth buffer) and the prepared colour `rgba16f` (it holds signed YCoCg). When a pass is
missing (shaders not built, render size below 64 px) the frame falls back to TAA-U. Measured on the Linux
container (dev room, High, 0.67 of 1600×900): a still image scores like TAA-U against native TAA (PSNR
34.9 vs 35.0 dB, gradient energy 4.35 vs 4.34); in a camera pan the zombies stay visibly crisper than with
TAA-U. Its GPU cost on the target GPU is not measured yet (on an RTX 3060 Ti AMD quotes ~1 ms at 1440p).

## Dynamic resolution

`r_dynres 1` (Settings → Graphics → Display, under the upscaler; the `dynres` Engine Fidelity feature,
available from Medium) lowers the share of the display a temporal upscaler (TAA-U, FSR 2) renders
while the GPU frame runs over the target, and raises it back to the player's `r_upscale_ratio` when
there is room. The target is `r_dynres_fps`, or the frame rate limit (`r_maxfps`), or the display's
refresh rate. The share never goes below `r_dynres_min` (0.5).

The controller (`src/render/dynres.c`, pure; tests/test_dynres.c) smooths the last completed GPU frame
time (`render_gpu_frame_ms`, ~0.25 s), lowers the share when it stays above 95 % of the budget for
0.4 s and raises it when it stays below 75 % for 2 s. It aims the load at 85 % of the budget (the pixel
count goes with the square of the share), moves in 0.05 steps (at most two up at a time) and holds
for 1.5 s after each change.

A change re-creates the scene targets (HDR, depth, viewmodel depth) after a device wait: a short stall,
which is why the controller changes rarely. Nothing else starts over: the pipelines stay, the frame
graph keeps its display-size histories and drops the old render-size images two frames later
(`fg_trim`), TAA-U keeps its history (the disocclusion test skips the one frame without a depth
history), FSR 2 keeps its accumulation and clears only its two render-size images, and the material
mip bias follows the player's share, not the dynamic one (no sampler rebuild). The proper alternative,
render targets at the maximum size with the frame drawn into a sub-rectangle, needs every post shader
to scale its UVs and screen positions by the used share; not done.

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
SMAA, 3 TAA), `r_upscaler` (0 off, 1 TAA-U, 2 FSR 1, 3 FSR 2, 4 DLSS slot: FSR 2),
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
  14 bloom, 15 flare, 16 motion blur tiles, 17 fog opacity, 18 raw GTAO, 19 blurred GTAO,
  20 final AO (the lit pass' input × intensity × the material weight; thin occluders tinted green).
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

## Foliage-aware AO

Screen-space AO that leaves vegetation alone: alpha-tested cards neither go black themselves nor throw
dark halos onto what stands behind them, and each material says how much screen-space AO it takes.

**Pass order** (`post_screen_space.c`, every stage its own frame graph pass):

```
gtao (raw, half or full res, R16F "ao_raw")
  -> ao_blur_x -> ao_blur_y   separable bilateral blur (depth + normal weights, R16F)
  -> gtao_temporal            ping-pong history "ao_hist_a/b": velocity reprojection, 3x3 clamp,
                              depth rejection against last frame's depth ("depth_prev")
  -> gtao_upsample            depth-aware 2x bilateral to the R8 "ao" (half res only; at full res
                              the lit pass samples the last stage directly)
lit opaque: ao = combine(lerp(1, ss_ao, ao_strength), baked AO), indirect light only
```

The targets are transients / histories of the frame graph, so its barriers / layout transitions cover
them (storage writes in GENERAL, sampled reads in SHADER_READ_ONLY_OPTIMAL; history invalidated on a
resize, camera cut or a resolution toggle). `r_post_disable ao_blur_x,gtao_temporal,...` skips a
stage; the next one reads its input. No new attachment: the G-buffer's flags byte had 5 spare bits.

**Thin occluders** (`gtao.comp`): materials flagged thin (alpha-tested: glTF MASK, `VGM_MASKED`) write
`GB_THIN`. In the horizon search a thin sample more than `r_ao_thickness` in front of the receiver
(along the view) fades out over another `r_ao_thickness`: the depth buffer only has a card's front,
and behind it is air. Thin samples only count within `r_ao_foliage_radius` of the receiver, and a thin
receiver searches that clamped radius itself (no halos). A thin receiver integrates against its
geometric normal (from the depth buffer), because foliage shading normals are authored (bush spheres,
grass pointing up) and would let the card's own plane occlude it. Every sample is reconstructed at
the depth texel it reads: a half-res UV between texels put grazing planes off themselves, which showed
as grey noise and 1-pixel white columns on walls seen at an angle.

**Per-material** (`src/render/material_ao.h`): `Material.ao_strength` (0 = automatic: 1, or 0.3 for
alpha-tested; `MATERIAL_AO_OFF` = none), `ao_combine` (auto / min / multiply), `ao_vertex` (vertex
colour alpha = baked AO, not opacity). They travel as one byte, the alpha of the pbr emissive word
(`PushLit.pbr.z`, `VgMaterial.emissive_rgb`), on the legacy, classic and vgeo paths alike. The lit pass
applies `lerp(1, ao, ao_strength)` from the material itself (exact); the G-buffer keeps a 4-bit copy
for the debug view and the thin bit for GTAO. Data: glTF material `extras`
(`ao_strength`, `ao_combine`, `ao_vertex`), a model's `"ao": { "strength", "combine", "vertex" }`
in `data/assets.json`, grass assets' `shading.ao_strength` / `ao_combine`
(`assets/foliage/grass/*.json`). Trees and procedural foliage set the defaults in code. In data, 0 means
none.

**Baked AO** (vertex alpha with `ao_vertex`, the glTF occlusion map, surface-layer AO, the grass
clump AO) multiplies indirect light only (ambient, SSGI, specular occlusion), never the sun / lamps.
Against the screen-space term it is `min()` for foliage (default: the card's baked term already holds
the self-occlusion GTAO sees) and multiplied otherwise. The vgeo glTF importer now reads `COLOR_0`.
Limitation: for WPO (wind) foliage the vertex colour is the wind weights, so vertex AO is 1 there
(their textures' AO is used instead).

**Uniforms / cvars** (Settings > Graphics > Debug, under Ambient occlusion; `PostUBO s_ao / s_ao2 / s_ao3`):

| cvar | default | |
|---|---|---|
| `r_ao` | 2 | 0 off, 1 low (2 slices × 3 steps), 2 high (3 × 6) |
| `r_ao_full_res` | 0 | full render resolution instead of half |
| `r_ao_radius` | 0.9 m | world-space radius |
| `r_ao_intensity` | 1 | < 1 fades the AO (lit lerp), > 1 deepens it (power) |
| `r_ao_falloff` | 0.4 | the share of the radius occluders fade out over |
| `r_ao_thickness` | 0.25 m | thin-occluder heuristic (above) |
| `r_ao_foliage_radius` | 0.35 m | radius clamp for thin occluders / receivers |
| `r_ao_slices`, `r_ao_steps` | 0 | 0 = by quality |
| `r_ao_blur`, `r_ao_blur_sharpness` | 3 px, 40 | blur taps per side (0 = off), depth sharpness |
| `r_ao_temporal` | 0.9 | history weight (0 = off) |

**Recommended for foliage**: `ao_strength` 0.25–0.35 on leaf / grass cards with `min` against their
baked AO (vertex alpha or the clump / nta map); thickness 0.2–0.3 m (a leaf clump's depth), foliage
radius 0.3–0.5 m (below the global radius); keep bark / trunks at 1 (opaque, not thin). Dense crops
(wheat) can go to 0.2.

**Test**: dev room **AO foliage corner** (-50, 0, -32; `tools/gen_foliage_ao_test.py` makes the
models): legacy-path bushes by a wall and 4 m out, a reference bush at 1.0 / multiply, the same bush
and a grass patch through vgeo, a solid step. `tools/tests/ao_foliage.cfg` captures lit / raw / blurred
/ final / AO off and a camera strafe; `test_material_ao` checks the byte encoding and the composite.
Checked on lavapipe with validation (dev room, `map vegetation`, `map wateryard`, classic renderer,
full res): 0 messages; static frames change < 1 % of pixels, and motion changes only parallax edges.

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

* DLSS is a selectable slot without its SDK (it runs FSR 2); FSR 3 frame generation is not integrated.
* Post volumes are evaluated at the camera position (the whole frame takes the blend), not per
  pixel.
* GPU particles don't receive the volumetric fog yet (blended lit surfaces do).
* The frame graph pool favours speed over memory (≈ 175–280 MB at 1080p); transients alias only
  within identical format / size / mip keys.
* Async compute (PF-0027) is measured on one NVIDIA GPU only. AMD / Intel take the same compute
  family path (CONCURRENT sharing) untested; macOS (MoltenVK, one queue family) runs it off unless
  `r_async_compute 2`. The async passes' GPU scopes are elapsed times under overlap.
