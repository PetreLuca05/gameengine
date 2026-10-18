# Tracked performance findings

Generated from `docs/perf/fixes.json` by `tools/perf_audit/perf_audit.py findings`. A finding is **closed** only when `perf_verify` showed the timing improvement AND every reference image pair passed AND the gameplay state hashes matched (docs/PERF_AUDIT.md "Closing a finding"). Fixes are switched off for A/B runs with `perf_fix_disable <id>`. A **visual trade-off** (marked in the title) changes the image by design: its image pairs are recorded but do not decide, its visual difference is documented with screenshots, and `perf_fix_disable all` leaves it on.

| Fix | Finding | Title | Status | Effort | Verified on | Result |
|---|---|---|---|---|---|---|
| PF-0001 | R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig) | Empty shadow maps are not re-cleared every frame | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: scopec:shadow_moon:clear_px 4194304.0 -> 0.0 (-100.00%, p=0); images pass; state identical |
| PF-0002 | R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each) | cvar_find through a case-insensitive hash index | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone_all:cvar_find 0.08996 -> 0.01043 (-88.41%, p=0); images pass; state identical |
| PF-0003 | R08 malloc / free in vg_palettes_record every frame | vgeo palette upload regions without a per-frame malloc | dropped | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | NOT CLOSED: zone_all:vgeo:palettes_record 0.88855 -> 0.91525 (+3.00%, p=0.628); images pass; state identical |
| PF-0004 | R08 malloc / free in vg_stream_update every frame | vgeo streaming copy list without a per-frame malloc | dropped | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone_all:vgeo:stream_update 0.97765 -> 0.0366 (-96.26%, p=0); images pass; state identical |
| PF-0005 | R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it | Frame graph validation report built on demand | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone:fg:validate 0.01746 -> 0.0 (-100.00%, p=0); images pass; state identical |
| PF-0006 | R11 shadow caster items far above the scene items / R02 static casters in every cascade (F-7b209b, F-b96020): every caster inside a cascade's light box was drawn, also those whose shadow falls only on ground outside the view | Sun cascade casters culled to the cascade's receiver volume | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 1.687 -> 1.5465 (-8.33%, p=0); images pass; state identical |
| PF-0007 | R10 duplicate skinning palettes (F-5c8ab1): every part of a modular character copied the same matrices into the frame's bone buffer (1927 copies, 3.4 MB per frame with 500 zombies), then again into the GPU buffer, the previous-palette buffer and its GPU copy | Parts of one character share one palette copy | closed | S | DESKTOP-SN4N74U | CLOSED: zone:render:prepare 1.2605 -> 0.75955 (-39.74%, p=0); images pass; state identical |
| PF-0008 | R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.2 ms, 500 zombies), most of it chars_render interpolating every bone of every drawn character and building its skinning matrices one character after the other | Character draw poses on the job system | dropped | M | DESKTOP-SN4N74U | NOT CLOSED: zone:game_render 5.803 -> 3.5205 (-39.33%, p=0); images pass; state identical |
| PF-0009 | R07 main thread stalls waiting for jobs (F-37a7e1): in the vegetation scenes the classic cull ran one jobs_parallel_for per view (main view, up to 4 sun cascades, the scope PiP), each ending in a tail wait for its slowest cell before the next view could start (0.5 - 0.7 ms of wait:parfor_tail per frame, render:prepare 3.8 ms) | The classic renderer's views culled in one parallel pass | closed | M | DESKTOP-SN4N74U | CLOSED: zone:render:prepare 3.258 -> 2.646 (-18.78%, p=0); images pass; state identical |
| PF-0010 | R07 workers idle while the main thread runs serial zones (F-66ec5c): weather_frame -> wind_game_frame -> wind_tile_update evaluated the 64 x 64 wind window (wind_at: gust fronts, three turbulence noises, sources; a damped spring per texel) on the main thread every frame, ~1.6 ms of game_render in every scene (found by zoning game_render on the vegetation map) | Wind tile rows on the job system | closed | S | DESKTOP-SN4N74U | CLOSED: zone:game_render 1.594 -> 0.96035 (-39.75%, p=0); images pass; state identical |
| PF-0011 | R11 shadow casters drawn into cascades (F-7b209b): the skinned shadow pass (every zombie's parts, instanced) was drawn whole into each of the 4 sun cascades and the moon map, so a 500-zombie horde was skinned 5 times for the shadows; the raster and classic casters were already culled per tile | Skinned shadow casters culled per sun cascade | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 0.3625 -> 0.3092 (-14.70%, p=0); images pass; state identical |
| PF-0012 | R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: the pine forest interior's sun shadow pass ran 670 M alpha-tested fragment invocations per frame (~40 x the 4 tiles' 16.7 M texels; 14 - 21 ms of GPU at Modern), instances in cell order, small meshes before trees | Classic shadow instances drawn front to back from the light | closed | S | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 29.5 -> 27.49 (-6.81%, p=1e-05); images pass; state identical |
| PF-0013 | R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: after PF-0012 the pine forest's sun shadow pass still ran ~660 M alpha-tested fragment invocations per frame (~40 x overdraw of leaf / grass cards in the 4 x 2048^2 tiles, 14 - 30 ms at Modern); 2 cascades instead of 4 cut 58 % but blurred every tree shadow | Grass and small plant shadows in the near cascades only (per-class caster cascade limit) **(visual trade-off)** | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 3.009 -> 2.546 (-15.39%, p=0); images differ (visual trade-off, by design); state identical |
| PF-0020 | R23 memcpy volume per frame (horde500 12.9 MB / frame): every skinning palette was gathered into a scratch array, copied into the frame's CPU bone array (render_draw_skinned, after a memcmp against the recent copies for PF-0007), copied again into the slot's mapped bone buffer, last frame's CPU copy appended behind it for the previous-palette velocity, and the CPU array copied once more into last frame's copy at render_end_frame: four copies of ~1.75 MB with 500 zombies (~1.1 ms of the main thread) | Skinning palettes written straight into the mapped GPU buffer | closed | M | DESKTOP-SN4N74U | CLOSED: counter:memcpy_bytes 5990486.5 -> 3278679.0 (-45.27%, p=0); images pass; state identical |
| PF-0021 | R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.5 ms with 500 zombies), most of it chars_render interpolating every bone of every drawn character, building its skinning matrices and gathering each part's palette one character after the other. Retry of PF-0008 (dropped: CPU -21 % but GPU frame +11 %). | Character draw poses and palettes on the job system | closed | M | DESKTOP-SN4N74U | CLOSED: zone:chars:render 1.206 -> 0.9718 (-19.42%, p=0); images pass; state identical |
| PF-0033 | R12 off-screen animation: the presentation post-pass (chars_present_tick) chose a character's update rate and its foot IK / lean / turn from its projected size alone, so the zombies behind the camera or beside the view were posed like the ones in front of it (every tick with IK at LOD0), and with a horde the anim_eval_budget's 64 post-passes per tick went to them as much as to what is on screen (horde500: 40 - 85 % of the characters outside the view, depending on the camera) | Characters outside the view pose at the cheap rate without IK (frustum-aware animation LOD) **(visual trade-off)** | closed | S | DESKTOP-SN4N74U | CLOSED: counter:anim_eval_outside 42.0 -> 12.0 (-71.43%, p=0); images differ (visual trade-off, by design); state identical |
| PF-0034 | R09 per-character animation cost with a horde (the step toward 1000+ zombies): every drawn character interpolated its own 65-bone pose, built its own skeleton palette and gathered one palette per part every frame, also the far ones (LOD2 / LOD3, < 60 px) that walk the same few clips: horde500 ~6.5 ms of worker time per frame (pose 4.6 + gathers 1.9), 1.75 MB of palettes; 1000 zombies ~12.5 ms and 3.35 MB | Far characters share one sampled pose per clip / phase bucket (shared pose cache) **(visual trade-off)** | closed | L | DESKTOP-SN4N74U | CLOSED: counter:skin_palette_bytes 1749760.0 -> 905536.0 (-48.25%, p=0); images differ (visual trade-off, by design); state identical |

## PF-0001: Empty shadow maps are not re-cleared every frame

The sun cascade atlas, the moon map and the viewmodel shadow tile were transitioned, cleared to depth 1 and transitioned back every frame even when nothing is drawn into them (the whole atlas at night, the moon map all day). A map that was cleared and left empty last frame and draws nothing this frame keeps its content: the clear, both barriers and the empty rendering are skipped.

- Rule / finding: R01 R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig)
- Code: `src/render/render.c shadow_clear_skip`, `src/render/vk_device.c (shadow_gen)`
- Metrics (perf_verify): `scopec:shadow_moon:clear_px`, `scope:shadow_moon`, `scopec:shadow_sun:clear_px`, `scope:shadow_sun`, `scope:shadow_vm`, `gpu_frame`
- Scenes: night_lights, devroom
- Why fidelity-neutral: The skipped map holds exactly the clear the next sample expects (depth 1, read as lit), in the same SHADER_READ_ONLY layout; the earlier barrier made that clear visible to every later read in submission order. A re-created map (resize, resolution change) bumps shadow_gen and is cleared again. shadow_drawn bits (validation) are unchanged.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: scopec:shadow_moon:clear_px 4194304.0 -> 0.0 (-100.00%, p=0); images pass; state identical

## PF-0002: cvar_find through a case-insensitive hash index

cvar_find scanned every registered cvar with SDL_strcasecmp; 114 call sites, several per frame. The cvars now sit in a 2048-slot open-addressing index keyed by a case-insensitive FNV-1a hash of the name, filled by cvar_reg (cvars are never removed). MAX_CVARS rose from 512 to 768 (the lint counts 782 commands + cvars).

- Rule / finding: R26 R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each)
- Code: `src/core/cvar.c cvar_find / cvar_reg (index[])`
- Metrics (perf_verify): `zone_all:cvar_find`, `counter:cvar_find`, `cpu_work`
- Scenes: devroom, horde500
- Why fidelity-neutral: Returns the same pointer for every name (the index probes compare with the same SDL_strcasecmp; a name longer than the stored one misses in both paths). tests/test_profiler.c checks hash == scan, case-insensitive, and the miss.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone_all:cvar_find 0.08996 -> 0.01043 (-88.41%, p=0); images pass; state identical

## PF-0003: vgeo palette upload regions without a per-frame malloc

The VkBufferCopy region list of the skinning palette upload was malloc'd and freed every frame (max_slots + 1 entries). It is now allocated once with the palettes. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_palettes_record every frame
- Code: `src/render/vgeo/vgeo_skin.c vg_palettes_record`, `src/render/vgeo/vgeo_skin.h VgPalettes.regions`
- Metrics (perf_verify): `zone_all:vgeo:palettes_record`, `site:vgeo_skin.c`, `cpu_work`
- Scenes: horde500, devroom
- Why fidelity-neutral: vkCmdCopyBuffer copies the regions into the command buffer at record time: the same regions, the same copies, the same data.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): NOT CLOSED: zone_all:vgeo:palettes_record 0.88855 -> 0.91525 (+3.00%, p=0.628); images pass; state identical

## PF-0004: vgeo streaming copy list without a per-frame malloc

The page copy list of the virtualized geometry streamer (2 x pages_per_frame VkBufferCopy) was malloc'd and freed every frame. It is now allocated with the stream. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_stream_update every frame
- Code: `src/render/vgeo/vgeo_stream.c vg_stream_update`, `src/render/vgeo/vgeo_stream.h VgStream.copy_scratch`
- Metrics (perf_verify): `zone_all:vgeo:stream_update`, `site:vgeo_stream.c`, `cpu_work`
- Scenes: foliage, devroom
- Why fidelity-neutral: Same capacity (ppf_cap never changes after init), same contents, consumed by vkCmdCopyBuffer at record time.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone_all:vgeo:stream_update 0.97765 -> 0.0366 (-96.26%, p=0); images pass; state identical

## PF-0005: Frame graph validation report built on demand

fg_execute built the validation report every frame. It is now built by the first fg_last_report after the frame (render_graph_report: fidelity_test, bench_fidelity, the profile's fg section); fg_trim and fg_targets_changed build it before they drop pool images.

- Rule / finding: R20 R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it
- Code: `src/render/graph/fg.c fg_last_report / fg_execute / fg_trim / fg_targets_changed`
- Metrics (perf_verify): `zone:fg:validate`, `cpu_work`
- Scenes: devroom
- Why fidelity-neutral: Validation only: no rendering decision reads it. Between fg_execute and the next fg_begin the graph, the resources and the pool state it reads are unchanged, so an on-demand report is identical to the eager one.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone:fg:validate 0.01746 -> 0.0 (-100.00%, p=0); images pass; state identical

## PF-0006: Sun cascade casters culled to the cascade's receiver volume

Per cascade k below the last, the light-space convex hull of the view slice up to split k (and a 3 m box around the eye: the first-person rig) is extruded along the light into world planes, grown by the filter's reach (normal offset + (1.5 x PCSS max texels + 6) texels + 0.25 m). Classic instances, placement cells / chunks and raster draws whose bounds lie outside it are skipped in that cascade.

- Rule / finding: R11 R11 shadow caster items far above the scene items / R02 static casters in every cascade (F-7b209b, F-b96020): every caster inside a cascade's light box was drawn, also those whose shadow falls only on ground outside the view
- Code: `src/render/shadow_csm.c csm_receiver_planes`, `src/render/render.c compute_matrices (shadow_rcv), draw_items`, `src/render/vk_classic.c cull_cells / cull_instance (ClCull.rcv)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:vs_invocations`, `gpu_frame`, `zone:render:prepare`
- Scenes: foliage, devroom
- Why fidelity-neutral: lighting.glsl samples cascade k < last only for receivers at a selection distance <= split k (its range, the blend from k - 1, the fallback from k - 1), i.e. visible points of the view slice, plus the rig near the eye (shadow_factor_vm). Every tap of every filter (hard, tent, PCSS search and disc) lies within the margin of the receiver's projection, and a caster outside the extruded hull projects outside it: no texel any receiver reads changes. The last cascade (particles, decals, froxels sample it anywhere in its tile) is never culled, nor any cascade while the scope PiP or a phone camera samples them. tests/test_shadow_csm.c: 36000 receiver / caster pairs, none culled.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 1.687 -> 1.5465 (-8.33%, p=0); images pass; state identical

## PF-0007: Parts of one character share one palette copy

render_draw_skinned compares the palette with the copy it made for the previous draw (same bone count, memcmp); when equal, the draw takes that copy's offset instead of appending a new one. The bone buffer, its GPU upload and the previous-palette copies shrink by the duplicates. skin_poses now counts character poses drawn (draw_char), so R10 compares uploads with poses.

- Rule / finding: R10 R10 duplicate skinning palettes (F-5c8ab1): every part of a modular character copied the same matrices into the frame's bone buffer (1927 copies, 3.4 MB per frame with 500 zombies), then again into the GPU buffer, the previous-palette buffer and its GPU copy
- Code: `src/render/render.c render_draw_skinned`, `src/gameplay/characters.c draw_char / draw_char_fp (PC_SKIN_POSES)`
- Metrics (perf_verify): `zone:render:prepare`, `counter:skin_palette_bytes`, `counter:memcpy_bytes`, `cpu_work`
- Scenes: horde500
- Why fidelity-neutral: Only bit-identical palettes are shared: the skinning shaders read the same matrices at the shared offset. The previous-palette lookup (velocity) stores the offset / count each draw got, so last frame's matrices of every part are the same values too.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:render:prepare 1.2605 -> 0.75955 (-39.74%, p=0); images pass; state identical

## PF-0008: Character draw poses on the job system

chars_render first reads the interpolation instant and the seat fix (vehicles) of every drawn character on the main thread, then interpolates the poses and builds the skinning matrices (skel_inst_set_pose) with jobs_parallel_for over the characters (ragdolls stay on the main thread: they read the physics world). The part selection, LOD choice and every draw call stay serial, in the same entity order. Dropped: perf_verify (horde500, 3 x A/B) measured game_render -39 % and CPU work -21 %, but the GPU frame +11 % in every B pass (shadow_sun +0.16 ms; the GPU reads the host-visible bone / instance buffers while 11 workers run), so the guard failed. The code was reverted; the id is not reused.

- Rule / finding: R07 R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.2 ms, 500 zombies), most of it chars_render interpolating every bone of every drawn character and building its skinning matrices one character after the other
- Code: `src/gameplay/characters.c chars_render / pose_for_draw / pose_range`
- Metrics (perf_verify): `zone:game_render`, `cpu_work`
- Scenes: horde500
- Why fidelity-neutral: Each job writes only its own character's pose_draw and skeleton instance from inputs no job writes (pose_prev / pose_cur, the tick, the seat fix taken before), with the same arithmetic as the serial loop: the poses, palettes and the draw order are identical whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): NOT CLOSED: zone:game_render 5.803 -> 3.5205 (-39.33%, p=0); images pass; state identical

## PF-0009: The classic renderer's views culled in one parallel pass

vkc_frame builds every view's cull state first (the same ClCull per view as before), the cell tests of each view into its own task list, then culls the instances of every view in one jobs_parallel_for over the concatenated task lists, each view writing into its own output buffer. The outputs are packed per view in task order and emitted in the old order (PiP, main, cascades).

- Rule / finding: R07 R07 main thread stalls waiting for jobs (F-37a7e1): in the vegetation scenes the classic cull ran one jobs_parallel_for per view (main view, up to 4 sun cascades, the scope PiP), each ending in a tail wait for its slowest cell before the next view could start (0.5 - 0.7 ms of wait:parfor_tail per frame, render:prepare 3.8 ms)
- Code: `src/render/vk_classic.c cull_views / cull_range_multi / cell_tasks / cascade_view (vkc_frame)`
- Metrics (perf_verify): `zone:render:prepare`, `cpu_work`
- Scenes: foliage
- Why fidelity-neutral: cull_instance is unchanged and reads only shared, frame-constant data; every view's tasks are the same cells in the same order with the same output offsets, packed the same way, so each view's instance stream, batch list and the frame's instance buffer layout are identical to the per-view passes whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:render:prepare 3.258 -> 2.646 (-18.78%, p=0); images pass; state identical

## PF-0010: Wind tile rows on the job system

wind_tile_update is split into begin (window origin, the global bend spring), rows and end (the new origin). wind_game_frame runs the 64 rows with jobs_parallel_for; the pure module keeps the one-call form (tests/test_wind.c).

- Rule / finding: R07 R07 workers idle while the main thread runs serial zones (F-66ec5c): weather_frame -> wind_game_frame -> wind_tile_update evaluated the 64 x 64 wind window (wind_at: gust fronts, three turbulence noises, sources; a damped spring per texel) on the main thread every frame, ~1.6 ms of game_render in every scene (found by zoning game_render on the vegetation map)
- Code: `src/world/wind.c wind_tile_begin / wind_tile_rows / wind_tile_end (wind_tile_update = the three in a row)`, `src/game/game_wind.c wind_game_frame`
- Metrics (perf_verify): `zone:game_render`, `cpu_work`
- Scenes: foliage, devroom
- Why fidelity-neutral: Row j writes only the texels of world row oz + j (64 consecutive rows map to 64 distinct slots) and reads the field and the previous window origin, which only end changes: every texel gets the same wind_at / spring arithmetic, so the uploaded RGBA16F window is identical whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:game_render 1.594 -> 0.96035 (-39.75%, p=0); images pass; state identical

## PF-0011: Skinned shadow casters culled per sun cascade

After the shadow pass's instance records, each active cascade (and the moon map) gets its own copy of the pass with only the casters whose bounding sphere (hips, character height) touches the cascade's light box and its receiver volume (PF-0006): the same batches in the same order, with the instances kept. The culling-mask path (per-draw shadow draws) and the local light tiles keep the whole pass. The skinning instance buffer grows to 8 x MAX_SKIN_DRAWS records.

- Rule / finding: R11 R11 shadow casters drawn into cascades (F-7b209b): the skinned shadow pass (every zombie's parts, instanced) was drawn whole into each of the 4 sun cascades and the moon map, so a 500-zombie horde was skinned 5 times for the shadows; the raster and classic casters were already culled per tile
- Code: `src/render/render.c cull_skin_shadow / render_end_frame (skin_shadow_k)`, `render.c build_skin_pass (g_skin_shadow_order)`
- Metrics (perf_verify): `scope:shadow_sun`, `gpu_frame`, `scopec:shadow_sun:vs_invocations`
- Scenes: horde500
- Why fidelity-neutral: A shadow record depends on its draw only, so a kept instance draws exactly as before; a dropped one lies outside the tile's light box (it rasterizes no texel of the tile: the tile scissor and the box clip it) or outside the receiver volume (no receiver reads the texels it would write). Depth is a min over the kept fragments: identical.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 0.3625 -> 0.3092 (-14.70%, p=0); images pass; state identical

## PF-0012: Classic shadow instances drawn front to back from the light

A sun cascade's instance stream is emitted with each batch ordered front to back from the light (32 light-depth buckets over the view's depth range, stable within a bucket) and the batches ordered by mesh size (trees before bushes and grass). Canopies are drawn first, so the alpha-tested fragments behind already written depth fail the early depth test before their shader runs.

- Rule / finding: R02 R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: the pine forest interior's sun shadow pass ran 670 M alpha-tested fragment invocations per frame (~40 x the 4 tiles' 16.7 M texels; 14 - 21 ms of GPU at Modern), instances in cell order, small meshes before trees
- Code: `src/render/vk_classic.c emit_view_light_order / cmp_batch_size (cull_views, vkc_frame)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:fs_invocations`, `gpu_frame`
- Scenes: veg_forest_pine, veg_forest_broadleaf
- Why fidelity-neutral: The shadow pass is depth only: every texel ends at the minimum depth of the fragments that pass their alpha test, and which fragments exist, their depths and their alpha tests do not depend on the draw order. The same instances and batches are drawn, so the maps are bit-identical; only the order (and the number of fragment shader invocations) changes.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 29.5 -> 27.49 (-6.81%, p=1e-05); images pass; state identical

## PF-0013: Grass and small plant shadows in the near cascades only (per-class caster cascade limit)

Every vgeo material carries a sun shadow caster class (ground / small / tree / none) from its asset: grass assets' and model foliage blocks' "shadow_class", the procedural kinds (tuft ground, bush and crop small), the Tree It roles (bush small, flower ground, the rest tree); a mesh takes the least limited class of its materials. Per Engine Fidelity tier (r_shadow_caster_limit, TIER: 1 at High / Modern, 2 at Off .. Medium) assets/render/shadow_casters.json gives each class its last cascade and a distance cap. A sun cascade view skips the classes that do not cast into it (whole placement cells / chunks by their class mask, then per instance) and dithers the limited classes out over the last 30 % of their end distance (the shadow-distance fade path). In PCSS wide mode the limited classes also cast into the next cascade, so their reach stays.

- Rule / finding: R02 R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: after PF-0012 the pine forest's sun shadow pass still ran ~660 M alpha-tested fragment invocations per frame (~40 x overdraw of leaf / grass cards in the 4 x 2048^2 tiles, 14 - 30 ms at Modern); 2 cascades instead of 4 cut 58 % but blurred every tree shadow
- Code: `src/render/shadow_casters.c scc_view / scc_alpha (assets/render/shadow_casters.json)`, `src/render/vk_classic.c caster_limits / cascade_view / cull_instance / cell_tasks (cell_scc)`, `src/render/vgeo/vgeo_scene.c mesh_foliage (VGMF_SHADOW_CLASS)`, `src/render/vgeo/grass_asset.c, vgeo_procgen_foliage.c, vgeo_tree.c, src/assets/assets.c (shadow_class)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:fs_invocations`, `gpu_frame`, `cpu_work`, `zone:render:prepare`
- Scenes: veg_forest_pine, veg_forest_broadleaf, veg_forest_edge, veg_meadow, veg_wheat, veg_horde_forest
- **Visual trade-off** (not fidelity neutral): Grass / ground cover casts sun shadows only into cascade 0 (to ~10 m at High / Modern, capped at 15 m on Off .. Medium) and small plants (bushes, ferns, reeds, wheat, small Tree It bushes) into cascades 0 - 1 (~30 m, capped at 30 m), each dithering out over the last 30 % of that distance; before, grass shadows reached 30 m and bushes 60 m. Tree, character, building and terrain shadows and every plant itself are unchanged. Visible only as fewer grass / bush shadow speckles on open ground beyond 10 / 30 m (docs/SHADOWS.md "Caster cascade limits"). Measured (one process, A / B alternating, 2 reps, Modern + Low, RTX 3060 Ti): the meadow's shadow pass -38 % at Modern (fragments -52 %, GPU frame -5 %) and -84 % at Low, horde_foliage -28 % / -47 %, wheat -8 % / -16 %; in the forest interiors fragments only -1 .. -3 % (their ~660 M fragments are tree leaf cards, which keep every cascade), shadow pass -1 .. -9 % at Modern, -2 .. -20 % at Low; CPU work -4 .. -26 % (fewer cascade instances to cull). docs/SHADOWS.md has the table.
- Screenshots: `docs/shadow_screenshots/caster_limit_meadow_1730.jpg`, `docs/shadow_screenshots/caster_limit_meadow_1300.jpg`, `docs/shadow_screenshots/caster_limit_wheat_1730.jpg`, `docs/shadow_screenshots/caster_limit_pine_1300_close.jpg`
- What stays identical: The game state (no rendering decision feeds the simulation: the state hashes must match), every non-foliage shadow, tree shadows in every cascade, every main-view / PiP / local-light draw. The image differs by design where grass / small plant shadows fall beyond their end distance.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 3.009 -> 2.546 (-15.39%, p=0); images differ (visual trade-off, by design); state identical

## PF-0020: Skinning palettes written straight into the mapped GPU buffer

One host-visible ring of 3 x MAX_SKIN_BONES matrices, shared by the frame slots, replaces the per-slot bone buffers: frame N writes region N mod 3, and its previous-palette lookups read region N - 1 where last frame left them (the region a frame writes was last read by frames N - 3 and N - 2, both complete once its slot's fence signalled). draw_part asks render_skin_palette for room in this frame's region, keyed by (skeleton instance, pose version, bone table class: skm_palette_equal), and skm_gather_palette writes the palette there directly; every later part / LOD / shadow draw of the same pose and class gets the same handle (the PF-0007 sharing, decided by the key instead of a memcmp, which would read write-combined memory). No CPU bone array, no upload copy, no previous-palette copy remain. perf_fix_disable PF-0020 (and r_cpu_skinning) binds the old per-slot buffer (created on first use) and takes the old path. Measured (horde500, Modern, A / B alternating in one process, RTX 3060 Ti, shared machine): alone, memcpy 12.9 -> 5.9 MB per frame (-54 %), render:prepare 0.88 -> 0.34 ms (-61 %), CPU work -4.6 % (2 x A/B) and -9.8 % (3 x A/B, with PF-0021), GPU frame -3.8 %; with PF-0021 and PF-0034 on (half the palettes): memcpy -45 %, render:prepare -33 %, CPU work unchanged (+0.3 %, n.s.: the gathers now write the uncached ring instead of a cached array, the upload copy is gone), GPU frame -3 %. The ring lands in device-local BAR memory when the BAR heap has room, else in host memory (seen both on this machine).

- Rule / finding: R23 R23 memcpy volume per frame (horde500 12.9 MB / frame): every skinning palette was gathered into a scratch array, copied into the frame's CPU bone array (render_draw_skinned, after a memcmp against the recent copies for PF-0007), copied again into the slot's mapped bone buffer, last frame's CPU copy appended behind it for the previous-palette velocity, and the CPU array copied once more into last frame's copy at render_end_frame: four copies of ~1.75 MB with 500 zombies (~1.1 ms of the main thread)
- Code: `src/render/render.c render_skin_palette / render_draw_skinned / bind_bones / build_skin_pass (previous palette) / render_end_frame`, `src/render/vk_internal.h Renderer.bone_ring / bone_region / skin_direct`, `src/gameplay/characters.c draw_part (GpuPart.pal_class)`, `src/render/skeletal_mesh.c skm_palette_equal`
- Metrics (perf_verify): `counter:memcpy_bytes`, `zone:render:prepare`, `zone:game_render`, `cpu_work`, `gpu_frame`
- Scenes: horde500
- Why fidelity-neutral: The shaders read the same matrices: each palette is the same skm_gather_palette output, written once where the instance's info.x points (absolute ring offsets fit the 20-bit field: 3 x 196608 < 2^20); a key repeats only for the same instance, pose version and an equal bone table, which gather bit-identical palettes. The previous palette of an object is the same matrices last frame wrote, read in place (info.z = its ring offset + 1) instead of from a copy behind this frame's bones. Draws, batches, instance records and their order are unchanged. The ring is written only after this slot's fence: a region is never written while a submitted frame reads it.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: counter:memcpy_bytes 5990486.5 -> 3278679.0 (-45.27%, p=0); images pass; state identical

## PF-0021: Character draw poses and palettes on the job system

chars_render runs in four steps: (1) main thread: every drawn character's pose source, interpolation instant and seat fix (vehicles), ragdoll poses (they read the physics world); (2) jobs_parallel_for over the characters: the interpolated pose into pose_draw and skel_inst_set_pose; (3) main thread, in entity order: the part selection, LOD, fades, every draw call and the held item exactly as before, each part reserving its palette in this frame's bone buffer (render_skin_palette, PF-0020) and queuing its gather; (4) jobs_parallel_for over the queued gathers: skm_gather_palette straight into the reserved slots. Why PF-0008's GPU frame rose: not the GPU work. The palette ring (and the old bone buffers, same allocation flags) is device-local BAR memory on the RTX 3060 Ti in most runs (the log's 'skinning palette ring' line; host memory when other processes had filled the BAR heap), so the vertex shaders already read VRAM and a staging copy into a 'GPU-side' buffer would add a transfer without removing a read; the capped A / B below ran with the ring in BAR memory. The GPU frame metric is the span between a TOP_OF_PIPE timestamp at the start of the frame's command buffer and a BOTTOM_OF_PIPE one at its end: with a faster CPU the frames are submitted at a higher rate, and on a shared GPU the span then also holds the queueing behind the previous frame and other processes' GPU work (the 30 - 40 % outside load of that run). Measured here (horde500, A / B alternating in one process): uncapped, 4 x A/B: game_render -28 %, CPU work -10 %, frame period 9.8 -> 8.8 ms, GPU frame +3.9 % pooled (p < 0.001 for B > A, but not in every pass: the guard holds; shadow_sun +2 %), and in the same session an uncapped 3 x A/B under heavy outside GPU load: GPU frame -8.6 %; with the frame cap kept at 60 fps on both sides (perf_verify cap=60: the same submission rate), 4 x A/B: game_render -41 %, CPU work -25 %, GPU frame +0.2 % (p = 0.09 for B > A), shadow_sun +0.004 ms. The GPU input is bit-identical (same palettes at the same offsets, same instance records and draws); only the uncapped span moves, with the submission rate and the outside load. With PF-0034 on, most far zombies no longer pose themselves (477 of ~540 in horde500, 436 in veg_horde_forest), so what is left to spread is smaller: 3 x A/B over both scenes, chars:render (zone added for this: the whole chars_render) -19 % (horde500 -21 %, veg_horde_forest -18 %), game_render -5 %, CPU work -2.6 %, GPU frame +0.7 % (n.s.).

- Rule / finding: R07 R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.5 ms with 500 zombies), most of it chars_render interpolating every bone of every drawn character, building its skinning matrices and gathering each part's palette one character after the other. Retry of PF-0008 (dropped: CPU -21 % but GPU frame +11 %).
- Code: `src/gameplay/characters.c chars_render (CharDrawPrep, pose_range, fill_range)`, `src/render/render.c render_skin_palette (PF-0020: palettes reserved on the main thread, written by the jobs)`
- Metrics (perf_verify): `zone:chars:render`, `zone:game_render`, `cpu_work`, `gpu_frame`
- Scenes: horde500, veg_horde_forest
- Why fidelity-neutral: Each job writes only its own character's pose_draw / skeleton instance (step 2) or its own reserved palette slot (step 4), from inputs no job writes (pose_prev / pose_cur, the instant and seat fix taken in step 1; the instances of step 2), with the arithmetic of the serial loop: poses, palettes, palette offsets, draw items and their order are identical whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:chars:render 1.206 -> 0.9718 (-19.42%, p=0); images pass; state identical

## PF-0033: Characters outside the view pose at the cheap rate without IK (frustum-aware animation LOD)

chars_present_tick tests each NPC's bounding sphere (hips height, the character's height plus distance x sin(margin)) against the last frame's main view frustum (render_sphere_maybe_visible: always visible while a phone camera renders, its lens looks anywhere). Outside: the eased update interval snaps to at least offscreen_update, ik_w is 0 (post_one drops foot IK, lean and turn), and its budget priority is a quarter. Entering the view: the interval snaps back and the eval accumulator restarts on the view's rate from the last eval, so a pose older than that interval is redone the same tick. r_lod_legacy 1 is unchanged. Counter anim_eval_outside: post-passes of characters outside the view.

- Rule / finding: R12 R12 off-screen animation: the presentation post-pass (chars_present_tick) chose a character's update rate and its foot IK / lean / turn from its projected size alone, so the zombies behind the camera or beside the view were posed like the ones in front of it (every tick with IK at LOD0), and with a horde the anim_eval_budget's 64 post-passes per tick went to them as much as to what is on screen (horde500: 40 - 85 % of the characters outside the view, depending on the camera)
- Code: `src/gameplay/characters.c chars_present_tick (frustum_lod, CharacterInst.outside / offscreen)`, `src/render/render.c render_sphere_maybe_visible (last frame's main view frustum; phone views)`, `src/render/lod_fade.c, assets/render/lod.json characters.offscreen_update / offscreen_margin_deg`
- Metrics (perf_verify): `counter:anim_eval_outside`, `zone:anim:post_pass`, `zone:sim:anim_present`, `cpu_work`
- Scenes: horde500
- **Visual trade-off** (not fidelity neutral): NPCs outside the last frame's main view, grown by 10 degrees (lod.json characters.offscreen_margin_deg) and with no phone camera rendering, pose at most every 8 ticks (offscreen_update, LOD3's rate) and without foot IK / lean / turn (ik_w 0), and rank at a quarter of their size for the post-pass budget (anim_eval_budget). They are not on screen; what can show is their shadow falling into view (sun behind the camera): it animates from poses up to 8 ticks apart (interpolated, so it does not step) with the feet not planted on slopes. A character entering the view gets its LOD's rate back at once and is re-posed the same tick when its pose is older than that interval. Players are never affected (the aim model reads their post-pass), nor any dead / ragdolled body. With the default budget (64 post-passes per tick, binding in every horde scene) the CPU cost is unchanged: the evaluations go to the characters on screen instead (horde500, 2 x A/B: post-passes of characters outside the view 42 -> 12 per tick, -71 %; anim:post_pass -1 %, CPU work +0.9 % n.s.). That moves the post-pass schedule of the characters on screen too: they are re-posed at other (earlier) ticks, the same animation from other interpolation keys, which a pixel diff of a crowd shows (horde500: 5 - 8 % of the pixels over 4 / 255, SSIM 0.94 - 0.96) but is not a quality loss. With the budget not binding (anim_eval_budget 4096) the post-passes fall with the share outside the view: 117 -> 70 - 76 per tick (-40 %) with the camera turned away from the horde, unchanged with most of it in view.
- Screenshots: `docs/perf/tradeoff_shots/pf0033_horde500_before.jpg`, `docs/perf/tradeoff_shots/pf0033_horde500_after.jpg`, `docs/perf/tradeoff_shots/pf0033_horde500_diff.jpg`
- What stays identical: The game state: the host's animation, hitboxes and lag compensation come from chars_host_tick on the base pose, which this never touches; the post-pass pose feeds only the drawing, client-side cosmetic hitboxes and the ragdoll's start pose (cosmetic: removed after anim_ragdoll_time whatever the pose, corpses collide with nothing by default). Players keep their post-pass (the aim model reads it), so the state hashes must match. Every character on screen is posed as before (it is never outside); the image differs only where a shadow of a character outside the view falls into it.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: counter:anim_eval_outside 42.0 -> 12.0 (-71.43%, p=0); images differ (visual trade-off, by design); state identical

## PF-0034: Far characters share one sampled pose per clip / phase bucket (shared pose cache)

A 512-entry cache (128 sets x 4 ways, LRU, never evicting an entry used this frame, at most 24 samples per frame) maps a bucket key (controller, anim set, base state and variant, phase bucket, quantized speed / move direction / weapon type) to the controller's pose at the bucket's phase centre and inputs, as a model-space skeleton palette. chars_render's main-thread step keys each eligible character (the base layer's phase interpolated between its last two post-passes) and finds or fills its entry; the pose jobs then only interpolate its root; its draws take the bucket's palette (reserved once per part class and frame, gathered on the jobs) and carry the root and last frame's root, which skin.vert / skin_shadow.vert apply (info.z SKIN_ROOTED). Its bounding sphere, skinned decals (chars_bone_draw) and shadow use the same placement. Counters: anim_pose_shared, anim_pose_samples.

- Rule / finding: R09 R09 per-character animation cost with a horde (the step toward 1000+ zombies): every drawn character interpolated its own 65-bone pose, built its own skeleton palette and gathered one palette per part every frame, also the far ones (LOD2 / LOD3, < 60 px) that walk the same few clips: horde500 ~6.5 ms of worker time per frame (pose 4.6 + gathers 1.9), 1.75 MB of palettes; 1000 zombies ~12.5 ms and 3.35 MB
- Code: `src/gameplay/characters.c pose_key / pose_get / pose_fill / shared_pose (PoseEntry cache), prep_char / pose_char / draw_part (rooted draws), chars_bone_draw`, `src/render/render.c skin_instance_root, build_skin_pass / cull_skin_shadow / cpu_skin_all (SkinnedDraw.rooted)`, `shaders/common.glsl SkinInstance.root / prev_root (SKIN_ROOTED), shaders/skin.vert, shaders/skin_shadow.vert`, `src/render/lod_fade.c, assets/render/lod.json characters.shared_pose_lod / shared_pose_phases`
- Metrics (perf_verify): `counter:skin_palette_bytes`, `zone_all:parfor:pose_range`, `zone_all:parfor:fill_range`, `zone:game_render`, `cpu_work`
- Scenes: horde500
- **Visual trade-off** (not fidelity neutral): Far characters (LOD >= lod.json characters.shared_pose_lod, 2: below ~60 px at 1080p) in a plain base-layer state (no crossfade or inertial blend, upper body and hit layers empty, nothing in the hands, legs straight, no kneel / swim, not seated, alive, not a player) show the pose of their bucket: the controller evaluated at the centre of a phase bucket (32 per cycle) with the speed quantized to 0.25 m/s and the move direction to 0.125, placed at their own interpolated root. Visible as a walk cycle off by up to half a bucket (1/64 of a cycle, ~15 ms of a 1 s jog), several far zombies in the same bucket stepping in exact sync, and blend weights off by up to half a quantization step; their limb motion is not in the motion vectors (the root motion is), so TAA / motion blur see a far zombie's limbs as rigid with its body for that frame. Anything nearer, anything in a transition, attacking with the upper body, hit, holding an item or dead draws its own pose as before. The pose of a LOD2 / LOD3 character was already the plain base layer (no IK, spine, hold or damage at those sizes), interpolated between evaluations 4 / 8 ticks apart; the bucket pose is sampled at the drawn instant instead. horde500 pixel diff: 3 - 4 % of the pixels over 4 / 255, SSIM 0.985 - 0.988, all on far zombies. Measured (2 x A/B, horde500, 477 of ~540 characters shared): palettes 1.75 -> 0.91 MB per frame (-48 %), pose jobs 5.3 -> 0.8 ms of worker time (-85 %), gathers 2.2 -> 1.0 ms (-55 %), game_render -7.6 %, CPU work -6.1 %; 1000 zombies (~950 shared, captures alternating in one session): pose + gather worker time ~12.5 -> 1.5 - 2.3 ms, palettes 3.35 -> 0.9 - 1.25 MB, 0.2 - 0.4 cache fills per frame once warm (a cold cache fills at most 24 per frame).
- Screenshots: `docs/perf/tradeoff_shots/pf0034_horde500_before.jpg`, `docs/perf/tradeoff_shots/pf0034_horde500_after.jpg`, `docs/perf/tradeoff_shots/pf0034_horde500_diff.jpg`
- What stays identical: The game state: the host's animation, hitboxes and lag compensation come from chars_host_tick on each character's own base pose, which this never reads or writes (the state hashes must match). Every character not eligible draws exactly as before; the non-rooted skinning path in the shaders is unchanged arithmetic.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: counter:skin_palette_bytes 1749760.0 -> 905536.0 (-48.25%, p=0); images differ (visual trade-off, by design); state identical
