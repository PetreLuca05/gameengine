# Tracked performance findings

Generated from `docs/perf/fixes.json` by `tools/perf_audit/perf_audit.py findings`. A finding is **closed** only when `perf_verify` showed the timing improvement AND every reference image pair passed AND the gameplay state hashes matched (docs/PERF_AUDIT.md "Closing a finding"). Fixes are switched off for A/B runs with `perf_fix_disable <id>`.

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
